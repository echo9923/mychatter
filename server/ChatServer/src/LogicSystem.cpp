#include "LogicSystem.h"
#include "MysqlMgr.h"
#include "const.h"
#include "RedisMgr.h"
#include "UserMgr.h"
#include "ChatGrpcClient.h"
#include "DistLock.h"
#include <string>
#include "ConfigMgr.h"
#include "utils.h"
#include "Sha256.h"
#include <vector>
#include <set>
#include <algorithm>
#include <cctype>
#include <limits>

using namespace std;

namespace {
/// 从 [Concurrency] 读取 worker 数量；缺失/非数字/0/>64 一律回退 4（计划1.2）
std::size_t ReadWorkerCount(const std::string& key) {
	const std::size_t fallback = 4;
	try {
		auto val = ConfigMgr::Inst().GetValue("Concurrency", key);
		if (!val.empty()) {
			std::size_t pos = 0;
			int n = std::stoi(val, &pos);
			//必须整串消费（允许前导空白），否则视为非法值回退 4（计划1.2）
			if (pos == val.size() && n > 0 && n <= 64) {
				return static_cast<std::size_t>(n);
			}
		}
	}
	catch (...) {
		//配置缺失/非数字，回退默认值
	}
	return fallback;
}

/// 从 JSON 值解析 64 位 id：破坏性协议只接受非负十进制字符串。
bool ParseJsonId(const json& v, std::int64_t& out) {
	if (v.is_string()) {
		try {
			auto s = v.get<std::string>();
			if (s.empty() || !std::all_of(s.begin(), s.end(),
				[](unsigned char c) { return std::isdigit(c) != 0; })) {
				return false;
			}
			std::size_t pos = 0;
			unsigned long long n = std::stoull(s, &pos);
			if (pos == s.size()) {
				if (n > static_cast<unsigned long long>(
					std::numeric_limits<std::int64_t>::max())) return false;
				out = static_cast<std::int64_t>(n);
				return true;
			}
		}
		catch (...) {
			//非法字符串，按解析失败处理
		}
	}
	return false;
}

/// Parse a JSON integer without allowing narrowing conversion to throw.
bool ParseJsonInt(const json& v, int& out) {
	try {
		if (v.is_number_unsigned()) {
			const auto value = v.get<std::uint64_t>();
			if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
				return false;
			}
			out = static_cast<int>(value);
			return true;
		}
		if (!v.is_number_integer()) {
			return false;
		}
		const auto value = v.get<std::int64_t>();
		if (value < std::numeric_limits<int>::min() ||
			value > std::numeric_limits<int>::max()) {
			return false;
		}
		out = static_cast<int>(value);
		return true;
	}
	catch (...) {
		return false;
	}
}

bool ParseJsonUid(const json& v, int& out) {
	return ParseJsonInt(v, out) && out > 0;
}

/// 资源文件名清洗：剥除路径分隔符/控制字符，压缩空白；结果为 UTF-8 且不超过 255 字节
///（在多字节字符边界上截断）。清洗后为空返回 false。
bool SanitizeFileName(const std::string& in, std::string& out) {
	out.clear();
	out.reserve(in.size());
	for (const unsigned char c : in) {
		if (c == '/' || c == '\\' || c < 0x20 || c == 0x7F) {
			continue; // 路径分隔符与控制字符：根除路径穿越
		}
		out.push_back(static_cast<char>(c));
	}
	//UTF-8 边界截断到 255 字节（后继字节 0b10xxxxxx 不可作首字节）
	if (out.size() > 255) {
		std::size_t cut = 255;
		while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
			--cut;
		}
		out.resize(cut);
	}
	return !out.empty();
}

/// MIME 类型格式校验：type/subtype，各段非空、仅 [A-Za-z0-9.+-]、总长 <=128
bool IsValidMimeType(const std::string& mime) {
	if (mime.empty() || mime.size() > 128) {
		return false;
	}
	const std::size_t slash = mime.find('/');
	if (slash == std::string::npos || mime.find('/', slash + 1) != std::string::npos) {
		return false; // 恰好一个 '/'
	}
	const auto seg_ok = [](const std::string& seg) {
		if (seg.empty()) return false;
		for (const char c : seg) {
			const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
				(c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-';
			if (!ok) return false;
		}
		return true;
	};
	return seg_ok(mime.substr(0, slash)) && seg_ok(mime.substr(slash + 1));
}

/// 从 [Resource] 读取资源大小上限（字节）；缺失/非法回退默认值
std::uint64_t ReadResourceLimit(const std::string& key, std::uint64_t fallback) {
	try {
		auto val = ConfigMgr::Inst().GetValue("Resource", key);
		if (!val.empty()) {
			std::size_t pos = 0;
			unsigned long long n = std::stoull(val, &pos);
			if (pos == val.size()) {
				return static_cast<std::uint64_t>(n);
			}
		}
	}
	catch (...) {
		//配置缺失/非数字，回退默认值
	}
	return fallback;
}
} // namespace

LogicSystem::LogicSystem() {
	RegisterCallBacks();
	const std::size_t logic_count = ReadWorkerCount("LogicWorkers");
	const std::size_t delivery_count = ReadWorkerCount("DeliveryWorkers");
	_logic_workers.reserve(logic_count);
	for (std::size_t i = 0; i < logic_count; ++i) {
		_logic_workers.emplace_back(std::make_unique<LogicWorker>());
	}
	_delivery_workers.reserve(delivery_count);
	for (std::size_t i = 0; i < delivery_count; ++i) {
		_delivery_workers.emplace_back(std::make_unique<LogicWorker>());
	}
	std::cout << "LogicSystem started, logic_workers=" << logic_count
		<< " delivery_workers=" << delivery_count << std::endl;
}

LogicSystem::~LogicSystem() {
	Stop();
}

bool LogicSystem::PostMsgToQue(std::size_t routing_key, std::shared_ptr<LogicNode> msg) {
	//停机中或未初始化分片：拒绝，消息不持久化、不重传
	if (_ingress_stopping.load() || _logic_workers.empty()) {
		return false;
	}
	auto& worker = _logic_workers[routing_key % _logic_workers.size()];
	return worker->Post([this, msg]() { DispatchClientMessage(msg); });
}

bool LogicSystem::PostToUser(int uid, LogicWorker::Task task) {
	if (_ingress_stopping.load() || _logic_workers.empty()) {
		return false;
	}
	std::size_t idx = std::hash<int>{}(uid) % _logic_workers.size();
	return _logic_workers[idx]->Post(std::move(task));
}

bool LogicSystem::PostDelivery(int sender_uid, LogicWorker::Task task) {
	if (_delivery_stopping.load() || _delivery_workers.empty()) {
		return false;
	}
	std::size_t idx = std::hash<int>{}(sender_uid) % _delivery_workers.size();
	return _delivery_workers[idx]->Post(std::move(task));
}

