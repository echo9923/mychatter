// im_status_client.cpp — mTLS gRPC StatusService client implementation.
#include "im_status_client.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#ifndef IM_REPO_ROOT
#define IM_REPO_ROOT "."
#endif

namespace imt {

// Read an entire file into a string (used to load PEM blobs for SslCredentials).
// Returns empty string on failure; callers treat empty as "could not load".
static std::string ReadFile(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f.is_open()) {
		std::printf("[status-client] cannot open cert file: %s\n", path.c_str());
		return std::string();
	}
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

// Absolute path to a test cert under tests/integration/certs.
static std::string CertPath(const char* name) {
	return std::string(IM_REPO_ROOT) + "/tests/integration/certs/" + name;
}

bool StatusClient::Connect(const std::string& host, int port) {
	const std::string ca  = ReadFile(CertPath("ca.crt"));
	const std::string crt = ReadFile(CertPath("gate.crt"));
	const std::string key = ReadFile(CertPath("gate.key"));
	if (ca.empty() || crt.empty() || key.empty()) return false;

	grpc::SslCredentialsOptions opts;
	opts.pem_root_certs  = ca;
	opts.pem_cert_chain  = crt;
	opts.pem_private_key = key;
	auto creds = grpc::SslCredentials(opts);

	channel_ = grpc::CreateChannel(host + ":" + std::to_string(port), creds);
	if (!channel_) return false;
	stub_ = message::StatusService::NewStub(channel_);
	return stub_ != nullptr;
}

bool StatusClient::ConnectInsecure(const std::string& host, int port) {
	channel_ = grpc::CreateChannel(host + ":" + std::to_string(port),
		grpc::InsecureChannelCredentials());
	if (!channel_) return false;
	stub_ = message::StatusService::NewStub(channel_);
	return stub_ != nullptr;
}

bool StatusClient::GetChatServer(int uid, int intent,
                                 const std::string& session_token_sha256,
                                 int& out_error, std::string& out_server_name,
                                 std::string& out_host, std::string& out_port,
                                 std::string& out_chat_ticket) {
	if (!stub_) return false;
	grpc::ClientContext context;
	context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));

	message::GetChatServerReq req;
	req.set_uid(uid);
	req.set_intent(static_cast<message::TicketIntent>(intent));
	req.set_session_token_sha256(session_token_sha256);
	message::GetChatServerRsp rsp;

	grpc::Status st = stub_->GetChatServer(&context, req, &rsp);
	if (!st.ok()) {
		// Handshake rejection (insecure client vs mTLS server), channel down or
		// deadline. Do not print credentials; only the gRPC status code/message.
		std::printf("[status-client] GetChatServer rpc failed: err_code=%d msg=%s\n",
			static_cast<int>(st.error_code()), st.error_message().c_str());
		return false;
	}
	out_error        = rsp.error();
	out_server_name  = rsp.server_name();
	out_host         = rsp.host();
	out_port         = rsp.port();
	out_chat_ticket  = rsp.chat_ticket();
	return true;
}

} // namespace imt
