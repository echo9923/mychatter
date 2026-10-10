#pragma once
#include <cstddef>
#include "Singleton.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

/**
 * @brief 统一用户消息异步通知器（1701 实时投递的发送侧）
 *
 * FileWorker 在发布事务提交并回包 1506 之后，把接收者通知投递到这里；
 * Redis uip_ 在线查询和带退避的 gRPC NotifyUserMessage 全部在本通知器的
 * 单线程上执行，绝不在 FileWorker 线程上做任何等待。
 *
 * 单线程使任务按入队顺序串行执行；不同 FileWorker 的事务提交与入队仍可能
 * 交错，本层不保证跨消息的 event_seq 全序——漏投/乱序一律由接收端
 * 1405/1406 缺口同步兜底。队列有界（kNotifierQueueLimit），满时丢弃新
 * 任务并记日志，被丢弃的通知不影响已提交的 user_events 行。
 *
 * 停机语义：Stop() 幂等且必须在单例析构前显式调用（main 停机序列）。
 * Stop 后拒绝新任务；队列中尚未执行的任务直接放弃（尽力而为的实时
 * 通知，非可靠队列），正在执行的 gRPC 完成后线程退出并 join。
 */
class UserMessageNotifier : public Singleton<UserMessageNotifier>
{
	friend class Singleton<UserMessageNotifier>;
public:
	~UserMessageNotifier();
	/// 投递一条接收者通知（发布事务已提交后调用）。
	/// 返回 false = 队列满或已停机（任务被丢弃，由增量同步兜底）。
	bool Post(long long message_id, int event_type, int recipient_user_id);
	/// 幂等停机：拒绝新任务、放弃未执行任务、join 工作线程。
	void Stop();
private:
	UserMessageNotifier();
	void RunLoop();

	struct NotifyJob {
		long long message_id;
		int event_type;
		int recipient_user_id;
	};

	/// 有界队列上限；溢出即丢弃（1701 是尽力而为的实时通知）
	static constexpr std::size_t kNotifierQueueLimit = 4096;

	std::thread _thread;
	std::deque<NotifyJob> _queue;
	std::mutex _mutex;
	std::condition_variable _cv;
	std::atomic<bool> _stopping{false};
};
