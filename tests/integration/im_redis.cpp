// im_redis.cpp — hiredis wrapper implementation.
#include "im_redis.h"

#include <cstring>

// On Windows hiredis needs the WinSock definitions; include them first so the
// timeval / socket types resolve before hiredis pulls them in.
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif
#include <hiredis/hiredis.h>

namespace imt {

Redis::~Redis() { Close(); }

bool Redis::Connect(const std::string& host, int port, const std::string& pwd) {
	Close();
	// Plain blocking connect (matches production RedisConPool behaviour);
	// localhost connect is effectively instant, so no explicit timeval needed.
	ctx_ = redisConnect(host.c_str(), port);
	if (!ctx_ || ctx_->err) {
		if (ctx_) { redisFree(ctx_); ctx_ = nullptr; }
		return false;
	}
	if (!pwd.empty()) {
		auto* r = static_cast<redisReply*>(redisCommand(ctx_, "AUTH %s", pwd.c_str()));
		if (!r || r->type == REDIS_REPLY_ERROR) {
			if (r) freeReplyObject(r);
			redisFree(ctx_); ctx_ = nullptr;
			return false;
		}
		freeReplyObject(r);
	}
	return true;
}

void Redis::Close() {
	if (ctx_) { redisFree(ctx_); ctx_ = nullptr; }
}

bool Redis::Set(const std::string& key, const std::string& value) {
	if (!ctx_) return false;
	// argv-based: binary-safe for value.
	const char* argv[3] = { "SET", key.c_str(), value.c_str() };
	const std::size_t lens[3] = { 3, key.size(), value.size() };
	auto* r = static_cast<redisReply*>(redisCommandArgv(ctx_, 3, argv, lens));
	bool ok = r && r->type == REDIS_REPLY_STATUS && r->str &&
		(std::strcmp(r->str, "OK") == 0);
	if (r) freeReplyObject(r);
	return ok;
}

bool Redis::SetEx(const std::string& key, int ttl, const std::string& value) {
	if (!ctx_) return false;
	std::string ttl_str = std::to_string(ttl);
	// SET key value EX ttl (replaces deprecated SETEX)
	const char* argv[5] = { "SET", key.c_str(), value.c_str(), "EX", ttl_str.c_str() };
	const std::size_t lens[5] = { 3, key.size(), value.size(), 2, ttl_str.size() };
	auto* r = static_cast<redisReply*>(redisCommandArgv(ctx_, 5, argv, lens));
	bool ok = r && r->type == REDIS_REPLY_STATUS && r->str &&
		(std::strcmp(r->str, "OK") == 0);
	if (r) freeReplyObject(r);
	return ok;
}

int Redis::Ttl(const std::string& key) {
	if (!ctx_) return -2;
	auto* r = static_cast<redisReply*>(redisCommand(ctx_, "TTL %s", key.c_str()));
	int ttl = -2;
	if (r && r->type == REDIS_REPLY_INTEGER) ttl = static_cast<int>(r->integer);
	if (r) freeReplyObject(r);
	return ttl;
}

bool Redis::Get(const std::string& key, std::string& value) {
	if (!ctx_) return false;
	auto* r = static_cast<redisReply*>(redisCommand(ctx_, "GET %s", key.c_str()));
	bool ok = r && r->type == REDIS_REPLY_STRING;
	if (ok) value.assign(r->str, r->len);
	if (r) freeReplyObject(r);
	return ok;
}

bool Redis::Del(const std::string& key) {
	if (!ctx_) return false;
	auto* r = static_cast<redisReply*>(redisCommand(ctx_, "DEL %s", key.c_str()));
	bool ok = r && r->type == REDIS_REPLY_INTEGER;
	if (r) freeReplyObject(r);
	return ok;
}

bool Redis::Exists(const std::string& key, bool& out) {
	if (!ctx_) return false;
	auto* r = static_cast<redisReply*>(redisCommand(ctx_, "EXISTS %s", key.c_str()));
	bool ok = r && r->type == REDIS_REPLY_INTEGER;
	if (ok) out = (r->integer != 0);
	if (r) freeReplyObject(r);
	return ok;
}

bool Redis::HSet(const std::string& key, const std::string& field,
                 const std::string& value) {
	if (!ctx_) return false;
	const char* argv[4] = { "HSET", key.c_str(), field.c_str(), value.c_str() };
	const std::size_t lens[4] = { 4, key.size(), field.size(), value.size() };
	auto* r = static_cast<redisReply*>(redisCommandArgv(ctx_, 4, argv, lens));
	bool ok = r && r->type == REDIS_REPLY_INTEGER;
	if (r) freeReplyObject(r);
	return ok;
}

bool Redis::HGet(const std::string& key, const std::string& field,
                 std::string& value) {
	if (!ctx_) return false;
	const char* argv[3] = { "HGET", key.c_str(), field.c_str() };
	const std::size_t lens[3] = { 4, key.size(), field.size() };
	auto* r = static_cast<redisReply*>(redisCommandArgv(ctx_, 3, argv, lens));
	bool ok = r && r->type == REDIS_REPLY_STRING;
	if (ok) value.assign(r->str, r->len);
	if (r) freeReplyObject(r);
	return ok;
}

bool Redis::HDel(const std::string& key, const std::string& field) {
	if (!ctx_) return false;
	const char* argv[3] = { "HDEL", key.c_str(), field.c_str() };
	const std::size_t lens[3] = { 4, key.size(), field.size() };
	auto* r = static_cast<redisReply*>(redisCommandArgv(ctx_, 3, argv, lens));
	bool ok = r && r->type == REDIS_REPLY_INTEGER;
	if (r) freeReplyObject(r);
	return ok;
}

bool Redis::ZRange(const std::string& key, std::vector<std::string>& members) {
	members.clear();
	if (!ctx_) return false;
	auto* r = static_cast<redisReply*>(
		redisCommand(ctx_, "ZRANGE %s 0 -1 WITHSCORES", key.c_str()));
	if (!r) return false;
	bool ok = (r->type == REDIS_REPLY_ARRAY);
	if (ok) {
		// pairs: [member, score, member, score, ...]
		for (std::size_t i = 0; i + 1 < r->elements; i += 2) {
			redisReply* m = r->element[i];
			if (m && m->type == REDIS_REPLY_STRING)
				members.emplace_back(m->str, m->len);
		}
	}
	freeReplyObject(r);
	return ok;
}

int Redis::ZCard(const std::string& key) {
	if (!ctx_) return -1;
	auto* r = static_cast<redisReply*>(redisCommand(ctx_, "ZCARD %s", key.c_str()));
	int n = -1;
	if (r && r->type == REDIS_REPLY_INTEGER) n = static_cast<int>(r->integer);
	if (r) freeReplyObject(r);
	return n;
}

bool Redis::ZRem(const std::string& key, const std::string& member) {
	if (!ctx_) return false;
	const char* argv[3] = { "ZREM", key.c_str(), member.c_str() };
	const std::size_t lens[3] = { 3, key.size(), member.size() };
	auto* r = static_cast<redisReply*>(redisCommandArgv(ctx_, 3, argv, lens));
	bool ok = r && r->type == REDIS_REPLY_INTEGER;
	if (r) freeReplyObject(r);
	return ok;
}

bool Redis::FlushZSet(const std::string& key) {
	if (!ctx_) return false;
	// DEL the whole key (offline_msg:<uid>) — safe, full teardown of the index.
	auto* r = static_cast<redisReply*>(redisCommand(ctx_, "DEL %s", key.c_str()));
	bool ok = r && r->type == REDIS_REPLY_INTEGER;
	if (r) freeReplyObject(r);
	return ok;
}

} // namespace imt
