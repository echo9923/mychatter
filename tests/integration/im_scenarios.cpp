// im_scenarios.cpp — implementation of the four IM integration scenarios.
//
// Deterministic assertions (plan Verification.6):
//   gate-smoke: 32 concurrent /get_test + /user_login all yield complete HTTP
//               responses; the server accepts a brand-new connection while at
//               least one handler is still in flight (overlap proof).
//   order-n4:   LogicWorkers=4; uid A=1002 and B=1019 each send 1000 texts
//               concurrently. Per uid, 1018 arrival order == submission order and
//               canonical message_id strictly increases; the two uids' global
//               receipt-sequence ranges overlap (different shards, true parallel).
//   order-n1:   LogicWorkers=1; same 1000-each send, order preserved; no overlap
//               assertion (single shard serializes, overlap is not guaranteed).
//   dedup:      pipelined re-send of an identical (sender_id,unique_id) yields the
//               same message_id with exactly one DB row; a same-key different-
//               content send returns MESSAGE_CONFLICT and leaves the original row.
#include "im_scenarios.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <process.h>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "im_common.h"
#include "im_frame.h"
#include "im_harness.h"
#include "im_http_client.h"
#include "im_mysql.h"
#include "im_redis.h"
#include "im_tcp_client.h"

namespace imt {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

static unsigned long GetCurrentPidSafe() {
#ifdef _WIN32
	return static_cast<unsigned long>(_getpid());
#else
	return 0;
#endif
}

// Unique per-run tag embedded in every unique_id so concurrent ctest runs or
// re-runs never collide on the (sender_id, unique_id) unique key.
static std::string RunTag() {
	static std::atomic<unsigned long long> seq{0};
	auto s = seq.fetch_add(1);
	std::ostringstream os;
	os << GetCurrentPidSafe() << "-" << s;
	return os.str();
}

// Delete every chat_message row + Redis key the scenario may have created.
// Only touches keys/rows tied to the fixture uids and the "imtest-" prefix.
static void CleanupFootprint(Redis& redis, Mysql& mysql) {
	mysql.DeleteByUniqueIdLike("imtest-%");
	redis.FlushZSet("offline_msg:" + std::to_string(SENDER_UID));
	redis.FlushZSet("offline_msg:" + std::to_string(RECEIVER_UID));
	for (int uid : { SENDER_UID, RECEIVER_UID }) {
		const std::string u = std::to_string(uid);
		redis.Del("utoken_"   + u);
		redis.Del("uip_"      + u);
		redis.Del("usession_" + u);
		redis.Del("ubaseinfo_" + u);
	}
}

// Seed utoken and log a uid in to the ChatServer; returns true on error==0.
static bool LoginUser(Redis& redis, TcpClient& c, int uid, const std::string& token,
                      unsigned short chat_port) {
	if (!redis.Set("utoken_" + std::to_string(uid), token)) {
		std::printf("[login] redis SET utoken_%d failed\n", uid);
		return false;
	}
	if (!c.Connect("127.0.0.1", chat_port, 10000)) {
		std::printf("[login] connect failed uid=%d port=%u\n", uid, chat_port);
		return false;
	}
	json lj;
	lj["uid"] = uid;
	lj["token"] = token;
	if (!c.Send(ID_CHAT_LOGIN, lj.dump())) return false;
	Frame f;
	if (!c.Wait(ID_CHAT_LOGIN_RSP, 10000, &f)) {
		std::printf("[login] no 1006 for uid=%d\n", uid);
		return false;
	}
	auto j = ParseJson(f.body);
	if (!j.is_object() || j.value("error", -1) != ERR_SUCCESS) {
		std::printf("[login] uid=%d login error=%d\n", uid,
			j.is_object() ? j.value("error", -1) : -1);
		return false;
	}
	return true;
}

// Build a 1017 body carrying a single text message.
static std::string BuildTextReq(int fromuid, int touid, int thread_id,
                                const std::string& content, const std::string& unique_id) {
	json j;
	j["fromuid"] = fromuid;
	j["touid"]   = touid;
	j["thread_id"] = thread_id;
	json item;
	item["content"]  = content;
	item["unique_id"] = unique_id;
	j["text_array"] = json::array({ item });
	return j.dump();
}

// ---------------------------------------------------------------------------
// gate-smoke
// ---------------------------------------------------------------------------
bool ScenarioGateSmoke() {
	std::printf("\n=== scenario: gate-smoke ===\n");
	ProcessManager pm;
	Redis redis; Mysql mysql;

	if (!pm.Start({ "StatusServer", { "StatusServer", "StatusServer.exe" },
		MakeStatusIni(), STATUS_GRPC_PORT }, 15000)) {
		Fail("gate-smoke: start StatusServer", "ready timeout"); return false;
	}
	if (!pm.Start({ "GateServer", { "GateServer", "GateServer.exe" },
		MakeGateIni(), GATE_HTTP_PORT }, 15000)) {
		Fail("gate-smoke: start GateServer", "ready timeout"); return false;
	}

	bool all_ok = true;

	// Wave 1: 32 concurrent GET /get_test.
	{
		const int N = 32;
		std::vector<std::thread> ts;
		std::vector<HttpResponse> rs(N);
		for (int i = 0; i < N; ++i)
			ts.emplace_back([&, i] { rs[i] = HttpGet("127.0.0.1", GATE_HTTP_PORT, "/get_test?i=" + std::to_string(i), 5000); });
		for (auto& t : ts) t.join();
		int good = 0;
		for (int i = 0; i < N; ++i) {
			// /get_test returns plain text ("receive get_test req ..."), not JSON.
			if (rs[i].status == 200 && rs[i].body.find("receive get_test") != std::string::npos) ++good;
		}
		Check(good == N, "gate-smoke: 32x GET /get_test complete",
			("only " + std::to_string(good) + "/" + std::to_string(N) + " ok").c_str());
		if (good != N) all_ok = false;
	}

	// Wave 2: 32 concurrent POST /user_login (exercises MySQL + StatusServer gRPC).
	{
		const int N = 32;
		std::vector<std::thread> ts;
		std::vector<HttpResponse> rs(N);
		for (int i = 0; i < N; ++i)
			ts.emplace_back([&, i] {
				rs[i] = HttpPost("127.0.0.1", GATE_HTTP_PORT, "/user_login",
					"{\"email\":\"smoke@test\",\"passwd\":\"x\"}", "text/json", 8000);
			});
		for (auto& t : ts) t.join();
		int good = 0;
		for (int i = 0; i < N; ++i) {
			auto j = ParseJson(rs[i].body);
			if (rs[i].status == 200 && j.is_object() && j.contains("error")) ++good;
		}
		Check(good == N, "gate-smoke: 32x POST /user_login complete",
			("only " + std::to_string(good) + "/" + std::to_string(N) + " ok").c_str());
		if (good != N) all_ok = false;
	}

	// Overlap proof: fire 32 POSTs, and while at least one is in flight, open a
	// brand-new connection with GET /get_test. If the probe succeeds, the server
	// accepted a new connection during active handler work (plan gate-smoke).
	{
		const int N = 32;
		std::atomic<int> inflight{0};
		std::vector<std::thread> ts;
		for (int i = 0; i < N; ++i)
			ts.emplace_back([&] {
				inflight.fetch_add(1);
				HttpPost("127.0.0.1", GATE_HTTP_PORT, "/user_login",
					"{\"email\":\"overlap@test\",\"passwd\":\"x\"}", "text/json", 8000);
				inflight.fetch_sub(1);
			});
		// Spin until at least one request is outstanding, then probe immediately.
		HttpResponse probe;
		bool caught_inflight = false;
		for (int spin = 0; spin < 2000; ++spin) {
			if (inflight.load() >= 1) {
				caught_inflight = true;
				probe = HttpGet("127.0.0.1", GATE_HTTP_PORT, "/get_test?probe=1", 5000);
				break;
			}
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
		for (auto& t : ts) t.join();
		auto pj_body = probe.body;
		const bool ok = caught_inflight && probe.status == 200
			&& pj_body.find("receive get_test") != std::string::npos;
		Check(ok, "gate-smoke: accepts new conn while handlers active",
			caught_inflight ? "probe did not get a complete response" : "no in-flight window observed");
		if (!ok) all_ok = false;
	}

	pm.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// order (shared by n4 / n1)
// ---------------------------------------------------------------------------

struct OrderSide {
	TcpClient* client = nullptr;
	int uid = 0;
	int peer = 0;
	int total = 0;
	const std::unordered_map<std::string, int>* uid_to_idx = nullptr;
	std::vector<int>        message_ids;   // canonical id by submission index
	std::vector<long long>  seqs;          // global receipt sequence by index
	std::vector<char>       received;
	std::atomic<int>        collected{0};
	std::atomic<bool>       ok{true};
};

static void OrderConsumer(OrderSide& s, std::atomic<int>& gseq, int deadline_ms) {
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(deadline_ms);
	while (s.collected.load() < s.total) {
		if (std::chrono::steady_clock::now() >= deadline) { s.ok = false; return; }
		Frame f;
		if (!s.client->Wait(0, 500, &f)) {
			if (s.client->IsClosed()) { s.ok = false; return; }
			continue;
		}
		if (f.id != ID_TEXT_CHAT_MSG_RSP) continue;  // drain 1019 etc.
		auto j = ParseJson(f.body);
		if (!j.is_object() || j.value("error", -1) != ERR_SUCCESS) { s.ok = false; return; }
		const auto& cd = j.value("chat_datas", json::array());
		if (!cd.is_array() || cd.empty()) { s.ok = false; return; }
		const std::string uid = cd[0].value("unique_id", "");
		const int mid = cd[0].value("message_id", 0);
		auto it = s.uid_to_idx->find(uid);
		if (it == s.uid_to_idx->end()) continue;
		const int idx = it->second;
		if (idx < 0 || idx >= s.total) continue;
		if (!s.received[idx]) {
			s.received[idx] = 1;
			s.message_ids[idx] = mid;
			s.seqs[idx] = gseq.fetch_add(1);
			s.collected.fetch_add(1);
		}
	}
}

static void OrderProducer(OrderSide& s, const std::string& tag,
                          std::atomic<bool>& go) {
	while (!go.load()) std::this_thread::yield();
	for (int i = 0; i < s.total; ++i) {
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%04d", i);
		const std::string unique_id = "imtest-order-" + tag + "-"
			+ std::to_string(s.uid) + "-" + buf;
		const std::string body = BuildTextReq(s.uid, s.peer, THREAD_ID,
			"m" + std::to_string(i), unique_id);
		if (!s.client->Send(ID_TEXT_CHAT_MSG_REQ, body)) { s.ok = false; return; }
	}
}

static bool RunOrderScenario(int logic_workers, bool require_overlap, const std::string& label) {
	std::printf("\n=== scenario: %s (LogicWorkers=%d) ===\n", label.c_str(), logic_workers);
	const int PER = 1000;
	ProcessManager pm;
	Redis redis; Mysql mysql;
	std::string tag = RunTag();

	if (!redis.Connect(REDIS_HOST, REDIS_PORT, REDIS_PASSWD)) {
		Fail(label + ": connect Redis", "redis connect failed"); return false;
	}
	if (!mysql.Connect(MYSQL_HOST, MYSQL_PORT, MYSQL_USER, MYSQL_PASSWD, MYSQL_SCHEMA)) {
		Fail(label + ": connect MySQL", "mysql connect failed"); return false;
	}

	// Teardown guard: clean footprint no matter how we exit.
	auto cleanup = [&] {
		// Close clients first so the server does not try to notify dead sockets.
		CleanupFootprint(redis, mysql);
	};

	if (!pm.Start({ "StatusServer", { "StatusServer", "StatusServer.exe" },
		MakeStatusIni(), STATUS_GRPC_PORT }, 15000)) {
		Fail(label + ": start StatusServer", "ready timeout"); cleanup(); return false;
	}
	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, logic_workers),
		CHAT1_TCP_PORT }, 15000)) {
		Fail(label + ": start ChatServer", "ready timeout"); cleanup(); return false;
	}

	TcpClient cA, cB;
	if (!LoginUser(redis, cA, SENDER_UID, "tokA-" + tag, CHAT1_TCP_PORT) ||
	    !LoginUser(redis, cB, RECEIVER_UID, "tokB-" + tag, CHAT1_TCP_PORT)) {
		Fail(label + ": login fixture uids", "1006 error or timeout"); cleanup(); return false;
	}

	// Build uid->index maps (shared, read-only) and side state.
	std::unordered_map<std::string, int> mapA, mapB;
	for (int i = 0; i < PER; ++i) {
		char buf[64]; std::snprintf(buf, sizeof(buf), "%04d", i);
		mapA["imtest-order-" + tag + "-" + std::to_string(SENDER_UID)   + "-" + buf] = i;
		mapB["imtest-order-" + tag + "-" + std::to_string(RECEIVER_UID) + "-" + buf] = i;
	}
	OrderSide sA{ &cA, SENDER_UID,   RECEIVER_UID, PER, &mapA };
	OrderSide sB{ &cB, RECEIVER_UID, SENDER_UID,   PER, &mapB };
	sA.message_ids.assign(PER, 0); sA.seqs.assign(PER, 0); sA.received.assign(PER, 0);
	sB.message_ids.assign(PER, 0); sB.seqs.assign(PER, 0); sB.received.assign(PER, 0);

	std::atomic<int>  gseq{0};
	std::atomic<bool> go{false};

	// Start consumers first so they are draining when producers fire.
	std::thread consA(&OrderConsumer, std::ref(sA), std::ref(gseq), 180000);
	std::thread consB(&OrderConsumer, std::ref(sB), std::ref(gseq), 180000);
	std::thread prodA(&OrderProducer, std::ref(sA), std::cref(tag), std::ref(go));
	std::thread prodB(&OrderProducer, std::ref(sB), std::cref(tag), std::ref(go));

	// Release both producers together (true concurrency start).
	go = true;

	prodA.join(); prodB.join();
	consA.join(); consB.join();

	cA.Close(); cB.Close();

	bool all_ok = true;

	// (1) Both sides collected all 1000 and no protocol error.
	Check(sA.ok.load() && sB.ok.load() &&
		      sA.collected.load() == PER && sB.collected.load() == PER,
		      label + ": both uids received 1000x 1018",
		      ("A collected=" + std::to_string(sA.collected.load()) +
	       " B collected=" + std::to_string(sB.collected.load()) +
	       " okA=" + std::to_string(sA.ok.load()) + " okB=" + std::to_string(sB.ok.load())).c_str());
	if (!(sA.ok.load() && sB.ok.load() &&
	      sA.collected.load() == PER && sB.collected.load() == PER)) all_ok = false;

	// (2) Per-uid: 1018 arrival order == submission order (same shard FIFO) and
	//     canonical message_id strictly increases.
	auto check_side = [&](OrderSide& s, const std::string& who) -> bool {
		long long prev_seq = -1; int prev_mid = -1; bool ordered = true, increasing = true;
		for (int i = 0; i < s.total; ++i) {
			if (!s.received[i]) { ordered = false; break; }
			if (s.seqs[i] <= prev_seq) { ordered = false; }
			prev_seq = s.seqs[i];
			if (s.message_ids[i] <= prev_mid) { increasing = false; }
			prev_mid = s.message_ids[i];
		}
		Check(ordered, label + ": " + who + " 1018 order preserved",
			"arrival sequence not monotonic in submission order");
		Check(increasing, label + ": " + who + " message_id strictly increasing",
			"canonical message_id not strictly increasing");
		return ordered && increasing;
	};
	if (!check_side(sA, "A=1002")) all_ok = false;
	if (!check_side(sB, "B=1019")) all_ok = false;

	// (3) Overlap (n4 only): the two uids' global receipt-seq ranges intersect,
	//     proving different shards processed them in parallel (not wall-clock).
	if (require_overlap) {
		long long amin = LLONG_MAX, amax = LLONG_MIN, bmin = LLONG_MAX, bmax = LLONG_MIN;
		for (int i = 0; i < PER; ++i) {
			if (sA.received[i]) { amin = std::min(amin, sA.seqs[i]); amax = std::max(amax, sA.seqs[i]); }
			if (sB.received[i]) { bmin = std::min(bmin, sB.seqs[i]); bmax = std::max(bmax, sB.seqs[i]); }
		}
		const bool overlap = (amin <= bmax && bmin <= amax);
		Check(overlap, label + ": A and B completion overlap (parallel shards)",
			("A[" + std::to_string(amin) + ".." + std::to_string(amax) +
			 "] B[" + std::to_string(bmin) + ".." + std::to_string(bmax) + "]").c_str());
		if (!overlap) all_ok = false;
	}

	cleanup();
	pm.StopAll();
	return all_ok;
}

