#include "ResourceLogicWorker.h"
#include "FileSystem.h"
#include "CSession.h"
#include "LogicSystem.h"
#include "ConfigMgr.h"
#include "RedisMgr.h"
#include "MysqlMgr.h"
#include "Sha256.h"
#include "ResourceUploadFile.h" //llfc::ResourceBlobPath（P2 秒传 blob 判定）

#include <algorithm>
#include <boost/filesystem.hpp>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <limits>

namespace {

/// 单任务异常边界：异常绝不能逃逸到 common 执行器线程（否则 terminate）。
/// 吞掉并记录，队列中的后续任务继续执行。
void RunGuarded(const char* what, const std::function<void()>& fn) {
	try {
		fn();
	}
	catch (const std::exception& e) {
		std::cerr << "ResourceServer: " << what << " threw: " << e.what() << std::endl;
	}
	catch (...) {
		std::cerr << "ResourceServer: " << what << " threw unknown exception" << std::endl;
	}
}

//将请求消息类型映射为对应的回复消息类型；无对应回复（通知类）或未知类型返回0
short ReqToRspId(short msg_type)
{
	switch (msg_type) {
	case ID_UPLOAD_HEAD_ICON_REQ:             return ID_UPLOAD_HEAD_ICON_RSP;
	case ID_DOWN_LOAD_FILE_REQ:               return ID_DOWN_LOAD_FILE_RSP;
	case ID_RESOURCE_CHUNK_UPLOAD_REQ:        return ID_RESOURCE_CHUNK_UPLOAD_RSP;
	case ID_RESOURCE_UPLOAD_PROGRESS_REQ:     return ID_RESOURCE_UPLOAD_PROGRESS_RSP;
	case ID_RESOURCE_DOWN_INFO_REQ:           return ID_RESOURCE_DOWN_INFO_RSP;
	case ID_RESOURCE_CHUNK_DOWN_REQ:          return ID_RESOURCE_CHUNK_DOWN_RSP;
	case ID_RESOURCE_INSTANT_PROOF_REQ:      return ID_RESOURCE_INSTANT_PROOF_RSP;
	default:                                  return 0;
	}
}

/// 从 JSON 值解析 64 位无符号整数：破坏性协议只接受十进制字符串。
bool ParseJsonUInt64(const json& v, unsigned long long& out) {
	if (v.is_string()) {
		try {
			auto s = v.get<std::string>();
			if (s.empty() || !std::all_of(s.begin(), s.end(),
				[](unsigned char c) { return std::isdigit(c) != 0; })) {
				return false;
			}
			std::size_t pos = 0;
			unsigned long long n = std::stoull(s, &pos);
			if (pos == s.size() && n <= static_cast<unsigned long long>(
				std::numeric_limits<long long>::max())) {
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

/// 头像文件名校验：拒绝空名、`..`、路径分隔符与绝对路径前缀，
/// 防止 1601/1603 的 name 字段拼出穿越路径（resource/<uid>/<name>）。
bool IsValidAvatarName(const std::string& name) {
	if (name.empty() || name.size() > 128 || name.front() == '.') {
		return false;
	}
	if (name.find("..") != std::string::npos) {
		return false;
	}
	for (const char c : name) {
		if (c == '/' || c == '\\' || static_cast<unsigned char>(c) < 0x20) {
			return false;
		}
	}
	return true;
}
} // namespace

namespace {

//P1c 统一回包出口：1506/1508/1510 等仍为 JSON；1512 一律二进制线协议
//（[u16 meta_len BE][meta][raw]，含错误分支与鉴权拦截），raw 可为空。
void SendRspFrame(const std::shared_ptr<CSession>& session, short rsp_id,
    const json& meta, const std::string& raw = std::string())
{
    if (rsp_id == ID_RESOURCE_CHUNK_DOWN_RSP) {
        std::string body;
        if (llfc_wire::EncodeFrame(meta.dump(), raw, &body)) {
            session->Send(body, rsp_id);
        }
        return;
    }
    session->Send(meta.dump(), rsp_id);
}

void SendRspFrameAndClose(const std::shared_ptr<CSession>& session,
    short rsp_id, const json& meta)
{
    if (rsp_id == ID_RESOURCE_CHUNK_DOWN_RSP) {
        std::string body;
        if (llfc_wire::EncodeFrame(meta.dump(), std::string(), &body)) {
            session->SendAndClose(body, rsp_id);
        }
        return;
    }
    session->SendAndClose(meta.dump(), rsp_id);
}

} // namespace

ResourceLogicWorker::ResourceLogicWorker()
{
	RegisterCallBacks();
}

ResourceLogicWorker::~ResourceLogicWorker()
{
	// _executor（最后声明的成员）析构时排空队列并 join 工作线程，
	// 排空期间其余成员仍存活，任务可安全访问。
}

bool ResourceLogicWorker::PostTask(std::shared_ptr<LogicNode> task)
{
	return _executor.Post([this, task]() {
		RunGuarded("logic task", [this, &task]() { task_callback(task); });
	});
}

void ResourceLogicWorker::Stop()
{
	_executor.Stop();
}

void ResourceLogicWorker::RegisterCallBacks()
{
	_fun_callbacks[ID_UPLOAD_HEAD_ICON_REQ] = &ResourceLogicWorker::handleUploadHeadIcon;
	_fun_callbacks[ID_DOWN_LOAD_FILE_REQ] = &ResourceLogicWorker::handleDownloadFile;
	//1505 上传资源分片：{message_id:"<str>", offset:"<str>", chunk_sha256, data:"<base64>"}
	//固定路由 message_id % FILE_WORKER_COUNT：同一 .part 只被一个线程写
	_fun_callbacks[ID_RESOURCE_CHUNK_UPLOAD_REQ] = &ResourceLogicWorker::handleResourceChunkUpload;
	//1507 查询上传进度：{message_id:"<str>"} -> {error, message_id, server_offset, total_size,
	//status, sha256}。与 1505 共用会话；首次恢复回退尾片，查询不重复回退
	_fun_callbacks[ID_RESOURCE_UPLOAD_PROGRESS_REQ] = &ResourceLogicWorker::handleResourceUploadProgress;
	//P2 1513 秒传证明：{message_id:"<str>", proof:"<64hex>"} -> 1514
	//{error, message_id, status, server_offset, event_seq}；证明不符/挑战
	//丢失回 2118（客户端回退普通上传）。固定路由到上传 worker（挑战在
	//UploadSession 内，仅该线程可访问）。
	_fun_callbacks[ID_RESOURCE_INSTANT_PROOF_REQ] = &ResourceLogicWorker::handleInstantProof;
	//1509 查询资源下载信息：{message_id:"<str>"} -> {error, message_id, file_name, total_size,
	//sha256, mime_type, message_type, status}。权限：请求者必须是私聊成员
	_fun_callbacks[ID_RESOURCE_DOWN_INFO_REQ] = &ResourceLogicWorker::handleResourceDownInfo;
	//1511 按偏移量下载资源分片：{message_id:"<str>", offset:"<str>"} ->
	//{error, message_id, offset, bytes, chunk_sha256, data, total_size, is_last}
	_fun_callbacks[ID_RESOURCE_CHUNK_DOWN_REQ] = &ResourceLogicWorker::handleResourceChunkDown;
	_fun_callbacks[ID_RESOURCE_LOGIN_REQ] = &ResourceLogicWorker::handleResourceLogin;
}

void ResourceLogicWorker::handleUploadHeadIcon(shared_ptr<CSession> session, const short& msg_type,
	const string& msg_data, shared_ptr<SessionTaskQuota> quota)
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
		std::string return_str = rtvalue.dump();
		session->Send(return_str, ID_UPLOAD_HEAD_ICON_RSP);
	};

	//头像 name 是路径拼接成分：拒绝 `..`/分隔符等（2019，permanent）
	if (!IsValidAvatarName(name)) {
		std::cerr << "ResourceServer: reject avatar name \"" << name << "\"" << std::endl;
		rtvalue["error"] = ErrorCodes::ResourceInvalid;
		rtvalue["seq"] = seq;
		rtvalue["name"] = name;
		session->Send(rtvalue.dump(), ID_UPLOAD_HEAD_ICON_RSP);
		return;
	}

	//在途字节配额令牌（quota 参数）由首层 AsyncReadBody 获取并随任务传入，
	//这里不再二次获取：直接转入 FileTask，FileWorker 队列驻留期间持续占用。

	// 使用 std::hash 对字符串进行哈希
	std::hash<std::string> hash_fn;
	size_t hash_value = hash_fn(name); // 生成哈希值
	int index = hash_value % FILE_WORKER_COUNT;

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
			std::string return_str = rtvalue.dump();
			session->Send(return_str, ID_UPLOAD_HEAD_ICON_RSP);
			return;
		}
	}
	else {
		auto file_info = RedisMgr::GetInstance()->GetFileInfo(name);
		if (file_info == nullptr) {
			rtvalue["error"] = ErrorCodes::FileNotExists;
			std::string return_str = rtvalue.dump();
			session->Send(return_str, ID_UPLOAD_HEAD_ICON_RSP);
			return;
		}
		file_info->_seq = seq;
		file_info->_trans_size = trans_size;
		bool success = RedisMgr::GetInstance()->SetFileInfo(name, file_info);
		if (!success) {
			rtvalue["error"] = ErrorCodes::FileSaveRedisFailed;
			std::string return_str = rtvalue.dump();
			session->Send(return_str, ID_UPLOAD_HEAD_ICON_RSP);
			return;
		}
	}


