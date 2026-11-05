// I03-NEW-065: EventNotifier::init must report failure when the MQ module's
// create_server returns 0 (no server). run.sh places the fake module as
// ./libWtMsgQue.dylib in the case's cwd (zero = create fails, ok = id 7).
#include <gtest/gtest.h>

#include <dlfcn.h>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "Includes/WTSVariant.hpp"
#include "WtCore/EventNotifier.h"
#include "WtCore/WtHelper.h"

using namespace wtp;

namespace {
struct FakeModule
{
	FakeModule() : handle(dlopen("./libWtMsgQue.dylib", RTLD_NOW)) {}
	~FakeModule() { if (handle) dlclose(handle); }
	int get(const char* name) const
	{
		auto fn = handle ? reinterpret_cast<int (*)()>(dlsym(handle, name)) : nullptr;
		return fn ? fn() : -1000;
	}
	void* handle;
};

WTSVariant* activeCfg()
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("active", true);
	cfg->append("url", "fake://s5_notifier");
	return cfg;
}
}

TEST(S5EventNotifierInit, CreateServerFailureIsReportedAndStartsNothing)
{
	FakeModule m;
	ASSERT_NE(nullptr, m.handle);
	WTSVariant* cfg = activeCfg();
	{
		EventNotifier notifier;
		EXPECT_FALSE(notifier.init(cfg)) << "create_server returned 0";
		EXPECT_EQ(1, m.get("fake_create_count"));
		notifier.notify_event("must not be published");
		notifier.notify_log("tag", "must not be published");
	}
	EXPECT_EQ(0, m.get("fake_publish_count"));
	EXPECT_EQ(0, m.get("fake_destroy_count")) << "no server id 0 is destroyed";
	cfg->release();
}

TEST(S5EventNotifierInit, SuccessfulCreateStillPublishes)
{
	FakeModule m;
	ASSERT_NE(nullptr, m.handle);
	WTSVariant* cfg = activeCfg();
	{
		EventNotifier notifier;
		ASSERT_TRUE(notifier.init(cfg));
		notifier.notify_event("published");
	}
	EXPECT_EQ(1, m.get("fake_create_count"));
	EXPECT_EQ(1, m.get("fake_publish_count"));
	EXPECT_EQ(1, m.get("fake_destroy_count"));
	cfg->release();
}

// Decision K24=E1 (BUG-R41-003): a worker blocked in the MQ publisher is not
// joined without bound. The notifier returns after stop_timeout_ms, keeps the MQ
// server (no destroy_server) and its core alive; releasing the publisher later
// lets the detached worker finish without touching the destroyed notifier.
TEST(S5EventNotifierStop, BlockedPublisherIsDetachedAfterBoundedWait)
{
	FakeModule m;
	ASSERT_NE(nullptr, m.handle);
	auto release = m.handle ? reinterpret_cast<int (*)()>(dlsym(m.handle, "fake_release_publish")) : nullptr;
	ASSERT_NE(nullptr, release) << "blocking fake module required";
	WTSVariant* cfg = activeCfg();
	cfg->append("stop_timeout_ms", 200u);
	const auto started = std::chrono::steady_clock::now();
	{
		EventNotifier notifier;
		ASSERT_TRUE(notifier.init(cfg));
		notifier.notify_event("blocks in the publisher");
		for (int i = 0; i < 200 && m.get("fake_in_publish") == 0; i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		ASSERT_EQ(1, m.get("fake_in_publish"));
	}
	const auto elapsed = std::chrono::steady_clock::now() - started;
	EXPECT_LT(elapsed, std::chrono::seconds(3)) << "destruction must not wait for the blocked publisher";
	EXPECT_EQ(0, m.get("fake_destroy_count")) << "a possibly in-use MQ server is retained";
	release();
	for (int i = 0; i < 200 && m.get("fake_in_publish") != 0; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	EXPECT_EQ(0, m.get("fake_in_publish"));
	EXPECT_EQ(1, m.get("fake_publish_count"));
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	cfg->release();
}