bool ScenarioOrderN4() { return RunOrderScenario(4, true,  "order-n4"); }
bool ScenarioOrderN1() { return RunOrderScenario(1, false, "order-n1"); }

// ---------------------------------------------------------------------------
// probe-cleanup (diagnostic, not a ctest scenario)
// ---------------------------------------------------------------------------
bool ScenarioProbeCleanup() {
	std::printf("\n=== probe-cleanup ===\n");
	Mysql mysql; Redis redis;
	bool clean = true;
	if (!mysql.Connect(MYSQL_HOST, MYSQL_PORT, MYSQL_USER, MYSQL_PASSWD, MYSQL_SCHEMA)) {
		std::printf("[probe] mysql connect failed\n"); clean = false;
	}
	if (!redis.Connect(REDIS_HOST, REDIS_PORT, REDIS_PASSWD)) {
		std::printf("[probe] redis connect failed\n"); clean = false;
	}
	if (mysql.connected()) {
		const long long rows = mysql.CountByUniqueIdLike("imtest-%");
		std::printf("[probe] chat_message rows with unique_id LIKE 'imtest-%%': %lld\n", rows);
		if (rows != 0) clean = false;
	}
	if (redis.connected()) {
		int z1 = redis.ZCard("offline_msg:" + std::to_string(SENDER_UID));
		int z2 = redis.ZCard("offline_msg:" + std::to_string(RECEIVER_UID));
		bool t1=false, t2=false, u1=false, u2=false;
		redis.Exists("utoken_" + std::to_string(SENDER_UID), t1);
		redis.Exists("utoken_" + std::to_string(RECEIVER_UID), t2);
		redis.Exists("uip_" + std::to_string(SENDER_UID), u1);
		redis.Exists("uip_" + std::to_string(RECEIVER_UID), u2);
		std::printf("[probe] offline_msg:%d=%d  offline_msg:%d=%d  utoken A=%d B=%d  uip A=%d B=%d\n",
			SENDER_UID, z1, RECEIVER_UID, z2, (int)t1, (int)t2, (int)u1, (int)u2);
		if (z1 != 0 || z2 != 0 || t1 || t2 || u1 || u2) clean = false;
	}
	std::printf("[probe] result: %s\n", clean ? "CLEAN" : "DIRTY");
	return clean;
}

