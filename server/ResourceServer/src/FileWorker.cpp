#include "FileWorker.h"
#include "CSession.h"
#include "base64.h"
#include "ConfigMgr.h"
#include "MysqlMgr.h"
#include "RedisMgr.h"
#include "UserMessageNotifier.h"
#include "Sha256.h"
#include "FileInfo.h"
#include "ResourceUploadFile.h"

#include <fstream>

namespace {
/// UploadSession 空闲逐出阈值
constexpr auto kSessionIdleTimeout = std::chrono::minutes(30);
/// P2 秒传挑战有效期：超时即失效（客户端 2118 后经 1507 重取新挑战）
constexpr long long kInstantProofChallengeTtlSeconds = 120;
/// 上传分片大小：线协议 v1 固定 32KiB（与客户端 MAX_FILE_LEN 一致），
/// .part 恢复按该单位回退末片；P1c 线协议 v2 换用新常量。
constexpr unsigned long long kUploadChunkSize = kResourceUploadChunkSize;

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

/// P2 内容寻址存储布局（根路径来自 ConfigMgr `[Storage]/[Output] ResourceRoot`，
/// 缺省 `<Output.Path>/resource`）：
///   uploading/<message_id>.part   进行中上传（不再按发送者分目录）
///   blobs/<sha[0:2]>/<sha256>     已发布内容寻址 blob
/// blob 只会在 哈希校验通过 → flush → no-replace 持久化重命名 之后出现，
/// 不新建 resource_blobs 表。但**"同名文件存在"不等于"内容可信"**（磁盘损坏/
/// 外部改动/上一轮发布半途失败都可能留下坏 blob）：去重命中必须复核
/// 长度 + 整文件哈希 + 耐久性，任一不符就保留 .part 与既有 blob 并让客户端
/// 重试（2104），绝不覆盖已发布的 blob。
/// 旧 resource/<sender_uid>/<message_id> 布局已废弃（部署约定清空重建，
/// 清理任务不自动删除旧布局文件）。
/// 路径推导唯一真值：llfc::ResourceUploadingPartPath / llfc::ResourceBlobPath
///（ResourceUploadFile.h），本文件与 1507 判定、清理任务共用。
boost::filesystem::path UploadingPartPath(long long message_id) {
	return llfc::ResourceUploadingPartPath(
		ConfigMgr::Inst().GetResourceRootPath(), message_id);
}

boost::filesystem::path BlobPathOf(const std::string& sha256_hex) {
	return llfc::ResourceBlobPath(
		ConfigMgr::Inst().GetResourceRootPath(), sha256_hex);
}

/// 读取文件磁盘实际长度：不存在视为 0；元数据/权限错误返回 false。
/// 恢复路径一律以此为准，不信任内存计数（防虚报进度）。
bool DiskFileLength(const boost::filesystem::path& path, unsigned long long& size) {
	boost::system::error_code ec;
	const auto file_status = boost::filesystem::status(path, ec);
	if (file_status.type() == boost::filesystem::file_not_found) {
		size = 0;
		return true;
	}
	if (ec) return false;
	const auto length = boost::filesystem::file_size(path, ec);
	if (ec) return false;
	size = static_cast<unsigned long long>(length);
	return true;
}

/// 秒传（1513/1514）成功路径清理：秒传不写字节，正常情况下
/// uploading/<message_id>.part 从未创建；但同一 message_id 的历史尝试可能
/// 留下残缺 .part（恢复规则会把 ≤1 片的残留回退为 0 长度）。blob 已是唯一
/// 内容真值，直接删除这份中间产物，不必等 7 天保留期的清理任务；删除失败
/// （被占用等）交清理任务兜底，不影响成功回包。
void RemoveStubPart(long long message_id) {
	const auto stub_part = UploadingPartPath(message_id);
	boost::system::error_code rm_ec;
	boost::filesystem::remove(stub_part, rm_ec);
	if (rm_ec) {
		std::cerr << "ResourceServer: stub part remove deferred msg="
			<< message_id << " ec=" << rm_ec.message() << std::endl;
	}
}

} // namespace

FileWorker::FileWorker()
{
	RegisterHandlers();
}

FileWorker::~FileWorker()
{
	// _executor（最后声明的成员）析构时排空队列并 join 工作线程，
	// 排空期间其余成员仍存活，任务可安全访问。
}

void FileWorker::Stop()
{
	_executor.Stop();
}

void FileWorker::RegisterHandlers()
{
	//处理头像上传（独立头像通道，旧 seq 协议保持不变）
	_handlers[ID_UPLOAD_HEAD_ICON_REQ] = [this](std::shared_ptr<FileTask> task) {
		// 解码
		std::string decoded = base64_decode(task->_file_data);

		auto file_path_str = task->_path;
		auto last = task->_last;

		boost::filesystem::path file_path(file_path_str);
		boost::filesystem::path dir_path = file_path.parent_path();
		// 获取完整文件名（包含扩展名）
		std::string filename = file_path.filename().string();
		json result;
		result["error"] = ErrorCodes::Success;

		// Check if directory exists, if not, create it
		if (!boost::filesystem::exists(dir_path)) {
			if (!boost::filesystem::create_directories(dir_path)) {
				std::cerr << "Failed to create directory: " << dir_path.string() << std::endl;
				result["error"] = ErrorCodes::FileNotExists;
				task->_callback(result);
				return;
			}
		}


		std::ofstream outfile;
		//第一个包
		if (task->_seq == 1) {
			// 打开文件，如果存在则清空，不存在则创建
			outfile.open(file_path_str, std::ios::binary | std::ios::trunc);
		}
		else {
			// 保存为文件
			outfile.open(file_path_str, std::ios::binary | std::ios::app);
		}


		if (!outfile) {
			std::cerr << "无法打开文件进行写入。" << std::endl;
			result["error"] = ErrorCodes::FileWritePermissionFailed;
			task->_callback(result);
			return;
		}

		outfile.write(decoded.data(), decoded.size());
		if (!outfile) {
			std::cerr << "写入文件失败。" << std::endl;
			result["error"] = ErrorCodes::FileWritePermissionFailed;
			task->_callback(result);
			return;
		}

		outfile.close();
		if (last) {
			std::cout << "文件已成功保存为: " << task->_name << std::endl;
			//更新头像
			MysqlMgr::GetInstance()->UpdateUserIcon(task->_uid, filename);
			//获取用户信息
			auto user_info = MysqlMgr::GetInstance()->GetUser(task->_uid);
			if (user_info == nullptr) {
				return;
			}

			//将数据库内容写入redis缓存
			json redis_root;
			redis_root["uid"] = task->_uid;
			redis_root["name"] = user_info->username;
			redis_root["email"] = user_info->email;
			redis_root["nick"] = user_info->nickname;
			redis_root["desc"] = user_info->profile_bio;
			redis_root["sex"] = user_info->gender;
			redis_root["icon"] = user_info->avatar_key;
			std::string base_key = USER_BASE_INFO + std::to_string(task->_uid);
			RedisMgr::GetInstance()->Set(base_key, redis_root.dump(4));
		}

		if (task->_callback) {
			task->_callback(result);
		}
	};
}

