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
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/asio.hpp>

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

// ---------------------------------------------------------------------------
// Shared helpers for Verification.6 second half (offline / lost-ack / pull-
// bytes / cross-server / image-offline)
// ---------------------------------------------------------------------------

// Build a 1049 delivery-ACK body: {"uid":<receiver>,"message_ids":[...]}
static std::string BuildAckReq(int uid, const std::vector<int>& ids) {
	json j;
	j["uid"] = uid;
	json arr = json::array();
	for (int id : ids) arr.push_back(id);
	j["message_ids"] = arr;
	return j.dump();
}

// Build a 1051 pull-offline body: {"uid":<receiver>,"after_message_id":<id>,"limit":<n>}
static std::string BuildPullReq(int uid, int after_message_id, int limit) {
	json j;
	j["uid"] = uid;
	j["after_message_id"] = after_message_id;
	j["limit"] = limit;
	return j.dump();
}

// Build a 1035 image-chat-metadata body.
static std::string BuildImgMetaReq(int fromuid, int touid, int thread_id,
                                   const std::string& md5, const std::string& name,
                                   const std::string& token, const std::string& unique_id,
                                   long long content_size) {
	json j;
	j["fromuid"] = fromuid;
	j["touid"]   = touid;
	j["thread_id"] = thread_id;
	j["md5"]   = md5;
	j["name"]  = name;
	j["token"] = token;
	j["unique_id"] = unique_id;
	j["content_size"] = std::to_string(content_size);
	return j.dump();
}

// Build a 1037 image-upload-chunk body (single-chunk).
static std::string BuildImgUploadReq(int uid, int sender, int receiver, int message_id,
                                     const std::string& md5, const std::string& name,
                                     long long total_size, long long trans_size,
                                     int last, const std::string& data_b64) {
	json j;
	j["uid"]         = uid;
	j["sender"]      = sender;
	j["receiver"]    = receiver;
	j["message_id"]  = message_id;
	j["md5"]   = md5;
	j["name"]  = name;
	j["seq"]   = 1;
	j["total_size"] = std::to_string(total_size);
	j["trans_size"] = std::to_string(trans_size);
	j["last"]  = last;
	j["data"]  = data_b64;
	return j.dump();
}

// Minimal base64 encoder (no line breaks; standard alphabet).
static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string Base64Encode(const std::string& in) {
	std::string out;
	int val = 0, valb = -6;
	for (unsigned char c : in) {
		val = (val << 8) + c;
		valb += 8;
		while (valb >= 0) {
			out.push_back(kB64[(val >> valb) & 0x3F]);
			valb -= 6;
		}
	}
	if (valb > -6) out.push_back(kB64[((val << 8) >> (valb + 8)) & 0x3F]);
	while (out.size() % 4) out.push_back('=');
	return out;
}

// ---------------------------------------------------------------------------
// ResourceServer uses a different wire format than ChatServer:
//   [2-byte big-endian id][4-byte big-endian int32 length][UTF-8 JSON body]
// (HEAD_TOTAL_LEN=6, HEAD_DATA_LEN=4), vs ChatServer's
//   [2-byte id][2-byte short length][body]
// This synchronous helper sends one frame and reads one response.
// ---------------------------------------------------------------------------
struct ResFrame { short id = 0; std::string body; };

static bool ResSendAndRecv(const std::string& host, unsigned short port,
                           short id, const std::string& body, ResFrame* out,
                           int timeout_ms = 15000) {
	(void)timeout_ms;  // synchronous; connect/write/read are local
	namespace asio = boost::asio;
	using boost::asio::ip::tcp;
	try {
		asio::io_context ioc;
		tcp::socket sock(ioc);
		tcp::resolver resolver(ioc);
		auto eps = resolver.resolve(host, std::to_string(port));
		asio::connect(sock, eps);

		// Encode 6-byte header + body.
		const unsigned short id_be = static_cast<unsigned short>(id);
		const std::uint32_t len_be = static_cast<std::uint32_t>(body.size());
		std::string frame;
		frame.resize(6 + body.size());
		frame[0] = static_cast<char>((id_be >> 8) & 0xFF);
		frame[1] = static_cast<char>(id_be & 0xFF);
		frame[2] = static_cast<char>((len_be >> 24) & 0xFF);
		frame[3] = static_cast<char>((len_be >> 16) & 0xFF);
		frame[4] = static_cast<char>((len_be >>  8) & 0xFF);
		frame[5] = static_cast<char>(len_be & 0xFF);
		std::memcpy(&frame[6], body.data(), body.size());

		asio::write(sock, asio::buffer(frame));

		// Read 6-byte header.
		char hdr[6];
		std::size_t got = 0;
		while (got < 6) {
			got += sock.read_some(asio::buffer(hdr + got, 6 - got));
		}
		short rsp_id = static_cast<short>(
			(static_cast<unsigned short>(static_cast<unsigned char>(hdr[0])) << 8) |
			static_cast<unsigned char>(hdr[1]));
		std::uint32_t rsp_len =
			(static_cast<std::uint32_t>(static_cast<unsigned char>(hdr[2])) << 24) |
			(static_cast<std::uint32_t>(static_cast<unsigned char>(hdr[3])) << 16) |
			(static_cast<std::uint32_t>(static_cast<unsigned char>(hdr[4])) <<  8) |
			static_cast<unsigned char>(hdr[5]);
		if (rsp_len > 1024 * 1024) return false;  // sanity

		// Read body.
		std::string rsp_body(rsp_len, '\0');
		got = 0;
		while (got < rsp_len) {
			got += sock.read_some(asio::buffer(&rsp_body[got], rsp_len - got));
		}

		boost::system::error_code ec;
		sock.close(ec);
		out->id = rsp_id;
		out->body = std::move(rsp_body);
		return true;
	} catch (...) {
		return false;
	}
}

