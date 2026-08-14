#pragma once
#include <boost/asio.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast.hpp>
#include <boost/asio.hpp>
#include <queue>
#include <mutex>
#include <memory>
#include "const.h"
#include "MsgNode.h"
#include "data.h"
#include "chat.grpc.pb.h"
#include "chat.pb.h"
#include <grpcpp/grpcpp.h>
using namespace std;


namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

class CServer;
class LogicSystem;

/**
 * @brief 客户端TCP会话类
 * 
 * 代表一个客户端与ChatServer之间的TCP连接会话，负责：
 * - 异步读取客户端发送的消息（分包解析：先读头部，再读数据体）
 * - 向客户端发送消息（带发送队列和异步写入）
 * - 心跳管理（记录最后心跳时间，检测超时）
 * - 会话生命周期管理（登录、下线、异常处理）
 */
class CSession: public std::enable_shared_from_this<CSession>
{
public:
	/**
	 * @brief 构造函数，创建一个新的客户端会话
	 * @param io_context 该会话绑定的IO上下文（来自IOServicePool轮询分配）
	 * @param server 所属的CServer指针，用于会话注册/注销
	 */
	CSession(boost::asio::io_context& io_context, CServer* server);

	/// 析构函数，关闭socket连接
	~CSession();

	/**
	 * @brief 获取该会话的TCP socket引用
	 * @return socket引用，用于acceptor接受连接时绑定
	 */
	tcp::socket& GetSocket();

	/**
	 * @brief 获取会话的唯一标识符（UUID生成）
	 * @return 会话ID字符串引用
	 */
	std::string& GetSessionId();

	/**
	 * @brief 设置该会话对应的用户ID（登录成功后调用）
	 * @param uid 用户ID
	 */
	void SetUserId(int uid);

	/**
	 * @brief 获取该会话对应的用户ID
	 * @return 用户ID，未登录时为0
	 */
	int GetUserId() const;

	/**
	 * @brief 绑定路由分片用的用户ID（计划1.3）
	 *
	 * 仅在第一条合法 MSG_CHAT_LOGIN 包上用 compare-exchange 从 0 固定为 uid，
	 * 保证同一连接的登录与紧随其后的业务包从第一包起进入同一 worker 分片。
	 * @param uid 登录请求中解析出的正整数 uid
	 * @return 首次绑定成功、或已绑定到同一 uid 时返回 true；已绑定到不同 uid 时返回 false
	 */
	bool BindRoutingUid(int uid);

	/**
	 * @brief 获取路由分片绑定的用户ID
	 * @return 路由 uid，未绑定时为0
	 */
	int GetRoutingUid() const;

	/// 启动会话，开始异步读取客户端数据
	void Start();

	/**
	 * @brief 向客户端发送消息（字符串版本）
	 * @param msg 消息内容字符串（通常为JSON）
	 * @param msg_type 消息类型
	 */
	void Send(std::string msg, short msg_type);

	/// 关闭会话，断开TCP连接
	void Close();

	/**
	 * @brief 原子地发送最后一帧并安排写完即关
	 *
	 * 在同一把 _send_lock 内置关闭标志并入队终帧：之后的 Send 一律被拒绝，
	 * HandleWrite 在发送队列排空后才真正 Close，保证错误响应完整写出。
	 * @param msg 消息内容字符串（通常为JSON错误响应）
	 * @param msg_type 消息类型
	 */
	void SendAndClose(std::string msg, short msg_type);

	/**
	 * @brief 获取自身的shared_ptr（用于异步回调中延长生命周期）
	 * @return 当前会话的共享指针
	 */
	std::shared_ptr<CSession> SharedSelf();

	/**
	 * @brief 异步读取消息体（已知数据长度后调用）
	 * @param length 要读取的消息体字节数
	 */
	void AsyncReadBody(int length);

	/**
	 * @brief 异步读取消息头部（解析消息类型和数据长度）
	 * @param total_len 头部总长度
	 */
	void AsyncReadHead(int total_len);

	/**
	 * @brief 通知客户端被踢下线（发送下线通知消息）
	 * @param uid 被踢用户的ID
	 */
	void NotifyOffline(int uid);

