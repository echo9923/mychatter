#pragma once
#include <functional>
#include <memory>
#include <unordered_map>
#include "MsgNode.h"
#include "LogicWorker.h"  // server/common 执行器（重命名后不再与本头文件同名遮蔽）

class CSession;
class LogicNode {
public:
	LogicNode(std::shared_ptr<CSession>, std::shared_ptr<RecvNode>);
	shared_ptr<CSession> _session;
	shared_ptr<RecvNode> _recvnode;
};

class ResourceLogicWorker;

/// 消息处理回调：ResourceLogicWorker 的具名成员函数，注册处一行直达实现
typedef void (ResourceLogicWorker::*MsgHandler)(shared_ptr<CSession>,
	const short& msg_type, const string& msg_data);

/// ResourceServer 协议入口 worker（原 LogicWorker，因与 server/common 的
/// ::LogicWorker 同名遮蔽而重命名）。自身不再持有线程/队列，
/// 组合 common ::LogicWorker：任务在队列锁外执行，停机时排空全部任务，
/// 每个任务有独立异常边界（异常不逃逸出工作线程）。
class ResourceLogicWorker
{
public:
	ResourceLogicWorker();
	~ResourceLogicWorker();
	/// 投递一个待处理消息；执行器停机（拒绝新任务）时返回 false
	bool PostTask(std::shared_ptr<LogicNode> task);
	void RegisterCallBacks();
	/// 幂等停机：拒绝新任务并排空既有任务（透传 common 执行器语义）
	void Stop();
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
	std::unordered_map<short, MsgHandler> _fun_callbacks;
	/// 执行器（common ::LogicWorker）。必须声明为最后一个成员：
	/// 其析构会排空队列并执行仍引用本对象其余成员的任务，届时其余成员必须仍存活。
	::LogicWorker _executor;
};
