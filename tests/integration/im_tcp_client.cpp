// im_tcp_client.cpp — headless IM TCP client implementation.
#include "im_tcp_client.h"

#include <chrono>
#include <cstring>
#include <utility>

#include "im_common.h"

namespace imt {

TcpClient::TcpClient() : socket_(ioc_) {}

TcpClient::~TcpClient() { Close(); }

bool TcpClient::Connect(const std::string& host, unsigned short port, int timeout_ms) {
	namespace asio = boost::asio;
	using boost::asio::ip::tcp;

	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeout_ms);
	boost::system::error_code ec;
	tcp::resolver resolver(ioc_);
	// resolve once; endpoints are tried in Connect-retry below.
	tcp::resolver::results_type endpoints;
	while (true) {
		endpoints = resolver.resolve(host, std::to_string(port), ec);
		if (!ec) break;
		if (std::chrono::steady_clock::now() >= deadline) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	while (true) {
		asio::connect(socket_, endpoints, ec);
		if (!ec) break;
		socket_.close(ec);
		if (std::chrono::steady_clock::now() >= deadline) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	// Disable Nagle so 1000-message bursts do not coalesce in a way that
	// would change per-frame arrival ordering at the reader.
	boost::asio::ip::tcp::no_delay nodelay(true);
	socket_.set_option(nodelay, ec);

	closed_ = false;
	stopping_ = false;
	reader_ = std::thread([this] { ReaderLoop(); });
	return true;
}

bool TcpClient::ReadExact(char* dst, std::size_t n) {
	boost::system::error_code ec;
	std::size_t got = boost::asio::read(socket_, boost::asio::buffer(dst, n), ec);
	return !ec && got == n;
}

void TcpClient::ReaderLoop() {
	char header[HEAD_TOTAL_LEN];
	while (!stopping_.load()) {
		if (!ReadExact(header, HEAD_TOTAL_LEN)) break;
		const short type = ReadBE16(header);
		const short len = ReadBE16(header + HEAD_TYPE_LEN);
		if (len < 0) break;  // malformed; protocol uses unsigned short
		std::string body(static_cast<std::size_t>(len), '\0');
		if (len && !ReadExact(&body[0], static_cast<std::size_t>(len))) break;

		Frame f;
		f.type = id;
		f.body = std::move(body);
		{
			std::lock_guard<std::mutex> lk(queue_mtx_);
			queue_.push_back(std::move(f));
		}
		queue_cv_.notify_one();
	}
	closed_ = true;
	queue_cv_.notify_all();
}

bool TcpClient::Send(short type, const std::string& body) {
	if (closed_.load()) return false;
	std::string frame;
	EncodeFrame(type, body, frame);
	std::lock_guard<std::mutex> lk(write_mtx_);
	boost::system::error_code ec;
	std::size_t sent = boost::asio::write(socket_, boost::asio::buffer(frame), ec);
	return !ec && sent == frame.size();
}

bool TcpClient::Wait(short want_type, int timeout_ms, Frame* out) {
	std::unique_lock<std::mutex> lk(queue_mtx_);
	const auto pred = [&] {
		if (closed_.load()) return true;
		if (want_type <= 0) return !queue_.empty();
		for (const auto& f : queue_) if (f.type == want_type) return true;
		return false;
	};
	if (!pred())
		queue_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), pred);
	if (queue_.empty()) return false;

	// Pick the first matching frame (or the very first frame for want_type<=0).
	std::size_t idx = 0;
	if (want_type > 0) {
		bool found = false;
		for (std::size_t i = 0; i < queue_.size(); ++i) {
			if (queue_[i].type == want_type) { idx = i; found = true; break; }
		}
		if (!found) return false;
	}
	Frame f = std::move(queue_[idx]);
	queue_.erase(queue_.begin() + idx);
	if (out) *out = std::move(f);
	return true;
}

int TcpClient::Drain(short want_type, int max_count, int timeout_ms, std::vector<Frame>* out) {
	// Short grace: let any in-flight frames land.
	if (timeout_ms > 0) {
		std::unique_lock<std::mutex> lk(queue_mtx_);
		queue_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
			[&] { return closed_.load(); });
	}
	int collected = 0;
	std::lock_guard<std::mutex> lk(queue_mtx_);
	for (auto it = queue_.begin(); it != queue_.end() && collected < max_count;) {
		if (it->type == want_type) {
			if (out) out->push_back(*it);
			it = queue_.erase(it);
			++collected;
		} else {
			++it;
		}
	}
	return collected;
}

void TcpClient::Close() {
	bool expected = false;
	if (!stopping_.compare_exchange_strong(expected, true)) return;

	boost::system::error_code ec;
	socket_.cancel(ec);
	socket_.close(ec);
	closed_ = true;
	queue_cv_.notify_all();

	if (reader_.joinable()) {
		// The reader's blocking read unblocks when the socket closes; give it
		// a bounded window to exit rather than hanging the test on shutdown.
		// reader_ is non-detached, join() is correct here.
		reader_.join();
	}
}

// ---------------------------------------------------------------------------
// ResClient — same reader/send/wait model as TcpClient, but with the
// ResourceServer 6-byte header (4-byte length) instead of Chat's 2-byte.
// ---------------------------------------------------------------------------
ResClient::ResClient() : socket_(ioc_) {}
ResClient::~ResClient() { Close(); }

