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

/// 从 JSON 值解析 64 位 id：十进制字符串（协议现状）与数字（旧格式）都兼容，
/// 字符串必须整串消费；解析失败返回 false
bool ParseJsonId(const json& v, std::int64_t& out) {
	if (v.is_number_integer()) {
		out = v.get<std::int64_t>();
		return true;
	}
	if (v.is_string()) {
		try {
			auto s = v.get<std::string>();
			std::size_t pos = 0;
			long long n = std::stoll(s, &pos);
			if (pos == s.size()) {
				out = n;
				return true;
			}
		}
		catch (...) {
			//非法字符串，按解析失败处理
		}
	}
	return false;
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
	short msg_type = msg->_recvnode->_msg_type;
	std::string msg_data(msg->_recvnode->_data, msg->_recvnode->_cur_len);
	auto session = msg->_session;

	cout << "recv_msg type is " << msg_type << endl;

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
	call_back_iter->second(session, msg_type, msg_data);
}

void LogicSystem::RegisterCallBacks() {
	_fun_callbacks[MSG_CHAT_LOGIN] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
			LoginHandler(session, msg_type, msg_data);
		};

	_fun_callbacks[ID_SEARCH_USER_REQ] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
			SearchInfo(session, msg_type, msg_data);
		};

	_fun_callbacks[ID_ADD_FRIEND_REQ] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
			AddFriendApply(session, msg_type, msg_data);
		};

	_fun_callbacks[ID_AUTH_FRIEND_REQ] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
			AuthFriendApply(session, msg_type, msg_data);
		};

	_fun_callbacks[ID_TEXT_CHAT_MSG_REQ] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
			DealChatTextMsg(session, msg_type, msg_data);
		};

	_fun_callbacks[ID_HEART_BEAT_REQ] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
			HeartBeatHandler(session, msg_type, msg_data);
		};

	_fun_callbacks[ID_LOAD_CHAT_THREAD_REQ] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
			GetUserThreadsHandler(session, msg_type, msg_data);
		};
	
	_fun_callbacks[ID_CREATE_PRIVATE_CHAT_REQ] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
			CreatePrivateChat(session, msg_type, msg_data);
		};

	_fun_callbacks[ID_LOAD_CHAT_MSG_REQ] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
			LoadChatMsg(session, msg_type, msg_data);
		};

	_fun_callbacks[ID_IMG_CHAT_MSG_REQ] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
			DealChatImgMsg(session, msg_type, msg_data);
		};

	_fun_callbacks[ID_CHAT_DELIVERY_ACK_REQ] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
			DealDeliveryAck(session, msg_type, msg_data);
		};

	_fun_callbacks[ID_SYNC_MESSAGE_REQ] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
			DealSyncMessage(session, msg_type, msg_data);
		};

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

void LogicSystem::AddFriendApply(std::shared_ptr<CSession> session, const short& msg_type, const string& msg_data)
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

