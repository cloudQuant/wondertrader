// I03-NEW-065: EventNotifier::init must report failure when the MQ module's
// create_server returns 0 (no server). run.sh places the fake module as
// ./libWtMsgQue.dylib in the case's cwd (zero = create fails, ok = id 7).
#include <gtest/gtest.h>

#include <dlfcn.h>
#include <cstdlib>
#include <string>

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
