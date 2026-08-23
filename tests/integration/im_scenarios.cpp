// im_scenarios.cpp — implementation of the IM integration scenarios.
//
// Deterministic assertions (plan Verification.6):
//   gate-smoke: 32 concurrent /get_test + /user_login all yield complete HTTP
//               responses; the server accepts a brand-new connection while at
//               least one handler is still in flight (overlap proof).
//   order-n4:   LogicWorkers=4; uid A=1002 and B=1019 each send 1000 texts
//               concurrently. Per uid, 1302 arrival order == submission order and
//               canonical message_id strictly increases; the two uids' global
//               receipt-sequence ranges overlap (different shards, true parallel).
//   order-n1:   LogicWorkers=1; same 1000-each send, order preserved; no overlap
//               assertion (single shard serializes, overlap is not guaranteed).
//   dedup:      pipelined re-send of an identical (sender_id,unique_id) yields the
//               same message_id with exactly one DB row; a same-key different-
//               content send returns MESSAGE_CONFLICT and leaves the original row.
//   offline:    离线期间产生的消息，重连后经 1405/1406 增量同步按 sync_seq 升序补齐。
//   lost-ack:   1408 丢失后重发同一 1407，服务端幂等成功（重复 ACK 不产生错误）。
//   pull-bytes: 多页同步：超过一页的消息量逐页 after_sync_seq 推进，无遗漏无重复、
//               sync_seq 严格递增、has_more 正确。
//   sync-bootstrap: {bootstrap:true} 返回 checkpoint；checkpoint 之后的消息全部
//               同步到、之前的不推。
//   big-ids:    AUTO_INCREMENT 调到 2^32 以上后，>32 位 message_id 以十进制字符串
//               在 1302/1406 全链路无损、同步游标正常推进（结束恢复原值）。
#include "im_scenarios.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <ctime>

#include "im_common.h"
#include "im_frame.h"
#include "im_harness.h"
#include "im_http_client.h"
#include "im_mysql.h"
#include "im_redis.h"
#include "im_status_client.h"
#include "im_tcp_client.h"

// 资源消息分片/整文件 SHA-256 校验（生产实现，见 tests/CMakeLists.txt）
#include "Sha256.h"

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
// user_message_sync 行需先按 unique_id JOIN 删除，再删 chat_message 本体。
//
// The per-user login token lives in utoken_<uid> (written by Status on every
// password login, TTL 86400); clear it so each scenario starts clean.
static void CleanupFootprint(Redis& redis, Mysql& mysql) {
	mysql.DeleteSyncRowsByUniqueIdLike("imtest-%");
	mysql.DeleteByUniqueIdLike("imtest-%");
	for (int uid : { SENDER_UID, RECEIVER_UID }) {
		const std::string u = std::to_string(uid);
		redis.Del(UserTokenKey(uid));
		redis.Del("uip_"      + u);
		redis.Del("usession_" + u);
		redis.Del("ubaseinfo_" + u);
		// 登录/异常清理用的分布式锁（DistLock 实际 key 为 "lock:"+"lock_<uid>"）。
		// 被强杀的进程可能持锁死亡，残留 TTL<=10s，不清掉会卡下一场景的登录。
		redis.Del("lock:lock_" + u);
	}
	// 清除可能残留的 ChatServer lease（上个场景被 TerminateProcess 的节点会留下
	// TTL<=8s 的 lease，导致下一场景的 Status 选中未启动的节点）。
	redis.Del(ChatLeaseKey("chatserver1"));
	redis.Del(ChatLeaseKey("chatserver2"));
	redis.Del(ChatLeaseKey("chatserver3"));
	redis.HDel(CHAT_REGISTRY_KEY, "chatserver1");
	redis.HDel(CHAT_REGISTRY_KEY, "chatserver2");
	redis.HDel(CHAT_REGISTRY_KEY, "chatserver3");
}

// Result of a Gate /user_login call: the per-user login token plus the address
// of the ChatServer Status selected for this login.
struct GateLoginInfo {
	bool           ok = false;
	int            error = -1;
	std::string    token;
	std::string    server_name;
	std::string    chat_host;
	unsigned short chat_port = 0;
};

// POST Gate /user_login {email, passwd} for a fixture uid. Verifies the
// password (Gate checks the DB directly) and obtains the login token plus the
// assigned ChatServer address. Does not touch the ChatServer.
static GateLoginInfo GateLogin(int uid) {
	GateLoginInfo info;
	json req;
	req["email"]  = FixtureEmailForUid(uid);
	req["passwd"] = FIXTURE_PASSWD;
	HttpResponse r = HttpPost("127.0.0.1", GATE_HTTP_PORT, "/user_login",
	                          req.dump(), "text/json", 15000);
	if (r.status != 200) {
		std::printf("[login] /user_login uid=%d http status=%d err=%s\n",
			uid, r.status, r.error.c_str());
		return info;
	}
	auto j = ParseJson(r.body);
	if (!j.is_object()) {
		std::printf("[login] /user_login uid=%d non-json body\n", uid);
		return info;
	}
	info.error = j.value("error", -1);
	if (info.error != ERR_SUCCESS) {
		std::printf("[login] /user_login uid=%d error=%d\n", uid, info.error);
		return info;
	}
	info.token     = j.value("token", "");
	info.server_name = j.value("server_name", "");
	info.chat_host = j.value("chathost", "");
	const std::string port_str = j.value("chatport", "0");
	info.chat_port = static_cast<unsigned short>(std::atoi(port_str.c_str()));
	if (info.token.empty() || info.server_name.empty() ||
	    info.chat_host.empty() || info.chat_port == 0) {
		std::printf("[login] /user_login uid=%d missing chat fields\n", uid);
		return info;
	}
	info.ok = true;
	return info;
}

// Delivery/topology scenarios do not test passwords. Ask Status for the same
// utoken/endpoint that Gate requests after password verification so these
// scenarios are independent of fixture password migration state.
static GateLoginInfo StatusLogin(int uid) {
	GateLoginInfo info;
	StatusClient status;
	std::string port_text;
	if (!status.Connect("127.0.0.1", STATUS_GRPC_PORT) ||
		!status.GetChatServer(uid, info.error, info.server_name,
			info.chat_host, port_text, info.token)) {
		return info;
	}
	info.chat_port = static_cast<unsigned short>(std::atoi(port_text.c_str()));
	info.ok = info.error == ERR_SUCCESS && !info.token.empty() &&
		!info.server_name.empty() && !info.chat_host.empty() && info.chat_port != 0;
	return info;
}

// Full Chat login for a fixture uid using a Status-issued token. Password
// behavior remains covered separately by ScenarioSimpleAuth through GateLogin.
struct LoginInfo {
	bool           ok = false;
	std::string    token;
	std::string    chat_host;
	unsigned short chat_port = 0;
};

static LoginInfo LoginUser(TcpClient& c, int uid) {
	LoginInfo info;
	GateLoginInfo gl = StatusLogin(uid);
	if (!gl.ok) return info;
	if (!c.Connect(gl.chat_host, gl.chat_port, 10000)) {
		std::printf("[login] connect chat uid=%d %s:%u failed\n",
			uid, gl.chat_host.c_str(), gl.chat_port);
		return info;
	}
	auto lo = ChatLogin(c, uid, gl.token);
	if (!lo.ok) {
		std::printf("[login] chat login uid=%d error=%d\n", uid, lo.error);
		return info;
	}
	info.ok        = true;
	info.token     = gl.token;
	info.chat_host = gl.chat_host;
	info.chat_port = gl.chat_port;
	return info;
}

// 钉选登录服务器：Status 总是选最小负载 lease 的节点。把另一台的 lease 充到
// 999（TTL 8s）后立即 /user_login，Status 即选中 want_port 对应节点。对方节点
// 的 lease 上报器会在 <=2s 内覆盖我们的种子值，故用重试循环直到 Gate 返回想要
// 的端口（重试会踢掉上一次偶然落到别处的会话，幂等）。
static bool LoginUserPinned(TcpClient& c, int uid, Redis& redis,
                            const std::string& want_name, unsigned short want_port) {
	const std::string other = (want_name == "chatserver1") ? "chatserver2" : "chatserver1";
	for (int attempt = 0; attempt < 15; ++attempt) {
		redis.SetEx(ChatLeaseKey(other), 8, "999");
		GateLoginInfo gl = StatusLogin(uid);
		if (gl.ok && gl.chat_port == want_port) {
			if (!c.Connect(gl.chat_host, gl.chat_port, 10000)) {
				std::printf("[login] pinned connect uid=%d %s:%u failed\n",
					uid, gl.chat_host.c_str(), gl.chat_port);
				return false;
			}
			auto lo = ChatLogin(c, uid, gl.token);
			if (lo.ok) return true;
			// 登录失败（如残留分布式锁 RPCFailed）：关闭后重试，残余锁 TTL 10s 内会过期
			std::printf("[login] pinned chat login uid=%d error=%d, retrying\n", uid, lo.error);
		}
		c.Close();
		std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	}
	std::printf("[login] pinned login uid=%d never landed on %s\n", uid, want_name.c_str());
	return false;
}

// Build a 1301 body carrying a single text message (单条化：content/unique_id 顶层平铺;
// 协议字符串化：thread_id 为十进制字符串).
static std::string BuildTextReq(int fromuid, int touid, std::int64_t thread_id,
                                const std::string& content, const std::string& unique_id) {
	json j;
	j["fromuid"] = fromuid;
	j["touid"]   = touid;
	j["thread_id"] = ToIdStr(thread_id);
	j["content"]   = content;
	j["unique_id"] = unique_id;
	return j.dump();
}

// json 对象中按十进制字符串读取 id 字段（message_id/thread_id/sync_seq/
// next_sync_seq/checkpoint）；缺省或非字符串返回 dfl。
static std::int64_t JsonIdStr(const json& j, const char* key, std::int64_t dfl = 0) {
	if (!j.is_object() || !j.contains(key) || !j[key].is_string()) return dfl;
	return ParseIdStr(j[key].get<std::string>(), dfl);
}

// Build a 1407 delivery-ACK body: {"uid":<receiver>,"message_ids":["<id>",...]}
// （协议字符串化：message_ids 数组元素为十进制字符串）
static std::string BuildAckReq(int uid, const std::vector<std::int64_t>& ids) {
	json j;
	j["uid"] = uid;
	json arr = json::array();
	for (std::int64_t id : ids) arr.push_back(ToIdStr(id));
	j["message_ids"] = arr;
	return j.dump();
}

// Build a 1405 incremental-sync body:
//   {"uid":<uid>,"after_sync_seq":"<seq十进制字符串>","limit":<n>}
static std::string BuildSyncReq(int uid, std::uint64_t after_sync_seq, int limit) {
	json j;
	j["uid"] = uid;
	j["after_sync_seq"] = std::to_string(after_sync_seq);
	j["limit"] = limit;
	return j.dump();
}

// Build a 1405 bootstrap body: {"uid":<uid>,"bootstrap":true}
static std::string BuildSyncBootstrapReq(int uid) {
	json j;
	j["uid"] = uid;
	j["bootstrap"] = true;
	return j.dump();
}

// 1406 增量同步响应的单条 envelope（共享约定：message_id/thread_id/sync_seq 为
// 十进制字符串；fromuid/touid/msg_type/status 为数字；content_size/chat_time 维持字符串）。
struct SyncEnvelope {
	std::int64_t  message_id = 0;
	std::int64_t  thread_id  = 0;
	std::uint64_t sync_seq   = 0;
	int fromuid = 0;
	int touid   = 0;
	int msg_type = -1;
	int status   = -1;
	std::string unique_id;
	std::string content;
	std::string md5;
	std::string content_size;
	std::string chat_time;
};

// Parse the messages array of a 1406 response into envelopes (order preserved).
static std::vector<SyncEnvelope> ExtractSyncMessages(const json& j) {
	std::vector<SyncEnvelope> out;
	if (!j.is_object() || !j.contains("messages")) return out;
	const auto& msgs = j["messages"];
	if (!msgs.is_array()) return out;
	for (const auto& m : msgs) {
		SyncEnvelope e;
		e.message_id = JsonIdStr(m, "message_id", 0);
		e.thread_id  = JsonIdStr(m, "thread_id", 0);
		e.sync_seq   = static_cast<std::uint64_t>(JsonIdStr(m, "sync_seq", 0));
		e.fromuid  = m.value("fromuid", 0);
		e.touid    = m.value("touid", 0);
		e.msg_type = m.value("msg_type", -1);
		e.status   = m.value("status", -1);
		e.unique_id = m.value("unique_id", "");
		e.content   = m.value("content", "");
		e.md5       = m.value("md5", "");
		e.content_size = m.value("content_size", "");
		e.chat_time    = m.value("chat_time", "");
		out.push_back(std::move(e));
	}
	return out;
}

// 发送 1405 增量同步请求并等待 1406；成功时填充 messages/next_sync_seq/has_more。
static bool DoSyncPage(TcpClient& c, int uid, std::uint64_t after, int limit,
                       std::vector<SyncEnvelope>& msgs, std::uint64_t& next_seq,
                       bool& has_more) {
	if (!c.Send(ID_SYNC_MESSAGE_REQ, BuildSyncReq(uid, after, limit))) return false;
	Frame f;
	if (!c.Wait(ID_SYNC_MESSAGE_RSP, 10000, &f)) return false;
	auto j = ParseJson(f.body);
	if (!j.is_object() || j.value("error", -1) != ERR_SUCCESS) return false;
	msgs = ExtractSyncMessages(j);
	next_seq = static_cast<std::uint64_t>(JsonIdStr(j, "next_sync_seq", (std::int64_t)after));
	has_more = j.value("has_more", false);
	return true;
}

// 发送 1405 bootstrap 请求并等待 1406；成功时填充 checkpoint（max_seq 十进制串解析）。
static bool DoSyncBootstrap(TcpClient& c, int uid, std::uint64_t& checkpoint) {
	if (!c.Send(ID_SYNC_MESSAGE_REQ, BuildSyncBootstrapReq(uid))) return false;
	Frame f;
	if (!c.Wait(ID_SYNC_MESSAGE_RSP, 10000, &f)) return false;
	auto j = ParseJson(f.body);
	if (!j.is_object() || j.value("error", -1) != ERR_SUCCESS) return false;
	checkpoint = static_cast<std::uint64_t>(JsonIdStr(j, "checkpoint", 0));
	return true;
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
	std::vector<std::int64_t> message_ids;  // canonical id by submission index
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
		if (f.type != ID_TEXT_CHAT_MSG_RSP) continue;  // drain 1303 etc.
		auto j = ParseJson(f.body);
		if (!j.is_object() || j.value("error", -1) != ERR_SUCCESS) { s.ok = false; return; }
		//单条化：1302 成功响应为顶层拍平 envelope（message_id 为十进制字符串）
		const std::string uid = j.value("unique_id", "");
		const std::int64_t mid = JsonIdStr(j, "message_id", 0);
		if (uid.empty() || mid <= 0) { s.ok = false; return; }
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
	if (!pm.Start({ "GateServer", { "GateServer", "GateServer.exe" },
		MakeGateIni(), GATE_HTTP_PORT }, 15000)) {
		Fail(label + ": start GateServer", "ready timeout"); cleanup(); return false;
	}
	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, logic_workers),
		CHAT1_TCP_PORT }, 15000)) {
		Fail(label + ": start ChatServer", "ready timeout"); cleanup(); return false;
	}

	TcpClient cA, cB;
	auto la = LoginUser(cA, SENDER_UID);
	auto lb = LoginUser(cB, RECEIVER_UID);
	if (!la.ok || !lb.ok) {
		Fail(label + ": login fixture uids", "gate/chat login failed"); cleanup(); return false;
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
		      label + ": both uids received 1000x 1302",
		      ("A collected=" + std::to_string(sA.collected.load()) +
	       " B collected=" + std::to_string(sB.collected.load()) +
	       " okA=" + std::to_string(sA.ok.load()) + " okB=" + std::to_string(sB.ok.load())).c_str());
	if (!(sA.ok.load() && sB.ok.load() &&
	      sA.collected.load() == PER && sB.collected.load() == PER)) all_ok = false;

	// (2) Per-uid: 1302 arrival order == submission order (same shard FIFO) and
	//     canonical message_id strictly increases.
	auto check_side = [&](OrderSide& s, const std::string& who) -> bool {
		long long prev_seq = -1; std::int64_t prev_mid = -1; bool ordered = true, increasing = true;
		for (int i = 0; i < s.total; ++i) {
			if (!s.received[i]) { ordered = false; break; }
			if (s.seqs[i] <= prev_seq) { ordered = false; }
			prev_seq = s.seqs[i];
			if (s.message_ids[i] <= prev_mid) { increasing = false; }
			prev_mid = s.message_ids[i];
		}
		Check(ordered, label + ": " + who + " 1302 order preserved",
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
		const long long sync_rows = mysql.CountSyncRowsByUniqueIdLike("imtest-%");
		std::printf("[probe] chat_message rows with unique_id LIKE 'imtest-%%': %lld\n", rows);
		std::printf("[probe] user_message_sync rows tied to 'imtest-%%': %lld\n", sync_rows);
		if (rows != 0 || sync_rows != 0) clean = false;
	}
	if (redis.connected()) {
		bool t1=false, t2=false, u1=false, u2=false;
		redis.Exists(UserTokenKey(SENDER_UID), t1);
		redis.Exists(UserTokenKey(RECEIVER_UID), t2);
		redis.Exists("uip_" + std::to_string(SENDER_UID), u1);
		redis.Exists("uip_" + std::to_string(RECEIVER_UID), u2);
		std::printf("[probe] utoken A=%d B=%d  uip A=%d B=%d\n",
			(int)t1, (int)t2, (int)u1, (int)u2);
		if (t1 || t2 || u1 || u2) clean = false;
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
	if (!pm.Start({ "GateServer", { "GateServer", "GateServer.exe" },
		MakeGateIni(), GATE_HTTP_PORT }, 15000)) {
		Fail("dedup: start GateServer", "ready timeout"); cleanup(); return false;
	}
	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4),
		CHAT1_TCP_PORT }, 15000)) {
		Fail("dedup: start ChatServer", "ready timeout"); cleanup(); return false;
	}

	TcpClient c;
	auto ld = LoginUser(c, SENDER_UID);
	if (!ld.ok) {
		Fail("dedup: login sender", "gate/chat login failed"); cleanup(); return false;
	}

	// --- Assertion A: identical (sender_id, unique_id) sent twice (pipelined) ->8740	//     two 1302 with the SAME message_id, exactly one DB row.
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
		std::int64_t mid1 = -1, mid2 = -1; int err1 = -1, err2 = -1;
		if (g1) { auto j = ParseJson(r1.body); err1 = j.value("error", -1);
			if (err1 == 0) mid1 = JsonIdStr(j, "message_id", -1); }
		if (g2) { auto j = ParseJson(r2.body); err2 = j.value("error", -1);
			if (err2 == 0) mid2 = JsonIdStr(j, "message_id", -1); }

		Check(s1 && s2 && g1 && g2 && err1 == 0 && err2 == 0,
			"dedup: both re-sends get 1302 success",
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
		std::int64_t mid_orig = -1; int err_orig = -1;
		if (go) { auto j = ParseJson(ro.body); err_orig = j.value("error", -1);
			if (err_orig == 0) mid_orig = JsonIdStr(j, "message_id", -1); }
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
		std::int64_t after_mid = -1;
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
// bytes / cross-server / resource-offline)
// ---------------------------------------------------------------------------

// Build a 1503 create-resource-message body (图片/文件统一协议)。
// thread_id/content_size/message_id 按协议字符串化；msg_type 1=图片 3=文件。
static std::string BuildResourceCreateReq(int fromuid, int touid, std::int64_t thread_id,
	                                  const std::string& unique_id, int msg_type,
	                                  const std::string& file_name, long long content_size,
	                                  const std::string& content_hash,
	                                  const std::string& mime_type) {
	json j;
	j["fromuid"] = fromuid;
	j["touid"]   = touid;
	j["thread_id"] = ToIdStr(thread_id);
	j["unique_id"] = unique_id;
	j["msg_type"]  = msg_type;
	j["file_name"] = file_name;
	j["content_size"] = std::to_string(content_size);
	j["content_hash"] = content_hash;
	j["mime_type"] = mime_type;
	return j.dump();
}

// Build a 1507 resource-chunk-upload body: {message_id, offset, chunk_sha256, data}
// （message_id/offset 十进制字符串；data 为 <=32KiB 分片的 Base64）
static std::string BuildChunkUploadReq(std::int64_t message_id, long long offset,
	                               const std::string& chunk_sha256,
	                               const std::string& data_b64) {
	json j;
	j["message_id"] = ToIdStr(message_id);
	j["offset"] = std::to_string(offset);
	j["chunk_sha256"] = chunk_sha256;
	j["data"] = data_b64;
	return j.dump();
}

// Build a 1509 upload-progress query body: {message_id}
static std::string BuildUploadProgressReq(std::int64_t message_id) {
	json j;
	j["message_id"] = ToIdStr(message_id);
	return j.dump();
}

// Build a 1511 download-info query body: {message_id}
static std::string BuildDownInfoReq(std::int64_t message_id) {
	json j;
	j["message_id"] = ToIdStr(message_id);
	return j.dump();
}

// Build a 1513 chunk-download body: {message_id, offset}
static std::string BuildChunkDownReq(std::int64_t message_id, long long offset) {
	json j;
	j["message_id"] = ToIdStr(message_id);
	j["offset"] = std::to_string(offset);
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

// Minimal base64 decoder（下载校验用；忽略填充）。
static std::string Base64Decode(const std::string& in) {
	auto val_of = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '+') return 62;
		if (c == '/') return 63;
		return -1;
	};
	std::string out;
	int val = 0, valb = -8;
	for (char c : in) {
		const int d = val_of(c);
		if (d < 0) break;
		val = (val << 6) + d;
		valb += 6;
		if (valb >= 0) {
			out.push_back(static_cast<char>((val >> valb) & 0xFF));
			valb -= 8;
		}
	}
	return out;
}

