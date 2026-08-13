#include "LogicWorker.h"
#include "FileSystem.h"
#include "CSession.h"
#include "LogicSystem.h"
#include "ConfigMgr.h"
#include "RedisMgr.h"
#include "MysqlMgr.h"

//将请求消息类型映射为对应的回复消息类型；无对应回复（通知类）或未知类型返回0
static short ReqToRspId(short msg_type)
{
	switch (msg_type) {
	case ID_UPLOAD_HEAD_ICON_REQ:           return ID_UPLOAD_HEAD_ICON_RSP;
	case ID_DOWN_LOAD_FILE_REQ:             return ID_DOWN_LOAD_FILE_RSP;
	case ID_IMG_CHAT_UPLOAD_REQ:            return ID_IMG_CHAT_UPLOAD_RSP;
	case ID_FILE_INFO_SYNC_REQ:             return ID_FILE_INFO_SYNC_RSP;
	case ID_IMG_CHAT_CONTINUE_UPLOAD_REQ:   return ID_IMG_CHAT_CONTINUE_UPLOAD_RSP;
	case ID_IMG_CHAT_DOWN_INFO_SYNC_REQ:    return ID_IMG_CHAT_DOWN_INFO_SYNC_RSP;
	case ID_IMG_CHAT_DOWN_REQ:              return ID_IMG_CHAT_DOWN_RSP;
	default:                                return 0;
	}
}

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
	_fun_callbacks[ID_UPLOAD_HEAD_ICON_REQ] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
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

	};

	_fun_callbacks[ID_DOWN_LOAD_FILE_REQ] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
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

	};

	_fun_callbacks[ID_IMG_CHAT_UPLOAD_REQ] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
			auto root = json::parse(msg_data, nullptr, false);
			auto md5 = root["md5"].get<std::string>();
			auto seq = root["seq"].get<int>();
			auto name = root["name"].get<std::string>();
			auto total_size_str = root["total_size"].get<std::string>();
			auto trans_size_str = root["trans_size"].get<std::string>();
			int64_t total_size = std::stoll(total_size_str);
			int64_t trans_size = std::stoll(trans_size_str);
			auto last = root["last"].get<int>();
			auto file_data = root["data"].get<std::string>();
			auto file_path = ConfigMgr::Inst().GetFileOutPath();
			auto uid = session->GetUserId();
			//上传资源时发送者即已认证上传者，不信任客户端 JSON 的 sender 字段
			auto sender = uid;
			auto receiver = root["receiver"].get<int>();
			auto message_id = root["message_id"].get<long long>(); //message_id 按 64 位解析（协议为数字）
			//转化为字符串
			auto uid_str = std::to_string(uid);
			auto file_path_str = (file_path / uid_str / name).string();
			json  rtvalue;

			auto callback = [=](const json& result) {

				// 在异步任务完成后调用
				json rtvalue = result;
				rtvalue["error"] = ErrorCodes::Success;
				rtvalue["total_size"] = std::to_string(total_size);
				rtvalue["seq"] = seq;
				rtvalue["name"] = name;
				rtvalue["trans_size"] = std::to_string(trans_size);
				rtvalue["last"] = last;
				rtvalue["md5"] = md5;
				rtvalue["uid"] = uid;
				rtvalue["sender"] = sender;
				rtvalue["receiver"] = receiver;
				std::string return_str = rtvalue.dump(4);
				session->Send(return_str, ID_IMG_CHAT_UPLOAD_RSP);
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
					session->Send(return_str, ID_IMG_CHAT_UPLOAD_RSP);
					return;
				}
			}
			else {
				auto file_info = RedisMgr::GetInstance()->GetFileInfo(name);
				if (file_info == nullptr) {
					rtvalue["error"] = ErrorCodes::FileNotExists;
					std::string return_str = rtvalue.dump(4);
					session->Send(return_str, ID_IMG_CHAT_UPLOAD_RSP);
					return;
				}
				file_info->_seq++;
				file_info->_trans_size = trans_size;
				bool success = RedisMgr::GetInstance()->SetFileInfo(name, file_info);
				if (!success) {
					rtvalue["error"] = ErrorCodes::FileSaveRedisFailed;
					std::string return_str = rtvalue.dump(4);
					session->Send(return_str, ID_IMG_CHAT_UPLOAD_RSP);
					return;
				}
			}


			FileSystem::GetInstance()->PostMsgToQue(
				std::make_shared<FileTask>(session, ID_IMG_CHAT_UPLOAD_REQ, uid, file_path_str, name, seq, total_size,
					trans_size, last, file_data, callback, message_id,sender,receiver),
				index
			);
	};	


	_fun_callbacks[ID_FILE_INFO_SYNC_REQ] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
			auto root = json::parse(msg_data, nullptr, false);
			auto md5 = root["md5"].get<std::string>();
			auto seq = root["seq"].get<int>();
			auto name = root["name"].get<std::string>();
			auto total_size_str = root["total_size"].get<std::string>();
			auto trans_size_str = root["trans_size"].get<std::string>();
			auto total_size = std::stoll(total_size_str);
			auto trans_size = std::stoll(trans_size_str);
			auto last = root["last"].get<int>();
			auto file_data = root["data"].get<std::string>();
			auto file_path = ConfigMgr::Inst().GetFileOutPath();
			auto uid = session->GetUserId();
			auto message_id = root["message_id"].get<long long>(); //message_id 按 64 位解析（协议为数字）
			//上传资源时发送者即已认证上传者，不信任客户端 JSON 的 sender 字段
			auto sender = uid;
			auto receiver = root["receiver"].get<int>();
			//转化为字符串
			auto uid_str = std::to_string(uid);
			auto file_path_str = (file_path / uid_str / name).string();
			json  rtvalue;

			auto callback = [=](const json& result) {

				// 在异步任务完成后调用
				json rtvalue = result;
				rtvalue["error"] = ErrorCodes::Success;		
				rtvalue["seq"] = seq;
				rtvalue["name"] = name;
				rtvalue["last"] = last;
				rtvalue["md5"] = md5;
				rtvalue["uid"] = uid;
				rtvalue["sender"] = sender;
				rtvalue["receiver"] = receiver;
				std::string return_str = rtvalue.dump(4);
				session->Send(return_str, ID_FILE_INFO_SYNC_RSP);
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
					session->Send(return_str, ID_FILE_INFO_SYNC_RSP);
					return;
				}
			}
			else {
				auto file_info = RedisMgr::GetInstance()->GetFileInfo(name);
				if (file_info == nullptr) {
					rtvalue["error"] = ErrorCodes::FileNotExists;
					std::string return_str = rtvalue.dump(4);
					session->Send(return_str, ID_FILE_INFO_SYNC_RSP);
					return;
				}
				file_info->_seq = seq;
				file_info->_trans_size = trans_size;
				bool success = RedisMgr::GetInstance()->SetFileInfo(name, file_info);
				if (!success) {
					rtvalue["error"] = ErrorCodes::FileSaveRedisFailed;
					std::string return_str = rtvalue.dump(4);
					session->Send(return_str, ID_FILE_INFO_SYNC_RSP);
					return;
				}
			}


			FileSystem::GetInstance()->PostMsgToQue(
				std::make_shared<FileTask>(session, ID_FILE_INFO_SYNC_REQ, uid, file_path_str, name, seq, total_size,
					trans_size, last, file_data, callback, message_id,sender,receiver),
				index
			);
	};



	_fun_callbacks[ID_IMG_CHAT_CONTINUE_UPLOAD_REQ] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
			auto root = json::parse(msg_data, nullptr, false);
			auto md5 = root["md5"].get<std::string>();
			auto seq = root["seq"].get<int>();
			auto name = root["name"].get<std::string>();
			auto total_size = root["total_size"].get<int>();
			auto trans_size = root["trans_size"].get<int>();
			auto last = root["last"].get<int>();
			auto file_data = root["data"].get<std::string>();
			auto file_path = ConfigMgr::Inst().GetFileOutPath();
			auto uid = session->GetUserId();
			auto message_id = root["message_id"].get<long long>(); //message_id 按 64 位解析（协议为数字）
			//上传资源时发送者即已认证上传者，不信任客户端 JSON 的 sender 字段
			auto sender = uid;
			auto receiver = root["receiver"].get<int>();
			//转化为字符串
			auto uid_str = std::to_string(uid);
			auto file_path_str = (file_path / uid_str / name).string();
			json  rtvalue;

			auto callback = [=](const json& result) {

				// 在异步任务完成后调用
				json rtvalue = result;
				rtvalue["error"] = ErrorCodes::Success;
				rtvalue["total_size"] = total_size;
				rtvalue["seq"] = seq;
				rtvalue["name"] = name;
				rtvalue["trans_size"] = trans_size;
				rtvalue["last"] = last;
				rtvalue["md5"] = md5;
				rtvalue["uid"] = uid;
				rtvalue["sender"] = sender;
				rtvalue["receiver"] = receiver;
				std::string return_str = rtvalue.dump(4);
				session->Send(return_str, ID_IMG_CHAT_CONTINUE_UPLOAD_RSP);
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
					session->Send(return_str, ID_IMG_CHAT_CONTINUE_UPLOAD_RSP);
					return;
				}
			}
			else {
				auto file_info = RedisMgr::GetInstance()->GetFileInfo(name);
				if (file_info == nullptr) {
					rtvalue["error"] = ErrorCodes::FileNotExists;
					std::string return_str = rtvalue.dump(4);
					session->Send(return_str, ID_IMG_CHAT_CONTINUE_UPLOAD_RSP);
					return;
				}
				file_info->_seq = seq;
				file_info->_trans_size = trans_size;
				bool success = RedisMgr::GetInstance()->SetFileInfo(name, file_info);
				if (!success) {
					rtvalue["error"] = ErrorCodes::FileSaveRedisFailed;
					std::string return_str = rtvalue.dump(4);
					session->Send(return_str, ID_IMG_CHAT_CONTINUE_UPLOAD_RSP);
					return;
				}
			}


			FileSystem::GetInstance()->PostMsgToQue(
				std::make_shared<FileTask>(session, ID_IMG_CHAT_CONTINUE_UPLOAD_REQ, uid, file_path_str, name, seq, total_size,
					trans_size, last, file_data, callback, message_id,sender,receiver),
				index
			);
	};

	_fun_callbacks[ID_IMG_CHAT_DOWN_INFO_SYNC_REQ] = [this](std::shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
			auto root = json::parse(msg_data, nullptr, false);
			auto message_id = root["message_id"].get<long long>(); //message_id 按 64 位解析（协议为数字）
			auto chat_msg = MysqlMgr::GetInstance()->GetChatMsgById(message_id);
			if (chat_msg == nullptr) {
				json rtvalue;
				rtvalue["error"] = ErrorCodes::MsgIdErr;
				return;
			}

			// 资源文件路径
			auto file_dir = ConfigMgr::Inst().GetFileOutPath();
			//该消息是接收方客户端发送过来的,服务器将资源存储在发送方的文件夹中
			auto uid_str = std::to_string(chat_msg->sender_id);
			auto file_path = (file_dir / uid_str / chat_msg->content);
			//文件可能缺失（头像/历史图片被清理）：用 error_code 重载，绝不让
			//boost::filesystem_error 穿出 LogicWorker 线程导致 std::terminate/abort
			boost::system::error_code fs_ec;
			boost::uintmax_t file_size = boost::filesystem::file_size(file_path, fs_ec);
			if (fs_ec) {
				std::cerr << "img down info sync: file missing " << file_path
				          << " ec=" << fs_ec.message() << std::endl;
				json err_value;
				err_value["error"] = ErrorCodes::FileNotExists;
				err_value["message_id"] = chat_msg->message_id;
				session->Send(err_value.dump(4), ID_IMG_CHAT_DOWN_INFO_SYNC_RSP);
				return;
			}

			json rtvalue ;
			rtvalue["error"] = ErrorCodes::Success;
			rtvalue["message_id"] = chat_msg->message_id;
			rtvalue["thread_id"] = chat_msg->thread_id;
			rtvalue["sender_id"] = chat_msg->sender_id;
			rtvalue["recv_id"] = chat_msg->recv_id;
			rtvalue["name"] = chat_msg->content;
			rtvalue["msg_type"] = chat_msg->msg_type;
			rtvalue["status"] = chat_msg->status;
			rtvalue["total_size"] = std::to_string(file_size);
 			std::string return_str = rtvalue.dump(4);
			session->Send(return_str, ID_IMG_CHAT_DOWN_INFO_SYNC_RSP);
	};

	_fun_callbacks[ID_IMG_CHAT_DOWN_REQ] = [this](std::shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {

			auto root = json::parse(msg_data, nullptr, false);

			auto seq = root["seq"].get<int>();
			auto name = root["name"].get<std::string>();
			auto total_size_str = root["total_size"].get<std::string>();
			auto trans_size_str = root["trans_size"].get<std::string>();
			auto file_path = ConfigMgr::Inst().GetFileOutPath();
			auto message_id = root["message_id"].get<long long>(); //message_id 按 64 位解析（协议为数字）
			auto sender = root["sender_id"].get<int>();
			auto receiver = root["receiver_id"].get<int>();
			auto uid = session->GetUserId();
			
			auto callback = [=](const json& result) {
				// 在异步任务完成后调用
				json rtvalue = result;
				rtvalue["error"] = ErrorCodes::Success;
				rtvalue["name"] = name;
				rtvalue["sender_id"] = sender;
				rtvalue["receiver_id"] = receiver;
				std::string return_str = rtvalue.dump(4);
				session->Send(return_str, ID_IMG_CHAT_DOWN_RSP);
			};

			// 使用 std::hash 对字符串进行哈希
			std::hash<std::string> hash_fn;
			size_t hash_value = hash_fn(name); // 生成哈希值
			int index = hash_value % DOWN_LOAD_WORKER_COUNT;
			std::cout << "Hash value: " << hash_value << std::endl;


			auto sender_str = std::to_string(sender);
			auto file_path_str = (file_path / sender_str / name).string();

		    auto down_load_task = std::make_shared<DownloadTask>(session, uid, name, seq, file_path_str, callback);

			FileSystem::GetInstance()->PostDownloadTaskToQue(down_load_task,index);
	};

	_fun_callbacks[ID_RESOURCE_LOGIN_REQ] = [this](shared_ptr<CSession> session, const short& msg_type,
		const string& msg_data) {
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

			session->SetAuth(uid);
			rtvalue["error"] = ErrorCodes::Success;
			rtvalue["uid"] = uid;
			session->Send(rtvalue.dump(4), ID_RESOURCE_LOGIN_RSP);
	};
}

