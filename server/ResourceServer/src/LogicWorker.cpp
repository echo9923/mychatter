#include "LogicWorker.h"
#include "FileSystem.h"
#include "CSession.h"
#include "LogicSystem.h"
#include "ConfigMgr.h"
#include "RedisMgr.h"
#include "MysqlMgr.h"
#include "Sha256.h"

#include <boost/filesystem.hpp>

//将请求消息类型映射为对应的回复消息类型；无对应回复（通知类）或未知类型返回0
static short ReqToRspId(short msg_type)
{
	switch (msg_type) {
	case ID_UPLOAD_HEAD_ICON_REQ:             return ID_UPLOAD_HEAD_ICON_RSP;
	case ID_DOWN_LOAD_FILE_REQ:               return ID_DOWN_LOAD_FILE_RSP;
	case ID_RESOURCE_CHUNK_UPLOAD_REQ:        return ID_RESOURCE_CHUNK_UPLOAD_RSP;
	case ID_RESOURCE_UPLOAD_PROGRESS_REQ:     return ID_RESOURCE_UPLOAD_PROGRESS_RSP;
	case ID_RESOURCE_DOWN_INFO_REQ:           return ID_RESOURCE_DOWN_INFO_RSP;
	case ID_RESOURCE_CHUNK_DOWN_REQ:          return ID_RESOURCE_CHUNK_DOWN_RSP;
	default:                                  return 0;
	}
}

