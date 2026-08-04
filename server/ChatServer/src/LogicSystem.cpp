#include "LogicSystem.h"
#include "MysqlMgr.h"
#include "const.h"
#include "RedisMgr.h"
#include "UserMgr.h"
#include "ChatGrpcClient.h"
#include "DistLock.h"
#include <string>
#include "CServer.h"
#include "ConfigMgr.h"
#include "utils.h"
#include <vector>
#include <set>
#include <algorithm>

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

/// 从 [Delivery] 读取整数配置；非法/缺失时回退 fallback（计划4.2/5.3）
int ReadDeliveryInt(const std::string& key, int fallback) {
	try {
		auto val = ConfigMgr::Inst().GetValue("Delivery", key);
		if (!val.empty()) {
			std::size_t pos = 0;
			int n = std::stoi(val, &pos);
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
} // namespace

LogicSystem::LogicSystem() : _p_server(nullptr) {
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

void LogicSystem::SetServer(std::shared_ptr<CServer> pserver) {
	_p_server = pserver;
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
	short msg_id = msg->_recvnode->_msg_id;
	std::string msg_data(msg->_recvnode->_data, msg->_recvnode->_cur_len);
	auto session = msg->_session;

	cout << "recv_msg id  is " << msg_id << endl;

	if (msg_id == MSG_CHAT_LOGIN) {
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
			short rsp_id = ReqToRspId(msg_id);
			if (rsp_id != 0) {
				session->Send(err.dump(4), rsp_id);
			}
			return;
		}
	}

	auto call_back_iter = _fun_callbacks.find(msg_id);
	if (call_back_iter == _fun_callbacks.end()) {
		std::cout << "msg id [" << msg_id << "] handler not found" << std::endl;
		return;
	}
	call_back_iter->second(session, msg_id, msg_data);
}

void LogicSystem::RegisterCallBacks() {
	_fun_callbacks[MSG_CHAT_LOGIN] = std::bind(&LogicSystem::LoginHandler, this,
		placeholders::_1, placeholders::_2, placeholders::_3);

	_fun_callbacks[ID_SEARCH_USER_REQ] = std::bind(&LogicSystem::SearchInfo, this,
		placeholders::_1, placeholders::_2, placeholders::_3);

	_fun_callbacks[ID_ADD_FRIEND_REQ] = std::bind(&LogicSystem::AddFriendApply, this,
		placeholders::_1, placeholders::_2, placeholders::_3);

	_fun_callbacks[ID_AUTH_FRIEND_REQ] = std::bind(&LogicSystem::AuthFriendApply, this,
		placeholders::_1, placeholders::_2, placeholders::_3);

	_fun_callbacks[ID_TEXT_CHAT_MSG_REQ] = std::bind(&LogicSystem::DealChatTextMsg, this,
		placeholders::_1, placeholders::_2, placeholders::_3);

	_fun_callbacks[ID_HEART_BEAT_REQ] = std::bind(&LogicSystem::HeartBeatHandler, this,
		placeholders::_1, placeholders::_2, placeholders::_3);

	_fun_callbacks[ID_LOAD_CHAT_THREAD_REQ] = std::bind(&LogicSystem::GetUserThreadsHandler, this,
		placeholders::_1, placeholders::_2, placeholders::_3);
	
	_fun_callbacks[ID_CREATE_PRIVATE_CHAT_REQ] = std::bind(&LogicSystem::CreatePrivateChat, this,
		placeholders::_1, placeholders::_2, placeholders::_3);

	_fun_callbacks[ID_LOAD_CHAT_MSG_REQ] = std::bind(&LogicSystem::LoadChatMsg, this,
		placeholders::_1, placeholders::_2, placeholders::_3);

	_fun_callbacks[ID_IMG_CHAT_MSG_REQ] = std::bind(&LogicSystem::DealChatImgMsg, this,
		placeholders::_1, placeholders::_2, placeholders::_3);

	_fun_callbacks[ID_CHAT_DELIVERY_ACK_REQ] = std::bind(&LogicSystem::DealDeliveryAck, this,
		placeholders::_1, placeholders::_2, placeholders::_3);

	_fun_callbacks[ID_PULL_OFFLINE_MSG_REQ] = std::bind(&LogicSystem::PullOfflineMsg, this,
		placeholders::_1, placeholders::_2, placeholders::_3);

}

void LogicSystem::LoginHandler(shared_ptr<CSession> session, const short &msg_id, const string &msg_data) {
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
	rtvalue["name"] = user_info->name;
	rtvalue["email"] = user_info->email;
	rtvalue["nick"] = user_info->nick;
	rtvalue["desc"] = user_info->desc;
	rtvalue["sex"] = user_info->sex;
	rtvalue["icon"] = user_info->icon;

	//从数据库获取申请列表
	std::vector<std::shared_ptr<ApplyInfo>> apply_list;
	auto b_apply = GetFriendApplyInfo(uid, apply_list);
	if (b_apply) {
		for (auto& apply : apply_list) {
			json obj;
			obj["name"] = apply->_name;
			obj["uid"] = apply->_uid;
			obj["icon"] = apply->_icon;
			obj["nick"] = apply->_nick;
			obj["sex"] = apply->_sex;
			obj["desc"] = apply->_desc;
			obj["status"] = apply->_status;
			rtvalue["apply_list"].push_back(obj);
		}
	}

	//获取好友列表
	std::vector<std::shared_ptr<UserInfo>> friend_list;
	bool b_friend_list = GetFriendList(uid, friend_list);
	for (auto& friend_ele : friend_list) {
		json obj;
		obj["name"] = friend_ele->name;
		obj["uid"] = friend_ele->uid;
		obj["icon"] = friend_ele->icon;
		obj["nick"] = friend_ele->nick;
		obj["sex"] = friend_ele->sex;
		obj["desc"] = friend_ele->desc;
		obj["back"] = friend_ele->back;
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
					_p_server->ClearSession(old_session->GetSessionId());
				}
			}
			else {
				//旧登录在其它服务器：经 gRPC 通知踢人
				KickUserReq kick_req;
				kick_req.set_uid(uid);
				ChatGrpcClient::GetInstance()->NotifyKickUser(uid_ip_value, kick_req);
			}
		}

		//session 绑定用户 uid
		session->SetUserId(uid);
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

void LogicSystem::SearchInfo(std::shared_ptr<CSession> session, const short& msg_id, const string& msg_data)
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

void LogicSystem::AddFriendApply(std::shared_ptr<CSession> session, const short& msg_id, const string& msg_data)
{
	auto root = json::parse(msg_data, nullptr, false);
	auto uid = root["uid"].get<int>();
	auto desc = root["applyname"].get<std::string>();
	auto bakname = root["bakname"].get<std::string>();
	auto touid = root["touid"].get<int>();
	std::cout << "user login uid is  " << uid << " applydesc  is "
		<< desc << " bakname is " << bakname << " touid is " << touid << endl;

	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	Defer defer([this, &rtvalue, session]() {
		std::string return_str = rtvalue.dump(4);
		session->Send(return_str, ID_ADD_FRIEND_RSP);
		});

	//先更新数据库
	MysqlMgr::GetInstance()->AddFriendApply(uid, touid, desc, bakname);

	//查询redis 查找touid对应的server ip
	auto to_str = std::to_string(touid);
	auto to_ip_key = USERIPPREFIX + to_str;
	std::string to_ip_value = "";
	bool b_ip = RedisMgr::GetInstance()->Get(to_ip_key, to_ip_value);
	if (!b_ip) {
		return;
	}


	auto& cfg = ConfigMgr::Inst();
	auto self_name = cfg["SelfServer"]["Name"];


	std::string base_key = USER_BASE_INFO + std::to_string(uid);
	auto apply_info = std::make_shared<UserInfo>();
	bool b_info = GetBaseInfo(base_key, uid, apply_info);

	//直接通知对方有申请消息
	if (to_ip_value == self_name) {
		//计划1.5：本机 recipient 分支也纳入其 uid 分片，闭包内重新查 session 存在才发送
		json  notify;
		notify["error"] = ErrorCodes::Success;
		notify["applyuid"] = uid;
		notify["name"] = apply_info->name;
		notify["desc"] = desc;
		if (b_info) {
			notify["icon"] = apply_info->icon;
			notify["sex"] = apply_info->sex;
			notify["nick"] = apply_info->nick;
		}
		std::string return_str = notify.dump(4);
		PostToUser(touid, [touid, return_str]() {
			auto session = UserMgr::GetInstance()->GetSession(touid);
			if (session) {
				session->Send(return_str, ID_NOTIFY_ADD_FRIEND_REQ);
			}
		});

		return ;
	}

	
	AddFriendReq add_req;
	add_req.set_applyuid(uid);
	add_req.set_touid(touid);
	add_req.set_name(apply_info->name);
	add_req.set_desc(desc);
	if (b_info) {
		add_req.set_icon(apply_info->icon);
		add_req.set_sex(apply_info->sex);
		add_req.set_nick(apply_info->nick);
	}

	//发送通知
	ChatGrpcClient::GetInstance()->NotifyAddFriend(to_ip_value,add_req);

}

void LogicSystem::AuthFriendApply(std::shared_ptr<CSession> session, const short& msg_id, const string& msg_data) {
	
	auto root = json::parse(msg_data, nullptr, false);

	auto uid = root["fromuid"].get<int>();
	auto touid = root["touid"].get<int>();
	auto back_name = root["back"].get<std::string>();
	std::cout << "from " << uid << " auth friend to " << touid << std::endl;

	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	auto user_info = std::make_shared<UserInfo>();

	std::string base_key = USER_BASE_INFO + std::to_string(touid);
	bool b_info = GetBaseInfo(base_key, touid, user_info);
	if (b_info) {
		rtvalue["name"] = user_info->name;
		rtvalue["nick"] = user_info->nick;
		rtvalue["icon"] = user_info->icon;
		rtvalue["sex"] = user_info->sex;
		rtvalue["uid"] = touid;
	}
	else {
		rtvalue["error"] = ErrorCodes::UidInvalid;
	}


	Defer defer([this, &rtvalue, session]() {
		std::string return_str = rtvalue.dump(4);
		session->Send(return_str, ID_AUTH_FRIEND_RSP);
		});

	//先更新数据库， 放到事务中，此处不再处理
	//MysqlMgr::GetInstance()->AuthFriendApply(uid, touid);

	std::vector<std::shared_ptr<AddFriendMsg>> chat_datas;

	//更新数据库添加好友
	MysqlMgr::GetInstance()->AddFriend(uid, touid,back_name, chat_datas);

	//查询redis 查找touid对应的server ip
	auto to_str = std::to_string(touid);
	auto to_ip_key = USERIPPREFIX + to_str;
	std::string to_ip_value = "";
	bool b_ip = RedisMgr::GetInstance()->Get(to_ip_key, to_ip_value);
	if (!b_ip) {
		return;
	}

	auto& cfg = ConfigMgr::Inst();
	auto self_name = cfg["SelfServer"]["Name"];
	//直接通知对方有认证通过消息
	if (to_ip_value == self_name) {
		//计划1.5：本机 recipient 分支也纳入其 uid 分片，闭包内重新查 session 存在才发送
		json  notify;
		notify["error"] = ErrorCodes::Success;
		notify["fromuid"] = uid;
		notify["touid"] = touid;
		std::string base_key = USER_BASE_INFO + std::to_string(uid);
		auto user_info = std::make_shared<UserInfo>();
		bool b_info = GetBaseInfo(base_key, uid, user_info);
		if (b_info) {
			notify["name"] = user_info->name;
			notify["nick"] = user_info->nick;
			notify["icon"] = user_info->icon;
			notify["sex"] = user_info->sex;
		}
		else {
			notify["error"] = ErrorCodes::UidInvalid;
		}

		auto chat_time = getCurrentTimestamp();
		for(auto & chat_data : chat_datas)
		{
			json  chat;
			chat["sender"] = chat_data->sender_id();
			chat["msg_id"] = chat_data->msg_id();
			chat["thread_id"] = chat_data->thread_id();
			chat["unique_id"] = chat_data->unique_id();
			chat["msg_content"] = chat_data->msgcontent();
			chat["chat_time"] = chat_time;
			chat["status"] = chat_data->status();
			notify["chat_datas"].push_back(chat);
			rtvalue["chat_datas"].push_back(chat);
		}

		std::string return_str = notify.dump(4);
		PostToUser(touid, [touid, return_str]() {
			auto session = UserMgr::GetInstance()->GetSession(touid);
			if (session) {
				session->Send(return_str, ID_NOTIFY_AUTH_FRIEND_REQ);
			}
		});

		return ;
	}


	AuthFriendReq auth_req;
	auth_req.set_fromuid(uid);
	auth_req.set_touid(touid);
	auto chat_time = getCurrentTimestamp();
	for(auto& chat_data : chat_datas)
	{
		auto text_msg = auth_req.add_textmsgs();
		text_msg->CopyFrom(*chat_data);
		json  chat;
		chat["sender"] = chat_data->sender_id();
		chat["msg_id"] = chat_data->msg_id();
		chat["thread_id"] = chat_data->thread_id();
		chat["unique_id"] = chat_data->unique_id();
		chat["msg_content"] = chat_data->msgcontent();
		chat["chat_time"] = chat_time;
		chat["status"] = chat_data->status();
		rtvalue["chat_datas"].push_back(chat);
	}
	//发送通知
	ChatGrpcClient::GetInstance()->NotifyAuthFriend(to_ip_value, auth_req);
}

void LogicSystem::DealChatTextMsg(std::shared_ptr<CSession> session, const short& msg_id, const string& msg_data) {
	auto root = json::parse(msg_data, nullptr, false);

	auto uid = root["fromuid"].get<int>();
	auto touid = root["touid"].get<int>();
	auto thread_id = root["thread_id"].get<int>();

	const json  arrays = root["text_array"];

	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["fromuid"] = uid;
	rtvalue["touid"] = touid;
	rtvalue["thread_id"] = thread_id;

	//逐条构造 ChatMessage：status 统一 UN_READ，content_size 固定 0（计划5.4/3.2）
	auto timestamp = getCurrentTimestamp();
	std::vector<std::shared_ptr<ChatMessage>> chat_datas;
	for (const auto& txt_obj : arrays) {
		auto content = txt_obj["content"].get<std::string>();
		auto unique_id = txt_obj["unique_id"].get<std::string>();
		auto chat_msg = std::make_shared<ChatMessage>();
		chat_msg->chat_time = timestamp;
		chat_msg->sender_id = uid;
		chat_msg->recv_id = touid;
		chat_msg->unique_id = unique_id;
		chat_msg->thread_id = thread_id;
		chat_msg->content = content;
		chat_msg->status = MsgStatus::UN_READ;
		chat_msg->msg_type = static_cast<int>(ChatMsgType::TEXT);
		chat_msg->content_size = 0;
		chat_datas.push_back(chat_msg);
	}

	//插入数据库（同一事务；任一 conflict/SQL 错误回滚本批新行）。DAO 在 commit 后回写
	//canonical message_id/status/chat_time/delivery_status（计划3.3）
	std::vector<std::string> conflict_unique_ids;
	auto save_res = MysqlMgr::GetInstance()->AddChatMsg(chat_datas, conflict_unique_ids);
	if (save_res == SaveMessageResult::Failed) {
		//持久化失败：不 ACK、不转发，sender 按 transient 重传（计划3.5/5.4）
		rtvalue["error"] = ErrorCodes::MESSAGE_STORE_FAILED;
		session->Send(rtvalue.dump(4), ID_TEXT_CHAT_MSG_RSP);
		return;
	}
	if (save_res == SaveMessageResult::Conflict) {
		//永久冲突：原消息不变，sender 停止重传对应 unique_id（计划3.5/5.4）
		rtvalue["error"] = ErrorCodes::MESSAGE_CONFLICT;
		rtvalue["conflict_unique_ids"] = conflict_unique_ids;
		session->Send(rtvalue.dump(4), ID_TEXT_CHAT_MSG_RSP);
		return;
	}

	//Stored/Duplicate：用 canonical 持久值构造相同 sender response。每项为统一十字段
	//envelope（兼容旧 message_id/unique_id/content/status/chat_time 字段）（计划5.4/5.3）
	for (const auto& chat_data : chat_datas) {
		rtvalue["chat_datas"].push_back(BuildMessageEnvelope(chat_data));
	}

	//【关键顺序】事务已提交 → 先发 1018（语义固定为“服务端已持久化”），再做 pending
	//激活与 live delivery。不得用 Defer 延后：那会让 Redis/live 先于 sender ACK（计划5.4）
	std::string sender_rsp = rtvalue.dump(4);
	session->Send(sender_rsp, ID_TEXT_CHAT_MSG_RSP);

	//仅 canonical delivery_status==Pending 的行才写 pending ZSET 并尝试 live delivery。
	//已 ACK 的 duplicate 只重发上面的 sender response，绝不重新打开 pending（计划5.4）
	std::vector<std::shared_ptr<ChatMessage>> pending_rows;
	for (const auto& chat_data : chat_datas) {
		if (chat_data->delivery_status == DeliveryStatus::Pending) {
			pending_rows.push_back(chat_data);
		}
	}
	if (pending_rows.empty()) {
		return;
	}

	//pending 激活：ZAdd offline_msg:<recv_uid>（score=member=十进制 message_id，天然幂等
	//且按 DB ID 排序）+Expire。Redis 失败只日志，不否定 sender response；后续 pull 由
	//MySQL 兜底（计划4.1/4.2/5.4）
	int ttl = ReadDeliveryInt("OfflineTtlSeconds", 604800);
	if (ttl < 1) ttl = 604800;
	std::string zkey = OFFLINE_MSG_PREFIX + std::to_string(touid);
	auto redis = RedisMgr::GetInstance();
	for (const auto& chat_data : pending_rows) {
		std::string mid = std::to_string(chat_data->message_id);
		if (!redis->ZAdd(zkey, chat_data->message_id, mid)) {
			std::cout << "DealChatTextMsg ZAdd failed, key=" << zkey
				<< " message_id=" << chat_data->message_id << std::endl;
		}
	}
	if (!redis->Expire(zkey, ttl)) {
		std::cout << "DealChatTextMsg Expire failed, key=" << zkey << std::endl;
	}

	//live delivery：查询 touid 路由。pending duplicate 可能再次 live-push，这是
	//at-least-once 允许的行为，recipient 以 message_id 去重（计划5.4）
	auto to_str = std::to_string(touid);
	auto to_ip_key = USERIPPREFIX + to_str;
	std::string to_ip_value = "";
	bool b_ip = RedisMgr::GetInstance()->Get(to_ip_key, to_ip_value);
	if (!b_ip) {
		return; //目标不在任何节点，pending 已建立，等 receiver 登录 pull
	}

	auto& cfg = ConfigMgr::Inst();
	auto self_name = cfg["SelfServer"]["Name"];
	if (to_ip_value == self_name) {
		//本机 recipient：构造统一十字段 live envelope，纳入 recipient uid 分片（计划1.5/5.5）
		json notify;
		notify["error"] = ErrorCodes::Success;
		notify["fromuid"] = uid;
		notify["touid"] = touid;
		notify["thread_id"] = thread_id;
		for (const auto& chat_data : pending_rows) {
			notify["chat_datas"].push_back(BuildMessageEnvelope(chat_data));
		}
		std::string notify_str = notify.dump(4);
		PostToUser(touid, [touid, notify_str]() {
			auto session = UserMgr::GetInstance()->GetSession(touid);
			if (session) {
				session->Send(notify_str, ID_NOTIFY_TEXT_CHAT_MSG_REQ);
			}
		});
		return;
	}

	//远端：TextChatMsgReq 携带 unique_id/msg_id/content/chat_time。投递经 sender uid
	//固定的 delivery shard，避免 3s deadline 阻塞 logic shard，且同一 sender 的多次跨服
	//调用在 delivery worker 上保持顺序（计划5.6）。带重试的 NotifyTextChatMsg 内部按
	//[Delivery] 配置 deadline/最多尝试次数/退避，重试耗尽只日志：pending 已在 RPC 前
	//建立，receiver 后续 pull 兜底；不撤销 sender ACK、不改 DB。
	TextChatMsgReq text_msg_req;
	text_msg_req.set_fromuid(uid);
	text_msg_req.set_touid(touid);
	text_msg_req.set_thread_id(thread_id);
	for (const auto& chat_data : pending_rows) {
		auto* text_msg = text_msg_req.add_textmsgs();
		text_msg->set_unique_id(chat_data->unique_id);
		text_msg->set_msgcontent(chat_data->content);
		text_msg->set_msg_id(chat_data->message_id);
		text_msg->set_chat_time(chat_data->chat_time);
	}

	//拷贝 server_ip / proto req 进闭包（按值捕获），闭包在 sender 固定的 delivery worker 上执行
	std::string server_ip = to_ip_value;
	auto posted = PostDelivery(uid, [server_ip, text_msg_req]() {
		auto res = ChatGrpcClient::GetInstance()->NotifyTextChatMsg(server_ip, text_msg_req);
		if (res.grpc_code == grpc::StatusCode::OK && res.app_error == ErrorCodes::Success) {
			std::cout << "DealChatTextMsg cross-server delivered, server=" << server_ip << std::endl;
		}
		else {
			//重试耗尽 / 不可重试 / 未知 server：只日志。pending 已在 RPC 前建立，receiver 后续 pull 兜底（计划5.6）
			std::cout << "DealChatTextMsg cross-server delivery final result server=" << server_ip
				<< " grpc_code=" << static_cast<int>(res.grpc_code)
				<< " app_error=" << res.app_error
				<< " (pending already established, receiver will pull)" << std::endl;
		}
	});
	if (!posted) {
		//停机：delivery worker 已拒绝投递。pending 已建立，receiver 后续 pull 兜底（计划5.6）
		std::cout << "DealChatTextMsg PostDelivery rejected (stopping), server=" << server_ip
			<< " (pending already established, receiver will pull)" << std::endl;
	}
}

void LogicSystem::HeartBeatHandler(std::shared_ptr<CSession> session, const short& msg_id, const string& msg_data) {
	auto root = json::parse(msg_data, nullptr, false);
	auto uid = root["fromuid"].get<int>();
	std::cout << "receive heart beat msg, uid is " << uid << std::endl;
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
	redis_root["uid"] = user_info->uid;
	redis_root["name"] = user_info->name;
	redis_root["email"] = user_info->email;
	redis_root["nick"] = user_info->nick;
	redis_root["desc"] = user_info->desc;
	redis_root["sex"] = user_info->sex;
	redis_root["icon"] = user_info->icon;

	RedisMgr::GetInstance()->Set(base_key, redis_root.dump(4));

	//返回数据
	rtvalue["uid"] = user_info->uid;
	rtvalue["name"] = user_info->name;
	rtvalue["email"] = user_info->email;
	rtvalue["nick"] = user_info->nick;
	rtvalue["desc"] = user_info->desc;
	rtvalue["sex"] = user_info->sex;
	rtvalue["icon"] = user_info->icon;
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
	redis_root["uid"] = user_info->uid;
	redis_root["name"] = user_info->name;
	redis_root["email"] = user_info->email;
	redis_root["nick"] = user_info->nick;
	redis_root["desc"] = user_info->desc;
	redis_root["sex"] = user_info->sex;
	redis_root["icon"] = user_info->icon;

	RedisMgr::GetInstance()->Set(base_key, redis_root.dump(4));
	
	//返回数据
	rtvalue["uid"] = user_info->uid;
	rtvalue["name"] = user_info->name;
	rtvalue["email"] = user_info->email;
	rtvalue["nick"] = user_info->nick;
	rtvalue["desc"] = user_info->desc;
	rtvalue["sex"] = user_info->sex;
	rtvalue["icon"] = user_info->icon;
}

bool LogicSystem::GetBaseInfo(std::string base_key, int uid, std::shared_ptr<UserInfo>& userinfo)
{
	//优先查redis中查询用户信息
	std::string info_str = "";
	bool b_base = RedisMgr::GetInstance()->Get(base_key, info_str);
	if (b_base) {
		auto root = json::parse(info_str, nullptr, false);
		userinfo->uid = root["uid"].get<int>();
		userinfo->name = root["name"].get<std::string>();
		userinfo->email = root["email"].get<std::string>();
		userinfo->nick = root["nick"].get<std::string>();
		userinfo->desc = root["desc"].get<std::string>();
		userinfo->sex = root["sex"].get<int>();
		userinfo->icon = root["icon"].get<std::string>();
		std::cout << "user login uid is  " << userinfo->uid << " name  is "
			<< userinfo->name << " email is " << userinfo->email << endl;
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
		redis_root["name"] = userinfo->name;
		redis_root["email"] = userinfo->email;
		redis_root["nick"] = userinfo->nick;
		redis_root["desc"] = userinfo->desc;
		redis_root["sex"] = userinfo->sex;
		redis_root["icon"] = userinfo->icon;
		RedisMgr::GetInstance()->Set(base_key, redis_root.dump(4));
	}

	return true;
}

bool LogicSystem::GetFriendApplyInfo(int to_uid, std::vector<std::shared_ptr<ApplyInfo>> &list) {
	//从mysql获取好友申请列表
	return MysqlMgr::GetInstance()->GetApplyList(to_uid, list, 0, 10);
}

bool LogicSystem::GetFriendList(int self_id, std::vector<std::shared_ptr<UserInfo>>& user_list) {
	//从mysql获取好友列表
	return MysqlMgr::GetInstance()->GetFriendList(self_id, user_list);
}

void LogicSystem::GetUserThreadsHandler(std::shared_ptr<CSession> session, 
	const short& msg_id, const string& msg_data)
{
	//从数据库加chat_threads记录
	auto root = json::parse(msg_data, nullptr, false);
	auto uid = root["uid"].get<int>();
	int last_id = root["thread_id"].get<int>();
	std::cout << "get uid  threads  " << uid << std::endl;

	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["uid"] = uid;
	Defer defer([this, &rtvalue, session]() {
		std::string return_str = rtvalue.dump(4);
		session->Send(return_str, ID_LOAD_CHAT_THREAD_RSP);
		});
	
	std::vector<std::shared_ptr<ChatThreadInfo>> threads;
	
	int page_size = 10;
	bool load_more = false;
	int next_last_id = 0;
	bool res = GetUserThreads(uid, last_id, page_size, threads, load_more, next_last_id);
	if (!res) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}


	rtvalue["load_more"] = load_more;
	rtvalue["next_last_id"] = (int)next_last_id;
	//整理threads数据写入json返回
	for (auto& thread : threads) {
		json thread_value;
		thread_value["thread_id"] = int(thread->_thread_id);
		thread_value["type"] = thread->_type;
		thread_value["user1_id"] = thread->_user1_id;
		thread_value["user2_id"] = thread->_user2_id;
		rtvalue["threads"].push_back(thread_value);
	}
}

