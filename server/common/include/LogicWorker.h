#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

/**
 * @brief 纯任务执行 worker
 *
 * 每个实例持有一条独立线程，按 FIFO 顺序串行执行投递进来的任务闭包。
 * 用于将逻辑处理从单连接 IO 线程剥离，并按 uid 分片固定到不同 worker，
 * 从而在保证同一 uid 任务有序的前提下获得并发。
 *
 * 停机语义：Stop() 幂等，调用后拒绝接收新任务，但会排空队列中已有
 * 的全部任务再退出工作线程；析构函数自动调用 Stop()。
 */
class LogicWorker
{
public:
	/// 任务类型，任意无参无返回值的可调用对象
	using Task = std::function<void()>;

	/// 构造函数，启动内部工作线程
	LogicWorker();

	/// 析构函数，确保排空并停止工作线程
	~LogicWorker();

	/**
	 * @brief 投递一个任务到队列尾部
	 * @param task 待执行的任务闭包
	 * @return 已停止时返回 false（任务未被入队），否则入队成功返回 true
	 */
	bool Post(Task task);

	/**
	 * @brief 幂等停止：拒绝新任务、排空既有任务、回收线程
	 *
	 * 可被多次安全调用。置 stopping 后唤醒工作线程，但工作线程会继续
	 * 执行队列中剩余的全部任务，直到队列为空才退出循环。
	 */
	void Stop();

private:
	/// 工作线程主循环：取任务执行，stopping 且队列为空时退出
	void RunLoop();

	/// 工作线程
	std::thread _work_thread;
	/// FIFO 任务队列
	std::queue<Task> _task_que;
	/// 保护队列与停止标志的互斥锁
	std::mutex _mutex;
	/// 唤醒工作线程的条件变量
	std::condition_variable _cv;
	/// 停止标志，置 true 后拒绝新任务、排空后退出循环
	std::atomic<bool> _stopping{false};
};
