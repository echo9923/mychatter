// im_redis.h — thin hiredis wrapper used by scenarios for token seeding,
// lease/registry inspection and teardown cleanup (plan Verification.5/6).
#pragma once

#include <string>

// Pull in the real redisContext so the member is a complete type (forward
// declaring `struct redisContext` inside namespace imt would instead introduce
// a distinct imt::redisContext — see plan: do not shadow global C types).
#include <hiredis/hiredis.h>

namespace imt {

class Redis {
public:
	Redis() = default;
	~Redis();
	Redis(const Redis&) = delete;
	Redis& operator=(const Redis&) = delete;

	bool Connect(const std::string& host, int port, const std::string& pwd);
	void Close();

	bool Set(const std::string& key, const std::string& value);
	bool SetEx(const std::string& key, int ttl, const std::string& value);
	bool Get(const std::string& key, std::string& value);
	bool Del(const std::string& key);
	bool Exists(const std::string& key, bool& out);
	bool HSet(const std::string& key, const std::string& field,
	          const std::string& value);
	bool HGet(const std::string& key, const std::string& field,
	          std::string& value);
	bool HDel(const std::string& key, const std::string& field);

	int  Ttl(const std::string& key);  // -2 key 不存在, -1 无 TTL, 否则剩余秒

	bool connected() const { return ctx_ != nullptr; }

private:
	redisContext* ctx_ = nullptr;
};

} // namespace imt
