#include <gtest/gtest.h>

#include "WtCore/WtEngine.h"

#include <atomic>
#include <chrono>
#if !defined(_WIN32)
#include <csignal>
#include <unistd.h>
#endif
#include <cstdlib>
#include <future>
#include <thread>

using namespace wtp;

namespace
{
class R214TaskEngine : public WtEngine
{
public:
	void run() override {}
	void on_bar(const char*, const char*, uint32_t, WTSBarStruct*) override {}

	void schedule(TaskItem task)
	{
		push_task(std::move(task));
	}

	StdUniqueLock lockTaskQueue()
	{
		return StdUniqueLock(_mtx_task);
	}

	bool isStopped() const
	{
		return _terminated.load();
	}
};

void failChild(int code)
{
	std::_Exit(code);
}

bool waitForStopRequest(const R214TaskEngine& engine)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!engine.isStopped())
	{
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::yield();
	}
	return true;
}

#if defined(_WIN32)
TEST(R214EngineTaskShutdown, releaseCannotLoseWakeBetweenIdleCheckAndWait)
{
	GTEST_SKIP() << "The bounded alarm child-process probe requires POSIX.";
}

TEST(R214EngineTaskShutdown, externalReleaseDrainsAcceptedTasksAndSerializesCallers)
{
	GTEST_SKIP() << "The bounded alarm child-process probe requires POSIX.";
}

TEST(R214EngineTaskShutdown, taskCanRequestStopAndOwnerLaterJoinsWorker)
{
	GTEST_SKIP() << "The bounded alarm child-process probe requires POSIX.";
}
#else
TEST(R214EngineTaskShutdown, releaseCannotLoseWakeBetweenIdleCheckAndWait)
{
	ASSERT_EXIT(
		{
			alarm(8);
			R214TaskEngine engine;
			std::promise<void> taskStarted;
			std::promise<void> continueTask;
			auto continueFuture = continueTask.get_future().share();
			auto startedFuture = taskStarted.get_future();

			engine.schedule([&]() {
				taskStarted.set_value();
				continueFuture.wait();
			});
			if (startedFuture.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
				failChild(2);

			// Hold the queue lock while the task returns so the worker passes its
			// old outer termination check and reaches the empty-queue wait window.
			auto queueLock = engine.lockTaskQueue();
			continueTask.set_value();
			std::this_thread::sleep_for(std::chrono::milliseconds(100));

			std::promise<void> releaserStarted;
			auto releaseStarted = releaserStarted.get_future();
			std::thread releaser([&engine, &releaserStarted]() {
				releaserStarted.set_value();
				engine.release();
			});
			if (releaseStarted.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
				failChild(3);
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			queueLock.unlock();
			releaser.join();
			failChild(0);
		},
		::testing::ExitedWithCode(0), "");
}

TEST(R214EngineTaskShutdown, externalReleaseDrainsAcceptedTasksAndSerializesCallers)
{
	ASSERT_EXIT(
		{
			alarm(8);
			R214TaskEngine engine;
			std::promise<void> taskStarted;
			std::promise<void> continueTask;
			auto continueFuture = continueTask.get_future().share();
			auto startedFuture = taskStarted.get_future();
			std::atomic<uint32_t> completed(0);

			engine.schedule([&]() {
				taskStarted.set_value();
				continueFuture.wait();
				++completed;
			});
			if (startedFuture.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
				failChild(2);

			for (uint32_t i = 0; i < 32; ++i)
				engine.schedule([&completed]() { ++completed; });

			std::promise<void> firstReleaseStarted;
			std::promise<void> secondReleaseStarted;
			auto firstStarted = firstReleaseStarted.get_future();
			auto secondStarted = secondReleaseStarted.get_future();
			std::thread firstRelease([&engine, &firstReleaseStarted]() {
				firstReleaseStarted.set_value();
				engine.release();
			});
			std::thread secondRelease([&engine, &secondReleaseStarted]() {
				secondReleaseStarted.set_value();
				engine.release();
			});
			if (firstStarted.wait_for(std::chrono::seconds(2)) != std::future_status::ready ||
				secondStarted.wait_for(std::chrono::seconds(2)) != std::future_status::ready ||
				!waitForStopRequest(engine))
				failChild(4);
			continueTask.set_value();
			firstRelease.join();
			secondRelease.join();
			engine.release();

			if (completed.load() != 33)
				failChild(3);
			failChild(0);
		},
		::testing::ExitedWithCode(0), "");
}

TEST(R214EngineTaskShutdown, taskCanRequestStopAndOwnerLaterJoinsWorker)
{
	ASSERT_EXIT(
		{
			alarm(8);
			R214TaskEngine engine;
			std::promise<void> stopRequested;
			auto requested = stopRequested.get_future();
			engine.schedule([&]() {
				engine.release();
				stopRequested.set_value();
			});
			if (requested.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
				failChild(2);
			engine.release();
			failChild(0);
		},
		::testing::ExitedWithCode(0), "");
}
#endif
}
