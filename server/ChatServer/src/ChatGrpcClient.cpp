#include "ChatGrpcClient.h"

#include "ChatServerRegistry.h"
#include "ConfigMgr.h"
#include "RedisMgr.h"
#include "const.h"

#include <charconv>
#include <chrono>
#include <nlohmann/json.hpp>
#include <thread>

namespace {

int ReadDeliveryInt(const std::string& key, int fallback) {
	try {
		const auto value = ConfigMgr::Inst().GetValue("Delivery", key);
		if (!value.empty()) {
			std::size_t consumed = 0;
			const int parsed = std::stoi(value, &consumed);
			if (consumed == value.size() && parsed > 0) return parsed;
		}
	} catch (...) {
	}
	return fallback;
}

bool ResolveRpcEndpoint(const std::string& server_name, std::string& endpoint) {
	std::string lease;
	if (!RedisMgr::GetInstance()->Get(llfc::ChatServerLeaseKey(server_name), lease) ||
		lease.empty()) {
		return false;
	}
	int load = 0;
	const auto load_result = std::from_chars(lease.data(), lease.data() + lease.size(), load);
	if (load_result.ec != std::errc{} || load_result.ptr != lease.data() + lease.size() ||
		load < 0) {
		return false;
	}

	const auto metadata = nlohmann::json::parse(
		RedisMgr::GetInstance()->HGet(llfc::kChatServerRegistryKey, server_name),
		nullptr, false);
	if (!metadata.is_object()) return false;
	for (const char* key : { "name", "rpc_host", "rpc_port" }) {
		if (!metadata.contains(key) || !metadata[key].is_string()) return false;
	}
	const std::string name = metadata["name"].get<std::string>();
	const std::string host = metadata["rpc_host"].get<std::string>();
	const std::string port_text = metadata["rpc_port"].get<std::string>();
	int port = 0;
	const auto port_result = std::from_chars(
		port_text.data(), port_text.data() + port_text.size(), port);
	if (name != server_name || host.empty() || port_result.ec != std::errc{} ||
		port_result.ptr != port_text.data() + port_text.size() || port <= 0 || port > 65535) {
		return false;
	}
	endpoint = host + ":" + port_text;
	return true;
}

} // namespace

std::shared_ptr<grpc::Channel> ChatGrpcClient::ResolveChannel(
	const std::string& server_name) {
	std::string endpoint;
	if (!ResolveRpcEndpoint(server_name, endpoint)) return nullptr;

	std::lock_guard<std::mutex> lock(_channels_mutex);
	auto found = _channels.find(server_name);
	if (found != _channels.end() && found->second.endpoint == endpoint) {
		return found->second.channel;
	}
	auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
	_channels[server_name] = { endpoint, channel };
	return channel;
}

NotifyResult ChatGrpcClient::NotifyUserEvent(const std::string& server_name,
	int to_uid, int event_type, std::int64_t message_id,
	std::int64_t friend_request_id) {
	NotifyResult result{ grpc::StatusCode::OK, ErrorCodes::Success };
	const int deadline_ms = ReadDeliveryInt("RpcDeadlineMs", 3000);
	const int max_attempts = ReadDeliveryInt("RpcMaxAttempts", 3);
	const int backoff_ms = ReadDeliveryInt("RpcBackoffMs", 100);
	auto channel = ResolveChannel(server_name);
	if (!channel) {
		return { grpc::StatusCode::NOT_FOUND, ErrorCodes::RPCFailed };
	}

	for (int attempt = 1; attempt <= max_attempts; ++attempt) {
		auto stub = message::ChatService::NewStub(channel);
		grpc::ClientContext context;
		context.set_deadline(std::chrono::system_clock::now() +
			std::chrono::milliseconds(deadline_ms));
		message::NotifyUserMessageReq request;
		request.set_to_uid(to_uid);
		request.set_message_id(message_id);
		request.set_event_type(event_type);
		request.set_friend_request_id(friend_request_id);
		message::NotifyUserMessageRsp response;
		const grpc::Status status = stub->NotifyUserMessage(&context, request, &response);
		if (status.ok()) {
			result = { grpc::StatusCode::OK, response.error() };
			if (response.error() != ErrorCodes::SERVER_BUSY || attempt == max_attempts) {
				return result;
			}
		} else {
			result = { status.error_code(), ErrorCodes::RPCFailed };
			const bool retryable = status.error_code() == grpc::StatusCode::UNAVAILABLE ||
				status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ||
				status.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED;
			if (!retryable || attempt == max_attempts) return result;
		}
		const int shift = attempt > 11 ? 10 : attempt - 1;
		std::this_thread::sleep_for(
			std::chrono::milliseconds(backoff_ms * (1 << shift)));
	}
	return result;
}

message::KickUserRsp ChatGrpcClient::NotifyKickUser(std::string server_name,
	const message::KickUserReq& request) {
	message::KickUserRsp response;
	response.set_error(ErrorCodes::RPCFailed);
	response.set_uid(request.uid());
	auto channel = ResolveChannel(server_name);
	if (!channel) return response;

	auto stub = message::ChatService::NewStub(channel);
	grpc::ClientContext context;
	context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
	if (!stub->NotifyKickUser(&context, request, &response).ok()) {
		response.set_error(ErrorCodes::RPCFailed);
	}
	return response;
}