bool LogicSystem::GetUserThreads(int64_t userId,
	int64_t lastId,
	int      pageSize,
	std::vector<std::shared_ptr<ChatThreadInfo>>& threads,
	bool& loadMore,
	int& nextLastId)
{
	return MysqlMgr::GetInstance()->GetUserThreads(userId, lastId, pageSize, 
		threads, loadMore, nextLastId);
}

void LogicSystem::CreatePrivateChat(std::shared_ptr<CSession> session, const short& msg_id, const string& msg_data)
{
	auto root = json::parse(msg_data, nullptr, false);
	auto uid = root["uid"].get<int>();
	auto other_id = root["other_id"].get<int>();
	
	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["uid"] = uid;
	rtvalue["other_id"] = other_id;

	Defer defer([this, &rtvalue, session]() {
		std::string return_str = rtvalue.dump(4);
		session->Send(return_str, ID_CREATE_PRIVATE_CHAT_RSP);
		});

	int thread_id = 0;
	bool res = MysqlMgr::GetInstance()->CreatePrivateChat(uid, other_id, thread_id);
	if (!res) {
		rtvalue["error"] = ErrorCodes::CREATE_CHAT_FAILED;
		return;
	}

	rtvalue["thread_id"] = thread_id;
}

void LogicSystem::LoadChatMsg(std::shared_ptr<CSession> session, 
	const short& msg_id, const string& msg_data) {

	auto root = json::parse(msg_data, nullptr, false);
	auto thread_id = root["thread_id"].get<int>();
	auto message_id = root["message_id"].get<int>();


	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["thread_id"] = thread_id;

	Defer defer([this, &rtvalue, session]() {
		std::string return_str = rtvalue.dump(4);
		session->Send(return_str, ID_LOAD_CHAT_MSG_RSP);
		});

	int page_size = 10;
	std::shared_ptr<PageResult> res = MysqlMgr::GetInstance()->LoadChatMsg(thread_id, message_id, page_size);
	if (!res) {
		rtvalue["error"] = ErrorCodes::LOAD_CHAT_FAILED;
		return;
	}

	rtvalue["last_message_id"] = res->next_cursor;
	rtvalue["load_more"] = res->load_more;
	for (auto& chat : res->messages) {
		json  chat_data;
		chat_data["sender"] = chat.sender_id;
		chat_data["msg_id"] = chat.message_id;
		chat_data["thread_id"] = chat.thread_id;
		chat_data["unique_id"] = 0;
		chat_data["msg_content"] = chat.content;
		chat_data["chat_time"] = chat.chat_time;
		chat_data["status"] = chat.status;
		chat_data["msg_type"] = chat.msg_type;
		chat_data["receiver"] = chat.recv_id;
		rtvalue["chat_datas"].push_back(chat_data);
	}

}

