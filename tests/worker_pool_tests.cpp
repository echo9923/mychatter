// worker_pool_tests — Verification.1
//
// Deterministic concurrency-semantics tests for the two production worker
// pools introduced by the IM concurrency plan:
//   - ChatServer  LogicWorker      (single-thread FIFO shard, plan 1.1)
//   - GateServer  HandlerExecutor  (bounded multi-thread pool, plan 2.1)
//
// Design rules (from the plan):
//   * No test framework. Plain main() runs four case groups in order; every
//     assertion prints [PASS]/[FAIL] plus case name and expected/actual
//     diagnostics; any failure makes main() exit(1).
//   * Synchronization primitives are hand-rolled on top of
//     <thread>/<mutex>/<condition_variable>/<atomic> (C++17 has no
//     std::barrier / std::counting_semaphore).
//   * Deadlock detection uses condition_variable::wait_for with a generous
//     timeout. This is NOT a wall-clock performance assertion: the only way
//     the arrival predicate can stay unsatisfied is if fewer worker threads
//     exist than concurrent tasks, which is a genuine permanent deadlock.
//     A generous timeout therefore never false-fails on slow hardware but
//     reliably fires on a real single-thread regression.
//
// The test compiles the production LogicWorker.cpp and HandlerExecutor.cpp
// directly (see tests/CMakeLists.txt) with no link dependency on either
// server target, so it runs on any build without Redis/MySQL/gRPC.

#include "LogicWorker.h"       // server/ChatServer/include
#include "HandlerExecutor.h"   // server/GateServer/include

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// Deadlock-detector timeout. Only reached when a concurrency invariant is
// actually violated (fewer live worker threads than required); never reached
// on a correct implementation regardless of hardware speed.
constexpr auto kDeadlockTimeout = std::chrono::seconds(10);

// ---------------------------------------------------------------------------
// Hand-rolled synchronization primitives (C++17 has no std::barrier).
// ---------------------------------------------------------------------------

// One-shot phase barrier: N threads call ArriveAndWait(); every caller blocks
// until the Nth arrives, then all are released together. WaitForArrival lets
// the main thread observe (with timeout) that all N have reached the barrier,
// which is the deterministic proof of N-way concurrency.
class Barrier
{
public:
	explicit Barrier(std::size_t n) : _threshold(n), _count(n) {}

	void ArriveAndWait()
	{
		std::unique_lock<std::mutex> lock(_mutex);
		if (_count == 0) {
			return; // phase already complete
		}
		--_count;
		if (_count == 0) {
			_released = true;
			_cv.notify_all();
		} else {
			_cv.wait(lock, [this]() { return _released; });
		}
	}

	template <class Rep, class Period>
	bool WaitForArrival(const std::chrono::duration<Rep, Period>& rel) const
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _cv.wait_for(lock, rel, [this]() { return _count == 0; });
	}

	std::size_t arrived() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _threshold - _count;
	}

private:
	mutable std::mutex _mutex;
	mutable std::condition_variable _cv;
	std::size_t _threshold;
	std::size_t _count;
	bool _released = false;
};

// Manual-reset event: workers block on Wait() until the controlling thread
// calls Signal(). Used to hold worker tasks in-flight while we probe capacity.
class ManualResetEvent
{
public:
	void Signal()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_signaled = true;
		_cv.notify_all();
	}

	void Wait()
	{
		std::unique_lock<std::mutex> lock(_mutex);
		_cv.wait(lock, [this]() { return _signaled; });
	}

private:
	std::mutex _mutex;
	std::condition_variable _cv;
	bool _signaled = false;
};

// Arrival counter with timed wait: workers announce arrival; main waits until
// N have arrived. Used to confirm a known number of tasks are simultaneously
// in-flight before probing further behavior.
class ArrivalCounter
{
public:
	void Arrive()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		++_count;
		_cv.notify_all();
	}

	template <class Rep, class Period>
	bool WaitFor(std::size_t target, const std::chrono::duration<Rep, Period>& rel) const
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _cv.wait_for(lock, rel, [this, target]() { return _count >= target; });
	}

	std::size_t count() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _count;
	}

private:
	mutable std::mutex _mutex;
	mutable std::condition_variable _cv;
	std::size_t _count = 0;
};

// ---------------------------------------------------------------------------
// Reporting.
// ---------------------------------------------------------------------------

int g_failures = 0;

