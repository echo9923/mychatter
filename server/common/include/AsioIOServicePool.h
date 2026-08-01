#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>
#include <boost/asio.hpp>

/**
 * @brief Boost.Asio IO 服务池（显式拥有对象，不再是单例）
 *
 * 管理多个 io_context 实例及其对应的运行线程。新建立的连接以轮询
 * (Round-Robin) 方式分配到池中的某个 io_context 上。
 *
 * 生命周期由调用方持有：各服务在 main 中以显式线程数构造
 * （Gate=2，Chat/Resource=hardware_concurrency），并通过引用传给 CServer。
 *
 * 停机语义：Stop() 幂等——原子标志保证重复调用只执行一次停止流程；
 * 析构自动调用 Stop()。构造参数为 0 时归一为 1。
 */
class AsioIOServicePool
{
public:
	/// io_context 类型别名，每个实例代表一个独立的IO事件循环
	using IOService = boost::asio::io_context;
	/// work_guard 类型别名，用于阻止 io_context 在没有任务时退出 run()
	using Work = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
	/// work_guard 智能指针类型别名
	using WorkPtr = std::unique_ptr<Work>;

	/**
	 * @brief 构造 IO 服务池并启动每 context 一条运行线程
	 * @param size 池中 io_context 数量；传 0 时归一为 1
	 */
	explicit AsioIOServicePool(std::size_t size);

	/// 析构函数，调用 Stop() 停止全部 context 并 join 所有线程
	~AsioIOServicePool();

	AsioIOServicePool(const AsioIOServicePool&) = delete;
	AsioIOServicePool& operator=(const AsioIOServicePool&) = delete;

	/**
	 * @brief 以轮询方式获取下一个 io_context
	 * @return 当前轮询到的 io_context 引用
	 */
	boost::asio::io_context& GetIOService();

	/**
	 * @brief 幂等停止：停止所有 io_context、释放 work guard 并 join 全部线程
	 *
	 * 原子停止标志保证并发/重复调用只执行一次停止流程。
	 */
	void Stop();

private:
	/// io_context 容器
	std::vector<IOService> _ioServices;
	/// work_guard 容器
	std::vector<WorkPtr> _works;
	/// 每个 io_context 一条运行线程
	std::vector<std::thread> _threads;
	/// 轮询索引
	std::size_t _nextIOService;
	/// 停止标志，保证 Stop() 只生效一次
	std::atomic<bool> _stopped{false};
};
