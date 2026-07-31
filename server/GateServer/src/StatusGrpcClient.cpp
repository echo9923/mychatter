#include "StatusGrpcClient.h"
#include <fstream>
#include <sstream>
#include <cstdlib>

/// Read an entire file into a string. Returns empty on failure.
static std::string ReadCertFile(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f.is_open()) {
		return std::string();
	}
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

/// Fetch a required environment variable; aborts the process if missing.
static std::string RequireCertEnv(const char* name) {
	const char* val = std::getenv(name);
	if (val == nullptr || val[0] == '\0') {
		std::cerr << "FATAL: missing required environment variable: " << name << std::endl;
		std::abort();
	}
	return std::string(val);
}

GetChatServerRsp StatusGrpcClient::GetChatServer(int uid, int intent,
	const std::string& session_token_sha256)
{
	ClientContext context;
	context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
	GetChatServerRsp reply;
	GetChatServerReq request;
	request.set_uid(uid);
	request.set_intent(static_cast<message::TicketIntent>(intent));
	if (!session_token_sha256.empty()) {
		request.set_session_token_sha256(session_token_sha256);
	}
	auto stub = StatusService::NewStub(channel_);
	Status status = stub->GetChatServer(&context, request, &reply);
	if (status.ok()) {
		return reply;
	}
	else {
		reply.set_error(ErrorCodes::RPCFailed);
		return reply;
	}
}

StatusGrpcClient::StatusGrpcClient()
{
	auto& gCfgMgr = ConfigMgr::Inst();
	std::string host = gCfgMgr["StatusServer"]["Host"];
	std::string port = gCfgMgr["StatusServer"]["Port"];

	// --- Load mTLS client credentials from environment (fail-fast) ---
	std::string ca_path = RequireCertEnv("LLFC_STATUS_CA_CERT_PATH");
	std::string cert_path = RequireCertEnv("LLFC_GATE_CLIENT_CERT_PATH");
	std::string key_path = RequireCertEnv("LLFC_GATE_CLIENT_KEY_PATH");

	std::string ca_pem = ReadCertFile(ca_path);
	if (ca_pem.empty()) {
		std::cerr << "FATAL: cannot read gate CA cert: " << ca_path << std::endl;
		std::abort();
	}
	std::string cert_pem = ReadCertFile(cert_path);
	if (cert_pem.empty()) {
		std::cerr << "FATAL: cannot read gate client cert: " << cert_path << std::endl;
		std::abort();
	}
	std::string key_pem = ReadCertFile(key_path);
	if (key_pem.empty()) {
		std::cerr << "FATAL: cannot read gate client key: " << key_path << std::endl;
		std::abort();
	}

	grpc::SslCredentialsOptions ssl_opts;
	ssl_opts.pem_root_certs = ca_pem;
	ssl_opts.pem_private_key = key_pem;
	ssl_opts.pem_cert_chain = cert_pem;

	channel_ = grpc::CreateChannel(host + ":" + port,
		grpc::SslCredentials(ssl_opts));
}