void FileWorker::PostTask(std::shared_ptr<FileTask> task)
{
	_executor.Post([this, task]() {
		RunGuarded("file task", [this, task]() { task_callback(task); });
		});
}

void FileWorker::PostChunkTask(std::shared_ptr<ResourceChunkTask> task)
{
	_executor.Post([this, task]() {
		RunGuarded("resource chunk upload", [this, task]() { HandleResourceChunk(task); });
		});
}

void FileWorker::PostClosure(std::function<void(FileWorker&)> fn)
{
	_executor.Post([this, fn = std::move(fn)]() {
		RunGuarded("file worker closure", [this, &fn]() { fn(*this); });
		});
}

void FileWorker::task_callback(std::shared_ptr<FileTask> task)
{
	auto iter = _handlers.find(task->_msg_type);
	if (iter == _handlers.end()) {
		return;
	}

	iter->second(task);
}

std::shared_ptr<UploadSession> FileWorker::FindUploadSession(long long message_id) {
	auto iter = _upload_sessions.find(message_id);
	if (iter == _upload_sessions.end()) {
		return nullptr;
	}
	iter->second->last_active = std::chrono::steady_clock::now();
	return iter->second;
}

std::shared_ptr<UploadSession> FileWorker::LoadUploadSession(long long message_id,
	const std::shared_ptr<ChatMessage>& msg) {
	auto session = std::make_shared<UploadSession>();
	session->message_id = message_id;
	if (!msg->resource) return nullptr;
	session->file_size_bytes = static_cast<long long>(msg->resource->file_size_bytes);
	session->sha256 = msg->resource->sha256;
	session->sender_user_id = msg->sender_user_id;
	session->recipient_user_id = msg->recipient_user_id;
	//长度不能证明最后一片写完；仅在会话重建时回退，后续查询复用缓存。
	const auto part_path = UploadingPartPath(message_id).string();
	if (!llfc::RecoverUploadFile(part_path, msg->resource->file_size_bytes,
		kUploadChunkSize, session->received)) {
		std::cerr << "ResourceServer: upload recovery failed msg=" << message_id << std::endl;
		return nullptr;
	}
	//会话重建：received>0 时磁盘前缀未经过流式累计，增量哈希无效（完成点
	//回退整文件重算）；received==0 时 hasher（新对象）天然覆盖 [0,0)。
	session->hash_valid = (session->received == 0);
	session->final_hex.clear();
	session->last_active = std::chrono::steady_clock::now();
	_upload_sessions[message_id] = session;
	return session;
}

void FileWorker::EvictIdleUploadSessions() {
	const auto now = std::chrono::steady_clock::now();
	for (auto iter = _upload_sessions.begin(); iter != _upload_sessions.end();) {
		if (now - iter->second->last_active > kSessionIdleTimeout) {
			iter = _upload_sessions.erase(iter);
		}
		else {
			++iter;
		}
	}
}

void FileWorker::HandleResourceChunk(std::shared_ptr<ResourceChunkTask> task) {
	//会话已关闭（客户端断开/被踢）：不再写盘也不再回包，
	//已确认进度留在 .part 中供重连续传。
	if (!task->_session || !task->_session->IsOpen()) {
		return;
	}

	EvictIdleUploadSessions();

	//统一响应格式；资源发布后附带接收者事件序号。offset 回显请求值，
	//供客户端把回包与在途分片一一对应（P1b 引擎按此比对）。
	auto respond = [task](int error, unsigned long long server_offset,
		int status, unsigned long long event_seq = 0) {
		json result;
		result["error"] = error;
		result["message_id"] = std::to_string(task->_message_id);
		result["offset"] = std::to_string(task->_offset);
		result["server_offset"] = std::to_string(server_offset);
		result["status"] = status;
		if (event_seq > 0) result["event_seq"] = std::to_string(event_seq);
		if (task->_callback) {
			task->_callback(result);
		}
	};

	const int uid = task->_session->GetUserId();

	//会话缓存命中：活跃上传走内存，不打 DB
	auto session = FindUploadSession(task->_message_id);
	if (!session) {
		//未命中：按 MySQL 行 + .part 实际长度重建
		std::shared_ptr<ChatMessage> msg;
		switch (MysqlMgr::GetInstance()->GetChatMsgById(task->_message_id, msg)) {
		case MysqlDao::ChatMsgLookup::Found:
			break;
		case MysqlDao::ChatMsgLookup::NotFound:
			//确认不存在才是永久错误；Transient 按 2002 可重试，不误标
			respond(ErrorCodes::MsgIdErr, 0, -1);
			return;
		case MysqlDao::ChatMsgLookup::Transient:
		default:
			respond(ErrorCodes::RPCFailed, 0, -1);
			return;
		}
		if (uid == 0 || uid != msg->sender_user_id) {
			respond(ErrorCodes::ResourceForbidden, 0, -1);
			return;
		}
		if (msg->message_type != 1 && msg->message_type != 3) {
			respond(ErrorCodes::ResourceStateInvalid, 0, -1);
			return;
		}
		if (msg->status == MessageStatus::Published) {
			//已完成（重发末片/响应丢失重试）：幂等成功
			respond(ErrorCodes::Success, msg->resource->file_size_bytes,
				static_cast<int>(MessageStatus::Published), msg->event_seq);
			return;
		}
		if (msg->status == MessageStatus::Failed) {
			respond(ErrorCodes::ResourceStateInvalid, 0,
				static_cast<int>(MessageStatus::Failed));
			return;
		}
		session = LoadUploadSession(task->_message_id, msg);
		if (!session) {
			respond(ErrorCodes::RPCFailed, 0, -1);
			return;
		}
	}

	//权限：上传者必须是消息 sender（sender 取已认证会话，不信任客户端 JSON）
	if (uid == 0 || uid != session->sender_user_id) {
		respond(ErrorCodes::ResourceForbidden, 0, -1);
		return;
	}

	//P2：收到普通分片（1505）= 本消息已选择字节上传通道：立即作废未使用的
	//挑战并抑制后续下发。否则一条迟到/交错的 1513 可能在普通上传进行中用旧
	//挑战完成一次"秒传切换"（挑战与 .part 双轨状态），也让"2118 → 1507"永不
	//再次拿到挑战（双端防无限挑战闭环的服务端一侧）。
	session->challenge_active = false;
	session->challenge_suppressed = true;

	const auto part_path = UploadingPartPath(task->_message_id).string();
	const unsigned long long offset = static_cast<unsigned long long>(task->_offset);
	const unsigned long long received_before = session->received;
	const std::string& decoded = task->_file_data; //P1c：1505 raw 直传，不再 Base64
	const int error = llfc::WriteUploadChunk(part_path, session->file_size_bytes,
		kUploadChunkSize, offset, decoded, task->_chunk_sha256, session->received);
	if (error != ErrorCodes::Success) {
		//失败路径可能已回退/截断 received（重复片损坏回退、写失败回滚），
		//增量哈希与完成点缓存一律失效。
		session->hash_valid = false;
		session->final_hex.clear();
		std::cerr << "ResourceServer: upload chunk failed msg=" << task->_message_id
			<< " offset=" << task->_offset << " error=" << error << std::endl;
		respond(error, session->received, -1);
		return;
	}

	if (offset == received_before) {
		//连续追加（含新会话从 0 开始）：哈希同步推进，保持覆盖 [0, received)。
		//重复片命中路径（offset < received_before）不改变任何状态。
		if (session->hash_valid && !session->hasher.Update(decoded)) {
			session->hash_valid = false;  //EVP 失败：完成点回退整文件重算
		}
	}

	//完成判定（服务端按 total_size 判定，不信任客户端 last 字段）
	if (session->received == static_cast<unsigned long long>(session->file_size_bytes)) {
		CompleteResourceUpload(task, session);
		return;
	}

	respond(ErrorCodes::Success, session->received, static_cast<int>(MessageStatus::Pending));
}

