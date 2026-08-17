#include "CSession.h"
#include "CServer.h"
#include <chrono>
#include <iostream>
#include <sstream>
#include "LogicSystem.h"

namespace {
constexpr std::chrono::seconds kGracefulCloseTimeout(5);
}

CSession::CSession(boost::asio::io_context& io_context, CServer* server):
	_socket(io_context), _drain_timer(io_context), _server(server), _b_head_parse(false), _user_uid(0){
	boost::uuids::uuid  a_uuid = boost::uuids::random_generator()();
	_session_id = boost::uuids::to_string(a_uuid);
	_recv_head_node = make_shared<MsgNode>(HEAD_TOTAL_LEN);
}
CSession::~CSession() {
	std::cout << "~CSession destruct" << endl;
}

tcp::socket& CSession::GetSocket() {
	return _socket;
}

std::string& CSession::GetSessionId() {
	return _session_id;
}

bool CSession::TrySetAuth(int uid)
{
	if (uid <= 0) {
		return false;
	}
	std::lock_guard<std::mutex> lock(_lifecycle_mtx);
	if (!_lifecycle.IsOpen() || _authed.load()) {
		return false;
	}
	_user_uid.store(uid);
	_authed.store(true);
	return true;
}

bool CSession::IsAuthed() const
{
	return _authed.load();
}

int CSession::GetUserId() const
{
	return _user_uid.load();
}

bool CSession::IsOpen() const noexcept
{
	return _lifecycle.IsOpen();
}

bool CSession::BeginDrain()
{
	std::lock_guard<std::mutex> lock(_lifecycle_mtx);
	return _lifecycle.BeginDrain();
}

void CSession::Start(){
	//首轮读必须由 socket 所属 IO 线程发起（accept 线程只负责投递），并发修复
	auto self = shared_from_this();
	boost::asio::post(_socket.get_executor(), [self]() {
		if (!self->IsOpen()) {
			return;
		}
		std::cout << "session : " << self->_session_id << " started to read" << std::endl;
		self->AsyncReadHead(HEAD_TOTAL_LEN);
	});
}

void CSession::Send(std::string msg, short msg_type) {
	//投递到 socket 所属 IO 线程执行，避免 worker 线程跨线程触碰 socket（并发修复）
	auto self = shared_from_this();
	boost::asio::post(_socket.get_executor(), [self, msg = std::move(msg), msg_type]() {
		if (!self->IsOpen() || !self->_socket.is_open()) {
			return;
		}
		std::lock_guard<std::mutex> lock(self->_send_lock);
		if (!self->IsOpen()) {
			return;
		}
		int send_que_size = self->_send_que.size();
		if (send_que_size > MAX_SENDQUE) {
			std::cout << "session: " << self->_session_id << " send que fulled, size is " << MAX_SENDQUE << endl;
			return;
		}

		self->_send_que.push(make_shared<SendNode>(msg.c_str(), msg.length(), msg_type));
		if (send_que_size > 0) {
			return;
		}
		auto& msgnode = self->_send_que.front();
		boost::asio::async_write(self->_socket, boost::asio::buffer(msgnode->_data, msgnode->_total_len),
			[self](const boost::system::error_code& error,
				std::size_t /*bytes_transferred*/) {
					self->HandleWrite(error, self);
				});
	});
}

void CSession::SendAndClose(std::string msg, short msg_type) {
	auto self = shared_from_this();
	boost::asio::post(_socket.get_executor(), [self, msg = std::move(msg), msg_type]() {
		bool close_immediately = false;
		{
			std::lock_guard<std::mutex> lock(self->_send_lock);
			if (!self->_socket.is_open()) {
				close_immediately = true;
			}
			else if (!self->BeginDrain()) {
				return;
			}
			else {
				self->_drain_timer.expires_after(kGracefulCloseTimeout);
				self->_drain_timer.async_wait([self](const boost::system::error_code& ec) {
					if (!ec) {
						self->Close();
					}
				});

				self->_send_que.push(make_shared<SendNode>(msg.c_str(), msg.length(), msg_type));
				if (self->_send_que.size() == 1) {
					auto& msgnode = self->_send_que.front();
					boost::asio::async_write(self->_socket,
						boost::asio::buffer(msgnode->_data, msgnode->_total_len),
						[self](const boost::system::error_code& error,
							std::size_t /*bytes_transferred*/) {
								self->HandleWrite(error, self);
							});
				}
			}
		}
		if (close_immediately) {
			self->Close();
		}
	});
}

void CSession::Close() {
	{
		std::lock_guard<std::mutex> send_lock(_send_lock);
		std::lock_guard<std::mutex> lock(_lifecycle_mtx);
		if (!_lifecycle.BeginClose()) {
			return;
		}
	}

	auto self = shared_from_this();
	boost::asio::dispatch(_socket.get_executor(), [self]() {
		boost::system::error_code ignored;
		try {
			self->_drain_timer.cancel();
		}
		catch (const boost::system::system_error&) {
			// Close is idempotent and must remain non-throwing.
		}
		self->_socket.cancel(ignored);
		self->_socket.shutdown(tcp::socket::shutdown_both, ignored);
		self->_socket.close(ignored);
		self->_lifecycle.CompleteClose();
	});
	_server->RemoveSession(self);
}

std::shared_ptr<CSession>CSession::SharedSelf() {
	return shared_from_this();
}

