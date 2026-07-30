#pragma once
#include <hiredis/hiredis.h>
#include <string>

/**
 * @brief Redis分布式锁工具类（单例）
 * 
 * 基于Redis实现的分布式锁，用于多服务实例间的互斥访问控制。
 * 典型场景：用户登录时的并发控制、跨服务器资源竞争等。
 * 支持超时自动释放（防止死锁）和获取重试超时。
 */
class DistLock
{
public:
	/**
	 * @brief 获取DistLock单例实例
	 * @return DistLock单例引用
	 */
	static DistLock& Inst();

	/// 析构函数
	~DistLock() = default;

	/**
	 * @brief 尝试获取分布式锁
	 * @param context Redis连接上下文
	 * @param lockName 锁名称（唯一标识被保护的资源）
	 * @param lockTimeout 锁的持有超时时间（秒），超时后自动释放
	 * @param acquireTimeout 获取锁的重试超时时间（秒），超过则放弃
	 * @return 锁的唯一标识符（用于释放时验证），获取失败返回空字符串
	 */
	std::string acquireLock(redisContext* context, const std::string& lockName,
		int lockTimeout, int acquireTimeout);

	/**
	 * @brief 释放分布式锁
	 * @param context Redis连接上下文
	 * @param lockName 锁名称
	 * @param identifier 获取锁时返回的唯一标识符（确保只能释放自己持有的锁）
	 * @return 是否成功释放
	 */
	bool releaseLock(redisContext* context, const std::string& lockName,
		const std::string& identifier);

private:
	/// 私有构造函数（单例模式）
	DistLock() = default;
};

