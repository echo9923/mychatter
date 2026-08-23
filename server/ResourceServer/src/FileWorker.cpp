#include "FileWorker.h"
#include "CSession.h"
#include "base64.h"
#include "ConfigMgr.h"
#include "MysqlMgr.h"
#include "RedisMgr.h"
#include "ChatServerGrpcClient.h"
#include "Sha256.h"
#include "FileInfo.h"

#include <fstream>

namespace {
	/// 对端 ChatServer RECIPIENT_OFFLINE 应用层错误码（20xx 通用表；只记录不重试，增量同步兜底）
	constexpr int kAppRecipientOffline = llfc_proto::ERR_RECIPIENT_OFFLINE;
/// UploadSession 空闲逐出阈值
constexpr auto kSessionIdleTimeout = std::chrono::minutes(30);

/// 资源文件路径：resource/<sender_uid>/<message_id>（进行中为 .part 后缀）
boost::filesystem::path ResourceFilePath(long long sender_id, long long message_id) {
	return ConfigMgr::Inst().GetResourceRootPath()
		/ std::to_string(sender_id) / std::to_string(message_id);
}

/// 读 .part 实际长度（磁盘真值）；文件不存在返回 0，出错返回 ~0ULL
unsigned long long PartFileLength(const boost::filesystem::path& part_path) {
	boost::system::error_code ec;
	if (!boost::filesystem::exists(part_path, ec)) {
		return 0;
	}
	boost::uintmax_t size = boost::filesystem::file_size(part_path, ec);
	if (ec) {
		std::cerr << "ResourceServer: file_size(" << part_path.string()
			<< ") ec=" << ec.message() << std::endl;
		return ~0ULL;
	}
	return static_cast<unsigned long long>(size);
}

/// 从 .part 读回 [offset, offset+len) 并计算 SHA-256；失败返回空串
std::string HashPartRange(const boost::filesystem::path& part_path,
	unsigned long long offset, unsigned long long len) {
	std::ifstream in(part_path.string(), std::ios::binary);
	if (!in) {
		return std::string();
	}
	in.seekg(static_cast<std::streamoff>(offset));
	std::string buf(len, '\0');
	in.read(&buf[0], static_cast<std::streamsize>(len));
	if (static_cast<unsigned long long>(in.gcount()) != len) {
		return std::string();
	}
	return llfc::Sha256Hex(buf);
}
} // namespace

FileWorker::FileWorker() :_b_stop(false)
{
	RegisterHandlers();
	_work_thread = std::thread([this]() {
		while (!_b_stop) {
			std::unique_lock<std::mutex> lock(_mtx);
			_cv.wait(lock, [this]() {
				if (_b_stop) {
					return true;
				}

				if (_task_que.empty()) {
					return false;
				}

				return true;
				});

			if (_b_stop) {
				break;
			}

			auto task_call = _task_que.front();
			_task_que.pop();
			task_call();
		}

		});
}

FileWorker::~FileWorker()
{
	_b_stop = true;
	_cv.notify_one();
	_work_thread.join();
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
			redis_root["name"] = user_info->name;
			redis_root["email"] = user_info->email;
			redis_root["nick"] = user_info->nick;
			redis_root["desc"] = user_info->desc;
			redis_root["sex"] = user_info->sex;
			redis_root["icon"] = user_info->icon;
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
	{
		std::lock_guard<std::mutex> lock(_mtx);
		//借鉴python万物皆对象思想，构造伪闭包将函数对象扔到队列中
		_task_que.push([task, this]() {
			task_callback(task);
			});
	}

	_cv.notify_one();
}

void FileWorker::PostChunkTask(std::shared_ptr<ResourceChunkTask> task)
{
	{
		std::lock_guard<std::mutex> lock(_mtx);
		_task_que.push([task, this]() {
			HandleResourceChunk(task);
			});
	}

	_cv.notify_one();
}