void FileWorker::EvictUploadSession(long long message_id) {
	_upload_sessions.erase(message_id);
}

void FileWorker::CompleteResourceUpload(std::shared_ptr<ResourceChunkTask> task,
	std::shared_ptr<UploadSession> session) {
	//P2：最终文件即内容寻址 blob blobs/<sha[:2]>/<sha256>；.part 在
	//uploading/ 下。no-replace 重命名，目标已存在 = 去重命中（可信内容）。
	const auto blob_path = BlobPathOf(session->sha256);
	const auto part_path = UploadingPartPath(task->_message_id).string();
	const auto base_path = blob_path; //下方慢路径恢复逻辑沿用该命名

	//完成点失败且未终态化时消息仍为 PENDING，客户端重发末片可再触发。
	auto respond_error = [task](int error, unsigned long long server_offset, int status) {
		json result;
		result["error"] = error;
		result["message_id"] = std::to_string(task->_message_id);
		result["offset"] = std::to_string(task->_offset);
		result["server_offset"] = std::to_string(server_offset);
		result["status"] = status;
		if (task->_callback) {
			task->_callback(result);
		}
	};
	const auto respond_pending = [&respond_error](int error,
		unsigned long long server_offset) {
		respond_error(error, server_offset, static_cast<int>(MessageStatus::Pending));
	};

	//整文件 SHA-256：会话内连续追加时直接用流式哈希（不重读文件）；
	//重建/截断后增量失效则回退整文件重算（100MB 约数百 ms）。
	//结果缓存于 final_hex，重试完成不重复 finalize/重算。
	std::string full_hash;
	if (!session->final_hex.empty()) {
		full_hash = session->final_hex;
	}
	else {
		if (session->hash_valid) {
			session->final_hex = session->hasher.FinalHex();
		}
		if (session->final_hex.empty()) {
			session->final_hex = llfc::Sha256FileHex(part_path);
		}
		full_hash = session->final_hex;
	}
	if (full_hash.empty()) {
		session->final_hex.clear();
		respond_pending(ErrorCodes::FileReadFailed, session->received);
		return;
	}
	if (full_hash != session->sha256) {
		//整文件校验失败：分片校验都通过仍不匹配 = 源文件已变更或元数据分叉。
		//前两次删 .part 从 0 重传（2117）；同一会话累计第 3 次终态化为
		//Failed（2116），客户端停止重传。
		++session->whole_hash_mismatches;
		std::cerr << "ResourceServer: whole-file hash mismatch msg=" << task->_message_id
			<< " count=" << session->whole_hash_mismatches
			<< " expected=" << session->sha256 << " actual=" << full_hash << std::endl;
		if (session->whole_hash_mismatches >= 3) {
			if (!MysqlMgr::GetInstance()->MarkResourceFailed(task->_message_id)) {
				//置 Failed 失败（DB 异常）：保持 PENDING，客户端重试末片再触发
				respond_pending(ErrorCodes::RPCFailed, session->received);
				return;
			}
			boost::system::error_code rm_ec;
			boost::filesystem::remove(part_path, rm_ec);  //残留交给清理任务兜底
			_upload_sessions.erase(task->_message_id);
			respond_error(ErrorCodes::ResourceStateInvalid, 0,
				static_cast<int>(MessageStatus::Failed));
			return;
		}
		boost::system::error_code rm_ec;
		boost::filesystem::remove(part_path, rm_ec);
		if (rm_ec) {
			respond_pending(ErrorCodes::FileWritePermissionFailed, session->received);
			return;
		}
		session->received = 0;
		//从 0 重开：重置流式上下文，后续连续追加重新有效
		session->hasher.Reset();
		session->hash_valid = true;
		session->final_hex.clear();
		respond_pending(ErrorCodes::FileContentMismatch, 0);
		return;
	}

	//崩溃一致性：rename 前先把 .part 数据与元数据刷到物理存储
	if (!llfc::FlushFileToDisk(part_path)) {
		std::cerr << "ResourceServer: flush failed msg=" << task->_message_id << std::endl;
		respond_pending(ErrorCodes::FileWritePermissionFailed, session->received);
		return;
	}

	//结果不确定慢路径的 .part 恢复（rename 返回 false 但可能已生效、DB 提交
	//结果未确认等）。此时 base_path 已经过 整文件哈希 + flush + 持久化
	//rename，是磁盘上唯一可信的完整副本：Transient 不代表事务一定未提交
	//（commit 回执丢失、setAutoCommit 抛错都会走到这里，消息可能实际已是
	//Published），因此严禁把 base_path rename/move 回 .part——那会移走已
	//发布消息的最终文件、破坏 1509/1511 下载路径。.part 只能用 copy_file
	//从 base_path 复制恢复，并按磁盘实际长度校验；满进度恢复不了时按
	//.part 实际长度回退（绝不按内存 received 虚报满进度）并逐出会话，
	//客户端经 1507 按磁盘重建续传。
	auto restore_part_after_publish_failure = [&]() -> unsigned long long {
		const unsigned long long expected = session->received;
		unsigned long long part_len = 0;
		if (DiskFileLength(part_path, part_len) && part_len == expected) {
			return expected;  //.part 完整：rename 未发生或此前已恢复
		}
		unsigned long long base_len = 0;
		if (DiskFileLength(base_path, base_len) && base_len == expected) {
			//残缺 .part（上次复制中断的产物等）先让位再复制；
			//remove/copy_file 只触碰 .part，base_path 原样保留。
			boost::system::error_code rm_ec;
			boost::filesystem::remove(part_path, rm_ec);  //不存在时为 no-op
			boost::system::error_code copy_ec;
			boost::filesystem::copy_file(base_path, part_path, copy_ec);
			unsigned long long copied_len = 0;
			if (!rm_ec && !copy_ec
				&& DiskFileLength(part_path, copied_len) && copied_len == expected) {
				return expected;
			}
			std::cerr << "CompleteResourceUpload: copy_file restore failed msg="
				<< task->_message_id << " rm_ec=" << rm_ec.message()
				<< " copy_ec=" << copy_ec.message() << std::endl;
		}
		//满进度恢复失败：按 .part 磁盘实际长度回退到分片边界（缺失即 0），
		//逐出会话让 1507/末片重发按磁盘重建；base_path 保持不动。
		unsigned long long disk_received = 0;
		if (!llfc::RecoverUploadFile(part_path,
			static_cast<unsigned long long>(session->file_size_bytes),
			kUploadChunkSize, disk_received)) {
			disk_received = 0;
		}
		session->received = disk_received;
		_upload_sessions.erase(task->_message_id);
		std::cerr << "CompleteResourceUpload: full .part unavailable msg="
			<< task->_message_id << ", resume from " << disk_received
			<< " (final file kept)" << std::endl;
		return disk_received;
	};

	//跨目录持久化改名（no-replace）：.part -> blobs/<sha[:2]>/<sha>。
	//目标已存在（并发同内容上传/重试完成或遗留）= 去重命中：不能直接当成功
	//——必须复核目标长度 + 整文件哈希 + 耐久性（见下面的可信度判定），不可信
	//则保留 .part 与公开 blob 并回显式可重试错误。其余失败按结果不确定处理：
	//保留 blob、恢复 .part 后回可重试错误。
	//CAS/GC 互斥：从 rename 到发布事务提交持 BlobInstallGcMutex——清理
	//任务持同一锁做"孤儿判定查询 + unlink"，本临界区保证 GC 不会在
	//blob 已安装但发布未提交的空窗删掉它（取舍见 ResourceUploadFile.h）。
	unsigned long long event_seq = 0;
	std::shared_ptr<ChatMessage> canonical_msg;
	{
	std::lock_guard<std::mutex> cas_gc_lock(llfc::BlobInstallGcMutex());
	//blobs/<sha[:2]>/ 目录先行就绪（幂等）；POSIX 下整条祖先条目链都要 fsync
	//（不仅本次新建的层级：根/blobs 由 ConfigMgr 启动时建好，其条目可能尚未
	//落盘，重试时目录已存在也不会出现在"新建"集合里）。该调用在 CAS/GC 互斥
	//临界区内，代价是若干次目录 fsync（与路径深度成正比，实测吞吐影响未测）。
	if (!llfc::EnsureDirectoryDurable(base_path.parent_path())) {
		std::cerr << "ResourceServer: blob dir not durable msg=" << task->_message_id
			<< " dir=" << base_path.parent_path().string() << std::endl;
		respond_pending(ErrorCodes::FileWritePermissionFailed,
			restore_part_after_publish_failure());
		return;
	}
	bool dedup_hit = false;
	if (!llfc::DurableRename(part_path, base_path, false, &dedup_hit)) {
		std::cerr << "ResourceServer: durable rename failed (outcome unconfirmed) msg="
			<< task->_message_id << std::endl;
		respond_pending(ErrorCodes::FileWritePermissionFailed,
			restore_part_after_publish_failure());
		return;
	}
	if (dedup_hit) {
		//去重命中：目标 blob 已存在，但"存在"不等于"可信"——必须复核精确
		//长度 + 整文件 SHA-256，并确认数据可持久化（FlushFileToDisk 返回真），
		//全部通过才复用（回收本地 .part）。任一不通过/无法取证：保留本次
		//已校验的 .part 与已存在的公开 blob 原样不动，回显式可重试错误
		//（FileWritePermissionFailed）交重试/运维收敛。
		//绝不覆盖已有 blob（它可能正被其他已发布消息引用，覆盖等于静默损坏
		//已发布内容；同名内容与 sha 不符属运维异常，需要人工/部署处置），
		//也绝不把不可信的 blob 复制回 .part（那会毁掉唯一可信副本）。
		unsigned long long existing_len = 0;
		const bool len_ok = DiskFileLength(base_path, existing_len)
			&& existing_len == static_cast<unsigned long long>(
				session->file_size_bytes);
		const bool hex_ok = len_ok
			&& llfc::Sha256FileHex(base_path.string()) == session->sha256;
		if (!len_ok || !hex_ok || !llfc::FlushFileToDisk(base_path)) {
			std::cerr << "ResourceServer: dedup target not trusted (part and public"
				" blob kept, retryable) msg=" << task->_message_id
				<< " len_ok=" << len_ok << " hex_ok=" << hex_ok << std::endl;
			respond_pending(ErrorCodes::FileWritePermissionFailed,
				restore_part_after_publish_failure());
			return;
		}
		boost::system::error_code rm_ec;
		boost::filesystem::remove(part_path, rm_ec); //残留 .part 交清理兜底
		if (rm_ec) {
			std::cerr << "ResourceServer: dedup .part remove deferred msg="
				<< task->_message_id << " ec=" << rm_ec.message() << std::endl;
		}
	}

	//MySQL 是真值：按 message_id 读 canonical ChatMessage 取发送者和接收者，
	//不信任会话字段（可能与 canonical 分叉，污染错误用户的同步流）。
	//Transient 时按磁盘真值非破坏恢复 .part 供重试（2002），绝不误标永久失败。
	switch (MysqlMgr::GetInstance()->GetChatMsgById(task->_message_id, canonical_msg)) {
	case MysqlDao::ChatMsgLookup::Found:
		break;
	case MysqlDao::ChatMsgLookup::NotFound:
		std::cerr << "CompleteResourceUpload: canonical ChatMessage not found for message_id="
			<< task->_message_id << std::endl;
		_upload_sessions.erase(task->_message_id);
		respond_error(ErrorCodes::MsgIdErr, 0, static_cast<int>(MessageStatus::Failed));
		return;
	case MysqlDao::ChatMsgLookup::Transient:
	default:
		std::cerr << "CompleteResourceUpload: canonical lookup transient failure msg="
			<< task->_message_id << std::endl;
		respond_pending(ErrorCodes::RPCFailed, restore_part_after_publish_failure());
		return;
	}

	//只有 DB 发布和 event_seq 分配（单事务）成功才回包和投递通知；
	//死锁（1213/40001）已在 DAO 内有界重试。
	const auto publish_rc = MysqlMgr::GetInstance()->CompleteResourceUpload(
		task->_message_id, canonical_msg->sender_user_id, event_seq);
	switch (publish_rc) {
	case MysqlDao::PublishResult::Published:
	case MysqlDao::PublishResult::AlreadyPublished:
		break;
	case MysqlDao::PublishResult::NotPending:
		//清理任务等已把消息置为终态：永久错误，客户端停止重传
		std::cerr << "CompleteResourceUpload: message not pending msg="
			<< task->_message_id << ", publish refused" << std::endl;
		_upload_sessions.erase(task->_message_id);
		respond_error(ErrorCodes::ResourceStateInvalid, 0,
			static_cast<int>(MessageStatus::Failed));
		return;
	case MysqlDao::PublishResult::NotFound:
		std::cerr << "CompleteResourceUpload: row missing or sender mismatch msg="
			<< task->_message_id << std::endl;
		_upload_sessions.erase(task->_message_id);
		respond_error(ErrorCodes::MsgIdErr, 0, static_cast<int>(MessageStatus::Failed));
		return;
	case MysqlDao::PublishResult::Transient:
	default:
		//提交结果未确认（commit 可能已生效：setAutoCommit 抛错、回执丢失，
		//消息可能实际已是 Published 且 event_seq 已分配）：回送可重试错误、
		//绝不通知；base_path 一律保留，.part 走非破坏恢复，客户端重试末片后
		//经幂等发布（Published/AlreadyPublished）收敛。
		std::cerr << "CompleteResourceUpload: publish commit outcome unconfirmed msg="
			<< task->_message_id << ", final file kept, retryable" << std::endl;
		respond_pending(ErrorCodes::RPCFailed, restore_part_after_publish_failure());
		return;
	}
	} // BlobInstallGcMutex 临界区结束：blob 已安装且发布事务结果已定

	//上传完成，移除会话缓存
	_upload_sessions.erase(task->_message_id);

	//先回送上传成功响应（文件、PUBLISHED 状态和 event_seq 均已提交）。
	json result;
	result["error"] = ErrorCodes::Success;
	result["message_id"] = std::to_string(task->_message_id);
	result["offset"] = std::to_string(task->_offset);
	result["server_offset"] = std::to_string(session->received);
	result["status"] = static_cast<int>(MessageStatus::Published);
	result["event_seq"] = std::to_string(event_seq);
	if (task->_callback) {
		task->_callback(result);
	}

	//实时通知（1701）交给单线程 Notifier：Redis uip_ 查询与带退避 gRPC
	//不再占用 FileWorker；队列满/离线/失败均由接收者增量同步兜底。
	UserMessageNotifier::GetInstance()->Post(task->_message_id,
		canonical_msg->message_type, canonical_msg->recipient_user_id);
}

