#include "FileWorker.h"
#include "CSession.h"
#include "base64.h"
#include "ConfigMgr.h"
#include "MysqlMgr.h"
#include "RedisMgr.h"
#include "ChatServerGrpcClient.h"

namespace {
/// 从 [Delivery] 读取整数配置；非法/缺失时回退 fallback（计划4.2/5.7）
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
	}
	return fallback;
}

/// 对端 ChatServer RECIPIENT_OFFLINE 应用层错误码（计划5.7，只记录不重试）
constexpr int kAppRecipientOffline = 1015;
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
	_handlers[ID_UPLOAD_FILE_REQ] = [this](std::shared_ptr<FileTask> task) {
		// 解码
		std::string decoded = base64_decode(task->_file_data);

		auto file_path_str = task->_path;
		auto last = task->_last;
		//std::cout << "file_path_str is " << file_path_str << std::endl;

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
		}

		if (task->_callback) {
			task->_callback(result);
		}
	};

	//处理头像上传
	_handlers[ID_UPLOAD_HEAD_ICON_REQ] = [this](std::shared_ptr<FileTask> task) {
		// 解码
		std::string decoded = base64_decode(task->_file_data);

		auto file_path_str = task->_path;
		auto last = task->_last;
		//std::cout << "file_path_str is " << file_path_str << std::endl;

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

	//处理聊天图片上传
	_handlers[ID_IMG_CHAT_UPLOAD_REQ] = [this](std::shared_ptr<FileTask> task) {
		// 解码
		std::string decoded = base64_decode(task->_file_data);

		auto file_path_str = task->_path;
		auto last = task->_last;
		//std::cout << "file_path_str is " << file_path_str << std::endl;

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
			//图片上传完成：统一激活 pending + 跨服通知（计划5.7）
			CompleteChatImageUpload(task);
			return;
		}

		if (task->_callback) {
			task->_callback(result);
		}
	};

	//处理文件信息同步请求
	_handlers[ID_FILE_INFO_SYNC_REQ] = [this](std::shared_ptr<FileTask> task) {
		// 解码
		std::string decoded = base64_decode(task->_file_data);

		auto file_path_str = task->_path;
		auto last = task->_last;
		//std::cout << "file_path_str is " << file_path_str << std::endl;

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
			//图片上传完成：统一激活 pending + 跨服通知（计划5.7）
			CompleteChatImageUpload(task);
			return;
		}

		if (task->_callback) {
			task->_callback(result);
		}
	};

	//处理续传图片请求
	_handlers[ID_IMG_CHAT_CONTINUE_UPLOAD_REQ] = [this](std::shared_ptr<FileTask> task) {
		// 解码
		std::string decoded = base64_decode(task->_file_data);

		auto file_path_str = task->_path;
		auto last = task->_last;
		//std::cout << "file_path_str is " << file_path_str << std::endl;

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
			//图片上传完成：统一激活 pending + 跨服通知（计划5.7）
			CompleteChatImageUpload(task);
			return;
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

void FileWorker::task_callback(std::shared_ptr<FileTask> task)
{
	auto iter = _handlers.find(task->_msg_id);
	if (iter == _handlers.end()) {
		return;
	}

	iter->second(task);
}

void FileWorker::CompleteChatImageUpload(std::shared_ptr<FileTask> task)
{
	json result;
	result["error"] = ErrorCodes::Success;

	//只有 DB 状态迁移成功才激活 pending 并尝试 live RPC（计划5.7 不变式）
	if (!MysqlMgr::GetInstance()->UpdateUploadStatus(task->_chat_msg_id)) {
		//DB 失败：向当前 chunk callback 回送定义好的失败响应；绝不 ZADD、绝不 RPC
		std::cerr << "CompleteChatImageUpload: UpdateUploadStatus failed for chat_msg_id="
		          << task->_chat_msg_id << ", pending not activated, peer not notified" << std::endl;
		result["error"] = ErrorCodes::RPCFailed; //服务端完成失败，客户端据此重传上传
		if (task->_callback) {
			task->_callback(result);
		}
		return;
	}

	//MySQL 是真值：按 message_id 读 canonical ChatMessage，Redis key 与路由一律用其 recv_id，
	//不信任任务字段（task->_receiver 可能与 canonical 分叉，污染错误用户的 ZSET）
	auto canonical_msg = MysqlMgr::GetInstance()->GetChatMsgById(task->_chat_msg_id);
	if (canonical_msg == nullptr) {
		std::cerr << "CompleteChatImageUpload: canonical ChatMessage not found for chat_msg_id="
		          << task->_chat_msg_id << ", pending not activated, peer not notified" << std::endl;
		result["error"] = ErrorCodes::RPCFailed; //真值缺失：客户端据此重传上传
		if (task->_callback) {
			task->_callback(result);
		}
		return;
	}
	if (canonical_msg->recv_id != task->_receiver) {
		//任务字段与真值分叉：记录并一律以 canonical 为准
		std::cerr << "CompleteChatImageUpload: task receiver " << task->_receiver
		          << " diverges from canonical recv_id " << canonical_msg->recv_id
		          << " for msg_id=" << task->_chat_msg_id << ", using canonical" << std::endl;
	}

	//1) 激活离线 pending：ZADD offline_msg:<canonical recv_id>（score/member=message_id）+ EXPIRE
	auto receiver_str = std::to_string(canonical_msg->recv_id);
	auto offline_key = OFFLINE_MSG_PREFIX + receiver_str;
	auto member = std::to_string(task->_chat_msg_id);
	int ttl = ReadDeliveryInt("OfflineTtlSeconds", 604800);
	if (ttl < 1) ttl = 604800;
	bool zadd_ok = RedisMgr::GetInstance()->ZAdd(offline_key, (long long)task->_chat_msg_id, member);
	bool expire_ok = RedisMgr::GetInstance()->Expire(offline_key, ttl);
	if (!zadd_ok || !expire_ok) {
		//Redis 失败只日志：上传仍成功，登录拉取会用 MySQL 真值补回缺项（计划5.7/4.3）
		std::cerr << "CompleteChatImageUpload: activate pending ZSET failed msg_id="
		          << task->_chat_msg_id << " zadd=" << zadd_ok << " expire=" << expire_ok
		          << " (offline pull will fall back to MySQL)" << std::endl;
	}

	//先回送上传成功响应（图片已持久化、pending 已尽力激活）
	if (task->_callback) {
		task->_callback(result);
	}

	//2) 仅当接收者在线才尝试 live RPC；失败只日志，不影响离线拉取
	std::string uid_ip_value;
	auto uid_ip_key = USERIPPREFIX + receiver_str;
	bool b_ip = RedisMgr::GetInstance()->Get(uid_ip_key, uid_ip_value);
	if (!b_ip) {
		//接收者未登录：pending 已记录，由离线 pull 兜底，不做 live 推送
		return;
	}

	auto notify = ChatServerGrpcClient::GetInstance()->NotifyChatImgMsg(task->_chat_msg_id, uid_ip_value);
	if (notify.app_error == kAppRecipientOffline) {
		//RECIPIENT_OFFLINE：只记录 pending 状态，不重复重试（离线 pull 兜底，计划5.7）
		std::cout << "CompleteChatImageUpload: recipient offline msg_id="
		          << task->_chat_msg_id << ", pending retained for offline pull" << std::endl;
	}
	else if (notify.app_error != ErrorCodes::Success) {
		std::cerr << "CompleteChatImageUpload: NotifyChatImgMsg failed msg_id="
		          << task->_chat_msg_id << " grpc_code=" << notify.grpc_code
		          << " app_error=" << notify.app_error << " (offline pull will deliver)" << std::endl;
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

			auto task = _task_que.front();
			_task_que.pop();
			task_callback(task);
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
		_task_que.push(task);
	}

	_cv.notify_one();
}

void DownloadWorker::task_callback(std::shared_ptr<DownloadTask> task)
{
	// 解码
	auto file_path_str = task->_file_path;

	//std::cout << "file_path_str is " << file_path_str << std::endl;

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
