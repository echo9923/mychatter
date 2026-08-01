#pragma once
#include "const.h"
#include <hiredis/hiredis.h>
#include <queue>
#include <vector>
#include <atomic>
#include <mutex>
#include "Singleton.h"
#include "RedisConPool.h"

class RedisMgr: public Singleton<RedisMgr>,
	public std::enable_shared_from_this<RedisMgr>
{
	friend class Singleton<RedisMgr>;
public:
	~RedisMgr();
	bool Get(const std::string &key, std::string& value);
	bool Set(const std::string &key, const std::string &value);
	bool SetEx(const std::string& key, int ttl_seconds, const std::string& value);
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
	bool HGetAll(const std::string& key, std::unordered_map<std::string, std::string>& result);

	/// SCAN iterates all keys matching a glob pattern, returning the full key list.
	std::vector<std::string> Scan(const std::string& pattern);

	/// SET key value NX EX ttl — returns true only if the key was newly set.
	bool SetNx(const std::string& key, const std::string& value, int ttl_seconds);

	/// EVAL a Lua script with the given keys and args, returning the string reply
	/// (empty string on error or non-string reply).
	std::string Eval(const std::string& script,
		const std::vector<std::string>& keys,
		const std::vector<std::string>& args);
	void Close() {
		_con_pool->Close();
		_con_pool->ClearConnections();
	}

	std::string acquireLock(const std::string& lockName,
		int lockTimeout, int acquireTimeout);

	bool releaseLock(const std::string& lockName,
		const std::string& identifier);


private:
	RedisMgr();
	unique_ptr<RedisConPool>  _con_pool;
};