void CheckTrue(const char* case_name, bool cond,
               const char* expected, const std::string& actual)
{
	if (cond) {
		std::printf("[PASS] %s\n", case_name);
	} else {
		std::printf("[FAIL] %s\n", case_name);
		std::printf("      expected: %s\n", expected);
		std::printf("      actual:   %s\n", actual.c_str());
		++g_failures;
	}
}

std::string operator""_s(const char* lit, std::size_t) { return std::string(lit); }

// ---------------------------------------------------------------------------
// Group 1 — GateServer HandlerExecutor.
// ---------------------------------------------------------------------------

// 1a. Four handlers can be running simultaneously: 4 worker threads each grab
//     one task and all reach the barrier. Under a single-thread executor this
//     would deadlock permanently at the barrier (only 1 arrival ever), so the
//     timed wait is the deterministic failure signal.
void Group1a_HandlerExecutorFourConcurrent()
{
	std::printf("\n== Group 1a: HandlerExecutor — 4 handlers start concurrently ==\n");
	constexpr std::size_t kWorkers = 4;
	HandlerExecutor ex(kWorkers, kWorkers * 4);
	Barrier barrier(kWorkers);

	for (std::size_t i = 0; i < kWorkers; ++i) {
		bool posted = ex.Post([&barrier]() { barrier.ArriveAndWait(); });
		if (!posted) {
			CheckTrue("post 4 barrier tasks", false,
			          "all Post() == true", "Post #" + std::to_string(i) + " returned false");
			ex.Stop();
			return;
		}
	}

	bool allArrived = barrier.WaitForArrival(kDeadlockTimeout);
	ex.Stop(); // tasks already released; Stop just joins

	CheckTrue("4 handlers reached barrier concurrently (>= 4 live workers)",
	          allArrived,
	          "barrier.Arrived() == 4",
	          "arrived=" + std::to_string(barrier.arrived()));
}

// 1b. Bounded capacity: with 2 workers busy on blocked tasks (outstanding=2)
//     and capacity=3, one extra Post fills the queue (outstanding=3, true) and
//     every further Post is rejected (false). outstanding is updated under the
//     pool lock at Post time, so this is race-free and deterministic.
void Group1b_HandlerExecutorCapacityRejects()
{
	std::printf("\n== Group 1b: HandlerExecutor — capacity-full Post returns false ==\n");
	constexpr std::size_t kWorkers = 2;
	constexpr std::size_t kCapacity = 3;
	HandlerExecutor ex(kWorkers, kCapacity);
	ArrivalCounter running;
	ManualResetEvent release;

	// Two tasks occupy both worker threads and stay in-flight.
	for (std::size_t i = 0; i < kWorkers; ++i) {
		ex.Post([&running, &release]() {
			running.Arrive();
			release.Wait();
		});
	}

	if (!running.WaitFor(kWorkers, kDeadlockTimeout)) {
		CheckTrue("2 workers entered blocked tasks", false,
		          "running.count() == 2",
		          "running.count()=" + std::to_string(running.count()));
		release.Signal();
		ex.Stop();
		return;
	}

	// At this point outstanding == 2 (both running). Capacity allows one more.
	bool third = ex.Post([]() {});            // outstanding 2 -> 3, accepted
	bool fourth = ex.Post([]() {});           // outstanding == capacity, rejected
	bool fifth = ex.Post([]() {});            // still rejected

	release.Signal(); // let the two in-flight tasks finish
	ex.Stop();        // drain the accepted third task, then join

	CheckTrue("Post within capacity accepted (outstanding 2 -> 3)",
	          third, "true", third ? "true"_s : "false"_s);
	CheckTrue("Post at capacity rejected",
	          !fourth, "false", fourth ? "true"_s : "false"_s);
	CheckTrue("Post beyond capacity rejected",
	          !fifth, "false", fifth ? "true"_s : "false"_s);
}

// 1c. Stop drains: every task posted before Stop() must execute exactly once
//     before Stop() returns (it joins workers only after the queue is empty).
void Group1c_HandlerExecutorStopDrains()
{
	std::printf("\n== Group 1c: HandlerExecutor — Stop drains queued tasks ==\n");
	constexpr int kTasks = 500;
	HandlerExecutor ex(2, 1024);
	std::atomic<int> executed{0};

	for (int i = 0; i < kTasks; ++i) {
		ex.Post([&executed]() { executed.fetch_add(1, std::memory_order_relaxed); });
	}
	ex.Stop(); // must drain all kTasks before returning

	CheckTrue("all pre-Stop tasks executed after drain",
	          executed.load() == kTasks,
	          std::string("executed == ").append(std::to_string(kTasks)).c_str(),
	          "executed=" + std::to_string(executed.load()));
}

