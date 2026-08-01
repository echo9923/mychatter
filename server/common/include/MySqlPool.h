#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include <jdbc/mysql_driver.h>
#include <jdbc/mysql_connection.h>
#include <jdbc/cppconn/prepared_statement.h>
#include <jdbc/cppconn/resultset.h>
#include <jdbc/cppconn/statement.h>
#include <jdbc/cppconn/exception.h>
#include <jdbc/cppconn/datatype.h>

/**
 * @brief MySQL连接封装类
 *
 * 封装一个MySQL数据库连接及其最后操作时间，
 * 用于连接池中的连接健康检测和保活。
 */
class SqlConnection {
public:
	/**
	 * @brief 构造函数
	 * @param con MySQL连接指针
	 * @param lasttime 最后一次操作的时间戳（秒）
	 */
	SqlConnection(sql::Connection* con, int64_t lasttime) :_con(con), _last_oper_time(lasttime) {}
	std::unique_ptr<sql::Connection> _con;  ///< MySQL数据库连接智能指针
	int64_t _last_oper_time;                ///< 最后一次操作的时间戳（用于保活检测）
};

/**
 * @brief MySQL连接池
 *
 * 管理多个MySQL数据库连接，提供连接的获取、归还、健康检测和自动重连功能。
 * 内部启动一个检测线程，首次 60 秒后、此后每 60 秒对空闲连接执行
 * "SELECT 1" 保活查询，失败的连接会被移除并重新创建。
 *
 * 停机语义：Close() 幂等——原子置停、唤醒健康线程与借连接等待者、
 * 仅在线程 joinable 时 join；析构走同一幂等且不抛异常的关闭路径。
 * 即使构造期连接创建抛出 sql::SQLException、检测线程尚未启动，
 * 显式 Close()/析构也安全。
 */
class MySqlPool {
public:
	/**
	 * @brief 构造连接池，预创建指定数量的MySQL连接并启动健康检测线程
	 * @param url MySQL服务器地址（如 "tcp://127.0.0.1:3306"）
	 * @param user 数据库用户名
	 * @param pass 数据库密码
	 * @param schema 数据库名
	 * @param poolSize 连接池大小
	 */
	MySqlPool(const std::string& url, const std::string& user, const std::string& pass, const std::string& schema, int poolSize);

	/// 析构函数，走与 Close() 相同的幂等关闭路径后清空连接队列
	~MySqlPool();

	/**
	 * @brief 从连接池获取一个可用连接（阻塞等待）
	 * @return MySQL连接智能指针，若池已停止则返回nullptr
	 */
	std::unique_ptr<SqlConnection> getConnection();

	/**
	 * @brief 将使用完毕的连接归还到连接池
	 * @param con 要归还的MySQL连接
	 */
	void returnConnection(std::unique_ptr<SqlConnection> con);

	/// 关闭连接池：幂等置停、唤醒健康线程与所有借连接等待者并 join 检测线程
	void Close();

private:
	/// 连接健康检测（无锁版本），逐个取出连接执行保活查询，失败的连接移除后重连
	void checkConnectionPro();

	/**
	 * @brief 重新创建一个MySQL连接并加入池中
	 * @param timestamp 当前时间戳
	 * @return 是否重连成功
	 */
	bool reconnect(long long timestamp);

	std::string url_;       ///< MySQL服务器地址
	std::string user_;      ///< 数据库用户名
	std::string pass_;      ///< 数据库密码
	std::string schema_;    ///< 数据库名
	int poolSize_;          ///< 连接池容量
	std::queue<std::unique_ptr<SqlConnection>> pool_;  ///< 连接队列
	std::mutex mutex_;      ///< 互斥锁，保护连接队列的线程安全
	std::condition_variable cond_;  ///< 条件变量，无可用连接时阻塞等待
	std::atomic<bool> b_stop_;      ///< 停止标志
	std::thread _check_thread;      ///< 健康检测线程，定期保活和重连
	std::atomic<int> _fail_count;   ///< 失败连接计数，用于触发重连
	std::mutex stop_mutex_;         ///< 配合 stop_cv_ 让健康线程可被 Close 及时唤醒
	std::condition_variable stop_cv_; ///< Close 时唤醒睡眠中的健康线程
};