	FileSystem::GetInstance()->PostMsgToQue(
		std::make_shared<FileTask>(session, ID_UPLOAD_HEAD_ICON_REQ, uid, file_path_str, name, seq, total_size,
			trans_size, last, file_data, callback, 0, 0, 0, std::move(quota)),
		index
	);

}

void ResourceLogicWorker::handleDownloadFile(shared_ptr<CSession> session, const short& msg_type,
	const string& msg_data, shared_ptr<SessionTaskQuota> quota)
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
		std::string return_str = rtvalue.dump();
		session->Send(return_str, ID_DOWN_LOAD_FILE_RSP);
	};

	//头像 name 是路径拼接成分：与 1601 同一校验（2019，permanent）
	if (!IsValidAvatarName(name)) {
		std::cerr << "ResourceServer: reject avatar download name \"" << name << "\"" << std::endl;
		rtvalue["error"] = ErrorCodes::ResourceInvalid;
		rtvalue["seq"] = seq;
		rtvalue["name"] = name;
		session->Send(rtvalue.dump(), ID_DOWN_LOAD_FILE_RSP);
		return;
	}

	// 使用 std::hash 对字符串进行哈希
	std::hash<std::string> hash_fn;
	size_t hash_value = hash_fn(name); // 生成哈希值
	int index = hash_value % FILE_WORKER_COUNT;

	FileSystem::GetInstance()->PostDownloadTaskToQue(
		std::make_shared<DownloadTask>(session, uid, name, seq, file_path_str, callback,
			std::move(quota)),
		index
	);

}

