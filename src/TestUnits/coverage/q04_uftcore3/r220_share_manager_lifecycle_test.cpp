#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>

#include <gtest/gtest.h>

#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/UftStraContext.h"

#define private public
#include "WtUftCore/ShareManager.h"
#undef private

using namespace wtp;

namespace {

struct WatchGate
{
	std::mutex mutex;
	std::condition_variable condition;
	bool entered = false;
	bool release = false;
	bool stop_requested_from_worker = false;
	bool worker_wait_result = true;
};

struct ChildReport
{
	int fail_line = 0;
	int checks = 0;
};

ShareManager* g_share_manager = nullptr;

uint64_t reportSectionUpdate(const char*, const char*)
{
	return 2;
}

class GatedShareContext : public UftStraContext
{
public:
	GatedShareContext(WtUftEngine* engine, const char* name, WatchGate* gate)
		: UftStraContext(engine, name), _gate(gate)
	{}

	void on_params_updated() override
	{
		_gate->stop_requested_from_worker = g_share_manager->request_stop_watching();
		_gate->worker_wait_result = g_share_manager->wait_watching_stopped();
		std::unique_lock<std::mutex> lock(_gate->mutex);
		_gate->entered = true;
		_gate->condition.notify_all();
		_gate->condition.wait(lock, [this]() { return _gate->release; });
	}

private:
	WatchGate* _gate;
};

void check(ChildReport& report, bool result, int line)
{
	++report.checks;
	if (!result && report.fail_line == 0)
		report.fail_line = line;
}

#define R220_CHECK(report, expression) check((report), (expression), __LINE__)

void runWatcherLifecycleInChild(ChildReport& report)
{
	ShareManager& manager = ShareManager::self();
	WtUftEngine engine;
	WatchGate gate;
	g_share_manager = &manager;

	std::shared_ptr<GatedShareContext> context(new GatedShareContext(&engine, "r220_share_section", &gate));
	engine.addContext(UftContextPtr(context));

	// The process is forked so the singleton's initialized test state cannot
	// affect other ShareManager tests in the parent process.
	manager._inited = true;
	manager._exchg = "r220_test_domain";
	manager._get_section_updatetime = &reportSectionUpdate;
	manager._engine = &engine;
	manager._secnames["r220_share_section"] = 1;

	R220_CHECK(report, manager.start_watching(10000));
	R220_CHECK(report, manager.start_watching(10000));
	{
		std::unique_lock<std::mutex> lock(gate.mutex);
		if (!gate.condition.wait_for(lock, std::chrono::seconds(2), [&gate]() { return gate.entered; }))
		{
			R220_CHECK(report, false);
			lock.unlock();
			manager.request_stop_watching();
			manager.wait_watching_stopped();
			return;
		}
	}

	R220_CHECK(report, gate.stop_requested_from_worker);
	R220_CHECK(report, !gate.worker_wait_result);
	R220_CHECK(report, manager._engine == &engine);

	std::atomic<bool> waiter1_started{ false };
	std::atomic<bool> waiter2_started{ false };
	std::atomic<bool> waiter1_result{ false };
	std::atomic<bool> waiter2_result{ false };
	std::thread waiter1([&]() {
		waiter1_started.store(true, std::memory_order_release);
		waiter1_result.store(manager.wait_watching_stopped(), std::memory_order_release);
	});
	while (!waiter1_started.load(std::memory_order_acquire))
		std::this_thread::yield();
	std::thread waiter2([&]() {
		waiter2_started.store(true, std::memory_order_release);
		waiter2_result.store(manager.wait_watching_stopped(), std::memory_order_release);
	});
	while (!waiter2_started.load(std::memory_order_acquire))
		std::this_thread::yield();

	// Observe the first control waiter taking ownership of the join while the
	// engine callback remains blocked. The borrowed pointer must stay attached.
	{
		std::unique_lock<std::mutex> lock(manager._watch_mutex);
		const bool joinStarted = manager._watch_condition.wait_for(lock, std::chrono::seconds(2), [&manager]() {
			return manager._watch_join_in_progress;
		});
		R220_CHECK(report, joinStarted);
		R220_CHECK(report, manager._engine == &engine);
	}

	{
		std::lock_guard<std::mutex> lock(gate.mutex);
		gate.release = true;
	}
	gate.condition.notify_all();
	waiter1.join();
	waiter2.join();

	R220_CHECK(report, waiter1_result.load(std::memory_order_acquire));
	R220_CHECK(report, waiter2_result.load(std::memory_order_acquire));
	R220_CHECK(report, manager._worker == nullptr);
	R220_CHECK(report, manager._engine == nullptr);
	R220_CHECK(report, manager.wait_watching_stopped());
	R220_CHECK(report, !manager.start_watching(1000));

	g_share_manager = nullptr;
}

TEST(R220ShareManagerLifecycle, watcherEngineBorrowEndsOnlyAfterControlThreadJoin)
{
	int descriptors[2] = { -1, -1 };
	ASSERT_EQ(0, ::pipe(descriptors));

	std::fflush(nullptr);
	const pid_t child = ::fork();
	ASSERT_GE(child, 0);
	if (child == 0)
	{
		::close(descriptors[0]);
		ChildReport report;
		runWatcherLifecycleInChild(report);
		const ssize_t written = ::write(descriptors[1], &report, sizeof(report));
		::close(descriptors[1]);
		::_exit(written == static_cast<ssize_t>(sizeof(report)) ? 0 : 3);
	}

	::close(descriptors[1]);
	ChildReport report;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	size_t bytesRead = 0;
	while (bytesRead < sizeof(report))
	{
		const auto now = std::chrono::steady_clock::now();
		if (now >= deadline)
			break;

		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
		const int timeoutMs = remaining > 0 ? static_cast<int>(remaining) : 1;
		struct pollfd pipeState = { descriptors[0], POLLIN, 0 };
		const int ready = ::poll(&pipeState, 1, timeoutMs);
		if (ready == 0)
			break;
		if (ready < 0)
		{
			if (errno == EINTR)
				continue;
			break;
		}

		const ssize_t count = ::read(descriptors[0], reinterpret_cast<char*>(&report) + bytesRead,
			sizeof(report) - bytesRead);
		if (count > 0)
			bytesRead += static_cast<size_t>(count);
		else if (count < 0 && errno == EINTR)
			continue;
		else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			continue;
		else
			break;
	}
	::close(descriptors[0]);
	const bool reportComplete = bytesRead == sizeof(report);

	int status = 0;
	pid_t waited = 0;
	while (reportComplete && std::chrono::steady_clock::now() < deadline)
	{
		waited = ::waitpid(child, &status, WNOHANG);
		if (waited == child || (waited < 0 && errno != EINTR))
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	const bool childExited = waited == child;
	if (!reportComplete || !childExited)
	{
		::kill(child, SIGKILL);
		do
		{
			waited = ::waitpid(child, &status, 0);
		} while (waited < 0 && errno == EINTR);
	}

	ASSERT_EQ(sizeof(report), bytesRead) << "child did not report before the 10-second deadline";
	ASSERT_EQ(child, waited);
	ASSERT_TRUE(WIFEXITED(status));
	ASSERT_EQ(0, WEXITSTATUS(status));
	EXPECT_EQ(0, report.fail_line) << "failed check at child line " << report.fail_line;
	EXPECT_EQ(13, report.checks);
}

} // namespace
