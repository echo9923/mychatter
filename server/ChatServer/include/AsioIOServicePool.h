#pragma once
#include <vector>
#include <thread>
#include <memory>
#include <boost/asio.hpp>
#include "Singleton.h"

/**
 * @brief Boost.Asio IO服务池（单例）
 * 
 * 管理多个 io_context 实例及其对应的运行线程，实现IO多线程并发处理。
 * 新建立的客户端Session会以轮询(Round-Robin)方式分配到池中的某个io_context上，
 * 从而实现负载均衡，避免单线程IO瓶颈。
 */
class AsioIOServicePool : public Singleton<AsioIOServicePool>
{
	friend Singleton<AsioIOServicePool>;
public:
	/// io_context 类型别名，每个实例代表一个独立的IO事件循环
	using IOService = boost::asio::io_context;
	/// work_guard 类型别名，用于阻止 io_context 在没有任务时退出 run()
	using Work = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
	/// work_guard 智能指针类型别名
	using WorkPtr = std::unique_ptr<Work>;

	/// 析构函数，停止所有IO线程并释放资源
	~AsioIOServicePool();
	/// 禁止拷贝构造
	AsioIOServicePool(const AsioIOServicePool&) = delete;
	/// 禁止拷贝赋值
	AsioIOServicePool& operator=(const AsioIOServicePool&) = delete;

	/**
	 * @brief 以轮询方式获取下一个可用的 io_context
	 * @return 当前轮询到的 io_context 引用，用于绑定新Session的异步IO操作
	 */
	boost::asio::io_context& GetIOService();

	/**
	 * @brief 停止IO服务池，终止所有io_context的事件循环并join所有线程
	 */
	void Stop();

private:
	/**
	 * @brief 私有构造函数，初始化指定数量的io_context并启动对应线程
	 * @param size 池中io_context的数量，默认为CPU硬件并发数
	 */
	AsioIOServicePool(std::size_t size = std::thread::hardware_concurrency());

	/// io_context 容器，存储池中所有的IO事件循环实例
	std::vector<IOService> _ioServices;
	/// work_guard 容器，防止对应的io_context在无任务时自动退出run()
	std::vector<WorkPtr> _works;
	/// 线程容器，每个线程运行一个io_context的run()事件循环
	std::vector<std::thread> _threads;
	/// 轮询索引，记录下一次 GetIOService() 应返回的io_context下标
	std::size_t _nextIOService;
};
