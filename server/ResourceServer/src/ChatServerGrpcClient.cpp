#include "ChatServerGrpcClient.h"
#include "ChatServerRegistry.h"
#include "ConfigMgr.h"
#include "const.h"
#include "MysqlMgr.h"
#include "RedisMgr.h"
#include <charconv>
#include <iostream>
#include <chrono>
#include <nlohmann/json.hpp>
#include <thread>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

using grpc::ClientContext;
using grpc::Status;
using message::ChatService;
using message::NotifyResourceReq;
using message::NotifyResourceRsp;

namespace {
/// 从 [Delivery] 读取整数配置；非法/缺失时回退 fallback（与 ChatServer 同一模式，计划4.2/5.7）
int ReadDeliveryInt(const std::string& key, int fallback) {
	try {
		auto val = ConfigMgr::Inst().GetValue("Delivery", key);
		if (!val.empty()) {
			std::size_t pos = 0;
			int n = std::stoi(val, &pos);
			//必须整串消费（允许前导空白），否则视为非法值回退 fallback
			if (pos == val.size() && n > 0) {
				return n;
			}
		}
	}
	catch (...) {
		//配置缺失/非数字，回退默认值
	}
	return fallback;
}

/// 对端 ChatServer 应用层错误码（ResourceServer const.h 未定义这些语义，用字面量常量，计划5.7）
constexpr int kAppRecipientOffline = 1015;  // RECIPIENT_OFFLINE：只记录 pending，不重试
constexpr int kAppServerBusy = 1016;        // SERVER_BUSY：可重试

bool ResolveRpcEndpoint(const std::string& server_name, std::string& endpoint) {
	std::string lease;
	if (!RedisMgr::GetInstance()->Get(llfc::ChatServerLeaseKey(server_name), lease) ||
		lease.empty()) {
		return false;
	}
	int load = 0;
	const auto load_result = std::from_chars(
		lease.data(), lease.data() + lease.size(), load);
	if (load_result.ec != std::errc{} ||
		load_result.ptr != lease.data() + lease.size() || load < 0) {
		return false;
	}

	const std::string metadata = RedisMgr::GetInstance()->HGet(
		llfc::kChatServerRegistryKey, server_name);
	const auto data = nlohmann::json::parse(metadata, nullptr, false);
	if (!data.is_object()) return false;
	for (const char* key : { "name", "rpc_host", "rpc_port" }) {
		if (!data.contains(key) || !data[key].is_string()) return false;
	}
	const std::string name = data["name"].get<std::string>();
	const std::string host = data["rpc_host"].get<std::string>();
	const std::string port_text = data["rpc_port"].get<std::string>();
	int port = 0;
	const auto port_result = std::from_chars(
		port_text.data(), port_text.data() + port_text.size(), port);
	if (name != server_name || host.empty() ||
		port_result.ec != std::errc{} ||
		port_result.ptr != port_text.data() + port_text.size() ||
		port <= 0 || port > 65535) {
		return false;
	}
	endpoint = host + ":" + port_text;
	return true;
}
} // namespace

std::shared_ptr<Channel> ChatServerGrpcClient::ResolveChannel(
	const std::string& server_name) {
	std::string endpoint;
	if (!ResolveRpcEndpoint(server_name, endpoint)) return nullptr;

	std::lock_guard<std::mutex> lock(_channels_mutex);
	auto found = _hash_channels.find(server_name);
	if (found != _hash_channels.end() && found->second.endpoint == endpoint) {
		return found->second.channel;
	}
	auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
	_hash_channels[server_name] = { endpoint, channel };
	return channel;
}

NotifyResult ChatServerGrpcClient::NotifyChatResourceMsg(long long message_id,
	long long thread_id, int from_uid, int to_uid, std::string chatserver)
{
	NotifyResult result{ grpc::StatusCode::OK, ErrorCodes::Success };

	//配置：每次尝试 deadline、最多尝试次数、退避基数
	int deadline_ms = ReadDeliveryInt("RpcDeadlineMs", 3000);
	if (deadline_ms < 1) deadline_ms = 3000;
	int max_attempts = ReadDeliveryInt("RpcMaxAttempts", 3);
	if (max_attempts < 1) max_attempts = 1;
	int backoff_ms = ReadDeliveryInt("RpcBackoffMs", 100);
	if (backoff_ms < 1) backoff_ms = 100;

	auto channel = ResolveChannel(chatserver);
	if (!channel) {
		//未知 server：配置/路由缺失，参数类错误，立即停止不重试
		result.grpc_code = grpc::StatusCode::NOT_FOUND;
		result.app_error = ErrorCodes::RPCFailed;
		return result;
	}

	//只传定位字段：ChatServer 按 message_id 回读 DB 组统一 envelope，
	//避免展示字段在 gRPC 层与 DB 真值分叉
	NotifyResourceReq request;
	request.set_message_id(message_id);
	request.set_thread_id(thread_id);
	request.set_from_uid(from_uid);
	request.set_to_uid(to_uid);

	for (int attempt = 1; attempt <= max_attempts; ++attempt) {
		//每次尝试新的 stub + ClientContext，deadline 固定 RpcDeadlineMs
		auto stub = ChatService::NewStub(channel);
		ClientContext context;
		context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(deadline_ms));

		NotifyResourceRsp reply;
		Status status = stub->NotifyChatResourceMsg(&context, request, &reply);

		if (status.ok()) {
			result.grpc_code = grpc::StatusCode::OK;
			result.app_error = reply.error();
			//仅对端 SERVER_BUSY(1016) 重试；Success/RECIPIENT_OFFLINE(1015)/未知应用错误立即停止
			if (reply.error() == kAppServerBusy && attempt < max_attempts) {
				//退避：RpcBackoffMs、2×RpcBackoffMs（100/200ms）；位移限幅防溢出
				int shift = attempt - 1;
				if (shift > 10) shift = 10;
				std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms * (1 << shift)));
				continue;
			}
			return result;
		}

		//transport 失败：记录后判断是否重试
		result.grpc_code = status.error_code();
		result.app_error = ErrorCodes::RPCFailed;
		bool retryable = (status.error_code() == grpc::StatusCode::UNAVAILABLE ||
		                  status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ||
		                  status.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED);
		if (retryable && attempt < max_attempts) {
			//退避：RpcBackoffMs、2×RpcBackoffMs（100/200ms）；位移限幅防溢出
			int shift = attempt - 1;
			if (shift > 10) shift = 10;
			std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms * (1 << shift)));
			continue;
		}
		//不可重试或已耗尽：停止
		break;
	}

	return result;
}
