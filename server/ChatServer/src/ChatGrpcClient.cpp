#include "ChatGrpcClient.h"
#include "RedisMgr.h"
#include "ConfigMgr.h"
#include "UserMgr.h"

#include "CSession.h"
#include "MysqlMgr.h"
#include <chrono>
#include <thread>

namespace {
/// 从 [Delivery] 读取整数配置；非法/缺失时回退 fallback（与 LogicSystem 同一模式，计划4.2/5.3）
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
} // namespace

ChatGrpcClient::ChatGrpcClient()
{
	auto& cfg = ConfigMgr::Inst();
	auto server_list = cfg["PeerServer"]["Servers"];

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
		_channels[cfg[word]["Name"]] = grpc::CreateChannel(cfg[word]["Host"] + ":" + cfg[word]["Port"], grpc::InsecureChannelCredentials());
	}

}

AddFriendRsp ChatGrpcClient::NotifyAddFriend(std::string server_ip, const AddFriendReq& req)
{
	AddFriendRsp rsp;
	Defer defer([&rsp, &req]() {
		rsp.set_error(ErrorCodes::Success);
		rsp.set_applyuid(req.applyuid());
		rsp.set_touid(req.touid());
		});

	auto find_iter = _channels.find(server_ip);
	if (find_iter == _channels.end()) {
		return rsp;
	}

	auto stub = ChatService::NewStub(find_iter->second);
	ClientContext context;
	context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
	Status status = stub->NotifyAddFriend(&context, req, &rsp);

	if (!status.ok()) {
		rsp.set_error(ErrorCodes::RPCFailed);
		return rsp;
	}

	return rsp;
}


bool ChatGrpcClient::GetBaseInfo(std::string base_key, int uid, std::shared_ptr<UserInfo>& userinfo)
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

}

AuthFriendRsp ChatGrpcClient::NotifyAuthFriend(std::string server_ip, const AuthFriendReq& req) {
	AuthFriendRsp rsp;
	rsp.set_error(ErrorCodes::Success);

	Defer defer([&rsp, &req]() {
		rsp.set_fromuid(req.fromuid());
		rsp.set_touid(req.touid());
		});

	auto find_iter = _channels.find(server_ip);
	if (find_iter == _channels.end()) {
		return rsp;
	}

	auto stub = ChatService::NewStub(find_iter->second);
	ClientContext context;
	context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
	Status status = stub->NotifyAuthFriend(&context, req, &rsp);

	if (!status.ok()) {
		rsp.set_error(ErrorCodes::RPCFailed);
		return rsp;
	}

	return rsp;
}

NotifyResult ChatGrpcClient::NotifyTextChatMsg(const std::string& server_ip, const TextChatMsgReq& req) {
	NotifyResult result{ grpc::StatusCode::OK, ErrorCodes::Success };

	//配置：每次尝试 deadline、最多尝试次数、退避基数（计划5.6/4.2）
	int deadline_ms = ReadDeliveryInt("RpcDeadlineMs", 3000);
	if (deadline_ms < 1) deadline_ms = 3000;
	int max_attempts = ReadDeliveryInt("RpcMaxAttempts", 3);
	if (max_attempts < 1) max_attempts = 1;
	int backoff_ms = ReadDeliveryInt("RpcBackoffMs", 100);
	if (backoff_ms < 1) backoff_ms = 100;

	auto find_iter = _channels.find(server_ip);
	if (find_iter == _channels.end()) {
		//未知 server：配置/路由缺失，参数类错误，立即停止不重试（计划5.6）
		result.grpc_code = grpc::StatusCode::NOT_FOUND;
		result.app_error = ErrorCodes::RPCFailed;
		return result;
	}
	auto& channel = find_iter->second;

	for (int attempt = 1; attempt <= max_attempts; ++attempt) {
		//每次尝试新的 stub + ClientContext，deadline 固定 RpcDeadlineMs（计划5.6）
		auto stub = ChatService::NewStub(channel);
		ClientContext context;
		context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(deadline_ms));

		TextChatMsgRsp rsp;
		Status status = stub->NotifyTextChatMsg(&context, req, &rsp);

		if (status.ok()) {
			result.grpc_code = grpc::StatusCode::OK;
			result.app_error = rsp.error();
			//仅对端 SERVER_BUSY(1016) 重试；Success/RECIPIENT_OFFLINE(1015)/未知应用错误立即停止（计划5.6）
			if (rsp.error() == ErrorCodes::SERVER_BUSY && attempt < max_attempts) {
				//退避：RpcBackoffMs、2×RpcBackoffMs（100/200ms）；位移限幅防溢出
				int shift = attempt - 1;
				if (shift > 10) shift = 10;
				std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms * (1 << shift)));
				continue;
			}
			return result;
		}

		//transport 失败：记录后判断是否重试（计划5.6）
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

KickUserRsp ChatGrpcClient::NotifyKickUser(std::string server_ip, const KickUserReq& req)
{
	KickUserRsp rsp;
	Defer defer([&rsp, &req]() {
		rsp.set_error(ErrorCodes::Success);
		rsp.set_uid(req.uid());
		});

	auto find_iter = _channels.find(server_ip);
	if (find_iter == _channels.end()) {
		return rsp;
	}

	auto stub = ChatService::NewStub(find_iter->second);
	ClientContext context;
	context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
	Status status = stub->NotifyKickUser(&context, req, &rsp);

	if (!status.ok()) {
		rsp.set_error(ErrorCodes::RPCFailed);
		return rsp;
	}

	return rsp;
}
