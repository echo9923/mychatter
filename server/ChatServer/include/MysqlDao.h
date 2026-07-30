#pragma once
#include "const.h"
#include <thread>
#include <jdbc/mysql_driver.h>
#include <jdbc/mysql_connection.h>
#include <jdbc/cppconn/prepared_statement.h>
#include <jdbc/cppconn/resultset.h>
#include <jdbc/cppconn/statement.h>
#include <jdbc/cppconn/exception.h>
#include "data.h"
#include <memory>
#include <queue>
#include <mutex>
#include "chat.pb.h"
using message::AddFriendMsg;
using message::TextChatData;

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
	SqlConnection(sql::Connection* con, int64_t lasttime):_con(con), _last_oper_time(lasttime){}
	std::unique_ptr<sql::Connection> _con;  ///< MySQL数据库连接智能指针
	int64_t _last_oper_time;                ///< 最后一次操作的时间戳（用于保活检测）
};

/**
 * @brief MySQL连接池
 * 
 * 管理多个MySQL数据库连接，提供连接的获取、归还、健康检测和自动重连功能。
 * 内部启动一个检测线程，每60秒对空闲连接执行 "SELECT 1" 保活查询，
 * 失败的连接会被移除并重新创建。
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
	MySqlPool(const std::string& url, const std::string& user, const std::string& pass, const std::string& schema, int poolSize)
		: url_(url), user_(user), pass_(pass), schema_(schema), poolSize_(poolSize), b_stop_(false), _fail_count(0){
		try {
			for (int i = 0; i < poolSize_; ++i) {
				sql::mysql::MySQL_Driver* driver = sql::mysql::get_mysql_driver_instance();
				auto*  con = driver->connect(url_, user_, pass_);
				con->setSchema(schema_);
				// 获取当前时间戳
				auto currentTime = std::chrono::system_clock::now().time_since_epoch();
				// 将时间戳转换为秒
				long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(currentTime).count();
				pool_.push(std::make_unique<SqlConnection>(con, timestamp));
				std::cout << "mysql connection init success" << std::endl;
			}

			_check_thread = 	std::thread([this]() {
				int count = 0;
				while (!b_stop_) {
					if (count >= 60) {
						count = 0;
						checkConnectionPro();
					}
					std::this_thread::sleep_for(std::chrono::seconds(1));
					count++;
				}
			});

			_check_thread.detach();
		}
		catch (sql::SQLException& e) {
			// 处理异常
			std::cout << "mysql pool init failed, error is " << e.what()<< std::endl;
		}
	}

	/// 连接健康检测（无锁版本），逐个取出连接执行保活查询，失败的连接移除后重连
	void checkConnectionPro() {
		// 1)先读取“目标处理数”
		size_t targetCount;
		{
			std::lock_guard<std::mutex> guard(mutex_);
			targetCount = pool_.size();
		}

		//2 当前已经处理的数量
		size_t processed = 0;

		//3 时间戳
		auto now = std::chrono::system_clock::now().time_since_epoch();
		long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(now).count();

		while (processed < targetCount) {
			std::unique_ptr<SqlConnection> con;
			{
				std::lock_guard<std::mutex> guard(mutex_);
				if (pool_.empty()) {
					break;
				}
				con = std::move(pool_.front());
				pool_.pop();
			}

			bool healthy = true;
			//解锁后做检查/重连逻辑
			if (timestamp - con->_last_oper_time >= 5) {
				try {
					std::unique_ptr<sql::Statement> stmt(con->_con->createStatement());
					stmt->executeQuery("SELECT 1");
					con->_last_oper_time = timestamp;
				}
				catch (sql::SQLException& e) {
					std::cout << "Error keeping connection alive: " << e.what() << std::endl;
					healthy = false;
					_fail_count++;
				}

			}

			if (healthy)
			{
				std::lock_guard<std::mutex> guard(mutex_);
				pool_.push(std::move(con));
				cond_.notify_one();
			}

			++processed;
		}

		while (_fail_count > 0) {
			auto b_res = reconnect(timestamp);
			if (b_res) {
				_fail_count--;
			}
			else {
				break;
			}
		}
	}

	/**
	 * @brief 重新创建一个MySQL连接并加入池中
	 * @param timestamp 当前时间戳
	 * @return 是否重连成功
	 */
	bool reconnect(long long timestamp) {
		try {

			sql::mysql::MySQL_Driver* driver = sql::mysql::get_mysql_driver_instance();
			auto* con = driver->connect(url_, user_, pass_);
			con->setSchema(schema_);

			auto newCon = std::make_unique<SqlConnection>(con, timestamp);
			{
				std::lock_guard<std::mutex> guard(mutex_);
				pool_.push(std::move(newCon));
			}
			std::cout << "mysql connection reconnect success" << std::endl;
			return true;

		}
		catch (sql::SQLException& e) {
			std::cout << "Reconnect failed, error is " << e.what() << std::endl;
			return false;
		}
	}


	/// 连接健康检测（有锁版本，已废弃，保留作为参考）
	void checkConnection() {
		std::lock_guard<std::mutex> guard(mutex_);
		int poolsize = pool_.size();
		// 获取当前时间戳
		auto currentTime = std::chrono::system_clock::now().time_since_epoch();
		// 将时间戳转换为秒
		long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(currentTime).count();
		for (int i = 0; i < poolsize; i++) {
			auto con = std::move(pool_.front());
			pool_.pop();
			Defer defer([this, &con]() {
				pool_.push(std::move(con));
			});

			if (timestamp - con->_last_oper_time < 5) {
				continue;
			}
			
			try {
				std::unique_ptr<sql::Statement> stmt(con->_con->createStatement());
				stmt->executeQuery("SELECT 1");
				con->_last_oper_time = timestamp;
				//std::cout << "execute timer alive query , cur is " << timestamp << std::endl;
			}
			catch (sql::SQLException& e) {
				std::cout << "Error keeping connection alive: " << e.what() << std::endl;
				// 重新创建连接并替换旧的连接
				sql::mysql::MySQL_Driver* driver = sql::mysql::get_mysql_driver_instance();
				auto* newcon = driver->connect(url_, user_, pass_);
				newcon->setSchema(schema_);
				con->_con.reset(newcon);
				con->_last_oper_time = timestamp;
			}
		}
	}

	/**
	 * @brief 从连接池获取一个可用连接（阻塞等待）
	 * @return MySQL连接智能指针，若池已停止则返回nullptr
	 */
	std::unique_ptr<SqlConnection> getConnection() {
		std::unique_lock<std::mutex> lock(mutex_);
		cond_.wait(lock, [this] { 
			if (b_stop_) {
				return true;
			}		
			return !pool_.empty(); });
		if (b_stop_) {
			return nullptr;
		}
		std::unique_ptr<SqlConnection> con(std::move(pool_.front()));
		pool_.pop();
		return con;
	}

	/**
	 * @brief 将使用完毕的连接归还到连接池
	 * @param con 要归还的MySQL连接
	 */
	void returnConnection(std::unique_ptr<SqlConnection> con) {
		std::unique_lock<std::mutex> lock(mutex_);
		if (b_stop_) {
			return;
		}
		pool_.push(std::move(con));
		cond_.notify_one();
	}

	/// 关闭连接池，唤醒所有等待线程使其退出
	void Close() {
		b_stop_ = true;
		cond_.notify_all();
	}

	/// 析构函数，清空连接池中所有连接
	~MySqlPool() {
		std::unique_lock<std::mutex> lock(mutex_);
		while (!pool_.empty()) {
			pool_.pop();
		}
	}