// ---------------------------------------------------------------------------
// GrpcProxy — in-process TCP forwarder that can break connections on demand.
//
// Used by cross-server to sit between chatserver1 and chatserver2's gRPC
// endpoint. When break_mode_ == true every accepted connection is immediately
// reset (RST), forcing the gRPC client to see UNAVAILABLE on every attempt.
// connection_count_ records how many inbound connections were accepted.
// ---------------------------------------------------------------------------
class GrpcProxy {
public:
	GrpcProxy() = default;
	~GrpcProxy() { Stop(); }

	bool Start(const std::string& target_host, unsigned short target_port,
	           unsigned short listen_port) {
		target_host_ = target_host;
		target_port_ = target_port;
		break_mode_ = true;
		connection_count_ = 0;
		running_ = true;
		try {
			acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(
				ioc_, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), listen_port));
			DoAccept();
			thread_ = std::thread([this] { ioc_.run(); });
			return true;
		} catch (...) {
			return false;
		}
	}

	void SetPassThrough() { break_mode_ = false; }
	int  connection_count() const { return connection_count_.load(); }

	void Stop() {
		if (!running_.exchange(false)) return;
		boost::system::error_code ec;
		if (acceptor_) acceptor_->close(ec);
		ioc_.stop();
		if (thread_.joinable()) thread_.join();
		ioc_.restart();
		acceptor_.reset();
	}

private:
	using tcp = boost::asio::ip::tcp;
	using socket_ptr = std::shared_ptr<tcp::socket>;

	void DoAccept() {
		if (!running_) return;
		auto sock = std::make_shared<tcp::socket>(ioc_);
		acceptor_->async_accept(*sock,
			[this, sock](const boost::system::error_code& ec) {
				if (ec) return;
				connection_count_.fetch_add(1);
				HandleConnection(sock);
				DoAccept();
			});
	}

	void HandleConnection(socket_ptr upstream) {
		if (break_mode_) {
			// Break: close immediately with abort to force RST.
			boost::system::error_code ig;
			upstream->set_option(boost::asio::ip::tcp::socket::linger(true, 0), ig);
			upstream->close(ig);
			return;
		}
		// Pass-through: connect to target and relay.
		auto downstream = std::make_shared<tcp::socket>(ioc_);
		auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(ioc_);
		auto self = this;
		auto up = upstream;
		auto down = downstream;
		resolver->async_resolve(target_host_, std::to_string(target_port_),
			[self, up, down, resolver](const boost::system::error_code& ec,
			                           boost::asio::ip::tcp::resolver::results_type eps) {
				if (ec) return;
				boost::asio::async_connect(*down, eps,
					[up, down](const boost::system::error_code& e2,
					           const boost::asio::ip::tcp::endpoint&) {
						if (e2) return;
						Relay(up, down);
						Relay(down, up);
					});
			});
	}

	static void Relay(socket_ptr from, socket_ptr to) {
		auto buf = std::make_shared<std::vector<char>>(8192);
		from->async_read_some(boost::asio::buffer(*buf),
			[from, to, buf](const boost::system::error_code& ec, std::size_t n) {
				if (ec || n == 0) {
					boost::system::error_code ig;
					from->close(ig);
					to->close(ig);
					return;
				}
				boost::asio::async_write(*to, boost::asio::buffer(buf->data(), n),
					[from, to, buf](const boost::system::error_code& e2, std::size_t) {
						if (e2) {
							boost::system::error_code ig;
							from->close(ig);
							to->close(ig);
							return;
						}
						Relay(from, to);
					});
				});
	}

	boost::asio::io_context ioc_;
	std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
	std::thread thread_;
	std::atomic<bool> running_{false};
	std::atomic<bool> break_mode_{true};
	std::atomic<int>  connection_count_{0};
	std::string target_host_;
	unsigned short target_port_ = 0;
};

// Parse a 1052 pull response, extracting message_ids in order.
static std::vector<int> ExtractPullMessageIds(const json& j) {
	std::vector<int> ids;
	if (!j.is_object() || !j.contains("messages")) return ids;
	const auto& msgs = j["messages"];
	if (!msgs.is_array()) return ids;
	for (const auto& m : msgs) {
		if (m.contains("message_id")) ids.push_back(m.value("message_id", 0));
	}
	return ids;
}

