// WtDtCore ParserAdapter / ParserAdapterMgr lifecycle matrix under card 9.
// Built twice: against the current sources (WT_HOST_NEW) and against the
// pre-change copies (red control). Only APIs common to both are used outside
// the WT_HOST_NEW blocks.
#include <gtest/gtest.h>

#include "fake_probe.hpp"

#include "WtDtCore/ParserAdapter.h"
#include "WtDtCore/DataManager.h"
#include "WtDtCore/WtHelper.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSLogger.h"
#include "Includes/WTSVariant.hpp"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace
{
std::string fixture_dir()
{
	const char* dir = std::getenv("I03_DT_HOST_PARSER_DIR");
	return dir == nullptr ? std::string() : std::string(dir);
}

// Probe paths (dlopen by the test) and adapter module names (resolved by the
// adapter as <module_dir>/parsers/lib<name>.dylib).
std::string v2_path() { return fixture_dir() + "/libI03DtFakeV2.dylib"; }
std::string legacy_path() { return fixture_dir() + "/libI03DtFakeLegacy.dylib"; }
std::string v2_module() { return "I03DtFakeV2"; }
std::string legacy_module() { return "I03DtFakeLegacy"; }
std::string module_dir() { return fixture_dir() + "/../"; }

WTSVariant* make_cfg(const std::string& module)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", module.c_str());
	return cfg;
}

bool wait_until(const std::function<bool()>& pred, int ms = 2000)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (!pred())
	{
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;
}

class DtCoreHost : public ::testing::Test
{
protected:
	static void SetUpTestSuite()
	{
		// Logger left uninitialized: diagnostics go to the default sink only.
		WtHelper::set_module_dir(module_dir().c_str());
	}

	void SetUp() override
	{
		ASSERT_FALSE(fixture_dir().empty()) << "I03_DT_HOST_PARSER_DIR is required";
		ASSERT_TRUE(_v2.open(v2_path())) << v2_path();
		ASSERT_TRUE(_legacy.open(legacy_path())) << legacy_path();
		_v2.reset();
		_legacy.reset();
	}

	WTSBaseDataMgr _bd;

	dtfake::Probe _v2;
	dtfake::Probe _legacy;
};

using namespace dtfake;

TEST_F(DtCoreHost, LegacyModuleWithoutV2IsRejectedBeforeCreateParser)
{
	WTSVariant* cfg = make_cfg(legacy_module());
	ParserAdapter pa(&_bd, nullptr, nullptr);
	EXPECT_FALSE(pa.init("legacy", cfg));
	EXPECT_EQ(_legacy[C_CREATE], 0);
	EXPECT_EQ(_legacy[C_REGISTER], 0);
	EXPECT_EQ(_legacy[C_INIT], 0);
	EXPECT_FALSE(pa.hasApi());
	EXPECT_EQ(cfg->retainCount(), 1u);
	EXPECT_FALSE(pa.run());
	EXPECT_EQ(_legacy[C_CONNECT], 0);
	pa.release();
	cfg->release();
}

TEST_F(DtCoreHost, FailedProviderInitRollsBackThroughV2AndReturnsConfig)
{
	_v2.set(K_INIT_OK, 0);
	WTSVariant* cfg = make_cfg(v2_module());
	ParserAdapter pa(&_bd, nullptr, nullptr);
	EXPECT_FALSE(pa.init("fail", cfg));
	EXPECT_EQ(_v2[C_CREATE], 1);
	EXPECT_EQ(_v2[C_INIT], 1);
	EXPECT_EQ(_v2[C_SUBSCRIBE], 0);
	// Rolled back before init returned: stopped, drained, destroyed.
	EXPECT_EQ(_v2[C_REQUEST_STOP], 1);
	EXPECT_EQ(_v2[C_DESTROY], 1);
	EXPECT_EQ(_v2[C_LIVE], 0);
	EXPECT_EQ(_v2[C_DELETE_UNSAFE], 0);
	EXPECT_FALSE(pa.hasApi());
	EXPECT_TRUE(pa.releaseComplete());
	EXPECT_EQ(cfg->retainCount(), 1u);
	EXPECT_FALSE(pa.run());
	EXPECT_EQ(_v2[C_CONNECT], 0);
	pa.release();
	pa.release();
	EXPECT_EQ(_v2[C_DESTROY], 1);		// cleanup exactly once
	cfg->release();
}