void LogicSystem::Stop() {
	//幂等：只允许一次完整排空/join，静态析构重复调用安全
	std::lock_guard<std::mutex> lk(_stop_mutex);
	if (_ingress_stopping.load()) {
		return;
	}
	//1) 先拒绝新的 client/gRPC 入口投递
	_ingress_stopping.store(true);
	//2) 排空/join logic workers；排空期间 PostDelivery 保持开放，任务仍可产生 outbound 跨服投递
	for (auto& w : _logic_workers) {
		if (w) w->Stop();
	}
	//3) logic workers 全部 join 后才拒绝新的跨服投递
	_delivery_stopping.store(true);
	//4) 最后排空/join delivery workers
	for (auto& w : _delivery_workers) {
		if (w) w->Stop();
	}
}

void LogicSystem::DispatchClientMessage(std::shared_ptr<LogicNode> msg) {
	short msg_type = msg->_recvnode->_msg_type;
	std::string msg_data(msg->_recvnode->_data, msg->_recvnode->_cur_len);
	auto session = msg->_session;

	cout << "recv_msg type is " << msg_type << endl;
	if (!session->IsOpen()) {
		return;
	}

	if (msg_type == MSG_CHAT_LOGIN) {
		//登录一次性：已认证后再收登录包一律拒绝并关闭，避免 LoginHandler 踢掉本连接的自毁路径
		if (session->GetUserId() != 0) {
			json err;
			err["error"] = ErrorCodes::UidInvalid;
			//原子发送错误终帧，写完后关闭
			session->SendAndClose(err.dump(4), MSG_CHAT_LOGIN_RSP);
			return;
		}
		//token 失败后 _user_uid 仍为 0，允许用原 routing uid 重试登录，正常进入 LoginHandler
	}
	else {
		//非登录包必须已认证且与路由 uid 一致；登录失败后排队的业务包回 UidInvalid 而不执行
		int user_uid = session->GetUserId();
		int routing_uid = session->GetRoutingUid();
		if (user_uid == 0 || user_uid != routing_uid) {
			json err;
			err["error"] = ErrorCodes::UidInvalid;
			short rsp_id = ReqToRspId(msg_type);
			if (rsp_id != 0) {
				session->Send(err.dump(4), rsp_id);
			}
			return;
		}
	}

	auto call_back_iter = _fun_callbacks.find(msg_type);
	if (call_back_iter == _fun_callbacks.end()) {
		std::cout << "msg type [" << msg_type << "] handler not found" << std::endl;
		return;
	}
	(this->*call_back_iter->second)(session, msg_type, msg_data);
}

void LogicSystem::RegisterCallBacks() {
	_fun_callbacks[MSG_CHAT_LOGIN] = &LogicSystem::LoginHandler;
	_fun_callbacks[ID_SEARCH_USER_REQ] = &LogicSystem::SearchInfo;
	_fun_callbacks[ID_ADD_FRIEND_REQ] = &LogicSystem::AddFriendApply;
	_fun_callbacks[ID_HANDLE_FRIEND_REQ] = &LogicSystem::AuthFriendApply;
	_fun_callbacks[ID_TEXT_CHAT_MSG_REQ] = &LogicSystem::DealChatTextMsg;
	_fun_callbacks[ID_HEART_BEAT_REQ] = &LogicSystem::HeartBeatHandler;
	_fun_callbacks[ID_LOAD_CHAT_THREAD_REQ] = &LogicSystem::GetUserThreadsHandler;
	_fun_callbacks[ID_CREATE_PRIVATE_CHAT_REQ] = &LogicSystem::CreatePrivateChat;
	_fun_callbacks[ID_LOAD_CHAT_MSG_REQ] = &LogicSystem::LoadChatMsg;
	_fun_callbacks[ID_CREATE_RESOURCE_MSG_REQ] = &LogicSystem::DealCreateResourceMsg;
	_fun_callbacks[ID_SYNC_USER_MESSAGE_REQ] = &LogicSystem::DealSyncMessage;
}

