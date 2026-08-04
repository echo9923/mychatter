#include "StatusServiceImpl.h"
#include "ConfigMgr.h"
#include "const.h"
#include "RedisMgr.h"
#include <climits>

std::string generate_unique_string() {
	// 创建UUID对象
	boost::uuids::uuid uuid = boost::uuids::random_generator()();

	// 将UUID转换为字符串
	std::string unique_string = to_string(uuid);

	return unique_string;
}

Status StatusServiceImpl::GetChatServer(ServerContext* context, const GetChatServerReq* request, GetChatServerRsp* reply)
{
	// --- Least-loaded live node selection ---
	const auto& server = getChatServer();
	if (server.host.empty()) {
		reply->set_error(ErrorCodes::NoAvailableChatServer);
		return Status::OK;
	}

	// --- Issue login token: utoken_<uid> -> token (TTL 86400s) ---
	std::string token = generate_unique_string();
	std::string token_key = USERTOKENPREFIX + std::to_string(request->uid());

	if (!RedisMgr::GetInstance()->SetEx(token_key, 86400, token)) {
		// Redis failure: do not return server address.
		reply->set_error(ErrorCodes::RPCFailed);
		return Status::OK;
	}

	reply->set_error(ErrorCodes::Success);
	reply->set_server_name(server.name);
	reply->set_host(server.host);
	reply->set_port(server.port);
	reply->set_token(token);
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
		_server_order.push_back(server.name);
	}

}

ChatServer StatusServiceImpl::getChatServer() {
	std::lock_guard<std::mutex> guard(_server_mtx);

	// 1. 按 _server_order（配置出现顺序）逐个读取 lease
	//    key = chatserver:lease:<name>，value = 已认证会话数（十进制字符串）
	//    只把存在且可解析为非负整数的节点放入候选集
	const std::string kLeasePrefix = "chatserver:lease:";
	struct Candidate { std::string name; int load; };
	std::vector<Candidate> candidates;

	for (const auto& name : _server_order) {
		std::string val;
		if (!RedisMgr::GetInstance()->Get(kLeasePrefix + name, val) || val.empty()) {
			continue;  // lease 缺失：节点未上报或已过期
		}
		// 仅接受全数字（非负整数）的值
		bool all_digit = true;
		for (char c : val) {
			if (c < '0' || c > '9') { all_digit = false; break; }
		}
		if (!all_digit) {
			continue;
		}
		int load = 0;
		for (char c : val) { load = load * 10 + (c - '0'); }
		candidates.push_back({ name, load });
	}

	if (candidates.empty()) {
		// 无任何活节点：返回空 host，由 GetChatServer 映射为 NoAvailableChatServer
		ChatServer none;
		return none;
	}

	// 2. 选最小负载
	int min_load = candidates[0].load;
	for (const auto& c : candidates) {
		if (c.load < min_load) min_load = c.load;
	}

	// 3. 负载相同的候选用原子计数轮转起点，避免配置表第一项长期占优
	std::vector<Candidate> tied;
	for (const auto& c : candidates) {
		if (c.load == min_load) tied.push_back(c);
	}
	size_t idx = _rr.fetch_add(1) % tied.size();
	const std::string chosen = tied[idx].name;

	// 4. 从静态地址簿取 host/port（lease 只携带负载，不含地址）
	ChatServer best;
	auto it = _servers.find(chosen);
	if (it != _servers.end()) {
		best = it->second;
		best.con_count = min_load;
	} else {
		best.name = chosen;  // 配置缺地址：视为不可用（host 留空）
	}
	return best;
}
