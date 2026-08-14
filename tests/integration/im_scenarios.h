// im_scenarios.h — the IM integration scenarios (plan Verification.6):
//   gate-smoke, order-n4, order-n1, dedup, offline, lost-ack, pull-bytes,
//   cross-server, image-offline, status-discovery, chat-failover, simple-auth,
//   sync-bootstrap, big-ids.
//
// Each Scenario*() returns true on success (and records [PASS]/[FAIL] lines).
// Scenarios own their ProcessManager/Redis/Mysql and clean their own footprint.
#pragma once

namespace imt {

// Diagnostic only (not a ctest scenario): report leftover test footprint so the
// teardown can be verified. Prints counts and returns true if clean.
bool ScenarioProbeCleanup();

bool ScenarioGateSmoke();
bool ScenarioOrderN4();
bool ScenarioOrderN1();
bool ScenarioDedup();

// --- Verification.6 second half (plan §2-3 后半) ---
bool ScenarioOffline();     // offline: 离线消息重连后经 1051/1052 增量同步按 sync_seq 补齐
bool ScenarioLostAck();    // lost-ack: 1050 丢失重发 1049，服务端幂等成功
bool ScenarioPullBytes();  // pull-bytes: 多页同步，无遗漏无重复、sync_seq 严格递增
bool ScenarioCrossServer();// cross-server: gRPC proxy break, retry bounded, restart sync
bool ScenarioResourceOffline(); // resource-offline: 上传完成前同步流不含该资源，1038 Ready 后出现

// --- 统一资源传输（1035/1037/1041/1045/1047 新协议）---
bool ScenarioResourceCreate();     // resource-create: 1035 校验链（超限/坏哈希/幂等/冲突/伪造/非成员）
bool ScenarioResourceUpload();     // resource-upload: 多分片上传-下载逐字节一致
bool ScenarioResourceResume();     // resource-resume: 杀 ResourceServer 重启后从非零偏移续传
bool ScenarioResourceIdempotent(); // resource-idempotent: 重复分片幂等不双写
bool ScenarioResourceCorrupt();    // resource-corrupt: 坏分片/整文件哈希 1023 与重置
bool ScenarioResourcePerm();       // resource-perm: 非会话成员上传 1026、越界下载 1018
bool ScenarioResourceOffset();     // resource-offset: 跳片 1018 + server_offset 对齐
bool ScenarioResourceExpiry();     // resource-expiry: 7 天清理标记失败并回收磁盘

// --- Plan 3.1: StatusServer lease-based least-loaded discovery ---
bool ScenarioStatusDiscovery();// status-discovery: lease selection, rotation, NoAvailableChatServer
bool ScenarioChatFailover();   // chat-failover: token-preserving reassignment and sync recovery

// --- Simple token auth: shared utoken_<uid> for Chat + Resource ---
bool ScenarioSimpleAuth();    // simple-auth: forged/valid token on Chat+Resource, secret-field hygiene, rotation

// --- 增量同步（user_message_sync）---
bool ScenarioSyncBootstrap();// sync-bootstrap: checkpoint 之后全量同步、之前不推
bool ScenarioBigIds();       // big-ids: >32 位 message_id 字符串全链路无损

} // namespace imt