void LogicSystem::LoginHandler(shared_ptr<CSession> session, const short &msg_type, const string &msg_data) {
	auto root = json::parse(msg_data, nullptr, false);

	json  rtvalue;
	Defer defer([&rtvalue, session]() {
		std::string return_str = rtvalue.dump(4);
		session->Send(return_str, MSG_CHAT_LOGIN_RSP);
		});

	//请求 JSON 非法 → fail closed，不绑定 session
	if (root.is_discarded()) {
		rtvalue["error"] = ErrorCodes::TokenInvalid;
		return;
	}

	//解析请求 {uid, token}（token 由 StatusServer 密码登录后下发，存于 Redis utoken_<uid>）
	int uid = 0;
	std::string token;
	try {
		uid = root.at("uid").get<int>();
		token = root.at("token").get<std::string>();
	}
	catch (...) {
		rtvalue["error"] = ErrorCodes::TokenInvalid;
		return;
	}

	std::string uid_str = std::to_string(uid);

	//唯一鉴权：Redis 中 utoken_<uid> 必须存在且等于请求 token（缺失/不匹配一律 TokenInvalid，不绑定）
	std::string stored;
	bool ok = RedisMgr::GetInstance()->Get(USERTOKENPREFIX + uid_str, stored);
	if (!ok || stored != token) {
		rtvalue["error"] = ErrorCodes::TokenInvalid;
		return;
	}

	//本服务器名（踢人跨服寻址与 uip_ 绑定均依赖）
	auto self_name = ConfigMgr::Inst().GetValue("SelfServer", "Name");

	//加载用户基础信息（失败一律 UidInvalid 且不绑定）
	std::string base_key = USER_BASE_INFO + uid_str;
	auto user_info = std::make_shared<UserInfo>();
	bool b_base = GetBaseInfo(base_key, uid, user_info);
	if (!b_base) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}

	//构造响应：保留原有用户字段（不含 pwd、token 等任何凭证字段）
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["uid"] = uid;
	rtvalue["name"] = user_info->username;
	rtvalue["email"] = user_info->email;
	rtvalue["nick"] = user_info->nickname;
	rtvalue["desc"] = user_info->profile_bio;
	rtvalue["sex"] = user_info->gender;
	rtvalue["icon"] = user_info->avatar_key;
	std::uint64_t checkpoint = 0;
	if (!MysqlMgr::GetInstance()->GetLastEventSeq(uid, checkpoint)) {
		rtvalue["error"] = ErrorCodes::MESSAGE_STORE_FAILED;
		return;
	}
	rtvalue["checkpoint"] = std::to_string(checkpoint);
	rtvalue["apply_list"] = json::array();
	rtvalue["friend_list"] = json::array();

	//从数据库获取申请列表
	std::vector<std::shared_ptr<ApplyInfo>> apply_list;
	if (!GetFriendApplyInfo(uid, apply_list)) {
		rtvalue["error"] = ErrorCodes::MESSAGE_STORE_FAILED;
		return;
	}
	for (const auto& apply : apply_list) {
		if (!apply || !apply->request || !apply->peer) continue;
		json obj;
		obj["friend_request_id"] = std::to_string(apply->request->friend_request_id);
		obj["requester_user_id"] = apply->request->requester_user_id;
		obj["target_user_id"] = apply->request->target_user_id;
		obj["request_message"] = apply->request->request_message;
		obj["status"] = static_cast<int>(apply->request->status);
		obj["thread_id"] = apply->request->thread_id > 0
			? json(std::to_string(apply->request->thread_id)) : json(nullptr);
		obj["peer_username"] = apply->peer->username;
		obj["peer_nickname"] = apply->peer->nickname;
		obj["peer_avatar_key"] = apply->peer->avatar_key;
		obj["peer_gender"] = apply->peer->gender;
		rtvalue["apply_list"].push_back(std::move(obj));
	}

	//获取好友列表
	std::vector<ContactInfo> contacts;
	if (!GetFriendList(uid, contacts)) {
		rtvalue["error"] = ErrorCodes::MESSAGE_STORE_FAILED;
		return;
	}
	for (const auto& contact : contacts) {
		if (!contact.user) continue;
		json obj;
		obj["user_id"] = contact.user->user_id;
		obj["username"] = contact.user->username;
		obj["nickname"] = contact.user->nickname;
		obj["avatar_key"] = contact.user->avatar_key;
		obj["gender"] = contact.user->gender;
		obj["thread_id"] = contact.thread_id > 0
			? json(std::to_string(contact.thread_id)) : json(nullptr);
		rtvalue["friend_list"].push_back(obj);
	}

	//分布式锁内执行踢人 + 绑定 session / 设置 uip_/usession_
	{
		auto lock_key = LOCK_PREFIX + uid_str;
		auto identifier = RedisMgr::GetInstance()->acquireLock(lock_key, LOCK_TIME_OUT, ACQUIRE_TIME_OUT);
		Defer defer2([this, identifier, lock_key]() {
			RedisMgr::GetInstance()->releaseLock(lock_key, identifier);
			});
		if (identifier.empty()) {
			//未能获取登录锁，fail closed 不绑定 session
			rtvalue["error"] = ErrorCodes::RPCFailed;
			return;
		}

		//登录即踢掉该用户的旧连接（异地/同机重新登录均触发单点登录）
		std::string uid_ip_value = "";
		auto uid_ip_key = USERIPPREFIX + uid_str;
		bool b_ip = RedisMgr::GetInstance()->Get(uid_ip_key, uid_ip_value);
		if (b_ip) {
			if (uid_ip_value == self_name) {
				//旧登录就在本服务器：直接发踢人帧并清除旧连接
				auto old_session = UserMgr::GetInstance()->GetSession(uid);
				if (old_session && old_session != session) {
					old_session->NotifyOffline(uid);
				}
			}
			else {
				//旧登录在其它服务器：经 gRPC 通知踢人
				message::KickUserReq kick_req;
				kick_req.set_uid(uid);
				ChatGrpcClient::GetInstance()->NotifyKickUser(uid_ip_value, kick_req);
			}
		}

		//关闭可能与登录并发；只有仍为 Open 的会话才能完成身份绑定。
		if (!session->TrySetUserId(uid)) {
			rtvalue["error"] = ErrorCodes::RPCFailed;
			return;
		}
		//为用户设置登录 ip server 的名字
		std::string  ipkey = USERIPPREFIX + uid_str;
		RedisMgr::GetInstance()->Set(ipkey, self_name);
		//uid 和 session 绑定管理,方便以后踢人操作
		UserMgr::GetInstance()->SetUserSession(uid, session);
		std::string  uid_session_key = USER_SESSION_PREFIX + uid_str;
		RedisMgr::GetInstance()->Set(uid_session_key, session->GetSessionId());
	}

	return;
}

void LogicSystem::SearchInfo(std::shared_ptr<CSession> session, const short& msg_type, const string& msg_data)
{
	auto root = json::parse(msg_data, nullptr, false);
	auto uid_str = root["uid"].get<std::string>();
	std::cout << "user SearchInfo uid is  " << uid_str << endl;

	json  rtvalue;

	Defer defer([this, &rtvalue, session]() {
		std::string return_str = rtvalue.dump(4);
		session->Send(return_str, ID_SEARCH_USER_RSP);
		});

	bool b_digit = isPureDigit(uid_str);
	if (b_digit) {
		GetUserByUid(uid_str, rtvalue);
	}
	else {
		GetUserByName(uid_str, rtvalue);
	}
	return;
}

void LogicSystem::AddFriendApply(std::shared_ptr<CSession> session,
	const short&, const string& msg_data)
{
	json rsp;
	auto root = json::parse(msg_data, nullptr, false);
	int target_user_id = 0;
	if (!root.is_object() || !root.contains("target_user_id") ||
		!ParseJsonUid(root["target_user_id"], target_user_id) ||
		(root.contains("request_message") && !root["request_message"].is_string()) ||
		!root.contains("client_request_id") || !root["client_request_id"].is_string() ||
		root["client_request_id"].get<std::string>().empty()) {
		rsp["error"] = ErrorCodes::Error_Json;
		session->Send(rsp.dump(), ID_ADD_FRIEND_RSP);
		return;
	}
	const int requester_user_id = session->GetUserId();
	const std::string request_message = root.value("request_message", std::string());
	const std::string client_request_id = root["client_request_id"].get<std::string>();
	if (target_user_id <= 0 || target_user_id == requester_user_id ||
		client_request_id.size() > 64 || request_message.size() > 255) {
		rsp["error"] = ErrorCodes::Error_Json;
		session->Send(rsp.dump(), ID_ADD_FRIEND_RSP);
		return;
	}

	std::shared_ptr<FriendRequest> request;
	const auto result = MysqlMgr::GetInstance()->AddFriendApply(
		requester_user_id, target_user_id, request_message, client_request_id, request);
	bool should_deliver = false;
	if (result == FriendOperationResult::AlreadyFriends) {
		rsp["error"] = ErrorCodes::AlreadyFriends;
	} else if (result == FriendOperationResult::NotFound) {
		rsp["error"] = ErrorCodes::UidInvalid;
	} else if (result == FriendOperationResult::Conflict) {
		rsp["error"] = ErrorCodes::MESSAGE_CONFLICT;
	} else if (result == FriendOperationResult::Failed || !request) {
		rsp["error"] = ErrorCodes::MESSAGE_STORE_FAILED;
	} else {
		rsp["error"] = ErrorCodes::Success;
		rsp.update(BuildFriendRequestEnvelope(request, requester_user_id));
		should_deliver = result == FriendOperationResult::Stored;
	}
	session->Send(rsp.dump(), ID_ADD_FRIEND_RSP);
	if (should_deliver) {
		UserEvent event;
		event.event_seq = request->event_seq;
		event.event_type = static_cast<int>(UserEventType::FRIEND_APPLY);
		event.friend_request = request;
		DeliverUserEvent(event, target_user_id);
	}
}

