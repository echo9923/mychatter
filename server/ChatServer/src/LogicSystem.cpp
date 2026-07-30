#include "LogicSystem.h"
#include "StatusGrpcClient.h"
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

}

void LogicSystem::LoginHandler(shared_ptr<CSession> session, const short &msg_id, const string &msg_data) {
	auto root = json::parse(msg_data, nullptr, false);
	auto uid = root["uid"].get<int>();
	auto token = root["token"].get<std::string>();
	std::cout << "user login uid is  " << uid << " user token  is "
		<< token << endl;

	json  rtvalue;
	Defer defer([this, &rtvalue, session]() {
		std::string return_str = rtvalue.dump(4);
		session->Send(return_str, MSG_CHAT_LOGIN_RSP);
		});


	//从redis获取用户token是否正确
	std::string uid_str = std::to_string(uid);
	std::string token_key = USERTOKENPREFIX + uid_str;
	std::string token_value = "";
	bool success = RedisMgr::GetInstance()->Get(token_key, token_value);
	if (!success) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return ;
	}

	if (token_value != token) {
		rtvalue["error"] = ErrorCodes::TokenInvalid;
		return ;
	}

	rtvalue["error"] = ErrorCodes::Success;


	std::string base_key = USER_BASE_INFO + uid_str;
	auto user_info = std::make_shared<UserInfo>();
	bool b_base = GetBaseInfo(base_key, uid, user_info);
	if (!b_base) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}
	rtvalue["uid"] = uid;
	rtvalue["pwd"] = user_info->pwd;
	rtvalue["name"] = user_info->name;
	rtvalue["email"] = user_info->email;
	rtvalue["nick"] = user_info->nick;
	rtvalue["desc"] = user_info->desc;
	rtvalue["sex"] = user_info->sex;
	rtvalue["icon"] = user_info->icon;
	rtvalue["token"] = token;

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

	auto server_name = ConfigMgr::Inst().GetValue("SelfServer", "Name");
	{
		//此处添加分布式锁，让该线程独占登录
		//拼接用户ip对应的key
		auto lock_key = LOCK_PREFIX + uid_str;
		auto identifier = RedisMgr::GetInstance()->acquireLock(lock_key, LOCK_TIME_OUT, ACQUIRE_TIME_OUT);
		//利用defer解锁
		Defer defer2([this, identifier, lock_key]() {
			RedisMgr::GetInstance()->releaseLock(lock_key, identifier);
			});
		//此处判断该用户是否在别处或者本服务器登录

		std::string uid_ip_value = "";
		auto uid_ip_key = USERIPPREFIX + uid_str;
		bool b_ip = RedisMgr::GetInstance()->Get(uid_ip_key, uid_ip_value);
		//说明用户已经登录了，此处应该踢掉之前的用户登录状态
		if (b_ip) {
			//获取当前服务器ip信息
			auto& cfg = ConfigMgr::Inst();
			auto self_name = cfg["SelfServer"]["Name"];
			//如果之前登录的服务器和当前相同，则直接在本服务器踢掉
			if (uid_ip_value == self_name) {
				//查找旧有的连接
				auto old_session = UserMgr::GetInstance()->GetSession(uid);

				//此处应该发送踢人消息
				if (old_session) {
					old_session->NotifyOffline(uid);
					//清除旧的连接
					_p_server->ClearSession(old_session->GetSessionId());
				}

			}
			else {
				//如果不是本服务器，则通知grpc通知其他服务器踢掉
				//发送通知
				KickUserReq kick_req;
				kick_req.set_uid(uid);
				ChatGrpcClient::GetInstance()->NotifyKickUser(uid_ip_value, kick_req);
			}
		}

		//session绑定用户uid
		session->SetUserId(uid);
		//为用户设置登录ip server的名字
		std::string  ipkey = USERIPPREFIX + uid_str;
		RedisMgr::GetInstance()->Set(ipkey, server_name);
		//uid和session绑定管理,方便以后踢人操作
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
		auto session = UserMgr::GetInstance()->GetSession(touid);
		if (session) {
			//在内存中则直接发送通知对方
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
			session->Send(return_str, ID_NOTIFY_ADD_FRIEND_REQ);
		}

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
		auto session = UserMgr::GetInstance()->GetSession(touid);
		if (session) {
			//在内存中则直接发送通知对方
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
			session->Send(return_str, ID_NOTIFY_AUTH_FRIEND_REQ);
		}

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

	const json  arrays = root["text_array"];
	
	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;

	rtvalue["fromuid"] = uid;
	rtvalue["touid"] = touid;
	auto thread_id = root["thread_id"].get<int>();
	rtvalue["thread_id"] = thread_id;
	std::vector<std::shared_ptr<ChatMessage>> chat_datas;
	auto timestamp = getCurrentTimestamp();
	for (const auto& txt_obj : arrays) {
		auto content = txt_obj["content"].get<std::string>();
		auto unique_id = txt_obj["unique_id"].get<std::string>();
		std::cout << "content is " << content << std::endl;
		std::cout << "unique_id is " << unique_id << std::endl;
		auto chat_msg = std::make_shared<ChatMessage>();
		chat_msg->chat_time = timestamp;
		chat_msg->sender_id = uid;
		chat_msg->recv_id = touid;
		chat_msg->unique_id = unique_id;
		chat_msg->thread_id = thread_id;
		chat_msg->content = content;
		chat_msg->status = 2;
		chat_msg->msg_type = int(ChatMsgType::TEXT);
		chat_datas.push_back(chat_msg);
	}


	//插入数据库
	MysqlMgr::GetInstance()->AddChatMsg(chat_datas);


	for (const auto& chat_data : chat_datas) {
		json  chat_msg;
		chat_msg["message_id"] = chat_data->message_id;
		chat_msg["unique_id"] = chat_data->unique_id;
		chat_msg["content"] = chat_data->content;
		chat_msg["status"] = chat_data->status;
		chat_msg["chat_time"] = chat_data->chat_time;
		rtvalue["chat_datas"].push_back(chat_msg);
	}

	Defer defer([this, &rtvalue, session]() {
		std::string return_str = rtvalue.dump(4);
		session->Send(return_str, ID_TEXT_CHAT_MSG_RSP);
		});


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
		auto session = UserMgr::GetInstance()->GetSession(touid);
		if (session) {
			//在内存中则直接发送通知对方
			std::string return_str = rtvalue.dump(4);
			session->Send(return_str, ID_NOTIFY_TEXT_CHAT_MSG_REQ);
		}

		return ;
	}


	TextChatMsgReq text_msg_req;
	text_msg_req.set_fromuid(uid);
	text_msg_req.set_touid(touid);
	text_msg_req.set_thread_id(thread_id);
	for (const auto& chat_data : chat_datas) {
		auto *text_msg = text_msg_req.add_textmsgs();
		text_msg->set_unique_id(chat_data->unique_id);
		text_msg->set_msgcontent(chat_data->content);
		text_msg->set_msg_id(chat_data->message_id);
		text_msg->set_chat_time(chat_data->chat_time);
	}


	//发送通知 todo...
	ChatGrpcClient::GetInstance()->NotifyTextChatMsg(to_ip_value, text_msg_req, rtvalue);
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
		auto pwd = root["pwd"].get<std::string>();
		auto email = root["email"].get<std::string>();
		auto nick = root["nick"].get<std::string>();
		auto desc = root["desc"].get<std::string>();
		auto sex = root["sex"].get<int>();
		auto icon = root["icon"].get<std::string>();
		std::cout << "user  uid is  " << uid << " name  is "
			<< name << " pwd is " << pwd << " email is " << email <<" icon is " << icon << endl;

		rtvalue["uid"] = uid;
		rtvalue["pwd"] = pwd;
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
	redis_root["pwd"] = user_info->pwd;
	redis_root["name"] = user_info->name;
	redis_root["email"] = user_info->email;
	redis_root["nick"] = user_info->nick;
	redis_root["desc"] = user_info->desc;
	redis_root["sex"] = user_info->sex;
	redis_root["icon"] = user_info->icon;

	RedisMgr::GetInstance()->Set(base_key, redis_root.dump(4));

	//返回数据
	rtvalue["uid"] = user_info->uid;
	rtvalue["pwd"] = user_info->pwd;
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
		auto pwd = root["pwd"].get<std::string>();
		auto email = root["email"].get<std::string>();
		auto nick = root["nick"].get<std::string>();
		auto desc = root["desc"].get<std::string>();
		auto sex = root["sex"].get<int>();
		auto icon = root["icon"].get<std::string>();
		std::cout << "user  uid is  " << uid << " name  is "
			<< name << " pwd is " << pwd << " email is " << email << endl;

		rtvalue["uid"] = uid;
		rtvalue["pwd"] = pwd;
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
	redis_root["pwd"] = user_info->pwd;
	redis_root["name"] = user_info->name;
	redis_root["email"] = user_info->email;
	redis_root["nick"] = user_info->nick;
	redis_root["desc"] = user_info->desc;
	redis_root["sex"] = user_info->sex;
	redis_root["icon"] = user_info->icon;

	RedisMgr::GetInstance()->Set(base_key, redis_root.dump(4));
	
	//返回数据
	rtvalue["uid"] = user_info->uid;
	rtvalue["pwd"] = user_info->pwd;
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
		userinfo->pwd = root["pwd"].get<std::string>();
		userinfo->email = root["email"].get<std::string>();
		userinfo->nick = root["nick"].get<std::string>();
		userinfo->desc = root["desc"].get<std::string>();
		userinfo->sex = root["sex"].get<int>();
		userinfo->icon = root["icon"].get<std::string>();
		std::cout << "user login uid is  " << userinfo->uid << " name  is "
			<< userinfo->name << " pwd is " << userinfo->pwd << " email is " << userinfo->email << endl;
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
		redis_root["pwd"] = userinfo->pwd;
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

	auto md5 = root["md5"].get<std::string>();
	auto unique_name = root["name"].get<std::string>();
	auto token = root["token"].get<std::string>();
	auto unique_id = root["unique_id"].get<std::string>();
	auto chat_time = root["chat_time"].get<std::string>();
	auto status = root["status"].get<int>();

	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;

	rtvalue["fromuid"] = uid;
	rtvalue["touid"] = touid;
	auto thread_id = root["thread_id"].get<int>();
	rtvalue["thread_id"] = thread_id;
	rtvalue["md5"] = md5;
	rtvalue["unique_name"] = unique_name;
	rtvalue["unique_id"] = unique_id;
	rtvalue["chat_time"] = chat_time;
	rtvalue["status"] = MsgStatus::UN_UPLOAD;

	auto timestamp = getCurrentTimestamp();
	auto chat_msg = std::make_shared<ChatMessage>();
	chat_msg->chat_time = timestamp;
	chat_msg->sender_id = uid;
	chat_msg->recv_id = touid;
	chat_msg->unique_id = unique_id;
	chat_msg->thread_id = thread_id;
	chat_msg->content = unique_name;
	chat_msg->status = MsgStatus::UN_UPLOAD;
	chat_msg->msg_type = int(ChatMsgType::PIC);

	//插入数据库
	MysqlMgr::GetInstance()->AddChatMsg(chat_msg);

	rtvalue["message_id"] = chat_msg->message_id;
	Defer defer([this, &rtvalue, session]() {
		std::string return_str = rtvalue.dump(4);
		session->Send(return_str, ID_IMG_CHAT_MSG_RSP);
		});

}