void LogicSystem::DealChatImgMsg(std::shared_ptr<CSession> session,
	const short& msg_id, const string& msg_data) {
	auto root = json::parse(msg_data, nullptr, false);

	auto uid = root["fromuid"].get<int>();
	auto touid = root["touid"].get<int>();
	auto thread_id = root["thread_id"].get<int>();

	auto md5 = root["md5"].get<std::string>();
	auto unique_name = root["name"].get<std::string>();
	auto unique_id = root["unique_id"].get<std::string>();

	//content_size：JSON 十进制字符串（兼容当前整数）；非法一律 0（计划5.4/6.3）
	std::uint64_t content_size = 0;
	if (root.contains("content_size")) {
		const auto& cs = root["content_size"];
		if (cs.is_string()) {
			try { content_size = std::stoull(cs.get<std::string>()); }
			catch (...) { content_size = 0; }
		}
		else if (cs.is_number()) {
			content_size = cs.get<std::uint64_t>();
		}
	}

	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["fromuid"] = uid;
	rtvalue["touid"] = touid;
	rtvalue["thread_id"] = thread_id;
	rtvalue["md5"] = md5;
	rtvalue["unique_name"] = unique_name;
	rtvalue["unique_id"] = unique_id;

	//服务端生成 chat_time/status=UN_UPLOAD，不读取 client 的 chat_time/status（计划5.4/6.3）
	auto timestamp = getCurrentTimestamp();
	rtvalue["chat_time"] = timestamp;
	rtvalue["status"] = MsgStatus::UN_UPLOAD;

	auto chat_msg = std::make_shared<ChatMessage>();
	chat_msg->chat_time = timestamp;
	chat_msg->sender_id = uid;
	chat_msg->recv_id = touid;
	chat_msg->unique_id = unique_id;
	chat_msg->thread_id = thread_id;
	chat_msg->content = unique_name;
	chat_msg->status = MsgStatus::UN_UPLOAD;
	chat_msg->msg_type = static_cast<int>(ChatMsgType::PIC);
	chat_msg->content_size = content_size;

	//插入数据库：duplicate 回同一 canonical message_id/unique_id，不建第二行（计划5.4）
	auto save_res = MysqlMgr::GetInstance()->AddChatMsg(chat_msg);
	if (save_res == SaveMessageResult::Failed) {
		//持久化失败：不 ACK、不转发，sender 按 transient 重传
		rtvalue["error"] = ErrorCodes::MESSAGE_STORE_FAILED;
		session->Send(rtvalue.dump(4), ID_IMG_CHAT_MSG_RSP);
		return;
	}
	if (save_res == SaveMessageResult::Conflict) {
		//永久冲突：原消息不变，不创建第二行
		rtvalue["error"] = ErrorCodes::MESSAGE_CONFLICT;
		rtvalue["conflict_unique_ids"] = std::vector<std::string>{chat_msg->unique_id};
		session->Send(rtvalue.dump(4), ID_IMG_CHAT_MSG_RSP);
		return;
	}

	//canonical：message_id 由 DAO 回写；content_size 十进制字符串（计划5.4/6.3）
	rtvalue["message_id"] = chat_msg->message_id;
	rtvalue["content_size"] = std::to_string(chat_msg->content_size);

	//【关键顺序】事务已提交 → 发 1036 sender response（语义固定为“服务端已持久化”）（计划5.4）
	session->Send(rtvalue.dump(4), ID_IMG_CHAT_MSG_RSP);

	//UN_UPLOAD 图片绝不 ZADD/实时通知，待 §5.7 上传完成点（UpdateUploadStatus 成功后）
	//才激活 pending，避免拉取尚不可下载的图片（计划5.4/4.2）
}