void LogicSystem::AuthFriendApply(std::shared_ptr<CSession> session,
	const short&, const string& msg_data)
{
	json rsp;
	auto root = json::parse(msg_data, nullptr, false);
	std::int64_t friend_request_id = 0;
	if (!root.is_object() || !root.contains("friend_request_id") ||
		!root["friend_request_id"].is_string() ||
		!ParseJsonId(root["friend_request_id"], friend_request_id) ||
		!root.contains("action") || !root["action"].is_string() ||
		friend_request_id <= 0) {
		rsp["error"] = ErrorCodes::Error_Json;
		session->Send(rsp.dump(), ID_HANDLE_FRIEND_RSP);
		return;
	}
	const std::string action = root["action"].get<std::string>();
	if (action != "accept" && action != "reject") {
		rsp["error"] = ErrorCodes::FriendActionInvalid;
		session->Send(rsp.dump(), ID_HANDLE_FRIEND_RSP);
		return;
	}
	FriendHandleOutput output;
	const auto result = MysqlMgr::GetInstance()->HandleFriendApply(
		session->GetUserId(), friend_request_id, action == "accept", output);
	bool should_deliver = false;
	switch (result) {
	case FriendOperationResult::Stored:
	case FriendOperationResult::Duplicate:
		rsp["error"] = ErrorCodes::Success;
		if (output.request) {
			rsp.update(BuildFriendRequestEnvelope(output.request, session->GetUserId()));
		}
		should_deliver = result == FriendOperationResult::Stored && output.request;
		break;
	case FriendOperationResult::NotFound:
		rsp["error"] = ErrorCodes::FriendRequestNotFound;
		break;
	case FriendOperationResult::AlreadyHandled:
		rsp["error"] = ErrorCodes::FriendRequestHandled;
		break;
	case FriendOperationResult::AlreadyFriends:
		rsp["error"] = ErrorCodes::AlreadyFriends;
		break;
	case FriendOperationResult::Forbidden:
		rsp["error"] = ErrorCodes::FriendRequestNotFound;
		break;
	default:
		rsp["error"] = ErrorCodes::MESSAGE_STORE_FAILED;
		break;
	}
	session->Send(rsp.dump(), ID_HANDLE_FRIEND_RSP);
	if (should_deliver) {
		UserEvent event;
		event.event_seq = output.request->event_seq;
		event.event_type = action == "accept"
			? static_cast<int>(UserEventType::FRIEND_ACCEPT)
			: static_cast<int>(UserEventType::FRIEND_REJECT);
		event.friend_request = output.request;
		DeliverUserEvent(event, output.request->requester_user_id);
	}
}

void LogicSystem::DealChatTextMsg(std::shared_ptr<CSession> session, const short& msg_type, const string& msg_data) {
	auto root = json::parse(msg_data, nullptr, false);
	int target_user_id = 0;
	if (!root.is_object() || !root.contains("target_user_id") ||
		!ParseJsonUid(root["target_user_id"], target_user_id) ||
		!root.contains("thread_id") || !root["thread_id"].is_string() ||
		!root.contains("text_content") || !root["text_content"].is_string() ||
		!root.contains("client_message_id") || !root["client_message_id"].is_string() ||
		root["client_message_id"].get<std::string>().empty()) {
		json err;
		err["error"] = ErrorCodes::Error_Json;
		session->Send(err.dump(), ID_TEXT_CHAT_MSG_RSP);
		return;
	}

	auto uid = session->GetUserId();
	//thread_id 按十进制字符串解析。
	std::int64_t thread_id = 0;
	if (!ParseJsonId(root["thread_id"], thread_id)) {
		json err;
		err["error"] = ErrorCodes::Error_Json;
		session->Send(err.dump(4), ID_TEXT_CHAT_MSG_RSP);
		return;
	}
	int member1 = 0;
	int member2 = 0;
	if (!MysqlMgr::GetInstance()->GetPrivateChatMembers(thread_id, member1, member2) ||
		!((uid == member1 && target_user_id == member2) ||
			(uid == member2 && target_user_id == member1))) {
		json err;
		err["error"] = ErrorCodes::CREATE_CHAT_FAILED;
		session->Send(err.dump(), ID_TEXT_CHAT_MSG_RSP);
		return;
	}

	//当前文本发送协议只接收单条 client_message_id + text_content。
	auto text_content = root["text_content"].get<std::string>();
	auto client_message_id = root["client_message_id"].get<std::string>();
	if (client_message_id.size() > 64) {
		json err;
		err["error"] = ErrorCodes::Error_Json;
		session->Send(err.dump(), ID_TEXT_CHAT_MSG_RSP);
		return;
	}

	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["client_message_id"] = client_message_id;
	auto chat_msg = std::make_shared<ChatMessage>();
	chat_msg->created_at = getCurrentTimestamp();
	chat_msg->sender_user_id = uid;
	chat_msg->recipient_user_id = target_user_id;
	chat_msg->client_message_id = client_message_id;
	chat_msg->thread_id = thread_id;
	chat_msg->text_content = text_content;
	chat_msg->status = MessageStatus::Published;
	chat_msg->message_type = static_cast<int>(ChatMsgType::TEXT);

	//消息、接收方 event_seq 和事件引用在同一事务提交。
	auto save_res = MysqlMgr::GetInstance()->AddChatMsg(chat_msg);
	if (save_res == SaveMessageResult::Failed) {
		//持久化失败：不返回成功、不转发，sender 按 transient 重传。
		rtvalue["error"] = ErrorCodes::MESSAGE_STORE_FAILED;
		session->Send(rtvalue.dump(4), ID_TEXT_CHAT_MSG_RSP);
		return;
	}
	if (save_res == SaveMessageResult::Conflict) {
		//永久冲突：原消息不变，发送端停止重传该 client_message_id。
		rtvalue["error"] = ErrorCodes::MESSAGE_CONFLICT;
		rtvalue["client_message_id"] = chat_msg->client_message_id;
		session->Send(rtvalue.dump(4), ID_TEXT_CHAT_MSG_RSP);
		return;
	}

	//Stored/Duplicate：canonical 持久值 envelope 拍平到响应顶层（计划5.4/5.3）
	rtvalue.update(BuildMessageEnvelope(chat_msg));

	//【关键顺序】事务已提交 → 先发 1302（语义固定为“服务端已持久化”），再做 live delivery。
	//不得用 Defer 延后：业务响应必须先于接收方实时通知。
	std::string sender_rsp = rtvalue.dump(4);
	session->Send(sender_rsp, ID_TEXT_CHAT_MSG_RSP);
	if (save_res == SaveMessageResult::Stored) {
		UserEvent event;
		event.event_seq = chat_msg->event_seq;
		event.event_type = chat_msg->message_type;
		event.message = chat_msg;
		DeliverUserEvent(event, chat_msg->recipient_user_id);
	}
}

