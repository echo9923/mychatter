#pragma once
#include "const.h"
#include <hiredis/hiredis.h>
#include <queue>
#include <vector>
#include <atomic>
#include <mutex>
#include "Singleton.h"
#include <cstring>
/**
 * @brief Redis连接池
 * 
 * 管理多个Redis连接（hiredis），提供连接的获取、归还、健康检测和自动重连。
 * 内部启动一个检测线程，每60秒对空闲连接发送PING命令保活，
 * 失败的连接会被释放并重新创建。
 */
class RedisConPool {
public:
	/**
	 * @brief 构造连接池，预创建指定数量的Redis连接并启动健康检测线程
	 * @param poolSize 连接池大小
	 * @param host Redis服务器地址
	 * @param port Redis服务器端口
	 * @param pwd Redis认证密码
	 */
	RedisConPool(size_t poolSize, const char* host, int port, const char* pwd)
		: poolSize_(poolSize), host_(host), port_(port), b_stop_(false), pwd_(pwd), counter_(0), fail_count_(0){
		for (size_t i = 0; i < poolSize_; ++i) {
			auto* context = redisConnect(host, port);
			if (context == nullptr || context->err != 0) {
				if (context != nullptr) {
					redisFree(context);
				}
				continue;
			}

			auto reply = (redisReply*)redisCommand(context, "AUTH %s", pwd);
			if (reply->type == REDIS_REPLY_ERROR) {
				std::cout << "��֤ʧ��" << std::endl;
				//ִ�гɹ� �ͷ�redisCommandִ�к󷵻ص�redisReply��ռ�õ��ڴ�
				freeReplyObject(reply);
				continue;
			}

			//ִ�гɹ� �ͷ�redisCommandִ�к󷵻ص�redisReply��ռ�õ��ڴ�
			freeReplyObject(reply);
			std::cout << "��֤�ɹ�" << std::endl;
			connections_.push(context);
		}

		check_thread_ = std::thread([this]() {
			while (!b_stop_) {
				counter_++;
				if (counter_ >= 60) {
					checkThreadPro();
					counter_ = 0;
				}

				std::this_thread::sleep_for(std::chrono::seconds(1)); // ÿ�� 30 �뷢��һ�� PING ����
			}	
		});

	}

	/// 析构函数
	~RedisConPool() {

	}

	/// 清空并释放所有Redis连接
	void ClearConnections() {
		std::lock_guard<std::mutex> lock(mutex_);
		while (!connections_.empty()) {
			auto* context = connections_.front();
			redisFree(context);
			connections_.pop();
		}
	}

	/**
	 * @brief 从连接池获取一个可用Redis连接（阻塞等待）
	 * @return redisContext指针，若池已停止则返回nullptr
	 */
	redisContext* getConnection() {
		std::unique_lock<std::mutex> lock(mutex_);
		cond_.wait(lock, [this] { 
			if (b_stop_) {
				return true;
			}
			return !connections_.empty(); 
			});
		//���ֹͣ��ֱ�ӷ��ؿ�ָ��
		if (b_stop_) {
			return  nullptr;
		}
		auto* context = connections_.front();
		connections_.pop();
		return context;
	}

	/**
	 * @brief 从连接池获取一个Redis连接（非阻塞，无可用连接时立即返回nullptr）
	 * @return redisContext指针，无可用连接或已停止时返回nullptr
	 */
	redisContext* getConNonBlock() {
		std::unique_lock<std::mutex> lock(mutex_);
		if (b_stop_) {
			return nullptr;
		}

		if (connections_.empty()) {
			return nullptr;
		}

		auto* context = connections_.front();
		connections_.pop();
		return context;
	}

	/**
	 * @brief 将使用完毕的Redis连接归还到连接池
	 * @param context 要归还的Redis连接
	 */
	void returnConnection(redisContext* context) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (b_stop_) {
			return;
		}
		connections_.push(context);
		cond_.notify_one();
	}

	/// 关闭连接池，停止检测线程并唤醒所有等待线程
	void Close() {
		b_stop_ = true;
		cond_.notify_all();
		check_thread_.join();
	}

