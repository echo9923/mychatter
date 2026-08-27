#pragma once

#include "Singleton.h"
#include "chat.grpc.pb.h"

#include <cstdint>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

struct NotifyResult {
	grpc::StatusCode grpc_code;
	int app_error;
};

class ChatGrpcClient : public Singleton<ChatGrpcClient>
{
	friend class Singleton<ChatGrpcClient>;
public:
	~ChatGrpcClient() = default;

	NotifyResult NotifyUserMessage(const std::string& server_name, int to_uid,
		std::int64_t message_id);
	message::KickUserRsp NotifyKickUser(std::string server_name,
		const message::KickUserReq& req);

private:
	ChatGrpcClient() = default;
	std::shared_ptr<grpc::Channel> ResolveChannel(const std::string& server_name);
	struct CachedChannel {
		std::string endpoint;
		std::shared_ptr<grpc::Channel> channel;
	};
	std::unordered_map<std::string, CachedChannel> _channels;
	std::mutex _channels_mutex;
};
