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

#include "protocol_ids.h"  // 协议号单一来源（proto/protocol_ids.h）

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

// ---- TCP/JSON protocol message IDs (数值来源 proto/protocol_ids.h) ----------
// 编号规则：百位=功能域（11连接/12好友/13聊天/14同步/15资源/16头像），
// 奇数=发起方（请求/通知），偶数=回包，同一动作 REQ→RSP→NOTIFY 连号。
inline constexpr short ID_CHAT_LOGIN              = llfc_proto::MSG_CHAT_LOGIN;         // 1101
inline constexpr short ID_CHAT_LOGIN_RSP          = llfc_proto::MSG_CHAT_LOGIN_RSP;     // 1102
inline constexpr short ID_ADD_FRIEND_REQ          = llfc_proto::MSG_ADD_FRIEND_REQ;     // 1203
inline constexpr short ID_ADD_FRIEND_RSP          = llfc_proto::MSG_ADD_FRIEND_RSP;     // 1204
inline constexpr short ID_HANDLE_FRIEND_REQ       = llfc_proto::MSG_HANDLE_FRIEND_REQ;  // 1207
inline constexpr short ID_HANDLE_FRIEND_RSP       = llfc_proto::MSG_HANDLE_FRIEND_RSP;  // 1208
inline constexpr short ID_TEXT_CHAT_MSG_REQ       = llfc_proto::MSG_TEXT_CHAT_REQ;      // 1301
inline constexpr short ID_TEXT_CHAT_MSG_RSP       = llfc_proto::MSG_TEXT_CHAT_RSP;      // 1302
inline constexpr short ID_NOTIFY_USER_MESSAGE     = llfc_proto::MSG_NOTIFY_USER_MESSAGE; // 1701
// 资源消息（图片/文件统一传输）：创建-分片上传-进度查询-下载信息-分片下载
inline constexpr short ID_CREATE_RESOURCE_MSG_REQ   = llfc_proto::MSG_CREATE_RESOURCE_REQ;   // 1503
inline constexpr short ID_CREATE_RESOURCE_MSG_RSP   = llfc_proto::MSG_CREATE_RESOURCE_RSP;  // 1504
inline constexpr short ID_RESOURCE_CHUNK_UPLOAD_REQ = llfc_proto::MSG_RESOURCE_CHUNK_UPLOAD_REQ;   // 1507
inline constexpr short ID_RESOURCE_CHUNK_UPLOAD_RSP = llfc_proto::MSG_RESOURCE_CHUNK_UPLOAD_RSP;  // 1508
inline constexpr short ID_RESOURCE_UPLOAD_PROGRESS_REQ = llfc_proto::MSG_RESOURCE_UPLOAD_PROGRESS_REQ;  // 1509
inline constexpr short ID_RESOURCE_UPLOAD_PROGRESS_RSP = llfc_proto::MSG_RESOURCE_UPLOAD_PROGRESS_RSP; // 1510
// 旧 1043/1044 续传分支已废弃：首传/续传统一 1507+1509
inline constexpr short ID_RESOURCE_DOWN_INFO_REQ    = llfc_proto::MSG_RESOURCE_DOWN_INFO_REQ;   // 1511
inline constexpr short ID_RESOURCE_DOWN_INFO_RSP    = llfc_proto::MSG_RESOURCE_DOWN_INFO_RSP;  // 1512
inline constexpr short ID_RESOURCE_CHUNK_DOWN_REQ   = llfc_proto::MSG_RESOURCE_CHUNK_DOWN_REQ; // 1513
inline constexpr short ID_RESOURCE_CHUNK_DOWN_RSP   = llfc_proto::MSG_RESOURCE_CHUNK_DOWN_RSP; // 1514
// 1405/1406 增量同步（按 recv_seq 游标）。
inline constexpr short ID_SYNC_USER_MESSAGE_REQ   = llfc_proto::MSG_SYNC_USER_MESSAGE_REQ; // 1405
inline constexpr short ID_SYNC_USER_MESSAGE_RSP   = llfc_proto::MSG_SYNC_USER_MESSAGE_RSP; // 1406
// Resource auth: client presents the login token once per connection;
// all subsequent file frames are authorized against the bound session.
inline constexpr short ID_RESOURCE_LOGIN_REQ       = llfc_proto::MSG_RESOURCE_LOGIN_REQ;   // 1501
inline constexpr short ID_RESOURCE_LOGIN_RSP       = llfc_proto::MSG_RESOURCE_LOGIN_RSP;  // 1502