void LogicSystem::AuthFriendApply(std::shared_ptr<CSession> session, const short& msg_type, const string& msg_data) {
	
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

void LogicSystem::DealChatTextMsg(std::shared_ptr<CSession> session, const short& msg_type, const string& msg_data) {
	auto root = json::parse(msg_data, nullptr, false);

	auto uid = root["fromuid"].get<int>();
	auto touid = root["touid"].get<int>();
	//thread_id 按 64 位解析：协议字符串化后兼容十进制字符串与数字
	std::int64_t thread_id = 0;
	if (!ParseJsonId(root["thread_id"], thread_id)) {
		json err;
		err["error"] = ErrorCodes::Error_Json;
		session->Send(err.dump(4), ID_TEXT_CHAT_MSG_RSP);
		return;
	}

	//单条化：content/unique_id 顶层平铺（text_array 批量设计已删除）
	auto content = root["content"].get<std::string>();
	auto unique_id = root["unique_id"].get<std::string>();

	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["fromuid"] = uid;
	rtvalue["touid"] = touid;
	rtvalue["thread_id"] = std::to_string(thread_id);

	//构造 ChatMessage：status 统一 UN_READ，content_size 固定 0（计划5.4/3.2）
	auto chat_msg = std::make_shared<ChatMessage>();
	chat_msg->chat_time = getCurrentTimestamp();
	chat_msg->sender_id = uid;
	chat_msg->recv_id = touid;
	chat_msg->unique_id = unique_id;
	chat_msg->thread_id = thread_id;
	chat_msg->content = content;
	chat_msg->status = MsgStatus::UN_READ;
	chat_msg->msg_type = static_cast<int>(ChatMsgType::TEXT);
	chat_msg->content_size = 0;

	//插入数据库（单条自成事务）。DAO 在 commit 后回写
	//canonical message_id/status/chat_time/delivery_status（计划3.3）
	auto save_res = MysqlMgr::GetInstance()->AddChatMsg(chat_msg);
	if (save_res == SaveMessageResult::Failed) {
		//持久化失败：不 ACK、不转发，sender 按 transient 重传（计划3.5/5.4）
		rtvalue["error"] = ErrorCodes::MESSAGE_STORE_FAILED;
		session->Send(rtvalue.dump(4), ID_TEXT_CHAT_MSG_RSP);
		return;
	}
	if (save_res == SaveMessageResult::Conflict) {
		//永久冲突：原消息不变，sender 停止重传该 unique_id（计划3.5/5.4）
		rtvalue["error"] = ErrorCodes::MESSAGE_CONFLICT;
		rtvalue["unique_id"] = chat_msg->unique_id;
		session->Send(rtvalue.dump(4), ID_TEXT_CHAT_MSG_RSP);
		return;
	}

	//Stored/Duplicate：canonical 持久值 envelope 拍平到响应顶层（计划5.4/5.3）
	rtvalue.update(BuildMessageEnvelope(chat_msg));

	//【关键顺序】事务已提交 → 先发 1018（语义固定为“服务端已持久化”），再做 live delivery。
	//不得用 Defer 延后：那会让 live 先于 sender ACK（计划5.4）
	std::string sender_rsp = rtvalue.dump(4);
	session->Send(sender_rsp, ID_TEXT_CHAT_MSG_RSP);

	//仅 canonical delivery_status==Pending 才尝试 live delivery。
	//已 ACK 的 duplicate 只重发上面的 sender response（计划5.4）
	if (chat_msg->delivery_status != DeliveryStatus::Pending) {
		return;
	}

	//同步行已随 AddChatMsg 事务写入 user_message_sync，receiver 增量同步兜底。
	//live delivery：查询 touid 路由。pending duplicate 可能再次 live-push，这是
	//at-least-once 允许的行为，recipient 以 message_id 去重（计划5.4）
	auto to_str = std::to_string(touid);
	auto to_ip_key = USERIPPREFIX + to_str;
	std::string to_ip_value = "";
	bool b_ip = RedisMgr::GetInstance()->Get(to_ip_key, to_ip_value);
	if (!b_ip) {
		return; //目标不在任何节点，同步行已持久化，等 receiver 增量同步
	}

	auto& cfg = ConfigMgr::Inst();
	auto self_name = cfg["SelfServer"]["Name"];
	if (to_ip_value == self_name) {
		//本机 recipient：顶层拍平 envelope（与 1039 图片通知同构），纳入 recipient uid 分片（计划1.5/5.5）
		json notify = BuildMessageEnvelope(chat_msg);
		notify["error"] = ErrorCodes::Success;
		std::string notify_str = notify.dump(4);
		PostToUser(touid, [touid, notify_str]() {
			auto session = UserMgr::GetInstance()->GetSession(touid);
			if (session) {
				session->Send(notify_str, ID_NOTIFY_TEXT_CHAT_MSG_REQ);
			}
		});
		return;
	}

	//远端：TextChatMsgReq 携带 unique_id/msg_id/content/chat_time（单条 textmsg 字段）。
	//投递经 sender uid 固定的 delivery shard，避免 3s deadline 阻塞 logic shard，且同一
	//sender 的多次跨服调用在 delivery worker 上保持顺序（计划5.6）。带重试的
	//NotifyTextChatMsg 内部按 [Delivery] 配置 deadline/最多尝试次数/退避，重试耗尽只日志：
	//同步行已随事务提交，receiver 后续增量同步兜底；不撤销 sender ACK、不改 DB。
	TextChatMsgReq text_msg_req;
	text_msg_req.set_fromuid(uid);
	text_msg_req.set_touid(touid);
	text_msg_req.set_thread_id(thread_id);
	auto* text_msg = text_msg_req.mutable_textmsg();
	text_msg->set_unique_id(chat_msg->unique_id);
	text_msg->set_msgcontent(chat_msg->content);
	text_msg->set_msg_id(chat_msg->message_id);
	text_msg->set_chat_time(chat_msg->chat_time);

	//拷贝 server_ip / proto req 进闭包（按值捕获），闭包在 sender 固定的 delivery worker 上执行
	std::string server_ip = to_ip_value;
	auto posted = PostDelivery(uid, [server_ip, text_msg_req]() {
		auto res = ChatGrpcClient::GetInstance()->NotifyTextChatMsg(server_ip, text_msg_req);
		if (res.grpc_code == grpc::StatusCode::OK && res.app_error == ErrorCodes::Success) {
			std::cout << "DealChatTextMsg cross-server delivered, server=" << server_ip << std::endl;
		}
		else {
			//重试耗尽 / 不可重试 / 未知 server：只日志。同步行已随事务提交，receiver 后续增量同步兜底（计划5.6）
			std::cout << "DealChatTextMsg cross-server delivery final result server=" << server_ip
				<< " grpc_code=" << static_cast<int>(res.grpc_code)
				<< " app_error=" << res.app_error
				<< " (sync row committed, receiver will sync)" << std::endl;
		}
	});
	if (!posted) {
		//停机：delivery worker 已拒绝投递。同步行已随事务提交，receiver 后续增量同步兜底（计划5.6）
		std::cout << "DealChatTextMsg PostDelivery rejected (stopping), server=" << server_ip
			<< " (sync row committed, receiver will sync)" << std::endl;
	}
}

void LogicSystem::HeartBeatHandler(std::shared_ptr<CSession> session, const short& msg_type, const string& msg_data) {
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
	const short& msg_type, const string& msg_data)
{
	//从数据库加chat_threads记录
	auto root = json::parse(msg_data, nullptr, false);
	auto uid = root["uid"].get<int>();
	std::cout << "get uid  threads  " << uid << std::endl;

	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["uid"] = uid;
	Defer defer([this, &rtvalue, session]() {
		std::string return_str = rtvalue.dump(4);
		session->Send(return_str, ID_LOAD_CHAT_THREAD_RSP);
		});

	//thread_id 游标按 64 位解析：协议字符串化后兼容十进制字符串与数字
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
	int64_t& nextLastId)
{
	return MysqlMgr::GetInstance()->GetUserThreads(userId, lastId, pageSize, 
		threads, loadMore, nextLastId);
}

void LogicSystem::CreatePrivateChat(std::shared_ptr<CSession> session, const short& msg_type, const string& msg_data)
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

	std::int64_t thread_id = 0;
	bool res = MysqlMgr::GetInstance()->CreatePrivateChat(uid, other_id, thread_id);
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

	//thread_id / before_message_id 按 64 位解析：协议字符串化后兼容十进制字符串与数字
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
	std::shared_ptr<PageResult> res = MysqlMgr::GetInstance()->LoadChatMsg(thread_id, message_id, page_size);
	if (!res) {
		rtvalue["error"] = ErrorCodes::LOAD_CHAT_FAILED;
		return;
	}

	rtvalue["last_message_id"] = std::to_string(res->next_cursor);
	rtvalue["load_more"] = res->load_more;
	for (auto& chat : res->messages) {
		json  chat_data;
		chat_data["sender"] = chat.sender_id;
		chat_data["msg_id"] = std::to_string(chat.message_id);
		chat_data["thread_id"] = std::to_string(chat.thread_id);
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
	const short& msg_type, const string& msg_data) {
	auto root = json::parse(msg_data, nullptr, false);

	auto uid = root["fromuid"].get<int>();
	auto touid = root["touid"].get<int>();
	//thread_id 按 64 位解析：协议字符串化后兼容十进制字符串与数字
	std::int64_t thread_id = 0;
	if (!ParseJsonId(root["thread_id"], thread_id)) {
		json err;
		err["error"] = ErrorCodes::Error_Json;
		session->Send(err.dump(4), ID_IMG_CHAT_MSG_RSP);
		return;
	}

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
	rtvalue["thread_id"] = std::to_string(thread_id);
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
		//永久冲突：原消息不变，不创建第二行（unique_id 已在 rtvalue 顶层）
		rtvalue["error"] = ErrorCodes::MESSAGE_CONFLICT;
		session->Send(rtvalue.dump(4), ID_IMG_CHAT_MSG_RSP);
		return;
	}

	//canonical：message_id 十进制字符串；content_size 十进制字符串（计划5.4/6.3）
	rtvalue["message_id"] = std::to_string(chat_msg->message_id);
	rtvalue["content_size"] = std::to_string(chat_msg->content_size);

	//【关键顺序】事务已提交 → 发 1036 sender response（语义固定为“服务端已持久化”）（计划5.4）
	session->Send(rtvalue.dump(4), ID_IMG_CHAT_MSG_RSP);

	//UN_UPLOAD 图片不写同步行、不实时通知，待 §5.7 上传完成点（UpdateUploadStatusWithSync
	//成功后同事务补同步行）才对增量同步可见，避免同步到尚不可下载的图片（计划5.4/4.2）
}

