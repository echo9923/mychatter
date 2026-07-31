// im_tcp_client.h — headless IM TCP client (plan Verification.2).
//
// One TcpClient = one logged-in connection to a ChatServer. A background
// reader thread drains [id][len][body] frames into a mutex-guarded queue; the
// test thread sends 1017/1049/1051 frames with Send() and consumes inbound
// frames (1018/1019/1050/1052/...) with Wait(). This mirrors the production Qt
// TcpMgr receive model and lets senders pump requests while 1018s stream back,
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
	bool Send(short id, const std::string& body);

	// Block until a frame with id==want_id arrives (want_id<=0 means any frame),
	// or until timeout_ms elapses / the connection closes. On success fills *out
	// and removes the frame from the internal queue; non-matching frames remain.
	bool Wait(short want_id, int timeout_ms, Frame* out);

	// Remove and return up to max_count frames with id==want_id currently
	// buffered, without blocking beyond a short grace. Returns count collected.
	int Drain(short want_id, int max_count, int timeout_ms, std::vector<Frame>* out);

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

} // namespace imt