void LogicSystem::HeartBeatHandler(std::shared_ptr<CSession> session, const short& msg_type, const string& msg_data) {
	std::cout << "receive heart beat msg, uid is " << session->GetUserId() << std::endl;
	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	session->Send(rtvalue.dump(4), ID_HEARTBEAT_RSP);
}

bool LogicSystem::isPureDigit(const std::string& str)
{
	for (char c : str) {
		if (!std::isdigit(c)) {
			return false;
		}
	}
	return true;
}

void LogicSystem::GetUserByUid(std::string uid_str, json& rtvalue)
{
	rtvalue["error"] = ErrorCodes::Success;

	std::string base_key = USER_BASE_INFO + uid_str;

	//优先查redis中查询用户信息
	std::string info_str = "";
	bool b_base = RedisMgr::GetInstance()->Get(base_key, info_str);
	if (b_base) {
		auto root = json::parse(info_str, nullptr, false);
		auto uid = root["uid"].get<int>();
		auto name = root["name"].get<std::string>();
		auto email = root["email"].get<std::string>();
		auto nick = root["nick"].get<std::string>();
		auto desc = root["desc"].get<std::string>();
		auto sex = root["sex"].get<int>();
		auto icon = root["icon"].get<std::string>();
		std::cout << "user  uid is  " << uid << " name  is "
			<< name << " email is " << email <<" icon is " << icon << endl;

		rtvalue["uid"] = uid;
		rtvalue["name"] = name;
		rtvalue["email"] = email;
		rtvalue["nick"] = nick;
		rtvalue["desc"] = desc;
		rtvalue["sex"] = sex;
		rtvalue["icon"] = icon;
		return;
	}

	auto uid = std::stoi(uid_str);
	//redis中没有则查询mysql
	//查询数据库
	std::shared_ptr<UserInfo> user_info = nullptr;
	user_info = MysqlMgr::GetInstance()->GetUser(uid);
	if (user_info == nullptr) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}

	//将数据库内容写入redis缓存
	json redis_root;
	redis_root["uid"] = user_info->user_id;
	redis_root["name"] = user_info->username;
	redis_root["email"] = user_info->email;
	redis_root["nick"] = user_info->nickname;
	redis_root["desc"] = user_info->profile_bio;
	redis_root["sex"] = user_info->gender;
	redis_root["icon"] = user_info->avatar_key;

	RedisMgr::GetInstance()->Set(base_key, redis_root.dump(4));

	//返回数据
	rtvalue["uid"] = user_info->user_id;
	rtvalue["name"] = user_info->username;
	rtvalue["email"] = user_info->email;
	rtvalue["nick"] = user_info->nickname;
	rtvalue["desc"] = user_info->profile_bio;
	rtvalue["sex"] = user_info->gender;
	rtvalue["icon"] = user_info->avatar_key;
}

void LogicSystem::GetUserByName(std::string name, json& rtvalue)
{
	rtvalue["error"] = ErrorCodes::Success;

	std::string base_key = NAME_INFO + name;

	//优先查redis中查询用户信息
	std::string info_str = "";
	bool b_base = RedisMgr::GetInstance()->Get(base_key, info_str);
	if (b_base) {
		auto root = json::parse(info_str, nullptr, false);
		auto uid = root["uid"].get<int>();
		auto name = root["name"].get<std::string>();
		auto email = root["email"].get<std::string>();
		auto nick = root["nick"].get<std::string>();
		auto desc = root["desc"].get<std::string>();
		auto sex = root["sex"].get<int>();
		auto icon = root["icon"].get<std::string>();
		std::cout << "user  uid is  " << uid << " name  is "
			<< name << " email is " << email << endl;

		rtvalue["uid"] = uid;
		rtvalue["name"] = name;
		rtvalue["email"] = email;
		rtvalue["nick"] = nick;
		rtvalue["desc"] = desc;
		rtvalue["sex"] = sex;
		rtvalue["icon"] = icon;
		return;
	}

	//redis中没有则查询mysql
	//查询数据库
	std::shared_ptr<UserInfo> user_info = nullptr;
	user_info = MysqlMgr::GetInstance()->GetUser(name);
	if (user_info == nullptr) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}

	//将数据库内容写入redis缓存
	json redis_root;
	redis_root["uid"] = user_info->user_id;
	redis_root["name"] = user_info->username;
	redis_root["email"] = user_info->email;
	redis_root["nick"] = user_info->nickname;
	redis_root["desc"] = user_info->profile_bio;
	redis_root["sex"] = user_info->gender;
	redis_root["icon"] = user_info->avatar_key;

	RedisMgr::GetInstance()->Set(base_key, redis_root.dump(4));
	
	//返回数据
	rtvalue["uid"] = user_info->user_id;
	rtvalue["name"] = user_info->username;
	rtvalue["email"] = user_info->email;
	rtvalue["nick"] = user_info->nickname;
	rtvalue["desc"] = user_info->profile_bio;
	rtvalue["sex"] = user_info->gender;
	rtvalue["icon"] = user_info->avatar_key;
}

bool LogicSystem::GetBaseInfo(std::string base_key, int uid, std::shared_ptr<UserInfo>& userinfo)
{
	//优先查redis中查询用户信息
	std::string info_str = "";
	bool b_base = RedisMgr::GetInstance()->Get(base_key, info_str);
	if (b_base) {
		auto root = json::parse(info_str, nullptr, false);
		userinfo->user_id = root["uid"].get<int>();
		userinfo->username = root["name"].get<std::string>();
		userinfo->email = root["email"].get<std::string>();
		userinfo->nickname = root["nick"].get<std::string>();
		userinfo->profile_bio = root["desc"].get<std::string>();
		userinfo->gender = root["sex"].get<int>();
		userinfo->avatar_key = root["icon"].get<std::string>();
		std::cout << "user login uid is  " << userinfo->user_id << " name  is "
			<< userinfo->username << " email is " << userinfo->email << endl;
	}
	else {
		//redis中没有则查询mysql
		//查询数据库
		std::shared_ptr<UserInfo> user_info = nullptr;
		user_info = MysqlMgr::GetInstance()->GetUser(uid);
		if (user_info == nullptr) {
			return false;
		}

		userinfo = user_info;

		//将数据库内容写入redis缓存
		json redis_root;
		redis_root["uid"] = uid;
		redis_root["name"] = userinfo->username;
		redis_root["email"] = userinfo->email;
		redis_root["nick"] = userinfo->nickname;
		redis_root["desc"] = userinfo->profile_bio;
		redis_root["sex"] = userinfo->gender;
		redis_root["icon"] = userinfo->avatar_key;
		RedisMgr::GetInstance()->Set(base_key, redis_root.dump(4));
	}

	return true;
}

