// im_status_client.cpp — plain gRPC StatusService client implementation.
#include "im_status_client.h"

#include <chrono>
#include <cstdio>

namespace imt {

bool StatusClient::Connect(const std::string& host, int port) {
	channel_ = grpc::CreateChannel(host + ":" + std::to_string(port),
		grpc::InsecureChannelCredentials());
	if (!channel_) return false;
	stub_ = message::StatusService::NewStub(channel_);
	return stub_ != nullptr;
}

bool StatusClient::GetChatServer(int uid, int& out_error, std::string& out_server_name,
                                 std::string& out_host, std::string& out_port,
                                 std::string& out_token) {
	if (!stub_) return false;
	grpc::ClientContext context;
	context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));

	message::GetChatServerReq req;
	req.set_uid(uid);
	message::GetChatServerRsp rsp;

	grpc::Status st = stub_->GetChatServer(&context, req, &rsp);
	if (!st.ok()) {
		// Channel down or deadline. Do not print credentials; only the gRPC
		// status code/message.
		std::printf("[status-client] GetChatServer rpc failed: err_code=%d msg=%s\n",
			static_cast<int>(st.error_code()), st.error_message().c_str());
		return false;
	}
	out_error        = rsp.error();
	out_server_name  = rsp.server_name();
	out_host         = rsp.host();
	out_port         = rsp.port();
	out_token        = rsp.token();
	return true;
}

} // namespace imt
