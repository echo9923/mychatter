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
	/// Request a chat server assignment with an mTLS-protected one-time ticket.
	/// @param uid                   user id
	/// @param intent                0=INITIAL (after password), 1=RESUME (valid session token)
	/// @param session_token_sha256  SHA-256 hex of the session token (RESUME only)
	GetChatServerRsp GetChatServer(int uid, int intent,
		const std::string& session_token_sha256);
private:
	StatusGrpcClient();
	std::shared_ptr<Channel> channel_;
	
};