private:

	/// 重新创建一个Redis连接并加入池中
	bool  reconnect() {
		auto context = redisConnect(host_, port_);
		if (context == nullptr || context->err != 0) {
			if (context != nullptr) {
				redisFree(context);
			}
			return false;
		}

		auto reply = (redisReply*)redisCommand(context, "AUTH %s", pwd_);
		if (reply->type == REDIS_REPLY_ERROR) {
			std::cout << "��֤ʧ��" << std::endl;
			//ִ�гɹ� �ͷ�redisCommandִ�к󷵻ص�redisReply��ռ�õ��ڴ�
			freeReplyObject(reply);
			redisFree(context);
			return false;
		}

		//ִ�гɹ� �ͷ�redisCommandִ�к󷵻ص�redisReply��ռ�õ��ڴ�
		freeReplyObject(reply);
		std::cout << "��֤�ɹ�" << std::endl;
		returnConnection(context);
		return true;
	}

	/// 连接健康检测（无锁版本），逐个取出连接发送PING，失败的移除后重连
	void checkThreadPro() {
			size_t pool_size;
			{
				// ���õ���ǰ������
				std::lock_guard<std::mutex> lock(mutex_);
				pool_size = connections_.size();
			}

			
			for (int i = 0; i < pool_size && !b_stop_; ++i) {
				redisContext* ctx = nullptr;
				// 1) ȡ��һ������(������)
				bool bsuccess = false;
				auto * context = getConNonBlock();
				if (context == nullptr) {
					break;
				}

				redisReply* reply = nullptr;
				try {
					reply = (redisReply*)redisCommand(context, "PING");
					// 2. �ȿ��ײ� I/O��Э�����û�д�
					if (context->err) {
						std::cout << "Connection error: " << context->err << std::endl;
						if (reply) {
							freeReplyObject(reply);
						}
						redisFree(context);
						fail_count_++;
						continue;
					}

					// 3. �ٿ� Redis �������ص��ǲ��� ERROR
					if (!reply || reply->type == REDIS_REPLY_ERROR) {
						std::cout << "reply is null, redis ping failed: " << std::endl;
						if (reply) {
							freeReplyObject(reply);
						}
						redisFree(context);
						fail_count_++;
						continue;
					}
					// 4. �����û���⣬�򻹻�ȥ
					//std::cout << "connection alive" << std::endl;
					freeReplyObject(reply);
					returnConnection(context);
				}
				catch (std::exception& exp) {
					if (reply) {
						freeReplyObject(reply);
					}

					redisFree(context);
					fail_count_++;
				}
							
			}

			//ִ����������
			while (fail_count_ > 0) {
				auto res = reconnect();
				if(res){
					fail_count_--;
				}
				else {
					//�����´�������
					break;
				}
			}
	}
	

	/// 连接健康检测（有锁版本，已废弃，保留作为参考）
	void checkThread() {
		std::lock_guard<std::mutex> lock(mutex_);
		if (b_stop_) {
			return;
		}
		auto pool_size = connections_.size();
		for (int i = 0; i < pool_size && !b_stop_; i++) {
			auto* context = connections_.front();
			connections_.pop();
			try {
				auto reply = (redisReply*)redisCommand(context, "PING");
				if (!reply) {
					std::cout << "reply is null, redis ping failed: " << std::endl;
					connections_.push(context);
					continue;
				}
				freeReplyObject(reply);
				connections_.push(context);
			}
			catch(std::exception& exp){
				std::cout << "Error keeping connection alive: " << exp.what() << std::endl;
				redisFree(context);
				context = redisConnect(host_, port_);
				if (context == nullptr || context->err != 0) {
					if (context != nullptr) {
						redisFree(context);
					}
					continue;
				}

				auto reply = (redisReply*)redisCommand(context, "AUTH %s", pwd_);
				if (reply->type == REDIS_REPLY_ERROR) {
					std::cout << "��֤ʧ��" << std::endl;
					//ִ�гɹ� �ͷ�redisCommandִ�к󷵻ص�redisReply��ռ�õ��ڴ�
					freeReplyObject(reply);
					continue;
				}

				//ִ�гɹ� �ͷ�redisCommandִ�к󷵻ص�redisReply��ռ�õ��ڴ�
				freeReplyObject(reply);
				std::cout << "��֤�ɹ�" << std::endl;
				connections_.push(context);
			}
		}
	}
	std::atomic<bool> b_stop_;    ///< 停止标志
	size_t poolSize_;             ///< 连接池容量
	const char* host_;            ///< Redis服务器地址
	const char* pwd_;             ///< Redis认证密码
	int port_;                    ///< Redis服务器端口
	std::queue<redisContext*> connections_;  ///< Redis连接队列
	std::atomic<int> fail_count_; ///< 失败连接计数，用于触发重连
	std::mutex mutex_;            ///< 互斥锁，保护连接队列的线程安全
	std::condition_variable cond_; ///< 条件变量，无可用连接时阻塞等待
	std::thread  check_thread_;   ///< 健康检测线程
	int counter_;                 ///< 计时器，累计到60秒触发一次检测
};