void FileWorker::HandleInstantProof(std::shared_ptr<CSession> req_session,
	int req_uid, long long message_id, const std::string& proof_hex,
	const std::function<void(const json&)>& respond) {
	//P2 秒传证明（1513，仅本 worker 线程）。判定顺序：
	//1) DB 真值（typed：Transient→2002 保留挑战；NotFound→2111 逐出会话）；
	//2) 请求 uid 必须是本消息 sender（proof 只能证明本人 Pending 资源）；
	//3) 已 Published → 直接成功回 1514 携带原 event_seq（不产生新事件、
	//   不投递 Notifier）——重试/迟到 1513 即使挑战已失效也幂等收敛；
	//4) 非 Pending → 2116；
	//5) 会话挑战：存在、未消费、未抑制、TTL 内、且 req_session 即下发时
	//   那条连接，否则 2118（客户端回退普通上传并重发 1507 重新同步）；
	//   只有合法持有者才消费挑战，"别人的连接/已过期"不作废挑战；
	//6) 合法持有者先消费挑战（一次性，防用同一挑战反复试探碰撞），再做
	//   proof 格式校验（64 hex）与常量时间比对 SHA256(nonce_raw16 ||
	//   blob[off, off+len))；摘要长度不为 64 视为不可信、绝不进入比对；
	//   不符/异常均抑制本会话（服务端防无限挑战闸门）后回 2118；
	//7) blob 区段读取 + 发布事务在 BlobInstallGcMutex 临界区内（与清理
	//   任务的"孤儿判定 + unlink"互斥），成功回包与 1701 通知在锁外；
	//8) 与普通上传共用发布事务（幂等），成功回 1514 并清理 .part 残留。
	auto fail_proof = [&](int error, int status) {
		json rtvalue;
		rtvalue["error"] = error;
		rtvalue["message_id"] = std::to_string(message_id);
		rtvalue["status"] = status;
		respond(rtvalue);
	};

	//1) DB 真值复核
	std::shared_ptr<ChatMessage> msg;
	switch (MysqlMgr::GetInstance()->GetChatMsgById(message_id, msg)) {
	case MysqlDao::ChatMsgLookup::Found:
		break;
	case MysqlDao::ChatMsgLookup::NotFound:
		_upload_sessions.erase(message_id);
		fail_proof(ErrorCodes::MsgIdErr, static_cast<int>(MessageStatus::Failed));
		return;
	case MysqlDao::ChatMsgLookup::Transient:
	default:
		//瞬时故障不作废挑战：客户端原样重试 1513 或 1507 重同步
		fail_proof(ErrorCodes::RPCFailed, static_cast<int>(MessageStatus::Pending));
		return;
	}
	//2) proof 请求只能由本消息 sender 发起
	if (req_uid == 0 || !msg->resource
		|| req_uid != msg->sender_user_id) {
		fail_proof(ErrorCodes::ResourceForbidden,
			static_cast<int>(msg->status));
		return;
	}
	//3) 已 Published：幂等重放成功（原 event_seq，无新事件、无 Notifier）
	if (msg->status == MessageStatus::Published) {
		_upload_sessions.erase(message_id);
		RemoveStubPart(message_id);
		json rtvalue;
		rtvalue["error"] = ErrorCodes::Success;
		rtvalue["message_id"] = std::to_string(message_id);
		rtvalue["status"] = static_cast<int>(MessageStatus::Published);
		rtvalue["server_offset"] = std::to_string(msg->resource->file_size_bytes);
		rtvalue["event_seq"] = std::to_string(msg->event_seq);
		respond(rtvalue);
		return;
	}
	//4) 其他终态（Failed 等）：非法操作
	if (msg->status != MessageStatus::Pending) {
		_upload_sessions.erase(message_id);
		fail_proof(ErrorCodes::ResourceStateInvalid, static_cast<int>(msg->status));
		return;
	}

	//5) 会话挑战校验：一次性 + TTL + 绑定下发连接
	auto it = _upload_sessions.find(message_id);
	if (it == _upload_sessions.end()) {
		//挑战随会话逐出/重启丢失 → 2118，客户端 1507 重同步获取新挑战
		fail_proof(ErrorCodes::InstantProofInvalid,
			static_cast<int>(MessageStatus::Pending));
		return;
	}
	auto session = it->second;
	const bool challenge_expired = session->challenge_active
		&& std::chrono::steady_clock::now() - session->challenge_issued_at
			> std::chrono::seconds(kInstantProofChallengeTtlSeconds);
	//不属于本挑战的持有者（无活跃挑战/已过期/来自别的连接）一律不动挑战：
	//只有"该挑战的合法请求方"才能消费它，别人不能替它作废。
	if (!session->challenge_active || challenge_expired
		|| session->challenge_session.lock() != req_session) {
		fail_proof(ErrorCodes::InstantProofInvalid,
			static_cast<int>(MessageStatus::Pending));
		return;
	}
	//到达这里 = 挑战有效（一次性 + TTL 内 + 绑定下发连接）：先作废再校验
	//proof 格式。格式非法也必须消费挑战并抑制本会话——否则同一挑战可被
	//反复拿去试探碰撞，也绕开"单次使用"约束。
	session->challenge_active = false;
	if (!llfc::IsValidSha256Hex(proof_hex)) {
		session->challenge_suppressed = true;
		fail_proof(ErrorCodes::InstantProofInvalid,
			static_cast<int>(MessageStatus::Pending));
		return;
	}

	//6)+7) blob 区段读取、常量时间比对与发布事务同处 BlobInstallGcMutex
	//临界区：与清理任务"孤儿判定查询 + unlink"互斥（与普通上传完成点同一
	//锁）。失败回包留在临界区里发送是可以的——CSession::Send 只向 socket
	//所属 IO 线程 post 一段闭包，不阻塞、不重入 FileWorker/清理代码；成功
	//回包与 1701 通知（含 Redis 投递，可能阻塞）移到锁外。
	unsigned long long event_seq = 0;
	{
	std::lock_guard<std::mutex> cas_gc_lock(llfc::BlobInstallGcMutex());
	const auto blob_path = BlobPathOf(msg->resource->sha256);
	std::ifstream in(blob_path.string(), std::ios::binary);
	if (!in.is_open()) {
		//blob 被清理/缺失：挑战失效，客户端 2118 回退普通上传
		session->challenge_suppressed = true;
		fail_proof(ErrorCodes::InstantProofInvalid,
			static_cast<int>(MessageStatus::Pending));
		return;
	}
	std::string section(session->challenge_length, '\0');
	in.seekg(static_cast<std::streamoff>(session->challenge_offset));
	in.read(&section[0], static_cast<std::streamsize>(section.size()));
	if (static_cast<unsigned long long>(in.gcount()) != session->challenge_length) {
		session->challenge_suppressed = true;
		fail_proof(ErrorCodes::FileReadFailed,
			static_cast<int>(MessageStatus::Pending));
		return;
	}
	const std::string expect = llfc::Sha256Hex(session->challenge_nonce_raw + section);
	if (expect.size() != 64) {
		//摘要异常（随机源/摘要上下文失败）：绝不能在长度不符时进入比对——
		//ConstantTimeEquals 对长度为 0 的输入返回"相等"，零长度摘要会直接把
		//任何请求放行。此处按可重试错误（2002）返回，挑战已消费。
		std::cerr << "ResourceServer: proof digest unavailable msg="
			<< message_id << std::endl;
		session->challenge_suppressed = true;
		fail_proof(ErrorCodes::RPCFailed, static_cast<int>(MessageStatus::Pending));
		return;
	}
	if (!llfc::ConstantTimeEquals(expect.data(), proof_hex.data(), expect.size())) {
		std::cerr << "ResourceServer: instant proof mismatch msg=" << message_id << std::endl;
		//证明不符：抑制本会话再次下发挑战（破无限挑战循环的服务端闸门；
		//客户端 2118 后本传输内同样禁用 proof，双端闭环）
		session->challenge_suppressed = true;
		fail_proof(ErrorCodes::InstantProofInvalid,
			static_cast<int>(MessageStatus::Pending));
		return;
	}

	switch (MysqlMgr::GetInstance()->CompleteResourceUpload(
		message_id, msg->sender_user_id, event_seq)) {
	case MysqlDao::PublishResult::Published:
	case MysqlDao::PublishResult::AlreadyPublished:
		break;
	case MysqlDao::PublishResult::NotPending:
		_upload_sessions.erase(message_id);
		fail_proof(ErrorCodes::ResourceStateInvalid, static_cast<int>(MessageStatus::Failed));
		return;
	case MysqlDao::PublishResult::NotFound:
		_upload_sessions.erase(message_id);
		fail_proof(ErrorCodes::MsgIdErr, static_cast<int>(MessageStatus::Failed));
		return;
	case MysqlDao::PublishResult::Transient:
	default:
		//提交结果未确认：挑战已消费。客户端重试 1513 → 若事务实际已提交
		//走上面 Published 幂等重放；若未提交 → 2118 → 1507 重同步拿新挑战
		//或回退普通上传，最终一致
		fail_proof(ErrorCodes::RPCFailed, static_cast<int>(MessageStatus::Pending));
		return;
	}
	} // BlobInstallGcMutex 释放：成功回包与 Notifier 不在临界区
	_upload_sessions.erase(message_id);
	RemoveStubPart(message_id);
	json rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["message_id"] = std::to_string(message_id);
	rtvalue["status"] = static_cast<int>(MessageStatus::Published);
	rtvalue["server_offset"] = std::to_string(msg->resource->file_size_bytes);
	rtvalue["event_seq"] = std::to_string(event_seq);
	respond(rtvalue);
	UserMessageNotifier::GetInstance()->Post(message_id,
		msg->message_type, msg->recipient_user_id);
}

