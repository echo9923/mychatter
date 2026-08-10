#pragma once
#include "const.h"

class HttpConnection: public std::enable_shared_from_this<HttpConnection>
{
	friend class LogicSystem;
public:
	HttpConnection(boost::asio::io_context& ioc);
	void Start();
	void PreParseGetParam();
	tcp::socket& GetSocket() {
		return _socket;
	}
	/**
	 * @brief 返回连接 socket 所属的 executor（计划2.2/2.3）
	 *
	 * 供 worker 线程在 handler 完成后 boost::asio::post 回连接的 socket
	 * executor，再设置最终状态并 WriteResponse()，从而避免 worker 线程
	 * 直接跨线程触碰 socket/timer。
	 */
	tcp::socket::executor_type GetExecutor() noexcept {
		return _socket.get_executor();
	}
private:
	void CheckDeadline();
	void WriteResponse();
	void HandleReq();
	tcp::socket  _socket;
	// 用于执行读取操作的缓冲区。
	beast::flat_buffer  _buffer{ 8192 };

	// HTTP 请求消息。
	http::request<http::dynamic_body> _request;

	// HTTP 响应消息。
	http::response<http::dynamic_body> _response;

	// 连接处理超时定时器（60 秒）。
	net::steady_timer deadline_{
		_socket.get_executor(), std::chrono::seconds(60) };

	std::string _get_url;
	std::unordered_map<std::string, std::string> _get_params;
};

