#pragma once
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <memory>
#include "MsgNode.h"
#include <unordered_map>

class CSession;
class LogicNode {
public:
	LogicNode(std::shared_ptr<CSession>, std::shared_ptr<RecvNode>);
	shared_ptr<CSession> _session;
	shared_ptr<RecvNode> _recvnode;
};

class LogicWorker;

/// 消息处理回调：LogicWorker 的具名成员函数，注册处一行直达实现
typedef void (LogicWorker::*MsgHandler)(shared_ptr<CSession>,
	const short& msg_type, const string& msg_data);

class LogicWorker
{
public:
	LogicWorker();
	~LogicWorker();
	void PostTask(std::shared_ptr<LogicNode> task);
	void RegisterCallBacks();
private:
	// 1601 头像分片上传
	void handleUploadHeadIcon(shared_ptr<CSession> session, const short& msg_type, const string& msg_data);
	// 1603 头像/旧文件下载
	void handleDownloadFile(shared_ptr<CSession> session, const short& msg_type, const string& msg_data);
	// 1505 资源分片上传
	void handleResourceChunkUpload(shared_ptr<CSession> session, const short& msg_type, const string& msg_data);
	// 1507 上传进度查询
	void handleResourceUploadProgress(shared_ptr<CSession> session, const short& msg_type, const string& msg_data);
	// 1509 资源下载元数据
	void handleResourceDownInfo(shared_ptr<CSession> session, const short& msg_type, const string& msg_data);
	// 1511 资源分片下载
	void handleResourceChunkDown(shared_ptr<CSession> session, const short& msg_type, const string& msg_data);
	// 1501 Resource 登录鉴权
	void handleResourceLogin(shared_ptr<CSession> session, const short& msg_type, const string& msg_data);
	void task_callback(std::shared_ptr<LogicNode>);
	std::thread _work_thread;
	std::queue<std::shared_ptr<LogicNode>> _task_que;
	std::atomic<bool> _b_stop;
	std::mutex  _mtx;
	std::condition_variable _cv;
	std::unordered_map<short, MsgHandler> _fun_callbacks;
};