private:
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
};



/**
 * @brief MySQL数据访问对象(DAO)
 * 
 * 封装所有数据库操作，包括用户管理、好友关系、聊天会话、聊天消息等。
 * 内部使用MySqlPool连接池管理数据库连接，每次操作从池中获取连接，
 * 使用完毕后归还。
 */
class MysqlDao
{
public:
	/// 构造函数，初始化连接池
	MysqlDao();
	/// 析构函数，关闭连接池
	~MysqlDao();

	/**
	 * @brief 注册新用户
	 * @param name 用户名
	 * @param email 邮箱
	 * @param pwd 密码
	 * @return 新用户的uid，失败返回-1
	 */
	int RegUser(const std::string& name, const std::string& email, const std::string& pwd);

	/**
	 * @brief 检查用户名和邮箱是否匹配（用于重置密码验证）
	 * @param name 用户名
	 * @param email 邮箱
	 * @return 是否匹配
	 */
	bool CheckEmail(const std::string& name, const std::string & email);

	/**
	 * @brief 更新用户密码
	 * @param name 用户名
	 * @param newpwd 新密码
	 * @return 是否更新成功
	 */
	bool UpdatePwd(const std::string& name, const std::string& newpwd);

	/**
	 * @brief 验证用户名和密码，成功则填充用户信息
	 * @param name 用户名
	 * @param pwd 密码
	 * @param userInfo [out] 验证成功时填充的用户信息
	 * @return 是否验证成功
	 */
	bool CheckPwd(const std::string& name, const std::string& pwd, UserInfo& userInfo);

