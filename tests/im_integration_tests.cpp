// im_integration_tests.cpp — entry point (plan Verification.2 / Verification.4).
//
// Usage:
//   im_integration_tests --scenario <gate-smoke|order-n4|order-n1|dedup>
//   im_integration_tests            # runs all four in sequence
//
// No test framework. Each scenario prints [PASS]/[FAIL] lines and returns a
// boolean; main() exits non-zero if the scenario (or any scenario, when running
// all) recorded a failure.
//
// Each scenario owns its own ProcessManager (spawns Status/Chat/Gate with
// generated INIs on dedicated ports, tears them down on exit), logs a uid in
// to a ChatServer via the per-user login token, and cleans its own DB/Redis
// footprint. Built and registered as CTest cases only when
// LLFC_RUN_INTEGRATION_TESTS=ON (see tests/CMakeLists.txt).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "integration/im_scenarios.h"
#include "integration/im_common.h"

namespace {

struct ScenarioEntry {
	const char* name;
	bool (*fn)();
};

ScenarioEntry kScenarios[] = {
	{ "gate-smoke",    imt::ScenarioGateSmoke    },
	{ "order-n4",      imt::ScenarioOrderN4      },
	{ "order-n1",      imt::ScenarioOrderN1      },
	{ "dedup",         imt::ScenarioDedup        },
	{ "offline",       imt::ScenarioOffline      },
	{ "lost-ack",      imt::ScenarioLostAck      },
	{ "pull-bytes",    imt::ScenarioPullBytes    },
	{ "cross-server",  imt::ScenarioCrossServer  },
	{ "image-offline", imt::ScenarioImageOffline },
	{ "status-discovery", imt::ScenarioStatusDiscovery },
	{ "simple-auth",   imt::ScenarioSimpleAuth   },
};

} // namespace

int main(int argc, char** argv) {
	std::printf("=== im_integration_tests (Verification.2-3) ===\n");

	std::string scenario;
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--scenario" && i + 1 < argc) { scenario = argv[++i]; }
		else if (a == "--help" || a == "-h") {
			std::printf("usage: im_integration_tests --scenario <gate-smoke|order-n4|order-n1|dedup|offline|lost-ack|pull-bytes|cross-server|image-offline|status-discovery|simple-auth>\n");
			return 0;
		}
	}

	const int failures_before = imt::Failures();

	auto run_one = [&](const ScenarioEntry& e) -> bool {
		std::printf("\n########## running scenario: %s ##########\n", e.name);
		const bool ok = e.fn();
		std::printf("########## scenario %s : %s ##########\n",
			e.name, ok ? "PASS" : "FAIL");
		return ok;
	};

	bool ok = true;
	if (scenario == "probe-cleanup") {
		ok = imt::ScenarioProbeCleanup();
		std::printf("RESULT: %s\n", ok ? "CLEAN" : "DIRTY");
		return ok ? 0 : 1;
	} else if (scenario.empty()) {
		for (const auto& e : kScenarios) ok = run_one(e) && ok;
	} else {
		bool found = false;
		for (const auto& e : kScenarios) {
			if (scenario == e.name) { ok = run_one(e); found = true; break; }
		}
		if (!found) {
			std::printf("[FAIL] unknown scenario: %s\n", scenario.c_str());
			ok = false;
		}
	}

	const int total_failures = imt::Failures() - failures_before;
	std::printf("\n=== im_integration_tests summary: %d failure(s) ===\n", total_failures);
	std::printf("RESULT: %s\n", (ok && total_failures == 0) ? "PASS" : "FAIL");
	return (ok && total_failures == 0) ? 0 : 1;
}
