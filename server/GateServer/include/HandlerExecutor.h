#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

/**
 * @brief 有界任务执行器（计划2.1）
 *
 * 持有固定数量的工作线程与一条有界 FIFO 任务队列，用于将 GateServer
 * 的 HTTP handler 从连接 io_context 剥离到独立 worker 池执行，从而
 * 让阻塞型 handler（同步 mysql-concpp / hiredis / gRPC）不再卡住
 * 连接的 accept/read 与 60 秒 deadline。
 *
 * 容量语义：Post 在"运行中 + 排队"的任务总数达到 capacity 时拒绝入队，
 * 对调用方形成确定性反压，避免在突发流量下无限堆积。
 *
 * 停机语义：Stop() 幂等，调用后拒绝接收新任务，但会排空队列中已有
 * 的全部任务再回收全部工作线程；析构函数自动调用 Stop()。
 */
class HandlerExecutor
{
public:
	/// 任务类型，任意无参无返回值的可调用对象
	using Task = std::function<void()>;

	/**
	 * @brief 构造函数，按指定数量启动工作线程
	 * @param worker_count   工作线程数（0 视为 1）
	 * @param queue_capacity "运行中 + 排队"的总容量上限（0 视为 1）
	 */
	HandlerExecutor(std::size_t worker_count, std::size_t queue_capacity);

	/// 析构函数，确保排空并停止全部工作线程
	~HandlerExecutor();

	/**
	 * @brief 投递一个任务到队列尾部
	 * @param task 待执行的任务闭包
	 * @return 已停止，或"运行中 + 排队"达到容量上限时返回 false（任务未入队）；
	 *         否则入队成功返回 true
	 */
	bool Post(Task task);

	/**
	 * @brief 幂等停止：拒绝新任务、排空既有任务、回收全部线程
	 *
	 * 可被多次安全调用。置 stopping 后唤醒全部工作线程，但工作线程会
	 * 继续执行队列中剩余的全部任务，直到队列为空才退出循环。
	 */
	void Stop();

	/// 是否已进入停止状态（拒绝接收新任务）
	bool IsStopping() const noexcept;

private:
	/// 工作线程主循环：取任务执行，stopping 且队列为空时退出
	void RunLoop();

	/// 工作线程集合
	std::vector<std::thread> _workers;
	/// FIFO 任务队列
	std::queue<Task> _task_que;
	/// "运行中 + 排队"的任务总数（已投递未完成）
	std::size_t _outstanding{0};
	/// 容量上限（"运行中 + 排队"达到该值后 Post 返回 false）
	std::size_t _capacity;
	/// 保护队列、计数与停止标志的互斥锁
	std::mutex _mutex;
	/// 唤醒工作线程的条件变量
	std::condition_variable _cv;
	/// 停止标志，置 true 后拒绝新任务、排空后退出循环
	std::atomic<bool> _stopping{false};
};