void ResourceLogicWorker::handleResourceChunkUpload(shared_ptr<CSession> session, const short& msg_type,
	const string& msg_data, shared_ptr<SessionTaskQuota> quota)
{
	//P1c：1505 为二进制帧 [u16 meta_len][meta JSON][raw]；raw 即分片原始
	//字节（不再 Base64）。解析失败/字段非法回 1506 JSON（1506 仍是 JSON）。
	std::string meta_str;
	std::string file_data;
	if (!llfc_wire::DecodeFrame(msg_data, &meta_str, &file_data)) {
		json rtvalue;
		rtvalue["error"] = ErrorCodes::Error_Json;
		SendRspFrame(session, ID_RESOURCE_CHUNK_UPLOAD_RSP, rtvalue);
		return;
	}
	auto root = json::parse(meta_str, nullptr, false);
	if (root.is_discarded() || !root.is_object()) {
		json rtvalue;
		rtvalue["error"] = ErrorCodes::Error_Json;
		SendRspFrame(session, ID_RESOURCE_CHUNK_UPLOAD_RSP, rtvalue);
		return;
	}

	unsigned long long message_id = 0, offset = 0;
	if (!root.contains("message_id") || !root.contains("offset") ||
		!root.contains("chunk_sha256") ||
		!ParseJsonUInt64(root["message_id"], message_id) ||
		!ParseJsonUInt64(root["offset"], offset) ||
		!root["chunk_sha256"].is_string() || file_data.empty()) {
		json rtvalue;
		rtvalue["error"] = ErrorCodes::Error_Json;
		rtvalue["message_id"] = root.contains("message_id")
			&& root["message_id"].is_string() ? root["message_id"].get<std::string>() : "0";
		SendRspFrame(session, ID_RESOURCE_CHUNK_UPLOAD_RSP, rtvalue);
		return;
	}

	auto chunk_sha256 = root["chunk_sha256"].get<std::string>();

	//协议字段格式校验：片哈希必须是 64 位小写 hex（真值比对在 FileWorker 进行）
	if (message_id == 0 || !llfc::IsValidSha256Hex(chunk_sha256)) {
		json rtvalue;
		rtvalue["error"] = ErrorCodes::Error_Json;
		rtvalue["message_id"] = std::to_string(message_id);
		SendRspFrame(session, ID_RESOURCE_CHUNK_UPLOAD_RSP, rtvalue);
		return;
	}

	auto callback = [=](const json& result) {
		std::string return_str = result.dump();
		session->Send(return_str, ID_RESOURCE_CHUNK_UPLOAD_RSP);
	};

	//在途字节配额令牌（quota 参数）由首层 AsyncReadBody 获取并随任务传入
	//（按整帧字节计，覆盖 raw 分片与 meta）；转入 ResourceChunkTask，
	//FileWorker 队列驻留期间持续占用。

	int index = ResourceWorkerIndex(static_cast<long long>(message_id), FILE_WORKER_COUNT);
	FileSystem::GetInstance()->PostChunkToQue(
		std::make_shared<ResourceChunkTask>(session, static_cast<long long>(message_id),
			static_cast<long long>(offset), chunk_sha256, file_data, callback,
			std::move(quota)),
		index
	);
}

