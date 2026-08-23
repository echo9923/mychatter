// im_tcp_client.h — headless IM TCP client (plan Verification.2).
//
// One TcpClient = one logged-in connection to a ChatServer. A background
// reader thread drains [type][len][body] frames into a mutex-guarded queue; the
// test thread sends 1301/1407/1405 frames with Send() and consumes inbound
// frames (1302/1303/1408/1406/...) with Wait(). This mirrors the production Qt
// TcpMgr receive model and lets senders pump requests while 1302s stream back,
// avoiding TCP receive-buffer back-pressure that a strict request/response
// client would hit under 1000-message bursts.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "im_frame.h"

namespace imt {

class TcpClient {
public:
	TcpClient();
	~TcpClient();

	TcpClient(const TcpClient&) = delete;
	TcpClient& operator=(const TcpClient&) = delete;

	// Connect to host:port. Retries for up to timeout_ms (server may still be
	// coming up). Returns true on success.
	bool Connect(const std::string& host, unsigned short port, int timeout_ms);

	// Send a complete frame. Thread-safe (serialized by a write mutex).
	bool Send(short type, const std::string& body);

	// Block until a frame with type==want_type arrives (want_type<=0 means any frame),
	// or until timeout_ms elapses / the connection closes. On success fills *out
	// and removes the frame from the internal queue; non-matching frames remain.
	bool Wait(short want_type, int timeout_ms, Frame* out);

	// Remove and return up to max_count frames with type==want_type currently
	// buffered, without blocking beyond a short grace. Returns count collected.
	int Drain(short want_type, int max_count, int timeout_ms, std::vector<Frame>* out);

	bool IsClosed() const { return closed_.load(); }

	// Tear down: cancel/close the socket so the reader unblocks, then join.
	void Close();

private:
	void ReaderLoop();
	bool ReadExact(char* dst, std::size_t n);

	boost::asio::io_context ioc_;
	boost::asio::ip::tcp::socket socket_;

	std::thread            reader_;
	std::atomic<bool>      closed_{false};
	std::atomic<bool>      stopping_{false};

	std::mutex             queue_mtx_;
	std::condition_variable queue_cv_;
	std::deque<Frame>      queue_;

	std::mutex             write_mtx_;
};

// ---------------------------------------------------------------------------
// ResClient — a logged-in connection to the ResourceServer.
//
// ResourceServer uses a different wire format than ChatServer:
//   [2-byte type][4-byte big-endian int32 length][body]   (6-byte header)
// vs Chat's [2-byte type][2-byte length]. Otherwise the API mirrors TcpClient:
// a background reader thread drains frames into a mutex-guarded queue.
// ---------------------------------------------------------------------------
class ResClient {
public:
	ResClient();
	~ResClient();

	ResClient(const ResClient&) = delete;
	ResClient& operator=(const ResClient&) = delete;

	bool Connect(const std::string& host, unsigned short port, int timeout_ms);
	bool Send(short type, const std::string& body);
	bool Wait(short want_type, int timeout_ms, Frame* out);
	int  Drain(short want_type, int max_count, int timeout_ms, std::vector<Frame>* out);
	bool IsClosed() const { return closed_.load(); }
	void Close();

private:
	void ReaderLoop();
	bool ReadExact(char* dst, std::size_t n);

	boost::asio::io_context ioc_;
	boost::asio::ip::tcp::socket socket_;

	std::thread            reader_;
	std::atomic<bool>      closed_{false};
	std::atomic<bool>      stopping_{false};

	std::mutex             queue_mtx_;
	std::condition_variable queue_cv_;
	std::deque<Frame>      queue_;

	std::mutex             write_mtx_;
};

// ---------------------------------------------------------------------------
// Auth helpers
//
// The same per-user login token (issued by Status on password login, stored in
// Redis as utoken_<uid>) authenticates both Chat and Resource. These helpers
// send the login frame and parse the response; callers wire the TCP transport.
// ---------------------------------------------------------------------------

// Outcome of a Chat login (1101→1102). `ok` is true only when error==0. The
// parsed 1102 body is kept in `response` so scenarios can assert the server did
// not leak secret fields (pwd/token/session_token).
struct ChatLoginOutcome {
	bool ok      = false;
	int  error   = -1;     // 1102 error field (-1 = transport/parse fail)
	json response;         // parsed 1102 body (object on success)
};

// Chat login: sends 1101 {uid, token}, waits for 1102. out.ok = (error==0).
// The token is the value Gate returned from /user_login (== Redis utoken_<uid>).
ChatLoginOutcome ChatLogin(TcpClient& c, int uid, const std::string& token);

// Resource login (1501→1502): sends 1501 {uid, token}, returns the 1502 error
// field (0 = authed, 2010 = TokenInvalid), or -1 on transport/parse failure.
int ResourceLogin(ResClient& r, int uid, const std::string& token);

} // namespace imt