DownloadWorker::DownloadWorker()
{
}

DownloadWorker::~DownloadWorker()
{
	// _executor（最后声明的成员）析构时排空队列并 join 工作线程，
	// 排空期间其余成员仍存活，任务可安全访问。
}

void DownloadWorker::Stop()
{
	_executor.Stop();
}

void DownloadWorker::PostTask(std::shared_ptr<DownloadTask> task)
{
	_executor.Post([this, task]() {
		RunGuarded("download task", [this, task]() { task_callback(task); });
		});
}

void DownloadWorker::PostChunkTask(std::shared_ptr<ResourceChunkDownTask> task)
{
	_executor.Post([this, task]() {
		RunGuarded("resource chunk down", [this, task]() { HandleResourceChunkDown(task); });
		});
}

void DownloadWorker::PostDownInfoTask(std::shared_ptr<ResourceDownInfoTask> task)
{
	_executor.Post([this, task]() {
		RunGuarded("resource down info", [this, task]() { HandleResourceDownInfo(task); });
		});
}

void DownloadWorker::task_callback(std::shared_ptr<DownloadTask> task)
{
	//头像下载（1603）：旧 seq + Redis 断点协议保持不变
	auto file_path_str = task->_file_path;

	boost::filesystem::path file_path(file_path_str);

	json result;
	result["error"] = ErrorCodes::Success;

	if (!boost::filesystem::exists(file_path)) {
		std::cerr << "文件不存在: " << file_path_str << std::endl;
		result["error"] = ErrorCodes::FileNotExists;
		task->_callback(result);
		return;
	}

	std::ifstream infile(file_path_str, std::ios::binary);
	if (!infile) {
		std::cerr << "无法打开文件进行读取。" << std::endl;
		result["error"] = ErrorCodes::FileReadPermissionFailed;
		task->_callback(result);
		return;
	}

	std::shared_ptr<FileInfo> file_info = nullptr;

	if (task->_seq == 1) {
		// 获取文件大小
		infile.seekg(0, std::ios::end);
		std::streamsize file_size = infile.tellg();
		infile.seekg(0, std::ios::beg);
		//如果为空，则创建FileInfo 构造数据存储
		file_info = std::make_shared<FileInfo>();
		file_info->_file_path_str = file_path_str;
		file_info->_name = task->_name;
		file_info->_seq = 1;

		file_info->_total_size = file_size;
		file_info->_trans_size = 0;
		// 立即保存到 Redis，覆盖旧数据，设置过期时间
		RedisMgr::GetInstance()->SetDownLoadInfo(task->_name, file_info);
		std::cout << "[新下载] 文件: " << task->_name
			<< ", 大小: " << file_size << " 字节" << std::endl;
	}
	else {
		//断点续传，从 Redis 获取历史信息
		file_info = RedisMgr::GetInstance()->GetDownloadInfo(task->_name);
		if (file_info == nullptr) {
			// Redis 中没有信息（可能过期了）
			std::cerr << "断点续传失败，Redis 中无下载信息: " << task->_name << std::endl;
			result["error"] = ErrorCodes::RedisReadErr;
			task->_callback(result);
			infile.close();
			return;
		}
		// 验证序列号是否匹配
		if (task->_seq != file_info->_seq) {
			std::cerr << "序列号不匹配，期望: " << file_info->_seq
				<< ", 实际: " << task->_seq << std::endl;
			result["error"] = ErrorCodes::FileSeqInvalid;
			task->_callback(result);
			infile.close();
			return;
		}

		//逐帧日志已去除（原 [续传] 打印在每个分片上触发）
	}

	// 计算当前偏移量
	std::streamsize offset = ((std::streamsize)task->_seq - 1) * MAX_FILE_LEN;
	if (offset >= file_info->_total_size) {
		std::cerr << "偏移量超出文件大小。" << std::endl;
		result["error"] = ErrorCodes::FileOffsetInvalid;
		task->_callback(result);
		infile.close();
		return;
	}

	// 定位到指定偏移量
	infile.seekg(offset);

	// 读取最多MAX_FILE_LEN字节
	char buffer[MAX_FILE_LEN];
	infile.read(buffer, MAX_FILE_LEN);
	//获取read实际读取多少字节
	std::streamsize bytes_read = infile.gcount();

	if (bytes_read <= 0) {
		std::cerr << "读取文件失败。" << std::endl;
		result["error"] = ErrorCodes::FileReadFailed;
		task->_callback(result);
		infile.close();
		return;
	}

	// 将读取的数据进行base64编码
	std::string data_to_encode(buffer, bytes_read);
	std::string encoded_data = base64_encode(data_to_encode);

	// 检查是否是最后一个包
	std::streamsize current_pos = offset + bytes_read;
	bool is_last = (current_pos >= file_info->_total_size);

	// 设置返回结果
	result["data"] = encoded_data;
	result["seq"] = task->_seq;
	result["total_size"] = std::to_string(file_info->_total_size);
	result["current_size"] = std::to_string(current_pos);
	result["is_last"] = is_last;

	infile.close();

	if (is_last) {
		std::cout << "文件读取完成: " << file_path_str << std::endl;
		RedisMgr::GetInstance()->DelDownLoadInfo(task->_name);
	}
	else {
		//更新信息
		file_info->_seq++;
		file_info->_trans_size = offset + bytes_read;
		//更新redis
		RedisMgr::GetInstance()->SetDownLoadInfo(task->_name, file_info);
	}

	if (task->_callback) {
		task->_callback(result);
	}
}