TEST_F(DtCoreHost, ProviderInitThrowRollsBackThroughV2)
{
	_v2.set(K_INIT_THROWS, 1);
	_v2.set(K_START_THREAD, 0);
	WTSVariant* cfg = make_cfg(v2_module());
	ParserAdapter pa(&_bd, nullptr, nullptr);
	EXPECT_FALSE(pa.init("throw", cfg));
	EXPECT_EQ(_v2[C_DESTROY], 1);
	EXPECT_EQ(_v2[C_LIVE], 0);
	EXPECT_FALSE(pa.hasApi());
	EXPECT_EQ(cfg->retainCount(), 1u);
	EXPECT_FALSE(pa.run());
	cfg->release();
}

TEST_F(DtCoreHost, SuccessfulInitReleaseDrainsBeforeDestroy)
{
	WTSVariant* cfg = make_cfg(v2_module());
	{
		ParserAdapter pa(&_bd, nullptr, nullptr);
		ASSERT_TRUE(pa.init("ok", cfg));
		EXPECT_EQ(cfg->retainCount(), 2u);
		EXPECT_EQ(_v2[C_SUBSCRIBE], 1);
		EXPECT_TRUE(pa.run());
		EXPECT_EQ(_v2[C_CONNECT], 1);
		EXPECT_TRUE(wait_until([&]() { return _v2[C_CALLBACKS] > 3; }));

		pa.release();
		EXPECT_EQ(_v2[C_DELETE_UNSAFE], 0);
		EXPECT_EQ(_v2[C_CALLBACKS_AFTER_RELEASE], 0);
		EXPECT_EQ(_v2[C_DESTROY], 1);
		EXPECT_EQ(_v2[C_LIVE], 0);
		EXPECT_FALSE(pa.hasApi());
		EXPECT_TRUE(pa.releaseComplete());
		EXPECT_EQ(cfg->retainCount(), 1u);
		pa.release();		// repeated release is a no-op
		EXPECT_EQ(_v2[C_DESTROY], 1);
	}
	EXPECT_EQ(cfg->retainCount(), 1u);
	cfg->release();
}

TEST_F(DtCoreHost, SecondInitIsRejectedAndKeepsOriginal)
{
	WTSVariant* cfg = make_cfg(v2_module());
	WTSVariant* cfg2 = make_cfg(v2_module());
	ParserAdapter pa(&_bd, nullptr, nullptr);
	ASSERT_TRUE(pa.init("first", cfg));
	EXPECT_FALSE(pa.init("second", cfg2));
	EXPECT_EQ(_v2[C_CREATE], 1);
	EXPECT_EQ(cfg2->retainCount(), 1u);
	EXPECT_STREQ(pa.id(), "first");
	EXPECT_TRUE(pa.run());
	pa.release();
	EXPECT_EQ(cfg->retainCount(), 1u);
	cfg->release();
	cfg2->release();
}

TEST_F(DtCoreHost, ReleaseFromProviderThreadDefersToSafePoint)
{
	WTSVariant* cfg = make_cfg(v2_module());
	ParserAdapter* pa = new ParserAdapter(&_bd, nullptr, nullptr);
	ASSERT_TRUE(pa->init("reentry", cfg));
	std::atomic<int> hookRan{0};
	struct Ctx { ParserAdapter* pa; std::atomic<int>* ran; } ctx{pa, &hookRan};
	_v2.set_hook([](void* p) {
		Ctx* c = static_cast<Ctx*>(p);
		c->pa->release();
		c->ran->store(1);
	}, &ctx);
	ASSERT_TRUE(wait_until([&]() { return hookRan.load() == 1; }));
	// The provider thread could not wait for itself: nothing destroyed there.
	EXPECT_EQ(_v2[C_DELETE_UNSAFE], 0);
	EXPECT_EQ(_v2[C_DESTROY], 0);
	EXPECT_TRUE(pa->hasApi());

	pa->release();		// safe point on the control thread
	EXPECT_EQ(_v2[C_DESTROY], 1);
	EXPECT_EQ(_v2[C_LIVE], 0);
	EXPECT_FALSE(pa->hasApi());
	EXPECT_EQ(cfg->retainCount(), 1u);
	delete pa;
	cfg->release();
}