void ResourceLogicWorker::handleResourceUploadProgress(shared_ptr<CSession> session, const short& msg_type,
	const string& msg_data, shared_ptr<SessionTaskQuota> quota)
{
	auto root = json::parse(msg_data, nullptr, false);
	unsigned long long message_id = 0;
	if (root.is_discarded() || !root.is_object() ||
		!root.contains("message_id") ||
		!ParseJsonUInt64(root["message_id"], message_id) || message_id == 0) {
		json rtvalue;
		rtvalue["error"] = ErrorCodes::Error_Json;
		session->Send(rtvalue.dump(), ID_RESOURCE_UPLOAD_PROGRESS_RSP);
		return;
	}

	const int uid = session->GetUserId();
	//在途字节配额令牌随闭包转入 FileWorker：队列驻留与执行期间持续占用
	//会话预算，executor 任务（std::function）销毁时才释放，与 1505/1509/1511 一致。
	auto callback = [session, message_id, uid, quota = std::move(quota)](FileWorker& worker) {
		//DB 行是唯一真值：每次 1507 都复核状态——清理任务可能已把
		//PENDING 置为 FAILED，而 worker 会话缓存仍认为在上传中。
		//Transient 与 NotFound 分开，避免 DB 瞬时故障被误标 2111。
		std::shared_ptr<ChatMessage> chat_msg;
		json rtvalue;
		rtvalue["message_id"] = std::to_string(message_id);
		switch (MysqlMgr::GetInstance()->GetChatMsgById(
			static_cast<long long>(message_id), chat_msg)) {
		case MysqlDao::ChatMsgLookup::Found:
			break;
		case MysqlDao::ChatMsgLookup::NotFound:
			rtvalue["error"] = ErrorCodes::MsgIdErr;
			session->Send(rtvalue.dump(), ID_RESOURCE_UPLOAD_PROGRESS_RSP);
			return;
		case MysqlDao::ChatMsgLookup::Transient:
		default:
			rtvalue["error"] = ErrorCodes::RPCFailed;
			session->Send(rtvalue.dump(), ID_RESOURCE_UPLOAD_PROGRESS_RSP);
			return;
		}
		if (uid == 0 || uid != chat_msg->sender_user_id) {
			rtvalue["error"] = ErrorCodes::ResourceForbidden;
			session->Send(rtvalue.dump(), ID_RESOURCE_UPLOAD_PROGRESS_RSP);
			return;
		}
		if ((chat_msg->message_type != 1 && chat_msg->message_type != 3) || !chat_msg->resource) {
			rtvalue["error"] = ErrorCodes::ResourceStateInvalid;
			session->Send(rtvalue.dump(), ID_RESOURCE_UPLOAD_PROGRESS_RSP);
			return;
		}

		unsigned long long server_offset = 0;
		if (chat_msg->status == MessageStatus::Published) {
			server_offset = chat_msg->resource->file_size_bytes;
		}
		else if (chat_msg->status == MessageStatus::Pending) {
			auto upload = worker.FindUploadSession(static_cast<long long>(message_id));
			if (!upload) upload = worker.LoadUploadSession(static_cast<long long>(message_id), chat_msg);
			if (!upload) {
				rtvalue["error"] = ErrorCodes::FileWritePermissionFailed;
				session->Send(rtvalue.dump(), ID_RESOURCE_UPLOAD_PROGRESS_RSP);
				return;
			}
			//P2 秒传判定（仅 PENDING 且 server_offset==0 的全新上传）：同时
			//满足三点才发一次性挑战——①该发送者有一条 Published 消息 sha256
			//与大小都相同（只看本人历史，不泄露他人内容）；②对应 blob 存在
			//（blob 存在即为可信内容）；③文件大小一致（DB 行真值）。
			//判定查询 Transient（DB 瞬时故障）→ 2002 让客户端稍后重试，
			//绝不把"查不到"当"无历史"静默降级（typed 结果不得折叠）。
			//challenge_suppressed（本会话证明曾不符）→ 不再下发新挑战，
			//客户端按普通上传继续（服务端防无限挑战闸门）。
			if (upload->received == 0 && !upload->challenge_suppressed
				&& chat_msg->resource->file_size_bytes > 0) {
				const auto sha = chat_msg->resource->sha256;
				const auto size = chat_msg->resource->file_size_bytes;
				bool instant_hit = false;
				const auto published = MysqlMgr::GetInstance()
					->SenderHasPublishedContent(uid, sha, size);
				if (published == MysqlDao::TriState::Transient) {
					rtvalue["error"] = ErrorCodes::RPCFailed;
					rtvalue["status"] = static_cast<int>(MessageStatus::Pending);
					session->Send(rtvalue.dump(), ID_RESOURCE_UPLOAD_PROGRESS_RSP);
					return;
				}
				if (published == MysqlDao::TriState::Yes) {
					boost::system::error_code blob_ec;
					const auto blob_path = llfc::ResourceBlobPath(
						ConfigMgr::Inst().GetResourceRootPath(), sha);
					const auto blob_size = boost::filesystem::file_size(blob_path, blob_ec);
					instant_hit = !blob_ec
						&& blob_size == static_cast<boost::uintmax_t>(size);
				}
				if (instant_hit) {
					//挑战参数：nonce=16 字节安全随机（hex 下发）；区段 offset
					//随机，length=min(size, 64KiB)；绑定本会话 + 本连接，
					//一次性使用 + TTL（FileWorker 侧校验）
					unsigned char nonce_raw[16] = {0};
					const unsigned long long length = size < 64u * 1024u
						? size : 64u * 1024u;
					unsigned char off_raw[8] = {0};
					if (llfc::RandomBytes(nonce_raw, sizeof(nonce_raw))
						&& llfc::RandomBytes(off_raw, sizeof(off_raw))) {
						unsigned long long rand64 = 0;
						for (int i = 0; i < 8; ++i) {
							rand64 = (rand64 << 8) | off_raw[i];
						}
						const unsigned long long offset =
							length >= size ? 0 : rand64 % (size - length + 1);
						char nonce_hex[33] = {0};
						for (int i = 0; i < 16; ++i) {
							std::snprintf(nonce_hex + i * 2, 3, "%02x", nonce_raw[i]);
						}
						upload->challenge_nonce_raw.assign(
							reinterpret_cast<const char*>(nonce_raw), sizeof(nonce_raw));
						upload->challenge_offset = offset;
						upload->challenge_length = length;
						upload->challenge_active = true;
						upload->challenge_session = session;
						upload->challenge_issued_at =
							std::chrono::steady_clock::now();
						rtvalue["error"] = ErrorCodes::Success;
						rtvalue["mode"] = "proof";
						rtvalue["server_offset"] = std::to_string(0);
						rtvalue["file_size_bytes"] = std::to_string(size);
						rtvalue["status"] = static_cast<int>(MessageStatus::Pending);
						rtvalue["sha256"] = sha;
						rtvalue["chunk_size"] = std::to_string(kResourceUploadChunkSize);
						json challenge;
						challenge["nonce"] = std::string(nonce_hex);
						challenge["offset"] = std::to_string(offset);
						challenge["length"] = std::to_string(length);
						rtvalue["challenge"] = challenge;
						session->Send(rtvalue.dump(), ID_RESOURCE_UPLOAD_PROGRESS_RSP);
						return;
					}
					//随机源失败：不降级伪随机，按普通上传继续
				}
			}
			server_offset = upload->received;
		}
		else {
			//终态（清理任务已置 Failed 等）：同步逐出过期会话缓存
			worker.EvictUploadSession(static_cast<long long>(message_id));
			rtvalue["error"] = ErrorCodes::ResourceStateInvalid;
			rtvalue["status"] = static_cast<int>(chat_msg->status);
			session->Send(rtvalue.dump(), ID_RESOURCE_UPLOAD_PROGRESS_RSP);
			return;
		}

		rtvalue["error"] = ErrorCodes::Success;
		rtvalue["server_offset"] = std::to_string(server_offset);
		rtvalue["file_size_bytes"] = std::to_string(chat_msg->resource->file_size_bytes);
		rtvalue["status"] = static_cast<int>(chat_msg->status);
		rtvalue["sha256"] = chat_msg->resource->sha256;
		//分片对齐单位（服务端恢复/回退按此对齐），客户端据此切块
		rtvalue["chunk_size"] = std::to_string(kResourceUploadChunkSize);
		if (chat_msg->event_seq > 0) {
			rtvalue["event_seq"] = std::to_string(chat_msg->event_seq);
		}
		session->Send(rtvalue.dump(), ID_RESOURCE_UPLOAD_PROGRESS_RSP);
	};

	int index = ResourceWorkerIndex(static_cast<long long>(message_id), FILE_WORKER_COUNT);
	FileSystem::GetInstance()->PostClosureToQue(callback, index);
}

