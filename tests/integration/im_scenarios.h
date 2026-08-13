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
bool ScenarioImageOffline();// image-offline: 上传完成前同步流不含该图，1038 后出现

// --- Plan 3.1: StatusServer lease-based least-loaded discovery ---
bool ScenarioStatusDiscovery();// status-discovery: lease selection, rotation, NoAvailableChatServer
bool ScenarioChatFailover();   // chat-failover: token-preserving reassignment and sync recovery

// --- Simple token auth: shared utoken_<uid> for Chat + Resource ---
bool ScenarioSimpleAuth();    // simple-auth: forged/valid token on Chat+Resource, secret-field hygiene, rotation

// --- 增量同步（user_message_sync）---
bool ScenarioSyncBootstrap();// sync-bootstrap: checkpoint 之后全量同步、之前不推
bool ScenarioBigIds();       // big-ids: >32 位 message_id 字符串全链路无损

} // namespace imt