void DownloadWorker::HandleResourceDownInfo(std::shared_ptr<ResourceDownInfoTask> task) {
	auto respond_error = [task](int error) {
		json rtvalue;
		rtvalue["error"] = error;
		rtvalue["message_id"] = std::to_string(task->_message_id);
		if (task->_callback) {
			task->_callback(rtvalue);
		}
	};

	const int uid = task->_session ? task->_session->GetUserId() : 0;
	const PublishedCacheEntry* entry = nullptr;
	switch (ResolvePublished(task->_session, task->_message_id, uid, &entry)) {
	case ResolveStatus::Authorized:
		break;
	case ResolveStatus::Forbidden:
		respond_error(ErrorCodes::ResourceForbidden);
		return;
	case ResolveStatus::NotReady:
		respond_error(ErrorCodes::ResourceNotReady);
		return;
	case ResolveStatus::NotFound:
		respond_error(ErrorCodes::MsgIdErr);
		return;
	case ResolveStatus::DbError:
	default:
		respond_error(ErrorCodes::RPCFailed);
		return;
	}

	//最终文件必须存在（清理任务可能已回收）；file_size_bytes 统一用 DB 真值
	boost::system::error_code fs_ec;
	if (!boost::filesystem::exists(entry->file_path, fs_ec) || fs_ec) {
		std::cerr << "resource down info: file missing " << entry->file_path
			<< " ec=" << fs_ec.message() << std::endl;
		respond_error(ErrorCodes::FileNotExists);
		return;
	}

	json rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["message_id"] = std::to_string(task->_message_id);
	rtvalue["original_file_name"] = entry->original_file_name;
	rtvalue["file_size_bytes"] = std::to_string(entry->file_size_bytes);
	rtvalue["sha256"] = entry->sha256;
	rtvalue["mime_type"] = entry->mime_type;
	rtvalue["message_type"] = entry->message_type;
	rtvalue["status"] = static_cast<int>(MessageStatus::Published);
	if (task->_callback) {
		task->_callback(rtvalue);
	}
}

