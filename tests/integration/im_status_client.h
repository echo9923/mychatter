// im_status_client.h — plain gRPC client to StatusService.
//
// Drives GetChatServer directly against the harness StatusServer
// (127.0.0.1:STATUS_GRPC_PORT) to inspect least-loaded selection and the
// token/error surface. Gate->Status is plain gRPC (InsecureChannelCredentials),
// so Connect() builds an insecure channel.
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include "status.grpc.pb.h"

namespace imt {

class StatusClient {
public:
	// Plaintext gRPC channel to host:port; binds the StatusService stub.
	bool Connect(const std::string& host, int port);

	// Call GetChatServer(uid). On RPC success fills out_* and returns true.
	// out_error mirrors the server ErrorCodes field (0 = Success,
	// 1018 = NoAvailableChatServer when host is empty). out_token is the
	// per-user login token Status writes to Redis utoken_<uid> (TTL 86400).
	// Returns false only when the gRPC call itself fails (channel down /
	// deadline).
	bool GetChatServer(int uid, int& out_error, std::string& out_server_name,
	                   std::string& out_host, std::string& out_port,
	                   std::string& out_token);

private:
	std::shared_ptr<grpc::Channel>                          channel_;
	std::unique_ptr<message::StatusService::Stub>           stub_;
};

} // namespace imt