/**
 * @brief Redis管理器（单例）
 * 
 * 提供全局唯一的Redis访问入口，封装常用的Redis操作（String、List、Hash、Key）。
 * 内部使用RedisConPool连接池管理连接，支持分布式锁和服务器连接计数管理。
 * 用于用户会话管理、Token存储、负载均衡计数、分布式锁等场景。
 */
class RedisMgr: public Singleton<RedisMgr>, 
	public std::enable_shared_from_this<RedisMgr>
{
	friend class Singleton<RedisMgr>;
public:
	/// 析构函数
	~RedisMgr();

	/// 获取指定键的值
	bool Get(const std::string &key, std::string& value);
	/// 设置指定键的值
	bool Set(const std::string &key, const std::string &value);
	/// 设置指定键的值并设置过期时间（SETEX key seconds value）
	bool SetEx(const std::string& key, int ttl_seconds, const std::string& value);
	/// 从列表左侧插入元素
	bool LPush(const std::string &key, const std::string &value);
	/// 从列表左侧弹出元素
	bool LPop(const std::string &key, std::string& value);
	/// 从列表右侧插入元素
	bool RPush(const std::string& key, const std::string& value);
	/// 从列表右侧弹出元素
	bool RPop(const std::string& key, std::string& value);
	/// 设置Hash字段值（字符串版本）
	bool HSet(const std::string &key, const std::string  &hkey, const std::string &value);
	/// 设置Hash字段值（二进制安全版本，支持指定值长度）
	bool HSet(const char* key, const char* hkey, const char* hvalue, size_t hvaluelen);
	/// 获取Hash字段值
	std::string HGet(const std::string &key, const std::string &hkey);
	/// 删除Hash字段
	bool HDel(const std::string& key, const std::string& field);
	/// 删除指定键
	bool Del(const std::string &key);
	/// 检查指定键是否存在
	bool ExistsKey(const std::string &key);

	/// 向有序集合添加成员及其分值（二进制安全，ZADD key score member）
	bool ZAdd(const std::string& key, long long score, const std::string& member);
	/// 按分值排他下界到 +inf 取出有序集合成员，最多 limit 条（ZRANGEBYSCORE key (min +inf LIMIT 0 limit）
	bool ZRangeByScore(const std::string& key, long long exclusive_min, int limit, std::vector<std::string>& members);
	/// 从有序集合移除指定成员（二进制安全，ZREM key member）
	bool ZRem(const std::string& key, const std::string& member);
	/// 为键设置过期时间，单位秒（EXPIRE key seconds）
	bool Expire(const std::string& key, int seconds);

	/**
	 * @brief 执行 Lua 脚本（EVAL script numkeys key... arg...）
	 *
	 * 以 redisCommandArgv 二进制安全方式传递脚本、键、参数，避免 %s 格式被特殊字符拆分。
	 * @param script Lua 脚本源码
	 * @param keys KEYS 列表
	 * @param args ARGV 列表
	 * @return 字符串/状态回复返回其 str；整数回复转为十进制串；NIL/错误/NULL 返回空串（fail-closed）
	 */
	std::string Eval(const std::string& script, const std::vector<std::string>& keys, const std::vector<std::string>& args);

	/**
	 * @brief 仅当 key 当前值==token 时才刷新其 TTL（compare-and-expire，原子 Lua）
	 *
	 * 用于可恢复会话令牌的周期性续期：token 仍与登录时一致才 SETEX 续命，
	 * 若已被异地登录覆盖（token 不匹配）则不续期，使旧连接尽快失效。
	 * @param key Redis 键
	 * @param token 期望的当前值
	 * @param ttl_seconds 续期后的过期秒数
	 * @return token 匹配且 SETEX 成功返回 true，不匹配或出错返回 false
	 */
	bool CompareAndExpire(const std::string& key, const std::string& token, int ttl_seconds);

	/// 关闭Redis连接池并释放所有连接
	void Close() {
		_con_pool->Close();
		_con_pool->ClearConnections();
	}

	/**
	 * @brief 获取分布式锁
	 * @param lockName 锁名称
	 * @param lockTimeout 锁持有超时时间（秒）
	 * @param acquireTimeout 获取重试超时时间（秒）
	 * @return 锁标识符，失败返回空字符串
	 */
	std::string acquireLock(const std::string& lockName,
		int lockTimeout, int acquireTimeout);

	/**
	 * @brief 释放分布式锁
	 * @param lockName 锁名称
	 * @param identifier 获取锁时返回的标识符
	 * @return 是否成功释放
	 */
	bool releaseLock(const std::string& lockName,
		const std::string& identifier);

private:
	/// 私有构造函数，从配置文件读取Redis连接参数并初始化连接池
	RedisMgr();
	/// Redis连接池实例
	unique_ptr<RedisConPool>  _con_pool;
};