json LogicSystem::BuildMessageEnvelope(const std::shared_ptr<ChatMessage>& msg) {
	json env;
	//message_id/thread_id 一律十进制字符串，避免 Qt JSON number 对 64 位值丢精度
	env["message_id"] = std::to_string(msg->message_id);
	env["unique_id"] = msg->unique_id;
	env["thread_id"] = std::to_string(msg->thread_id);
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

void LogicSystem::DealDeliveryAck(std::shared_ptr<CSession> session, const short& msg_type, const string& msg_data) {
	//1049 {"uid":<receiver>,"message_ids":["<id>",...]} -> 1050 {"error":0,"message_ids":["<id>",...]}
	//严格校验：JSON 对象、uid 正整数且 == session->GetUserId()、message_ids 非空数组且每项
	//为十进制字符串（协议字符串化），解析为 uint64 后去重升序
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
	//每项必须正 id 十进制字符串，用 std::set 去重并升序
	std::set<std::int64_t> id_set;
	for (const auto& e : ids_json) {
		std::int64_t v = 0;
		if (!ParseJsonId(e, v)) {
			reject(ErrorCodes::Error_Json);
			return;
		}
		if (v <= 0) {
			reject(ErrorCodes::Error_Json);
			return;
		}
		id_set.insert(v);
	}
	std::vector<std::int64_t> ids(id_set.begin(), id_set.end());

	//GetMessagesByIds 带 recv_id 防越权：返回行数必须与请求完全吻合，未知/不属于本 receiver 的 id 不会被返回
	auto msgs = MysqlMgr::GetInstance()->GetMessagesByIds(uid, ids);
	if (msgs.size() != ids.size()) {
		reject(ErrorCodes::Error_Json);
		return;
	}

	//只有 DB 更新成功后才回 ACK response（计划4.3/5.2）
	bool ok = MysqlMgr::GetInstance()->MarkMessagesDelivered(uid, ids);
	if (!ok) {
		reject(ErrorCodes::MESSAGE_STORE_FAILED);
		return;
	}

	//先回 Success + 原（去重升序）ids（十进制字符串数组），让客户端尽快确认；重复 ACK 因 DAO 幂等仍 success
	json rsp;
	rsp["error"] = ErrorCodes::Success;
	json ids_arr = json::array();
	for (std::int64_t id : ids) {
		ids_arr.push_back(std::to_string(id));
	}
	rsp["message_ids"] = ids_arr;
	session->Send(rsp.dump(4), ID_CHAT_DELIVERY_ACK_RSP);
}

void LogicSystem::DealSyncMessage(std::shared_ptr<CSession> session, const short& msg_type, const string& msg_data) {
	//1051 增量 {"uid":<数字>,"after_sync_seq":"500","limit":100}
	//    -> 1052 {"error":0,"messages":[envelope+sync_seq],"next_sync_seq":"<seq>","has_more":<bool>}
	//1051 bootstrap {"uid":<数字>,"bootstrap":true}
	//    -> 1052 {"error":0,"checkpoint":"<max_seq>"}（首启 checkpoint，不带消息）
	auto root = json::parse(msg_data, nullptr, false);

	auto reject = [&session](ErrorCodes code) {
		json rsp;
		rsp["error"] = code;
		session->Send(rsp.dump(), ID_SYNC_MESSAGE_RSP);
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

	//bootstrap 变体：只回当前最大同步序号，供客户端首启建立 checkpoint
	if (root.contains("bootstrap") && root["bootstrap"].is_boolean()
		&& root["bootstrap"].get<bool>()) {
		std::uint64_t max_seq = 0;
		if (!MysqlMgr::GetInstance()->GetMaxSyncSeq(uid, max_seq)) {
			//DAO 失败不能当作空 checkpoint 下发，客户端据此 transient 重试
			reject(ErrorCodes::MESSAGE_STORE_FAILED);
			return;
		}
		json rsp;
		rsp["error"] = ErrorCodes::Success;
		rsp["checkpoint"] = std::to_string(max_seq);
		session->Send(rsp.dump(), ID_SYNC_MESSAGE_RSP);
		return;
	}

	//after_sync_seq：十进制字符串（缺省 "0"），整串消费，解析失败回 Error_Json
	std::uint64_t after_sync_seq = 0;
	if (root.contains("after_sync_seq")) {
		if (!root["after_sync_seq"].is_string()) {
			reject(ErrorCodes::Error_Json);
			return;
		}
		auto s = root["after_sync_seq"].get<std::string>();
		try {
			std::size_t pos = 0;
			after_sync_seq = std::stoull(s, &pos);
			if (pos != s.size()) {
				reject(ErrorCodes::Error_Json);
				return;
			}
		}
		catch (...) {
			reject(ErrorCodes::Error_Json);
			return;
		}
	}

	//limit 缺失默认 100，clamp [1,200]
	int limit = 100;
	if (root.contains("limit")) {
		if (!root["limit"].is_number_integer()) {
			reject(ErrorCodes::Error_Json);
			return;
		}
		limit = root["limit"].get<int>();
	}
	if (limit < 1) limit = 1;
	if (limit > 200) limit = 200;

	//DAO 多取一条供 has_more 判断；失败不能当作空页（会让客户端误判已同步到最新）
	std::vector<SyncedMessage> rows;
	if (!MysqlMgr::GetInstance()->GetMessagesAfterSyncSeq(uid, after_sync_seq, limit, rows)) {
		reject(ErrorCodes::MESSAGE_STORE_FAILED);
		return;
	}

	bool has_more = static_cast<int>(rows.size()) > limit;
	if (has_more) {
		rows.pop_back();  // 丢掉第 limit+1 条，下页从 next_sync_seq 再取
	}

	json rsp;
	rsp["error"] = ErrorCodes::Success;
	rsp["messages"] = json::array();
	//空页 next_sync_seq 保持 after_sync_seq；非空页为本页最后一条的 sync_seq
	std::uint64_t next_sync_seq = after_sync_seq;
	for (auto& row : rows) {
		json env = BuildMessageEnvelope(row.msg);
		env["sync_seq"] = std::to_string(row.sync_seq);
		rsp["messages"].push_back(env);
		next_sync_seq = row.sync_seq;
	}
	rsp["next_sync_seq"] = std::to_string(next_sync_seq);
	rsp["has_more"] = has_more;

	session->Send(rsp.dump(), ID_SYNC_MESSAGE_RSP);
}