void ResourceLogicWorker::handleInstantProof(shared_ptr<CSession> session, const short& msg_type,
	const string& msg_data, shared_ptr<SessionTaskQuota> quota)
{
	//msg_type 与既有 handler 一致留空不使用
	auto root = json::parse(msg_data, nullptr, false);
	unsigned long long message_id = 0;
	if (root.is_discarded() || !root.is_object()
		|| !root.contains("message_id") || !root.contains("proof")
		|| !ParseJsonUInt64(root["message_id"], message_id) || message_id == 0
		|| !root["proof"].is_string()) {
		json rtvalue;
		rtvalue["error"] = ErrorCodes::Error_Json;
		SendRspFrame(session, ID_RESOURCE_INSTANT_PROOF_RSP, rtvalue);
		return;
	}
	const std::string proof = root["proof"].get<std::string>();
	const long long mid = static_cast<long long>(message_id);
	const int uid = session->GetUserId();
	//在途字节配额令牌随闭包转入 FileWorker（与 1507 一致）；req_session/uid
	//一并传入：挑战绑定下发时的确切连接，proof 只能由本消息 sender 发起。
	auto callback = [session, uid, mid, proof, quota = std::move(quota)](FileWorker& worker) {
		auto respond = [session](const json& result) {
			session->Send(result.dump(), ID_RESOURCE_INSTANT_PROOF_RSP);
		};
		worker.HandleInstantProof(session, uid, mid, proof, respond);
	};
	const int index = ResourceWorkerIndex(mid, FILE_WORKER_COUNT);
	FileSystem::GetInstance()->PostClosureToQue(callback, index);
}

