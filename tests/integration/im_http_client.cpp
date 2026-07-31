// im_http_client.cpp — Boost.Beast synchronous HTTP/1.1 client.
#include "im_http_client.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <utility>

namespace imt {

namespace http = boost::beast::http;
namespace beast = boost::beast;
namespace asio = boost::asio;
using boost::asio::ip::tcp;

static HttpResponse DoRequest(http::verb verb, const std::string& host,
                              unsigned short port, const std::string& target,
                              const std::string& body, const std::string& content_type,
                              int timeout_ms) {
	HttpResponse out;
	asio::io_context ioc;
	tcp::resolver resolver(ioc);
	beast::tcp_stream stream(ioc);
	stream.expires_after(std::chrono::milliseconds(timeout_ms));

	boost::system::error_code ec;
	auto const results = resolver.resolve(host, std::to_string(port), ec);
	if (ec) { out.error = "resolve: " + ec.message(); return out; }
	stream.connect(results, ec);
	if (ec) { out.error = "connect: " + ec.message(); return out; }

	http::request<http::string_body> req{verb, target, 11};
	req.set(http::field::host, host);
	req.set(http::field::user_agent, "im_integration_tests/1.0");
	req.keep_alive(false);
	if (!body.empty()) {
		req.body() = body;
		req.set(http::field::content_type, content_type);
		req.prepare_payload();
	}

	stream.expires_after(std::chrono::milliseconds(timeout_ms));
	http::write(stream, req, ec);
	if (ec) { out.error = "write: " + ec.message(); return out; }

	http::response<http::string_body> res;
	beast::flat_buffer buf;
	stream.expires_after(std::chrono::milliseconds(timeout_ms));
	http::read(stream, buf, res, ec);
	if (ec) { out.error = "read: " + ec.message(); return out; }

	out.status = res.result_int();
	out.body = res.body();

	boost::system::error_code ignore;
	stream.socket().shutdown(tcp::socket::shutdown_both, ignore);
	(void)ignore;
	return out;
}

HttpResponse HttpGet(const std::string& host, unsigned short port,
                     const std::string& target, int timeout_ms) {
	return DoRequest(http::verb::get, host, port, target, "", "", timeout_ms);
}

HttpResponse HttpPost(const std::string& host, unsigned short port,
                      const std::string& target, const std::string& body,
                      const std::string& content_type, int timeout_ms) {
	return DoRequest(http::verb::post, host, port, target, body,
		content_type.empty() ? "text/json" : content_type, timeout_ms);
}

} // namespace imt
