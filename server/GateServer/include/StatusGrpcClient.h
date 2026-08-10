#pragma once
#include "const.h"
#include "Singleton.h"
#include "ConfigMgr.h"
#include <grpcpp/grpcpp.h> 
#include "status.grpc.pb.h"
#include "status.pb.h"

using grpc::Channel;
using grpc::Status;
using grpc::ClientContext;

using message::GetChatServerReq;
using message::GetChatServerRsp;
using message::StatusService;

class StatusGrpcClient :public Singleton<StatusGrpcClient>
{
	friend class Singleton<StatusGrpcClient>;
public:
	~StatusGrpcClient() {

	}
	/// Request a chat server assignment plus a login token (plain gRPC).
	/// @param uid  user id
	GetChatServerRsp GetChatServer(int uid, const std::string& token = {});
private:
	StatusGrpcClient();
	std::shared_ptr<Channel> channel_;
	
};
