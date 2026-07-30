#pragma once
#include "const.h"
#include "Singleton.h"
#include "ConfigMgr.h"
#include <grpcpp/grpcpp.h> 
#include "chat.grpc.pb.h"
#include "chat.pb.h"
#include <queue>
#include "const.h"
#include "data.h"
#include <nlohmann/json.hpp>
using json = nlohmann::json;

using grpc::Channel;
using grpc::Status;
using grpc::ClientContext;

using message::AddFriendReq;
using message::AddFriendRsp;

using message::AuthFriendReq;
using message::AuthFriendRsp;

using message::ChatService;

using message::TextChatMsgReq;
using message::TextChatMsgRsp;
using message::TextChatData;

using message::KickUserReq;
using message::KickUserRsp;


/**
 * @brief ChatServer gRPC 连接池
 * 
 * 管理与目标 ChatServer 节点的 gRPC 连接，采用对象池模式复用 Stub 连接。
 * 当需要向其他 ChatServer 发送跨服通知（如加好友、踢人、消息转发）时，
 * 从池中获取连接，使用完毕后归还。
 */
class ChatConPool {
public:
	/**
	 * @brief 构造连接池，预创建指定数量的gRPC Stub连接
	 * @param poolSize 连接池大小（预创建的连接数）
	 * @param host 目标ChatServer的IP地址
	 * @param port 目标ChatServer的gRPC端口号
	 */
	ChatConPool(size_t poolSize, std::string host, std::string port)
		: poolSize_(poolSize), host_(host), port_(port), b_stop_(false) {
		for (size_t i = 0; i < poolSize_; ++i) {

			std::shared_ptr<Channel> channel = grpc::CreateChannel(host + ":" + port,
				grpc::InsecureChannelCredentials());

			connections_.push(ChatService::NewStub(channel));
		}
	}

	/// 析构函数，关闭连接池并清空所有连接
	~ChatConPool() {
		std::lock_guard<std::mutex> lock(mutex_);
		Close();
		while (!connections_.empty()) {
			connections_.pop();
		}
	}

	/**
	 * @brief 从连接池中获取一个可用的gRPC Stub连接（阻塞等待）
	 * @return ChatService::Stub 智能指针，若池已停止则返回nullptr
	 */
	std::unique_ptr<ChatService::Stub> getConnection() {
		std::unique_lock<std::mutex> lock(mutex_);
		cond_.wait(lock, [this] {
			if (b_stop_) {
				return true;
			}
			return !connections_.empty();
			});
		//如果停止则直接返回空指针
		if (b_stop_) {
			return  nullptr;
		}
		auto context = std::move(connections_.front());
		connections_.pop();
		return context;
	}

	/**
	 * @brief 将使用完毕的gRPC Stub连接归还到连接池
	 * @param context 要归还的Stub连接智能指针
	 */
	void returnConnection(std::unique_ptr<ChatService::Stub> context) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (b_stop_) {
			return;
		}
		connections_.push(std::move(context));
		cond_.notify_one();
	}

	/// 关闭连接池，唤醒所有等待线程使其退出
	void Close() {
		b_stop_ = true;
		cond_.notify_all();
	}

private:
	/// 停止标志，为true时连接池不再提供连接
	atomic<bool> b_stop_;
	/// 连接池容量（预创建的连接总数）
	size_t poolSize_;
	/// 目标ChatServer的主机地址
	std::string host_;
	/// 目标ChatServer的gRPC端口
	std::string port_;
	/// gRPC Stub连接队列，存储所有可复用的ChatService::Stub
	std::queue<std::unique_ptr<ChatService::Stub> > connections_;
	/// 互斥锁，保护连接队列的线程安全访问
	std::mutex mutex_;
	/// 条件变量，当无可用连接时阻塞等待，有连接归还时唤醒
	std::condition_variable cond_;
};

/**
 * @brief ChatServer gRPC 客户端（单例）
 * 
 * 用于向其他 ChatServer 节点发送跨服 gRPC 请求。
 * 内部维护一个以 "ip:port" 为键的连接池映射，按需创建并复用连接池。
 * 支持的操作包括：通知加好友、通知认证好友、转发文本聊天消息、踢人等。
 */
class ChatGrpcClient :public Singleton<ChatGrpcClient>
{
	friend class Singleton<ChatGrpcClient>;
public:
	/// 析构函数
	~ChatGrpcClient() {

	}

	/**
	 * @brief 通知目标ChatServer有用户收到了好友申请
	 * @param server_ip 目标ChatServer的IP地址
	 * @param req 添加好友请求消息（包含申请者、被申请者信息等）
	 * @return 添加好友响应消息
	 */
	AddFriendRsp NotifyAddFriend(std::string server_ip, const AddFriendReq& req);

	/**
	 * @brief 通知目标ChatServer有用户的好友申请已被认证通过
	 * @param server_ip 目标ChatServer的IP地址
	 * @param req 认证好友请求消息
	 * @return 认证好友响应消息
	 */
	AuthFriendRsp NotifyAuthFriend(std::string server_ip, const AuthFriendReq& req);

	/**
	 * @brief 从 Redis 中获取用户基本信息
	 * @param base_key Redis中用户信息的键前缀
	 * @param uid 用户ID
	 * @param userinfo [out] 输出参数，存储查询到的用户信息
	 * @return 是否成功获取到用户信息
	 */
	bool GetBaseInfo(std::string base_key, int uid, std::shared_ptr<UserInfo>& userinfo);

	/**
	 * @brief 向目标ChatServer转发文本聊天消息
	 * @param server_ip 目标ChatServer的IP地址
	 * @param req 文本聊天消息请求
	 * @param rtvalue 原始JSON数据，用于日志或额外处理
	 * @return 文本聊天消息响应
	 */
	TextChatMsgRsp NotifyTextChatMsg(std::string server_ip, const TextChatMsgReq& req, const json& rtvalue);

	/**
	 * @brief 通知目标ChatServer踢出指定用户（用于跨服踢人/多端登录冲突）
	 * @param server_ip 目标ChatServer的IP地址
	 * @param req 踢人请求消息（包含被踢用户uid等）
	 * @return 踢人响应消息
	 */
	KickUserRsp NotifyKickUser(std::string server_ip, const KickUserReq& req);

private:
	/// 私有构造函数，从配置文件读取连接池参数
	ChatGrpcClient();
	/// 连接池映射表，键为目标服务器的 "ip:port"，值为对应的连接池实例
	unordered_map<std::string, std::unique_ptr<ChatConPool>> _pools;	
};