void FileWorker::PostClosure(std::function<void()> fn)
{
	{
		std::lock_guard<std::mutex> lock(_mtx);
		_task_que.push(std::move(fn));
	}

	_cv.notify_one();
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
	session->total_size = static_cast<long long>(msg->content_size);
	session->content_hash = msg->content_hash;
	session->sender_id = msg->sender_id;
	session->recv_id = msg->recv_id;
	//磁盘真值：.part 实际长度（ResourceServer 重启后据此续传）
	const auto part_path = ResourceFilePath(msg->sender_id, message_id).string() + ".part";
	session->received = PartFileLength(part_path);
	if (session->received == ~0ULL) {
		return nullptr;
	}
	if (session->received > static_cast<unsigned long long>(session->total_size)) {
		//.part 超过 total（异常残留）：截断为 0，从重头收
		std::cerr << "ResourceServer: part larger than total for msg " << message_id
			<< ", reset (" << session->received << " > " << session->total_size << ")" << std::endl;
		boost::system::error_code ec;
		boost::filesystem::remove(part_path, ec);
		session->received = 0;
	}
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
	EvictIdleUploadSessions();

	//统一响应格式：{error, message_id:"<str>", server_offset:"<str>", resource_status}
	auto respond = [task](int error, unsigned long long server_offset, int resource_status) {
		json result;
		result["error"] = error;
		result["message_id"] = std::to_string(task->_message_id);
		result["server_offset"] = std::to_string(server_offset);
		result["resource_status"] = resource_status;
		if (task->_callback) {
			task->_callback(result);
		}
	};

	const int uid = task->_session ? task->_session->GetUserId() : 0;

	//会话缓存命中：活跃上传走内存，不打 DB
	auto session = FindUploadSession(task->_message_id);
	if (!session) {
		//未命中：按 MySQL 行 + .part 实际长度重建
		auto msg = MysqlMgr::GetInstance()->GetChatMsgById(task->_message_id);
		if (!msg) {
			respond(ErrorCodes::MsgIdErr, 0, -1);
			return;
		}
		if (msg->resource_status == static_cast<int>(ResourceStatus::Ready)) {
			//已完成（重发末片/响应丢失重试）：幂等成功
			respond(ErrorCodes::Success, msg->content_size,
				static_cast<int>(ResourceStatus::Ready));
			return;
		}
		if (msg->resource_status == static_cast<int>(ResourceStatus::Expired)) {
			respond(ErrorCodes::ResourceStateInvalid, 0,
				static_cast<int>(ResourceStatus::Expired));
			return;
		}
		session = LoadUploadSession(task->_message_id, msg);
		if (!session) {
			respond(ErrorCodes::RPCFailed, 0, -1);
			return;
		}
	}

	//权限：上传者必须是消息 sender（sender 取已认证会话，不信任客户端 JSON）
	if (uid == 0 || uid != session->sender_id) {
		respond(ErrorCodes::ResourceForbidden, 0, -1);
		return;
	}

	//分片解码与尺寸校验
	std::string decoded = base64_decode(task->_file_data);
	if (decoded.empty() || decoded.size() > MAX_FILE_LEN) {
		respond(ErrorCodes::FileSizeExceeded, session->received, -1);
		return;
	}
	const unsigned long long offset = static_cast<unsigned long long>(task->_offset);
	const unsigned long long len = decoded.size();
	if (offset + len > static_cast<unsigned long long>(session->total_size)) {
		//越界：分片超出 total_size，永远无法收齐
		respond(ErrorCodes::FileOffsetInvalid, session->received, -1);
		return;
	}

	const auto base_path = ResourceFilePath(session->sender_id, task->_message_id);
	const auto part_path = base_path.string() + ".part";

	if (offset == session->received) {
		//顺序片：校验分片 SHA-256 后写入 .part 并读回验证
		if (!llfc::IsValidSha256Hex(task->_chunk_sha256) ||
			llfc::Sha256Hex(decoded) != task->_chunk_sha256) {
			std::cerr << "ResourceServer: chunk hash mismatch msg=" << task->_message_id
				<< " offset=" << offset << std::endl;
			respond(ErrorCodes::FileHashMismatch, session->received, -1);
			return;
		}

		boost::system::error_code ec;
		const auto dir = base_path.parent_path();
		if (!boost::filesystem::exists(dir) && !boost::filesystem::create_directories(dir, ec)) {
			std::cerr << "ResourceServer: create dir failed " << dir.string()
				<< " ec=" << ec.message() << std::endl;
			respond(ErrorCodes::CreateFilePathFailed, session->received, -1);
			return;
		}

		//写后读回校验：打开失败/写短/读回哈希不符都拒绝推进（防半写）
		{
			std::fstream io(part_path,
				std::ios::binary | std::ios::in | std::ios::out);
			if (!io) {
				//.part 不存在时用 out|trunc 语义创建（in|out 不建新文件）
				std::ofstream create(part_path, std::ios::binary | std::ios::app);
				if (!create) {
					respond(ErrorCodes::FileWritePermissionFailed, session->received, -1);
					return;
				}
				create.close();
				io.clear();
				io.open(part_path, std::ios::binary | std::ios::in | std::ios::out);
				if (!io) {
					respond(ErrorCodes::FileWritePermissionFailed, session->received, -1);
					return;
				}
			}
			io.seekp(static_cast<std::streamoff>(offset));
			io.write(decoded.data(), static_cast<std::streamsize>(len));
			io.flush();
			if (!io) {
				std::cerr << "ResourceServer: write failed msg=" << task->_message_id
					<< " offset=" << offset << std::endl;
				respond(ErrorCodes::FileWritePermissionFailed, session->received, -1);
				return;
			}
			io.seekg(static_cast<std::streamoff>(offset));
			std::string readback(len, '\0');
			io.read(&readback[0], static_cast<std::streamsize>(len));
			if (static_cast<unsigned long long>(io.gcount()) != len || readback != decoded) {
				std::cerr << "ResourceServer: readback mismatch msg=" << task->_message_id
					<< " offset=" << offset << std::endl;
				respond(ErrorCodes::FileWritePermissionFailed, session->received, -1);
				return;
			}
		}

		session->received += len;
	}
	else if (offset + len <= session->received) {
		//重复片（响应丢失后客户端重发）：读回该区间比对哈希，幂等确认，绝不追加两次
		if (HashPartRange(part_path, offset, len) != task->_chunk_sha256) {
			std::cerr << "ResourceServer: dup chunk hash mismatch msg=" << task->_message_id
				<< " offset=" << offset << " (data divergence)" << std::endl;
			respond(ErrorCodes::FileHashMismatch, session->received, -1);
			return;
		}
	}
	else {
		//offset 超前（洞）：返回服务端真实位置供客户端对齐重发
		respond(ErrorCodes::FileOffsetInvalid, session->received, -1);
		return;
	}

	//完成判定（服务端按 total_size 判定，不信任客户端 last 字段）
	if (session->received == static_cast<unsigned long long>(session->total_size)) {
		CompleteResourceUpload(task, session);
		return;
	}

	respond(ErrorCodes::Success, session->received, static_cast<int>(ResourceStatus::Uploading));
}

