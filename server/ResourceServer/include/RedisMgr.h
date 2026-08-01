#pragma once
#include "const.h"
#include <hiredis/hiredis.h>
#include <queue>
#include <vector>
#include <atomic>
#include <mutex>
#include "Singleton.h"
#include <cstring>
#include "FileInfo.h"

#include "RedisConPool.h"

class RedisMgr: public Singleton<RedisMgr>, 
	public std::enable_shared_from_this<RedisMgr>
{
	friend class Singleton<RedisMgr>;
public:
	~RedisMgr();
	bool Get(const std::string &key, std::string& value);
	bool Set(const std::string &key, const std::string &value);
	bool SetExp(const std::string& key, const std::string& value, int expire_seconds);
	bool LPush(const std::string &key, const std::string &value);
	bool LPop(const std::string &key, std::string& value);
	bool RPush(const std::string& key, const std::string& value);
	bool RPop(const std::string& key, std::string& value);
	bool HSet(const std::string &key, const std::string  &hkey, const std::string &value);
	bool HSet(const char* key, const char* hkey, const char* hvalue, size_t hvaluelen);
	std::string HGet(const std::string &key, const std::string &hkey);
	bool HDel(const std::string& key, const std::string& field);
	bool Del(const std::string &key);
	bool ExistsKey(const std::string &key);

	/// 向有序集合添加成员及其分值（二进制安全，ZADD key score member）
	bool ZAdd(const std::string& key, long long score, const std::string& member);
	/// 按分值排他下界到 +inf 取出有序集合成员，最多 limit 条（ZRANGEBYSCORE key (min +inf LIMIT 0 limit）
	bool ZRangeByScore(const std::string& key, long long exclusive_min, int limit, std::vector<std::string>& members);
	/// 从有序集合移除指定成员（二进制安全，ZREM key member）
	bool ZRem(const std::string& key, const std::string& member);
	/// 为键设置过期时间，单位秒（EXPIRE key seconds）
	bool Expire(const std::string& key, int seconds);
	void Close() {
		_con_pool->Close();
		_con_pool->ClearConnections();
	}

	std::string acquireLock(const std::string& lockName,
		int lockTimeout, int acquireTimeout);

	bool releaseLock(const std::string& lockName,
		const std::string& identifier);

	bool SetFileInfo(const std::string& name, std::shared_ptr<FileInfo>);
	std::shared_ptr<FileInfo> GetFileInfo(const std::string& name);
	std::shared_ptr<FileInfo> GetDownloadInfo(const std::string& name);
	bool SetDownLoadInfo(const std::string& name, std::shared_ptr<FileInfo>);
	bool DelDownLoadInfo(const std::string& name);
private:
	RedisMgr();
	unique_ptr<RedisConPool>  _con_pool;
};