void CSession::AsyncReadBody(int total_len)
{
	auto self = shared_from_this();
	asyncReadFull(total_len, [self, this, total_len](const boost::system::error_code& ec, std::size_t bytes_transfered) {
		try {
			if (ec) {
				std::cout << "handle read failed, error is " << ec.what() << endl;
				Close();
				return;
			}

			if (bytes_transfered < total_len) {
				std::cout << "read length not match, read [" << bytes_transfered << "] , total ["
					<< total_len<<"]" << endl;
				Close();
				return;
			}

			if (!IsOpen()) {
				return;
			}

			memcpy(_recv_msg_node->_data , _data , bytes_transfered);
			_recv_msg_node->_cur_len += bytes_transfered;
			_recv_msg_node->_data[_recv_msg_node->_total_len] = '\0';
			// 使用 std::hash 对字符串进行哈希
			std::hash<std::string> hash_fn;
			size_t hash_value = hash_fn(_session_id); // 生成哈希值
			int index = hash_value % LOGIC_WORKER_COUNT;
			//此处将消息投递到逻辑队列中
			LogicSystem::GetInstance()->PostMsgToQue(make_shared<LogicNode>(shared_from_this(), _recv_msg_node), index);
			//继续监听头部接受事件
			AsyncReadHead(HEAD_TOTAL_LEN);
		}
		catch (std::exception& e) {
			std::cout << "Exception code is " << e.what() << endl;
			Close();
		}
		});
}

void CSession::AsyncReadHead(int total_len)
{
	auto self = shared_from_this();
	asyncReadFull(HEAD_TOTAL_LEN, [self, this](const boost::system::error_code& ec, std::size_t bytes_transfered) {
		try {
			if (ec) {
				std::cout << "handle read failed, error is " << ec.what() << endl;
				Close();
				return;
			}

			if (bytes_transfered < HEAD_TOTAL_LEN) {
				std::cout << "read length not match, read [" << bytes_transfered << "] , total ["
					<< HEAD_TOTAL_LEN << "]" << endl;
				Close();
				return;
			}

			if (!IsOpen()) {
				return;
			}

			_recv_head_node->Clear();
			memcpy(_recv_head_node->_data, _data, bytes_transfered);

			//获取头部消息类型数据
			short msg_type = 0;
			memcpy(&msg_type, _recv_head_node->_data, HEAD_TYPE_LEN);
			//网络字节序转化为本地字节序
			msg_type = boost::asio::detail::socket_ops::network_to_host_short(msg_type);
			std::cout << "msg_type is " << msg_type << endl;
			//类型非法
			if (msg_type > MAX_LENGTH) {
				std::cout << "invalid msg_type is " << msg_type << endl;
				Close();
				return;
			}
			int msg_len = 0;
			memcpy(&msg_len, _recv_head_node->_data + HEAD_TYPE_LEN, HEAD_DATA_LEN);
			//网络字节序转化为本地字节序
			msg_len = boost::asio::detail::socket_ops::network_to_host_long(msg_len);
			std::cout << "msg_len is " << msg_len << endl;

			//长度非法
			if (msg_len > MAX_LENGTH) {
				std::cout << "invalid data length is " << msg_len << endl;
				Close();
				return;
			}

			_recv_msg_node = make_shared<RecvNode>(msg_len, msg_type);
			AsyncReadBody(msg_len);
		}
		catch (std::exception& e) {
			std::cout << "Exception code is " << e.what() << endl;
			Close();
		}
		});
}

void CSession::HandleWrite(const boost::system::error_code& error, std::shared_ptr<CSession> shared_self) {
	//增加异常处理
	try {
		if (!error) {
			bool drain_close = false;
			{
				std::lock_guard<std::mutex> lock(_send_lock);
				_send_que.pop();
				if (!_send_que.empty()) {
					auto& msgnode = _send_que.front();
					boost::asio::async_write(_socket, boost::asio::buffer(msgnode->_data, msgnode->_total_len),
						[shared_self, this](const boost::system::error_code& error,
							std::size_t /*bytes_transferred*/) {
								HandleWrite(error, shared_self);
							});
				}
				else {
					drain_close = _lifecycle.GetState()
						== llfc::SessionLifecycle::State::Draining;
				}
			}
			if (drain_close) {
				Close();
			}
		}
		else {
			std::cout << "handle write failed, error is " << error.what() << endl;
			Close();
		}
	}
	catch (std::exception& e) {
		std::cerr << "Exception code : " << e.what() << endl;
		Close();
	}
	
}

//读取完整长度
void CSession::asyncReadFull(std::size_t maxLength, std::function<void(const boost::system::error_code&, std::size_t)> handler )
{
	::memset(_data, 0, MAX_LENGTH);
	asyncReadLen(0, maxLength, handler);
}

//读取指定字节数
void CSession::asyncReadLen(std::size_t read_len, std::size_t total_len, 
	std::function<void(const boost::system::error_code&, std::size_t)> handler)
{
	auto self = shared_from_this();
	_socket.async_read_some(boost::asio::buffer(_data + read_len, total_len-read_len),
		[read_len, total_len, handler, self](const boost::system::error_code& ec, std::size_t  bytesTransfered) {
			if (ec) {
				// 出现错误，调用回调函数
				handler(ec, read_len + bytesTransfered);
				return;
			}

			if (read_len + bytesTransfered >= total_len) {
				//长度够了就调用回调函数
				handler(ec, read_len + bytesTransfered);
				return;
			}

			// 没有错误，且长度不足则继续读取
			self->asyncReadLen(read_len + bytesTransfered, total_len, handler);
	});
}

LogicNode::LogicNode(shared_ptr<CSession>  session, 
	shared_ptr<RecvNode> recvnode):_session(session),_recvnode(recvnode) {
	
}
