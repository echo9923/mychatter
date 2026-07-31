// im_status_client.h — thin gRPC client to StatusService (plan 3.1 verification).
//
// Used by the status-discovery scenario to drive GetChatServer directly against
// the harness StatusServer (127.0.0.1:STATUS_GRPC_PORT) and inspect the
// least-loaded selection / NoAvailableChatServer error path.
//
// This commit uses an insecure channel; plan 3.2 cuts it over to mTLS.
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include "status.grpc.pb.h"

namespace imt {

class StatusClient {
public:
	// Create an insecure channel to host:port and bind the StatusService stub.
	bool Connect(const std::string& host, int port);

	// Call GetChatServer for `uid`. On RPC success fills out_* and returns true.
	// out_error mirrors the server ErrorCodes field (0 = Success,
	// 1018 = NoAvailableChatServer when host is empty). Returns false only when
	// the gRPC call itself fails (channel down / deadline).
	bool GetChatServer(int uid, int& out_error, std::string& out_host,
	                   std::string& out_port, std::string& out_token);

private:
	std::shared_ptr<grpc::Channel>                          channel_;
	std::unique_ptr<message::StatusService::Stub>           stub_;
};

} // namespace imt