	/**
	 * @brief 添加好友申请记录到数据库
	 * @param from 申请者uid
	 * @param to 目标用户uid
	 * @param desc 申请附言
	 * @param back_name 申请者昵称/备注名
	 * @return 是否成功
	 */
	bool AddFriendApply(const int& from, const int& to, const std::string& desc, const std::string& back_name);

	/**
	 * @brief 认证好友申请（更新申请状态为已同意）
	 * @param from 认证者uid
	 * @param to 申请者uid
	 * @return 是否成功
	 */
	bool AuthFriendApply(const int& from, const int& to);

	/**
	 * @brief 添加好友关系（双向插入好友表，并生成聊天数据）
	 * @param from 用户A uid
	 * @param to 用户B uid
	 * @param back_name 备注名
	 * @param chat_datas [out] 生成的聊天数据（用于通知客户端）
	 * @return 是否成功
	 */
	bool AddFriend(const int& from, const int& to, std::string back_name, std::vector<std::shared_ptr<AddFriendMsg>> &chat_datas);
	
	/**
	 * @brief 根据uid获取用户信息
	 * @param uid 用户ID
	 * @return 用户信息智能指针，不存在则返回nullptr
	 */
	std::shared_ptr<UserInfo> GetUser(int uid);

	/**
	 * @brief 根据用户名获取用户信息
	 * @param name 用户名
	 * @return 用户信息智能指针，不存在则返回nullptr
	 */
	std::shared_ptr<UserInfo> GetUser(std::string name);

	/**
	 * @brief 获取指定用户的好友申请列表（分页）
	 * @param touid 目标用户uid
	 * @param applyList [out] 输出的申请列表
	 * @param offset 分页偏移量
	 * @param limit 每页数量
	 * @return 是否成功
	 */
	bool GetApplyList(int touid, std::vector<std::shared_ptr<ApplyInfo>>& applyList, int offset, int limit );

	/**
	 * @brief 获取指定用户的好友列表
	 * @param self_id 用户ID
	 * @param user_info [out] 输出的好友信息列表
	 * @return 是否成功
	 */
	bool GetFriendList(int self_id, std::vector<std::shared_ptr<UserInfo> >& user_info);

	/**
	 * @brief 分页查询用户的聊天会话列表
	 * @param userId 用户ID
	 * @param lastId 游标（上一页最后一条ID）
	 * @param pageSize 每页数量
	 * @param threads [out] 输出的会话列表
	 * @param loadMore [out] 是否还有更多
	 * @param nextLastId [out] 下一页游标
	 * @return 是否成功
	 */
	bool GetUserThreads(
		int64_t userId,
		int64_t lastId,
		int      pageSize,
		std::vector<std::shared_ptr<ChatThreadInfo>>& threads,
		bool& loadMore,
		int& nextLastId);

	/**
	 * @brief 创建私聊会话（若已存在则返回已有会话ID）
	 * @param user1_id 用户A ID
	 * @param user2_id 用户B ID
	 * @param thread_id [out] 输出的会话ID
	 * @return 是否成功
	 */
	bool CreatePrivateChat(int user1_id, int user2_id, int& thread_id);

	/**
	 * @brief 分页加载指定会话的历史聊天消息
	 * @param threadId 会话ID
	 * @param lastId 游标（上一页最后一条消息ID）
	 * @param pageSize 每页数量
	 * @return 分页结果智能指针
	 */
	std::shared_ptr<PageResult> LoadChatMsg(int threadId, int lastId, int pageSize);

	/**
	 * @brief 批量插入聊天消息到数据库
	 * @param chat_datas 消息列表
	 * @return 是否成功
	 */
	bool AddChatMsg(std::vector<std::shared_ptr<ChatMessage>>& chat_datas);

	/**
	 * @brief 插入单条聊天消息到数据库
	 * @param chat_data 消息智能指针
	 * @return 是否成功
	 */
	bool AddChatMsg(std::shared_ptr<ChatMessage> chat_data);

	/**
	 * @brief 根据消息ID获取单条聊天消息
	 * @param message_id 消息ID
	 * @return 消息智能指针，不存在则返回nullptr
	 */
	std::shared_ptr<ChatMessage> GetChatMsg(int message_id);

private:
	/// MySQL连接池实例，管理数据库连接的复用和保活
	std::unique_ptr<MySqlPool> pool_;
};


