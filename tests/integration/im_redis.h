// im_redis.h — thin hiredis wrapper used by scenarios for token seeding,
// offline-pending inspection and teardown cleanup (plan Verification.5/6).
#pragma once

#include <string>
#include <vector>

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
	bool Get(const std::string& key, std::string& value);
	bool Del(const std::string& key);
	bool Exists(const std::string& key, bool& out);

	// Inspect offline_msg:<uid>: members of the pending ZSET (sorted by score).
	bool ZRange(const std::string& key, std::vector<std::string>& members);
	int  ZCard(const std::string& key);
	bool ZRem(const std::string& key, const std::string& member);
	bool FlushZSet(const std::string& key);  // remove the whole ZSET key

	bool connected() const { return ctx_ != nullptr; }

private:
	redisContext* ctx_ = nullptr;
};

} // namespace imt