// ---------------------------------------------------------------------------
// offline (Verification.6)
//
// receiver 下线时 sender 发 100 条→MySQL 全 delivery_status=0、Redis ZSET 100
// 唯一 ID；receiver 登录后 headless 连续 1051 分页拉取，每 ID 只出现一次；
// 1049/1050 完成后 DB 全 1 且 ZSET 空。
// ---------------------------------------------------------------------------
bool ScenarioOffline() {
	std::printf("\n=== scenario: offline ===\n");
	ProcessManager pm;
	Redis redis; Mysql mysql;
	std::string tag = RunTag();
	bool all_ok = true;
	const int MSG_COUNT = 100;

	auto cleanup = [&] { CleanupFootprint(redis, mysql); };

	if (!redis.Connect(REDIS_HOST, REDIS_PORT, REDIS_PASSWD)) {
		Fail("offline: connect Redis", "redis connect failed"); return false;
	}
	if (!mysql.Connect(MYSQL_HOST, MYSQL_PORT, MYSQL_USER, MYSQL_PASSWD, MYSQL_SCHEMA)) {
		Fail("offline: connect MySQL", "mysql connect failed"); return false;
	}
	cleanup();

	if (!pm.Start({ "StatusServer", { "StatusServer", "StatusServer.exe" },
		MakeStatusIni(), STATUS_GRPC_PORT }, 15000)) {
		Fail("offline: start StatusServer", "ready timeout"); cleanup(); return false;
	}
	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4),
		CHAT1_TCP_PORT }, 15000)) {
		Fail("offline: start ChatServer", "ready timeout"); cleanup(); return false;
	}

	// Login sender only — receiver stays offline.
	TcpClient cS;
	if (!LoginUser(redis, cS, SENDER_UID, "tokO-" + tag, CHAT1_TCP_PORT)) {
		Fail("offline: login sender", "1006 error or timeout"); cleanup(); pm.StopAll(); return false;
	}

	// Send MSG_COUNT messages while receiver is offline.
	std::vector<int> sent_mids;
	sent_mids.reserve(MSG_COUNT);
	bool send_ok = true;
	for (int i = 0; i < MSG_COUNT; ++i) {
		char buf[64]; std::snprintf(buf, sizeof(buf), "%03d", i);
		const std::string uid_str = "imtest-offline-" + tag + "-" + buf;
		const std::string body = BuildTextReq(SENDER_UID, RECEIVER_UID, THREAD_ID,
			"offline-" + std::to_string(i), uid_str);
		if (!cS.Send(ID_TEXT_CHAT_MSG_REQ, body)) { send_ok = false; break; }
	}
	// Collect all 1018 responses.
	for (int i = 0; i < MSG_COUNT && send_ok; ++i) {
		Frame f;
		if (!cS.Wait(ID_TEXT_CHAT_MSG_RSP, 15000, &f)) { send_ok = false; break; }
		auto j = ParseJson(f.body);
		if (!j.is_object() || j.value("error", -1) != ERR_SUCCESS) { send_ok = false; break; }
		const auto& cd = j.value("chat_datas", json::array());
		if (cd.is_array() && !cd.empty())
			sent_mids.push_back(cd[0].value("message_id", 0));
	}
	Check(send_ok && (int)sent_mids.size() == MSG_COUNT,
		"offline: sender received 100x 1018",
		("sent=" + std::to_string(sent_mids.size())).c_str());
	if (!send_ok || (int)sent_mids.size() != MSG_COUNT) all_ok = false;

	// (1) MySQL: all 100 rows delivery_status=0.
	{
		const std::string pattern = "imtest-offline-" + tag + "-%";
		const long long total = mysql.CountByUniqueIdLike(pattern);
		const long long pending = mysql.CountByUniqueIdLikeAndDelivery(pattern, 0);
		Check(total == MSG_COUNT && pending == MSG_COUNT,
			"offline: MySQL 100 rows all delivery_status=0",
			("total=" + std::to_string(total) + " pending=" + std::to_string(pending)).c_str());
		if (total != MSG_COUNT || pending != MSG_COUNT) all_ok = false;
	}

	// (2) Redis: ZSET has the pending IDs.
	int zcard_before = redis.ZCard("offline_msg:" + std::to_string(RECEIVER_UID));
	Check(zcard_before >= MSG_COUNT,
		"offline: Redis ZSET has >= 100 pending IDs",
		("zcard=" + std::to_string(zcard_before)).c_str());
	if (zcard_before < MSG_COUNT) all_ok = false;

	// Login receiver and pull all pending via 1051.
	TcpClient cR;
	if (!LoginUser(redis, cR, RECEIVER_UID, "tokR-" + tag, CHAT1_TCP_PORT)) {
		Fail("offline: login receiver", "1006 error or timeout"); cS.Close(); cleanup(); pm.StopAll(); return false;
	}

	// Paged pull: after_message_id=0, limit=100, follow has_more/next_message_id.
	std::vector<int> pulled_ids;
	int cursor = 0;
	bool pull_ok = true;
	int pages = 0;
	while (true) {
		++pages;
		std::string body = BuildPullReq(RECEIVER_UID, cursor, 100);
		if (!cR.Send(ID_PULL_OFFLINE_MSG_REQ, body)) { pull_ok = false; break; }
		Frame f;
		if (!cR.Wait(ID_PULL_OFFLINE_MSG_RSP, 10000, &f)) { pull_ok = false; break; }
		auto j = ParseJson(f.body);
		if (!j.is_object() || j.value("error", -1) != ERR_SUCCESS) { pull_ok = false; break; }
		auto page_ids = ExtractPullMessageIds(j);
		for (int id : page_ids) pulled_ids.push_back(id);
		bool has_more = j.value("has_more", false);
		int next_mid = j.value("next_message_id", cursor);
		if (!has_more || page_ids.empty()) break;
		cursor = next_mid;
		if (pages > 20) { pull_ok = false; break; }  // safety
	}

	// (3) Every sent ID appears exactly once in the pull stream.
	std::set<int> sent_set(sent_mids.begin(), sent_mids.end());
	std::set<int> pulled_set(pulled_ids.begin(), pulled_ids.end());
	bool no_dup = ((int)pulled_ids.size() == (int)pulled_set.size());
	bool all_present = (sent_set == pulled_set);
	Check(pull_ok && no_dup && all_present && (int)pulled_set.size() == MSG_COUNT,
		"offline: paged pull returns all 100 IDs exactly once",
		("pulled=" + std::to_string(pulled_ids.size()) +
		 " unique=" + std::to_string(pulled_set.size()) +
		 " dup=" + std::to_string(!no_dup)).c_str());
	if (!(pull_ok && no_dup && all_present)) all_ok = false;

	// (4) ACK all via 1049 → 1050.
	bool ack_ok = false;
	{
		std::vector<int> ids_vec(pulled_set.begin(), pulled_set.end());
		std::string body = BuildAckReq(RECEIVER_UID, ids_vec);
		if (cR.Send(ID_CHAT_DELIVERY_ACK_REQ, body)) {
			Frame f;
			if (cR.Wait(ID_CHAT_DELIVERY_ACK_RSP, 10000, &f)) {
				auto j = ParseJson(f.body);
				ack_ok = j.is_object() && j.value("error", -1) == ERR_SUCCESS;
			}
		}
	}
	Check(ack_ok, "offline: ACK 1049/1050 success", "ack failed");
	if (!ack_ok) all_ok = false;

	// (5) After ACK: MySQL all delivery_status=1, Redis ZSET empty.
	// The server ZRems AFTER sending 1050, so poll briefly for the ZSET to drain.
	for (int poll = 0; poll < 50; ++poll) {
		if (redis.ZCard("offline_msg:" + std::to_string(RECEIVER_UID)) == 0) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	{
		const std::string pattern = "imtest-offline-" + tag + "-%";
		const long long pending = mysql.CountByUniqueIdLikeAndDelivery(pattern, 0);
		const long long acked = mysql.CountByUniqueIdLikeAndDelivery(pattern, 1);
		Check(pending == 0 && acked == MSG_COUNT,
			"offline: post-ACK MySQL all delivery_status=1",
			("pending=" + std::to_string(pending) + " acked=" + std::to_string(acked)).c_str());
		if (!(pending == 0 && acked == MSG_COUNT)) all_ok = false;
	}
	int zcard_after = redis.ZCard("offline_msg:" + std::to_string(RECEIVER_UID));
	Check(zcard_after == 0,
		"offline: post-ACK Redis ZSET empty",
		("zcard=" + std::to_string(zcard_after)).c_str());
	if (zcard_after != 0) all_ok = false;

	cS.Close(); cR.Close();
	cleanup();
	pm.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// lost-ack (Verification.6)
//
// 故意丢第一轮 1049（不发），下一次 pull 再得同一 message_id；去重层不重复呈现，
// 第二次 ACK 后不再返回。
// ---------------------------------------------------------------------------
bool ScenarioLostAck() {
	std::printf("\n=== scenario: lost-ack ===\n");
	ProcessManager pm;
	Redis redis; Mysql mysql;
	std::string tag = RunTag();
	bool all_ok = true;

	auto cleanup = [&] { CleanupFootprint(redis, mysql); };

	if (!redis.Connect(REDIS_HOST, REDIS_PORT, REDIS_PASSWD)) {
		Fail("lost-ack: connect Redis", "redis connect failed"); return false;
	}
	if (!mysql.Connect(MYSQL_HOST, MYSQL_PORT, MYSQL_USER, MYSQL_PASSWD, MYSQL_SCHEMA)) {
		Fail("lost-ack: connect MySQL", "mysql connect failed"); return false;
	}
	cleanup();

	if (!pm.Start({ "StatusServer", { "StatusServer", "StatusServer.exe" },
		MakeStatusIni(), STATUS_GRPC_PORT }, 15000)) {
		Fail("lost-ack: start StatusServer", "ready timeout"); cleanup(); return false;
	}
	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4),
		CHAT1_TCP_PORT }, 15000)) {
		Fail("lost-ack: start ChatServer", "ready timeout"); cleanup(); return false;
	}

	// Send one message to offline receiver.
	TcpClient cS;
	if (!LoginUser(redis, cS, SENDER_UID, "tokL-" + tag, CHAT1_TCP_PORT)) {
		Fail("lost-ack: login sender", "failed"); cleanup(); pm.StopAll(); return false;
	}
	const std::string uid_str = "imtest-lostack-" + tag;
	std::string body = BuildTextReq(SENDER_UID, RECEIVER_UID, THREAD_ID,
		"lost-ack-msg", uid_str);
	cS.Send(ID_TEXT_CHAT_MSG_REQ, body);
	Frame rsp; bool got_rsp = cS.Wait(ID_TEXT_CHAT_MSG_RSP, 10000, &rsp);
	int sent_mid = -1;
	if (got_rsp) {
		auto j = ParseJson(rsp.body);
		if (j.is_object() && j.value("error", -1) == ERR_SUCCESS &&
		    j.contains("chat_datas") && !j["chat_datas"].empty())
			sent_mid = j["chat_datas"][0].value("message_id", -1);
	}
	Check(got_rsp && sent_mid > 0, "lost-ack: message sent and persisted",
		("mid=" + std::to_string(sent_mid)).c_str());
	if (!(got_rsp && sent_mid > 0)) { cS.Close(); cleanup(); pm.StopAll(); return false; }

	// Login receiver, pull → get message_id (first time).
	TcpClient cR;
	if (!LoginUser(redis, cR, RECEIVER_UID, "tokR-" + tag, CHAT1_TCP_PORT)) {
		Fail("lost-ack: login receiver", "failed"); cS.Close(); cleanup(); pm.StopAll(); return false;
	}

	auto do_pull = [&](int after) -> std::vector<int> {
		cR.Send(ID_PULL_OFFLINE_MSG_REQ, BuildPullReq(RECEIVER_UID, after, 100));
		Frame f;
		if (!cR.Wait(ID_PULL_OFFLINE_MSG_RSP, 10000, &f)) return {};
		auto j = ParseJson(f.body);
		if (!j.is_object() || j.value("error", -1) != ERR_SUCCESS) return {};
		return ExtractPullMessageIds(j);
	};

	auto first_ids = do_pull(0);
	bool first_has = std::find(first_ids.begin(), first_ids.end(), sent_mid) != first_ids.end();
	Check(first_has, "lost-ack: first pull returns the message",
		("ids=" + std::to_string(first_ids.size())).c_str());
	if (!first_has) all_ok = false;

	// Deliberately do NOT ACK (simulates lost first 1049 round).
	// Pull again → same message_id must still be there.
	auto second_ids = do_pull(0);
	bool second_has = std::find(second_ids.begin(), second_ids.end(), sent_mid) != second_ids.end();
	Check(second_has, "lost-ack: re-pull (no ACK) returns same message_id",
		("ids=" + std::to_string(second_ids.size())).c_str());
	if (!second_has) all_ok = false;

	// Now ACK it (second round).
	bool ack_ok = false;
	{
		std::string ab = BuildAckReq(RECEIVER_UID, {sent_mid});
		if (cR.Send(ID_CHAT_DELIVERY_ACK_REQ, ab)) {
			Frame af;
			if (cR.Wait(ID_CHAT_DELIVERY_ACK_RSP, 10000, &af)) {
				auto j = ParseJson(af.body);
				ack_ok = j.is_object() && j.value("error", -1) == ERR_SUCCESS;
			}
		}
	}
	Check(ack_ok, "lost-ack: second ACK succeeds", "ack failed");
	if (!ack_ok) all_ok = false;

	// Pull again → message_id no longer returned.
	auto third_ids = do_pull(0);
	bool third_absent = std::find(third_ids.begin(), third_ids.end(), sent_mid) == third_ids.end();
	Check(third_absent, "lost-ack: post-ACK pull no longer returns message",
		("ids=" + std::to_string(third_ids.size())).c_str());
	if (!third_absent) all_ok = false;

	cS.Close(); cR.Close();
	cleanup();
	pm.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// pull-bytes (Verification.6)
