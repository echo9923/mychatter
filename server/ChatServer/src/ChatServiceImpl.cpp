#include "ChatServiceImpl.h"
#include "UserMgr.h"
#include "CSession.h"
#include "LogicSystem.h"
#include <nlohmann/json.hpp>
#include "RedisMgr.h"
#include "MysqlMgr.h"
#include "utils.h"

using json = nlohmann::json;

ChatServiceImpl::ChatServiceImpl()
{

}

Status ChatServiceImpl::NotifyAddFriend(ServerContext* context, const AddFriendReq* request, AddFriendRsp* reply)
{
	//计划1.5：面向 recipient 的写入必须纳入其 uid 分片，gRPC 线程只复制数据 + 投递闭包
	int err = ErrorCodes::Success;
	Defer defer([request, reply, &err]() {
		reply->set_error(err);
		reply->set_applyuid(request->applyuid());
		reply->set_touid(request->touid());
		});

	//查找用户是否在本服务器
	auto touid = request->touid();
	auto session = UserMgr::GetInstance()->GetSession(touid);

	//无 session 表示目标用户不在线
	if (session == nullptr) {
		err = ErrorCodes::RECIPIENT_OFFLINE;
		return Status::OK;
	}

	//在内存中则先构建通知（gRPC 线程不直接写 session），再投递到 recipient 分片
	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["applyuid"] = request->applyuid();
	rtvalue["name"] = request->name();
	rtvalue["desc"] = request->desc();
	rtvalue["icon"] = request->icon();
	rtvalue["sex"] = request->sex();
	rtvalue["nick"] = request->nick();

	std::string return_str = rtvalue.dump(4);

	//闭包在 recipient shard 上重新查 session，存在才发送；队列停止则返回 SERVER_BUSY
	if (!LogicSystem::GetInstance()->PostToUser(touid,
		[touid, return_str]() {
			auto session = UserMgr::GetInstance()->GetSession(touid);
			if (session) {
				session->Send(return_str, ID_NOTIFY_ADD_FRIEND_REQ);
			}
		})) {
		err = ErrorCodes::SERVER_BUSY;
		return Status::OK;
	}

	return Status::OK;
}

Status ChatServiceImpl::NotifyAuthFriend(ServerContext* context, const AuthFriendReq* request,
	AuthFriendRsp* reply) {
	//计划1.5：面向 recipient 的写入必须纳入其 uid 分片，gRPC 线程只复制数据 + 投递闭包
	int err = ErrorCodes::Success;
	Defer defer([request, reply, &err]() {
		reply->set_error(err);
		reply->set_fromuid(request->fromuid());
		reply->set_touid(request->touid());
		});

	//查找用户是否在本服务器
	auto touid = request->touid();
	auto fromuid = request->fromuid();
	auto session = UserMgr::GetInstance()->GetSession(touid);

	//无 session 表示目标用户不在线
	if (session == nullptr) {
		err = ErrorCodes::RECIPIENT_OFFLINE;
		return Status::OK;
	}

	//在内存中则先构建通知（gRPC 线程不直接写 session）
	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["fromuid"] = request->fromuid();
	rtvalue["touid"] = request->touid();

	std::string base_key = USER_BASE_INFO + std::to_string(fromuid);
	auto user_info = std::make_shared<UserInfo>();
	bool b_info = GetBaseInfo(base_key, fromuid, user_info);
	if (b_info) {
		rtvalue["name"] = user_info->name;
		rtvalue["nick"] = user_info->nick;
		rtvalue["icon"] = user_info->icon;
		rtvalue["sex"] = user_info->sex;
	}
	else {
		rtvalue["error"] = ErrorCodes::UidInvalid;
	}

	auto chat_time = getCurrentTimestamp();
	for(auto& msg : request->textmsgs()) {
		json  chat;
		chat["sender"] = msg.sender_id();
		chat["msg_id"] = msg.msg_id();
		chat["thread_id"] = msg.thread_id();
		chat["unique_id"] = msg.unique_id();
		chat["msg_content"] = msg.msgcontent();
		chat["chat_time"] = chat_time;
		chat["status"] = msg.status();
		rtvalue["chat_datas"].push_back(chat);
	}

	std::string return_str = rtvalue.dump(4);

	//闭包在 recipient shard 上重新查 session，存在才发送；队列停止则返回 SERVER_BUSY
	if (!LogicSystem::GetInstance()->PostToUser(touid,
		[touid, return_str]() {
			auto session = UserMgr::GetInstance()->GetSession(touid);
			if (session) {
				session->Send(return_str, ID_NOTIFY_AUTH_FRIEND_REQ);
			}
		})) {
		err = ErrorCodes::SERVER_BUSY;
		return Status::OK;
	}

	return Status::OK;
}

