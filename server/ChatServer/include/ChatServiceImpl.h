#pragma once

#include <grpcpp/grpcpp.h>
#include "chat.grpc.pb.h"

class ChatServiceImpl final : public message::ChatService::Service
{
public:
	ChatServiceImpl() = default;

	grpc::Status NotifyUserMessage(
		grpc::ServerContext* context,
		const message::NotifyUserMessageReq* request,
		message::NotifyUserMessageRsp* response) override;

	grpc::Status NotifyKickUser(
		grpc::ServerContext* context,
		const message::KickUserReq* request,
		message::KickUserRsp* response) override;
};