// ---- MsgStatus / ChatMsgType / ResourceStatus (mirror const.h / data.h) ----
inline constexpr int MSG_STATUS_UN_READ   = 0;
// 原 3=UN_UPLOAD 已废弃：资源生命周期读 resource_status 列
inline constexpr int MSG_TYPE_TEXT        = 0;
inline constexpr int MSG_TYPE_PIC         = 1;
inline constexpr int MSG_TYPE_FILE        = 3;
inline constexpr int MSG_TYPE_FRIEND_APPLY  = 10;
inline constexpr int MSG_TYPE_FRIEND_ACCEPT = 11;
inline constexpr int MSG_TYPE_FRIEND_REJECT = 12;
inline constexpr int BUSINESS_PENDING  = 1;
inline constexpr int BUSINESS_ACCEPTED = 2;
inline constexpr int BUSINESS_REJECTED = 3;
inline constexpr int RESOURCE_UPLOADING   = 0;   // 待上传
inline constexpr int RESOURCE_READY       = 1;   // 就绪可下载
inline constexpr int RESOURCE_EXPIRED     = 2;   // 失败/过期终态

// ---- Cross-server test proxy port -----------------------------------------
// The harness places a TCP proxy on this port between chatserver1 and
// chatserver2's gRPC endpoint to deterministically break the live RPC.
inline constexpr int CHAT2_PROXY_GRPC_PORT = 15057;

// ---- Server-side ErrorCodes (数值来源 proto/protocol_ids.h) ----------------
// 20xx 通用表（Gate/Status/Chat）+ 21xx 资源表（ResourceServer 专属语义）
inline constexpr int ERR_SUCCESS             = llfc_proto::ERR_SUCCESS;              // 0
inline constexpr int ERR_RPC_FAILED          = llfc_proto::ERR_RPC_FAILED;          // 2002
inline constexpr int ERR_TOKEN_INVALID       = llfc_proto::ERR_TOKEN_INVALID;       // 2010
inline constexpr int ERR_UID_INVALID         = llfc_proto::ERR_UID_INVALID;         // 2011
inline constexpr int ERR_MESSAGE_STORE_FAILED = llfc_proto::ERR_MESSAGE_STORE_FAILED; // 2014
inline constexpr int ERR_RECIPIENT_OFFLINE   = llfc_proto::ERR_RECIPIENT_OFFLINE;   // 2015
inline constexpr int ERR_SERVER_BUSY         = llfc_proto::ERR_SERVER_BUSY;         // 2016
inline constexpr int ERR_MESSAGE_CONFLICT    = llfc_proto::ERR_MESSAGE_CONFLICT;    // 2017
inline constexpr int ERR_NO_AVAILABLE_CHAT_SERVER = llfc_proto::ERR_NO_CHAT_SERVER; // 2018
inline constexpr int ERR_RESOURCE_INVALID    = llfc_proto::ERR_RESOURCE_INVALID;    // 2019 资源元数据非法
inline constexpr int ERR_RESOURCE_SIZE_EXCEEDED = llfc_proto::ERR_RESOURCE_SIZE_EXCEEDED; // 2020 超类型上限
inline constexpr int ERR_FRIEND_REQUEST_NOT_FOUND = llfc_proto::ERR_FRIEND_REQUEST_NOT_FOUND; // 2021
inline constexpr int ERR_FRIEND_REQUEST_HANDLED = llfc_proto::ERR_FRIEND_REQUEST_HANDLED; // 2022
inline constexpr int ERR_ALREADY_FRIENDS = llfc_proto::ERR_ALREADY_FRIENDS; // 2023
inline constexpr int ERR_SYNC_CURSOR_INVALID = llfc_proto::ERR_SYNC_CURSOR_INVALID; // 2025
// ResourceServer 侧（21xx 资源表）
inline constexpr int ERR_RS_FILE_NOT_EXISTS  = llfc_proto::RS_FILE_NOT_EXISTS;        // 2101
inline constexpr int ERR_RS_OFFSET_INVALID   = llfc_proto::RS_FILE_OFFSET_INVALID;    // 2107 偏移超前（响应带 server_offset）
inline constexpr int ERR_RS_MSG_ID_ERR       = llfc_proto::RS_MSG_ID_ERR;             // 2111
inline constexpr int ERR_RS_HASH_MISMATCH    = llfc_proto::RS_FILE_HASH_MISMATCH;     // 2112 分片/整文件 SHA-256 不符
inline constexpr int ERR_RS_SIZE_EXCEEDED    = llfc_proto::RS_FILE_SIZE_EXCEEDED;     // 2113
inline constexpr int ERR_RS_NOT_READY        = llfc_proto::RS_RESOURCE_NOT_READY;     // 2114 resource_status != Ready
inline constexpr int ERR_RS_FORBIDDEN        = llfc_proto::RS_RESOURCE_FORBIDDEN;     // 2115 非收发双方
inline constexpr int ERR_RS_STATE_INVALID    = llfc_proto::RS_RESOURCE_STATE_INVALID; // 2116 已过期/终态

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
// TCP JSON 中 message_id/thread_id/recv_seq 一律十进制字符串（64 位无损）。
inline std::string ToIdStr(std::int64_t v) { return std::to_string(v); }
// 解析十进制字符串 id；空串/非法输入返回 dfl。
inline std::int64_t ParseIdStr(const std::string& s, std::int64_t dfl = 0) {
	if (s.empty()) return dfl;
	try { return std::stoll(s); } catch (...) { return dfl; }
}

} // namespace imt
