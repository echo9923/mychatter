// im_status_client.h — mTLS gRPC client to StatusService (plan 3.1 / 3.2).
//
// Drives GetChatServer directly against the harness StatusServer
// (127.0.0.1:STATUS_GRPC_PORT) to inspect least-loaded selection and the
// ticket/error surface. The StatusServer now requires mutual TLS, so Connect()
// builds an SslCredentials channel from the test CA + gate client cert/key
// (IM_REPO_ROOT/tests/integration/certs). ConnectInsecure() is provided solely
// for the auth-ticket negative case: an mTLS server rejects the plaintext
// handshake, so the RPC itself fails.
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include "status.grpc.pb.h"

namespace imt {

class StatusClient {
public:
	// mTLS channel to host:port. Loads the test CA + gate client cert/key from
	// IM_REPO_ROOT/tests/integration/certs and binds the StatusService stub.
	// Returns false if any cert file cannot be read.
	bool Connect(const std::string& host, int port);

	// Plaintext channel (negative test only): an mTLS-only server rejects the
	// handshake, so the first RPC fails. Provided to prove the server refuses
	// non-TLS clients.
	bool ConnectInsecure(const std::string& host, int port);

	// Call GetChatServer(uid, intent, session_token_sha256). On RPC success fills
	// out_* and returns true. out_error mirrors the server ErrorCodes field
	// (0 = Success, 1018 = NoAvailableChatServer when host is empty). Returns
	// false only when the gRPC call itself fails (channel down / handshake
	// rejected / deadline) — the auth-ticket scenario uses that to prove mTLS
	// enforcement.
	bool GetChatServer(int uid, int intent, const std::string& session_token_sha256,
	                   int& out_error, std::string& out_server_name,
	                   std::string& out_host, std::string& out_port,
	                   std::string& out_chat_ticket);

private:
	std::shared_ptr<grpc::Channel>                          channel_;
	std::unique_ptr<message::StatusService::Stub>           stub_;
};

} // namespace imt