// 生成确定性测试载荷（n 字节），逐字节可与下载结果比对
static std::string MakeBlob(long long n) {
	std::string s;
	s.reserve(static_cast<std::size_t>(n));
	for (long long i = 0; i < n; ++i) {
		s.push_back(static_cast<char>((i * 31 + (i >> 8)) & 0xFF));
	}
	return s;
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

// ---------------------------------------------------------------------------
// offline (Verification.6)
//
// receiver 离线期间 sender 发 100 条（MySQL 全 delivery_status=0、
// user_message_sync 双方各写一行）；receiver 重连后经 1405/1406 增量同步按
// sync_seq 升序补齐，每 ID 只出现一次；1407/1408 完成后 DB 全 1。
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
	if (!pm.Start({ "GateServer", { "GateServer", "GateServer.exe" },
		MakeGateIni(), GATE_HTTP_PORT }, 15000)) {
		Fail("offline: start GateServer", "ready timeout"); cleanup(); return false;
	}
	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4),
		CHAT1_TCP_PORT }, 15000)) {
		Fail("offline: start ChatServer", "ready timeout"); cleanup(); return false;
	}

	// Step 0: receiver 短暂登录取 bootstrap checkpoint 作为同步起点，随后离线。
	std::uint64_t checkpoint = 0;
	{
		TcpClient cR0;
		if (!LoginUser(cR0, RECEIVER_UID).ok) {
			Fail("offline: login receiver (checkpoint)", "gate/chat login failed");
			cleanup(); pm.StopAll(); return false;
		}
		if (!DoSyncBootstrap(cR0, RECEIVER_UID, checkpoint)) {
			Fail("offline: bootstrap checkpoint", "1405/1406 bootstrap failed");
			cR0.Close(); cleanup(); pm.StopAll(); return false;
		}
		cR0.Close();
	}

	// Login sender only — receiver stays offline.
	TcpClient cS;
	if (!LoginUser(cS, SENDER_UID).ok) {
		Fail("offline: login sender", "gate/chat login failed"); cleanup(); pm.StopAll(); return false;
	}

	// Send MSG_COUNT messages while receiver is offline.
	std::vector<std::int64_t> sent_mids;
	sent_mids.reserve(MSG_COUNT);
	bool send_ok = true;
	for (int i = 0; i < MSG_COUNT; ++i) {
		char buf[64]; std::snprintf(buf, sizeof(buf), "%03d", i);
		const std::string uid_str = "imtest-offline-" + tag + "-" + buf;
		const std::string body = BuildTextReq(SENDER_UID, RECEIVER_UID, THREAD_ID,
			"offline-" + std::to_string(i), uid_str);
		if (!cS.Send(ID_TEXT_CHAT_MSG_REQ, body)) { send_ok = false; break; }
	}
	// Collect all 1302 responses (message_id 为十进制字符串).
	for (int i = 0; i < MSG_COUNT && send_ok; ++i) {
		Frame f;
		if (!cS.Wait(ID_TEXT_CHAT_MSG_RSP, 15000, &f)) { send_ok = false; break; }
		auto j = ParseJson(f.body);
		if (!j.is_object() || j.value("error", -1) != ERR_SUCCESS) { send_ok = false; break; }
		sent_mids.push_back(JsonIdStr(j, "message_id", 0));
	}
	Check(send_ok && (int)sent_mids.size() == MSG_COUNT,
		"offline: sender received 100x 1302",
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

	// (2) user_message_sync: checkpoint 之后 receiver 恰好多出 100 行。
	{
		const auto rows = mysql.QuerySyncRows(RECEIVER_UID, checkpoint, 0);
		Check((int)rows.size() == MSG_COUNT,
			"offline: receiver has 100 new sync rows after checkpoint",
			("rows=" + std::to_string(rows.size())).c_str());
		if ((int)rows.size() != MSG_COUNT) all_ok = false;
	}

	// Login receiver and sync all pending via 1405/1406.
	TcpClient cR;
	if (!LoginUser(cR, RECEIVER_UID).ok) {
		Fail("offline: login receiver", "gate/chat login failed"); cS.Close(); cleanup(); pm.StopAll(); return false;
	}

	// 分页同步：after_sync_seq=checkpoint，limit=30 强制多页，沿 next_sync_seq 推进。
	std::vector<std::int64_t> pulled_ids;
	std::uint64_t cursor = checkpoint;
	bool pull_ok = true;
	bool seq_increasing = true;
	bool cursor_consistent = true;
	std::uint64_t prev_seq = checkpoint;
	int pages = 0;
	while (true) {
		++pages;
		std::vector<SyncEnvelope> msgs;
		std::uint64_t next_seq = cursor;
		bool has_more = false;
		if (!DoSyncPage(cR, RECEIVER_UID, cursor, 30, msgs, next_seq, has_more)) {
			pull_ok = false; break;
		}
		for (const auto& m : msgs) {
			if (m.sync_seq <= prev_seq && !pulled_ids.empty()) seq_increasing = false;
			prev_seq = m.sync_seq;
			pulled_ids.push_back(m.message_id);
		}
		// 非空页 next_sync_seq 应等于本页最后一条的 sync_seq。
		if (!msgs.empty() && next_seq != msgs.back().sync_seq) cursor_consistent = false;
		// 空页 next_sync_seq 应等于 after_sync_seq 且 has_more=false。
		if (msgs.empty() && (next_seq != cursor || has_more)) cursor_consistent = false;
		cursor = next_seq;
		if (!has_more) break;
		if (pages > 20) { pull_ok = false; break; }  // safety
	}

	// (3) Every sent ID appears exactly once in the sync stream, in sync_seq order.
	std::set<std::int64_t> sent_set(sent_mids.begin(), sent_mids.end());
	std::set<std::int64_t> pulled_set(pulled_ids.begin(), pulled_ids.end());
	bool no_dup = ((int)pulled_ids.size() == (int)pulled_set.size());
	bool all_present = (sent_set == pulled_set);
	Check(pull_ok && no_dup && all_present && (int)pulled_set.size() == MSG_COUNT,
		"offline: paged sync returns all 100 IDs exactly once",
		("pulled=" + std::to_string(pulled_ids.size()) +
		 " unique=" + std::to_string(pulled_set.size()) +
		 " dup=" + std::to_string(!no_dup)).c_str());
	if (!(pull_ok && no_dup && all_present)) all_ok = false;

	Check(seq_increasing && cursor_consistent && pages >= 2,
		"offline: sync_seq strictly increasing, next_sync_seq consistent",
		("pages=" + std::to_string(pages) +
		 " increasing=" + std::to_string(seq_increasing) +
		 " consistent=" + std::to_string(cursor_consistent)).c_str());
	if (!(seq_increasing && cursor_consistent)) all_ok = false;

	// (4) ACK all via 1407 → 1408（message_ids 为字符串数组）.
	bool ack_ok = false;
	{
		std::vector<std::int64_t> ids_vec(pulled_set.begin(), pulled_set.end());
		std::string body = BuildAckReq(RECEIVER_UID, ids_vec);
		if (cR.Send(ID_CHAT_DELIVERY_ACK_REQ, body)) {
			Frame f;
			if (cR.Wait(ID_CHAT_DELIVERY_ACK_RSP, 10000, &f)) {
				auto j = ParseJson(f.body);
				ack_ok = j.is_object() && j.value("error", -1) == ERR_SUCCESS;
			}
		}
	}
	Check(ack_ok, "offline: ACK 1407/1408 success", "ack failed");
	if (!ack_ok) all_ok = false;

	// (5) After ACK: MySQL all delivery_status=1（短暂轮询等服务端落库）。
	for (int poll = 0; poll < 50; ++poll) {
		const std::string pattern = "imtest-offline-" + tag + "-%";
		if (mysql.CountByUniqueIdLikeAndDelivery(pattern, 0) == 0) break;
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

	cS.Close(); cR.Close();
	cleanup();
	pm.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// lost-ack (Verification.6)
//
// 第一次 1407 的 1408 回包假设丢失：客户端重发同一 1407，服务端幂等成功
// （重复 ACK 不产生错误），DB delivery_status 保持 1 且无重复记录。
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
	if (!pm.Start({ "GateServer", { "GateServer", "GateServer.exe" },
		MakeGateIni(), GATE_HTTP_PORT }, 15000)) {
		Fail("lost-ack: start GateServer", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}
	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4),
		CHAT1_TCP_PORT }, 15000)) {
		Fail("lost-ack: start ChatServer", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}

	// Send one message to offline receiver（checkpoint 需在发送前捕获）。
	const std::uint64_t checkpoint = mysql.MaxSyncSeq(RECEIVER_UID);
	TcpClient cS;
	if (!LoginUser(cS, SENDER_UID).ok) {
		Fail("lost-ack: login sender", "gate/chat login failed"); cleanup(); pm.StopAll(); return false;
	}
	const std::string uid_str = "imtest-lostack-" + tag;
	std::string body = BuildTextReq(SENDER_UID, RECEIVER_UID, THREAD_ID,
		"lost-ack-msg", uid_str);
	cS.Send(ID_TEXT_CHAT_MSG_REQ, body);
	Frame rsp; bool got_rsp = cS.Wait(ID_TEXT_CHAT_MSG_RSP, 10000, &rsp);
	std::int64_t sent_mid = -1;
	if (got_rsp) {
		auto j = ParseJson(rsp.body);
		if (j.is_object() && j.value("error", -1) == ERR_SUCCESS)
			sent_mid = JsonIdStr(j, "message_id", -1);
	}
	Check(got_rsp && sent_mid > 0, "lost-ack: message sent and persisted",
		("mid=" + std::to_string(sent_mid)).c_str());
	if (!(got_rsp && sent_mid > 0)) { cS.Close(); cleanup(); pm.StopAll(); return false; }

	// Login receiver, sync from checkpoint → get message_id.
	TcpClient cR;
	if (!LoginUser(cR, RECEIVER_UID).ok) {
		Fail("lost-ack: login receiver", "gate/chat login failed"); cS.Close(); cleanup(); pm.StopAll(); return false;
	}
	{
		std::vector<SyncEnvelope> msgs;
		std::uint64_t next_seq = checkpoint;
		bool has_more = false;
		bool synced = DoSyncPage(cR, RECEIVER_UID, checkpoint, 100, msgs, next_seq, has_more);
		bool found = false;
		for (const auto& m : msgs) if (m.message_id == sent_mid) found = true;
		Check(synced && found, "lost-ack: sync returns the message",
			("synced=" + std::to_string(synced) + " msgs=" + std::to_string(msgs.size())).c_str());
		if (!(synced && found)) all_ok = false;
	}

	// First ACK round: 1407 → 1408 success, DB delivery_status=1.
	auto do_ack = [&]() -> bool {
		if (!cR.Send(ID_CHAT_DELIVERY_ACK_REQ, BuildAckReq(RECEIVER_UID, {sent_mid}))) return false;
		Frame af;
		if (!cR.Wait(ID_CHAT_DELIVERY_ACK_RSP, 10000, &af)) return false;
		auto j = ParseJson(af.body);
		return j.is_object() && j.value("error", -1) == ERR_SUCCESS;
	};
	bool ack1 = do_ack();
	Check(ack1, "lost-ack: first ACK succeeds", "ack failed");
	if (!ack1) all_ok = false;
	for (int poll = 0; poll < 50; ++poll) {
		if (mysql.CountByUniqueIdLikeAndDelivery(uid_str, 0) == 0) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	{
		const long long acked = mysql.CountByUniqueIdLikeAndDelivery(uid_str, 1);
		Check(acked == 1, "lost-ack: post-ACK delivery_status=1",
			("acked=" + std::to_string(acked)).c_str());
		if (acked != 1) all_ok = false;
	}

	// 模拟 1408 丢失：重发同一 1407，服务端幂等成功（重复 ACK 不产生错误）。
	bool ack2 = do_ack();
	Check(ack2, "lost-ack: duplicate ACK idempotent success", "duplicate ack failed");
	if (!ack2) all_ok = false;

	// 重复 ACK 无副作用：仍单行、delivery_status 保持 1。
	{
		const long long rows = mysql.CountByUniqueId(uid_str);
		const long long acked = mysql.CountByUniqueIdLikeAndDelivery(uid_str, 1);
		Check(rows == 1 && acked == 1,
			"lost-ack: duplicate ACK leaves single acked row",
			("rows=" + std::to_string(rows) + " acked=" + std::to_string(acked)).c_str());
		if (!(rows == 1 && acked == 1)) all_ok = false;
	}

	cS.Close(); cR.Close();
	cleanup();
	pm.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// pull-bytes (Verification.6)
//
// 多页同步：制造超过一页的消息量（250 条，limit=100 → 3 页），逐页
// after_sync_seq 推进；断言无遗漏无重复、sync_seq 严格递增、has_more 正确。
// ---------------------------------------------------------------------------
bool ScenarioPullBytes() {
	std::printf("\n=== scenario: pull-bytes ===\n");
	ProcessManager pm;
	Redis redis; Mysql mysql;
	std::string tag = RunTag();
	bool all_ok = true;
	const int MSG_COUNT = 250;  // limit=100 → 3 页（100/100/50）

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
	if (!pm.Start({ "GateServer", { "GateServer", "GateServer.exe" },
		MakeGateIni(), GATE_HTTP_PORT }, 15000)) {
		Fail("pull-bytes: start GateServer", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}
	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4),
		CHAT1_TCP_PORT }, 15000)) {
		Fail("pull-bytes: start ChatServer", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}

	TcpClient cS;
	if (!LoginUser(cS, SENDER_UID).ok) {
		Fail("pull-bytes: login sender", "gate/chat login failed"); cleanup(); pm.StopAll(); return false;
	}

	// 同步起点：发送前 receiver 的最大 sync_seq。
	const std::uint64_t checkpoint = mysql.MaxSyncSeq(RECEIVER_UID);

	// Send MSG_COUNT messages to offline receiver.
	const std::string pattern = "imtest-pullbytes-" + tag + "-%";
	for (int i = 0; i < MSG_COUNT; ++i) {
		char buf[64]; std::snprintf(buf, sizeof(buf), "%03d", i);
		const std::string uid_str = "imtest-pullbytes-" + tag + "-" + buf;
		std::string body = BuildTextReq(SENDER_UID, RECEIVER_UID, THREAD_ID,
			"pullbytes-" + std::to_string(i), uid_str);
		cS.Send(ID_TEXT_CHAT_MSG_REQ, body);
	}
	// Drain 1302 responses.
	int got_1302 = 0;
	for (int i = 0; i < MSG_COUNT; ++i) {
		Frame f;
		if (!cS.Wait(ID_TEXT_CHAT_MSG_RSP, 15000, &f)) break;
		auto j = ParseJson(f.body);
		if (j.is_object() && j.value("error", -1) == ERR_SUCCESS) ++got_1302;
	}
	Check(got_1302 == MSG_COUNT, "pull-bytes: sender received all 1302",
		("got=" + std::to_string(got_1302)).c_str());
	if (got_1302 != MSG_COUNT) all_ok = false;

	// Login receiver, sync page by page (limit=100 强制 3 页).
	TcpClient cR;
	if (!LoginUser(cR, RECEIVER_UID).ok) {
		Fail("pull-bytes: login receiver", "gate/chat login failed"); cS.Close(); cleanup(); pm.StopAll(); return false;
	}

	std::vector<std::int64_t> all_pulled;
	std::uint64_t cursor = checkpoint;
	bool pull_ok = true;
	bool seq_increasing = true;
	bool has_more_ok = true;
	std::uint64_t prev_seq = checkpoint;
	int pages = 0;
	while (true) {
		++pages;
		std::vector<SyncEnvelope> msgs;
		std::uint64_t next_seq = cursor;
		bool has_more = false;
		if (!DoSyncPage(cR, RECEIVER_UID, cursor, 100, msgs, next_seq, has_more)) {
			pull_ok = false; break;
		}
		// has_more 正确性：本场景恰 3 页（100/100/50），仅最后一页为 false。
		const bool expect_more = ((int)all_pulled.size() + (int)msgs.size()) < MSG_COUNT;
		if (has_more != expect_more) has_more_ok = false;
		for (const auto& m : msgs) {
			if (m.sync_seq <= prev_seq) seq_increasing = false;
			prev_seq = m.sync_seq;
			all_pulled.push_back(m.message_id);
		}
		cursor = next_seq;
		if (!has_more) break;
		if (pages > 20) { pull_ok = false; break; }
	}

	std::set<std::int64_t> pulled_set(all_pulled.begin(), all_pulled.end());
	Check(pull_ok && (int)all_pulled.size() == MSG_COUNT &&
	      (int)pulled_set.size() == MSG_COUNT,
		"pull-bytes: multi-page sync returns all messages, no miss no dup",
		("pulled=" + std::to_string(all_pulled.size()) +
		 " unique=" + std::to_string(pulled_set.size()) +
		 " pages=" + std::to_string(pages)).c_str());
	if (!pull_ok || (int)pulled_set.size() != MSG_COUNT) all_ok = false;

	Check(seq_increasing && has_more_ok && pages == 3,
		"pull-bytes: sync_seq strictly increasing across pages, has_more correct",
		("pages=" + std::to_string(pages) +
		 " increasing=" + std::to_string(seq_increasing) +
		 " has_more_ok=" + std::to_string(has_more_ok)).c_str());
	if (!(seq_increasing && has_more_ok)) all_ok = false;

	// Clean up: ACK all（message_ids 为字符串数组）.
	if (!all_pulled.empty()) {
		std::vector<std::int64_t> v(pulled_set.begin(), pulled_set.end());
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
// 次、sender 仍收 1302；kill/restart receiver ChatServer 后 receiver 经增量同步
// 补齐消息。
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
	if (!pm.Start({ "GateServer", { "GateServer", "GateServer.exe" },
		MakeGateIni(), GATE_HTTP_PORT }, 15000)) {
		Fail("cross-server: start GateServer", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}
	// chatserver1: peer = chatserver2, but routed THROUGH the proxy port.
	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4),
		CHAT1_TCP_PORT }, 15000)) {
		Fail("cross-server: start chatserver1", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}
	if (!pm.Start({ "chatserver2", { "chatserver2", "ChatServer.exe" },
		MakeChatIni("chatserver2", CHAT2_TCP_PORT, CHAT2_GRPC_PORT, 4,
		            CHAT2_PROXY_GRPC_PORT),
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

	// Login receiver on chatserver2 (pinned via lease inflation; uip_1019 = "chatserver2").
	TcpClient cR;
	if (!LoginUserPinned(cR, RECEIVER_UID, redis, "chatserver2", CHAT2_TCP_PORT)) {
		Fail("cross-server: login receiver on chatserver2", "pinned gate/chat login failed");
		proxy.Stop(); cleanup(); pm.StopAll(); return false;
	}
	// Login sender on chatserver1.
	TcpClient cS;
	if (!LoginUserPinned(cS, SENDER_UID, redis, "chatserver1", CHAT1_TCP_PORT)) {
		Fail("cross-server: login sender on chatserver1", "pinned gate/chat login failed");
		cR.Close(); proxy.Stop(); cleanup(); pm.StopAll(); return false;
	}

	// 同步起点：发送前 receiver 的最大 sync_seq。
	const std::uint64_t checkpoint = mysql.MaxSyncSeq(RECEIVER_UID);

	// Sender sends one message to receiver (cross-server via proxy → broken).
	const std::string uid_str = "imtest-cross-" + tag;
	std::string body = BuildTextReq(SENDER_UID, RECEIVER_UID, THREAD_ID,
		"cross-server-msg", uid_str);
	auto t_start = std::chrono::steady_clock::now();
	cS.Send(ID_TEXT_CHAT_MSG_REQ, body);

	// Sender still receives 1302 (MySQL commit before RPC).
	Frame rsp;
	bool got_1302 = cS.Wait(ID_TEXT_CHAT_MSG_RSP, 15000, &rsp);
	std::int64_t sent_mid = -1;
	if (got_1302) {
		auto j = ParseJson(rsp.body);
		if (j.is_object() && j.value("error", -1) == ERR_SUCCESS)
			sent_mid = JsonIdStr(j, "message_id", -1);
	}
	Check(got_1302 && sent_mid > 0, "cross-server: sender got 1302 despite broken RPC",
		("mid=" + std::to_string(sent_mid)).c_str());
	if (!(got_1302 && sent_mid > 0)) all_ok = false;

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

	// Message is in pending: MySQL delivery_status=0，且 user_message_sync 已写行
	// （同步行与消息同事务落库，不依赖跨服 RPC 是否成功）。
	{
		auto rows = mysql.QueryByUniqueId(SENDER_UID, uid_str);
		bool db_pending = !rows.empty() && rows[0].delivery_status == 0;
		Check(db_pending, "cross-server: message in MySQL pending (delivery_status=0)",
			rows.empty() ? "no row" : ("ds=" + std::to_string(rows[0].delivery_status)).c_str());
		if (!db_pending) all_ok = false;
	}
	{
		const auto sync_rows = mysql.QuerySyncRows(RECEIVER_UID, checkpoint, 0);
		bool sync_pending = false;
		for (const auto& r : sync_rows) {
			if (r.message_id == (std::uint64_t)sent_mid) { sync_pending = true; break; }
		}
		Check(sync_pending, "cross-server: message_id has user_message_sync row",
			("mid=" + std::to_string(sent_mid)).c_str());
		if (!sync_pending) all_ok = false;
	}

	// Phase 2: kill/restart chatserver2, receiver reconnects, pulls pending.
	proxy.Stop();
	cR.Close();

	pm.StopOne("chatserver2");
	std::this_thread::sleep_for(std::chrono::milliseconds(500));

	// Restart chatserver2 with the same config (direct, no proxy needed for pull).
	if (!pm.Start({ "chatserver2", { "chatserver2", "ChatServer.exe" },
		MakeChatIni("chatserver2", CHAT2_TCP_PORT, CHAT2_GRPC_PORT, 4),
		CHAT2_TCP_PORT }, 15000)) {
		Fail("cross-server: restart chatserver2", "ready timeout");
		cS.Close(); cleanup(); pm.StopAll(); return false;
	}

	// Receiver reconnects and logs in. The killed chatserver2 may have died holding
	// the login lock (lock TTL 10s) after processing cR's disconnect, so retry the
	// reconnect a few times instead of assuming the first attempt succeeds.
	TcpClient cR2;
	bool relogin_ok = false;
	for (int attempt = 0; attempt < 6 && !relogin_ok; ++attempt) {
		if (attempt > 0) std::this_thread::sleep_for(std::chrono::seconds(2));
		relogin_ok = LoginUser(cR2, RECEIVER_UID).ok;
		if (!relogin_ok) cR2.Close();
	}
	if (!relogin_ok) {
		Fail("cross-server: receiver reconnect after restart", "gate/chat login failed");
		cS.Close(); cleanup(); pm.StopAll(); return false;
	}

	// Sync from checkpoint → should get the message（checkpoint 之前的旧消息不推）。
	std::vector<SyncEnvelope> sync_msgs;
	std::uint64_t next_seq = checkpoint;
	bool has_more = false;
	bool pull_ok = DoSyncPage(cR2, RECEIVER_UID, checkpoint, 100, sync_msgs, next_seq, has_more);
	bool got_msg = false;
	if (pull_ok) {
		for (const auto& m : sync_msgs) {
			if (m.message_id == sent_mid) { got_msg = true; break; }
		}
	}
	Check(got_msg, "cross-server: receiver synced message after restart",
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
// resource-offline（原 image-offline，统一资源协议改造）
//
// 走 ResourceServer 分片上传：创建（1503）后 resource_status=Uploading 的行绝不
// 出现在增量同步流；分片收齐且整文件 SHA-256 校验通过后（1508 resource_status=1），
// sync 行写入，同步流出现 msg_type=PIC/content_hash 正确的消息，ACK 后清理。
// ---------------------------------------------------------------------------
bool ScenarioResourceOffline() {
	std::printf("\n=== scenario: resource-offline ===\n");
	ProcessManager pm;
	Redis redis; Mysql mysql;
	std::string tag = RunTag();
	bool all_ok = true;

	auto cleanup = [&] { CleanupFootprint(redis, mysql); };

	if (!redis.Connect(REDIS_HOST, REDIS_PORT, REDIS_PASSWD)) {
		Fail("resource-offline: connect Redis", "failed"); return false;
	}
	if (!mysql.Connect(MYSQL_HOST, MYSQL_PORT, MYSQL_USER, MYSQL_PASSWD, MYSQL_SCHEMA)) {
		Fail("resource-offline: connect MySQL", "failed"); return false;
	}
	cleanup();

	if (!pm.Start({ "StatusServer", { "StatusServer", "StatusServer.exe" },
		MakeStatusIni(), STATUS_GRPC_PORT }, 15000)) {
		Fail("resource-offline: start StatusServer", "ready timeout"); cleanup(); return false;
	}
	if (!pm.Start({ "GateServer", { "GateServer", "GateServer.exe" },
		MakeGateIni(), GATE_HTTP_PORT }, 15000)) {
		Fail("resource-offline: start GateServer", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}
	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4),
		CHAT1_TCP_PORT }, 15000)) {
		Fail("resource-offline: start ChatServer", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}
	if (!pm.Start({ "ResourceServer", { "ResourceServer", "ResourceServer.exe" },
		MakeResourceIni(), RESOURCE_HTTP_PORT }, 15000)) {
		Fail("resource-offline: start ResourceServer", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}

	// Login sender (receiver stays offline).
	TcpClient cS;
	auto li = LoginUser(cS, SENDER_UID);
	if (!li.ok) {
		Fail("resource-offline: login sender", "gate/chat login failed"); cleanup(); pm.StopAll(); return false;
	}

	// Step 1: send 1503 resource metadata to ChatServer.
	const std::string uid_str = "imtest-res-" + tag;
	const std::string file_name = "test_img_" + tag + ".png";
	const long long file_size = 70000;  // >2 片（32KiB），覆盖多分片路径
	const std::string blob = MakeBlob(file_size);
	const std::string content_hash = llfc::Sha256Hex(blob);
	// 同步起点：元数据写入前 receiver 的最大 sync_seq。
	const std::uint64_t checkpoint = mysql.MaxSyncSeq(RECEIVER_UID);
	std::string meta_body = BuildResourceCreateReq(SENDER_UID, RECEIVER_UID, THREAD_ID,
		uid_str, MSG_TYPE_PIC, file_name, file_size, content_hash, "image/png");
	cS.Send(ID_CREATE_RESOURCE_MSG_REQ, meta_body);
	Frame mrs; bool got_1504 = cS.Wait(ID_CREATE_RESOURCE_MSG_RSP, 10000, &mrs);
	std::int64_t msg_id = -1;
	int create_err = -1;
	if (got_1504) {
		auto j = ParseJson(mrs.body);
		create_err = j.is_object() ? j.value("error", -1) : -1;
		if (create_err == ERR_SUCCESS)
			msg_id = JsonIdStr(j, "message_id", -1);
	}
	Check(got_1504 && create_err == ERR_SUCCESS && msg_id > 0,
		"resource-offline: 1503/1504 metadata persisted",
		("err=" + std::to_string(create_err) + " msg_id=" + std::to_string(msg_id)).c_str());
	if (!(got_1504 && create_err == ERR_SUCCESS && msg_id > 0)) {
		cS.Close(); cleanup(); pm.StopAll(); return false;
	}

	// Step 2: verify Uploading row is NOT in the sync stream / user_message_sync.
	{
		auto rows = mysql.QueryByMessageId(msg_id);
		bool is_uploading = !rows.empty() &&
			rows[0].resource_status == RESOURCE_UPLOADING &&
			rows[0].msg_type == MSG_TYPE_PIC &&
			rows[0].content_hash == content_hash;
		Check(is_uploading, "resource-offline: row is Uploading/PIC with hash before upload",
			rows.empty() ? "no row"
				: ("rs=" + std::to_string(rows[0].resource_status)).c_str());
		if (!is_uploading) all_ok = false;
	}
	// user_message_sync 必须还没有该 message_id（元数据阶段不写同步行）。
	{
		const auto sync_rows = mysql.QuerySyncRows(RECEIVER_UID, checkpoint, 0);
		bool absent = true;
		for (const auto& r : sync_rows) {
			if (r.message_id == (std::uint64_t)msg_id) { absent = false; break; }
		}
		Check(absent, "resource-offline: Uploading msg_id absent from user_message_sync",
			absent ? "ok" : "present before upload!");
		if (!absent) all_ok = false;
	}

	// Login receiver：上传完成前同步流不含该 message_id；保持在线到上传后再同步。
	TcpClient cR;
	if (!LoginUser(cR, RECEIVER_UID).ok) {
		Fail("resource-offline: login receiver", "gate/chat login failed");
		cS.Close(); cleanup(); pm.StopAll(); return false;
	}
	std::uint64_t cursor = checkpoint;
	{
		std::vector<SyncEnvelope> msgs;
		std::uint64_t next_seq = cursor;
		bool has_more = false;
		bool synced = DoSyncPage(cR, RECEIVER_UID, cursor, 100, msgs, next_seq, has_more);
		bool found = false;
		for (const auto& m : msgs) if (m.message_id == msg_id) found = true;
		Check(synced && !found, "resource-offline: sync stream excludes msg before upload",
			("synced=" + std::to_string(synced) + " found=" + std::to_string(found)).c_str());
		if (!(synced && !found)) all_ok = false;
		cursor = next_seq;
	}

	// Step 3: connect to ResourceServer, authenticate (1501), then upload all chunks
	// via 1507（offset/chunk_sha256/data）。最后一片收齐后服务端做整文件 SHA-256
	// 校验并通过 CompleteResourceUploadWithSync 置 Ready。
	ResClient res;
	if (!res.Connect("127.0.0.1", RESOURCE_HTTP_PORT, 10000)) {
		Fail("resource-offline: connect ResourceServer", "connect failed"); all_ok = false;
	} else {
		int ra = ResourceLogin(res, SENDER_UID, li.token);
		Check(ra == ERR_SUCCESS, "resource-offline: ResourceLogin (1501) success",
			("err=" + std::to_string(ra)).c_str());
		if (ra != ERR_SUCCESS) all_ok = false;
	}
	int final_rs = -1;
	for (long long off = 0; off < file_size; ) {
		const long long n = (std::min<long long>)(file_size - off, 32768);
		const std::string chunk = blob.substr(static_cast<std::size_t>(off),
			static_cast<std::size_t>(n));
		res.Send(ID_RESOURCE_CHUNK_UPLOAD_REQ,
			BuildChunkUploadReq(msg_id, off, llfc::Sha256Hex(chunk), Base64Encode(chunk)));
		Frame uf;
		if (!res.Wait(ID_RESOURCE_CHUNK_UPLOAD_RSP, 10000, &uf)) { final_rs = -2; break; }
		auto j = ParseJson(uf.body);
		if (!j.is_object() || j.value("error", -1) != ERR_SUCCESS) {
			final_rs = j.is_object() ? j.value("error", -1) : -3;
			break;
		}
		final_rs = j.value("resource_status", -1);
		off += n;
	}
	res.Close();
	Check(final_rs == RESOURCE_READY,
		"resource-offline: all chunks uploaded, last rsp resource_status=Ready",
		("rs=" + std::to_string(final_rs)).c_str());
	if (final_rs != RESOURCE_READY) all_ok = false;

	// Step 4: after upload, verify sync activation.
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	{
		auto rows = mysql.QueryByMessageId(msg_id);
		bool ready = !rows.empty() && rows[0].resource_status == RESOURCE_READY;
		Check(ready, "resource-offline: resource_status migrated to Ready after upload",
			rows.empty() ? "no row"
				: ("rs=" + std::to_string(rows[0].resource_status)).c_str());
		if (!ready) all_ok = false;
	}
	// 上传完成后 user_message_sync 应写入该 message_id（轮询等事务提交）。
	bool sync_row_present = false;
	for (int poll = 0; poll < 50 && !sync_row_present; ++poll) {
		const auto sync_rows = mysql.QuerySyncRows(RECEIVER_UID, cursor, 0);
		for (const auto& r : sync_rows) {
			if (r.message_id == (std::uint64_t)msg_id) { sync_row_present = true; break; }
		}
		if (!sync_row_present) std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	Check(sync_row_present, "resource-offline: msg_id in user_message_sync after upload",
		sync_row_present ? "ok" : "absent after upload!");
	if (!sync_row_present) all_ok = false;

	// Step 5: receiver 再次同步 → 出现 PIC 消息且 content_size 正确。
	bool got_pic = false;
	long long pulled_size = -1;
	{
		std::vector<SyncEnvelope> msgs;
		std::uint64_t next_seq = cursor;
		bool has_more = false;
		if (DoSyncPage(cR, RECEIVER_UID, cursor, 100, msgs, next_seq, has_more)) {
			for (const auto& m : msgs) {
				if (m.message_id == msg_id && m.msg_type == MSG_TYPE_PIC) {
					got_pic = true;
					pulled_size = ParseIdStr(m.content_size, -1);
					break;
				}
			}
		}
	}
	Check(got_pic && pulled_size == file_size,
		"resource-offline: receiver synced PIC msg with correct content_size",
		("got_pic=" + std::to_string(got_pic) + " size=" + std::to_string(pulled_size)).c_str());
	if (!(got_pic && pulled_size == file_size)) all_ok = false;

	// Step 6: ACK → cleanup.
	if (got_pic) {
		cR.Send(ID_CHAT_DELIVERY_ACK_REQ, BuildAckReq(RECEIVER_UID, {msg_id}));
		Frame af; cR.Wait(ID_CHAT_DELIVERY_ACK_RSP, 10000, &af);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		auto rows = mysql.QueryByMessageId(msg_id);
		bool acked = !rows.empty() && rows[0].delivery_status == 1;
		Check(acked, "resource-offline: ACK cleans up (delivery_status=1)",
			rows.empty() ? "no row" : ("ds=" + std::to_string(rows[0].delivery_status)).c_str());
		if (!acked) all_ok = false;
	}

	cS.Close(); cR.Close();
	cleanup();
	pm.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// status-discovery (plan 3.1)
//
// ChatServers publish endpoint metadata to chatserver:registry and refresh a
// short-lived load lease. StatusServer has no configured node list: every call
// discovers, validates and selects from the current Redis state.
// ---------------------------------------------------------------------------
bool ScenarioStatusDiscovery() {
	std::printf("\n=== scenario: status-discovery ===\n");
	ProcessManager pm;
	Redis redis;
	if (!redis.Connect(REDIS_HOST, REDIS_PORT, REDIS_PASSWD)) {
		Fail("status-discovery: connect redis", "failed");
		return false;
	}

	const std::string lease1 = ChatLeaseKey("chatserver1");
	const std::string lease2 = ChatLeaseKey("chatserver2");
	const std::string lease3 = ChatLeaseKey("chatserver3");
	const std::vector<std::string> registry_fields = {
		"chatserver1", "chatserver2", "chatserver3",
		"bad-json", "mismatched-name", "empty-endpoint", "bad-load"
	};
	auto cleanup = [&]() {
		redis.Del(UserTokenKey(1));
		redis.Del(lease1);
		redis.Del(lease2);
		redis.Del(lease3);
		for (const auto& field : registry_fields) {
			redis.Del(ChatLeaseKey(field));
			redis.HDel(CHAT_REGISTRY_KEY, field);
		}
	};
	cleanup();

	if (!pm.Start({ "StatusServer", { "StatusServer", "StatusServer.exe" },
		MakeStatusIni(), STATUS_GRPC_PORT }, 15000)) {
		Fail("status-discovery: start StatusServer", "ready timeout"); cleanup(); return false;
	}

	bool all_ok = true;
	StatusClient sc;
	if (!sc.Connect("127.0.0.1", STATUS_GRPC_PORT)) {
		Fail("status-discovery: status client connect", "failed");
		cleanup(); pm.StopAll(); return false;
	}

	// Status starts with no topology and therefore has no candidate.
	int empty_error = -1;
	std::string empty_name, empty_host, empty_port, empty_token;
	bool empty_rpc = sc.GetChatServer(1, empty_error, empty_name, empty_host,
		empty_port, empty_token);
	bool initially_empty = empty_rpc && empty_error == ERR_NO_AVAILABLE_CHAT_SERVER
		&& empty_name.empty() && empty_host.empty();
	Check(initially_empty, "status-discovery: no registry entries -> no available node",
		("err=" + std::to_string(empty_error) + " name=" + empty_name).c_str());
	if (!initially_empty) all_ok = false;

	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4),
		CHAT1_TCP_PORT }, 15000)) {
		Fail("status-discovery: start chatserver1", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}
	if (!pm.Start({ "chatserver2", { "chatserver2", "ChatServer.exe" },
		MakeChatIni("chatserver2", CHAT2_TCP_PORT, CHAT2_GRPC_PORT, 4),
		CHAT2_TCP_PORT }, 15000)) {
		Fail("status-discovery: start chatserver2", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}
	// This process uses the existing chatserver1 binary with an independent cwd.
	// StatusServer is deliberately not restarted before the third node joins.
	if (!pm.Start({ "chatserver3", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver3", CHAT3_TCP_PORT, CHAT3_GRPC_PORT, 4),
		CHAT3_TCP_PORT }, 15000)) {
		Fail("status-discovery: start chatserver3", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}

	// Every node must publish both its endpoint record and its live lease.
	auto wait_registration = [&](const std::string& name, int tcp_port,
	                             int rpc_port) -> bool {
		for (int i = 0; i < 100; ++i) {  // 100 * 100ms = 10s
			std::string lease_value;
			std::string metadata;
			if (redis.Get(ChatLeaseKey(name), lease_value) && lease_value == "0" &&
			    redis.HGet(CHAT_REGISTRY_KEY, name, metadata)) {
				int ttl = redis.Ttl(ChatLeaseKey(name));
				auto j = ParseJson(metadata);
				if (ttl > 0 && ttl <= 8 && j.is_object() &&
				    j.value("name", "") == name &&
				    j.value("tcp_host", "") == "127.0.0.1" &&
				    j.value("tcp_port", "") == std::to_string(tcp_port) &&
				    j.value("rpc_host", "") == "127.0.0.1" &&
				    j.value("rpc_port", "") == std::to_string(rpc_port)) {
					return true;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		return false;
	};
	const bool r1 = wait_registration("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT);
	const bool r2 = wait_registration("chatserver2", CHAT2_TCP_PORT, CHAT2_GRPC_PORT);
	const bool r3 = wait_registration("chatserver3", CHAT3_TCP_PORT, CHAT3_GRPC_PORT);
	Check(r1 && r2 && r3,
		"status-discovery: three nodes self-register without Status restart",
		("registered=" + std::to_string(r1) + "/" + std::to_string(r2) + "/" +
		 std::to_string(r3)).c_str());
	if (!r1 || !r2 || !r3) { cleanup(); pm.StopAll(); return false; }

	// Stop reporters so seeded load values remain deterministic. Metadata stays
	// in the hash, while liveness continues to be controlled by short leases.
	pm.StopOne("chatserver1");
	pm.StopOne("chatserver2");
	pm.StopOne("chatserver3");

	// Least load is selected from dynamically discovered records.
	redis.SetEx(lease1, 8, "3");
	redis.SetEx(lease2, 8, "1");
	redis.SetEx(lease3, 8, "2");
	int e1 = -1;
	std::string sn1, h1, p1, tok1;
	bool ok1 = sc.GetChatServer(1, e1, sn1, h1, p1, tok1);
	const std::string port2 = std::to_string(CHAT2_TCP_PORT);
	bool least_ok = ok1 && e1 == ERR_SUCCESS && sn1 == "chatserver2" &&
		p1 == port2 && !tok1.empty();
	Check(least_ok, "status-discovery: least-loaded dynamic node selected",
		("name=" + sn1 + " port=" + p1 + " err=" + std::to_string(e1)).c_str());
	if (!least_ok) all_ok = false;

	// Initial assignment creates a bounded per-user token.
	int ttl1 = redis.Ttl(UserTokenKey(1));
	bool ttl_ok = least_ok && ttl1 >= 1 && ttl1 <= 86400;
	Check(ttl_ok, "status-discovery: utoken_<uid> TTL in [1, 86400]",
		ttl_ok ? "" : ("ttl=" + std::to_string(ttl1)).c_str());
	if (!ttl_ok) all_ok = false;

	// Equal loads rotate across all three names rather than favoring one entry.
	redis.SetEx(lease1, 8, "0");
	redis.SetEx(lease2, 8, "0");
	redis.SetEx(lease3, 8, "0");
	std::set<std::string> rotated;
	for (int i = 0; i < 3; ++i) {
		int error = -1;
		std::string name, host, port, token;
		if (sc.GetChatServer(1, error, name, host, port, token) &&
		    error == ERR_SUCCESS) rotated.insert(name);
	}
	bool rotation_ok = rotated == std::set<std::string>{
		"chatserver1", "chatserver2", "chatserver3" };
	Check(rotation_ok, "status-discovery: equal load rotates across all live nodes",
		("unique_nodes=" + std::to_string(rotated.size())).c_str());
	if (!rotation_ok) all_ok = false;

	// Removing one lease excludes only that node.
	redis.Del(lease2);
	bool excl_ok = true;
	for (int i = 0; i < 4; ++i) {
		int error = -1;
		std::string name, host, port, token;
		excl_ok = sc.GetChatServer(1, error, name, host, port, token) &&
			error == ERR_SUCCESS && name != "chatserver2" && excl_ok;
	}
	Check(excl_ok, "status-discovery: missing lease excludes registered node",
		excl_ok ? "" : "chatserver2 was selected");
	if (!excl_ok) all_ok = false;

	// Malformed records and malformed loads are ignored fail-closed.
	redis.Del(lease1);
	redis.Del(lease3);
	redis.HSet(CHAT_REGISTRY_KEY, "bad-json", "{");
	redis.SetEx(ChatLeaseKey("bad-json"), 8, "0");
	redis.HSet(CHAT_REGISTRY_KEY, "mismatched-name",
		R"({"name":"someone-else","tcp_host":"127.0.0.1","tcp_port":"1","rpc_host":"127.0.0.1","rpc_port":"2"})");
	redis.SetEx(ChatLeaseKey("mismatched-name"), 8, "0");
	redis.HSet(CHAT_REGISTRY_KEY, "empty-endpoint",
		R"({"name":"empty-endpoint","tcp_host":"","tcp_port":"1","rpc_host":"127.0.0.1","rpc_port":"2"})");
	redis.SetEx(ChatLeaseKey("empty-endpoint"), 8, "0");
	redis.HSet(CHAT_REGISTRY_KEY, "bad-load",
		R"({"name":"bad-load","tcp_host":"127.0.0.1","tcp_port":"1","rpc_host":"127.0.0.1","rpc_port":"2"})");
	redis.SetEx(ChatLeaseKey("bad-load"), 8, "not-a-number");
	int e5 = -1; std::string sn5, h5, p5, tok5;
	bool ok5 = sc.GetChatServer(1, e5, sn5, h5, p5, tok5);
	bool none_ok = ok5 && e5 == ERR_NO_AVAILABLE_CHAT_SERVER && sn5.empty() && h5.empty();
	Check(none_ok, "status-discovery: malformed registry state is ignored",
		none_ok ? "" : ("err=" + std::to_string(e5) + " host=" + h5).c_str());
	if (!none_ok) all_ok = false;

	cleanup();
	pm.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// chat-failover
//
// Status issues the same Redis credential that Gate normally requests after
// password validation. After the assigned ChatServer disappears, Gate asks
// Status for another live node using that token without rotating or renewing it.
// ---------------------------------------------------------------------------
bool ScenarioChatFailover() {
	std::printf("\n=== scenario: chat-failover ===\n");
	ProcessManager pm;
	Redis redis; Mysql mysql;
	const std::string tag = RunTag();
	const int failover_uid = SENDER_UID;
	const int message_sender_uid = RECEIVER_UID;
	if (!redis.Connect(REDIS_HOST, REDIS_PORT, REDIS_PASSWD)) {
		Fail("chat-failover: connect redis", "failed");
		return false;
	}
	if (!mysql.Connect(MYSQL_HOST, MYSQL_PORT, MYSQL_USER, MYSQL_PASSWD, MYSQL_SCHEMA)) {
		Fail("chat-failover: connect MySQL", "failed");
		return false;
	}

	auto cleanup = [&]() {
		CleanupFootprint(redis, mysql);
	};
	cleanup();

	if (!pm.Start({ "StatusServer", { "StatusServer", "StatusServer.exe" },
		MakeStatusIni(), STATUS_GRPC_PORT }, 15000) ||
		!pm.Start({ "GateServer", { "GateServer", "GateServer.exe" },
			MakeGateIni(), GATE_HTTP_PORT }, 15000) ||
		!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
			MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4),
			CHAT1_TCP_PORT }, 15000) ||
		!pm.Start({ "chatserver2", { "chatserver2", "ChatServer.exe" },
			MakeChatIni("chatserver2", CHAT2_TCP_PORT, CHAT2_GRPC_PORT, 4),
			CHAT2_TCP_PORT }, 15000)) {
		Fail("chat-failover: start services", "ready timeout");
		cleanup(); pm.StopAll(); return false;
	}

	auto wait_registered = [&](const std::string& name) {
		for (int i = 0; i < 100; ++i) {
			std::string metadata;
			std::string lease;
			if (redis.HGet(CHAT_REGISTRY_KEY, name, metadata) &&
			    redis.Get(ChatLeaseKey(name), lease)) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		return false;
	};
	if (!wait_registered("chatserver1") || !wait_registered("chatserver2")) {
		Fail("chat-failover: wait for registrations", "registry or lease missing");
		cleanup(); pm.StopAll(); return false;
	}

	bool all_ok = true;
	GateLoginInfo initial;
	StatusClient initial_status_client;
	std::string initial_port_text;
	const bool initial_assigned = initial_status_client.Connect(
		"127.0.0.1", STATUS_GRPC_PORT) &&
		initial_status_client.GetChatServer(failover_uid, initial.error,
			initial.server_name, initial.chat_host, initial_port_text, initial.token);
	initial.chat_port = static_cast<unsigned short>(std::atoi(initial_port_text.c_str()));
	initial.ok = initial_assigned && initial.error == ERR_SUCCESS &&
		!initial.server_name.empty() && !initial.chat_host.empty() &&
		initial.chat_port != 0 && !initial.token.empty();
	if (!initial.ok) {
		Fail("chat-failover: Status assigns receiver and issues token",
			("error=" + std::to_string(initial.error)).c_str());
		cleanup(); pm.StopAll(); return false;
	}
	TcpClient original_receiver;
	const bool original_connect = original_receiver.Connect(
		initial.chat_host, initial.chat_port, 10000);
	const auto original_login = original_connect
		? ChatLogin(original_receiver, failover_uid, initial.token)
		: ChatLoginOutcome{};
	if (!original_connect || !original_login.ok) {
		Fail("chat-failover: receiver login on assigned node",
			("connected=" + std::to_string(original_connect) +
			 " error=" + std::to_string(original_login.error)).c_str());
		original_receiver.Close(); cleanup(); pm.StopAll(); return false;
	}
	std::string original_route;
	const bool route_bound = redis.Get("uip_" + std::to_string(failover_uid),
		original_route) && original_route == initial.server_name;
	Check(route_bound, "chat-failover: receiver route points at assigned node",
		("route=" + original_route + " expected=" + initial.server_name).c_str());
	if (!route_bound) all_ok = false;

	std::string stored_before;
	const int ttl_before = redis.Ttl(UserTokenKey(failover_uid));
	const bool token_before_ok = redis.Get(UserTokenKey(failover_uid), stored_before) &&
		stored_before == initial.token && ttl_before > 0;
	Check(token_before_ok, "chat-failover: initial token stored with TTL",
		("ttl=" + std::to_string(ttl_before)).c_str());
	if (!token_before_ok) all_ok = false;

	// 同步起点：故障前 receiver 的 bootstrap checkpoint（之后产生的消息才需补齐）。
	std::uint64_t checkpoint = 0;
	if (!DoSyncBootstrap(original_receiver, failover_uid, checkpoint)) {
		Fail("chat-failover: bootstrap checkpoint before failure", "1405/1406 failed");
		original_receiver.Close(); cleanup(); pm.StopAll(); return false;
	}

	const std::string stopped_name = initial.server_name;
	const std::string survivor_name = stopped_name == "chatserver1"
		? "chatserver2" : "chatserver1";
	const unsigned short survivor_port = survivor_name == "chatserver1"
		? CHAT1_TCP_PORT : CHAT2_TCP_PORT;
	if (!pm.StopOne(stopped_name)) {
		Fail("chat-failover: stop assigned node", stopped_name);
		original_receiver.Close(); cleanup(); pm.StopAll(); return false;
	}
	original_receiver.Close();
	// Forced test termination cannot run the process's graceful cleanup, so
	// remove only its short lease to model expiry without an eight-second wait.
	redis.Del(ChatLeaseKey(stopped_name));

	// The receiver route still names the failed node. Persist a message through
	// the surviving node and prove the failed live delivery remains recoverable.
	StatusClient status_client;
	int sender_status_error = -1;
	std::string sender_server_name;
	std::string sender_host;
	std::string sender_port_text;
	std::string sender_token;
	const bool sender_assigned = status_client.Connect("127.0.0.1", STATUS_GRPC_PORT) &&
		status_client.GetChatServer(message_sender_uid, sender_status_error,
			sender_server_name, sender_host, sender_port_text, sender_token) &&
		sender_status_error == ERR_SUCCESS && sender_server_name == survivor_name &&
		!sender_token.empty();
	const unsigned short sender_port = static_cast<unsigned short>(
		std::atoi(sender_port_text.c_str()));
	TcpClient sender;
	const bool sender_connect = sender_assigned && sender_port != 0 &&
		sender.Connect(sender_host, sender_port, 10000);
	const auto sender_login = sender_connect
		? ChatLogin(sender, message_sender_uid, sender_token)
		: ChatLoginOutcome{};
	if (!sender_connect || !sender_login.ok) {
		Fail("chat-failover: sender login on surviving node",
			("node=" + sender_server_name +
			 " error=" + std::to_string(sender_login.error)).c_str());
		sender.Close(); cleanup(); pm.StopAll(); return false;
	}

	const std::string unique_id = "imtest-failover-" + tag;
	const std::string content = "message-during-chatserver-failure";
	const std::string text_body = BuildTextReq(
		message_sender_uid, failover_uid, THREAD_ID, content, unique_id);
	Frame sender_rsp;
	json sender_envelope;
	std::int64_t message_id = -1;
	bool sender_ack = sender.Send(ID_TEXT_CHAT_MSG_REQ, text_body) &&
		sender.Wait(ID_TEXT_CHAT_MSG_RSP, 10000, &sender_rsp);
	if (sender_ack) {
		const auto response_json = ParseJson(sender_rsp.body);
		sender_ack = response_json.is_object() &&
			response_json.value("error", -1) == ERR_SUCCESS &&
			response_json.contains("message_id");
		if (sender_ack) {
			sender_envelope = response_json;
			message_id = JsonIdStr(sender_envelope, "message_id", -1);
			sender_ack = message_id > 0 &&
				sender_envelope.value("unique_id", "") == unique_id;
		}
	}
	Check(sender_ack, "chat-failover: sender receives persisted ACK during failure",
		("message_id=" + std::to_string(message_id)).c_str());
	if (!sender_ack) all_ok = false;

	ChatMessageRow stored_message;
	const auto pending_rows = mysql.QueryByUniqueId(message_sender_uid, unique_id);
	const bool mysql_pending = pending_rows.size() == 1 &&
		pending_rows[0].message_id == message_id &&
		pending_rows[0].delivery_status == 0;
	if (!pending_rows.empty()) stored_message = pending_rows[0];
	Check(mysql_pending, "chat-failover: MySQL has exactly one pending row",
		("rows=" + std::to_string(pending_rows.size()) +
		 " message_id=" + std::to_string(message_id)).c_str());
	if (!mysql_pending) all_ok = false;

	// 故障期间的实时投递失败不影响可恢复性：user_message_sync 行同事务落库。
	bool sync_row_present = false;
	{
		const auto sync_rows = mysql.QuerySyncRows(failover_uid, checkpoint, 0);
		for (const auto& r : sync_rows) {
			if (r.message_id == (std::uint64_t)message_id) { sync_row_present = true; break; }
		}
	}
	Check(sync_row_present, "chat-failover: user_message_sync records message id",
		("message_id=" + std::to_string(message_id)).c_str());
	if (!sync_row_present) all_ok = false;

	json request;
	request["uid"] = failover_uid;
	request["token"] = initial.token;
	HttpResponse response = HttpPost("127.0.0.1", GATE_HTTP_PORT,
		"/reassign_chat", request.dump(), "text/json", 10000);
	auto body = ParseJson(response.body);
	const int reassign_error = body.is_object() ? body.value("error", -1) : -1;
	const std::string reassigned_name = body.is_object()
		? body.value("server_name", "") : "";
	const std::string reassigned_host = body.is_object()
		? body.value("chathost", "") : "";
	const std::string reassigned_port = body.is_object()
		? body.value("chatport", "") : "";
	const std::string returned_token = body.is_object()
		? body.value("token", "") : "";
	const bool reassigned = response.status == 200 &&
		reassign_error == ERR_SUCCESS && reassigned_name == survivor_name &&
		reassigned_host == "127.0.0.1" &&
		reassigned_port == std::to_string(survivor_port) &&
		(returned_token.empty() || returned_token == initial.token);
	Check(reassigned, "chat-failover: Gate reassigns to surviving node",
		("http=" + std::to_string(response.status) + " err=" +
		 std::to_string(reassign_error) + " node=" + reassigned_name).c_str());
	if (!reassigned) all_ok = false;

	std::string stored_after;
	const int ttl_after = redis.Ttl(UserTokenKey(failover_uid));
	const bool token_unchanged = redis.Get(UserTokenKey(failover_uid), stored_after) &&
		stored_after == stored_before && ttl_after > 0 && ttl_after <= ttl_before;
	Check(token_unchanged, "chat-failover: reassignment preserves token and TTL",
		("ttl_before=" + std::to_string(ttl_before) +
		 " ttl_after=" + std::to_string(ttl_after)).c_str());
	if (!token_unchanged) all_ok = false;

	TcpClient recovered_receiver;
	const bool connect_ok = recovered_receiver.Connect(
		"127.0.0.1", survivor_port, 10000);
	const auto login = connect_ok
		? ChatLogin(recovered_receiver, failover_uid, initial.token)
		: ChatLoginOutcome{};
	Check(connect_ok && login.ok,
		"chat-failover: original token logs into surviving ChatServer",
		("connected=" + std::to_string(connect_ok) +
		 " error=" + std::to_string(login.error)).c_str());
	if (!connect_ok || !login.ok) all_ok = false;

	// 重连存活节点后从 checkpoint 增量同步，应补齐故障期间产生的消息。
	SyncEnvelope recovered_envelope;
	bool recovered_message = false;
	if (connect_ok && login.ok) {
		std::vector<SyncEnvelope> sync_msgs;
		std::uint64_t next_seq = checkpoint;
		bool has_more = false;
		if (DoSyncPage(recovered_receiver, failover_uid, checkpoint, 100,
			sync_msgs, next_seq, has_more)) {
			for (const auto& message : sync_msgs) {
				if (message.message_id == message_id) {
					recovered_envelope = message;
					recovered_message = true;
					break;
				}
			}
		}
	}

	const bool envelope_preserved = recovered_message && mysql_pending &&
		recovered_envelope.message_id == stored_message.message_id &&
		recovered_envelope.unique_id == stored_message.unique_id &&
		recovered_envelope.thread_id == stored_message.thread_id &&
		recovered_envelope.fromuid == stored_message.sender_id &&
		recovered_envelope.touid == stored_message.recv_id &&
		recovered_envelope.content == stored_message.content &&
		!stored_message.chat_time.empty() &&
		recovered_envelope.chat_time == stored_message.chat_time;
	Check(envelope_preserved,
		"chat-failover: recovered envelope preserves persisted message",
		("recovered=" + std::to_string(recovered_message) +
		 " message_id=" + std::to_string(message_id)).c_str());
	if (!envelope_preserved) all_ok = false;

	bool ack_ok = false;
	if (recovered_message && recovered_receiver.Send(
		ID_CHAT_DELIVERY_ACK_REQ, BuildAckReq(failover_uid, {message_id}))) {
		Frame ack_frame;
		if (recovered_receiver.Wait(ID_CHAT_DELIVERY_ACK_RSP, 10000, &ack_frame)) {
			const auto ack = ParseJson(ack_frame.body);
			ack_ok = ack.is_object() && ack.value("error", -1) == ERR_SUCCESS;
		}
	}
	Check(ack_ok, "chat-failover: recovered message ACK succeeds",
		ack_ok ? "" : "1407/1408 failed");
	if (!ack_ok) all_ok = false;

	bool cleanup_after_ack = false;
	for (int poll = 0; poll < 50 && !cleanup_after_ack; ++poll) {
		const auto rows = mysql.QueryByMessageId(message_id);
		cleanup_after_ack = rows.size() == 1 &&
			rows[0].delivery_status == 1;
		if (!cleanup_after_ack)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	Check(cleanup_after_ack,
		"chat-failover: ACK clears MySQL pending state",
		cleanup_after_ack ? "" : ("message_id=" + std::to_string(message_id)).c_str());
	if (!cleanup_after_ack) all_ok = false;

	request["token"] = "forged-reassign-token";
	HttpResponse forged_response = HttpPost("127.0.0.1", GATE_HTTP_PORT,
		"/reassign_chat", request.dump(), "text/json", 10000);
	auto forged = ParseJson(forged_response.body);
	const bool forged_rejected = forged_response.status == 200 && forged.is_object() &&
		forged.value("error", -1) == ERR_TOKEN_INVALID &&
		forged.value("server_name", "").empty() &&
		forged.value("chathost", "").empty() &&
		forged.value("chatport", "").empty();
	Check(forged_rejected, "chat-failover: forged token returns no endpoint",
		forged.is_object() ? ("err=" + std::to_string(forged.value("error", -1))).c_str()
		                   : "non-json response");
	if (!forged_rejected) all_ok = false;

	sender.Close();
	recovered_receiver.Close();
	cleanup();
	pm.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// simple-auth
//
// End-to-end exercise of the shared per-user login token (utoken_<uid>), issued
// by Status on password login and presented unchanged to Chat (1101) and
// Resource (1501). Drives Gate /user_login and the Chat/Resource login frames
// via the headless clients. Asserts:
//   a. a forged Chat token is rejected (1102 error TokenInvalid);
//   b. a valid token from a real Gate /user_login authenticates on Chat;
//   c. the 1102 response JSON carries no secret fields (pwd/token/session_token);
//   d. an unauthenticated Resource business frame (1509) is rejected / closed;
//   e. a forged Resource token yields 1502 TokenInvalid;
//   f. a valid token yields 1502 error==0 and the uid echoed;
//   g. a second password login for the same uid overwrites utoken_<uid>: the
//      old token can no longer authenticate on Chat or Resource, the new one can.
// ---------------------------------------------------------------------------
bool ScenarioSimpleAuth() {
	std::printf("\n=== scenario: simple-auth ===\n");
	ProcessManager pm;
	Redis redis;
	std::string tag = RunTag();
	bool all_ok = true;

	if (!redis.Connect(REDIS_HOST, REDIS_PORT, REDIS_PASSWD)) {
		Fail("simple-auth: connect redis", "failed");
		return false;
	}

	auto cleanup = [&]() {
		redis.Del(UserTokenKey(SENDER_UID));
		redis.Del("uip_"       + std::to_string(SENDER_UID));
		redis.Del("usession_"  + std::to_string(SENDER_UID));
		redis.Del("ubaseinfo_" + std::to_string(SENDER_UID));
		redis.Del(ChatLeaseKey("chatserver1"));
		redis.Del(ChatLeaseKey("chatserver2"));
		redis.HDel(CHAT_REGISTRY_KEY, "chatserver1");
		redis.HDel(CHAT_REGISTRY_KEY, "chatserver2");
	};

	if (!pm.Start({ "StatusServer", { "StatusServer", "StatusServer.exe" },
		MakeStatusIni(), STATUS_GRPC_PORT }, 15000)) {
		Fail("simple-auth: start StatusServer", "ready timeout"); cleanup(); return false;
	}
	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4),
		CHAT1_TCP_PORT }, 15000)) {
		Fail("simple-auth: start chatserver1", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}
	if (!pm.Start({ "chatserver2", { "chatserver2", "ChatServer.exe" },
		MakeChatIni("chatserver2", CHAT2_TCP_PORT, CHAT2_GRPC_PORT, 4),
		CHAT2_TCP_PORT }, 15000)) {
		Fail("simple-auth: start chatserver2", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}
	if (!pm.Start({ "GateServer", { "GateServer", "GateServer.exe" },
		MakeGateIni(), GATE_HTTP_PORT }, 15000)) {
		Fail("simple-auth: start GateServer", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}
	if (!pm.Start({ "ResourceServer", { "ResourceServer", "ResourceServer.exe" },
		MakeResourceIni(), RESOURCE_HTTP_PORT }, 15000)) {
		Fail("simple-auth: start ResourceServer", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}

	// Obtain a real login token via Gate /user_login (password login).
	GateLoginInfo gl = GateLogin(SENDER_UID);
	bool have_token = gl.ok && !gl.token.empty();
	Check(have_token, "simple-auth: Gate /user_login issues a token",
		have_token ? "" : ("err=" + std::to_string(gl.error)).c_str());
	if (!have_token) { cleanup(); pm.StopAll(); return false; }
	const std::string valid_token = gl.token;
	const std::string chat_host = gl.chat_host;
	const unsigned short chat_port = gl.chat_port;

	// (a) Forged Chat token → 1102 error TokenInvalid (no bind).
	{
		TcpClient c;
		if (!c.Connect(chat_host, chat_port, 10000)) {
			Fail("simple-auth: connect chat (forged)", "connect failed"); all_ok = false;
		} else {
			auto lo = ChatLogin(c, SENDER_UID, "forged-chat-" + tag);
			Check(lo.error == ERR_TOKEN_INVALID,
				"simple-auth: forged Chat token → TokenInvalid",
				("error=" + std::to_string(lo.error)).c_str());
			if (lo.error != ERR_TOKEN_INVALID) all_ok = false;
			c.Close();
		}
	}

	// (b) Valid token → Chat login success.
	// (c) 1102 response carries no secret fields (pwd/token/session_token).
	{
		TcpClient c;
		if (!c.Connect(chat_host, chat_port, 10000)) {
			Fail("simple-auth: connect chat (valid)", "connect failed"); all_ok = false;
		} else {
			auto lo = ChatLogin(c, SENDER_UID, valid_token);
			Check(lo.ok, "simple-auth: valid Chat token → login success",
				("error=" + std::to_string(lo.error)).c_str());
			if (!lo.ok) all_ok = false;
			bool clean = lo.response.is_object()
				&& !lo.response.contains("pwd")
				&& !lo.response.contains("token")
				&& !lo.response.contains("session_token");
			Check(clean, "simple-auth: 1102 response omits pwd/token/session_token",
				clean ? "" : "secret field present in response");
			if (!clean) all_ok = false;
			c.Close();
		}
	}

	// (d) Unauthenticated Resource business frame (1509) → rejected / closed.
	{
		ResClient r0;
		if (!r0.Connect("127.0.0.1", RESOURCE_HTTP_PORT, 10000)) {
			Fail("simple-auth: connect resource (unauth)", "connect failed"); all_ok = false;
		} else {
			json b; b["message_id"] = "1";
			r0.Send(ID_RESOURCE_UPLOAD_PROGRESS_REQ, b.dump());
			Frame f;
			bool got = r0.Wait(ID_RESOURCE_UPLOAD_PROGRESS_RSP, 5000, &f);
			int e = -1;
			if (got) { auto j = ParseJson(f.body); e = j.value("error", -1); }
			// Either an explicit TokenInvalid response or the connection was
			// closed (server rejects+close) — both prove the gate held.
			bool rejected = (got && e == ERR_TOKEN_INVALID) || r0.IsClosed();
			Check(rejected, "simple-auth: unauthenticated Resource frame rejected",
				(got ? ("error=" + std::to_string(e)).c_str() : "no response"));
			if (!rejected) all_ok = false;
			r0.Close();
		}
	}

	// (e) Forged Resource token → 1502 TokenInvalid.
	{
		ResClient r1;
		if (!r1.Connect("127.0.0.1", RESOURCE_HTTP_PORT, 10000)) {
			Fail("simple-auth: connect resource (forged)", "connect failed"); all_ok = false;
		} else {
			int e = ResourceLogin(r1, SENDER_UID, "forged-res-" + tag);
			Check(e == ERR_TOKEN_INVALID,
				"simple-auth: forged Resource token → TokenInvalid",
				("error=" + std::to_string(e)).c_str());
			if (e != ERR_TOKEN_INVALID) all_ok = false;
			r1.Close();
		}
	}

	// (f) Valid token → 1502 error==0 and uid echoed. Done as a raw exchange so
	// both the error code and the echoed uid can be inspected (the ResourceLogin
	// helper returns only the error code).
	{
		ResClient r2;
		if (!r2.Connect("127.0.0.1", RESOURCE_HTTP_PORT, 10000)) {
			Fail("simple-auth: connect resource (valid)", "connect failed"); all_ok = false;
		} else {
			json j; j["uid"] = SENDER_UID; j["token"] = valid_token;
			bool sent = r2.Send(ID_RESOURCE_LOGIN_REQ, j.dump());
			Frame f;
			bool got = sent && r2.Wait(ID_RESOURCE_LOGIN_RSP, 10000, &f);
			int e = -1, echoed_uid = -1;
			if (got) {
				auto rj = ParseJson(f.body);
				if (rj.is_object()) {
					e = rj.value("error", -1);
					echoed_uid = rj.value("uid", -1);
				}
			}
			Check(got && e == ERR_SUCCESS && echoed_uid == SENDER_UID,
				"simple-auth: valid Resource token → 1502 error==0, uid echoed",
				("error=" + std::to_string(e) + " uid=" + std::to_string(echoed_uid)).c_str());
			if (!(got && e == ERR_SUCCESS && echoed_uid == SENDER_UID)) all_ok = false;
			r2.Close();
		}
	}

	// (g) A second password login for the same uid overwrites utoken_<uid>: the
	// old token can no longer authenticate on Chat or Resource; the new one can.
	{
		GateLoginInfo gl2 = GateLogin(SENDER_UID);
		const std::string new_token = gl2.ok ? gl2.token : "";
		const bool rotated = gl2.ok && !new_token.empty() && new_token != valid_token;
		Check(rotated, "simple-auth: second /user_login overwrites the token",
			rotated ? "" : ("gl2.ok=" + std::to_string((int)gl2.ok)
				+ " new_empty=" + std::to_string((int)new_token.empty())).c_str());
		if (!rotated) {
			all_ok = false;
		} else {
			// Old token can no longer log into Chat.
			{
				TcpClient c;
				if (c.Connect(gl2.chat_host, gl2.chat_port, 10000)) {
					auto lo = ChatLogin(c, SENDER_UID, valid_token);
					Check(lo.error == ERR_TOKEN_INVALID,
						"simple-auth: old Chat token rejected after rotation",
						("error=" + std::to_string(lo.error)).c_str());
					if (lo.error != ERR_TOKEN_INVALID) all_ok = false;
					c.Close();
				} else {
					Check(false, "simple-auth: old Chat token rejected after rotation",
						"connect failed");
					all_ok = false;
				}
			}
			// Old token can no longer log into Resource.
			{
				ResClient r;
				if (r.Connect("127.0.0.1", RESOURCE_HTTP_PORT, 10000)) {
					int e = ResourceLogin(r, SENDER_UID, valid_token);
					Check(e == ERR_TOKEN_INVALID,
						"simple-auth: old Resource token rejected after rotation",
						("error=" + std::to_string(e)).c_str());
					if (e != ERR_TOKEN_INVALID) all_ok = false;
					r.Close();
				}
			}
			// New token authenticates on Chat and Resource.
			{
				TcpClient c;
				if (c.Connect(gl2.chat_host, gl2.chat_port, 10000)) {
					auto lo = ChatLogin(c, SENDER_UID, new_token);
					Check(lo.ok, "simple-auth: new Chat token authenticates",
						("error=" + std::to_string(lo.error)).c_str());
					if (!lo.ok) all_ok = false;
					c.Close();
				}
			}
			{
				ResClient r;
				if (r.Connect("127.0.0.1", RESOURCE_HTTP_PORT, 10000)) {
					int e = ResourceLogin(r, SENDER_UID, new_token);
					Check(e == ERR_SUCCESS,
						"simple-auth: new Resource token authenticates",
						("error=" + std::to_string(e)).c_str());
					if (e != ERR_SUCCESS) all_ok = false;
					r.Close();
				}
			}
		}
	}

	cleanup();
	pm.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// sync-bootstrap (增量同步 checkpoint)
//
// {bootstrap:true} 只回 {error, checkpoint:"<max_seq>"}；checkpoint 之后产生的
// 消息全部同步到、之前的不推；空页 next_sync_seq=after_sync_seq、has_more=false。
// ---------------------------------------------------------------------------
bool ScenarioSyncBootstrap() {
	std::printf("\n=== scenario: sync-bootstrap ===\n");
	ProcessManager pm;
	Redis redis; Mysql mysql;
	std::string tag = RunTag();
	bool all_ok = true;
	const int MSG_COUNT = 3;

	auto cleanup = [&] { CleanupFootprint(redis, mysql); };

	if (!redis.Connect(REDIS_HOST, REDIS_PORT, REDIS_PASSWD)) {
		Fail("sync-bootstrap: connect Redis", "redis connect failed"); return false;
	}
	if (!mysql.Connect(MYSQL_HOST, MYSQL_PORT, MYSQL_USER, MYSQL_PASSWD, MYSQL_SCHEMA)) {
		Fail("sync-bootstrap: connect MySQL", "mysql connect failed"); return false;
	}
	cleanup();

	if (!pm.Start({ "StatusServer", { "StatusServer", "StatusServer.exe" },
		MakeStatusIni(), STATUS_GRPC_PORT }, 15000)) {
		Fail("sync-bootstrap: start StatusServer", "ready timeout"); cleanup(); return false;
	}
	if (!pm.Start({ "GateServer", { "GateServer", "GateServer.exe" },
		MakeGateIni(), GATE_HTTP_PORT }, 15000)) {
		Fail("sync-bootstrap: start GateServer", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}
	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4),
		CHAT1_TCP_PORT }, 15000)) {
		Fail("sync-bootstrap: start ChatServer", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}

	// receiver 登录并取 bootstrap checkpoint；与 DB 最大 sync_seq 一致。
	TcpClient cR;
	if (!LoginUser(cR, RECEIVER_UID).ok) {
		Fail("sync-bootstrap: login receiver", "gate/chat login failed"); cleanup(); pm.StopAll(); return false;
	}
	std::uint64_t checkpoint = 0;
	bool boot_ok = DoSyncBootstrap(cR, RECEIVER_UID, checkpoint);
	Check(boot_ok, "sync-bootstrap: {bootstrap:true} returns checkpoint", "bootstrap failed");
	if (!boot_ok) { cleanup(); pm.StopAll(); return false; }
	{
		const std::uint64_t db_max = mysql.MaxSyncSeq(RECEIVER_UID);
		Check(checkpoint == db_max, "sync-bootstrap: checkpoint == DB max sync_seq",
			("checkpoint=" + std::to_string(checkpoint) +
			 " db_max=" + std::to_string(db_max)).c_str());
		if (checkpoint != db_max) all_ok = false;
	}

	// checkpoint 之后 sender 发 3 条。
	TcpClient cS;
	if (!LoginUser(cS, SENDER_UID).ok) {
		Fail("sync-bootstrap: login sender", "gate/chat login failed"); cR.Close(); cleanup(); pm.StopAll(); return false;
	}
	std::vector<std::int64_t> sent_mids;
	bool send_ok = true;
	for (int i = 0; i < MSG_COUNT; ++i) {
		const std::string uid_str = "imtest-boot-" + tag + "-" + std::to_string(i);
		if (!cS.Send(ID_TEXT_CHAT_MSG_REQ, BuildTextReq(SENDER_UID, RECEIVER_UID,
			THREAD_ID, "boot-" + std::to_string(i), uid_str))) { send_ok = false; break; }
		Frame f;
		if (!cS.Wait(ID_TEXT_CHAT_MSG_RSP, 10000, &f)) { send_ok = false; break; }
		auto j = ParseJson(f.body);
		if (!j.is_object() || j.value("error", -1) != ERR_SUCCESS) { send_ok = false; break; }
		sent_mids.push_back(JsonIdStr(j, "message_id", 0));
	}
	Check(send_ok && (int)sent_mids.size() == MSG_COUNT,
		"sync-bootstrap: sender sent 3 messages after checkpoint",
		("sent=" + std::to_string(sent_mids.size())).c_str());
	if (!send_ok || (int)sent_mids.size() != MSG_COUNT) all_ok = false;

	// 从 checkpoint 同步：恰得 3 条新消息（之前的旧消息不推），sync_seq 严格递增。
	std::vector<SyncEnvelope> msgs;
	std::uint64_t next_seq = checkpoint;
	bool has_more = false;
	bool sync_ok = DoSyncPage(cR, RECEIVER_UID, checkpoint, 100, msgs, next_seq, has_more);
	bool increasing = true;
	{
		std::uint64_t prev = checkpoint;
		for (const auto& m : msgs) {
			if (m.sync_seq <= prev) increasing = false;
			prev = m.sync_seq;
		}
	}
	std::set<std::int64_t> sent_set(sent_mids.begin(), sent_mids.end());
	std::set<std::int64_t> got_set;
	for (const auto& m : msgs) got_set.insert(m.message_id);
	Check(sync_ok && increasing && got_set == sent_set && (int)msgs.size() == MSG_COUNT,
		"sync-bootstrap: only post-checkpoint messages synced, ascending",
		("synced=" + std::to_string(msgs.size()) +
		 " increasing=" + std::to_string(increasing) +
		 " has_more=" + std::to_string(has_more)).c_str());
	if (!(sync_ok && increasing && got_set == sent_set)) all_ok = false;

	// 再次 bootstrap：checkpoint 推进到最后一条的 sync_seq。
	std::uint64_t checkpoint2 = 0;
	bool boot2_ok = DoSyncBootstrap(cR, RECEIVER_UID, checkpoint2);
	bool advanced = boot2_ok && !msgs.empty() && checkpoint2 == msgs.back().sync_seq;
	Check(advanced, "sync-bootstrap: checkpoint advances past synced messages",
		("checkpoint2=" + std::to_string(checkpoint2)).c_str());
	if (!advanced) all_ok = false;

	// 从 checkpoint2 同步：空页，next_sync_seq=after_sync_seq、has_more=false。
	{
		std::vector<SyncEnvelope> empty_msgs;
		std::uint64_t next2 = 0;
		bool more2 = true;
		bool page_ok = DoSyncPage(cR, RECEIVER_UID, checkpoint2, 100, empty_msgs, next2, more2);
		Check(page_ok && empty_msgs.empty() && next2 == checkpoint2 && !more2,
			"sync-bootstrap: empty page keeps cursor, has_more=false",
			("msgs=" + std::to_string(empty_msgs.size()) +
			 " next=" + std::to_string(next2) +
			 " has_more=" + std::to_string(more2)).c_str());
		if (!(page_ok && empty_msgs.empty() && next2 == checkpoint2 && !more2)) all_ok = false;
	}

	cS.Close(); cR.Close();
	cleanup();
	pm.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// big-ids (64 位 message_id 链路)
//
// ALTER TABLE chat_message AUTO_INCREMENT=4294967300（>2^32）后收发一条文本，
// 断言 >32 位 message_id 以十进制字符串在 1302/1406 全链路无损、同步游标正常
// 推进；场景结束（含失败路径）恢复原 AUTO_INCREMENT。
// ---------------------------------------------------------------------------
bool ScenarioBigIds() {
	std::printf("\n=== scenario: big-ids ===\n");
	ProcessManager pm;
	Redis redis; Mysql mysql;
	std::string tag = RunTag();
	bool all_ok = true;
	const std::int64_t BIG_BASE = 4294967300LL;  // 2^32 + 4

	if (!redis.Connect(REDIS_HOST, REDIS_PORT, REDIS_PASSWD)) {
		Fail("big-ids: connect Redis", "redis connect failed"); return false;
	}
	if (!mysql.Connect(MYSQL_HOST, MYSQL_PORT, MYSQL_USER, MYSQL_PASSWD, MYSQL_SCHEMA)) {
		Fail("big-ids: connect MySQL", "mysql connect failed"); return false;
	}

	// 先删测试行（含大 id 行）再把 AUTO_INCREMENT 调回原值，顺序不能反：
	// 大 id 行还在时无法把 AUTO_INCREMENT 调到其之下。
	const long long orig_ai = mysql.GetChatMessageAutoIncrement();
	auto cleanup = [&] {
		CleanupFootprint(redis, mysql);
		if (orig_ai > 0) mysql.SetChatMessageAutoIncrement(orig_ai);
	};
	cleanup();

	if (orig_ai <= 0 || !mysql.SetChatMessageAutoIncrement(BIG_BASE)) {
		Fail("big-ids: raise AUTO_INCREMENT", "alter table failed");
		cleanup(); return false;
	}

	if (!pm.Start({ "StatusServer", { "StatusServer", "StatusServer.exe" },
		MakeStatusIni(), STATUS_GRPC_PORT }, 15000)) {
		Fail("big-ids: start StatusServer", "ready timeout"); cleanup(); return false;
	}
	if (!pm.Start({ "GateServer", { "GateServer", "GateServer.exe" },
		MakeGateIni(), GATE_HTTP_PORT }, 15000)) {
		Fail("big-ids: start GateServer", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}
	if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
		MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4),
		CHAT1_TCP_PORT }, 15000)) {
		Fail("big-ids: start ChatServer", "ready timeout"); cleanup(); pm.StopAll(); return false;
	}

	// receiver 取同步起点。
	TcpClient cR;
	if (!LoginUser(cR, RECEIVER_UID).ok) {
		Fail("big-ids: login receiver", "gate/chat login failed"); cleanup(); pm.StopAll(); return false;
	}
	std::uint64_t checkpoint = 0;
	if (!DoSyncBootstrap(cR, RECEIVER_UID, checkpoint)) {
		Fail("big-ids: bootstrap checkpoint", "1405/1406 bootstrap failed");
		cR.Close(); cleanup(); pm.StopAll(); return false;
	}

	// sender 发一条文本，1302 的 message_id 必须是 >32 位的十进制字符串。
	TcpClient cS;
	if (!LoginUser(cS, SENDER_UID).ok) {
		Fail("big-ids: login sender", "gate/chat login failed"); cR.Close(); cleanup(); pm.StopAll(); return false;
	}
	const std::string uid_str = "imtest-bigids-" + tag;
	cS.Send(ID_TEXT_CHAT_MSG_REQ, BuildTextReq(SENDER_UID, RECEIVER_UID, THREAD_ID,
		"big-ids-msg", uid_str));
	Frame rsp; bool got_rsp = cS.Wait(ID_TEXT_CHAT_MSG_RSP, 10000, &rsp);
	std::int64_t sent_mid = -1;
	bool mid_is_string = false;
	if (got_rsp) {
		auto j = ParseJson(rsp.body);
		if (j.is_object() && j.value("error", -1) == ERR_SUCCESS) {
			mid_is_string = j.contains("message_id") && j["message_id"].is_string();
			sent_mid = JsonIdStr(j, "message_id", -1);
		}
	}
	Check(got_rsp && mid_is_string && sent_mid >= BIG_BASE,
		"big-ids: 1302 message_id is >32-bit decimal string",
		("mid=" + std::to_string(sent_mid) +
		 " is_string=" + std::to_string(mid_is_string)).c_str());
	if (!(got_rsp && sent_mid >= BIG_BASE)) { cS.Close(); cR.Close(); cleanup(); pm.StopAll(); return false; }

	// 1302 的 thread_id 也是十进制字符串。
	{
		auto j = ParseJson(rsp.body);
		const bool tid_ok = j.contains("thread_id") && j["thread_id"].is_string() &&
			JsonIdStr(j, "thread_id", -1) == THREAD_ID;
		Check(tid_ok, "big-ids: 1302 thread_id is decimal string",
			tid_ok ? "ok" : "thread_id not string");
		if (!tid_ok) all_ok = false;
	}

	// DB 直读：64 位值无损落库。
	{
		auto rows = mysql.QueryByUniqueId(SENDER_UID, uid_str);
		Check(rows.size() == 1 && rows[0].message_id == sent_mid,
			"big-ids: DB row keeps >32-bit message_id",
			rows.empty() ? "no row" : ("mid=" + std::to_string(rows[0].message_id)).c_str());
		if (!(rows.size() == 1 && rows[0].message_id == sent_mid)) all_ok = false;
	}

	// 1406 同步流：大 id envelope 无损，sync_seq 游标正常推进。
	{
		std::vector<SyncEnvelope> msgs;
		std::uint64_t next_seq = checkpoint;
		bool has_more = false;
		bool synced = DoSyncPage(cR, RECEIVER_UID, checkpoint, 100, msgs, next_seq, has_more);
		bool found = false;
		std::uint64_t msg_seq = 0;
		for (const auto& m : msgs) {
			if (m.message_id == sent_mid) { found = true; msg_seq = m.sync_seq; break; }
		}
		Check(synced && found && msg_seq > checkpoint && next_seq == msg_seq,
			"big-ids: 1406 sync delivers big id, cursor advances",
			("synced=" + std::to_string(synced) + " found=" + std::to_string(found) +
			 " seq=" + std::to_string(msg_seq) +
			 " next=" + std::to_string(next_seq)).c_str());
		if (!(synced && found && msg_seq > checkpoint)) all_ok = false;
	}

	// 1407 ACK 大 id（字符串数组元素）→ 1408 成功。
	{
		bool ack_ok = cR.Send(ID_CHAT_DELIVERY_ACK_REQ, BuildAckReq(RECEIVER_UID, {sent_mid}));
		if (ack_ok) {
			Frame af;
			ack_ok = cR.Wait(ID_CHAT_DELIVERY_ACK_RSP, 10000, &af) &&
				ParseJson(af.body).value("error", -1) == ERR_SUCCESS;
		}
		Check(ack_ok, "big-ids: ACK big message_id succeeds", "ack failed");
		if (!ack_ok) all_ok = false;
	}

	cS.Close(); cR.Close();
	cleanup();
	pm.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// 资源消息场景群（统一传输改造）：共享的服务栈搭建 + 资源链路辅助
// ---------------------------------------------------------------------------
namespace {

struct ResStack {
	ProcessManager pm;
	Redis redis;
	Mysql mysql;

	//启动 Status/Gate/Chat/Resource 全栈；任一失败返回 false
	bool StartAll(const char* scenario) {
		if (!redis.Connect(REDIS_HOST, REDIS_PORT, REDIS_PASSWD)) {
			Fail(std::string(scenario) + ": connect Redis", "failed"); return false;
		}
		if (!mysql.Connect(MYSQL_HOST, MYSQL_PORT, MYSQL_USER, MYSQL_PASSWD, MYSQL_SCHEMA)) {
			Fail(std::string(scenario) + ": connect MySQL", "failed"); return false;
		}
		CleanupFootprint(redis, mysql);
		if (!pm.Start({ "StatusServer", { "StatusServer", "StatusServer.exe" },
			MakeStatusIni(), STATUS_GRPC_PORT }, 15000)) {
			Fail(std::string(scenario) + ": start StatusServer", "timeout"); return false;
		}
		if (!pm.Start({ "GateServer", { "GateServer", "GateServer.exe" },
			MakeGateIni(), GATE_HTTP_PORT }, 15000)) {
			Fail(std::string(scenario) + ": start GateServer", "timeout"); return false;
		}
		if (!pm.Start({ "chatserver1", { "chatserver1", "ChatServer.exe" },
			MakeChatIni("chatserver1", CHAT1_TCP_PORT, CHAT1_GRPC_PORT, 4),
			CHAT1_TCP_PORT }, 15000)) {
			Fail(std::string(scenario) + ": start ChatServer", "timeout"); return false;
		}
		if (!pm.Start({ "ResourceServer", { "ResourceServer", "ResourceServer.exe" },
			MakeResourceIni(), RESOURCE_HTTP_PORT }, 15000)) {
			Fail(std::string(scenario) + ": start ResourceServer", "timeout"); return false;
		}
		return true;
	}

	void StopAll() {
		CleanupFootprint(redis, mysql);
		pm.StopAll();
	}
};

//登录 ResourceServer 并返回已鉴权连接（失败返回空指针）
std::shared_ptr<ResClient> ResLogin(const std::string& token, int uid, const char* scenario) {
	auto res = std::make_shared<ResClient>();
	if (!res->Connect("127.0.0.1", RESOURCE_HTTP_PORT, 10000)) {
		Fail(std::string(scenario) + ": connect ResourceServer", "connect failed");
		return nullptr;
	}
	int ra = ResourceLogin(*res, uid, token);
	if (ra != ERR_SUCCESS) {
		Fail(std::string(scenario) + ": ResourceLogin", "err=" + std::to_string(ra));
		return nullptr;
	}
	return res;
}

//发 1503 创建资源消息并回 message_id；失败返回 -1（err_out 带出服务端错误码）
std::int64_t CreateResource(TcpClient& chat, int fromuid, int touid,
	const std::string& unique_id, int msg_type, const std::string& file_name,
	long long size, const std::string& content_hash, const std::string& mime,
	int* err_out) {
	chat.Send(ID_CREATE_RESOURCE_MSG_REQ,
		BuildResourceCreateReq(fromuid, touid, THREAD_ID, unique_id, msg_type,
			file_name, size, content_hash, mime));
	Frame rsp;
	if (!chat.Wait(ID_CREATE_RESOURCE_MSG_RSP, 10000, &rsp)) {
		if (err_out) *err_out = -2;
		return -1;
	}
	auto j = ParseJson(rsp.body);
	const int err = j.is_object() ? j.value("error", -3) : -3;
	if (err_out) *err_out = err;
	if (err != ERR_SUCCESS) return -1;
	return JsonIdStr(j, "message_id", -1);
}

//上传 [from_offset, total) 区间全部分片；返回最后一片 1508 的 error（ready_out
//带回 resource_status）。遇到非 0 error 即停。
int UploadChunks(ResClient& res, std::int64_t msg_id, const std::string& blob,
	long long from_offset, int* ready_out) {
	const long long total = static_cast<long long>(blob.size());
	int err = ERR_SUCCESS;
	int rs = RESOURCE_UPLOADING;
	for (long long off = from_offset; off < total; ) {
		const long long n = (std::min<long long>)(total - off, 32768);
		const std::string chunk = blob.substr(static_cast<std::size_t>(off),
			static_cast<std::size_t>(n));
		if (!res.Send(ID_RESOURCE_CHUNK_UPLOAD_REQ,
			BuildChunkUploadReq(msg_id, off, llfc::Sha256Hex(chunk),
				Base64Encode(chunk)))) {
			return -4;
		}
		Frame uf;
		if (!res.Wait(ID_RESOURCE_CHUNK_UPLOAD_RSP, 10000, &uf)) return -5;
		auto j = ParseJson(uf.body);
		err = j.is_object() ? j.value("error", -6) : -6;
		rs = j.is_object() ? j.value("resource_status", -1) : -1;
		if (err != ERR_SUCCESS) break;
		off += n;
	}
	if (ready_out) *ready_out = rs;
	return err;
}

//单片上传（供越界/篡改/重复片场景）；回包 error 与 server_offset 带出
int UploadOneChunk(ResClient& res, std::int64_t msg_id, long long offset,
	const std::string& chunk, const std::string& chunk_sha, long long* server_offset) {
	res.Send(ID_RESOURCE_CHUNK_UPLOAD_REQ,
		BuildChunkUploadReq(msg_id, offset, chunk_sha, Base64Encode(chunk)));
	Frame uf;
	if (!res.Wait(ID_RESOURCE_CHUNK_UPLOAD_RSP, 10000, &uf)) return -4;
	auto j = ParseJson(uf.body);
	if (server_offset) {
		*server_offset = j.is_object()
			? static_cast<long long>(JsonIdStr(j, "server_offset", -1)) : -1;
	}
	return j.is_object() ? j.value("error", -6) : -6;
}

//1509 查询服务端偏移；失败返回 -1
long long QueryServerOffset(ResClient& res, std::int64_t msg_id, int* status_out) {
	res.Send(ID_RESOURCE_UPLOAD_PROGRESS_REQ, BuildUploadProgressReq(msg_id));
	Frame f;
	if (!res.Wait(ID_RESOURCE_UPLOAD_PROGRESS_RSP, 10000, &f)) return -1;
	auto j = ParseJson(f.body);
	if (!j.is_object() || j.value("error", -1) != ERR_SUCCESS) return -1;
	if (status_out) *status_out = j.value("resource_status", -1);
	return static_cast<long long>(JsonIdStr(j, "server_offset", -1));
}

//按偏移逐片下载完整资源；返回拼装内容（失败返回空串；total 带出总大小）
std::string DownloadWhole(ResClient& res, std::int64_t msg_id, long long total) {
	std::string out;
	for (long long off = 0; off < total; ) {
		res.Send(ID_RESOURCE_CHUNK_DOWN_REQ, BuildChunkDownReq(msg_id, off));
		Frame f;
		if (!res.Wait(ID_RESOURCE_CHUNK_DOWN_RSP, 10000, &f)) return std::string();
		auto j = ParseJson(f.body);
		if (!j.is_object() || j.value("error", -1) != ERR_SUCCESS) return std::string();
		const std::string chunk = Base64Decode(j.value("data", ""));
		//逐片校验：服务端随片下发的 SHA-256 必须匹配
		if (llfc::Sha256Hex(chunk) != j.value("chunk_sha256", "")) return std::string();
		out += chunk;
		off += static_cast<long long>(chunk.size());
	}
	return out;
}

} // namespace

// ---------------------------------------------------------------------------
// resource-create：1503 校验链 —— 合法创建 / 超限 2020 / 坏哈希 2019 /
// 重复 unique_id 幂等同 id / 内容冲突 2017 / 伪造 fromuid 拒绝 / 非成员会话拒绝
// ---------------------------------------------------------------------------
bool ScenarioResourceCreate() {
	std::printf("\n=== scenario: resource-create ===\n");
	ResStack stack;
	bool all_ok = true;
	if (!stack.StartAll("resource-create")) { stack.StopAll(); return false; }

	TcpClient cS;
	auto li = LoginUser(cS, SENDER_UID);
	if (!li.ok) {
		Fail("resource-create: login sender", "failed"); stack.StopAll(); return false;
	}

	const std::string tag = RunTag();
	const std::string blob = MakeBlob(40000);
	const std::string good_hash = llfc::Sha256Hex(blob);
	const std::string name = "create_" + tag + ".png";

	//1) 合法创建
	int err = -1;
	const std::int64_t mid = CreateResource(cS, SENDER_UID, RECEIVER_UID,
		"imtest-ok-" + tag, MSG_TYPE_PIC, name, (long long)blob.size(),
		good_hash, "image/png", &err);
	Check(err == ERR_SUCCESS && mid > 0, "resource-create: valid create succeeds",
		("err=" + std::to_string(err)).c_str());
	if (mid <= 0) all_ok = false;

	//2) 重复 unique_id 同内容 → 幂等返回同一 message_id
	{
		int err2 = -1;
		const std::int64_t mid2 = CreateResource(cS, SENDER_UID, RECEIVER_UID,
			"imtest-ok-" + tag, MSG_TYPE_PIC, name, (long long)blob.size(),
			good_hash, "image/png", &err2);
		Check(err2 == ERR_SUCCESS && mid2 == mid,
			"resource-create: duplicate unique_id is idempotent (same message_id)",
			("err=" + std::to_string(err2) + " mid2=" + std::to_string(mid2)).c_str());
		if (mid2 != mid) all_ok = false;
	}

	//3) 重复 unique_id 不同内容 → MESSAGE_CONFLICT
	{
		int err3 = -1;
		const std::int64_t mid3 = CreateResource(cS, SENDER_UID, RECEIVER_UID,
			"imtest-ok-" + tag, MSG_TYPE_PIC, "other_" + name, 1234,
			good_hash, "image/png", &err3);
		Check(err3 == ERR_MESSAGE_CONFLICT && mid3 < 0,
			"resource-create: same unique_id different content -> 2017 conflict",
			("err=" + std::to_string(err3)).c_str());
		if (err3 != ERR_MESSAGE_CONFLICT) all_ok = false;
	}

	//4) 坏哈希（非 64 位 hex）→ ResourceInvalid(2019)
	{
		int err4 = -1;
		CreateResource(cS, SENDER_UID, RECEIVER_UID, "imtest-badhash-" + tag,
			MSG_TYPE_PIC, name, (long long)blob.size(), "not-a-sha256", "image/png", &err4);
		Check(err4 == ERR_RESOURCE_INVALID, "resource-create: bad hash -> 2019",
			("err=" + std::to_string(err4)).c_str());
		if (err4 != ERR_RESOURCE_INVALID) all_ok = false;
	}

	//5) 超限（图片 20MB+1）→ ResourceSizeExceeded(2020)
	{
		int err5 = -1;
		CreateResource(cS, SENDER_UID, RECEIVER_UID, "imtest-oversize-" + tag,
			MSG_TYPE_PIC, name, 20LL * 1024 * 1024 + 1, good_hash, "image/png", &err5);
		Check(err5 == ERR_RESOURCE_SIZE_EXCEEDED, "resource-create: oversize -> 2020",
			("err=" + std::to_string(err5)).c_str());
		if (err5 != ERR_RESOURCE_SIZE_EXCEEDED) all_ok = false;
	}

	//6) 伪造 fromuid（session 是 SENDER，JSON 里写 RECEIVER）→ 拒绝
	{
		int err6 = -1;
		CreateResource(cS, RECEIVER_UID, SENDER_UID, "imtest-forge-" + tag,
			MSG_TYPE_PIC, name, (long long)blob.size(), good_hash, "image/png", &err6);
		Check(err6 == ERR_UID_INVALID, "resource-create: forged fromuid -> 2011",
			("err=" + std::to_string(err6)).c_str());
		if (err6 != ERR_UID_INVALID) all_ok = false;
	}

	//7) 非成员会话（fixture 中不存在的 thread）→ CREATE_CHAT_FAILED
	{
		const std::int64_t bogus_thread = 99999999;
		cS.Send(ID_CREATE_RESOURCE_MSG_REQ,
			BuildResourceCreateReq(SENDER_UID, RECEIVER_UID, bogus_thread,
				"imtest-badthread-" + tag, MSG_TYPE_PIC, name,
				(long long)blob.size(), good_hash, "image/png"));
		Frame rsp;
		bool got = cS.Wait(ID_CREATE_RESOURCE_MSG_RSP, 10000, &rsp);
		int err7 = got ? ParseJson(rsp.body).value("error", -3) : -2;
		Check(got && err7 == 2012, "resource-create: non-member thread -> 2012",
			("err=" + std::to_string(err7)).c_str());
		if (err7 != 2012) all_ok = false;
	}

	cS.Close();
	stack.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// resource-upload：多分片上传 → 1509 查询进度 → 1511 下载信息 → 逐片下载
// 逐字节一致（覆盖整文件 SHA-256 校验通过路径）
// ---------------------------------------------------------------------------
bool ScenarioResourceUpload() {
	std::printf("\n=== scenario: resource-upload ===\n");
	ResStack stack;
	bool all_ok = true;
	if (!stack.StartAll("resource-upload")) { stack.StopAll(); return false; }

	TcpClient cS;
	auto li = LoginUser(cS, SENDER_UID);
	if (!li.ok) {
		Fail("resource-upload: login sender", "failed"); stack.StopAll(); return false;
	}

	const std::string tag = RunTag();
	const std::string blob = MakeBlob(100000);  // 4 片（3×32KiB + 2176B）
	const std::string hash = llfc::Sha256Hex(blob);
	const std::int64_t mid = CreateResource(cS, SENDER_UID, RECEIVER_UID,
		"imtest-up-" + tag, MSG_TYPE_FILE, "upload_" + tag + ".bin",
		(long long)blob.size(), hash, "application/octet-stream", nullptr);
	Check(mid > 0, "resource-upload: create resource msg", ("mid=" + std::to_string(mid)).c_str());
	if (mid <= 0) { cS.Close(); stack.StopAll(); return false; }

	auto res = ResLogin(li.token, SENDER_UID, "resource-upload");
	if (!res) { cS.Close(); stack.StopAll(); return false; }

	int rs = -1;
	const int up_err = UploadChunks(*res, mid, blob, 0, &rs);
	Check(up_err == ERR_SUCCESS && rs == RESOURCE_READY,
		"resource-upload: all chunks uploaded -> Ready",
		("err=" + std::to_string(up_err) + " rs=" + std::to_string(rs)).c_str());
	if (rs != RESOURCE_READY) all_ok = false;

	//1509：就绪后 server_offset == total
	{
		int st = -1;
		const long long off = QueryServerOffset(*res, mid, &st);
		Check(off == (long long)blob.size() && st == RESOURCE_READY,
			"resource-upload: 1509 reports total_size & Ready",
			("off=" + std::to_string(off) + " st=" + std::to_string(st)).c_str());
		if (off != (long long)blob.size()) all_ok = false;
	}

	//1511（下载信息）：发送者与接收者都可查（接收方需先取得自己的 token，
	//ResourceLogin 校验 utoken_<uid>）
	TcpClient cR;
	auto lr = LoginUser(cR, RECEIVER_UID);
	std::shared_ptr<ResClient> res_recv;
	if (lr.ok) {
		res_recv = ResLogin(lr.token, RECEIVER_UID, "resource-upload");
	}
	for (ResClient* rc : { res.get(), res_recv.get() }) {
		if (!rc) continue;
		rc->Send(ID_RESOURCE_DOWN_INFO_REQ, BuildDownInfoReq(mid));
		Frame f;
		bool ok = rc->Wait(ID_RESOURCE_DOWN_INFO_RSP, 10000, &f);
		auto j = ok ? ParseJson(f.body) : json();
		bool good = ok && j.is_object() && j.value("error", -1) == ERR_SUCCESS
			&& j.value("content_hash", "") == hash
			&& j.value("file_name", "") == "upload_" + tag + ".bin";
		Check(good, "resource-upload: 1511 down info (name/hash match)",
			ok ? j.dump() : "no rsp");
		if (!good) all_ok = false;
	}

	//1513：逐片下载并与源逐字节比对
	if (res_recv) {
		const std::string got = DownloadWhole(*res_recv, mid, (long long)blob.size());
		Check(got == blob, "resource-upload: downloaded bytes identical",
			("got=" + std::to_string(got.size()) + " want="
				+ std::to_string(blob.size())).c_str());
		if (got != blob) all_ok = false;
	}

	if (res_recv) res_recv->Close();
	res->Close();
	cS.Close(); cR.Close();
	stack.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// resource-resume：上传中途 kill ResourceServer 重启 → .part 保留 →
// 1509 返回非零偏移 → 从该偏移续传成功，已确认内容不重传
// ---------------------------------------------------------------------------
bool ScenarioResourceResume() {
	std::printf("\n=== scenario: resource-resume ===\n");
	ResStack stack;
	bool all_ok = true;
	if (!stack.StartAll("resource-resume")) { stack.StopAll(); return false; }

	TcpClient cS;
	auto li = LoginUser(cS, SENDER_UID);
	if (!li.ok) {
		Fail("resource-resume: login sender", "failed"); stack.StopAll(); return false;
	}

	const std::string tag = RunTag();
	const std::string blob = MakeBlob(100000);
	const std::string hash = llfc::Sha256Hex(blob);
	const std::int64_t mid = CreateResource(cS, SENDER_UID, RECEIVER_UID,
		"imtest-resume-" + tag, MSG_TYPE_FILE, "resume_" + tag + ".bin",
		(long long)blob.size(), hash, "application/octet-stream", nullptr);
	Check(mid > 0, "resource-resume: create resource msg", ("mid=" + std::to_string(mid)).c_str());
	if (mid <= 0) { cS.Close(); stack.StopAll(); return false; }

	//上传前 2 片（65536 字节）
	auto res = ResLogin(li.token, SENDER_UID, "resource-resume");
	if (!res) { cS.Close(); stack.StopAll(); return false; }
	{
		int rs = -1;
		const int err = UploadChunks(*res, mid, blob.substr(0, 65536), 0, &rs);
		Check(err == ERR_SUCCESS, "resource-resume: first 2 chunks uploaded",
			("err=" + std::to_string(err)).c_str());
		if (err != ERR_SUCCESS) all_ok = false;
	}
	res->Close();

	//杀掉 ResourceServer 再拉起（同一持久化输出目录：harness 固定 cwd 复用）
	stack.pm.StopOne("ResourceServer");
	if (!stack.pm.Start({ "ResourceServer", { "ResourceServer", "ResourceServer.exe" },
		MakeResourceIni(), RESOURCE_HTTP_PORT }, 15000)) {
		Fail("resource-resume: restart ResourceServer", "timeout");
		cS.Close(); stack.StopAll(); return false;
	}

	//1509：重启后服务端按 .part 实际长度回 65536
	auto res2 = ResLogin(li.token, SENDER_UID, "resource-resume");
	if (!res2) { cS.Close(); stack.StopAll(); return false; }
	const long long offset = QueryServerOffset(*res2, mid, nullptr);
	Check(offset == 65536, "resource-resume: 1509 after restart reports 65536",
		("offset=" + std::to_string(offset)).c_str());
	if (offset != 65536) all_ok = false;

	//从服务端偏移续传剩余分片（不重传前 2 片）
	int rs = -1;
	const int err = UploadChunks(*res2, mid, blob, offset > 0 ? offset : 0, &rs);
	Check(err == ERR_SUCCESS && rs == RESOURCE_READY,
		"resource-resume: resume from server offset completes",
		("err=" + std::to_string(err) + " rs=" + std::to_string(rs)).c_str());
	if (rs != RESOURCE_READY) all_ok = false;

	//整文件内容一致性（1513 全量下载比对）
	const std::string got = DownloadWhole(*res2, mid, (long long)blob.size());
	Check(got == blob, "resource-resume: resumed upload is byte-identical",
		("got=" + std::to_string(got.size())).c_str());
	if (got != blob) all_ok = false;

	res2->Close();
	cS.Close();
	stack.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// resource-idempotent：模拟 1508 响应丢失后客户端重发已确认分片，
// 服务端幂等确认且不追加第二次（.part 长度不翻倍）
// ---------------------------------------------------------------------------
bool ScenarioResourceIdempotent() {
	std::printf("\n=== scenario: resource-idempotent ===\n");
	ResStack stack;
	bool all_ok = true;
	if (!stack.StartAll("resource-idempotent")) { stack.StopAll(); return false; }

	TcpClient cS;
	auto li = LoginUser(cS, SENDER_UID);
	if (!li.ok) {
		Fail("resource-idempotent: login sender", "failed"); stack.StopAll(); return false;
	}

	const std::string tag = RunTag();
	const std::string blob = MakeBlob(65536);  // 恰 2 片
	const std::string hash = llfc::Sha256Hex(blob);
	const std::int64_t mid = CreateResource(cS, SENDER_UID, RECEIVER_UID,
		"imtest-idem-" + tag, MSG_TYPE_FILE, "idem_" + tag + ".bin",
		(long long)blob.size(), hash, "application/octet-stream", nullptr);
	Check(mid > 0, "resource-idempotent: create", ("mid=" + std::to_string(mid)).c_str());
	if (mid <= 0) { cS.Close(); stack.StopAll(); return false; }

	auto res = ResLogin(li.token, SENDER_UID, "resource-idempotent");
	if (!res) { cS.Close(); stack.StopAll(); return false; }

	const std::string chunk0 = blob.substr(0, 32768);
	const std::string sha0 = llfc::Sha256Hex(chunk0);

	//第一片正常上传
	long long so = -1;
	int err = UploadOneChunk(*res, mid, 0, chunk0, sha0, &so);
	Check(err == ERR_SUCCESS && so == 32768, "resource-idempotent: chunk0 accepted",
		("err=" + std::to_string(err) + " so=" + std::to_string(so)).c_str());

	//模拟响应丢失：原样重发同一片（同 offset 同哈希）→ 幂等成功，server_offset 不翻倍
	err = UploadOneChunk(*res, mid, 0, chunk0, sha0, &so);
	Check(err == ERR_SUCCESS && so == 32768,
		"resource-idempotent: duplicate chunk0 acked without re-append",
		("err=" + std::to_string(err) + " so=" + std::to_string(so)).c_str());
	if (so != 32768) all_ok = false;

	//1509 交叉验证磁盘真值
	const long long disk_off = QueryServerOffset(*res, mid, nullptr);
	Check(disk_off == 32768, "resource-idempotent: 1509 agrees (no double write)",
		("offset=" + std::to_string(disk_off)).c_str());

	//第二片完成后整文件就绪
	int rs = -1;
	err = UploadChunks(*res, mid, blob, 32768, &rs);
	Check(err == ERR_SUCCESS && rs == RESOURCE_READY,
		"resource-idempotent: completes after duplicate chunk",
		("err=" + std::to_string(err) + " rs=" + std::to_string(rs)).c_str());
	if (rs != RESOURCE_READY) all_ok = false;

	res->Close();
	cS.Close();
	stack.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// resource-corrupt：错误分片哈希被拒（2112，偏移不前进）；整文件哈希不符时
// 服务端删 .part 并要求从 0 重传（响应 server_offset=0）
// ---------------------------------------------------------------------------
bool ScenarioResourceCorrupt() {
	std::printf("\n=== scenario: resource-corrupt ===\n");
	ResStack stack;
	bool all_ok = true;
	if (!stack.StartAll("resource-corrupt")) { stack.StopAll(); return false; }

	TcpClient cS;
	auto li = LoginUser(cS, SENDER_UID);
	if (!li.ok) {
		Fail("resource-corrupt: login sender", "failed"); stack.StopAll(); return false;
	}

	const std::string tag = RunTag();
	const std::string blob = MakeBlob(65536);
	//content_hash 故意写成另一份数据的哈希：分片校验全对、整文件校验必败
	const std::string wrong_whole_hash = llfc::Sha256Hex(MakeBlob(1));
	const std::int64_t mid = CreateResource(cS, SENDER_UID, RECEIVER_UID,
		"imtest-corrupt-" + tag, MSG_TYPE_FILE, "corrupt_" + tag + ".bin",
		(long long)blob.size(), wrong_whole_hash, "application/octet-stream", nullptr);
	Check(mid > 0, "resource-corrupt: create", ("mid=" + std::to_string(mid)).c_str());
	if (mid <= 0) { cS.Close(); stack.StopAll(); return false; }

	auto res = ResLogin(li.token, SENDER_UID, "resource-corrupt");
	if (!res) { cS.Close(); stack.StopAll(); return false; }

	//1) 分片哈希错误（数据与声明哈希不符）→ 2112 且 server_offset 不前进
	long long so = -1;
	int err = UploadOneChunk(*res, mid, 0, blob.substr(0, 32768),
		llfc::Sha256Hex(MakeBlob(2)), &so);
	Check(err == ERR_RS_HASH_MISMATCH && so == 0,
		"resource-corrupt: bad chunk hash -> 2112, offset stays 0",
		("err=" + std::to_string(err) + " so=" + std::to_string(so)).c_str());
	if (err != ERR_RS_HASH_MISMATCH) all_ok = false;

	//2) 正确分片全部传完 → 完成点整文件 SHA-256 与 content_hash 不符 →
	//   2112 且服务端清掉 .part（server_offset=0，客户端从 0 重传）
	err = UploadChunks(*res, mid, blob, 0, nullptr);
	Check(err == ERR_RS_HASH_MISMATCH,
		"resource-corrupt: whole-file mismatch -> 2112",
		("err=" + std::to_string(err)).c_str());
	const long long after = QueryServerOffset(*res, mid, nullptr);
	Check(after == 0, "resource-corrupt: server_offset reset to 0 after whole-file mismatch",
		("offset=" + std::to_string(after)).c_str());
	if (err != ERR_RS_HASH_MISMATCH || after != 0) all_ok = false;

	res->Close();
	cS.Close();
	stack.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// resource-perm：非 sender 会话上传被拒（2115）；越界偏移下载被拒（2107）。
// 1511/1513 的会话成员校验由 ResourceServer 统一执行（sender/recv 之外的
// session uid 一律 ResourceForbidden）。注：fixture 只有 2 个用户，发送方/
// 接收方之外的“纯第三方”下载拒绝由同一校验逻辑覆盖（上传侧 2115 已验证）。
// ---------------------------------------------------------------------------
bool ScenarioResourcePerm() {
	std::printf("\n=== scenario: resource-perm ===\n");
	ResStack stack;
	bool all_ok = true;
	if (!stack.StartAll("resource-perm")) { stack.StopAll(); return false; }

	TcpClient cS;
	auto li = LoginUser(cS, SENDER_UID);
	if (!li.ok) {
		Fail("resource-perm: login sender", "failed"); stack.StopAll(); return false;
	}

	const std::string tag = RunTag();
	const std::string blob = MakeBlob(32768);
	const std::string hash = llfc::Sha256Hex(blob);
	const std::int64_t mid = CreateResource(cS, SENDER_UID, RECEIVER_UID,
		"imtest-perm-" + tag, MSG_TYPE_FILE, "perm_" + tag + ".bin",
		(long long)blob.size(), hash, "application/octet-stream", nullptr);
	Check(mid > 0, "resource-perm: create", ("mid=" + std::to_string(mid)).c_str());
	if (mid <= 0) { cS.Close(); stack.StopAll(); return false; }

	//发送者完成上传（资源就绪）
	auto res = ResLogin(li.token, SENDER_UID, "resource-perm");
	if (!res) { cS.Close(); stack.StopAll(); return false; }
	int rs = -1;
	UploadChunks(*res, mid, blob, 0, &rs);
	Check(rs == RESOURCE_READY, "resource-perm: upload completes",
		("rs=" + std::to_string(rs)).c_str());
	res->Close();

	//权限校验覆盖：接收者 1511 允许（正向）；越界偏移 1513 → 2107；
	//非 sender 会话向他人消息上传分片 → 2115（ResourceForbidden）。
	TcpClient cR;
	auto lr = LoginUser(cR, RECEIVER_UID);
	if (!lr.ok) {
		Fail("resource-perm: login receiver", "failed"); cS.Close(); stack.StopAll(); return false;
	}
	auto res_r = ResLogin(lr.token, RECEIVER_UID, "resource-perm");
	if (res_r) {
		//合法接收者 1511 成功
		res_r->Send(ID_RESOURCE_DOWN_INFO_REQ, BuildDownInfoReq(mid));
		Frame f;
		bool ok = res_r->Wait(ID_RESOURCE_DOWN_INFO_RSP, 10000, &f);
		const int err = ok ? ParseJson(f.body).value("error", -3) : -2;
		Check(ok && err == ERR_SUCCESS, "resource-perm: receiver 1511 allowed",
			("err=" + std::to_string(err)).c_str());

		//越界偏移 1513（total 只有 32768，offset=65536）→ 2107
		res_r->Send(ID_RESOURCE_CHUNK_DOWN_REQ, BuildChunkDownReq(mid, 65536));
		ok = res_r->Wait(ID_RESOURCE_CHUNK_DOWN_RSP, 10000, &f);
		const int err2 = ok ? ParseJson(f.body).value("error", -3) : -2;
		Check(ok && err2 == ERR_RS_OFFSET_INVALID,
			"resource-perm: out-of-range offset -> 2107",
			("err=" + std::to_string(err2)).c_str());
		if (err2 != ERR_RS_OFFSET_INVALID) all_ok = false;
		res_r->Close();
	}

	//接收者会话尝试向「发送者创建的消息」上传分片（session uid=RECEIVER != sender）
	{
		//先建一条 receiver 自己的未完成消息，再让 sender 会话来上传 → 2115
		TcpClient cS2;
		auto li2 = LoginUser(cS2, RECEIVER_UID);
		std::int64_t mid2 = -1;
		if (li2.ok) {
			const std::string blob2 = MakeBlob(32768);
			mid2 = CreateResource(cS2, RECEIVER_UID, SENDER_UID,
				"imtest-perm2-" + tag, MSG_TYPE_FILE, "perm2_" + tag + ".bin",
				(long long)blob2.size(), llfc::Sha256Hex(blob2),
				"application/octet-stream", nullptr);
		}
		Check(mid2 > 0, "resource-perm: second message created",
			("mid2=" + std::to_string(mid2)).c_str());
		if (mid2 > 0) {
			auto res_s = ResLogin(li.token, SENDER_UID, "resource-perm");
			if (res_s) {
				const std::string chunk = MakeBlob(32768);
				long long so = -1;
				const int err3 = UploadOneChunk(*res_s, mid2, 0, chunk,
					llfc::Sha256Hex(chunk), &so);
				Check(err3 == ERR_RS_FORBIDDEN,
					"resource-perm: non-sender upload -> 2115",
					("err=" + std::to_string(err3)).c_str());
				if (err3 != ERR_RS_FORBIDDEN) all_ok = false;
				res_s->Close();
			}
		}
		cS2.Close();
	}

	cS.Close(); cR.Close();
	stack.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// resource-offset：跳过中间分片直接发靠后 offset → 2107 且响应携带服务端
// 真实 server_offset，客户端据此对齐后续传成功
// ---------------------------------------------------------------------------
bool ScenarioResourceOffset() {
	std::printf("\n=== scenario: resource-offset ===\n");
	ResStack stack;
	bool all_ok = true;
	if (!stack.StartAll("resource-offset")) { stack.StopAll(); return false; }

	TcpClient cS;
	auto li = LoginUser(cS, SENDER_UID);
	if (!li.ok) {
		Fail("resource-offset: login sender", "failed"); stack.StopAll(); return false;
	}

	const std::string tag = RunTag();
	const std::string blob = MakeBlob(98304);  // 3 片
	const std::string hash = llfc::Sha256Hex(blob);
	const std::int64_t mid = CreateResource(cS, SENDER_UID, RECEIVER_UID,
		"imtest-off-" + tag, MSG_TYPE_FILE, "off_" + tag + ".bin",
		(long long)blob.size(), hash, "application/octet-stream", nullptr);
	Check(mid > 0, "resource-offset: create", ("mid=" + std::to_string(mid)).c_str());
	if (mid <= 0) { cS.Close(); stack.StopAll(); return false; }

	auto res = ResLogin(li.token, SENDER_UID, "resource-offset");
	if (!res) { cS.Close(); stack.StopAll(); return false; }

	//跳片：第一片直接发 offset=32768 → 2107 + server_offset=0
	long long so = -1;
	int err = UploadOneChunk(*res, mid, 32768, blob.substr(32768, 32768),
		llfc::Sha256Hex(blob.substr(32768, 32768)), &so);
	Check(err == ERR_RS_OFFSET_INVALID && so == 0,
		"resource-offset: skip-ahead chunk -> 2107 with server_offset=0",
		("err=" + std::to_string(err) + " so=" + std::to_string(so)).c_str());
	if (err != ERR_RS_OFFSET_INVALID) all_ok = false;

	//按 server_offset 对齐后从 0 顺序上传 → 完成
	int rs = -1;
	err = UploadChunks(*res, mid, blob, 0, &rs);
	Check(err == ERR_SUCCESS && rs == RESOURCE_READY,
		"resource-offset: aligned re-upload completes",
		("err=" + std::to_string(err) + " rs=" + std::to_string(rs)).c_str());
	if (rs != RESOURCE_READY) all_ok = false;

	res->Close();
	cS.Close();
	stack.StopAll();
	return all_ok;
}

// ---------------------------------------------------------------------------
// resource-expiry：创建后只传一片，回拨 updated_at 到 8 天前并 touch .part
// mtime 为 8 天前，重启 ResourceServer 触发启动清理 → resource_status=2、
// 双方 sync 行补齐、.part 被删除
// ---------------------------------------------------------------------------
bool ScenarioResourceExpiry() {
	std::printf("\n=== scenario: resource-expiry ===\n");
	ResStack stack;
	bool all_ok = true;
	if (!stack.StartAll("resource-expiry")) { stack.StopAll(); return false; }

	TcpClient cS;
	auto li = LoginUser(cS, SENDER_UID);
	if (!li.ok) {
		Fail("resource-expiry: login sender", "failed"); stack.StopAll(); return false;
	}

	const std::string tag = RunTag();
	const std::string blob = MakeBlob(98304);
	const std::string hash = llfc::Sha256Hex(blob);
	const std::int64_t mid = CreateResource(cS, SENDER_UID, RECEIVER_UID,
		"imtest-expiry-" + tag, MSG_TYPE_FILE, "expiry_" + tag + ".bin",
		(long long)blob.size(), hash, "application/octet-stream", nullptr);
	Check(mid > 0, "resource-expiry: create", ("mid=" + std::to_string(mid)).c_str());
	if (mid <= 0) { cS.Close(); stack.StopAll(); return false; }

	//只传第一片（产生 .part 但不完成）
	auto res = ResLogin(li.token, SENDER_UID, "resource-expiry");
	if (!res) { cS.Close(); stack.StopAll(); return false; }
	{
		int rs = -1;
		UploadChunks(*res, mid, blob.substr(0, 32768), 0, &rs);
	}
	res->Close();

	//.part 实际路径（harness cwd = 工作目录，ConfigMgr 在其下建 bin/resource）
	namespace fs = boost::filesystem;
	const fs::path part_path = fs::path(stack.pm.base_dir()) / "ResourceServer"
		/ "bin" / "resource" / std::to_string(SENDER_UID)
		/ (std::to_string(mid) + ".part");
	boost::system::error_code fs_ec;
	const bool part_exists = fs::exists(part_path, fs_ec);
	Check(part_exists, "resource-expiry: .part exists before cleanup",
		part_path.string().c_str());

	//回拨 DB updated_at 与文件 mtime 到 8 天前（7 天保留期之外）
	stack.mysql.BackdateMessageUpdatedAt(mid, 8);
	fs::last_write_time(part_path, std::time(nullptr) - 8LL * 24 * 3600, fs_ec);

	//重启 ResourceServer → 启动清理立即执行
	stack.pm.StopOne("ResourceServer");
	if (!stack.pm.Start({ "ResourceServer", { "ResourceServer", "ResourceServer.exe" },
		MakeResourceIni(), RESOURCE_HTTP_PORT }, 15000)) {
		Fail("resource-expiry: restart ResourceServer", "timeout");
		cS.Close(); stack.StopAll(); return false;
	}

	//清理是启动回调里的同步逻辑，等服务完全起来后稍等即可断言
	std::this_thread::sleep_for(std::chrono::milliseconds(800));
	bool expired = false;
	for (int poll = 0; poll < 30 && !expired; ++poll) {
		auto rows = stack.mysql.QueryByMessageId(mid);
		if (!rows.empty() && rows[0].resource_status == RESOURCE_EXPIRED) expired = true;
		else std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
	Check(expired, "resource-expiry: stale message marked Expired(2)",
		"resource_status != 2 after cleanup");
	if (!expired) all_ok = false;

	const bool part_gone = !fs::exists(part_path, fs_ec);
	Check(part_gone, "resource-expiry: stale .part removed", part_path.string().c_str());
	if (!part_gone) all_ok = false;

	//失败终态也进同步流：双方 user_message_sync 行存在（客户端据此展示已过期）
	{
		const auto sync_rows = stack.mysql.QuerySyncRows(RECEIVER_UID, 0, 0);
		bool present = false;
		for (const auto& r : sync_rows) {
			if (r.message_id == (std::uint64_t)mid) { present = true; break; }
		}
		Check(present, "resource-expiry: sync row written for expired resource",
			present ? "ok" : "absent");
		if (!present) all_ok = false;
	}

	//过期资源继续上传被拒（2116）
	auto res2 = ResLogin(li.token, SENDER_UID, "resource-expiry");
	if (res2 && expired) {
		const std::string chunk = blob.substr(0, 32768);
		long long so = -1;
		const int err = UploadOneChunk(*res2, mid, 32768, chunk,
			llfc::Sha256Hex(chunk), &so);
		Check(err == ERR_RS_STATE_INVALID,
			"resource-expiry: upload to expired resource -> 2116",
			("err=" + std::to_string(err)).c_str());
		if (err != ERR_RS_STATE_INVALID) all_ok = false;
		res2->Close();
	}

	cS.Close();
	stack.StopAll();
	return all_ok;
}

} // namespace imt