// ---------------------------------------------------------------------------
// dedup
// ---------------------------------------------------------------------------
bool ScenarioDedup() {
	std::printf("\n=== scenario: dedup ===\n");
	ProcessManager pm;
	Redis redis; Mysql mysql;
	std::string tag = RunTag();
	bool all_ok = true;

	auto cleanup = [&] { CleanupFootprint(redis, mysql); };

	if (!redis.Connect(REDIS_HOST, REDIS_PORT, REDIS_PASSWD)) {
		Fail("dedup: connect Redis", "redis connect failed"); return false;
	}
	if (!mysql.Connect(MYSQL_HOST, MYSQL_PORT, MYSQL_USER, MYSQL_PASSWD, MYSQL_SCHEMA)) {
		Fail("dedup: connect MySQL", "mysql connect failed"); return false;
	}

	if (!pm.Start({ "StatusServer", { "StatusServer", "StatusServer.exe" },
		MakeStatusIni(), STATUS_GRPC_PORT }, 15000)) {
		Fail("dedup: start StatusServer", "ready timeout"); cleanup(); return false;
	}
	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4),
		CHAT1_TCP_PORT }, 15000)) {
		Fail("dedup: start ChatServer", "ready timeout"); cleanup(); return false;
	}

	TcpClient c;
	if (!LoginUser(redis, c, SENDER_UID, "tokD-" + tag, CHAT1_TCP_PORT)) {
		Fail("dedup: login sender", "1006 error or timeout"); cleanup(); return false;
	}

	// --- Assertion A: identical (sender_id, unique_id) sent twice (pipelined) ->8740	//     two 1018 with the SAME message_id, exactly one DB row.
	{
		const std::string uid1 = "imtest-dedup-same-" + tag;
		const std::string body = BuildTextReq(SENDER_UID, RECEIVER_UID, THREAD_ID,
			"hello-dedup", uid1);
		// Pipeline two requests before reading either: wire-level concurrency.
		bool s1 = c.Send(ID_TEXT_CHAT_MSG_REQ, body);
		bool s2 = c.Send(ID_TEXT_CHAT_MSG_REQ, body);
		Frame r1, r2;
		bool g1 = c.Wait(ID_TEXT_CHAT_MSG_RSP, 10000, &r1);
		bool g2 = c.Wait(ID_TEXT_CHAT_MSG_RSP, 10000, &r2);
		int mid1 = -1, mid2 = -1, err1 = -1, err2 = -1;
		if (g1) { auto j = ParseJson(r1.body); err1 = j.value("error", -1);
			if (err1 == 0 && j.contains("chat_datas") && !j["chat_datas"].empty())
				mid1 = j["chat_datas"][0].value("message_id", -1); }
		if (g2) { auto j = ParseJson(r2.body); err2 = j.value("error", -1);
			if (err2 == 0 && j.contains("chat_datas") && !j["chat_datas"].empty())
				mid2 = j["chat_datas"][0].value("message_id", -1); }

		Check(s1 && s2 && g1 && g2 && err1 == 0 && err2 == 0,
			"dedup: both re-sends get 1018 success",
			("send1=" + std::to_string(s1) + " send2=" + std::to_string(s2) +
			 " err1=" + std::to_string(err1) + " err2=" + std::to_string(err2)).c_str());
		Check(mid1 > 0 && mid1 == mid2, "dedup: identical message_id returned",
			("mid1=" + std::to_string(mid1) + " mid2=" + std::to_string(mid2)).c_str());

		const long long rows = mysql.CountByUniqueId(uid1);
		Check(rows == 1, "dedup: single DB row for identical re-send",
			("rows=" + std::to_string(rows)).c_str());
		if (!(mid1 > 0 && mid1 == mid2 && rows == 1 &&
		      s1 && s2 && g1 && g2 && err1 == 0 && err2 == 0)) all_ok = false;
	}

	// --- Assertion B: same key, different content -> MESSAGE_CONFLICT, original
	//     row unchanged.
	{
		const std::string uid2 = "imtest-dedup-conflict-" + tag;
		// Establish the canonical row first (deterministic baseline).
		const std::string body_orig = BuildTextReq(SENDER_UID, RECEIVER_UID, THREAD_ID,
			"original-content", uid2);
		bool so = c.Send(ID_TEXT_CHAT_MSG_REQ, body_orig);
		Frame ro; bool go = c.Wait(ID_TEXT_CHAT_MSG_RSP, 10000, &ro);
		int mid_orig = -1, err_orig = -1;
		if (go) { auto j = ParseJson(ro.body); err_orig = j.value("error", -1);
			if (err_orig == 0 && j.contains("chat_datas") && !j["chat_datas"].empty())
				mid_orig = j["chat_datas"][0].value("message_id", -1); }
		Check(so && go && err_orig == 0 && mid_orig > 0, "dedup: establish baseline row",
			("err=" + std::to_string(err_orig) + " mid=" + std::to_string(mid_orig)).c_str());

		// Capture the baseline content straight from MySQL (the durable truth).
		std::string baseline_content;
		{ auto rows = mysql.QueryByUniqueId(SENDER_UID, uid2);
		  if (!rows.empty()) baseline_content = rows[0].content; }

		// Now send the same key with DIFFERENT content -> expect conflict.
		const std::string body_diff = BuildTextReq(SENDER_UID, RECEIVER_UID, THREAD_ID,
			"DIFFERENT-content", uid2);
		c.Send(ID_TEXT_CHAT_MSG_REQ, body_diff);
		Frame rd; bool gd = c.Wait(ID_TEXT_CHAT_MSG_RSP, 10000, &rd);
		int err_diff = -1;
		if (gd) { auto j = ParseJson(rd.body); err_diff = j.value("error", -1); }
		Check(gd && err_diff == ERR_MESSAGE_CONFLICT, "dedup: conflict returns MESSAGE_CONFLICT",
			("err=" + std::to_string(err_diff)).c_str());

		// Original row unchanged: still exactly one row, content == baseline.
		const long long rows2 = mysql.CountByUniqueId(uid2);
		std::string after_content;
		int after_mid = -1;
		{ auto rows = mysql.QueryByUniqueId(SENDER_UID, uid2);
		  if (!rows.empty()) { after_content = rows[0].content; after_mid = rows[0].message_id; } }
		Check(rows2 == 1 && after_content == baseline_content && after_mid == mid_orig,
			"dedup: original row unchanged after conflict",
			("rows=" + std::to_string(rows2) + " content_same=" +
			 std::to_string(after_content == baseline_content) +
			 " mid_same=" + std::to_string(after_mid == mid_orig)).c_str());

		if (!(so && go && err_orig == 0 && mid_orig > 0 &&
		      gd && err_diff == ERR_MESSAGE_CONFLICT &&
		      rows2 == 1 && after_content == baseline_content && after_mid == mid_orig))
			all_ok = false;
	}

	c.Close();
	cleanup();
	pm.StopAll();
	return all_ok;
}

} // namespace imt