void ResourceLogicWorker::handleResourceDownInfo(shared_ptr<CSession> session, const short& msg_type,
	const string& msg_data, shared_ptr<SessionTaskQuota> quota)
{
	auto root = json::parse(msg_data, nullptr, false);
	unsigned long long message_id = 0;
	if (root.is_discarded() || !root.is_object() ||
		!root.contains("message_id") ||
		!ParseJsonUInt64(root["message_id"], message_id) || message_id == 0) {
		json rtvalue;
		rtvalue["error"] = ErrorCodes::Error_Json;
		session->Send(rtvalue.dump(), ID_RESOURCE_DOWN_INFO_RSP);
		return;
	}

	auto callback = [session](const json& result) {
		session->Send(result.dump(), ID_RESOURCE_DOWN_INFO_RSP);
	};

	//1509 与 1511 一样固定路由到 DownloadWorker[mid%N]：鉴权/DB/回包同线程，
	//同一 message_id 的 1510/1512 回包由同一线程有序发出。
	int index = ResourceWorkerIndex(static_cast<long long>(message_id), DOWN_LOAD_WORKER_COUNT);
	FileSystem::GetInstance()->PostDownInfoToQue(
		std::make_shared<ResourceDownInfoTask>(session, static_cast<long long>(message_id),
			callback, std::move(quota)),
		index
	);
}

