// im_common.h — shared constants, logging and assertion helpers for the IM
// integration tests (plan Verification.2 / Verification.3 first half).
//
// No test framework: every assertion prints [PASS]/[FAIL] plus a case name and
// diagnostic, and bumps a process-wide failure counter; main() exits non-zero
// if any failure was recorded. Header-only; included by every scenario TU.
#pragma once

#include <atomic>
#include <cstdarg>
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
inline constexpr short ID_IMG_CHAT_MSG_REQ        = 1035;
inline constexpr short ID_IMG_CHAT_MSG_RSP        = 1036;
inline constexpr short ID_IMG_CHAT_UPLOAD_REQ     = 1037;
inline constexpr short ID_IMG_CHAT_UPLOAD_RSP     = 1038;
inline constexpr short ID_NOTIFY_IMG_CHAT_MSG     = 1039;
inline constexpr short ID_CHAT_DELIVERY_ACK_REQ   = 1049;
inline constexpr short ID_CHAT_DELIVERY_ACK_RSP   = 1050;
inline constexpr short ID_PULL_OFFLINE_MSG_REQ    = 1051;
inline constexpr short ID_PULL_OFFLINE_MSG_RSP    = 1052;
// Resource auth: client presents the login token once per connection;
// all subsequent file frames are authorized against the bound session.
inline constexpr short ID_RESOURCE_LOGIN_REQ       = 1053;
inline constexpr short ID_RESOURCE_LOGIN_RSP       = 1054;
// A representative Resource business frame (rejected pre-auth).
inline constexpr short ID_FILE_INFO_SYNC_REQ       = 1041;
inline constexpr short ID_FILE_INFO_SYNC_RSP       = 1042;

// ---- MsgStatus / ChatMsgType (mirror server const.h / data.h) --------------
inline constexpr int MSG_STATUS_UN_READ   = 0;
inline constexpr int MSG_STATUS_UN_UPLOAD = 3;
inline constexpr int MSG_TYPE_TEXT        = 0;
inline constexpr int MSG_TYPE_PIC         = 1;

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

// PullMaxBytes from [Delivery] config; the harness-generated INI sets 30000.
// Used by pull-bytes to assert the 1052 frame stays under this ceiling.
inline constexpr int PULL_MAX_BYTES = 30000;

} // namespace imt
