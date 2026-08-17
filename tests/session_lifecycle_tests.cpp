#include "AsioIOServicePool.h"
#include "SessionLifecycle.h"

#include <atomic>
#include <boost/asio/post.hpp>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void Check(const char* name, bool condition)
{
	if (condition) {
		std::printf("[PASS] %s\n", name);
		return;
	}
	std::printf("[FAIL] %s\n", name);
	++g_failures;
}

void ImmediateCloseIsIdempotent()
{
	llfc::SessionLifecycle lifecycle;
	Check("new session is open", lifecycle.IsOpen());
	Check("first close request wins", lifecycle.BeginClose());
	Check("second close request is ignored", !lifecycle.BeginClose());
	Check("closing session rejects normal work", !lifecycle.IsOpen());
	Check("owner completes close once", lifecycle.CompleteClose());
	Check("closed session cannot complete twice", !lifecycle.CompleteClose());
	Check("final state is closed",
		lifecycle.GetState() == llfc::SessionLifecycle::State::Closed);
}

void GracefulDrainUsesTheSameClosePath()
{
	llfc::SessionLifecycle lifecycle;
	Check("first drain request wins", lifecycle.BeginDrain());
	Check("draining session rejects new work", !lifecycle.IsOpen());
	Check("duplicate drain request is ignored", !lifecycle.BeginDrain());
	Check("drain transitions through normal close", lifecycle.BeginClose());
	Check("drained session completes close", lifecycle.CompleteClose());
}

void ConcurrentCloseHasOneWinner()
{
	llfc::SessionLifecycle lifecycle;
	std::atomic<int> winners{0};
	std::vector<std::thread> threads;
	threads.reserve(32);
	for (int i = 0; i < 32; ++i) {
		threads.emplace_back([&lifecycle, &winners]() {
			if (lifecycle.BeginClose()) {
				winners.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}
	for (auto& thread : threads) {
		thread.join();
	}

	Check("concurrent close has exactly one winner",
		winners.load(std::memory_order_relaxed) == 1);
	Check("concurrent winner can complete close", lifecycle.CompleteClose());
}

void IoPoolDrainRunsAlreadyQueuedWork()
{
	AsioIOServicePool pool(2);
	std::atomic<int> executed{0};
	constexpr int kTasks = 64;
	for (int i = 0; i < kTasks; ++i) {
		boost::asio::post(pool.GetIOService(), [&executed]() {
			executed.fetch_add(1, std::memory_order_relaxed);
		});
	}

	pool.Drain();
	Check("io pool drain runs all previously queued work",
		executed.load(std::memory_order_relaxed) == kTasks);
	pool.Stop();
	pool.Stop();
}

} // namespace

int main()
{
	ImmediateCloseIsIdempotent();
	GracefulDrainUsesTheSameClosePath();
	ConcurrentCloseHasOneWinner();
	IoPoolDrainRunsAlreadyQueuedWork();

	if (g_failures != 0) {
		std::printf("\n%d lifecycle assertion(s) failed\n", g_failures);
		return 1;
	}
	std::printf("\nAll session lifecycle tests passed\n");
	return 0;
}