DownloadWorker::ResolveStatus DownloadWorker::ResolvePublished(
	const std::shared_ptr<CSession>& session, long long message_id, int uid,
	const PublishedCacheEntry** out_entry) {
	*out_entry = nullptr;
	auto iter = _published_cache.find(message_id);
	if (iter != _published_cache.end()) {
		auto& entry = iter->second;
		const auto cached_session = entry.first_session.lock();
		if (cached_session == session) {
			//同会话已鉴权：直接复用（1509/1511 高频路径不打 DB）
			*out_entry = &entry;
			return ResolveStatus::Authorized;
		}
		//不同会话：Published 行的资源字段不可变，直接比对 uid
		if (uid != 0 && (uid == entry.sender_user_id || uid == entry.recipient_user_id)) {
			entry.first_session = session;
			*out_entry = &entry;
			return ResolveStatus::Authorized;
		}
		return ResolveStatus::Forbidden;
	}

	//未命中：回查 DB；权限通过且 Published 才入缓存。
	//Transient（DB 瞬时故障）与 NotFound 分开：前者按 2002 可重试。
	std::shared_ptr<ChatMessage> msg;
	switch (MysqlMgr::GetInstance()->GetChatMsgById(message_id, msg)) {
	case MysqlDao::ChatMsgLookup::Found:
		break;
	case MysqlDao::ChatMsgLookup::NotFound:
		return ResolveStatus::NotFound;
	case MysqlDao::ChatMsgLookup::Transient:
	default:
		return ResolveStatus::DbError;
	}
	if (uid == 0 || (uid != msg->sender_user_id && uid != msg->recipient_user_id)) {
		return ResolveStatus::Forbidden;
	}
	if (msg->status != MessageStatus::Published) {
		return ResolveStatus::NotReady;
	}

	if (_published_cache.size() >= kPublishedCacheLimit) {
		//有界：整体清空（下轮回查 DB），不做精细 LRU
		_published_cache.clear();
	}
	PublishedCacheEntry entry;
	entry.first_session = session;
	entry.sender_user_id = msg->sender_user_id;
	entry.recipient_user_id = msg->recipient_user_id;
	entry.message_type = msg->message_type;
	//P2：下载路径由 message_resources.sha256 推出（内容寻址 blob）
	entry.file_path = BlobPathOf(msg->resource->sha256).string();
	entry.file_size_bytes = msg->resource->file_size_bytes;
	entry.sha256 = msg->resource->sha256;
	entry.original_file_name = msg->resource->original_file_name;
	entry.mime_type = msg->resource->mime_type;
	auto ins = _published_cache.emplace(message_id, std::move(entry));
	*out_entry = &ins.first->second;
	return ResolveStatus::Authorized;
}

