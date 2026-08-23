#pragma once
#include "Singleton.h"
#include "chat.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <mutex>
#include <unordered_map>
using grpc::Channel;

/**
 * @brief 跨服图片通知的最终结果（计划5.7）
 *
 * 同时携带 gRPC transport 状态码与对端应用层 error，供 FileWorker 记录日志/决策。
 * 重试决策完全封装在 ChatServerGrpcClient 内部（每次尝试新 ClientContext+deadline，
 * 按 [Delivery] 配置最多 RpcMaxAttempts 次），调用方只关心最终结果：
 * pending 已在 RPC 前由 ZADD 激活，重试耗尽不撤销 pending、不影响离线拉取。
 */
struct NotifyResult {
	grpc::StatusCode grpc_code;   ///< 最后一次尝试的 gRPC transport 状态码
	int app_error;                ///< 对端应用层 error（reply.error()）；无有效响应/未知 server 时为 RPCFailed
};

class ChatServerGrpcClient :public Singleton<ChatServerGrpcClient>
{
	friend class Singleton<ChatServerGrpcClient>;
public:
	~ChatServerGrpcClient() = default;
	/**
	 * @brief 通知目标 ChatServer 有图片消息可投递（带 deadline + 有界重试，计划5.7）
	 *
	 * 每次尝试创建新的 ClientContext，deadline = [Delivery] RpcDeadlineMs（回退 3000）；
	 * 最多 [Delivery] RpcMaxAttempts 次（回退 3，至少 1）。只对 gRPC
	 * UNAVAILABLE/DEADLINE_EXCEEDED/RESOURCE_EXHAUSTED 或对端应用层 SERVER_BUSY(2016)
	 * 重试，间隔 RpcBackoffMs、2×RpcBackoffMs（回退 100/200ms）；RECIPIENT_OFFLINE(2015)/
	 * 未知 server/参数类错误立即停止。重试耗尽不影响已激活的 pending（离线拉取兜底）。
	 * @param message_id 图片消息的 message_id
	 * @param chatserver 目标 ChatServer 节点名（_hash_channels 键）
	 * @return 同时含 grpc 状态码与对端应用层 error 的 NotifyResult
	 */
	NotifyResult NotifyChatResourceMsg(long long message_id, long long thread_id,
		int from_uid, int to_uid, std::string chatserver);
private:
	ChatServerGrpcClient() = default;
	//sever_ip到共享channel的映射,  <chatserver1,std::shared_ptr<Channel>>
	std::shared_ptr<Channel> ResolveChannel(const std::string& server_name);
	struct CachedChannel {
		std::string endpoint;
		std::shared_ptr<Channel> channel;
	};
	std::unordered_map<std::string, CachedChannel> _hash_channels;
	std::mutex _channels_mutex;
};