Status ChatServiceImpl::NotifyTextChatMsg(::grpc::ServerContext* context,
	const TextChatMsgReq* request, TextChatMsgRsp* reply) {
	//计划1.5：面向 recipient 的写入必须纳入其 uid 分片，gRPC 线程只复制数据 + 投递闭包
	int err = ErrorCodes::Success;
	Defer defer([reply, &err]() {
		reply->set_error(err);
		});

	//查找用户是否在本服务器
	auto touid = request->touid();
	auto session = UserMgr::GetInstance()->GetSession(touid);

	//无 session 表示目标用户不在线
	if (session == nullptr) {
		err = ErrorCodes::RECIPIENT_OFFLINE;
		return Status::OK;
	}

	//统一顶层拍平 live envelope（计划5.5，单条化后与 1039 图片通知同构）：
	//textmsg proto 字段携带 unique_id/msg_id/msgcontent/chat_time，thread_id/fromuid/touid
	//来自 req，msg_type=TEXT/status=UN_READ/content_size="0" 为常量。
	//message_id/thread_id 一律十进制字符串，避免 Qt JSON number 对 64 位值丢精度。
	json  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["fromuid"] = request->fromuid();
	rtvalue["touid"] = request->touid();
	rtvalue["thread_id"] = std::to_string(request->thread_id());
	const auto& msg = request->textmsg();
	rtvalue["message_id"] = std::to_string(msg.msg_id());
	rtvalue["unique_id"] = msg.unique_id();
	rtvalue["msg_type"] = static_cast<int>(ChatMsgType::TEXT);
	rtvalue["content"] = msg.msgcontent();
	rtvalue["content_size"] = "0";
	rtvalue["chat_time"] = msg.chat_time();
	rtvalue["status"] = MsgStatus::UN_READ;

	std::string return_str = rtvalue.dump(4);

	//闭包在 recipient shard 上重新查 session，存在才发送；队列停止则返回 SERVER_BUSY
	if (!LogicSystem::GetInstance()->PostToUser(touid,
		[touid, return_str]() {
			auto session = UserMgr::GetInstance()->GetSession(touid);
			if (session) {
				session->Send(return_str, ID_NOTIFY_TEXT_CHAT_MSG_REQ);
			}
		})) {
		err = ErrorCodes::SERVER_BUSY;
		return Status::OK;
	}

	return Status::OK;
}


bool ChatServiceImpl::GetBaseInfo(std::string base_key, int uid, std::shared_ptr<UserInfo>& userinfo)
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

Status ChatServiceImpl::NotifyKickUser(::grpc::ServerContext* context, 
	const KickUserReq* request, KickUserRsp* reply)
{
	//计划1.5：面向被踢用户的写入必须纳入其 uid 分片，gRPC 线程只复制数据 + 投递闭包
	int err = ErrorCodes::Success;
	Defer defer([request, reply, &err]() {
		reply->set_error(err);
		reply->set_uid(request->uid());
		});

	//查找用户是否在本服务器
	auto uid = request->uid();
	auto session = UserMgr::GetInstance()->GetSession(uid);

	//无 session 表示目标用户不在线
	if (session == nullptr) {
		err = ErrorCodes::RECIPIENT_OFFLINE;
		return Status::OK;
	}

	//复制 CServer 共享指针（闭包可能晚于 gRPC 调用执行），闭包在 uid shard 上重新查
	//session 后踢下线并清除旧连接；队列停止则返回 SERVER_BUSY
	auto p_server = _p_server;
	if (!LogicSystem::GetInstance()->PostToUser(uid,
		[uid, p_server]() {
			auto session = UserMgr::GetInstance()->GetSession(uid);
			if (session) {
				session->NotifyOffline(uid);
				//清除旧的连接
				p_server->ClearSession(session->GetSessionId());
			}
		})) {
		err = ErrorCodes::SERVER_BUSY;
		return Status::OK;
	}

	return Status::OK;
}

void ChatServiceImpl::RegisterServer(std::shared_ptr<CServer> pServer)
{
	_p_server = pServer;
}

Status ChatServiceImpl::NotifyChatImgMsg(::grpc::ServerContext* context, const ::message::NotifyChatImgReq* request, ::message::NotifyChatImgRsp* response)
{
	//计划1.5：面向 recipient 的写入必须纳入其 uid 分片，gRPC 线程只复制数据 + 投递闭包
	int err = ErrorCodes::Success;
	Defer defer([request, response, &err]() {
		//设置具体的回包信息
		response->set_error(err);
		response->set_message_id(request->message_id());
		});

	//查找用户是否在本服务器
	auto uid = request->to_uid();
	auto session = UserMgr::GetInstance()->GetSession(uid);

	//无 session 表示目标用户不在线
	if (session == nullptr) {
		err = ErrorCodes::RECIPIENT_OFFLINE;
		return Status::OK;
	}

	//复制 proto 请求为值（闭包可能晚于 gRPC 调用执行，不可持有 request 指针），闭包在
	//recipient shard 上重新查 session 后通知图片消息；队列停止则返回 SERVER_BUSY
	::message::NotifyChatImgReq req_copy = *request;
	if (!LogicSystem::GetInstance()->PostToUser(uid,
		[uid, req_copy]() {
			auto session = UserMgr::GetInstance()->GetSession(uid);
			if (session) {
				session->NotifyChatImgRecv(&req_copy);
			}
		})) {
		err = ErrorCodes::SERVER_BUSY;
		return Status::OK;
	}

	return Status::OK;
}
