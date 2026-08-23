#include "CSession.h"
#include "CServer.h"
#include "ConfigMgr.h"
#include "LogicSystem.h"
#include "MysqlMgr.h"
#include "RedisMgr.h"
#include "UserMgr.h"
#include <chrono>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace {
/// SendNode 以 short 记录 payload 长度并构造 _total_len=max_len+HEAD_TOTAL_LEN；
/// payload 超过 SHRT_MAX-HEAD_TOTAL_LEN 会使 short 溢出。这是帧上限硬约束（计划5.3）
constexpr int kMaxSendPayload = SHRT_MAX - HEAD_TOTAL_LEN;
constexpr std::chrono::seconds kGracefulCloseTimeout(5);

void CleanupUserPresence(int uid, const std::string &session_id,
                         const std::shared_ptr<CSession> &session) {
    UserMgr::GetInstance()->RmvUserSession(uid, session);

    const auto uid_str = std::to_string(uid);
    const auto lock_key = LOCK_PREFIX + uid_str;
    const auto identifier = RedisMgr::GetInstance()->acquireLock(
        lock_key, LOCK_TIME_OUT, ACQUIRE_TIME_OUT);
    if (identifier.empty()) {
        return;
    }
    Defer release([identifier, lock_key]() {
        RedisMgr::GetInstance()->releaseLock(lock_key, identifier);
    });

    std::string current_session_id;
    if (!RedisMgr::GetInstance()->Get(USER_SESSION_PREFIX + uid_str, current_session_id) || current_session_id != session_id) {
        return;
    }

    RedisMgr::GetInstance()->Del(USER_SESSION_PREFIX + uid_str);
    RedisMgr::GetInstance()->Del(USERIPPREFIX + uid_str);
}
} // namespace

CSession::CSession(boost::asio::io_context &io_context, CServer *server) : _socket(io_context), _drain_timer(io_context), _server(server), _b_head_parse(false), _user_uid(0) {
    boost::uuids::uuid a_uuid = boost::uuids::random_generator()();
    _session_id = boost::uuids::to_string(a_uuid);
    _recv_head_node = make_shared<MsgNode>(HEAD_TOTAL_LEN);
    _last_heartbeat = std::time(nullptr);
}
CSession::~CSession() {
    std::cout << "~CSession destruct" << endl;
}

tcp::socket &CSession::GetSocket() {
    return _socket;
}

std::string &CSession::GetSessionId() {
    return _session_id;
}