void FileWorker::CompleteResourceUpload(std::shared_ptr<ResourceChunkTask> task,
	std::shared_ptr<UploadSession> session) {
	const auto base_path = ResourceFilePath(session->sender_id, task->_message_id);
	const auto part_path = base_path.string() + ".part";

	//完成点统一失败响应（resource_status 仍为 Uploading，客户端重发末片可再触发）
	auto respond_error = [task, &session](int error, unsigned long long server_offset) {
		json result;
		result["error"] = error;
		result["message_id"] = std::to_string(task->_message_id);
		result["server_offset"] = std::to_string(server_offset);
		result["resource_status"] = static_cast<int>(ResourceStatus::Uploading);
		if (task->_callback) {
			task->_callback(result);
		}
	};

	//整文件 SHA-256 校验（内存增量上下文在重启后丢失，此处整文件重算，100MB 约数百 ms）
	const std::string full_hash = llfc::Sha256FileHex(part_path);
	if (full_hash.empty()) {
		respond_error(ErrorCodes::FileReadFailed, session->received);
		return;
	}
	if (full_hash != session->content_hash) {
		//整文件校验失败：删 .part 从 0 重传（分片校验都通过仍不匹配 = 会话元数据分叉）
		std::cerr << "ResourceServer: whole-file hash mismatch msg=" << task->_message_id
			<< " expected=" << session->content_hash << " actual=" << full_hash << std::endl;
		boost::system::error_code rm_ec;
		boost::filesystem::remove(part_path, rm_ec);
		session->received = 0;
		respond_error(ErrorCodes::FileHashMismatch, 0);
		return;
	}

	//同目录原子改名：.part -> 最终文件；rename 后下载侧才可见
	boost::system::error_code ec;
	if (boost::filesystem::exists(base_path, ec)) {
		boost::filesystem::remove(base_path, ec); //历史残留（重试完成路径）先清
	}
	boost::filesystem::rename(part_path, base_path, ec);
	if (ec) {
		std::cerr << "ResourceServer: rename failed msg=" << task->_message_id
			<< " ec=" << ec.message() << std::endl;
		respond_error(ErrorCodes::FileWritePermissionFailed, session->received);
		return;
	}

	//MySQL 是真值：按 message_id 读 canonical ChatMessage 取 sender/recv，
	//不信任会话字段（可能与 canonical 分叉，污染错误用户的同步流）
	auto canonical_msg = MysqlMgr::GetInstance()->GetChatMsgById(task->_message_id);
	if (canonical_msg == nullptr) {
		std::cerr << "CompleteResourceUpload: canonical ChatMessage not found for message_id="
			<< task->_message_id << ", sync rows not written, peer not notified" << std::endl;
		//最终文件保留：客户端重发末片可再触发完成（rename 后重复触发幂等）
		respond_error(ErrorCodes::RPCFailed, session->received);
		return;
	}

	//只有 DB 状态迁移 + 双方同步行写入（单事务）成功才尝试 live RPC（顺序不变式）
	if (!MysqlMgr::GetInstance()->CompleteResourceUploadWithSync(task->_message_id,
		canonical_msg->sender_id, canonical_msg->recv_id)) {
		//DB 失败：回送失败响应；绝不 RPC；最终文件保留（重发末片再触发）
		std::cerr << "CompleteResourceUpload: DB transaction failed for message_id="
			<< task->_message_id << ", sync rows not written, peer not notified" << std::endl;
		respond_error(ErrorCodes::RPCFailed, session->received);
		return;
	}

	//上传完成，移除会话缓存
	_upload_sessions.erase(task->_message_id);

	//先回送上传成功响应（文件已持久化、同步行已随事务提交）
	json result;
	result["error"] = ErrorCodes::Success;
	result["message_id"] = std::to_string(task->_message_id);
	result["server_offset"] = std::to_string(session->received);
	result["resource_status"] = static_cast<int>(ResourceStatus::Ready);
	if (task->_callback) {
		task->_callback(result);
	}

	//仅当接收者在线才尝试 live RPC；失败只日志，由 receiver 增量同步兜底
	auto receiver_str = std::to_string(canonical_msg->recv_id);
	std::string uid_ip_value;
	auto uid_ip_key = USERIPPREFIX + receiver_str;
	bool b_ip = RedisMgr::GetInstance()->Get(uid_ip_key, uid_ip_value);
	if (!b_ip) {
		//接收者未登录：同步行已记录，由增量同步兜底，不做 live 推送
		return;
	}

	auto notify = ChatServerGrpcClient::GetInstance()->NotifyChatResourceMsg(
		task->_message_id, canonical_msg->thread_id,
		canonical_msg->sender_id, canonical_msg->recv_id, uid_ip_value);
	if (notify.app_error == kAppRecipientOffline) {
		//RECIPIENT_OFFLINE：只记录，不重复重试（增量同步兜底）
		std::cout << "CompleteResourceUpload: recipient offline msg_id="
			<< task->_message_id << ", sync rows committed for incremental sync" << std::endl;
	}
	else if (notify.app_error != ErrorCodes::Success) {
		std::cerr << "CompleteResourceUpload: NotifyChatResourceMsg failed msg_id="
			<< task->_message_id << " grpc_code=" << notify.grpc_code
			<< " app_error=" << notify.app_error << " (incremental sync will deliver)" << std::endl;
	}
}

