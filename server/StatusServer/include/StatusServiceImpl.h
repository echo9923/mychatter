#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <grpcpp/grpcpp.h>
#include "status.grpc.pb.h"

using grpc::ServerContext;
using grpc::Status;
using message::GetChatServerReq;
using message::GetChatServerRsp;
using message::StatusService;

class ChatServer {
public:
	std::string host;
	std::string port;
	std::string name;
};

class StatusServiceImpl final : public StatusService::Service {
public:
	Status GetChatServer(ServerContext* context, const GetChatServerReq* request,
		GetChatServerRsp* reply) override;

private:
	ChatServer getChatServer();
	std::atomic<std::size_t> _rr{0};
};