namespace {
/// 从 JSON 值解析 64 位无符号整数：十进制字符串（协议约定）与数字（兼容）都接受，
/// 字符串必须整串消费；解析失败返回 false
bool ParseJsonUInt64(const json& v, unsigned long long& out) {
	if (v.is_number_unsigned()) {
		out = v.get<unsigned long long>();
		return true;
	}
	if (v.is_number_integer()) {
		const long long n = v.get<long long>();
		if (n >= 0) {
			out = static_cast<unsigned long long>(n);
			return true;
		}
		return false;
	}
	if (v.is_string()) {
		try {
			auto s = v.get<std::string>();
			std::size_t pos = 0;
			unsigned long long n = std::stoull(s, &pos);
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

/// 资源文件路径（磁盘真值）：resource/<sender_uid>/<message_id>
boost::filesystem::path ResourceFilePath(long long sender_id, long long message_id) {
	return ConfigMgr::Inst().GetResourceRootPath()
		/ std::to_string(sender_id) / std::to_string(message_id);
}
} // namespace

LogicWorker::LogicWorker():_b_stop(false)
{
	RegisterCallBacks();

	_work_thread = std::thread([this]() {
		while (!_b_stop) {
			std::unique_lock<std::mutex> lock(_mtx);
			_cv.wait(lock, [this]() {
				if(_b_stop) {
					return true;
				}

				if (_task_que.empty()) {
					return false;
				}

				return true;

			});

			if (_b_stop) {
				return;
			}

			auto task = _task_que.front();
			task_callback(task);
			_task_que.pop();
		}
	});

}

LogicWorker::~LogicWorker()
{
	_b_stop = true;
	_cv.notify_one();
	_work_thread.join();
}

void LogicWorker::PostTask(std::shared_ptr<LogicNode> task)
{
	std::lock_guard<std::mutex> lock(_mtx);
	_task_que.push(task);
	_cv.notify_one();
}

void LogicWorker::RegisterCallBacks()
{
	_fun_callbacks[ID_UPLOAD_HEAD_ICON_REQ] = &LogicWorker::handleUploadHeadIcon;
	_fun_callbacks[ID_DOWN_LOAD_FILE_REQ] = &LogicWorker::handleDownloadFile;
	//1037 上传资源分片：{message_id:"<str>", offset:"<str>", chunk_sha256, data:"<base64>"}
	//固定路由 message_id % FILE_WORKER_COUNT：同一 .part 只被一个线程写
	_fun_callbacks[ID_RESOURCE_CHUNK_UPLOAD_REQ] = &LogicWorker::handleResourceChunkUpload;
	//1041 查询上传进度：{message_id:"<str>"} -> {error, message_id, server_offset, total_size,
	//resource_status, content_hash}。与 1037 同 worker 串行化，server_offset 为磁盘 .part 真值
	_fun_callbacks[ID_RESOURCE_UPLOAD_PROGRESS_REQ] = &LogicWorker::handleResourceUploadProgress;
	//1045 查询资源下载信息：{message_id:"<str>"} -> {error, message_id, file_name, total_size,
	//content_hash, mime_type, msg_type, resource_status}。权限：请求者必须是 sender 或 recv
	_fun_callbacks[ID_RESOURCE_DOWN_INFO_REQ] = &LogicWorker::handleResourceDownInfo;
	//1047 按偏移量下载资源分片：{message_id:"<str>", offset:"<str>"} ->
	//{error, message_id, offset, bytes, chunk_sha256, data, total_size, is_last}
	_fun_callbacks[ID_RESOURCE_CHUNK_DOWN_REQ] = &LogicWorker::handleResourceChunkDown;
	_fun_callbacks[ID_RESOURCE_LOGIN_REQ] = &LogicWorker::handleResourceLogin;
}

void LogicWorker::handleUploadHeadIcon(shared_ptr<CSession> session, const short& msg_type,
	const string& msg_data)
{
	auto root = json::parse(msg_data, nullptr, false);
	auto md5 = root["md5"].get<std::string>();
	auto seq = root["seq"].get<int>();
	auto name = root["name"].get<std::string>();
	auto total_size = root["total_size"].get<int>();
	auto trans_size = root["trans_size"].get<int>();
	auto last = root["last"].get<int>();
	auto file_data = root["data"].get<std::string>();
	auto uid = session->GetUserId();
	auto last_seq = root["last_seq"].get<int>();
	//转化为字符串
	auto uid_str = std::to_string(uid);

	auto file_path = ConfigMgr::Inst().GetFileOutPath();
	auto file_path_str = (file_path / uid_str / name).string();
	json  rtvalue;
	auto callback = [=](const json& result) {

		// 在异步任务完成后调用
		json rtvalue = result;
		rtvalue["total_size"] = total_size;
		rtvalue["seq"] = seq;
		rtvalue["name"] = name;
		rtvalue["trans_size"] = trans_size;
		rtvalue["last"] = last;
		rtvalue["md5"] = md5;
		rtvalue["uid"] = uid;
		rtvalue["last_seq"] = last_seq;
		std::string return_str = rtvalue.dump(4);
		session->Send(return_str, ID_UPLOAD_HEAD_ICON_RSP);
	};

	// 使用 std::hash 对字符串进行哈希
	std::hash<std::string> hash_fn;
	size_t hash_value = hash_fn(name); // 生成哈希值
	int index = hash_value % FILE_WORKER_COUNT;
	std::cout << "Hash value: " << hash_value << std::endl;

	//第一个包
	if (seq == 1) {
		//构造数据存储
		auto file_info = std::make_shared<FileInfo>();
		file_info->_file_path_str = file_path_str;
		file_info->_name = name;
		file_info->_seq = seq;
		file_info->_total_size = total_size;
		file_info->_trans_size = trans_size;
		bool success = RedisMgr::GetInstance()->SetFileInfo(name, file_info);
		if (!success) {
			rtvalue["error"] = ErrorCodes::FileSaveRedisFailed;
			std::string return_str = rtvalue.dump(4);
			session->Send(return_str, ID_UPLOAD_HEAD_ICON_RSP);
			return;
		}
	}
	else {
		auto file_info = RedisMgr::GetInstance()->GetFileInfo(name);
		if (file_info == nullptr) {
			rtvalue["error"] = ErrorCodes::FileNotExists;
			std::string return_str = rtvalue.dump(4);
			session->Send(return_str, ID_UPLOAD_HEAD_ICON_RSP);
			return;
		}
		file_info->_seq = seq;
		file_info->_trans_size = trans_size;
		bool success = RedisMgr::GetInstance()->SetFileInfo(name, file_info);
		if (!success) {
			rtvalue["error"] = ErrorCodes::FileSaveRedisFailed;
			std::string return_str = rtvalue.dump(4);
			session->Send(return_str, ID_UPLOAD_HEAD_ICON_RSP);
			return;
		}
	}


	FileSystem::GetInstance()->PostMsgToQue(
		std::make_shared<FileTask>(session, ID_UPLOAD_HEAD_ICON_REQ, uid, file_path_str, name, seq, total_size,
			trans_size, last, file_data, callback),
		index
	);

}

void LogicWorker::handleDownloadFile(shared_ptr<CSession> session, const short& msg_type,
	const string& msg_data)
{
	auto root = json::parse(msg_data, nullptr, false);
	auto seq = root["seq"].get<int>();
	auto name = root["name"].get<std::string>();
	auto uid = session->GetUserId();
	auto client_path = root["client_path"].get<std::string>();
	auto req_type = root["req_type"].get<std::string>();
	//转化为字符串
	auto uid_str = std::to_string(uid);

	auto file_path = ConfigMgr::Inst().GetFileOutPath();
	auto file_path_str = (file_path / uid_str / name).string();
	json  rtvalue;
	auto callback = [=](const json& result) {

		// 在异步任务完成后调用
		json rtvalue = result;
		rtvalue["client_path"] = client_path;
		rtvalue["name"] = name;
		rtvalue["req_type"] = req_type;
		std::string return_str = rtvalue.dump(4);
		session->Send(return_str, ID_DOWN_LOAD_FILE_RSP);
	};

	// 使用 std::hash 对字符串进行哈希
	std::hash<std::string> hash_fn;
	size_t hash_value = hash_fn(name); // 生成哈希值
	int index = hash_value % FILE_WORKER_COUNT;
	std::cout << "Hash value: " << hash_value << std::endl;

	FileSystem::GetInstance()->PostDownloadTaskToQue(
		std::make_shared<DownloadTask>(session, uid, name, seq, file_path_str, callback),
		index
	);

}

void LogicWorker::handleResourceChunkUpload(shared_ptr<CSession> session, const short& msg_type,
	const string& msg_data)
{
	auto root = json::parse(msg_data, nullptr, false);
	if (root.is_discarded() || !root.is_object()) {
		json rtvalue;
		rtvalue["error"] = ErrorCodes::Error_Json;
		session->Send(rtvalue.dump(4), ID_RESOURCE_CHUNK_UPLOAD_RSP);
		return;
	}

	unsigned long long message_id = 0, offset = 0;
	if (!root.contains("message_id") || !root.contains("offset") ||
		!root.contains("chunk_sha256") || !root.contains("data") ||
		!ParseJsonUInt64(root["message_id"], message_id) ||
		!ParseJsonUInt64(root["offset"], offset) ||
		!root["chunk_sha256"].is_string() || !root["data"].is_string()) {
		json rtvalue;
		rtvalue["error"] = ErrorCodes::Error_Json;
		rtvalue["message_id"] = root.contains("message_id")
			&& root["message_id"].is_string() ? root["message_id"].get<std::string>() : "0";
		session->Send(rtvalue.dump(4), ID_RESOURCE_CHUNK_UPLOAD_RSP);
		return;
	}
	auto chunk_sha256 = root["chunk_sha256"].get<std::string>();
	auto file_data = root["data"].get<std::string>();

	//协议字段格式校验：片哈希必须是 64 位小写 hex（真值比对在 FileWorker 解码后进行）
	if (message_id == 0 || !llfc::IsValidSha256Hex(chunk_sha256)) {
		json rtvalue;
		rtvalue["error"] = ErrorCodes::Error_Json;
		rtvalue["message_id"] = std::to_string(message_id);
		session->Send(rtvalue.dump(4), ID_RESOURCE_CHUNK_UPLOAD_RSP);
		return;
	}

	auto callback = [=](const json& result) {
		std::string return_str = result.dump(4);
		session->Send(return_str, ID_RESOURCE_CHUNK_UPLOAD_RSP);
	};

	int index = ResourceWorkerIndex(static_cast<long long>(message_id), FILE_WORKER_COUNT);
	FileSystem::GetInstance()->PostChunkToQue(
		std::make_shared<ResourceChunkTask>(session, static_cast<long long>(message_id),
			static_cast<long long>(offset), chunk_sha256, file_data, callback),
		index
	);
}

void LogicWorker::handleResourceUploadProgress(shared_ptr<CSession> session, const short& msg_type,
	const string& msg_data)
{
	auto root = json::parse(msg_data, nullptr, false);
	unsigned long long message_id = 0;
	if (root.is_discarded() || !root.is_object() ||
		!root.contains("message_id") ||
		!ParseJsonUInt64(root["message_id"], message_id) || message_id == 0) {
		json rtvalue;
		rtvalue["error"] = ErrorCodes::Error_Json;
		session->Send(rtvalue.dump(4), ID_RESOURCE_UPLOAD_PROGRESS_RSP);
		return;
	}

	auto callback = [session, message_id]() {
		auto chat_msg = MysqlMgr::GetInstance()->GetChatMsgById(static_cast<long long>(message_id));
		json rtvalue;
		rtvalue["message_id"] = std::to_string(message_id);
		if (chat_msg == nullptr) {
			rtvalue["error"] = ErrorCodes::MsgIdErr;
			session->Send(rtvalue.dump(4), ID_RESOURCE_UPLOAD_PROGRESS_RSP);
			return;
		}

		unsigned long long server_offset = 0;
		if (chat_msg->resource_status == static_cast<int>(ResourceStatus::Ready)) {
			server_offset = chat_msg->content_size;
		}
		else {
			//磁盘真值：.part 实际长度（无文件即 0），error_code 重载防异常穿出线程
			const auto part_path = ResourceFilePath(chat_msg->sender_id,
				static_cast<long long>(message_id)).string() + ".part";
			boost::system::error_code fs_ec;
			if (boost::filesystem::exists(part_path, fs_ec)) {
				boost::uintmax_t size = boost::filesystem::file_size(part_path, fs_ec);
				if (!fs_ec) {
					server_offset = static_cast<unsigned long long>(size);
				}
			}
		}

		rtvalue["error"] = ErrorCodes::Success;
		rtvalue["server_offset"] = std::to_string(server_offset);
		rtvalue["total_size"] = std::to_string(chat_msg->content_size);
		rtvalue["resource_status"] = chat_msg->resource_status;
		rtvalue["content_hash"] = chat_msg->content_hash;
		session->Send(rtvalue.dump(4), ID_RESOURCE_UPLOAD_PROGRESS_RSP);
	};

	int index = ResourceWorkerIndex(static_cast<long long>(message_id), FILE_WORKER_COUNT);
	FileSystem::GetInstance()->PostClosureToQue(callback, index);
}

void LogicWorker::handleResourceDownInfo(shared_ptr<CSession> session, const short& msg_type,
	const string& msg_data)
{
	auto root = json::parse(msg_data, nullptr, false);
	unsigned long long message_id = 0;
	if (root.is_discarded() || !root.is_object() ||
		!root.contains("message_id") ||
		!ParseJsonUInt64(root["message_id"], message_id) || message_id == 0) {
		json rtvalue;
		rtvalue["error"] = ErrorCodes::Error_Json;
		session->Send(rtvalue.dump(4), ID_RESOURCE_DOWN_INFO_RSP);
		return;
	}

	auto uid = session->GetUserId();
	auto chat_msg = MysqlMgr::GetInstance()->GetChatMsgById(static_cast<long long>(message_id));

	auto respond_error = [session, message_id](int error) {
		json rtvalue;
		rtvalue["error"] = error;
		rtvalue["message_id"] = std::to_string(message_id);
		session->Send(rtvalue.dump(4), ID_RESOURCE_DOWN_INFO_RSP);
	};

	if (chat_msg == nullptr) {
		respond_error(ErrorCodes::MsgIdErr);
		return;
	}

	//权限校验：非收发双方一律拒绝（不泄露资源存在性以外的信息）
	if (uid != chat_msg->sender_id && uid != chat_msg->recv_id) {
		respond_error(ErrorCodes::ResourceForbidden);
		return;
	}

	//就绪校验：未完成/已过期的资源不可下载
	if (chat_msg->resource_status != static_cast<int>(ResourceStatus::Ready)) {
		json rtvalue;
		rtvalue["error"] = ErrorCodes::ResourceNotReady;
		rtvalue["message_id"] = std::to_string(message_id);
		rtvalue["resource_status"] = chat_msg->resource_status;
		session->Send(rtvalue.dump(4), ID_RESOURCE_DOWN_INFO_RSP);
		return;
	}

	//最终文件必须存在（清理任务可能已回收）
	boost::system::error_code fs_ec;
	const auto file_path = ResourceFilePath(chat_msg->sender_id, static_cast<long long>(message_id));
	boost::uintmax_t file_size = boost::filesystem::file_size(file_path, fs_ec);
	if (fs_ec) {
		std::cerr << "resource down info: file missing " << file_path.string()
			<< " ec=" << fs_ec.message() << std::endl;
		respond_error(ErrorCodes::FileNotExists);
		return;
	}

	json rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["message_id"] = std::to_string(message_id);
	rtvalue["file_name"] = chat_msg->content;   //原始文件名（仅展示用）
	rtvalue["total_size"] = std::to_string(file_size);
	rtvalue["content_hash"] = chat_msg->content_hash;
	rtvalue["mime_type"] = chat_msg->mime_type;
	rtvalue["msg_type"] = chat_msg->msg_type;
	rtvalue["resource_status"] = chat_msg->resource_status;
	session->Send(rtvalue.dump(4), ID_RESOURCE_DOWN_INFO_RSP);
}

void LogicWorker::handleResourceChunkDown(shared_ptr<CSession> session, const short& msg_type,
	const string& msg_data)
{
	auto root = json::parse(msg_data, nullptr, false);
	unsigned long long message_id = 0, offset = 0;
	if (root.is_discarded() || !root.is_object() ||
		!root.contains("message_id") || !root.contains("offset") ||
		!ParseJsonUInt64(root["message_id"], message_id) ||
		!ParseJsonUInt64(root["offset"], offset) || message_id == 0) {
		json rtvalue;
		rtvalue["error"] = ErrorCodes::Error_Json;
		session->Send(rtvalue.dump(4), ID_RESOURCE_CHUNK_DOWN_RSP);
		return;
	}

	auto uid = session->GetUserId();
	auto chat_msg = MysqlMgr::GetInstance()->GetChatMsgById(static_cast<long long>(message_id));

	auto respond_error = [session, message_id](int error) {
		json rtvalue;
		rtvalue["error"] = error;
		rtvalue["message_id"] = std::to_string(message_id);
		session->Send(rtvalue.dump(4), ID_RESOURCE_CHUNK_DOWN_RSP);
	};

	if (chat_msg == nullptr) {
		respond_error(ErrorCodes::MsgIdErr);
		return;
	}
	if (uid != chat_msg->sender_id && uid != chat_msg->recv_id) {
		respond_error(ErrorCodes::ResourceForbidden);
		return;
	}
	if (chat_msg->resource_status != static_cast<int>(ResourceStatus::Ready)) {
		respond_error(ErrorCodes::ResourceNotReady);
		return;
	}

	boost::system::error_code fs_ec;
	const auto file_path = ResourceFilePath(chat_msg->sender_id, static_cast<long long>(message_id));
	if (!boost::filesystem::exists(file_path, fs_ec) || fs_ec) {
		respond_error(ErrorCodes::FileNotExists);
		return;
	}

	auto callback = [=](const json& result) {
		std::string return_str = result.dump(4);
		session->Send(return_str, ID_RESOURCE_CHUNK_DOWN_RSP);
	};

	//下载只读最终文件（rename 后与写天然互斥）；固定路由保持同一 message_id 串行
	int index = ResourceWorkerIndex(static_cast<long long>(message_id), DOWN_LOAD_WORKER_COUNT);
	FileSystem::GetInstance()->PostChunkDownToQue(
		std::make_shared<ResourceChunkDownTask>(session, static_cast<long long>(message_id),
			static_cast<long long>(offset), file_path.string(), chat_msg->content_size, callback),
		index
	);
}

void LogicWorker::handleResourceLogin(shared_ptr<CSession> session, const short& msg_type,
	const string& msg_data)
{
	auto root = json::parse(msg_data, nullptr, false);

	int uid = 0;
	std::string token;
	bool parse_ok = root.is_object();
	if (parse_ok) {
		try {
			uid = root["uid"].get<int>();
			token = root["token"].get<std::string>();
		}
		catch (...) {
			parse_ok = false;
		}
	}

	json rtvalue;
	//解析失败按未授权处理(fail closed)，不暴露具体差异
	if (!parse_ok) {
		rtvalue["error"] = ErrorCodes::TokenInvalid;
		session->Send(rtvalue.dump(4), ID_RESOURCE_LOGIN_RSP);
		return;
	}

	//校验登录令牌: utoken_<uid>，缺失或不匹配即返回TokenInvalid且不绑定会话
	std::string stored;
	bool ok = RedisMgr::GetInstance()->Get(USERTOKENPREFIX + std::to_string(uid), stored);
	if (!ok || stored != token) {
		rtvalue["error"] = ErrorCodes::TokenInvalid;
		session->Send(rtvalue.dump(4), ID_RESOURCE_LOGIN_RSP);
		return;
	}

	if (!session->TrySetAuth(uid)) {
		return;
	}
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["uid"] = uid;
	session->Send(rtvalue.dump(4), ID_RESOURCE_LOGIN_RSP);
}

void LogicWorker::task_callback(std::shared_ptr<LogicNode> task)
{
	auto session = task->_session;
	if (!session->IsOpen()) {
		return;
	}
	short msg_type = task->_recvnode->_msg_type;
	std::string msg_data(task->_recvnode->_data, task->_recvnode->_cur_len);

	cout << "recv_msg type is " << msg_type << endl;

	//登录鉴权握手是唯一允许在未认证状态下处理的消息
	if (msg_type == ID_RESOURCE_LOGIN_REQ) {
		auto call_back_iter = _fun_callbacks.find(msg_type);
		if (call_back_iter == _fun_callbacks.end()) {
			return;
		}
		(this->*call_back_iter->second)(session, msg_type, msg_data);
		return;
	}

	//鉴权门控：除登录外所有消息都要求会话已认证，否则返回TokenInvalid并关闭连接
	if (!session->IsAuthed()) {
		short rsp_id = ReqToRspId(msg_type);
		if (rsp_id != 0) {
			json rtvalue;
			rtvalue["error"] = ErrorCodes::TokenInvalid;
			session->SendAndClose(rtvalue.dump(4), rsp_id);
		}
		else {
			session->Close();
		}
		return;
	}

	auto call_back_iter = _fun_callbacks.find(msg_type);
	if (call_back_iter == _fun_callbacks.end()) {
		return;
	}
	(this->*call_back_iter->second)(session, msg_type, msg_data);
}