// ---------------------------------------------------------------------------
// Group 2 — Single LogicWorker reproduces strict single-thread FIFO.
// ---------------------------------------------------------------------------

// 1000 tasks append their index; one worker thread serializes them, so the
// resulting sequence must equal [0..999] in order. This reproduces the
// original single-logic-thread ordering guarantee.
void Group2_LogicWorkerFifo1000()
{
	std::printf("\n== Group 2: LogicWorker — 1000 tasks strict FIFO ==\n");
	constexpr int kN = 1000;
	LogicWorker w;
	std::vector<int> seq;
	seq.reserve(kN);

	for (int i = 0; i < kN; ++i) {
		w.Post([&seq, i]() { seq.push_back(i); });
	}
	w.Stop(); // drains + joins; all writes visible to main

	bool sizeOk = seq.size() == static_cast<std::size_t>(kN);
	CheckTrue("sequence length == 1000", sizeOk,
	          "1000", std::to_string(seq.size()));

	if (!sizeOk) {
		return;
	}

	int firstBad = -1;
	for (int i = 0; i < kN; ++i) {
		if (seq[i] != i) {
			firstBad = i;
			break;
		}
	}
	CheckTrue("indices strictly ascending 0..999", firstBad < 0,
	          "seq[i] == i for all i",
	          firstBad < 0 ? "ok"_s
	                       : ("mismatch at " + std::to_string(firstBad) +
	                          ": seq=" + std::to_string(seq[firstBad]) +
	                          " expected=" + std::to_string(firstBad)));
}

// ---------------------------------------------------------------------------
// Group 3 — Two different LogicWorker blocking tasks overlap.
// ---------------------------------------------------------------------------

// Two independent LogicWorker instances each run one blocking task; both must
// reach the barrier. If they shared a single serialized thread, only one would
// ever arrive and the timed wait would fire — the deadlock signal.
void Group3_LogicWorkerCrossWorkerOverlap()
{
	std::printf("\n== Group 3: LogicWorker — blocking tasks on 2 workers overlap ==\n");
	Barrier barrier(2);
	LogicWorker wA, wB;

	wA.Post([&barrier]() { barrier.ArriveAndWait(); });
	wB.Post([&barrier]() { barrier.ArriveAndWait(); });

	bool both = barrier.WaitForArrival(kDeadlockTimeout);
	wA.Stop();
	wB.Stop();

	CheckTrue("2 distinct workers ran blocking tasks concurrently",
	          both,
	          "barrier.Arrived() == 2",
	          "arrived=" + std::to_string(barrier.arrived()));
}

// ---------------------------------------------------------------------------
// Group 4 — Stop is idempotent and rejects Post after shutdown.
// ---------------------------------------------------------------------------

// Repeated Stop() must not crash (no double-join / UB) and Post() after Stop
// must deterministically return false on both pool types.
void Group4_StopIdempotentAndPostRejects()
{
	std::printf("\n== Group 4: Stop idempotent; Post after Stop returns false ==\n");

	// LogicWorker
	{
		LogicWorker w;
		w.Stop();
		w.Stop();
		w.Stop(); // idempotent, no crash
		bool postAfter = w.Post([]() {});
		CheckTrue("LogicWorker Post() after Stop returns false",
		          !postAfter, "false", postAfter ? "true"_s : "false"_s);
	}

	// HandlerExecutor
	{
		HandlerExecutor ex(2, 8);
		ex.Stop();
		ex.Stop();
		ex.Stop(); // idempotent, no crash
		bool postAfter = ex.Post([]() {});
		CheckTrue("HandlerExecutor Post() after Stop returns false",
		          !postAfter, "false", postAfter ? "true"_s : "false"_s);
	}
}

} // namespace

int main()
{
	std::printf("=== worker_pool_tests (Verification.1) ===");

	Group1a_HandlerExecutorFourConcurrent();
	Group1b_HandlerExecutorCapacityRejects();
	Group1c_HandlerExecutorStopDrains();
	Group2_LogicWorkerFifo1000();
	Group3_LogicWorkerCrossWorkerOverlap();
	Group4_StopIdempotentAndPostRejects();

	std::printf("\n=== summary: %d failure(s) ===\n", g_failures);
	if (g_failures != 0) {
		std::printf("RESULT: FAIL\n");
		return 1;
	}
	std::printf("RESULT: PASS\n");
	return 0;
}
