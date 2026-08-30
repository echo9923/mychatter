#pragma once
#include "Singleton.h"
#include "chat.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <mutex>
#include <unordered_map>
using grpc::Channel;

/**
 * @brief 跨服统一消息通知的最终结果
 *
 * 同时携带 gRPC transport 状态码与对端应用层 error，供 FileWorker 记录日志/决策。
 * 重试决策完全封装在 ChatServerGrpcClient 内部（每次尝试新 ClientContext+deadline，
 * 按 [Delivery] 配置最多 RpcMaxAttempts 次）。消息发布和 event_seq 已先提交，
 * 因此重试耗尽只影响实时性，客户端仍会通过统一增量同步补齐。
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
	 * @brief 通知目标 ChatServer 有一条统一用户消息可实时投递
	 *
	 * 每次尝试创建新的 ClientContext，deadline = [Delivery] RpcDeadlineMs（回退 3000）；
	 * 最多 [Delivery] RpcMaxAttempts 次（回退 3，至少 1）。只对 gRPC
	 * UNAVAILABLE/DEADLINE_EXCEEDED/RESOURCE_EXHAUSTED 或对端应用层 SERVER_BUSY(2016)
	 * 重试，间隔 RpcBackoffMs、2×RpcBackoffMs（回退 100/200ms）；接收者离线、
	 * 未知 server 或参数错误立即停止。重试耗尽不回滚已提交消息。
	 * @param message_id 已创建 user_events 行的消息 ID
	 * @param chatserver 目标 ChatServer 节点名（_hash_channels 键）
	 * @return 同时含 grpc 状态码与对端应用层 error 的 NotifyResult
	 */
	NotifyResult NotifyUserMessage(long long message_id, int event_type, int to_uid,
		std::string chatserver);
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
