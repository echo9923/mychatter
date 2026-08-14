#pragma once
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#include <functional>
#include <chrono>
#include <unordered_map>
#include "const.h"

class CSession;
struct ChatMessage;

// ---------- 头像通道任务（1031/1033 旧协议，保持不变） ----------
struct FileTask {
	FileTask(std::shared_ptr<CSession> session,  MSG_TYPES msg_type, int uid, std::string path, std::string name,
		int seq, int total_size, int trans_size, int last,
		std::string file_data,
		std::function<void(const json&)> callback,long long chat_msg_id=0,
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
	long long _chat_msg_id;   // 64 位：关联 chat_message.message_id
	int _sender;
	int _receiver;
	long long _thread_id;     // 64 位
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

// ---------- 资源消息任务（1037 上传 / 1047 下载） ----------
/// 上传一个分片（1037）：data 为 Base64，offset 为该分片在整文件中的字节偏移
struct ResourceChunkTask {
	ResourceChunkTask(std::shared_ptr<CSession> session, long long message_id,
		long long offset, std::string chunk_sha256, std::string file_data,
		std::function<void(const json&)> callback)
		:_session(session), _message_id(message_id), _offset(offset),
		_chunk_sha256(std::move(chunk_sha256)), _file_data(std::move(file_data)),
		_callback(std::move(callback))
	{}
	std::shared_ptr<CSession> _session;
	long long _message_id;
	long long _offset;
	std::string _chunk_sha256;  ///< 分片内容 SHA-256（小写 hex）
	std::string _file_data;     ///< Base64 编码的分片数据
	std::function<void(const json&)> _callback;
};

/// 下载一个分片（1047）：从最终文件（已通过 .part 原子改名）按 offset 读取
struct ResourceChunkDownTask {
	ResourceChunkDownTask(std::shared_ptr<CSession> session, long long message_id,
		long long offset, std::string file_path, unsigned long long total_size,
		std::function<void(const json&)> callback)
		:_session(session), _message_id(message_id), _offset(offset),
		_file_path(std::move(file_path)), _total_size(total_size), _callback(std::move(callback))
	{}
	std::shared_ptr<CSession> _session;
	long long _message_id;
	long long _offset;
	std::string _file_path;    ///< resource/<sender_uid>/<message_id>
	unsigned long long _total_size;
	std::function<void(const json&)> _callback;
};

/// FileWorker 内存中的上传会话：仅固定 worker 线程访问，无锁。
/// received 的权威真值是 .part 文件长度 + chat_message 行；本结构只是缓存，
/// 重启/逐出后按 .part 长度与 MySQL 重建。
struct UploadSession {
	long long message_id = 0;
	long long total_size = 0;
	std::string content_hash;  ///< 整文件 SHA-256（来自 chat_message.content_hash）
	unsigned long long received = 0;  ///< 已确认字节数（== .part 文件长度）
	int sender_id = 0;          ///< 会话持有者（权限校验：上传者必须是消息 sender）
	int recv_id = 0;
	std::chrono::steady_clock::time_point last_active;
};

class FileWorker
{
public:
	FileWorker();
	~FileWorker();
	void RegisterHandlers();
	/// 头像通道（1031）：按调用方给定的 index 投递
	void PostTask(std::shared_ptr<FileTask> task);
	/// 资源分片上传（1037）：必须在 ResourceWorkerIndex 选定的 worker 上执行
	void PostChunkTask(std::shared_ptr<ResourceChunkTask> task);
	/// 任意闭包（1041 进度查询等需访问 _upload_sessions 的操作，同 worker 串行化）
	void PostClosure(std::function<void()> fn);

	/// 上传会话缓存（仅本 worker 线程内调用）：未命中返回 nullptr
	std::shared_ptr<UploadSession> FindUploadSession(long long message_id);
	/// 从 MySQL 行 + .part 实际长度重建会话（仅本 worker 线程内调用）
	std::shared_ptr<UploadSession> LoadUploadSession(long long message_id,
		const std::shared_ptr<ChatMessage>& msg);

private:
	void task_callback(std::shared_ptr<FileTask>);
	//资源上传分片状态机（1037）：校验-写入-完成判定
	void HandleResourceChunk(std::shared_ptr<ResourceChunkTask> task);
	//完成点：整文件 SHA-256 校验 -> .part 原子改名 -> 事务置 Ready+同步行 -> gRPC 通知
	void CompleteResourceUpload(std::shared_ptr<ResourceChunkTask> task,
		std::shared_ptr<UploadSession> session);
	//空闲会话逐出（30 分钟无活动）
	void EvictIdleUploadSessions();

	std::unordered_map<MSG_TYPES, std::function<void(std::shared_ptr<FileTask>)> > _handlers;
	///< 上传会话表：key=message_id，仅 worker 线程访问（固定路由保证单线程访问）
	std::unordered_map<long long, std::shared_ptr<UploadSession>> _upload_sessions;
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
	/// 资源分片下载（1047）：必须在 ResourceWorkerIndex 选定的 worker 上执行
	void PostChunkTask(std::shared_ptr<ResourceChunkDownTask> task);
private:
	void task_callback(std::shared_ptr<DownloadTask>);
	void HandleResourceChunkDown(std::shared_ptr<ResourceChunkDownTask> task);
	std::thread _work_thread;
	std::queue<std::function<void()>> _task_que;
	std::atomic<bool> _b_stop;
	std::mutex  _mtx;
	std::condition_variable _cv;
};