json LogicSystem::BuildMessageEnvelope(const std::shared_ptr<ChatMessage>& msg) {
	json env;
	env["message_id"] = msg->message_id;
	env["unique_id"] = msg->unique_id;
	env["thread_id"] = msg->thread_id;
	env["fromuid"] = msg->sender_id;
	env["touid"] = msg->recv_id;
	env["msg_type"] = msg->msg_type;
	env["content"] = msg->content;
	//content_size 一律十进制字符串，避免 Qt JSON number 对 64 位文件大小丢精度（计划5.3）
	env["content_size"] = std::to_string(msg->content_size);
	env["chat_time"] = msg->chat_time;
	env["status"] = msg->status;
	return env;
}

void LogicSystem::DealDeliveryAck(std::shared_ptr<CSession> session, const short& msg_id, const string& msg_data) {
	//1049 {"uid":<receiver>,"message_ids":[...]} -> 1050 {"error":0,"message_ids":[...]}
	//严格校验：JSON 对象、uid 正整数且 == session->GetUserId()、message_ids 非空数组且每项正 int
	auto root = json::parse(msg_data, nullptr, false);

	auto reject = [&session](ErrorCodes code) {
		json rsp;
		rsp["error"] = code;
		session->Send(rsp.dump(4), ID_CHAT_DELIVERY_ACK_RSP);
	};

	if (!root.is_object()) {
		reject(ErrorCodes::Error_Json);
		return;
	}
	if (!root.contains("uid") || !root["uid"].is_number_integer()) {
		reject(ErrorCodes::Error_Json);
		return;
	}
	int uid = root["uid"].get<int>();
	//uid 必须等于本连接已认证用户，否则防越权
	if (uid != session->GetUserId()) {
		reject(ErrorCodes::UidInvalid);
		return;
	}
	if (!root.contains("message_ids") || !root["message_ids"].is_array()) {
		reject(ErrorCodes::Error_Json);
		return;
	}
	const auto& ids_json = root["message_ids"];
	if (ids_json.empty()) {
		reject(ErrorCodes::Error_Json);
		return;
	}
	//每项必须正 int，用 std::set 去重并升序
	std::set<int> id_set;
	for (const auto& e : ids_json) {
		if (!e.is_number_integer()) {
			reject(ErrorCodes::Error_Json);
			return;
		}
		int v = e.get<int>();
		if (v <= 0) {
			reject(ErrorCodes::Error_Json);
			return;
		}
		id_set.insert(v);
	}
	std::vector<int> ids(id_set.begin(), id_set.end());

	//GetMessagesByIds 带 recv_id 防越权：返回行数必须与请求完全吻合，未知/不属于本 receiver 的 id 不会被返回
	auto msgs = MysqlMgr::GetInstance()->GetMessagesByIds(uid, ids);
	if (msgs.size() != ids.size()) {
		reject(ErrorCodes::Error_Json);
		return;
	}

	//只有 DB 更新成功后才回 ACK response 并清 Redis（计划4.3/5.2）
	bool ok = MysqlMgr::GetInstance()->MarkMessagesDelivered(uid, ids);
	if (!ok) {
		reject(ErrorCodes::MESSAGE_STORE_FAILED);
		return;
	}

	//先回 Success + 原（去重升序）ids，让客户端尽快确认；重复 ACK 因 DAO 幂等仍 success
	json rsp;
	rsp["error"] = ErrorCodes::Success;
	json ids_arr = json::array();
	for (int id : ids) {
		ids_arr.push_back(id);
	}
	rsp["message_ids"] = ids_arr;
	session->Send(rsp.dump(4), ID_CHAT_DELIVERY_ACK_RSP);

	//DB 成功后逐个 ZREM offline_msg:<uid>；Redis 删除失败只记录，不影响 success
	std::string zkey = OFFLINE_MSG_PREFIX + std::to_string(uid);
	auto redis = RedisMgr::GetInstance();
	for (int id : ids) {
		if (!redis->ZRem(zkey, std::to_string(id))) {
			std::cout << "ACK ZRem failed, key=" << zkey << " id=" << id << std::endl;
		}
	}
}

