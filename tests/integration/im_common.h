// im_common.h — shared constants, logging and assertion helpers for the IM
// integration tests (plan Verification.2 / Verification.3 first half).
//
// No test framework: every assertion prints [PASS]/[FAIL] plus a case name and
// diagnostic, and bumps a process-wide failure counter; main() exits non-zero
// if any failure was recorded. Header-only; included by every scenario TU.
#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace imt {

// ---- Fixture users / data (plan Verification.5) ----------------------------
// Reuse existing fixture accounts; never create new ones.
inline constexpr int SENDER_UID   = 1002;  // fixture sender
inline constexpr int RECEIVER_UID = 1019;  // fixture receiver
inline constexpr int THREAD_ID    = 35;    // fixture chat thread

// Fixture credentials (plan 3.2): Gate verifies the password against the DB
// pwd column directly, so it is sent as-is. Both fixture users share this pwd.
inline constexpr const char* FIXTURE_SENDER_EMAIL   = "secondtonone1@163.com";
inline constexpr const char* FIXTURE_RECEIVER_EMAIL = "1017234088@qq.com";
inline constexpr const char* FIXTURE_PASSWD         = "654321)";

// Resolve the fixture email for a uid (used by LoginUser to POST /user_login).
inline const char* FixtureEmailForUid(int uid) {
	return (uid == SENDER_UID) ? FIXTURE_SENDER_EMAIL : FIXTURE_RECEIVER_EMAIL;
}

// ---- Harness ports (plan Verification.3: fixed suffix ports) ----------------
// Production defaults (Gate 8080, Chat 8090/8091, Chat gRPC 50055/50056,
// Status 50052) are NOT used by the harness; these dedicated ports keep the
// test isolated from any concurrently running production instances.
inline constexpr int  GATE_HTTP_PORT    = 18080;
inline constexpr int  CHAT1_TCP_PORT    = 18090;
inline constexpr int  CHAT2_TCP_PORT    = 18091;
inline constexpr int  CHAT3_TCP_PORT    = 18092;
inline constexpr int  CHAT1_GRPC_PORT   = 15055;
inline constexpr int  CHAT2_GRPC_PORT   = 15056;
inline constexpr int  CHAT3_GRPC_PORT   = 15058;
inline constexpr int  STATUS_GRPC_PORT  = 15052;
inline constexpr int  RESOURCE_HTTP_PORT = 18081;

// ---- Live externals (plan Verification.5) ----------------------------------
inline constexpr const char* MYSQL_HOST   = "127.0.0.1";
inline constexpr int         MYSQL_PORT   = 3308;
inline constexpr const char* MYSQL_USER   = "root";
inline constexpr const char* MYSQL_PASSWD = "123456.";
inline constexpr const char* MYSQL_SCHEMA = "llfc";
inline constexpr const char* REDIS_HOST   = "127.0.0.1";
inline constexpr int         REDIS_PORT   = 6380;
inline constexpr const char* REDIS_PASSWD = "123456";

// ---- TCP/JSON protocol message IDs (mirror server const.h) -----------------
inline constexpr short ID_CHAT_LOGIN              = 1005;
inline constexpr short ID_CHAT_LOGIN_RSP          = 1006;
inline constexpr short ID_TEXT_CHAT_MSG_REQ       = 1017;
inline constexpr short ID_TEXT_CHAT_MSG_RSP       = 1018;
inline constexpr short ID_NOTIFY_TEXT_CHAT_MSG    = 1019;
// 资源消息（图片/文件统一传输）：创建-分片上传-进度查询-下载信息-分片下载
inline constexpr short ID_CREATE_RESOURCE_MSG_REQ   = 1035;
inline constexpr short ID_CREATE_RESOURCE_MSG_RSP   = 1036;
inline constexpr short ID_RESOURCE_CHUNK_UPLOAD_REQ = 1037;
inline constexpr short ID_RESOURCE_CHUNK_UPLOAD_RSP = 1038;
inline constexpr short ID_NOTIFY_RESOURCE_MSG       = 1039;
inline constexpr short ID_RESOURCE_UPLOAD_PROGRESS_REQ = 1041;
inline constexpr short ID_RESOURCE_UPLOAD_PROGRESS_RSP = 1042;
// 1043/1044 续传分支已废弃：首传/续传统一 1037+1041
inline constexpr short ID_RESOURCE_DOWN_INFO_REQ    = 1045;
inline constexpr short ID_RESOURCE_DOWN_INFO_RSP    = 1046;
inline constexpr short ID_RESOURCE_CHUNK_DOWN_REQ   = 1047;
inline constexpr short ID_RESOURCE_CHUNK_DOWN_RSP   = 1048;
inline constexpr short ID_CHAT_DELIVERY_ACK_REQ   = 1049;
inline constexpr short ID_CHAT_DELIVERY_ACK_RSP   = 1050;
// 1051/1052 数值不变，语义由离线拉取改为 user_message_sync 增量同步。
inline constexpr short ID_SYNC_MESSAGE_REQ        = 1051;
inline constexpr short ID_SYNC_MESSAGE_RSP        = 1052;
// Resource auth: client presents the login token once per connection;
// all subsequent file frames are authorized against the bound session.
inline constexpr short ID_RESOURCE_LOGIN_REQ       = 1053;
inline constexpr short ID_RESOURCE_LOGIN_RSP       = 1054;

