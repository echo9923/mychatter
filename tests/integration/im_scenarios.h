// im_scenarios.h — the four IM integration scenarios (plan Verification.6):
//   gate-smoke, order-n4, order-n1, dedup.
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
bool ScenarioOffline();     // offline: 100 msgs, paged pull, ACK, DB/Redis cleanup
bool ScenarioLostAck();    // lost-ack: dropped first ACK, re-pull, second ACK clears
bool ScenarioPullBytes();  // pull-bytes: ~2KiB msgs, frame < PullMaxBytes/SHRT_MAX
bool ScenarioCrossServer();// cross-server: gRPC proxy break, retry bounded, restart pull
bool ScenarioImageOffline();// image-offline: sharded upload, UN_UPLOAD exclusion, PIC pull

// --- Plan 3.1: StatusServer lease-based least-loaded discovery ---
bool ScenarioStatusDiscovery();// status-discovery: lease selection, rotation, NoAvailableChatServer

// --- Simple token auth: shared utoken_<uid> for Chat + Resource ---
bool ScenarioSimpleAuth();    // simple-auth: forged/valid token on Chat+Resource, secret-field hygiene, rotation

} // namespace imt
