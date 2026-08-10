#pragma once
#include "Singleton.h"
#include <grpcpp/grpcpp.h> 
#include "chat.grpc.pb.h"
#include <mutex>
#include <unordered_map>

struct UserInfo;

using grpc::Channel;

using message::AddFriendReq;
using message::AddFriendRsp;

using message::AuthFriendReq;
using message::AuthFriendRsp;

using message::TextChatMsgReq;
using message::KickUserReq;
using message::KickUserRsp;


/**
 * @brief 跨服文本通知的最终结果（计划5.6）
 *
 * 同时携带 gRPC transport 状态码与对端应用层 error，供调用方记录日志。
 * 重试决策完全封装在 ChatGrpcClient 内部（每次尝试新 ClientContext+deadline，
 * 按 [Delivery] 配置最多 RpcMaxAttempts 次），调用方只关心最终结果：
 * pending 已在 RPC 前建立，重试耗尽不撤销 sender ACK、不改 DB。
 */
struct NotifyResult {
	grpc::StatusCode grpc_code;   ///< 最后一次尝试的 gRPC transport 状态码
	int app_error;                ///< 对端应用层 error（rsp.error()）；无有效响应/未知 server 时为 RPCFailed
};


/**
 * @brief ChatServer gRPC 客户端（单例）
 * 
 * 用于向其他 ChatServer 节点发送跨服 gRPC 请求。
 * 内部维护一个以节点名为键的共享 gRPC 通道映射，调用时按需创建 Stub。
 * 支持的操作包括：通知加好友、通知认证好友、转发文本聊天消息、踢人等。
 */
class ChatGrpcClient :public Singleton<ChatGrpcClient>
{
	friend class Singleton<ChatGrpcClient>;
public:
	/// 析构函数
	~ChatGrpcClient() = default;

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
	 * @brief 向目标ChatServer转发文本聊天消息（带 deadline + 有界重试，计划5.6）
	 *
	 * 每次尝试创建新的 ClientContext，deadline = [Delivery] RpcDeadlineMs（回退 3000）；
	 * 最多 [Delivery] RpcMaxAttempts 次（回退 3，至少 1）。只对 gRPC
	 * UNAVAILABLE/DEADLINE_EXCEEDED/RESOURCE_EXHAUSTED 或对端应用层 SERVER_BUSY(1016)
	 * 重试，间隔 RpcBackoffMs、2×RpcBackoffMs（回退 100/200ms）；RECIPIENT_OFFLINE(1015)/
	 * 未知 server/参数类错误立即停止。重试耗尽不撤销 sender ACK（pending 已在 RPC 前建立）。
	 * @param server_ip 目标ChatServer的节点名（_channels 键）
	 * @param req 文本聊天消息请求（携带 unique_id/msg_id/content/chat_time）
	 * @return 同时含 grpc 状态码与对端应用层 error 的 NotifyResult
	 */
	NotifyResult NotifyTextChatMsg(const std::string& server_ip, const TextChatMsgReq& req);

	/**
	 * @brief 通知目标ChatServer踢出指定用户（用于跨服踢人/多端登录冲突）
	 * @param server_ip 目标ChatServer的IP地址
	 * @param req 踢人请求消息（包含被踢用户uid等）
	 * @return 踢人响应消息
	 */
	KickUserRsp NotifyKickUser(std::string server_ip, const KickUserReq& req);

private:
	/// 私有构造函数；目标通道在调用时从 Redis 注册表解析并缓存。
	ChatGrpcClient() = default;
	/// 共享gRPC通道映射表，键为目标节点的配置名，值为共享Channel
	std::shared_ptr<Channel> ResolveChannel(const std::string& server_name);
	struct CachedChannel {
		std::string endpoint;
		std::shared_ptr<Channel> channel;
	};
	std::unordered_map<std::string, CachedChannel> _channels;
	std::mutex _channels_mutex;
};