// ---- MsgStatus / ChatMsgType / ResourceStatus (mirror const.h / data.h) ----
inline constexpr int MSG_STATUS_UN_READ   = 0;
// 原 3=UN_UPLOAD 已废弃：资源生命周期读 resource_status 列
inline constexpr int MSG_TYPE_TEXT        = 0;
inline constexpr int MSG_TYPE_PIC         = 1;
inline constexpr int MSG_TYPE_FILE        = 3;
inline constexpr int RESOURCE_UPLOADING   = 0;   // 待上传
inline constexpr int RESOURCE_READY       = 1;   // 就绪可下载
inline constexpr int RESOURCE_EXPIRED     = 2;   // 失败/过期终态

// ---- Cross-server test proxy port -----------------------------------------
// The harness places a TCP proxy on this port between chatserver1 and
// chatserver2's gRPC endpoint to deterministically break the live RPC.
inline constexpr int CHAT2_PROXY_GRPC_PORT = 15057;

// ---- Server-side ErrorCodes (mirror const.h; subset used by tests) ---------
inline constexpr int ERR_SUCCESS             = 0;
inline constexpr int ERR_RPC_FAILED          = 1002;
inline constexpr int ERR_TOKEN_INVALID       = 1010;  // TokenInvalid
inline constexpr int ERR_UID_INVALID         = 1011;
inline constexpr int ERR_MESSAGE_STORE_FAILED = 1014;
inline constexpr int ERR_RECIPIENT_OFFLINE   = 1015;
inline constexpr int ERR_SERVER_BUSY         = 1016;
inline constexpr int ERR_MESSAGE_CONFLICT    = 1017;
inline constexpr int ERR_NO_AVAILABLE_CHAT_SERVER = 1018;
inline constexpr int ERR_RESOURCE_INVALID    = 1019;  // ChatServer：资源元数据非法
inline constexpr int ERR_RESOURCE_SIZE_EXCEEDED = 1020; // ChatServer：超类型上限
// ResourceServer 侧（同一数值段，不同语义表）
inline constexpr int ERR_RS_FILE_NOT_EXISTS  = 1012;
inline constexpr int ERR_RS_OFFSET_INVALID   = 1018;  // 偏移超前（响应带 server_offset）
inline constexpr int ERR_RS_MSG_ID_ERR       = 1022;
inline constexpr int ERR_RS_HASH_MISMATCH    = 1023;  // 分片/整文件 SHA-256 不符
inline constexpr int ERR_RS_SIZE_EXCEEDED    = 1024;
inline constexpr int ERR_RS_NOT_READY        = 1025;  // resource_status != Ready
inline constexpr int ERR_RS_FORBIDDEN        = 1026;  // 非收发双方
inline constexpr int ERR_RS_STATE_INVALID    = 1027;  // 已过期/终态

// ---- Process-wide failure counter ------------------------------------------
inline std::atomic<int> g_failures{0};

inline void Log(const char* fmt, ...) {
	va_list ap; va_start(ap, fmt);
	std::vprintf(fmt, ap); va_end(ap);
	std::printf("\n");
	std::fflush(stdout);
}

inline void Pass(const std::string& name) {
	std::printf("[PASS] %s\n", name.c_str()); std::fflush(stdout);
}

inline void Fail(const std::string& name, const std::string& detail) {
	std::printf("[FAIL] %s : %s\n", name.c_str(), detail.c_str());
	std::fflush(stdout);
	++g_failures;
}

// Records a [PASS]/[FAIL] line for `name` based on `cond`. Returns cond.
inline bool Check(bool cond, const std::string& name, const std::string& detail) {
	if (cond) Pass(name);
	else      Fail(name, detail);
	return cond;
}

// Total number of message bodies the test should leave on disk/Redis untouched
// Redis key helpers for chat leases and the simple per-user login token.
inline std::string ChatLeaseKey(const std::string& name) {
	return "chatserver:lease:" + name;
}
inline constexpr const char* CHAT_REGISTRY_KEY = "chatserver:registry";
inline std::string UserTokenKey(int uid) {
	return "utoken_" + std::to_string(uid);
}
// by an entire scenario run. Used for the final summary line.
inline int Failures() { return g_failures.load(); }

// ---- 协议字符串化辅助 -------------------------------------------------------
// TCP JSON 中 message_id/thread_id/sync_seq 一律十进制字符串（64 位无损）。
inline std::string ToIdStr(std::int64_t v) { return std::to_string(v); }
// 解析十进制字符串 id；空串/非法输入返回 dfl。
inline std::int64_t ParseIdStr(const std::string& s, std::int64_t dfl = 0) {
	if (s.empty()) return dfl;
	try { return std::stoll(s); } catch (...) { return dfl; }
}

} // namespace imt