//
// 接近单条 2 KiB 消息连续多发，1052 每页 encoded payload < PullMaxBytes(30000)
// 且 < 16-bit frame 上限；has_more/next_message_id 正确续取。
// ---------------------------------------------------------------------------
bool ScenarioPullBytes() {
	std::printf("\n=== scenario: pull-bytes ===\n");
	ProcessManager pm;
	Redis redis; Mysql mysql;
	std::string tag = RunTag();
	bool all_ok = true;
	const int MSG_COUNT = 40;  // ~1.9KiB each → multiple pages under PullMaxBytes
	// Server MAX_LENGTH=2048 caps the 1017 body; content must leave room for JSON
	// overhead (~114 bytes), so 1900 chars → body ~2014 bytes, just under the limit.
	const int CONTENT_LEN = 1900;

	auto cleanup = [&] { CleanupFootprint(redis, mysql); };

	if (!redis.Connect(REDIS_HOST, REDIS_PORT, REDIS_PASSWD)) {
		Fail("pull-bytes: connect Redis", "failed"); return false;
	}
	if (!mysql.Connect(MYSQL_HOST, MYSQL_PORT, MYSQL_USER, MYSQL_PASSWD, MYSQL_SCHEMA)) {
		Fail("pull-bytes: connect MySQL", "failed"); return false;
	}
	cleanup();

	if (!pm.Start({ "StatusServer", { "StatusServer", "StatusServer.exe" },
		MakeStatusIni(), STATUS_GRPC_PORT }, 15000)) {
		Fail("pull-bytes: start StatusServer", "ready timeout"); cleanup(); return false;
	}
	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4),
		CHAT1_TCP_PORT }, 15000)) {
		Fail("pull-bytes: start ChatServer", "ready timeout"); cleanup(); return false;
	}

	TcpClient cS;
	if (!LoginUser(redis, cS, SENDER_UID, "tokPB-" + tag, CHAT1_TCP_PORT)) {
		Fail("pull-bytes: login sender", "failed"); cleanup(); pm.StopAll(); return false;
	}

	// Send MSG_COUNT ~2KiB messages to offline receiver.
	const std::string pattern = "imtest-pullbytes-" + tag + "-%";
	for (int i = 0; i < MSG_COUNT; ++i) {
		char buf[64]; std::snprintf(buf, sizeof(buf), "%03d", i);
		const std::string uid_str = "imtest-pullbytes-" + tag + "-" + buf;
		// content of exactly CONTENT_LEN chars.
		std::string content(CONTENT_LEN, 'A' + (i % 26));
		std::string body = BuildTextReq(SENDER_UID, RECEIVER_UID, THREAD_ID,
			content, uid_str);
		cS.Send(ID_TEXT_CHAT_MSG_REQ, body);
	}
	// Drain 1018 responses.
	int got_1018 = 0;
	for (int i = 0; i < MSG_COUNT; ++i) {
		Frame f;
		if (!cS.Wait(ID_TEXT_CHAT_MSG_RSP, 15000, &f)) break;
		auto j = ParseJson(f.body);
		if (j.is_object() && j.value("error", -1) == ERR_SUCCESS) ++got_1018;
	}
	Check(got_1018 == MSG_COUNT, "pull-bytes: sender received all 1018",
		("got=" + std::to_string(got_1018)).c_str());
	if (got_1018 != MSG_COUNT) all_ok = false;

	// Login receiver, pull page by page.
	TcpClient cR;
	if (!LoginUser(redis, cR, RECEIVER_UID, "tokR-" + tag, CHAT1_TCP_PORT)) {
		Fail("pull-bytes: login receiver", "failed"); cS.Close(); cleanup(); pm.StopAll(); return false;
	}

	std::set<int> all_pulled;
	int cursor = 0;
	bool pull_ok = true;
	bool byte_violation = false;
	int pages = 0;
	while (true) {
		++pages;
		cR.Send(ID_PULL_OFFLINE_MSG_REQ, BuildPullReq(RECEIVER_UID, cursor, 100));
		Frame f;
		if (!cR.Wait(ID_PULL_OFFLINE_MSG_RSP, 10000, &f)) { pull_ok = false; break; }
		// Assert the raw frame body (encoded 1052 payload) stays under both limits.
		const std::size_t body_bytes = f.body.size();
		const std::size_t frame_bytes = body_bytes + HEAD_TOTAL_LEN;
		if (body_bytes > (std::size_t)PULL_MAX_BYTES) byte_violation = true;
		if (frame_bytes > 0xFF00) byte_violation = true;  // short safety
		auto j = ParseJson(f.body);
		if (!j.is_object() || j.value("error", -1) != ERR_SUCCESS) { pull_ok = false; break; }
		auto ids = ExtractPullMessageIds(j);
		for (int id : ids) all_pulled.insert(id);
		bool has_more = j.value("has_more", false);
		int next_mid = j.value("next_message_id", cursor);
		if (!has_more || ids.empty()) break;
		cursor = next_mid;
		if (pages > 20) { pull_ok = false; break; }
	}

	Check(!byte_violation, "pull-bytes: every 1052 frame < PullMaxBytes and < SHRT_MAX",
		byte_violation ? "frame exceeded limit" : "ok");
	if (byte_violation) all_ok = false;

	Check(pull_ok && (int)all_pulled.size() == MSG_COUNT,
		"pull-bytes: paged pull returns all messages",
		("pulled=" + std::to_string(all_pulled.size()) +
		 " pages=" + std::to_string(pages)).c_str());
	if (!pull_ok || (int)all_pulled.size() != MSG_COUNT) all_ok = false;

	// Clean up: ACK all.
	if (!all_pulled.empty()) {
		std::vector<int> v(all_pulled.begin(), all_pulled.end());
		cR.Send(ID_CHAT_DELIVERY_ACK_REQ, BuildAckReq(RECEIVER_UID, v));
		Frame af; cR.Wait(ID_CHAT_DELIVERY_ACK_RSP, 10000, &af);
	}

	cS.Close(); cR.Close();
	cleanup();
	pm.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// cross-server (Verification.6)