bool LogicSystem::GetFriendApplyInfo(int target_user_id,
	std::vector<std::shared_ptr<ApplyInfo>> &list) {
	list.clear();
	constexpr int kPageSize = 100;
	std::int64_t after_friend_request_id = 0;
	for (;;) {
		std::vector<std::shared_ptr<ApplyInfo>> page;
		if (!MysqlMgr::GetInstance()->GetApplyList(
			target_user_id, page, after_friend_request_id, kPageSize)) {
			return false;
		}
		if (page.empty()) return true;
		after_friend_request_id = page.back()->request->friend_request_id;
		list.insert(list.end(), page.begin(), page.end());
		if (static_cast<int>(page.size()) < kPageSize) return true;
	}
}

bool LogicSystem::GetFriendList(int user_id, std::vector<ContactInfo>& contacts) {
	return MysqlMgr::GetInstance()->GetFriendList(user_id, contacts);
}

void LogicSystem::GetUserThreadsHandler(std::shared_ptr<CSession> session, 
	const short& msg_type, const string& msg_data)
{
	//从数据库加chat_threads记录
	auto root = json::parse(msg_data, nullptr, false);
	auto uid = session->GetUserId();
	std::cout << "get uid  threads  " << uid << std::endl;

	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["uid"] = uid;
	Defer defer([this, &rtvalue, session]() {
		std::string return_str = rtvalue.dump(4);
		session->Send(return_str, ID_LOAD_CHAT_THREAD_RSP);
		});

	//thread_id 游标按 64 位十进制字符串解析。
	std::int64_t last_id = 0;
	if (!ParseJsonId(root["thread_id"], last_id)) {
		rtvalue["error"] = ErrorCodes::Error_Json;
		return;
	}

	std::vector<std::shared_ptr<ChatThreadInfo>> threads;

	int page_size = 10;
	bool load_more = false;
	std::int64_t next_last_id = 0;
	bool res = GetUserThreads(uid, last_id, page_size, threads, load_more, next_last_id);
	if (!res) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}


	rtvalue["load_more"] = load_more;
	rtvalue["next_last_id"] = std::to_string(next_last_id);
	//整理threads数据写入json返回（thread_id 一律十进制字符串）
	for (auto& thread : threads) {
		json thread_value;
		thread_value["thread_id"] = std::to_string(thread->_thread_id);
		thread_value["lower_user_id"] = thread->_lower_user_id;
		thread_value["higher_user_id"] = thread->_higher_user_id;
		thread_value["last_message_id"] = std::to_string(thread->_last_msg_id);
		rtvalue["threads"].push_back(thread_value);
	}
}

bool LogicSystem::GetUserThreads(int64_t userId,
	int64_t lastId,
	int      pageSize,
	std::vector<std::shared_ptr<ChatThreadInfo>>& threads,
	bool& loadMore,
	int64_t& nextLastId)
{
	return MysqlMgr::GetInstance()->GetUserThreads(userId, lastId, pageSize, 
		threads, loadMore, nextLastId);
}

void LogicSystem::CreatePrivateChat(std::shared_ptr<CSession> session, const short& msg_type, const string& msg_data)
{
	auto root = json::parse(msg_data, nullptr, false);
	auto uid = session->GetUserId();
	
	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;

	Defer defer([this, &rtvalue, session]() {
		std::string return_str = rtvalue.dump(4);
		session->Send(return_str, ID_CREATE_PRIVATE_CHAT_RSP);
		});
	if (root.is_discarded() || !root.is_object() ||
		!root.contains("target_user_id") || !root["target_user_id"].is_number_integer()) {
		rtvalue["error"] = ErrorCodes::Error_Json;
		return;
	}
	const int target_user_id = root["target_user_id"].get<int>();
	if (uid <= 0 || target_user_id <= 0 || uid == target_user_id) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}
	rtvalue["target_user_id"] = target_user_id;

	std::int64_t thread_id = 0;
	bool res = MysqlMgr::GetInstance()->CreatePrivateChat(uid, target_user_id, thread_id);
	if (!res) {
		rtvalue["error"] = ErrorCodes::CREATE_CHAT_FAILED;
		return;
	}

	rtvalue["thread_id"] = std::to_string(thread_id);
}

void LogicSystem::LoadChatMsg(std::shared_ptr<CSession> session,
	const short& msg_type, const string& msg_data) {

	auto root = json::parse(msg_data, nullptr, false);

	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;

	Defer defer([this, &rtvalue, session]() {
		std::string return_str = rtvalue.dump(4);
		session->Send(return_str, ID_LOAD_CHAT_MSG_RSP);
		});

	//thread_id / before_message_id 按 64 位十进制字符串解析。
	std::int64_t thread_id = 0;
	if (!ParseJsonId(root["thread_id"], thread_id)) {
		rtvalue["error"] = ErrorCodes::Error_Json;
		return;
	}
	std::int64_t message_id = 0;
	if (root.contains("before_message_id") &&
		!ParseJsonId(root["before_message_id"], message_id)) {
		rtvalue["error"] = ErrorCodes::Error_Json;
		return;
	}

	rtvalue["thread_id"] = std::to_string(thread_id);

	int page_size = 10;
	std::shared_ptr<PageResult> res = MysqlMgr::GetInstance()->LoadChatMsg(
		session->GetUserId(), thread_id, message_id, page_size);
	if (!res) {
		rtvalue["error"] = ErrorCodes::LOAD_CHAT_FAILED;
		return;
	}

	rtvalue["next_message_id"] = std::to_string(res->next_cursor);
	rtvalue["load_more"] = res->load_more;
	for (auto& chat : res->messages) {
		rtvalue["messages"].push_back(
			BuildMessageEnvelope(std::make_shared<ChatMessage>(chat)));
	}

}

