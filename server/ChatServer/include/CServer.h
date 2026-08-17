#pragma once
#include <boost/asio.hpp>
#include "AsioIOServicePool.h"
#include "CSession.h"
#include <memory.h>
#include <map>
#include <atomic>
#include <mutex>
#include <boost/asio/steady_timer.hpp>

using boost::asio::ip::tcp;

/**
 * @brief ChatServer TCP服务器核心类
 * 
 * 基于 Boost.Asio 实现的TCP服务器，负责：
 * - 监听指定端口并接受客户端连接
 * - 管理所有在线客户端会话(Session)的生命周期
 * - 提供心跳定时器，定期检测并清理超时会话
 * - 提供按uid查找/清除会话的接口，供gRPC服务层调用
 */
class CServer:public std::enable_shared_from_this<CServer>
{
public:
	/**
	 * @brief 构造函数，初始化服务器并开始监听
	 * @param io_context 主线程的IO上下文，用于acceptor和定时器
	 * @param port 服务器监听的TCP端口号
	 */
	CServer(boost::asio::io_context& io_context, short port, std::shared_ptr<AsioIOServicePool> pool);

	/// 析构函数，停止定时器并清理资源
	~CServer();

	/**
	 * @brief 心跳定时器回调函数，检测并清理心跳超时的会话
	 * @param ec 错误码，若定时器被取消则不执行清理逻辑
	 */
	void on_timer(const boost::system::error_code& ec);

	/// 启动心跳定时器，开始周期性检测超时会话
	void StartTimer();

	/// 停止心跳定时器
	void StopTimer();

	/// 幂等停止监听并让所有活跃会话走统一 Close 生命周期。
	void Stop();

	/**
	 * @brief 统计当前已认证（已登录）的会话数量
	 *
	 * 加锁拷贝 _sessions 后统计 GetUserId() > 0 的会话，用于 lease 上报负载。
	 * 刚 accept 尚未登录的连接不计入。
	 * @return 已认证会话数
	 */
	int GetAuthenticatedSessionCount();

private:
	friend class CSession;

	/// CSession::Close 的内部摘除步骤；同时校验 session id 与对象身份。
	void RemoveSession(const std::shared_ptr<CSession>& session);

	/**
	 * @brief 处理新连接接受完成的回调
	 * @param session 新创建的会话对象
	 * @param error 接受操作的错误码
	 */
	void HandleAccept(shared_ptr<CSession>, const boost::system::error_code & error);

	/// 发起下一次异步接受连接操作
	void StartAccept();

	/// Boost.Asio的核心I/O上下文，用于管理和分发所有的异步I/O操作
	boost::asio::io_context &_io_context;
	/// 服务器监听的网络端口号
	short _port;
	/// TCP接收器，负责监听指定端口并接受客户端的连接请求
	tcp::acceptor _acceptor;
	/// Asio IO 服务池（非单例，由 main 显式构造并传入）
	std::shared_ptr<AsioIOServicePool> _pool;
	/// 存储所有活跃客户端会话的映射表，键为用户唯一标识(如uid)，值为会话对象的共享指针
	std::map<std::string, shared_ptr<CSession>> _sessions;
	/// 互斥锁，用于保护_sessions等共享数据结构的线程安全访问
	std::mutex _mutex;
	/// 稳态定时器，用于执行周期性任务（心跳检测、超时断开等）
	boost::asio::steady_timer _timer;
	std::atomic<bool> _stopped{false};
};
