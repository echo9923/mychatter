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
	//发送预算在 post 之前预留：同时覆盖"已 post 到 IO 线程但尚未入队"的
	//闭包与已在 _send_que 中的帧。超限 = 对端不读导致积压，关闭会话
	//（Close 在 _send_lock 外调用）。
	const std::size_t frame_bytes = HEAD_TOTAL_LEN + msg.length();
	if (!TryReserveSendBudget(frame_bytes)) {
		std::cerr << "session: " << _session_id
			<< " send budget exhausted, closing" << std::endl;
		Close();
		return;
	}
	//RAII 守卫：任何未入队路径（会话已关/socket 已关）在闭包析构时归还预留。
	//先取 self：guard 持有 shared_ptr，析构归还时会话必然存活
	auto self = shared_from_this();
	auto budget = std::make_shared<SendBudgetGuard>(self, frame_bytes);

	//投递到 socket 所属 IO 线程执行，避免 worker 线程跨线程触碰 socket（并发修复）
	boost::asio::post(_socket.get_executor(),
		[self, msg = std::move(msg), msg_type, budget]() {
			if (!self->IsOpen() || !self->_socket.is_open()) {
				return;  //budget 析构归还预留
			}
			{
				std::lock_guard<std::mutex> lock(self->_send_lock);
				if (!self->IsOpen()) {
					return;  //归还预留
				}
				self->_send_que.push(make_shared<SendNode>(msg.c_str(), msg.length(), msg_type));
				budget->disarm();  //所有权移交队列：HandleWrite pop 时归还
				if (self->_send_que.size() > 1) {
					return;  //已有在途写，队列消费时继续
				}
				//回调共享持有 front：失败路径清队列时缓冲仍保活，不提前释放
				auto msgnode = self->_send_que.front();
				boost::asio::async_write(self->_socket,
					boost::asio::buffer(msgnode->_data, msgnode->_total_len),
					[self, msgnode](const boost::system::error_code& error,
						std::size_t /*bytes_transferred*/) {
						self->HandleWrite(error, self);
					});
			}
		});
}