bool ResClient::Connect(const std::string& host, unsigned short port, int timeout_ms) {
	namespace asio = boost::asio;
	using boost::asio::ip::tcp;

	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeout_ms);
	boost::system::error_code ec;
	tcp::resolver resolver(ioc_);
	tcp::resolver::results_type endpoints;
	while (true) {
		endpoints = resolver.resolve(host, std::to_string(port), ec);
		if (!ec) break;
		if (std::chrono::steady_clock::now() >= deadline) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	while (true) {
		asio::connect(socket_, endpoints, ec);
		if (!ec) break;
		socket_.close(ec);
		if (std::chrono::steady_clock::now() >= deadline) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	boost::asio::ip::tcp::no_delay nodelay(true);
	socket_.set_option(nodelay, ec);

	closed_ = false;
	stopping_ = false;
	reader_ = std::thread([this] { ReaderLoop(); });
	return true;
}

bool ResClient::ReadExact(char* dst, std::size_t n) {
	boost::system::error_code ec;
	std::size_t got = boost::asio::read(socket_, boost::asio::buffer(dst, n), ec);
	return !ec && got == n;
}

void ResClient::ReaderLoop() {
	char header[RES_HEAD_TOTAL_LEN];
	while (!stopping_.load()) {
		if (!ReadExact(header, RES_HEAD_TOTAL_LEN)) break;
		const short type = ReadBE16(header);
		const std::uint32_t len = ReadBE32(header + RES_HEAD_TYPE_LEN);
		if (len > 64 * 1024 * 1024) break;  // sanity guard against absurd lengths
		std::string body(static_cast<std::size_t>(len), '\0');
		if (len && !ReadExact(&body[0], static_cast<std::size_t>(len))) break;

		Frame f;
		f.type = id;
		f.body = std::move(body);
		{
			std::lock_guard<std::mutex> lk(queue_mtx_);
			queue_.push_back(std::move(f));
		}
		queue_cv_.notify_one();
	}
	closed_ = true;
	queue_cv_.notify_all();
}

bool ResClient::Send(short type, const std::string& body) {
	if (closed_.load()) return false;
	std::string frame;
	EncodeResFrame(type, body, frame);
	std::lock_guard<std::mutex> lk(write_mtx_);
	boost::system::error_code ec;
	std::size_t sent = boost::asio::write(socket_, boost::asio::buffer(frame), ec);
	return !ec && sent == frame.size();
}

bool ResClient::Wait(short want_type, int timeout_ms, Frame* out) {
	std::unique_lock<std::mutex> lk(queue_mtx_);
	const auto pred = [&] {
		if (closed_.load()) return true;
		if (want_type <= 0) return !queue_.empty();
		for (const auto& f : queue_) if (f.type == want_type) return true;
		return false;
	};
	if (!pred())
		queue_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), pred);
	if (queue_.empty()) return false;

	std::size_t idx = 0;
	if (want_type > 0) {
		bool found = false;
		for (std::size_t i = 0; i < queue_.size(); ++i) {
			if (queue_[i].type == want_type) { idx = i; found = true; break; }
		}
		if (!found) return false;
	}
	Frame f = std::move(queue_[idx]);
	queue_.erase(queue_.begin() + idx);
	if (out) *out = std::move(f);
	return true;
}

int ResClient::Drain(short want_type, int max_count, int timeout_ms, std::vector<Frame>* out) {
	if (timeout_ms > 0) {
		std::unique_lock<std::mutex> lk(queue_mtx_);
		queue_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
			[&] { return closed_.load(); });
	}
	int collected = 0;
	std::lock_guard<std::mutex> lk(queue_mtx_);
	for (auto it = queue_.begin(); it != queue_.end() && collected < max_count;) {
		if (it->type == want_type) {
			if (out) out->push_back(*it);
			it = queue_.erase(it);
			++collected;
		} else {
			++it;
		}
	}
	return collected;
}

void ResClient::Close() {
	bool expected = false;
	if (!stopping_.compare_exchange_strong(expected, true)) return;

	boost::system::error_code ec;
	socket_.cancel(ec);
	socket_.close(ec);
	closed_ = true;
	queue_cv_.notify_all();

	if (reader_.joinable()) reader_.join();
}

// ---------------------------------------------------------------------------
// Auth helpers
// ---------------------------------------------------------------------------
ChatLoginOutcome ChatLogin(TcpClient& c, int uid, const std::string& token) {
	ChatLoginOutcome out;
	json j;
	j["uid"] = uid;
	j["token"] = token;
	if (!c.Send(ID_CHAT_LOGIN, j.dump())) return out;
	Frame f;
	if (!c.Wait(ID_CHAT_LOGIN_RSP, 10000, &f)) return out;
	out.response = ParseJson(f.body);
	if (!out.response.is_object()) return out;
	out.error = out.response.value("error", -1);
	out.ok = (out.error == ERR_SUCCESS);
	return out;
}

int ResourceLogin(ResClient& r, int uid, const std::string& token) {
	json j;
	j["uid"] = uid;
	j["token"] = token;
	if (!r.Send(ID_RESOURCE_LOGIN_REQ, j.dump())) return -1;
	Frame f;
	if (!r.Wait(ID_RESOURCE_LOGIN_RSP, 10000, &f)) return -1;
	auto rj = ParseJson(f.body);
	if (!rj.is_object()) return -1;
	return rj.value("error", -1);
}

} // namespace imt
