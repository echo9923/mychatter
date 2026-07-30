#include "ChatServerGrpcClient.h"
#include "MysqlMgr.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

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
} // namespace

NotifyResult ChatServerGrpcClient::NotifyChatImgMsg(int message_id, std::string chatserver)
{
	NotifyResult result{ grpc::StatusCode::OK, ErrorCodes::Success };

	//配置：每次尝试 deadline、最多尝试次数、退避基数（计划5.7/4.2）
	int deadline_ms = ReadDeliveryInt("RpcDeadlineMs", 3000);
	if (deadline_ms < 1) deadline_ms = 3000;
	int max_attempts = ReadDeliveryInt("RpcMaxAttempts", 3);
	if (max_attempts < 1) max_attempts = 1;
	int backoff_ms = ReadDeliveryInt("RpcBackoffMs", 100);
	if (backoff_ms < 1) backoff_ms = 100;

	auto find_iter = _hash_channels.find(chatserver);
	if (find_iter == _hash_channels.end()) {
		//未知 server：配置/路由缺失，参数类错误，立即停止不重试（计划5.7）
		result.grpc_code = grpc::StatusCode::NOT_FOUND;
		result.app_error = ErrorCodes::RPCFailed;
		return result;
	}

	//构造请求（消息元数据 + 文件大小），与重试无关，构建一次
	NotifyChatImgReq request;
	request.set_message_id(message_id);
	auto chat_msg = MysqlMgr::GetInstance()->GetChatMsgById(message_id);
	if (chat_msg == nullptr) {
		//消息不存在：参数类错误，立即停止不重试
		result.grpc_code = grpc::StatusCode::NOT_FOUND;
		result.app_error = ErrorCodes::MsgIdErr;
		return result;
	}
	request.set_file_name(chat_msg->content);
	request.set_from_uid(chat_msg->sender_id);
	request.set_to_uid(chat_msg->recv_id);
	request.set_thread_id(chat_msg->thread_id);
	// 资源文件路径
	auto file_dir = ConfigMgr::Inst().GetFileOutPath();
	//该消息是接收方客户端发送过来的,服务器将资源存储在发送方的文件夹中
	auto uid_str = std::to_string(chat_msg->sender_id);
	auto file_path = (file_dir / uid_str / chat_msg->content);
	boost::uintmax_t file_size = 0;
	boost::system::error_code ec;
	file_size = boost::filesystem::file_size(file_path, ec);
	if (ec) {
		//文件不存在/不可读：参数类错误，立即停止不重试（不激活对端推送）
		std::cerr << "NotifyChatImgMsg file_size failed for " << file_path.string()
		          << ": " << ec.message() << std::endl;
		result.grpc_code = grpc::StatusCode::NOT_FOUND;
		result.app_error = ErrorCodes::FileNotExists;
		return result;
	}
	request.set_total_size(file_size);

	auto& channel = find_iter->second;
	for (int attempt = 1; attempt <= max_attempts; ++attempt) {
		//每次尝试新的 stub + ClientContext，deadline 固定 RpcDeadlineMs（计划5.7）
		auto stub = ChatService::NewStub(channel);
		ClientContext context;
		context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(deadline_ms));

		NotifyChatImgRsp reply;
		Status status = stub->NotifyChatImgMsg(&context, request, &reply);

		if (status.ok()) {
			result.grpc_code = grpc::StatusCode::OK;
			result.app_error = reply.error();
			//仅对端 SERVER_BUSY(1016) 重试；Success/RECIPIENT_OFFLINE(1015)/未知应用错误立即停止（计划5.7）
			if (reply.error() == kAppServerBusy && attempt < max_attempts) {
				//退避：RpcBackoffMs、2×RpcBackoffMs（100/200ms）；位移限幅防溢出
				int shift = attempt - 1;
				if (shift > 10) shift = 10;
				std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms * (1 << shift)));
				continue;
			}
			return result;
		}

		//transport 失败：记录后判断是否重试（计划5.7）
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

ChatServerGrpcClient::ChatServerGrpcClient()
{
	auto& gCfgMgr = ConfigMgr::Inst();
	std::string host1 = gCfgMgr["chatserver1"]["Host"];
	std::string port1 = gCfgMgr["chatserver1"]["Port"];
	_hash_channels["chatserver1"] = grpc::CreateChannel(host1 + ":" + port1, grpc::InsecureChannelCredentials());

	std::string host2 = gCfgMgr["chatserver2"]["Host"];
	std::string port2 = gCfgMgr["chatserver2"]["Port"];
	_hash_channels["chatserver2"] = grpc::CreateChannel(host2 + ":" + port2, grpc::InsecureChannelCredentials());
}
