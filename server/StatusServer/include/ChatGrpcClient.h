#pragma once
#include "const.h"
#include "Singleton.h"
#include <grpcpp/grpcpp.h> 
#include "chat.grpc.pb.h"
#include "chat.pb.h"

using grpc::Channel;
using grpc::Status;
using grpc::ClientContext;

using message::AddFriendReq;
using message::AddFriendRsp;

using message::ChatService;

class ChatGrpcClient :public Singleton<ChatGrpcClient>
{
	friend class Singleton<ChatGrpcClient>;
public:
	~ChatGrpcClient() {

	}

	AddFriendRsp NotifyAddFriend(const AddFriendReq& req);

private:
	ChatGrpcClient();
};