void LogicSystem::DealCreateResourceMsg(std::shared_ptr<CSession> session,
	const short& msg_type, const string& msg_data) {
	//1503 创建资源消息（图片/文件统一）：只登记元数据，不含文件体。
	// Request contains only current resource metadata. Sender comes from Session.
	auto root = json::parse(msg_data, nullptr, false);

	json rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	auto reject = [&rtvalue, &session](ErrorCodes code) {
		rtvalue["error"] = code;
		session->Send(rtvalue.dump(4), ID_CREATE_RESOURCE_MSG_RSP);
	};

	if (root.is_discarded() || !root.is_object()) {
		reject(ErrorCodes::Error_Json);
		return;
	}

	//---- 字段抽取（类型严格校验，缺一即 Error_Json）----
	auto get_int = [&root](const char* k, int& out) -> bool {
		return root.contains(k) && ParseJsonInt(root[k], out);
	};
	auto get_str = [&root](const char* k, std::string& out) -> bool {
		if (!root.contains(k) || !root[k].is_string()) return false;
		out = root[k].get<std::string>();
		return true;
	};

	const int uid = session->GetUserId();
	int target_user_id = 0, message_type = 0;
	std::int64_t thread_id = 0;
	std::string client_message_id, original_file_name, sha256, mime_type;
	if (!get_int("target_user_id", target_user_id) ||
		!get_int("message_type", message_type) ||
		!root.contains("thread_id") || !ParseJsonId(root["thread_id"], thread_id) ||
		!get_str("client_message_id", client_message_id) ||
		!get_str("original_file_name", original_file_name) ||
		!get_str("sha256", sha256) || !get_str("mime_type", mime_type)) {
		reject(ErrorCodes::Error_Json);
		return;
	}
	if (client_message_id.empty() || client_message_id.size() > 64) {
		reject(ErrorCodes::Error_Json);
		return;
	}
	rtvalue["client_message_id"] = client_message_id;

	std::uint64_t file_size_bytes = 0;
	if (root.contains("file_size_bytes") && root["file_size_bytes"].is_string()) {
		const auto value = root["file_size_bytes"].get<std::string>();
		try {
			if (!value.empty() && std::all_of(value.begin(), value.end(),
				[](unsigned char c) { return std::isdigit(c) != 0; })) {
				file_size_bytes = std::stoull(value);
			}
		} catch (...) {
			file_size_bytes = 0;
		}
	}

	//---- 校验链 ----
	//1. 发送者身份只取已认证 session，不接收客户端 sender_user_id。
	if (uid <= 0 || target_user_id <= 0) {
		reject(ErrorCodes::UidInvalid);
		return;
	}

	//2. sender_user_id/target_user_id 必须都是该私聊会话成员。
	int member1 = 0, member2 = 0;
	if (!MysqlMgr::GetInstance()->GetPrivateChatMembers(thread_id, member1, member2) ||
		!((uid == member1 && target_user_id == member2) ||
			(uid == member2 && target_user_id == member1))) {
		reject(ErrorCodes::CREATE_CHAT_FAILED);
		return;
	}

	//3. 类型：仅图片(1)/文件(3)
	const bool is_pic = message_type == static_cast<int>(ChatMsgType::PIC);
	const bool is_file = message_type == static_cast<int>(ChatMsgType::FILE);
	if (!is_pic && !is_file) {
		reject(ErrorCodes::ResourceInvalid);
		return;
	}

	//4. 大小：>0 且不超过类型上限（图片 20MB / 文件 100MB，可由 [Resource] 配置覆盖）
	const std::uint64_t size_limit = is_pic
		? ReadResourceLimit("MaxImageSize", kDefaultMaxImageSize)
		: ReadResourceLimit("MaxFileSize", kDefaultMaxFileSize);
	if (file_size_bytes == 0 || file_size_bytes > size_limit) {
		reject(ErrorCodes::ResourceSizeExceeded);
		return;
	}

	//5. 文件名：清洗后作为 content 存储（磁盘文件以 message_id 命名，content 仅展示用）
	if (!SanitizeFileName(original_file_name, original_file_name)) {
		reject(ErrorCodes::ResourceInvalid);
		return;
	}

	//6. 整文件哈希：64 位小写 hex（ResourceServer 收齐分片后据此校验）
	if (!llfc::IsValidSha256Hex(sha256)) {
		reject(ErrorCodes::ResourceInvalid);
		return;
	}

	//7. MIME 类型：type/subtype 格式
	if (!IsValidMimeType(mime_type)) {
		reject(ErrorCodes::ResourceInvalid);
		return;
	}

	//---- 落库（幂等 UPSERT：duplicate 回同一 canonical message_id，不建第二行）----
	//服务端生成 created_at，不读取客户端时间；资源消息初始状态为 PENDING。
	auto chat_msg = std::make_shared<ChatMessage>();
	chat_msg->created_at = getCurrentTimestamp();
	chat_msg->sender_user_id = uid;
	chat_msg->recipient_user_id = target_user_id;
	chat_msg->client_message_id = client_message_id;
	chat_msg->thread_id = thread_id;
	chat_msg->message_type = message_type;
	chat_msg->status = MessageStatus::Pending;
	chat_msg->resource = std::make_shared<MessageResource>();
	chat_msg->resource->original_file_name = original_file_name;
	chat_msg->resource->file_size_bytes = file_size_bytes;
	chat_msg->resource->sha256 = sha256;
	chat_msg->resource->mime_type = mime_type;

	auto save_res = MysqlMgr::GetInstance()->AddChatMsg(chat_msg);
	if (save_res == SaveMessageResult::Failed) {
		//持久化失败：不返回成功、不转发，sender 按 transient 重传
		reject(ErrorCodes::MESSAGE_STORE_FAILED);
		return;
	}
	if (save_res == SaveMessageResult::Conflict) {
		//永久冲突：原消息不变，不创建第二行。
		reject(ErrorCodes::MESSAGE_CONFLICT);
		return;
	}

	//canonical：message_id/file_size_bytes 使用十进制字符串。
	rtvalue.update(BuildMessageEnvelope(chat_msg));

	//【关键顺序】事务已提交 → 发 1504 sender response（语义固定为“服务端已持久化”）
	session->Send(rtvalue.dump(4), ID_CREATE_RESOURCE_MSG_RSP);

	//Uploading 资源不写同步行、不实时通知，待 ResourceServer 上传完成点
	//ResourceServer 在同一事务中置 PUBLISHED 并分配 event_seq 后才对接收者可见，
	//避免同步到尚不可下载的资源。
}

json LogicSystem::BuildMessageEnvelope(const std::shared_ptr<ChatMessage>& msg) {
	json env;
	if (!msg) return env;
	env["event_type"] = msg->message_type;
	if (msg->event_seq > 0) env["event_seq"] = std::to_string(msg->event_seq);
	env["message_id"] = std::to_string(msg->message_id);
	env["client_message_id"] = msg->client_message_id;
	env["thread_id"] = std::to_string(msg->thread_id);
	env["sender_user_id"] = msg->sender_user_id;
	env["message_type"] = msg->message_type;
	env["created_at"] = msg->created_at;
	if (msg->message_type == static_cast<int>(ChatMsgType::TEXT)) {
		env["text_content"] = msg->text_content;
	} else if (msg->resource) {
		env["original_file_name"] = msg->resource->original_file_name;
		env["file_size_bytes"] = std::to_string(msg->resource->file_size_bytes);
		env["sha256"] = msg->resource->sha256;
		env["mime_type"] = msg->resource->mime_type;
	}
	return env;
}

