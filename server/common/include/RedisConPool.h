#pragma once

#include <hiredis/hiredis.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

/**
 * @brief Redis连接池
 *
 * 管理多个Redis连接（hiredis），提供连接的获取、归还、健康检测和自动重连。
 * 内部启动一个检测线程，每60秒对空闲连接发送PING命令保活，
 * 失败的连接会被释放并重新创建。
 *
 * host/password 按值拥有，调用方传入的临时字符串不会产生悬空指针。
 * 停机语义：Close() 幂等——原子置停、唤醒借连接等待者与检测线程、
 * join 可 join 的检测线程并释放队列；析构自动走同一关闭路径。
 * 池停止后归还的非空连接立即 redisFree，不再入队。
 */
class RedisConPool {
public:
	/**
	 * @brief 构造连接池，预创建指定数量的Redis连接并启动健康检测线程
	 * @param poolSize 连接池大小
	 * @param host Redis服务器地址（按值拥有）
	 * @param port Redis服务器端口
	 * @param password Redis认证密码（按值拥有）
	 */
	RedisConPool(std::size_t poolSize, std::string host, int port, std::string password);

	/// 析构函数，走与 Close() 相同的幂等关闭路径
	~RedisConPool();

	/// 清空并释放所有空闲Redis连接
	void ClearConnections();

	/**
	 * @brief 从连接池获取一个可用Redis连接（阻塞等待）
	 * @return redisContext指针，若池已停止则返回nullptr
	 */
	redisContext* getConnection();

	/**
	 * @brief 将使用完毕的Redis连接归还到连接池
	 * @param context 要归还的Redis连接；nullptr 直接忽略，
	 *        池已停止时非空连接立即 redisFree
	 */
	void returnConnection(redisContext* context);

	/// 关闭连接池：幂等置停、唤醒所有等待线程、join检测线程并释放队列
	void Close();

private:
	/**
	 * @brief 从连接池获取一个Redis连接（非阻塞，无可用连接时立即返回nullptr）
	 * @return redisContext指针，无可用连接或已停止时返回nullptr
	 */
	redisContext* getConNonBlock();

	/// 重新创建一个Redis连接并加入池中
	bool reconnect();

	/// 连接健康检测（无锁版本），逐个取出连接发送PING，失败的移除后重连
	void checkThreadPro();

	std::atomic<bool> b_stop_;    ///< 停止标志
	std::size_t poolSize_;        ///< 连接池容量
	std::string host_;            ///< Redis服务器地址（按值拥有）
	std::string pwd_;             ///< Redis认证密码（按值拥有）
	int port_;                    ///< Redis服务器端口
	std::queue<redisContext*> connections_;  ///< Redis连接队列
	std::atomic<int> fail_count_; ///< 失败连接计数，用于触发重连
	std::mutex mutex_;            ///< 互斥锁，保护连接队列的线程安全
	std::condition_variable cond_; ///< 条件变量，无可用连接时阻塞等待
	std::thread  check_thread_;   ///< 健康检测线程
	int counter_;                 ///< 计时器，累计到60秒触发一次检测
	std::mutex stop_mutex_;       ///< 配合 stop_cv_ 让检测线程可被 Close 及时唤醒
	std::condition_variable stop_cv_; ///< Close 时唤醒睡眠中的检测线程
};