bool CSession::TrySetUserId(int uid) {
    if (uid <= 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(_lifecycle_mtx);
    if (!_lifecycle.IsOpen() || _user_uid.load() != 0) {
        return false;
    }
    _user_uid = uid;
    return true;
}

int CSession::GetUserId() const {
    return _user_uid.load();
}

bool CSession::IsOpen() const noexcept {
    return _lifecycle.IsOpen();
}

bool CSession::BeginDrain() {
    std::lock_guard<std::mutex> lock(_lifecycle_mtx);
    return _lifecycle.BeginDrain();
}

bool CSession::BindRoutingUid(int uid) {
    // 首 comparing-exchange：从 0 固定为 uid
    int expected = 0;
    if (_routing_uid.compare_exchange_strong(expected, uid)) {
        return true;
    }
    // 已绑定：只有声明同一 uid 才允许（token 失败后用原 routing uid 重试登录）
    return expected == uid;
}

int CSession::GetRoutingUid() const {
    return _routing_uid.load();
}

void CSession::Start() {
    // 首轮读必须由 socket 所属 IO 线程发起（accept 线程只负责投递），并发修复
    auto self = shared_from_this();
    boost::asio::post(_socket.get_executor(), [self]() {
        if (self->IsOpen()) {
            self->AsyncReadHead(HEAD_TOTAL_LEN);
        }
    });
}

void CSession::Send(std::string msg, short msg_type) {
    // 投递到 socket 所属 IO 线程执行，避免 worker 线程跨线程触碰 socket（并发修复）
    auto self = shared_from_this();
    boost::asio::post(_socket.get_executor(), [self, msg = std::move(msg), msg_type]() {
        if (!self->IsOpen() || !self->_socket.is_open()) {
            return;
        }
        std::lock_guard<std::mutex> lock(self->_send_lock);
        if (!self->IsOpen()) {
            return;
        }
        // 防御性：payload 超过 short 上限会令 SendNode 的长度字段溢出，拒绝并入队（计划5.3）
        if (static_cast<int>(msg.length()) > kMaxSendPayload) {
            std::cout << "session: " << self->_session_id << " drop oversize payload, msgtype=" << msg_type
                      << " length=" << msg.length() << " exceeds " << kMaxSendPayload << endl;
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
        auto &msgnode = self->_send_que.front();
        boost::asio::async_write(self->_socket, boost::asio::buffer(msgnode->_data, msgnode->_total_len),
                                 [self](const boost::system::error_code &error,
                                        std::size_t /*bytes_transferred*/) {
                                     self->HandleWrite(error, self);
                                 });
    });
}

void CSession::SendAndClose(std::string msg, short msg_type) {
    // 投递到 socket 所属 IO 线程执行，避免 worker 线程跨线程触碰 socket（并发修复）
    auto self = shared_from_this();
    boost::asio::post(_socket.get_executor(), [self, msg = std::move(msg), msg_type]() {
        bool close_immediately = false;
        {
            std::lock_guard<std::mutex> lock(self->_send_lock);
            if (!self->_socket.is_open()) {
                close_immediately = true;
            } else if (!self->BeginDrain()) {
                return;
            }
            // 防御性：payload 超过 short 上限会令 SendNode 的长度字段溢出；终帧虽小但仍统一检查（计划5.3）
            else if (static_cast<int>(msg.length()) > kMaxSendPayload) {
                std::cout << "session: " << self->_session_id << " drop oversize terminal payload, msgtype=" << msg_type
                          << " length=" << msg.length() << " exceeds " << kMaxSendPayload << endl;
                close_immediately = true;
            } else {
                // 启动优雅关闭计时器：在 kGracefulCloseTimeout 时间内未完成发送则强制关闭连接
                self->_drain_timer.expires_after(kGracefulCloseTimeout);
                // 异步等待计时器到期，到期后回调中关闭会话
                self->_drain_timer.async_wait([self](const boost::system::error_code &ec) {
                    if (!ec) { // ec 非空表示计时器被取消（如会话已关闭），无需再处理
                        self->Close();
                    }
                });
                // 将终止消息封装为发送节点，投递到发送队列等待异步写出
                self->_send_que.push(make_shared<SendNode>(msg.c_str(), msg.length(), msg_type));
                if (self->_send_que.size() == 1) {
                    auto &msgnode = self->_send_que.front();
                    boost::asio::async_write(self->_socket,
                                             boost::asio::buffer(msgnode->_data, msgnode->_total_len),
                                             [self](const boost::system::error_code &error,
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
    int uid = 0;
    {
        std::lock_guard<std::mutex> send_lock(_send_lock);
        std::lock_guard<std::mutex> lock(_lifecycle_mtx);
        if (!_lifecycle.BeginClose()) {
            return;
        }
        uid = _user_uid.load();
    }

    auto self = shared_from_this();
    boost::asio::dispatch(_socket.get_executor(), [self]() {
        boost::system::error_code ignored;
        try {
            self->_drain_timer.cancel();
        } catch (const boost::system::system_error &) {
            // Close is idempotent and must remain non-throwing.
        }
        self->_socket.cancel(ignored);
        self->_socket.shutdown(tcp::socket::shutdown_both, ignored);
        self->_socket.close(ignored);
        self->_lifecycle.CompleteClose();
    });

    // 先从连接表摘除；延迟回调只能摘除同一个对象，不能误删替代会话。
    _server->RemoveSession(self);

    if (uid > 0) {
        auto cleanup = [uid, session_id = _session_id, self]() {
            CleanupUserPresence(uid, session_id, self);
        };
        // 在线身份属于 uid 业务状态：排到同一分片，避免阻塞 socket I/O，
        // 并保证与正在进行的登录绑定按 FIFO 串行。
        if (!LogicSystem::GetInstance()->PostToUser(uid, cleanup)) {
            cleanup();
        }
    }
}

std::shared_ptr<CSession> CSession::SharedSelf() {
    return shared_from_this();
}

void CSession::AsyncReadBody(int total_len) {
    auto self = shared_from_this();
    asyncReadFull(total_len, [self, this, total_len](const boost::system::error_code &ec, std::size_t bytes_transfered) {
        try {
            if (ec) {
                std::cout << "handle read failed, error is " << ec.what() << endl;
                Close();
                return;
            }

            if (bytes_transfered < total_len) {
                std::cout << "read length not match, read [" << bytes_transfered << "] , total ["
                          << total_len << "]" << endl;
                Close();
                return;
            }

            if (!IsOpen()) {
                return;
            }

            memcpy(_recv_msg_node->_data, _data, bytes_transfered);
            _recv_msg_node->_cur_len += bytes_transfered;
            _recv_msg_node->_data[_recv_msg_node->_total_len] = '\0';
            cout << "receive data is " << _recv_msg_node->_data << endl;
            // 更新session心跳时间
            UpdateHeartbeat();

            const short msg_type = _recv_msg_node->GetMsgType();
            std::size_t routing_key = 0;

            if (msg_type == MSG_CHAT_LOGIN) {
                // 登录包：仅解析正整数 uid 以固定路由分片，不在 IO 线程处理登录业务
                int login_uid = 0;
                try {
                    auto root = json::parse(std::string(_recv_msg_node->_data, _recv_msg_node->_cur_len), nullptr, false);
                    if (root.is_object() && root.contains("uid") && root["uid"].is_number_integer()) {
                        login_uid = root["uid"].get<int>();
                    }
                } catch (std::exception &e) {
                    std::cout << "parse login routing uid failed, " << e.what() << endl;
                }

                if (login_uid <= 0 || !BindRoutingUid(login_uid)) {
                    // uid 非正整数或同连接声明了不同 uid：原子发送错误终帧，写完后关闭，不进入任何 handler
                    json err;
                    err["error"] = ErrorCodes::UidInvalid;
                    SendAndClose(err.dump(4), MSG_CHAT_LOGIN_RSP);
                    return;
                }
                routing_key = std::hash<int>{}(GetRoutingUid());
            } else {
                int routing_uid = GetRoutingUid();
                if (routing_uid == 0) {
                    // 未绑定 uid 时收到非登录消息：直接关闭，不进入 handler
                    Close();
                    return;
                }
                routing_key = std::hash<int>{}(routing_uid);
            }

            // 按路由 uid 的 hash 固定投递到某个 logic worker 分片（计划1.2）
            bool posted = LogicSystem::GetInstance()->PostMsgToQue(routing_key,
                                                                   make_shared<LogicNode>(shared_from_this(), _recv_msg_node));
            if (!posted) {
                // 服务端停机/队列拒绝：该消息绝不入队、绝不持久化
                short rsp_id = ReqToRspId(msg_type);
                if (rsp_id != 0) {
                    json busy;
                    busy["error"] = ErrorCodes::SERVER_BUSY;
                    // 原子发送 SERVER_BUSY 终帧，写完后关闭
                    SendAndClose(busy.dump(4), rsp_id);
                } else {
                    Close();
                }
                return;
            }

            // 继续监听头部接受事件
            AsyncReadHead(HEAD_TOTAL_LEN);
        } catch (std::exception &e) {
            std::cout << "Exception code is " << e.what() << endl;
            Close();
        }
    });
}

void CSession::AsyncReadHead(int total_len) {
    auto self = shared_from_this();
    asyncReadFull(HEAD_TOTAL_LEN, [self, this](const boost::system::error_code &ec, std::size_t bytes_transfered) {
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

            // 获取头部消息类型数据
            short msg_type = 0;
            memcpy(&msg_type, _recv_head_node->_data, HEAD_TYPE_LEN);
            // 网络字节序转化为本地字节序
            msg_type = boost::asio::detail::socket_ops::network_to_host_short(msg_type);
            std::cout << "msg_type is " << msg_type << endl;
            // 类型非法
            if (msg_type > MAX_LENGTH) {
                std::cout << "invalid msg_type is " << msg_type << endl;
                Close();
                return;
            }
            short msg_len = 0;
            memcpy(&msg_len, _recv_head_node->_data + HEAD_TYPE_LEN, HEAD_DATA_LEN);
            // 网络字节序转化为本地字节序
            msg_len = boost::asio::detail::socket_ops::network_to_host_short(msg_len);
            std::cout << "msg_len is " << msg_len << endl;

            // 长度非法
            if (msg_len > MAX_LENGTH) {
                std::cout << "invalid data length is " << msg_len << endl;
                Close();
                return;
            }

            _recv_msg_node = make_shared<RecvNode>(msg_len, msg_type);
            AsyncReadBody(msg_len);
        } catch (std::exception &e) {
            std::cout << "Exception code is " << e.what() << endl;
            Close();
        }
    });
}

void CSession::HandleWrite(const boost::system::error_code &error, std::shared_ptr<CSession> shared_self) {
    // 增加异常处理
    try {
        if (!error) {
            bool drain_close = false;
            {
                std::lock_guard<std::mutex> lock(_send_lock);
                // cout << "send data " << _send_que.front()->_data+HEAD_LENGTH << endl;
                _send_que.pop();
                if (!_send_que.empty()) {
                    auto &msgnode = _send_que.front();
                    boost::asio::async_write(_socket, boost::asio::buffer(msgnode->_data, msgnode->_total_len),
                                             [shared_self, this](const boost::system::error_code &error,
                                                                 std::size_t /*bytes_transferred*/) {
                                                 HandleWrite(error, shared_self);
                                             });
                } else {
                    // 队列已排空：若是 SendAndClose 安排的终帧，现在才真正关闭
                    drain_close = _lifecycle.GetState() == llfc::SessionLifecycle::State::Draining;
                }
            }
            if (drain_close) {
                Close();
            }
        } else {
            std::cout << "handle write failed, error is " << error.what() << endl;
            Close();
        }
    } catch (std::exception &e) {
        std::cerr << "Exception code : " << e.what() << endl;
        Close();
    }
}

// 读取完整长度
void CSession::asyncReadFull(std::size_t maxLength, std::function<void(const boost::system::error_code &, std::size_t)> handler) {
    ::memset(_data, 0, MAX_LENGTH);
    asyncReadLen(0, maxLength, handler);
}

// 读取指定字节数
void CSession::asyncReadLen(std::size_t read_len, std::size_t total_len,
                            std::function<void(const boost::system::error_code &, std::size_t)> handler) {
    auto self = shared_from_this();
    _socket.async_read_some(boost::asio::buffer(_data + read_len, total_len - read_len),
                            [read_len, total_len, handler, self](const boost::system::error_code &ec, std::size_t bytesTransfered) {
                                if (ec) {
                                    // 出现错误，调用回调函数
                                    handler(ec, read_len + bytesTransfered);
                                    return;
                                }

                                if (read_len + bytesTransfered >= total_len) {
                                    // 长度够了就调用回调函数
                                    handler(ec, read_len + bytesTransfered);
                                    return;
                                }

                                // 没有错误，且长度不足则继续读取
                                self->asyncReadLen(read_len + bytesTransfered, total_len, handler);
                            });
}

void CSession::NotifyOffline(int uid) {

    json rtvalue;
    rtvalue["error"] = ErrorCodes::Success;
    rtvalue["uid"] = uid;

    std::string return_str = rtvalue.dump(4);

    SendAndClose(return_str, ID_NOTIFY_OFF_LINE_REQ);
    return;
}

void CSession::NotifyResourceRecv(const std::shared_ptr<ChatMessage> &msg) {
    if (!msg) {
        return;
    }
    // 1505 通用资源消息通知：与 1303/1406 同构的统一 envelope
    // （msg_type/content/hash/mime/resource_status 均取 DB 真值）
    json rtvalue;
    rtvalue["error"] = ErrorCodes::Success;
    rtvalue["message_id"] = std::to_string(msg->message_id);
    rtvalue["unique_id"] = "";
    rtvalue["thread_id"] = std::to_string(msg->thread_id);
    rtvalue["fromuid"] = msg->sender_id;
    rtvalue["touid"] = msg->recv_id;
    rtvalue["msg_type"] = msg->msg_type;
    rtvalue["content"] = msg->content;
    rtvalue["content_size"] = std::to_string(msg->content_size);
    rtvalue["chat_time"] = msg->chat_time;
    rtvalue["status"] = msg->status;
    rtvalue["resource_status"] = static_cast<int>(msg->resource_status);
    rtvalue["content_hash"] = msg->content_hash;
    rtvalue["mime_type"] = msg->mime_type;

    std::string return_str = rtvalue.dump(4);
    Send(return_str, ID_NOTIFY_RESOURCE_MSG_REQ);
    return;
}

LogicNode::LogicNode(shared_ptr<CSession> session,
                     shared_ptr<RecvNode> recvnode) : _session(session), _recvnode(recvnode) {
}

bool CSession::IsHeartbeatExpired(std::time_t &now) {
    if (!IsOpen()) {
        return false;
    }
    // 过期阈值可配置（[Heartbeat] ExpiryThresholdSeconds），缺省 20 秒
    int threshold = 20;
    auto threshold_str = ConfigMgr::Inst()["Heartbeat"]["ExpiryThresholdSeconds"];
    if (!threshold_str.empty()) {
        int parsed = atoi(threshold_str.c_str());
        if (parsed > 0) {
            threshold = parsed;
        }
    }
    double diff_sec = std::difftime(now, _last_heartbeat);
    if (diff_sec > threshold) {
        std::cout << "heartbeat expired, session id is  " << _session_id << endl;
        return true;
    }

    return false;
}

void CSession::UpdateHeartbeat() {
    time_t now = std::time(nullptr);
    _last_heartbeat = now;
}