json LogicSystem::BuildFriendRequestEnvelope(
	const std::shared_ptr<FriendRequest>& request, int recipient_user_id) {
	json env;
	if (!request) return env;
	int event_type = static_cast<int>(UserEventType::FRIEND_APPLY);
	if (request->status == FriendRequestStatus::Accepted) {
		event_type = static_cast<int>(UserEventType::FRIEND_ACCEPT);
	} else if (request->status == FriendRequestStatus::Rejected) {
		event_type = static_cast<int>(UserEventType::FRIEND_REJECT);
	}
	env["event_type"] = event_type;
	if (request->event_seq > 0) env["event_seq"] = std::to_string(request->event_seq);
	env["friend_request_id"] = std::to_string(request->friend_request_id);
	env["requester_user_id"] = request->requester_user_id;
	env["target_user_id"] = request->target_user_id;
	env["client_request_id"] = request->client_request_id;
	env["request_message"] = request->request_message;
	env["status"] = static_cast<int>(request->status);
	env["thread_id"] = request->thread_id > 0
		? json(std::to_string(request->thread_id)) : json(nullptr);
	const int peer_user_id = recipient_user_id == request->requester_user_id
		? request->target_user_id : request->requester_user_id;
	auto peer = MysqlMgr::GetInstance()->GetUser(peer_user_id);
	if (peer) {
		env["peer_username"] = peer->username;
		env["peer_nickname"] = peer->nickname;
		env["peer_avatar_key"] = peer->avatar_key;
		env["peer_gender"] = peer->gender;
	}
	return env;
}

json LogicSystem::BuildUserEventEnvelope(const UserEvent& event,
	int recipient_user_id) {
	json env = event.message ? BuildMessageEnvelope(event.message)
		: BuildFriendRequestEnvelope(event.friend_request, recipient_user_id);
	env["event_type"] = event.event_type;
	env["event_seq"] = std::to_string(event.event_seq);
	return env;
}

void LogicSystem::DeliverUserEvent(const UserEvent& event, int recipient_user_id) {
	if (event.event_seq == 0 || recipient_user_id <= 0) return;
	std::string server_name;
	if (!RedisMgr::GetInstance()->Get(
		USERIPPREFIX + std::to_string(recipient_user_id), server_name)) {
		return;
	}
	const auto self_name = ConfigMgr::Inst()["SelfServer"]["Name"];
	if (server_name == self_name) {
		auto envelope = BuildUserEventEnvelope(event, recipient_user_id);
		envelope["error"] = ErrorCodes::Success;
		const std::string payload = envelope.dump();
		PostToUser(recipient_user_id, [uid = recipient_user_id, payload]() {
			auto target = UserMgr::GetInstance()->GetSession(uid);
			if (target) target->Send(payload, ID_NOTIFY_USER_MESSAGE);
		});
		return;
	}
	const std::int64_t message_id = event.message ? event.message->message_id : 0;
	const std::int64_t friend_request_id = event.friend_request
		? event.friend_request->friend_request_id : 0;
	const int sender_user_id = event.message ? event.message->sender_user_id
		: event.friend_request->requester_user_id;
	PostDelivery(sender_user_id, [server_name, recipient_user_id, event_type = event.event_type,
		message_id, friend_request_id]() {
		auto result = ChatGrpcClient::GetInstance()->NotifyUserEvent(server_name,
			recipient_user_id, event_type, message_id, friend_request_id);
		if (result.grpc_code != grpc::StatusCode::OK ||
			result.app_error != ErrorCodes::Success) {
			std::cout << "NotifyUserEvent deferred to sync: event_type="
				<< event_type << " app_error=" << result.app_error << std::endl;
		}
	});
}

void LogicSystem::DealSyncMessage(std::shared_ptr<CSession> session,
	const short&, const string& msg_data) {
	auto root = json::parse(msg_data, nullptr, false);
	auto reject = [&session](ErrorCodes code) {
		json rsp;
		rsp["error"] = code;
		session->Send(rsp.dump(), ID_SYNC_USER_MESSAGE_RSP);
	};
	if (!root.is_object()) {
		reject(ErrorCodes::Error_Json);
		return;
	}
	std::uint64_t after_event_seq = 0;
	if (root.contains("after_event_seq")) {
		if (!root["after_event_seq"].is_string()) {
			reject(ErrorCodes::Error_Json);
			return;
		}
		const std::string value = root["after_event_seq"].get<std::string>();
		try {
			if (value.empty() || !std::all_of(value.begin(), value.end(),
				[](unsigned char c) { return std::isdigit(c) != 0; })) {
				throw std::invalid_argument("event_seq");
			}
			std::size_t consumed = 0;
			after_event_seq = std::stoull(value, &consumed);
			if (consumed != value.size()) throw std::invalid_argument("event_seq");
		} catch (...) {
			reject(ErrorCodes::Error_Json);
			return;
		}
	}
	int limit = 100;
	if (root.contains("limit") && !ParseJsonInt(root["limit"], limit)) {
		reject(ErrorCodes::Error_Json);
		return;
	}
	if (limit < 1) limit = 1;
	if (limit > 200) limit = 200;

	std::uint64_t head = 0;
	if (!MysqlMgr::GetInstance()->GetLastEventSeq(session->GetUserId(), head)) {
		reject(ErrorCodes::MESSAGE_STORE_FAILED);
		return;
	}
	if (after_event_seq > head) {
		reject(ErrorCodes::SyncCursorInvalid);
		return;
	}
	std::vector<UserEvent> rows;
	if (!MysqlMgr::GetInstance()->GetEventsAfterSeq(
		session->GetUserId(), after_event_seq, limit, rows)) {
		reject(ErrorCodes::MESSAGE_STORE_FAILED);
		return;
	}
	const bool has_more = static_cast<int>(rows.size()) > limit;
	if (has_more) rows.pop_back();
	json rsp;
	rsp["error"] = ErrorCodes::Success;
	rsp["events"] = json::array();
	std::uint64_t next_event_seq = after_event_seq;
	for (const auto& row : rows) {
		rsp["events"].push_back(BuildUserEventEnvelope(row, session->GetUserId()));
		next_event_seq = row.event_seq;
	}
	rsp["next_event_seq"] = std::to_string(next_event_seq);
	rsp["has_more"] = has_more;
	session->Send(rsp.dump(), ID_SYNC_USER_MESSAGE_RSP);
}

