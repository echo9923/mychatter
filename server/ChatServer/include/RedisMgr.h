#pragma once
#include "const.h"
#include <hiredis/hiredis.h>
#include <queue>
#include <vector>
#include <atomic>
#include <mutex>
#include "Singleton.h"
#include <cstring>
#include "RedisConPool.h"

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
	/// 设置指定键的值并设置过期时间（SET key value EX seconds）
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