	/**
	 * @brief 通知客户端接收图片聊天消息（由gRPC服务层调用）
	 * @param request 图片聊天通知请求
	 */
	void NotifyResourceRecv(const std::shared_ptr<ChatMessage>& msg);

	/**
	 * @brief 判断心跳是否已超时
	 * @param now 当前时间戳
	 * @return true表示心跳已过期，应断开连接
	 */
	bool IsHeartbeatExpired(std::time_t& now);

	/// 更新心跳时间戳为当前时间
	void UpdateHeartbeat();

	/// 处理异常会话（连接断开、错误等），清理资源并通知服务器
	void DealExceptionSession();

private:
	/**
	 * @brief 异步读取指定最大长度的数据（底层读取封装）
	 * @param maxLength 最大读取字节数
	 * @param handler 读取完成后的回调函数
	 */
	void asyncReadFull(std::size_t maxLength, std::function<void(const boost::system::error_code& , std::size_t)> handler);

	/**
	 * @brief 异步读取精确长度的数据（用于分包读取）
	 * @param read_len 已读取的字节数
	 * @param total_len 需要读取的总字节数
	 * @param handler 读取完成后的回调函数
	 */
	void asyncReadLen(std::size_t  read_len, std::size_t total_len,
		std::function<void(const boost::system::error_code&, std::size_t)> handler);
	
	/**
	 * @brief 异步写入完成的回调处理
	 * @param error 写入操作的错误码
	 * @param shared_self 会话自身的共享指针，防止回调期间会话被析构
	 */
	void HandleWrite(const boost::system::error_code& error, std::shared_ptr<CSession> shared_self);

	/// TCP socket，与客户端的实际网络连接
	tcp::socket _socket;
	/// 会话唯一标识符（UUID），用于在服务器中唯一标识该连接
	std::string _session_id;
	/// 数据接收缓冲区，存储从socket读取的原始数据
	char _data[MAX_LENGTH];
	/// 所属CServer指针，用于会话注册/注销和状态查询
	CServer* _server;
	/// 会话关闭标志，为true时表示会话已关闭不再处理消息
	bool _b_close;
	/// 写完即关标志（_send_lock 保护）：为true时拒绝新 Send，队列排空后由 HandleWrite 关闭
	bool _close_after_send{false};
	/// 发送队列，缓存待发送给客户端的消息节点
	std::queue<shared_ptr<SendNode> > _send_que;
	/// 发送队列互斥锁，保护_send_que的线程安全
	std::mutex _send_lock;
	/// 当前正在接收的消息节点（包含完整消息体）
	std::shared_ptr<RecvNode> _recv_msg_node;
	/// 头部解析标志，true表示当前正在解析消息头部
	bool _b_head_parse;
	/// 当前接收到的消息头部节点（包含消息类型和数据长度）
	std::shared_ptr<MsgNode> _recv_head_node;
	/// 该会话对应的用户ID（登录成功后设置，原子变量保证 worker/IO 线程安全读取）
	std::atomic<int> _user_uid{0};
	/// 路由分片绑定的用户ID（第一条合法登录包 compare-exchange 固定，0 表示未绑定）
	std::atomic<int> _routing_uid{0};
	/// 最后一次收到心跳/数据的时间戳（原子变量，线程安全）
	std::atomic<time_t> _last_heartbeat;
	/// 会话级别的互斥锁，保护会话状态的并发访问
	std::mutex _session_mtx;
};

/**
 * @brief 逻辑处理节点
 * 
 * 封装一个待处理的客户端消息，包含会话指针和接收到的消息数据。
 * IO线程读取完消息后，将其封装为LogicNode投递到逻辑处理队列，
 * 由业务线程异步处理，实现IO与业务逻辑的线程分离。
 */
class LogicNode {
	friend class LogicSystem;
public:
	/**
	 * @brief 构造逻辑处理节点
	 * @param session 消息来源的客户端会话
	 * @param recvnode 接收到的完整消息数据节点
	 */
	LogicNode(shared_ptr<CSession>, shared_ptr<RecvNode>);
private:
	/// 消息来源的客户端会话指针
	shared_ptr<CSession> _session;
	/// 接收到的消息数据节点（包含消息类型和数据体）
	shared_ptr<RecvNode> _recvnode;
};
