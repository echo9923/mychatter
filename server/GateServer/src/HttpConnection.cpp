#include "HttpConnection.h"
#include "LogicSystem.h"
HttpConnection::HttpConnection(boost::asio::io_context& ioc)
	: _socket(ioc) {
}

//开启监听该链接的数据接受请求
void HttpConnection::Start()
{
	auto self = shared_from_this();
	// 先启动 60 秒连接 deadline（计划2.4）：覆盖读阶段与业务 worker 阻塞阶段，
	// 使业务 worker 阻塞时仍可由连接 executor 关闭 socket；post-back 发现
	// socket 已关闭时只结束生命周期，不再写响应。
	CheckDeadline();
	http::async_read(_socket, _buffer, _request, [self](beast::error_code ec,
		std::size_t bytes_transferred) {
			try {
				if (ec) {
					std::cout << "http read err is " << ec.what() << std::endl;
					return;
				}

				//处理读到的数据

				boost::ignore_unused(bytes_transferred);
				self->HandleReq();
				// CheckDeadline 已在 Start() 开头启动，此处不再重复启动
			}
			catch (std::exception& exp) {
				std::cout << "exception is " << exp.what() << std::endl;
			}
		}
	);
}

//char 转为16进制
unsigned char ToHex(unsigned char x)
{
	return  x > 9 ? x + 55 : x + 48;
}

//16进制转为char
unsigned char FromHex(unsigned char x)
{
	unsigned char y;
	if (x >= 'A' && x <= 'Z') y = x - 'A' + 10;
	else if (x >= 'a' && x <= 'z') y = x - 'a' + 10;
	else if (x >= '0' && x <= '9') y = x - '0';
	else assert(0);
	return y;
}

std::string UrlEncode(const std::string& str)
{
	std::string strTemp = "";
	size_t length = str.length();
	for (size_t i = 0; i < length; i++)
	{
		//判断是否仅有数字和字母构成
		if (isalnum((unsigned char)str[i]) ||
			(str[i] == '-') ||
			(str[i] == '_') ||
			(str[i] == '.') ||
			(str[i] == '~'))
			strTemp += str[i];
		else if (str[i] == ' ') //为空字符
			strTemp += "+";
		else
		{
			//其他字符需要提前加%并且高四位和低四位分别转为16进制
			strTemp += '%';
			strTemp += ToHex((unsigned char)str[i] >> 4);
			strTemp += ToHex((unsigned char)str[i] & 0x0F);
		}
	}
	return strTemp;
}

std::string UrlDecode(const std::string& str)
{
	std::string strTemp = "";
	size_t length = str.length();
	for (size_t i = 0; i < length; i++)
	{
		//还原+为空
		if (str[i] == '+') strTemp += ' ';
		//遇到%将后面的两个字符从16进制转为char再拼接
		else if (str[i] == '%')
		{
			assert(i + 2 < length);
			unsigned char high = FromHex((unsigned char)str[++i]);
			unsigned char low = FromHex((unsigned char)str[++i]);
			strTemp += high * 16 + low;
		}
		else strTemp += str[i];
	}
	return strTemp;
}

void HttpConnection::PreParseGetParam() {
	// 提取 URI  
	auto uri = _request.target();
	// 查找查询字符串的开始位置（即 '?' 的位置）  
	auto query_pos = uri.find('?');
	if (query_pos == std::string::npos) {
		_get_url = uri;
		return;
	}

	_get_url = uri.substr(0, query_pos);
	std::string query_string = uri.substr(query_pos + 1);
	std::string key;
	std::string value;
	size_t pos = 0;
	while ((pos = query_string.find('&')) != std::string::npos) {
		auto pair = query_string.substr(0, pos);
		size_t eq_pos = pair.find('=');
		if (eq_pos != std::string::npos) {
			key = UrlDecode(pair.substr(0, eq_pos)); // 假设有 url_decode 函数来处理URL解码  
			value = UrlDecode(pair.substr(eq_pos + 1));
			_get_params[key] = value;
		}
		query_string.erase(0, pos + 1);
	}
	// 处理最后一个参数对（如果没有 & 分隔符）  
	if (!query_string.empty()) {
		size_t eq_pos = query_string.find('=');
		if (eq_pos != std::string::npos) {
			key = UrlDecode(query_string.substr(0, eq_pos));
			value = UrlDecode(query_string.substr(eq_pos + 1));
			_get_params[key] = value;
		}
	}
}

//处理http请求
void HttpConnection::HandleReq() {
	// 初始化通用响应头：版本对齐请求、短连接、CORS（计划2.3）
	_response.version(_request.version());
	_response.keep_alive(false);
	_response.set(boost::beast::http::field::access_control_allow_origin, "*");

	// 解析路径：GET 需先分离查询参数得到 _get_url，POST 直接取 target
	std::string path;
	if (_request.method() == http::verb::get) {
		PreParseGetParam();
		path = _get_url;
	}
	else {
		path = std::string(_request.target());
	}

	// 唯一入口：查路由并把命中的 handler 投递到有界 worker 池
	auto result = LogicSystem::GetInstance()->Dispatch(
		_request.method(), std::move(path), shared_from_this());

	switch (result) {
	case LogicSystem::DispatchResult::NotFound: {
		// 现有 404 行为
		_response.result(http::status::not_found);
		_response.set(http::field::content_type, "text/plain");
		beast::ostream(_response.body()) << "url not found\r\n";
		WriteResponse();
		break;
	}
	case LogicSystem::DispatchResult::Overloaded:
	case LogicSystem::DispatchResult::Stopping: {
		// HTTP 503 + Retry-After: 1 + {"error":1002}（计划2.3）
		_response.result(http::status::service_unavailable);
		_response.set(http::field::retry_after, "1");
		_response.set(http::field::content_type, "text/json");
		beast::ostream(_response.body()) << R"({"error":1002})";
		WriteResponse();
		break;
	}
	case LogicSystem::DispatchResult::Accepted:
		// handler 已投递到 worker 池，最终响应（200/500）由 post-back 完成，
		// 此处不提前写响应
		break;
	}
}

void HttpConnection::CheckDeadline() {
	auto self = shared_from_this();

	deadline_.async_wait(
		[self](beast::error_code ec)
		{
			if (!ec)
			{
				// Close socket to cancel any outstanding operation.
				self->_socket.close(ec);
			}
		});
}

void HttpConnection::WriteResponse() {
	auto self = shared_from_this();

	_response.content_length(_response.body().size());

	http::async_write(
		_socket,
		_response,
		[self](beast::error_code ec, std::size_t)
		{
			self->_socket.shutdown(tcp::socket::shutdown_send, ec);
			self->deadline_.cancel();
		});
}