void LogicWorker::task_callback(std::shared_ptr<LogicNode> task)
{
	auto session = task->_session;
	short msg_type = task->_recvnode->_msg_type;
	std::string msg_data(task->_recvnode->_data, task->_recvnode->_cur_len);

	cout << "recv_msg type is " << msg_type << endl;

	//登录鉴权握手是唯一允许在未认证状态下处理的消息
	if (msg_type == ID_RESOURCE_LOGIN_REQ) {
		auto call_back_iter = _fun_callbacks.find(msg_type);
		if (call_back_iter == _fun_callbacks.end()) {
			return;
		}
		call_back_iter->second(session, msg_type, msg_data);
		return;
	}

	//鉴权门控：除登录外所有消息都要求会话已认证，否则返回TokenInvalid并关闭连接
	if (!session->IsAuthed()) {
		short rsp_id = ReqToRspId(msg_type);
		if (rsp_id != 0) {
			json rtvalue;
			rtvalue["error"] = ErrorCodes::TokenInvalid;
			session->Send(rtvalue.dump(4), rsp_id);
		}
		session->Close();
		return;
	}

	auto call_back_iter = _fun_callbacks.find(msg_type);
	if (call_back_iter == _fun_callbacks.end()) {
		return;
	}
	call_back_iter->second(session, msg_type, msg_data);
}