//
// 双 Chat 实例，receiver 登录到 chatserver2；在本节点 gRPC 端口放可控 proxy，
// 只在第一次 NotifyTextChatMsg 断流并计数尝试次数；验证每次调用 ≤3s、最多 3
// 次、sender 仍收 1018；kill/restart receiver ChatServer 后 receiver 从 pending
// pull 到消息。
// ---------------------------------------------------------------------------
bool ScenarioCrossServer() {
	std::printf("\n=== scenario: cross-server ===\n");
	ProcessManager pm;
	Redis redis; Mysql mysql;
	std::string tag = RunTag();
	bool all_ok = true;

	auto cleanup = [&] { CleanupFootprint(redis, mysql); };

	if (!redis.Connect(REDIS_HOST, REDIS_PORT, REDIS_PASSWD)) {
		Fail("cross-server: connect Redis", "failed"); return false;
	}
	if (!mysql.Connect(MYSQL_HOST, MYSQL_PORT, MYSQL_USER, MYSQL_PASSWD, MYSQL_SCHEMA)) {
		Fail("cross-server: connect MySQL", "failed"); return false;
	}
	cleanup();

	if (!pm.Start({ "StatusServer", { "StatusServer", "StatusServer.exe" },
		MakeStatusIni(), STATUS_GRPC_PORT }, 15000)) {
		Fail("cross-server: start StatusServer", "ready timeout"); cleanup(); return false;
	}
	// chatserver1: peer = chatserver2, but routed THROUGH the proxy port.
	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIniPeer("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4,
		                "chatserver2", CHAT2_TCP_PORT, CHAT2_PROXY_GRPC_PORT),
		CHAT1_TCP_PORT }, 15000)) {
		Fail("cross-server: start chatserver1", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}
	if (!pm.Start({ "chatserver2", { "chatserver2", "ChatServer.exe" },
		MakeChatIniPeer("chatserver2", CHAT2_TCP_PORT, CHAT2_GRPC_PORT, 4,
		                "chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT),
		CHAT2_TCP_PORT }, 15000)) {
		Fail("cross-server: start chatserver2", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}

	// Proxy: chatserver1 → proxy(15057) → chatserver2 gRPC(15056).
	// Break mode resets every connection → gRPC UNAVAILABLE.
	GrpcProxy proxy;
	if (!proxy.Start("127.0.0.1", CHAT2_GRPC_PORT, CHAT2_PROXY_GRPC_PORT)) {
		Fail("cross-server: start proxy", "failed"); cleanup(); pm.StopAll(); return false;
	}
	std::printf("[cross-server] proxy started on %d → %d (break mode)\n",
		CHAT2_PROXY_GRPC_PORT, CHAT2_GRPC_PORT);

	// Login receiver on chatserver2 (uip_1019 = "chatserver2").
	TcpClient cR;
	if (!LoginUser(redis, cR, RECEIVER_UID, "tokR-" + tag, CHAT2_TCP_PORT)) {
		Fail("cross-server: login receiver on chatserver2", "failed");
		proxy.Stop(); cleanup(); pm.StopAll(); return false;
	}
	// Login sender on chatserver1.
	TcpClient cS;
	if (!LoginUser(redis, cS, SENDER_UID, "tokS-" + tag, CHAT1_TCP_PORT)) {
		Fail("cross-server: login sender on chatserver1", "failed");
		cR.Close(); proxy.Stop(); cleanup(); pm.StopAll(); return false;
	}

	// Sender sends one message to receiver (cross-server via proxy → broken).
	const std::string uid_str = "imtest-cross-" + tag;
	std::string body = BuildTextReq(SENDER_UID, RECEIVER_UID, THREAD_ID,
		"cross-server-msg", uid_str);
	auto t_start = std::chrono::steady_clock::now();
	cS.Send(ID_TEXT_CHAT_MSG_REQ, body);

	// Sender still receives 1018 (MySQL commit before RPC).
	Frame rsp;
	bool got_1018 = cS.Wait(ID_TEXT_CHAT_MSG_RSP, 15000, &rsp);
	int sent_mid = -1;
	if (got_1018) {
		auto j = ParseJson(rsp.body);
		if (j.is_object() && j.value("error", -1) == ERR_SUCCESS &&
		    j.contains("chat_datas") && !j["chat_datas"].empty())
			sent_mid = j["chat_datas"][0].value("message_id", -1);
	}
	Check(got_1018 && sent_mid > 0, "cross-server: sender got 1018 despite broken RPC",
		("mid=" + std::to_string(sent_mid)).c_str());
	if (!(got_1018 && sent_mid > 0)) all_ok = false;

	// Wait for chatserver1's delivery worker to exhaust retries against the proxy.
	// Max 3 attempts × 3s deadline + backoff ≈ 9.5s; poll until connections stabilise.
	for (int i = 0; i < 120; ++i) {  // up to 12s
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	const int proxy_conns = proxy.connection_count();
	auto t_end = std::chrono::steady_clock::now();
	const double rpc_phase_sec = std::chrono::duration<double>(t_end - t_start).count();
	std::printf("[cross-server] proxy connections=%d  rpc_phase=%.1fs\n",
		proxy_conns, rpc_phase_sec);

	Check(proxy_conns >= 1, "cross-server: proxy observed at least 1 attempt",
		("conns=" + std::to_string(proxy_conns)).c_str());
	if (proxy_conns < 1) all_ok = false;

	// Total RPC phase bounded by 3 attempts × deadline + backoff + margin.
	Check(rpc_phase_sec <= 15.0, "cross-server: RPC phase bounded (≤15s for 3×3s+backoff)",
		(std::to_string(rpc_phase_sec) + "s").c_str());
	if (rpc_phase_sec > 15.0) all_ok = false;

	// Message is in pending: MySQL delivery_status=0, Redis ZSET has the ID.
	{
		auto rows = mysql.QueryByUniqueId(SENDER_UID, uid_str);
		bool db_pending = !rows.empty() && rows[0].delivery_status == 0;
		Check(db_pending, "cross-server: message in MySQL pending (delivery_status=0)",
			rows.empty() ? "no row" : ("ds=" + std::to_string(rows[0].delivery_status)).c_str());
		if (!db_pending) all_ok = false;
	}
	int zc = redis.ZCard("offline_msg:" + std::to_string(RECEIVER_UID));
	bool redis_pending = false;
	{
		std::vector<std::string> members;
		redis.ZRange("offline_msg:" + std::to_string(RECEIVER_UID), members);
		std::string mid_str = std::to_string(sent_mid);
		for (const auto& m : members) { if (m == mid_str) { redis_pending = true; break; } }
	}
	Check(redis_pending, "cross-server: message_id in Redis pending ZSET",
		("zcard=" + std::to_string(zc)).c_str());
	if (!redis_pending) all_ok = false;

	// Phase 2: kill/restart chatserver2, receiver reconnects, pulls pending.
	proxy.Stop();
	cR.Close();

	pm.StopOne("chatserver2");
	std::this_thread::sleep_for(std::chrono::milliseconds(500));

	// Restart chatserver2 with the same config (direct, no proxy needed for pull).
	if (!pm.Start({ "chatserver2", { "chatserver2", "ChatServer.exe" },
		MakeChatIniPeer("chatserver2", CHAT2_TCP_PORT, CHAT2_GRPC_PORT, 4,
		                "chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT),
		CHAT2_TCP_PORT }, 15000)) {
		Fail("cross-server: restart chatserver2", "ready timeout");
		cS.Close(); cleanup(); pm.StopAll(); return false;
	}

	// Receiver reconnects and logs in.
	TcpClient cR2;
	if (!LoginUser(redis, cR2, RECEIVER_UID, "tokR2-" + tag, CHAT2_TCP_PORT)) {
		Fail("cross-server: receiver reconnect after restart", "failed");
		cS.Close(); cleanup(); pm.StopAll(); return false;
	}

	// Pull pending → should get the message.
	cR2.Send(ID_PULL_OFFLINE_MSG_REQ, BuildPullReq(RECEIVER_UID, 0, 100));
	Frame pf;
	bool pull_ok = cR2.Wait(ID_PULL_OFFLINE_MSG_RSP, 10000, &pf);
	bool got_msg = false;
	if (pull_ok) {
		auto j = ParseJson(pf.body);
		auto ids = ExtractPullMessageIds(j);
		got_msg = std::find(ids.begin(), ids.end(), sent_mid) != ids.end();
	}
	Check(got_msg, "cross-server: receiver pulled message after restart",
		("sent_mid=" + std::to_string(sent_mid)).c_str());
	if (!got_msg) all_ok = false;

	// ACK to clean up.
	if (got_msg) {
		cR2.Send(ID_CHAT_DELIVERY_ACK_REQ, BuildAckReq(RECEIVER_UID, {sent_mid}));
		Frame af; cR2.Wait(ID_CHAT_DELIVERY_ACK_RSP, 10000, &af);
	}

	cS.Close(); cR2.Close();
	cleanup();
	pm.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// image-offline (Verification.6)
//
// 走 ResourceServer 分片上传，完成前 UN_UPLOAD 行绝不出现在 pull；完成后 pending
// ID 出现，离线 receiver 拉到 msg_type=PIC/content_size，ACK 后清理。
// ---------------------------------------------------------------------------
bool ScenarioImageOffline() {
	std::printf("\n=== scenario: image-offline ===\n");
	ProcessManager pm;
	Redis redis; Mysql mysql;
	std::string tag = RunTag();
	bool all_ok = true;

	auto cleanup = [&] { CleanupFootprint(redis, mysql); };

	if (!redis.Connect(REDIS_HOST, REDIS_PORT, REDIS_PASSWD)) {
		Fail("image-offline: connect Redis", "failed"); return false;
	}
	if (!mysql.Connect(MYSQL_HOST, MYSQL_PORT, MYSQL_USER, MYSQL_PASSWD, MYSQL_SCHEMA)) {
		Fail("image-offline: connect MySQL", "failed"); return false;
	}
	cleanup();

	if (!pm.Start({ "StatusServer", { "StatusServer", "StatusServer.exe" },
		MakeStatusIni(), STATUS_GRPC_PORT }, 15000)) {
		Fail("image-offline: start StatusServer", "ready timeout"); cleanup(); return false;
	}
	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4),
		CHAT1_TCP_PORT }, 15000)) {
		Fail("image-offline: start ChatServer", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}
	if (!pm.Start({ "ResourceServer", { "ResourceServer", "ResourceServer.exe" },
		MakeResourceIni(), RESOURCE_HTTP_PORT }, 15000)) {
		Fail("image-offline: start ResourceServer", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}

	// Login sender (receiver stays offline).
	TcpClient cS;
	if (!LoginUser(redis, cS, SENDER_UID, "tokImg-" + tag, CHAT1_TCP_PORT)) {
		Fail("image-offline: login sender", "failed"); cleanup(); pm.StopAll(); return false;
	}

	// Step 1: send 1035 image metadata to ChatServer.
	const std::string uid_str = "imtest-img-" + tag;
	const std::string img_name = "test_img_" + tag + ".png";
	const std::string md5 = "d41d8cd98f00b204e9800998ecf8427e";  // md5 of empty
	const long long file_size = 1024;
	std::string meta_body = BuildImgMetaReq(SENDER_UID, RECEIVER_UID, THREAD_ID,
		md5, img_name, "tokImg-" + tag, uid_str, file_size);
	cS.Send(ID_IMG_CHAT_MSG_REQ, meta_body);
	Frame mrs; bool got_1036 = cS.Wait(ID_IMG_CHAT_MSG_RSP, 10000, &mrs);
	int msg_id = -1;
	if (got_1036) {
		auto j = ParseJson(mrs.body);
		if (j.is_object() && j.value("error", -1) == ERR_SUCCESS)
			msg_id = j.value("message_id", -1);
	}
	Check(got_1036 && msg_id > 0, "image-offline: 1035/1036 metadata persisted",
		("msg_id=" + std::to_string(msg_id)).c_str());
	if (!(got_1036 && msg_id > 0)) { cS.Close(); cleanup(); pm.StopAll(); return false; }

	// Step 2: verify UN_UPLOAD row is NOT in pending pull / Redis ZSET.
	{
		auto rows = mysql.QueryByMessageId(msg_id);
		bool is_un_upload = !rows.empty() &&
			rows[0].status == MSG_STATUS_UN_UPLOAD &&
			rows[0].msg_type == MSG_TYPE_PIC;
		Check(is_un_upload, "image-offline: row is UN_UPLOAD/PIC before upload",
			rows.empty() ? "no row" : ("status=" + std::to_string(rows[0].status)).c_str());
		if (!is_un_upload) all_ok = false;
	}
	// Redis ZSET must NOT contain this message_id yet.
	{
		std::vector<std::string> members;
		redis.ZRange("offline_msg:" + std::to_string(RECEIVER_UID), members);
		std::string mid_str = std::to_string(msg_id);
		bool absent = std::find(members.begin(), members.end(), mid_str) == members.end();
		Check(absent, "image-offline: UN_UPLOAD msg_id absent from Redis ZSET",
			absent ? "ok" : "present before upload!");
		if (!absent) all_ok = false;
	}

	// Step 3: connect to ResourceServer and upload a single chunk (last=1).
	// ResourceServer uses [2-byte id][4-byte int len][body]; ResSendAndRecv handles that.
	std::string raw_data(file_size, 'X');
	std::string b64_data = Base64Encode(raw_data);
	std::string up_body = BuildImgUploadReq(SENDER_UID, SENDER_UID, RECEIVER_UID,
		msg_id, md5, img_name, file_size, file_size, 1, b64_data);
	ResFrame urs;
	bool got_1038 = ResSendAndRecv("127.0.0.1", RESOURCE_HTTP_PORT,
		ID_IMG_CHAT_UPLOAD_REQ, up_body, &urs);
	int up_err = -1;
	if (got_1038) {
		auto j = ParseJson(urs.body);
		up_err = j.is_object() ? j.value("error", -1) : -1;
	}
	Check(got_1038 && up_err == ERR_SUCCESS, "image-offline: upload last chunk success",
		("err=" + std::to_string(up_err)).c_str());
	if (!(got_1038 && up_err == ERR_SUCCESS)) all_ok = false;

	// Step 4: after upload, verify pending activation.
	// Give the server a brief moment to run CompleteChatImageUpload.
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	{
		auto rows = mysql.QueryByMessageId(msg_id);
		bool uploaded = !rows.empty() && rows[0].status != MSG_STATUS_UN_UPLOAD;
		Check(uploaded, "image-offline: status migrated from UN_UPLOAD after upload",
			rows.empty() ? "no row" : ("status=" + std::to_string(rows[0].status)).c_str());
		if (!uploaded) all_ok = false;
	}
	{
		std::vector<std::string> members;
		redis.ZRange("offline_msg:" + std::to_string(RECEIVER_UID), members);
		std::string mid_str = std::to_string(msg_id);
		bool present = std::find(members.begin(), members.end(), mid_str) != members.end();
		Check(present, "image-offline: msg_id in Redis ZSET after upload",
			present ? "ok" : "absent after upload!");
		if (!present) all_ok = false;
	}

	// Step 5: login receiver, pull → get PIC message with content_size.
	TcpClient cR;
	if (!LoginUser(redis, cR, RECEIVER_UID, "tokR-" + tag, CHAT1_TCP_PORT)) {
		Fail("image-offline: login receiver", "failed");
		cS.Close(); cleanup(); pm.StopAll(); return false;
	}
	cR.Send(ID_PULL_OFFLINE_MSG_REQ, BuildPullReq(RECEIVER_UID, 0, 100));
	Frame pf; bool pull_ok = cR.Wait(ID_PULL_OFFLINE_MSG_RSP, 10000, &pf);
	bool got_pic = false;
	long long pulled_size = -1;
	if (pull_ok) {
		auto j = ParseJson(pf.body);
		if (j.is_object() && j.contains("messages")) {
			for (const auto& m : j["messages"]) {
				if (m.value("message_id", 0) == msg_id) {
					got_pic = true;
					if (m.value("msg_type", -1) == MSG_TYPE_PIC) {
						// content_size is a string in the envelope.
						if (m["content_size"].is_string()) {
							pulled_size = std::stoll(m["content_size"].get<std::string>());
						} else {
							pulled_size = m.value("content_size", (long long)0);
						}
					}
					break;
				}
			}
		}
	}
	Check(got_pic && pulled_size == file_size,
		"image-offline: receiver pulled PIC msg with correct content_size",
		("got_pic=" + std::to_string(got_pic) + " size=" + std::to_string(pulled_size)).c_str());
	if (!(got_pic && pulled_size == file_size)) all_ok = false;

	// Step 6: ACK → cleanup.
	if (got_pic) {
		cR.Send(ID_CHAT_DELIVERY_ACK_REQ, BuildAckReq(RECEIVER_UID, {msg_id}));
		Frame af; cR.Wait(ID_CHAT_DELIVERY_ACK_RSP, 10000, &af);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		auto rows = mysql.QueryByMessageId(msg_id);
		bool acked = !rows.empty() && rows[0].delivery_status == 1;
		Check(acked, "image-offline: ACK cleans up (delivery_status=1)",
			rows.empty() ? "no row" : ("ds=" + std::to_string(rows[0].delivery_status)).c_str());
		if (!acked) all_ok = false;
	}

	cS.Close(); cR.Close();
	cleanup();
	pm.StopAll();
	return all_ok;
}

} // namespace imt
