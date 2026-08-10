#include "StatusServiceImpl.h"

#include "ChatServerRegistry.h"
#include "RedisMgr.h"
#include "const.h"

#include <algorithm>
#include <charconv>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace {

bool ParseNonNegativeInt(const std::string& value, int& parsed) {
	if (value.empty()) return false;
	int result = 0;
	const char* begin = value.data();
	const char* end = begin + value.size();
	const auto conversion = std::from_chars(begin, end, result);
	if (conversion.ec != std::errc{} || conversion.ptr != end || result < 0) {
		return false;
	}
	parsed = result;
	return true;
}

bool IsValidPort(const std::string& value) {
	int port = 0;
	return ParseNonNegativeInt(value, port) && port > 0 && port <= 65535;
}

bool ParseRegistration(const std::string& field, const std::string& value,
	ChatServer& server) {
	try {
		const auto data = json::parse(value);
		if (!data.is_object()) return false;
		for (const char* key : { "name", "tcp_host", "tcp_port", "rpc_host", "rpc_port" }) {
			if (!data.contains(key) || !data[key].is_string()) return false;
		}

		const std::string name = data["name"].get<std::string>();
		const std::string tcp_host = data["tcp_host"].get<std::string>();
		const std::string tcp_port = data["tcp_port"].get<std::string>();
		const std::string rpc_host = data["rpc_host"].get<std::string>();
		const std::string rpc_port = data["rpc_port"].get<std::string>();
		if (name != field || tcp_host.empty() || rpc_host.empty() ||
			!IsValidPort(tcp_port) || !IsValidPort(rpc_port)) {
			return false;
		}

		server.name = name;
		server.host = tcp_host;
		server.port = tcp_port;
		return true;
	} catch (const json::exception&) {
		return false;
	}
}

std::string GenerateUniqueString() {
	const boost::uuids::uuid uuid = boost::uuids::random_generator()();
	return to_string(uuid);
}

} // namespace

Status StatusServiceImpl::GetChatServer(ServerContext*,
	const GetChatServerReq* request, GetChatServerRsp* reply) {
	const bool reassignment = !request->token().empty();
	if (reassignment) {
		std::string stored_token;
		const std::string token_key = USERTOKENPREFIX + std::to_string(request->uid());
		if (!RedisMgr::GetInstance()->Get(token_key, stored_token)) {
			reply->set_error(ErrorCodes::UidInvalid);
			return Status::OK;
		}
		if (stored_token != request->token()) {
			reply->set_error(ErrorCodes::TokenInvalid);
			return Status::OK;
		}
	}

	const auto server = getChatServer();
	if (server.host.empty()) {
		reply->set_error(ErrorCodes::NoAvailableChatServer);
		return Status::OK;
	}

	std::string token = request->token();
	if (!reassignment) {
		token = GenerateUniqueString();
		const std::string token_key = USERTOKENPREFIX + std::to_string(request->uid());
		if (!RedisMgr::GetInstance()->SetEx(token_key, 86400, token)) {
			reply->set_error(ErrorCodes::RPCFailed);
			return Status::OK;
		}
	}

	reply->set_error(ErrorCodes::Success);
	reply->set_server_name(server.name);
	reply->set_host(server.host);
	reply->set_port(server.port);
	reply->set_token(token);
	return Status::OK;
}

ChatServer StatusServiceImpl::getChatServer() {
	std::unordered_map<std::string, std::string> registrations;
	if (!RedisMgr::GetInstance()->HGetAll(llfc::kChatServerRegistryKey, registrations)) {
		return {};
	}

	struct Candidate {
		ChatServer server;
		int load;
	};
	std::vector<Candidate> candidates;
	for (const auto& entry : registrations) {
		ChatServer server;
		if (!ParseRegistration(entry.first, entry.second, server)) continue;

		std::string lease_value;
		int load = 0;
		if (!RedisMgr::GetInstance()->Get(llfc::ChatServerLeaseKey(entry.first),
			lease_value) || !ParseNonNegativeInt(lease_value, load)) {
			continue;
		}
		candidates.push_back({ server, load });
	}

	if (candidates.empty()) return {};
	std::sort(candidates.begin(), candidates.end(),
		[](const Candidate& lhs, const Candidate& rhs) {
			return lhs.server.name < rhs.server.name;
		});

	const int min_load = std::min_element(candidates.begin(), candidates.end(),
		[](const Candidate& lhs, const Candidate& rhs) {
			return lhs.load < rhs.load;
		})->load;

	std::vector<ChatServer> tied;
	for (const auto& candidate : candidates) {
		if (candidate.load == min_load) tied.push_back(candidate.server);
	}
	const std::size_t index = _rr.fetch_add(1) % tied.size();
	return tied[index];
}