TEST_F(DtCoreHost, ManagerTeardownReleasesEveryDrainedAdapter)
{
	WTSVariant* cfg1 = make_cfg(v2_module());
	WTSVariant* cfg2 = make_cfg(v2_module());
	ParserAdapterMgr mgr;
	ParserAdapterPtr a1(new ParserAdapter(&_bd, nullptr, nullptr));
	ParserAdapterPtr a2(new ParserAdapter(&_bd, nullptr, nullptr));
	ASSERT_TRUE(a1->init("m1", cfg1));
	ASSERT_TRUE(a2->init("m2", cfg2));
	ASSERT_TRUE(mgr.addAdapter("m1", a1));
	ASSERT_TRUE(mgr.addAdapter("m2", a2));
	mgr.run();
	EXPECT_EQ(_v2[C_CONNECT], 2);
	mgr.release();
	EXPECT_EQ(_v2[C_DELETE_UNSAFE], 0);
	EXPECT_EQ(_v2[C_DESTROY], 2);
	EXPECT_EQ(_v2[C_LIVE], 0);
	EXPECT_EQ(mgr.size(), 0u);
	EXPECT_TRUE(mgr.releaseComplete());
	EXPECT_EQ(cfg1->retainCount(), 1u);
	EXPECT_EQ(cfg2->retainCount(), 1u);
	cfg1->release();
	cfg2->release();
}

#ifdef WT_HOST_NEW
TEST_F(DtCoreHost, UncertainDrainQuarantinesOwnerAndIsNotRetried)
{
	_v2.set(K_WAIT_MODE, 2);
	WTSVariant* cfg = make_cfg(v2_module());
	ParserAdapterMgr* mgr = new ParserAdapterMgr();	// process-lifetime quarantine owner
	ParserAdapterPtr pa(new ParserAdapter(&_bd, nullptr, nullptr));
	ASSERT_TRUE(pa->init("uncertain", cfg));
	ASSERT_TRUE(mgr->addAdapter("uncertain", pa));
	mgr->release();
	EXPECT_EQ(_v2[C_REQUEST_STOP], 1);
	EXPECT_EQ(_v2[C_WAIT], 1);
	EXPECT_EQ(_v2[C_DESTROY], 0);
	EXPECT_EQ(_v2[C_DELETE_UNSAFE], 0);
	EXPECT_EQ(_v2[C_LIVE], 1);
	EXPECT_TRUE(pa->isQuarantined());
	EXPECT_TRUE(pa->hasApi());
	EXPECT_FALSE(mgr->releaseComplete());
	EXPECT_EQ(mgr->size(), 1u);
	EXPECT_EQ(cfg->retainCount(), 2u);
	mgr->release();
	EXPECT_EQ(_v2[C_WAIT], 1);			// not retried
	EXPECT_FALSE(pa->run());
	// Intentionally leak mgr/pa/cfg: the quarantined owner lives to process exit.
	pa.reset();
}

TEST_F(DtCoreHost, PendingDrainIsRetriedUntilDrained)
{
	_v2.set(K_WAIT_MODE, 1);
	WTSVariant* cfg = make_cfg(v2_module());
	ParserAdapter pa(&_bd, nullptr, nullptr);
	ASSERT_TRUE(pa.init("pending", cfg));
	EXPECT_TRUE(pa.requestStopLifecycle());
	EXPECT_FALSE(pa.finishReleaseLifecycle(0));
	EXPECT_TRUE(pa.hasApi());
	EXPECT_FALSE(pa.isQuarantined());
	EXPECT_EQ(cfg->retainCount(), 2u);
	_v2.set(K_WAIT_MODE, 0);
	EXPECT_TRUE(pa.finishReleaseLifecycle(1000));
	EXPECT_EQ(_v2[C_REQUEST_STOP], 1);
	EXPECT_EQ(_v2[C_DESTROY], 1);
	EXPECT_FALSE(pa.hasApi());
	EXPECT_EQ(cfg->retainCount(), 1u);
	cfg->release();
}

TEST_F(DtCoreHost, DestructorDrainsProvableV2Owner)
{
	WTSVariant* cfg = make_cfg(v2_module());
	{
		ParserAdapter pa(&_bd, nullptr, nullptr);
		ASSERT_TRUE(pa.init("dtor", cfg));
		EXPECT_TRUE(wait_until([&]() { return _v2[C_CALLBACKS] > 0; }));
	}
	EXPECT_EQ(_v2[C_REQUEST_STOP], 1);
	EXPECT_EQ(_v2[C_DESTROY], 1);
	EXPECT_EQ(_v2[C_LIVE], 0);
	EXPECT_EQ(cfg->retainCount(), 1u);
	cfg->release();
}
#endif
}