void ResourceLogicWorker::handleResourceChunkDown(shared_ptr<CSession> session, const short& msg_type,
	const string& msg_data, shared_ptr<SessionTaskQuota> quota)
{
	auto root = json::parse(msg_data, nullptr, false);
	unsigned long long message_id = 0, offset = 0;
	if (root.is_discarded() || !root.is_object() ||
		!root.contains("message_id") || !root.contains("offset") ||
		!ParseJsonUInt64(root["message_id"], message_id) ||
		!ParseJsonUInt64(root["offset"], offset) || message_id == 0) {
		json rtvalue;
		rtvalue["error"] = ErrorCodes::Error_Json;
		SendRspFrame(session, ID_RESOURCE_CHUNK_DOWN_RSP, rtvalue); //二进制
		return;
	}

	//P1c：1512 二进制帧（meta+raw）；鉴权失败走拦截路径同样二进制
	auto callback = [session](const json& result, const std::string& raw) {
		std::string body;
		if (llfc_wire::EncodeFrame(result.dump(), raw, &body)) {
			session->Send(body, ID_RESOURCE_CHUNK_DOWN_RSP);
		}
	};

	//下载只读最终文件（rename 后与写天然互斥）；固定路由保持同一 message_id
	//串行，鉴权/元数据由 DownloadWorker 内缓存解析（不再逐片查 DB）
	int index = ResourceWorkerIndex(static_cast<long long>(message_id), DOWN_LOAD_WORKER_COUNT);
	FileSystem::GetInstance()->PostChunkDownToQue(
		std::make_shared<ResourceChunkDownTask>(session, static_cast<long long>(message_id),
			static_cast<long long>(offset), callback, std::move(quota)),
		index
	);
}

void ResourceLogicWorker::handleResourceLogin(shared_ptr<CSession> session, const short& msg_type,
	const string& msg_data, shared_ptr<SessionTaskQuota> quota)
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
		session->Send(rtvalue.dump(), ID_RESOURCE_LOGIN_RSP);
		return;
	}

	//校验登录令牌: utoken_<uid>，缺失或不匹配即返回TokenInvalid且不绑定会话
	std::string stored;
	bool ok = RedisMgr::GetInstance()->Get(USERTOKENPREFIX + std::to_string(uid), stored);
	if (!ok || stored != token) {
		rtvalue["error"] = ErrorCodes::TokenInvalid;
		session->Send(rtvalue.dump(), ID_RESOURCE_LOGIN_RSP);
		return;
	}

	if (!session->TrySetAuth(uid)) {
		return;
	}
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["uid"] = uid;
	session->Send(rtvalue.dump(), ID_RESOURCE_LOGIN_RSP);
}

void ResourceLogicWorker::task_callback(std::shared_ptr<LogicNode> task)
{
	auto session = task->_session;
	if (!session->IsOpen()) {
		return;
	}
	short msg_type = task->_recvnode->_msg_type;
	std::string msg_data(task->_recvnode->_data, task->_recvnode->_cur_len);


	//登录鉴权握手是唯一允许在未认证状态下处理的消息
	if (msg_type == ID_RESOURCE_LOGIN_REQ) {
		auto call_back_iter = _fun_callbacks.find(msg_type);
		if (call_back_iter == _fun_callbacks.end()) {
			return;
		}
		(this->*call_back_iter->second)(session, msg_type, msg_data, task->_quota);
		return;
	}

	//鉴权门控：除登录外所有消息都要求会话已认证，否则返回TokenInvalid并关闭
	//连接（1512 拦截回包同样走二进制线协议，不回 JSON）
	if (!session->IsAuthed()) {
		short rsp_id = ReqToRspId(msg_type);
		if (rsp_id != 0) {
			json rtvalue;
			rtvalue["error"] = ErrorCodes::TokenInvalid;
			SendRspFrameAndClose(session, rsp_id, rtvalue);
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
	(this->*call_back_iter->second)(session, msg_type, msg_data, task->_quota);
}