void LogicSystem::PullOfflineMsg(std::shared_ptr<CSession> session, const short& msg_id, const string& msg_data) {
	//1051 {"uid":<receiver>,"after_message_id":<id>,"limit":<n>} -> 1052 {"error":0,"messages":[...],"next_message_id":<id>,"has_more":<bool>}
	//配置：非法一律回退默认（计划4.2/5.3）
	int pull_batch = ReadDeliveryInt("OfflinePullBatch", 100);
	if (pull_batch < 1) pull_batch = 100;
	int pull_max_bytes = ReadDeliveryInt("PullMaxBytes", 30000);
	if (pull_max_bytes < 1) pull_max_bytes = 30000;
	int ttl = ReadDeliveryInt("OfflineTtlSeconds", 604800);
	if (ttl < 1) ttl = 604800;

	auto root = json::parse(msg_data, nullptr, false);

	auto reject = [&session](ErrorCodes code) {
		json rsp;
		rsp["error"] = code;
		session->Send(rsp.dump(), ID_PULL_OFFLINE_MSG_RSP);
	};

	if (!root.is_object()) {
		reject(ErrorCodes::Error_Json);
		return;
	}
	if (!root.contains("uid") || !root["uid"].is_number_integer()) {
		reject(ErrorCodes::Error_Json);
		return;
	}
	int uid = root["uid"].get<int>();
	//uid 必须匹配本连接已认证用户
	if (uid != session->GetUserId()) {
		reject(ErrorCodes::UidInvalid);
		return;
	}

	int after_message_id = 0;
	if (root.contains("after_message_id")) {
		if (!root["after_message_id"].is_number_integer()) {
			reject(ErrorCodes::Error_Json);
			return;
		}
		after_message_id = root["after_message_id"].get<int>();
		if (after_message_id < 0) {
			reject(ErrorCodes::Error_Json);
			return;
		}
	}

	//limit 缺失用配置 OfflinePullBatch；请求 limit clamp 1-100
	int limit = pull_batch;
	if (root.contains("limit")) {
		if (!root["limit"].is_number_integer()) {
			reject(ErrorCodes::Error_Json);
			return;
		}
		limit = root["limit"].get<int>();
	}
	if (limit < 1) limit = 1;
	if (limit > 100) limit = 100;

	std::string zkey = OFFLINE_MSG_PREFIX + std::to_string(uid);
	auto redis = RedisMgr::GetInstance();
	auto mysql = MysqlMgr::GetInstance();

	//1) Redis ZRangeByScore(cursor, limit+1) 得候选 IDs
	std::vector<std::string> redis_members;
	redis->ZRangeByScore(zkey, after_message_id, limit + 1, redis_members);
	std::vector<int> redis_id_ints;
	std::set<int> redis_all_ids;
	for (const auto& m : redis_members) {
		try {
			int v = std::stoi(m);
			redis_id_ints.push_back(v);
			redis_all_ids.insert(v);
		}
		catch (...) {
			//非数字 member 视为陈旧，直接清理
			redis->ZRem(zkey, m);
		}
	}

	//2) GetMessagesByIds(redis_ids)；Redis 命中行仅 delivery_status==Pending 且排除 PIC/UN_UPLOAD 才参与 pull；
	//   ACK/缺失/不可投递的陈旧 member 顺手 ZREM（best effort，失败不挤占 pending 页）
	std::vector<std::shared_ptr<ChatMessage>> redis_msgs;
	if (!redis_id_ints.empty()) {
		redis_msgs = mysql->GetMessagesByIds(uid, redis_id_ints);
	}
	std::map<int, std::shared_ptr<ChatMessage>> redis_by_id;
	for (auto& m : redis_msgs) {
		redis_by_id[m->message_id] = m;
	}
	std::vector<std::shared_ptr<ChatMessage>> deliverable_from_redis;
	for (int rid : redis_id_ints) {
		auto it = redis_by_id.find(rid);
		if (it == redis_by_id.end()) {
			//DB 已无此行（已删除）→ 陈旧 member，ZREM
			redis->ZRem(zkey, std::to_string(rid));
			continue;
		}
		auto& m = it->second;
		bool undeliverable = (m->delivery_status != DeliveryStatus::Pending)
			|| (m->msg_type == static_cast<int>(ChatMsgType::PIC) && m->status == MsgStatus::UN_UPLOAD);
		if (undeliverable) {
			redis->ZRem(zkey, std::to_string(rid));
			continue;
		}
		deliverable_from_redis.push_back(m);
	}

	//3) MySQL GetPendingMessages(uid, cursor, limit)：DAO 内部多取 1 条供 has_more
	//   MySQL 是完整真值：delivery_status=0 且排除 PIC/UN_UPLOAD
	auto mysql_msgs = mysql->GetPendingMessages(uid, after_message_id, limit);

	//4) 并集 deliverable_from_redis + mysql_msgs，按 message_id 去重升序
	std::map<int, std::shared_ptr<ChatMessage>> union_map;
	for (auto& m : deliverable_from_redis) {
		union_map[m->message_id] = m;
	}
	for (auto& m : mysql_msgs) {
		union_map[m->message_id] = m;
	}
	std::vector<std::shared_ptr<ChatMessage>> candidates;
	candidates.reserve(union_map.size());
	for (auto& kv : union_map) {
		candidates.push_back(kv.second);
	}

	//5) 缺失 Redis ID 的 DB pending → ZAdd+Expire 回填（失败不影响响应）
	bool backfilled = false;
	for (auto& m : mysql_msgs) {
		if (redis_all_ids.find(m->message_id) == redis_all_ids.end()) {
			std::string mid = std::to_string(m->message_id);
			if (redis->ZAdd(zkey, m->message_id, mid)) {
				backfilled = true;
			}
		}
	}
	if (backfilled) {
		redis->Expire(zkey, ttl);
	}

	//6) 序列化：以 dump() 后 UTF-8 byte 数为准受 PullMaxBytes 上限，条数 limit 也是上限。
	//   逐条构造候选 response；任何候选（含第一条）只要令 tentative response 超 PullMaxBytes
	//   就永不 append，byte_stopped=true 并 break。绝不“始终至少包含第一条以推进 cursor”——
	//   那会让单条超限消息越过上限、response 被 CSession::Send 的 short 长度截断后客户端卡死。
	json rsp;
	rsp["error"] = ErrorCodes::Success;
	rsp["messages"] = json::array();
	rsp["next_message_id"] = after_message_id;
	rsp["has_more"] = true;

	std::vector<int> included_ids;
	bool byte_stopped = false;
	int oversized_message_id = 0;
	std::size_t oversized_bytes = 0;
	for (auto& m : candidates) {
		if (static_cast<int>(included_ids.size()) >= limit) {
			break; //count limit reached
		}
		json env = BuildMessageEnvelope(m);
		//tentative 测量：has_more 取最长 true 以留余量
		json tent = rsp;
		tent["messages"].push_back(env);
		tent["next_message_id"] = m->message_id;
		tent["has_more"] = true;
		std::size_t tent_bytes = tent.dump().size();
		if (tent_bytes > static_cast<std::size_t>(pull_max_bytes)) {
			//加入本条会超限 → 永不 append（含第一条），记录后停止
			byte_stopped = true;
			oversized_message_id = m->message_id;
			oversized_bytes = tent_bytes;
			break;
		}
		//commit
		rsp["messages"].push_back(env);
		included_ids.push_back(m->message_id);
	}

	//病态情形：候选非空但首条即超 PullMaxBytes（included_ids 仍为空）。
	//不得推进 cursor 却不投递任何消息：返回显式非循环错误 RPCFailed，cursor 不动、不 ACK、has_more=false。
	if (included_ids.empty() && !candidates.empty()) {
		std::cout << "PullOfflineMsg oversized first message, uid=" << uid
			<< " message_id=" << oversized_message_id
			<< " bytes=" << oversized_bytes
			<< " pull_max_bytes=" << pull_max_bytes << std::endl;
		json err_rsp;
		err_rsp["error"] = ErrorCodes::RPCFailed;
		err_rsp["messages"] = json::array();
		err_rsp["next_message_id"] = after_message_id;
		err_rsp["has_more"] = false;
		session->Send(err_rsp.dump(), ID_PULL_OFFLINE_MSG_RSP);
		return;
	}

	//has_more 覆盖：byte 提前停止、count 上限、DB/Redis 多取 1 条后仍有剩余候选
	bool has_more = byte_stopped || (candidates.size() > included_ids.size());
	int next_message_id = included_ids.empty() ? after_message_id : included_ids.back();
	rsp["next_message_id"] = next_message_id;
	rsp["has_more"] = has_more;

	session->Send(rsp.dump(), ID_PULL_OFFLINE_MSG_RSP);
}

