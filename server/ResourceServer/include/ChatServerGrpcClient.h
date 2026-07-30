#pragma once
#include "const.h"
#include "Singleton.h"
#include "ConfigMgr.h"
#include "chat.grpc.pb.h"
#include "chat.pb.h"
#include <grpcpp/grpcpp.h>
using grpc::Channel;
using grpc::Status;
using grpc::ClientContext;


using message::ChatService;
using message::NotifyChatImgReq;
using message::NotifyChatImgRsp;

class ChatServerGrpcClient :public Singleton<ChatServerGrpcClient>
{
	friend class Singleton<ChatServerGrpcClient>;
public:
	~ChatServerGrpcClient() {

	}
	NotifyChatImgRsp NotifyChatImgMsg(int message_id, std::string chatserver);
private:
	ChatServerGrpcClient();
	//sever_ip到共享channel的映射,  <chatserver1,std::shared_ptr<Channel>>
	std::unordered_map<std::string, std::shared_ptr<Channel>> _hash_channels;
};
