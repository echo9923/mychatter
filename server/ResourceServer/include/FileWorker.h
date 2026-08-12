#pragma once
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#include <functional>
#include "const.h"
#include <functional>
#include "const.h"
#include <unordered_map>

class CSession;
struct FileTask {
	FileTask(std::shared_ptr<CSession> session,  MSG_TYPES msg_type, int uid, std::string path, std::string name,
		int seq, int total_size, int trans_size, int last,
		std::string file_data,
		std::function<void(const json&)> callback,int chat_msg_id=0,
		int sender = 0, int receiver = 0) :_session(session), _msg_type(msg_type),_uid(uid),
		_seq(seq), _path(path), _name(name), _total_size(total_size),
		_trans_size(trans_size), _last(last), _file_data(file_data), _callback(callback), _chat_msg_id(chat_msg_id),
		_sender(sender), _receiver(receiver)
	{}
	~FileTask(){}
	std::shared_ptr<CSession> _session;
	MSG_TYPES _msg_type;
	int _uid;
	int _seq ;
	std::string _path;
	std::string _name ;
	int _total_size ;
	int _trans_size ;
	int _last ;
	std::string _file_data;
	std::function<void(const json&)>  _callback;  //添加回调函数
	int _chat_msg_id;
	int _sender;
	int _receiver;
	int _thread_id;
};


struct DownloadTask {
	DownloadTask(std::shared_ptr<CSession> session, int uid, std::string name,
		int seq, std::string file_path,
		std::function<void(const json&)> callback) :_session(session), _uid(uid),
		_seq(seq), _name(name), _file_path(file_path), _callback(callback)
	{}
	~DownloadTask() {}
	std::shared_ptr<CSession> _session;
	int _uid;
	int _seq;
	std::string _name;
	std::string _file_path;
	std::function<void(const json&)>  _callback;  //添加回调函数
};

class FileWorker
{
public:
	FileWorker();
	~FileWorker();
	void RegisterHandlers();
	void PostTask(std::shared_ptr<FileTask> task);
private:
	void task_callback(std::shared_ptr<FileTask>);
	//图片上传完成点统一处理：UpdateUploadStatus 成功后激活 pending（ZADD+EXPIRE）并尝试跨服 live 通知（计划5.7）
	void CompleteChatImageUpload(std::shared_ptr<FileTask> task);
	std::unordered_map<MSG_TYPES, std::function<void(std::shared_ptr<FileTask>)> > _handlers;
	std::thread _work_thread;
	std::queue<std::function<void()>> _task_que;
	std::atomic<bool> _b_stop;
	std::mutex  _mtx;
	std::condition_variable _cv;
};


class DownloadWorker {
public:
	DownloadWorker();
	~DownloadWorker();
	void PostTask(std::shared_ptr<DownloadTask> task);
private:
	void task_callback(std::shared_ptr<DownloadTask>);
	std::thread _work_thread;
	std::queue<std::shared_ptr<DownloadTask>> _task_que;
	std::atomic<bool> _b_stop;
	std::mutex  _mtx;
	std::condition_variable _cv;
};