void CSession::SendAndClose(std::string msg, short msg_type) {
	const std::size_t frame_bytes = HEAD_TOTAL_LEN + msg.length();
	if (!TryReserveSendBudget(frame_bytes)) {
		std::cerr << "session: " << _session_id
			<< " send budget exhausted, closing" << std::endl;
		Close();
		return;
	}
	auto self = shared_from_this();
	auto budget = std::make_shared<SendBudgetGuard>(self, frame_bytes);

	boost::asio::post(_socket.get_executor(),
		[self, msg = std::move(msg), msg_type, budget]() {
			bool close_immediately = false;
			{
				std::lock_guard<std::mutex> lock(self->_send_lock);
				if (!self->_socket.is_open()) {
					close_immediately = true;  //budget 归还（未 disarm）
				}
				else if (!self->BeginDrain()) {
					return;  //已在排空：归还预留
				}
				else {
					self->_drain_timer.expires_after(kGracefulCloseTimeout);
					self->_drain_timer.async_wait([self](const boost::system::error_code& ec) {
						if (!ec) {
							self->Close();
						}
					});

					self->_send_que.push(make_shared<SendNode>(msg.c_str(), msg.length(), msg_type));
					budget->disarm();
					if (self->_send_que.size() == 1) {
						//回调共享持有 front：失败路径清队列时缓冲仍保活，不提前释放
						auto msgnode = self->_send_que.front();
						boost::asio::async_write(self->_socket,
							boost::asio::buffer(msgnode->_data, msgnode->_total_len),
							[self, msgnode](const boost::system::error_code& error,
								std::size_t /*bytes_transferred*/) {
								self->HandleWrite(error, self);
							});
					}
				}
			}
			if (close_immediately) {
				self->Close();  //锁外
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
	//包体直接读入按帧精确分配的 RecvNode，不再经过整帧 MAX_LENGTH 中转缓冲
	asyncReadFull(_recv_msg_node->_data, total_len,
		[self, this, total_len](const boost::system::error_code& ec, std::size_t bytes_transfered) {
		try {
			if (ec) {
				std::cerr << "handle read failed, error is " << ec.message() << std::endl;
				Close();
				return;
			}

			if (bytes_transfered < total_len) {
				std::cerr << "read length not match, read [" << bytes_transfered << "] , total ["
					<< total_len << "]" << std::endl;
				Close();
				return;
			}

			if (!IsOpen()) {
				return;
			}

			//首层队列配额：帧进入 LogicWorker 队列前按整帧字节（含 6B 头）
			//占用，RAII 令牌随 LogicNode 一路传递到 File/Download 任务，
			//整条链路执行完销毁即归还；超限说明单会话任务积压（8MiB），
			//按滥用处置关闭连接。
			auto quota = SessionTaskQuota::Acquire(self,
				HEAD_TOTAL_LEN + static_cast<std::size_t>(total_len));
			if (!quota) {
				std::cerr << "session: " << _session_id
					<< " inflight task budget exhausted, closing" << std::endl;
				Close();
				return;
			}

			_recv_msg_node->_cur_len = static_cast<int>(bytes_transfered);
			// 使用 std::hash 对字符串进行哈希
			std::hash<std::string> hash_fn;
			size_t hash_value = hash_fn(_session_id); // 生成哈希值
			int index = hash_value % LOGIC_WORKER_COUNT;
			//此处将消息投递到逻辑队列中（配额令牌随任务下传）
			LogicSystem::GetInstance()->PostMsgToQue(
				make_shared<LogicNode>(shared_from_this(), _recv_msg_node,
					std::move(quota)), index);
			//继续监听头部接受事件
			AsyncReadHead(HEAD_TOTAL_LEN);
		}
		catch (std::exception& e) {
			std::cerr << "Exception code is " << e.what() << std::endl;
			Close();
		}
		});
}

void CSession::AsyncReadHead(int total_len)
{
	auto self = shared_from_this();
	asyncReadFull(_head_data, HEAD_TOTAL_LEN,
		[self, this](const boost::system::error_code& ec, std::size_t bytes_transfered) {
		try {
			if (ec) {
				std::cerr << "handle read failed, error is " << ec.message() << std::endl;
				Close();
				return;
			}

			if (bytes_transfered < HEAD_TOTAL_LEN) {
				std::cerr << "read length not match, read [" << bytes_transfered << "] , total ["
					<< HEAD_TOTAL_LEN << "]" << std::endl;
				Close();
				return;
			}

			if (!IsOpen()) {
				return;
			}

			//获取头部消息类型数据
			unsigned short msg_type = 0;
			memcpy(&msg_type, _head_data, HEAD_TYPE_LEN);
			//网络字节序转化为本地字节序
			msg_type = boost::asio::detail::socket_ops::network_to_host_short(msg_type);
			//类型非法（与 ChatServer 同一上限规则）
			if (msg_type > llfc_proto::kMaxMsgId) {
				std::cerr << "invalid msg_type is " << msg_type << std::endl;
				Close();
				return;
			}
			unsigned int msg_len = 0;
			memcpy(&msg_len, _head_data + HEAD_TYPE_LEN, HEAD_DATA_LEN);
			//网络字节序转化为本地字节序；无符号比较，杜绝负长度/符号溢出
			msg_len = boost::asio::detail::socket_ops::network_to_host_long(msg_len);

			//长度非法（零长度包体无合法语义，且会绕过按字节计的配额）
			if (msg_len == 0 || msg_len > MAX_LENGTH) {
				std::cerr << "invalid data length is " << msg_len << std::endl;
				Close();
				return;
			}
			//未鉴权会话只允许小帧（1501 登录握手），防未认证大帧攻击
			if (!IsAuthed() && msg_len > kUnauthedMaxBodyLen) {
				std::cerr << "unauthed oversized frame, len=" << msg_len << std::endl;
				Close();
				return;
			}

			_recv_msg_node = make_shared<RecvNode>(static_cast<int>(msg_len),
				static_cast<short>(msg_type));
			AsyncReadBody(static_cast<int>(msg_len));
		}
		catch (std::exception& e) {
			std::cerr << "Exception code is " << e.what() << std::endl;
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
				if (!_send_que.empty()) {
					//SendNode::_total_len 已含 6B 头；已持锁，直接内联归还
					//（ReleaseSendBudget 会再次加锁，此处不可调用）
					const std::size_t done_bytes = _send_que.front()->_total_len;
					if (_send_pending_frames > 0) {
						--_send_pending_frames;
					}
					if (_send_pending_bytes >= done_bytes) {
						_send_pending_bytes -= done_bytes;
					}
					else {
						_send_pending_bytes = 0;
					}
					_send_que.pop();
				}
				if (!_send_que.empty()) {
					auto msgnode = _send_que.front();  //回调共享持有 front：清队列时缓冲保活
					boost::asio::async_write(_socket, boost::asio::buffer(msgnode->_data, msgnode->_total_len),
						[shared_self, this, msgnode](const boost::system::error_code& error,
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
			std::cerr << "handle write failed, error is " << error.message() << std::endl;
			//失败回调已触发：本次 write 已结束，且持 _send_lock 期间不会有新的
			//async_write 启动，清队列无提前释放风险。逐帧对称归还预算（内联
			//扣减，禁止在锁内调用会再次加锁的 ReleaseSendBudget），锁外再 Close
			{
				std::lock_guard<std::mutex> lock(_send_lock);
				ClearSendQueueLocked();
			}
			Close();
		}
	}
	catch (std::exception& e) {
		std::cerr << "Exception code : " << e.what() << std::endl;
		//异常点不确定：async_write 同步抛出则操作未启动；Close 等后续代码抛出
		//则可能仍有在途写——其回调已捕获 front 共享所有权，出队不悬垂
		{
			std::lock_guard<std::mutex> lock(_send_lock);
			ClearSendQueueLocked();
		}
		Close();
	}
}

void CSession::ClearSendQueueLocked() {
	while (!_send_que.empty()) {
		//SendNode::_total_len 已含 6B 头，与 TryReserveSendBudget 的计入对称；
		//已持锁，只能内联扣减（ReleaseSendBudget 会再次加锁）
		const std::size_t frame_bytes = _send_que.front()->_total_len;
		if (_send_pending_frames > 0) {
			--_send_pending_frames;
		}
		if (_send_pending_bytes >= frame_bytes) {
			_send_pending_bytes -= frame_bytes;
		}
		else {
			_send_pending_bytes = 0;
		}
		_send_que.pop();
	}
}

//读取完整长度（直接读入 dest，无逐帧 memset）
void CSession::asyncReadFull(char* dest, std::size_t total_len,
	std::function<void(const boost::system::error_code&, std::size_t)> handler)
{
	asyncReadLen(dest, 0, total_len, handler);
}

//读取指定字节数
void CSession::asyncReadLen(char* dest, std::size_t read_len, std::size_t total_len,
	std::function<void(const boost::system::error_code&, std::size_t)> handler)
{
	auto self = shared_from_this();
	_socket.async_read_some(boost::asio::buffer(dest + read_len, total_len - read_len),
		[dest, read_len, total_len, handler, self](const boost::system::error_code& ec, std::size_t bytesTransfered) {
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
			self->asyncReadLen(dest, read_len + bytesTransfered, total_len, handler);
	});
}

bool CSession::TryAcquireTaskBytes(std::size_t bytes) {
	if (bytes == 0) {
		return true;
	}
	std::size_t current = _inflight_task_bytes.load(std::memory_order_relaxed);
	while (true) {
		if (current > kMaxSessionInflightTaskBytes
			|| bytes > kMaxSessionInflightTaskBytes - current) {
			return false;
		}
		if (_inflight_task_bytes.compare_exchange_weak(current, current + bytes,
			std::memory_order_acq_rel, std::memory_order_relaxed)) {
			return true;
		}
	}
}

void CSession::ReleaseTaskBytes(std::size_t bytes) {
	if (bytes == 0) {
		return;
	}
	_inflight_task_bytes.fetch_sub(bytes, std::memory_order_acq_rel);
}

bool CSession::TryReserveSendBudget(std::size_t frame_bytes) {
	std::lock_guard<std::mutex> lock(_send_lock);
	if (_send_pending_frames >= kMaxSendQueFrames
		|| _send_pending_bytes + frame_bytes > kMaxSendQueBytes) {
		return false;
	}
	++_send_pending_frames;
	_send_pending_bytes += frame_bytes;
	return true;
}

void CSession::ReleaseSendBudget(std::size_t frame_bytes) {
	std::lock_guard<std::mutex> lock(_send_lock);
	if (_send_pending_frames > 0) {
		--_send_pending_frames;
	}
	if (_send_pending_bytes >= frame_bytes) {
		_send_pending_bytes -= frame_bytes;
	}
	else {
		_send_pending_bytes = 0;  //防御：不应发生（保留计数下限保护）
	}
}

std::shared_ptr<SessionTaskQuota> SessionTaskQuota::Acquire(
	std::shared_ptr<CSession> session, std::size_t bytes) {
	if (!session || !session->TryAcquireTaskBytes(bytes)) {
		return nullptr;
	}
	return std::shared_ptr<SessionTaskQuota>(
		new SessionTaskQuota(std::move(session), bytes));
}

SessionTaskQuota::SessionTaskQuota(std::shared_ptr<CSession> session, std::size_t bytes)
	: _session(std::move(session)), _bytes(bytes) {
}

SessionTaskQuota::~SessionTaskQuota() {
	if (_session) {
		_session->ReleaseTaskBytes(_bytes);
	}
}

LogicNode::LogicNode(shared_ptr<CSession> session,
	shared_ptr<RecvNode> recvnode,
	shared_ptr<SessionTaskQuota> quota)
	:_session(std::move(session)), _recvnode(std::move(recvnode)), _quota(std::move(quota)) {

}
