/**
 * s1_k24_notifier_bounded_stop_test.cpp - BUG-R41-003, decision K24=E1
 * (iteration 03, slice1 final gaps; moved here from the standalone fixture
 * i03_engine_followup_20261104s5 so it runs with the coverage regression).
 *
 * Contract: EventNotifier destruction waits at most stop_timeout_ms for the
 * worker to drain. A worker blocked inside the MQ publisher is detached, its
 * core is kept until process exit and destroy_server is NOT called (the server
 * may still be in use). Releasing the publisher later lets the detached worker
 * finish the accepted event exactly once.
 *
 * Only the public EventNotifier API is used, so the same file also compiles
 * against the pre-K24 implementation for the red control (that version joins
 * the blocked worker without bound). The MQ module is the deterministic R160
 * fake (src/TestUnits/fixtures/r160_fake_wtmsgque.cpp) with its opt-in
 * blocking publisher; no socket or provider is involved.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <Includes/WTSVariant.hpp>
#include <Share/DLLHelper.hpp>
#include <WtCore/EventNotifier.h>
#include <WtCore/WtHelper.h>
#include <WTSUtils/WTSCfgLoader.h>

USING_NS_WTP;

namespace
{
template <typename Pred>
bool s1WaitFor(Pred pred, std::chrono::milliseconds limit)
{
	const auto deadline = std::chrono::steady_clock::now() + limit;
	while (!pred())
	{
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return true;
}
}	// namespace

TEST(S1K24NotifierStop, BlockedPublisherIsDetachedAfterBoundedWait)
{
	const std::string oldInst = WtHelper::getInstDir();
	struct RestoreInstDir
	{
		std::string value;
		~RestoreInstDir() { WtHelper::setInstDir(value.c_str()); }
	} restore{ oldInst };
	WtHelper::setInstDir((std::string(WT_R160_FAKE_MQ_DIR) + "/").c_str());

	const std::string fakePath = std::string(WT_R160_FAKE_MQ_DIR) + "/" + DLLHelper::wrap_module("WtMsgQue", "lib");
	DllHandle inspector = DLLHelper::load_library(fakePath.c_str());
	ASSERT_NE(nullptr, inspector);
	using Reset = void (*)();
	using SetBlock = void (*)(bool);
	using Release = void (*)();
	using Count = uint32_t (*)();
	const auto reset = (Reset)DLLHelper::get_symbol(inspector, "r160_fake_reset");
	const auto setBlock = (SetBlock)DLLHelper::get_symbol(inspector, "r160_fake_set_block_publish");
	const auto releasePublish = (Release)DLLHelper::get_symbol(inspector, "r160_fake_release_publish");
	const auto inPublish = (Count)DLLHelper::get_symbol(inspector, "r160_fake_in_publish");
	const auto createCount = (Count)DLLHelper::get_symbol(inspector, "r160_fake_create_count");
	const auto destroyCount = (Count)DLLHelper::get_symbol(inspector, "r160_fake_destroy_count");
	const auto publishCount = (Count)DLLHelper::get_symbol(inspector, "r160_fake_publish_count");
	ASSERT_NE(nullptr, reset);
	ASSERT_NE(nullptr, setBlock) << "R160 fake MQ without the blocking publisher";
	ASSERT_NE(nullptr, releasePublish);
	ASSERT_NE(nullptr, inPublish);
	ASSERT_NE(nullptr, createCount);
	ASSERT_NE(nullptr, destroyCount);
	ASSERT_NE(nullptr, publishCount);
	reset();
	setBlock(true);

	WTSVariant* cfg = WTSCfgLoader::load_from_content(
		"{\"active\":true,\"url\":\"fake://s1_k24_stop\",\"stop_timeout_ms\":200}");
	ASSERT_NE(nullptr, cfg);

	EventNotifier* notifier = new EventNotifier();
	ASSERT_TRUE(notifier->init(cfg));
	EXPECT_EQ(1u, createCount());
	notifier->notify_event("blocks inside the MQ publisher");
	const bool entered = s1WaitFor([&] { return inPublish() == 1u; }, std::chrono::milliseconds(2000));
	EXPECT_TRUE(entered) << "the worker never reached the blocked publisher";

	std::atomic<bool> destroyed{ false };
	const auto started = std::chrono::steady_clock::now();
	std::thread destroyer([&] {
		delete notifier;
		destroyed.store(true);
	});
	// K24: the destructor returns after the bounded wait (200 ms here) although the
	// publisher is still blocked. 3 s leaves ample slack for a loaded machine.
	const bool returnedInTime = s1WaitFor([&] { return destroyed.load(); }, std::chrono::milliseconds(3000));
	const auto elapsed = std::chrono::steady_clock::now() - started;
	EXPECT_TRUE(returnedInTime) << "destruction waited for the blocked publisher";
	EXPECT_LT(elapsed, std::chrono::seconds(3));
	if (returnedInTime)
	{
		EXPECT_EQ(1u, inPublish()) << "the publisher is still blocked when the destructor returns";
		EXPECT_EQ(0u, destroyCount()) << "a possibly in-use MQ server must be retained";
	}

	// Releasing the publisher lets the detached worker finish the accepted event.
	releasePublish();
	destroyer.join();
	EXPECT_TRUE(s1WaitFor([&] { return inPublish() == 0u; }, std::chrono::milliseconds(2000)));
	EXPECT_TRUE(s1WaitFor([&] { return publishCount() == 1u; }, std::chrono::milliseconds(2000)));
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_EQ(1u, publishCount()) << "the accepted event is published exactly once";
	if (returnedInTime)
		EXPECT_EQ(0u, destroyCount()) << "the detached core never destroys the server";

	setBlock(false);
	cfg->release();
	// The detached core keeps the module mapped; the inspector handle is only a reference.
	DLLHelper::free_library(inspector);
}

// Positive control: an idle notifier still drains and destroys its server once.
TEST(S1K24NotifierStop, IdleNotifierDrainsAndDestroysServerOnce)
{
	const std::string oldInst = WtHelper::getInstDir();
	struct RestoreInstDir
	{
		std::string value;
		~RestoreInstDir() { WtHelper::setInstDir(value.c_str()); }
	} restore{ oldInst };
	WtHelper::setInstDir((std::string(WT_R160_FAKE_MQ_DIR) + "/").c_str());

	const std::string fakePath = std::string(WT_R160_FAKE_MQ_DIR) + "/" + DLLHelper::wrap_module("WtMsgQue", "lib");
	DllHandle inspector = DLLHelper::load_library(fakePath.c_str());
	ASSERT_NE(nullptr, inspector);
	using Reset = void (*)();
	using Count = uint32_t (*)();
	const auto reset = (Reset)DLLHelper::get_symbol(inspector, "r160_fake_reset");
	const auto destroyCount = (Count)DLLHelper::get_symbol(inspector, "r160_fake_destroy_count");
	const auto publishCount = (Count)DLLHelper::get_symbol(inspector, "r160_fake_publish_count");
	ASSERT_NE(nullptr, reset);
	ASSERT_NE(nullptr, destroyCount);
	ASSERT_NE(nullptr, publishCount);
	reset();

	WTSVariant* cfg = WTSCfgLoader::load_from_content(
		"{\"active\":true,\"url\":\"fake://s1_k24_idle\",\"stop_timeout_ms\":2000}");
	ASSERT_NE(nullptr, cfg);
	{
		EventNotifier notifier;
		ASSERT_TRUE(notifier.init(cfg));
		notifier.notify_event("published normally");
	}
	EXPECT_EQ(1u, publishCount());
	EXPECT_EQ(1u, destroyCount());
	cfg->release();
	DLLHelper::free_library(inspector);
}
