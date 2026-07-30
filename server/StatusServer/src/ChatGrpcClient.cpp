#include "ChatGrpcClient.h"

ChatGrpcClient::ChatGrpcClient() {}

AddFriendRsp ChatGrpcClient::NotifyAddFriend(const AddFriendReq& req)
{
	auto to_uid = req.touid();
	std::string  uid_str = std::to_string(to_uid);
	
	AddFriendRsp rsp;
	return rsp;
}