DownloadWorker::DownloadWorker() :_b_stop(false)
{
	_work_thread = std::thread([this]() {
		while (!_b_stop) {
			std::unique_lock<std::mutex> lock(_mtx);
			_cv.wait(lock, [this]() {
				if (_b_stop) {
					return true;
				}

				if (_task_que.empty()) {
					return false;
				}

				return true;
				});

			if (_b_stop) {
				break;
			}

			auto task_call = _task_que.front();
			_task_que.pop();
			task_call();
		}
		});
}

DownloadWorker::~DownloadWorker()
{
	_b_stop = true;
	_cv.notify_one();
	_work_thread.join();
}

void DownloadWorker::PostTask(std::shared_ptr<DownloadTask> task)
{
	{
		std::lock_guard<std::mutex> lock(_mtx);
		_task_que.push([task, this]() {
			task_callback(task);
			});
	}

	_cv.notify_one();
}

void DownloadWorker::PostChunkTask(std::shared_ptr<ResourceChunkDownTask> task)
{
	{
		std::lock_guard<std::mutex> lock(_mtx);
		_task_que.push([task, this]() {
			HandleResourceChunkDown(task);
			});
	}

	_cv.notify_one();
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

		std::cout << "[续传] 文件: " << task->_name
			<< ", seq: " << task->_seq
			<< ", 进度: " << file_info->_trans_size
			<< "/" << file_info->_total_size << std::endl;
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

void DownloadWorker::HandleResourceChunkDown(std::shared_ptr<ResourceChunkDownTask> task) {
	//1514 统一响应：{error, message_id, offset, bytes, chunk_sha256, data, total_size, is_last}
	auto respond = [task](int error, const std::string& data, unsigned long long bytes,
		const std::string& chunk_hash, bool is_last) {
		json result;
		result["error"] = error;
		result["message_id"] = std::to_string(task->_message_id);
		result["offset"] = std::to_string(task->_offset);
		result["bytes"] = std::to_string(bytes);
		if (!chunk_hash.empty()) {
			result["chunk_sha256"] = chunk_hash;
		}
		if (!data.empty()) {
			result["data"] = data;
		}
		result["total_size"] = std::to_string(task->_total_size);
		result["is_last"] = is_last;
		if (task->_callback) {
			task->_callback(result);
		}
	};

	const unsigned long long total = task->_total_size;
	const unsigned long long offset = static_cast<unsigned long long>(task->_offset);

	if (offset >= total) {
		//越界偏移（客户端游标分叉）
		respond(ErrorCodes::FileOffsetInvalid, "", 0, "", false);
		return;
	}

	std::ifstream in(task->_file_path, std::ios::binary);
	if (!in) {
		//最终文件缺失（Ready 后被清理任务回收等）
		respond(ErrorCodes::FileNotExists, "", 0, "", false);
		return;
	}
	in.seekg(static_cast<std::streamoff>(offset));

	const unsigned long long want = std::min<unsigned long long>(MAX_FILE_LEN, total - offset);
	std::string buf(want, '\0');
	in.read(&buf[0], static_cast<std::streamsize>(want));
	const unsigned long long got = static_cast<unsigned long long>(in.gcount());
	if (got != want) {
		std::cerr << "ResourceServer: chunk read short msg=" << task->_message_id
			<< " offset=" << offset << " want=" << want << " got=" << got << std::endl;
		respond(ErrorCodes::FileReadFailed, "", 0, "", false);
		return;
	}

	respond(ErrorCodes::Success, base64_encode(buf), got,
		llfc::Sha256Hex(buf), offset + got >= total);
}
