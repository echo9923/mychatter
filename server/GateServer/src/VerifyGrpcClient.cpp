#include "VerifyGrpcClient.h"
#include "const.h"

VerifyGrpcClient::VerifyGrpcClient() {
	auto& gCfgMgr = ConfigMgr::Inst();
	std::string host = gCfgMgr["VarifyServer"]["Host"];
	std::string port = gCfgMgr["VarifyServer"]["Port"];
	channel_ = grpc::CreateChannel(host + ":" + port, grpc::InsecureChannelCredentials());
}