void DownloadWorker::EvictIdleFileHandles() {
	const auto now = std::chrono::steady_clock::now();
	for (auto it = _file_handles.begin(); it != _file_handles.end();) {
		if (now - it->second.last_used > kFileHandleIdle) {
			it = _file_handles.erase(it);
		}
		else {
			++it;
		}
	}
}

void DownloadWorker::HandleResourceChunkDown(std::shared_ptr<ResourceChunkDownTask> task) {
	//P1c：1512 统一改为二进制帧 [u16 meta_len][meta][raw]——
	//meta = {error, message_id, offset, bytes, chunk_sha256, total_size,
	//is_last}，raw = 分片原始字节（错误分支 raw 为空；Base64 仅剩头像）
	auto respond = [task](int error, const std::string& raw, unsigned long long bytes,
		const std::string& chunk_hash, bool is_last, unsigned long long total) {
		json result;
		result["error"] = error;
		result["message_id"] = std::to_string(task->_message_id);
		result["offset"] = std::to_string(task->_offset);
		result["bytes"] = std::to_string(bytes);
		if (!chunk_hash.empty()) {
			result["chunk_sha256"] = chunk_hash;
		}
		result["total_size"] = std::to_string(total);
		result["is_last"] = is_last;
		if (task->_callback) {
			task->_callback(result, raw);
		}
	};

	const int uid = task->_session ? task->_session->GetUserId() : 0;
	const PublishedCacheEntry* entry = nullptr;
	switch (ResolvePublished(task->_session, task->_message_id, uid, &entry)) {
	case ResolveStatus::Authorized:
		break;
	case ResolveStatus::Forbidden:
		respond(ErrorCodes::ResourceForbidden, "", 0, "", false, 0);
		return;
	case ResolveStatus::NotReady:
		respond(ErrorCodes::ResourceNotReady, "", 0, "", false, 0);
		return;
	case ResolveStatus::NotFound:
		respond(ErrorCodes::MsgIdErr, "", 0, "", false, 0);
		return;
	case ResolveStatus::DbError:
	default:
		respond(ErrorCodes::RPCFailed, "", 0, "", false, 0);
		return;
	}

	const unsigned long long total = entry->file_size_bytes;
	const unsigned long long offset = static_cast<unsigned long long>(task->_offset);
	if (offset >= total) {
		//越界偏移（客户端游标分叉）
		respond(ErrorCodes::FileOffsetInvalid, "", 0, "", false, total);
		return;
	}

	//文件句柄 LRU：空闲 60s 淘汰；超上限淘汰最久未用，保持句柄有界
	EvictIdleFileHandles();
	auto handle_it = _file_handles.find(task->_message_id);
	if (handle_it == _file_handles.end()) {
		FileHandleEntry opened;
		opened.in.open(entry->file_path, std::ios::binary);
		if (!opened.in.is_open()) {
			//最终文件缺失（Ready 后被清理任务回收等）
			respond(ErrorCodes::FileNotExists, "", 0, "", false, total);
			return;
		}
		opened.last_used = std::chrono::steady_clock::now();
		auto ins = _file_handles.emplace(task->_message_id, std::move(opened));
		handle_it = ins.first;
		if (_file_handles.size() > kFileHandleLimit) {
			auto victim = _file_handles.end();
			for (auto it = _file_handles.begin(); it != _file_handles.end(); ++it) {
				if (it != handle_it
					&& (victim == _file_handles.end()
						|| it->second.last_used < victim->second.last_used)) {
					victim = it;
				}
			}
			if (victim != _file_handles.end()) {
				_file_handles.erase(victim);
			}
		}
	}
	else {
		handle_it->second.last_used = std::chrono::steady_clock::now();
	}

	std::ifstream& in = handle_it->second.in;
	in.clear();
	in.seekg(static_cast<std::streamoff>(offset));
	const unsigned long long want = std::min<unsigned long long>(
		kResourceUploadChunkSize, total - offset); //v2：256KiB 分片
	std::string buf(want, '\0');
	in.read(&buf[0], static_cast<std::streamsize>(want));
	const unsigned long long got = static_cast<unsigned long long>(in.gcount());
	if (got != want) {
		//句柄可能指向已被清理任务回收/替换的文件：淘汰后按读失败回包
		std::cerr << "ResourceServer: chunk read short msg=" << task->_message_id
			<< " offset=" << offset << " want=" << want << " got=" << got << std::endl;
		_file_handles.erase(handle_it);
		respond(ErrorCodes::FileReadFailed, "", 0, "", false, total);
		return;
	}

	respond(ErrorCodes::Success, buf, got,
		llfc::Sha256Hex(buf), offset + got >= total, total);
}
