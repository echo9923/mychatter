#include "StatusServiceImpl.h"
#include "ConfigMgr.h"
#include "const.h"
#include "RedisMgr.h"
#include <climits>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

std::string generate_unique_string() {
	// 创建UUID对象
	boost::uuids::uuid uuid = boost::uuids::random_generator()();

	// 将UUID转换为字符串
	std::string unique_string = to_string(uuid);

	return unique_string;
}

Status StatusServiceImpl::GetChatServer(ServerContext* context, const GetChatServerReq* request, GetChatServerRsp* reply)
{
	const auto& server = getChatServer();
	if (server.host.empty()) {
		reply->set_error(ErrorCodes::RPCFailed);
		return Status::OK;
	}
	reply->set_host(server.host);
	reply->set_port(server.port);
	reply->set_error(ErrorCodes::Success);
	reply->set_token(generate_unique_string());
	insertToken(request->uid(), reply->token());
	return Status::OK;
}

StatusServiceImpl::StatusServiceImpl()
{
	auto& cfg = ConfigMgr::Inst();
	auto server_list = cfg["chatservers"]["Name"];

	std::vector<std::string> words;

	std::stringstream ss(server_list);
	std::string word;

	while (std::getline(ss, word, ',')) {
		words.push_back(word);
	}

	for (auto& word : words) {
		if (cfg[word]["Name"].empty()) {
			continue;
		}

		ChatServer server;
		server.port = cfg[word]["Port"];
		server.host = cfg[word]["Host"];
		server.name = cfg[word]["Name"];
		_servers[server.name] = server;
	}

}

ChatServer StatusServiceImpl::getChatServer() {
	std::lock_guard<std::mutex> guard(_server_mtx);

	// 1. 从 Redis 读取所有注册的节点元数据
	std::unordered_map<std::string, std::string> server_infos;
	RedisMgr::GetInstance()->HGetAll(CHATSERVER_INFO_KEY, server_infos);

	ChatServer best;
	best.con_count = INT_MAX;
	bool found = false;

	for (auto& [name, json_str] : server_infos) {
		// 2. 健康检查：心跳 key 是否存在
		std::string hb_key = CHATSERVER_HEARTBEAT_PREFIX + name;
		if (!RedisMgr::GetInstance()->ExistsKey(hb_key)) {
			continue;  // 心跳过期，跳过此节点
		}

		// 3. 解析节点信息
		ChatServer cs;
		try {
			auto j = json::parse(json_str);
			cs.name = j["name"].get<std::string>();
			cs.host = j["host"].get<std::string>();
			cs.port = j["port"].get<std::string>();
		} catch (...) {
			continue;
		}

		// 4. 读取连接计数
		auto count_str = RedisMgr::GetInstance()->HGet(LOGIN_COUNT, name);
		cs.con_count = count_str.empty() ? INT_MAX : std::stoi(count_str);

		// 5. 选最少连接
		if (cs.con_count < best.con_count) {
			best = cs;
			found = true;
		}
	}

	// 6. 兜底：如果 Redis 无存活节点，回退到本地静态配置
	if (!found && !_servers.empty()) {
		return _servers.begin()->second;
	}

	return best;
}

Status StatusServiceImpl::Login(ServerContext* context, const LoginReq* request, LoginRsp* reply)
{
	auto uid = request->uid();
	auto token = request->token();

	std::string uid_str = std::to_string(uid);
	std::string token_key = USERTOKENPREFIX + uid_str;
	std::string token_value = "";
	bool success = RedisMgr::GetInstance()->Get(token_key, token_value);
	if (!success) {
		reply->set_error(ErrorCodes::UidInvalid);
		return Status::OK;
	}
	
	if (token_value != token) {
		reply->set_error(ErrorCodes::TokenInvalid);
		return Status::OK;
	}
	reply->set_error(ErrorCodes::Success);
	reply->set_uid(uid);
	reply->set_token(token);
	return Status::OK;
}

void StatusServiceImpl::insertToken(int uid, std::string token)
{
	std::string uid_str = std::to_string(uid);
	std::string token_key = USERTOKENPREFIX + uid_str;
	RedisMgr::GetInstance()->Set(token_key, token);
}

