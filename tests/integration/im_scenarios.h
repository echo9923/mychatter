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

} // namespace imt
