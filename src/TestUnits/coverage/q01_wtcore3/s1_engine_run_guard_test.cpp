/**
 * s1_engine_run_guard_test.cpp - BUG-R10-004 (iteration 03, slice1 final gaps)
 *
 * Original acceptance: "直接调用 WtSelEngine::run 的重复/拒绝语义；拒绝时不得覆盖
 * ticker 指针或泄漏". Before the fix the CTA/SEL/HFT engines created a new
 * real-time ticker on every run(), overwriting _tm_ticker: the first ticker
 * (and its worker thread) leaked and kept driving the engine, and HFT contexts
 * received on_init() again. A repeated run() is now rejected and leaves the
 * running ticker in place; release() still stops and frees exactly that ticker.
 *
 * The engine headers are included with `#define private public` (same technique
 * as r160/r170) only to read the ticker pointer.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <boost/core/noncopyable.hpp>

// Headers whose classes redeclare members with different access are included
// before the access override below.
#include <Includes/FasterDefs.h>
#include <Includes/WTSCollection.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSSessionInfo.hpp>
#include <Includes/WTSVariant.hpp>
#include <Share/StdUtils.hpp>
#include <Share/SpinMutex.hpp>

#define private public
#include <WtCore/WtCtaEngine.h>
#include <WtCore/WtHftEngine.h>
#include <WtCore/WtSelEngine.h>
#undef private

#include "q01_sel_env.h"

#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WtCore/TraderAdapter.h>
#include <WtCore/WtCtaTicker.h>
#include <WtCore/WtDtMgr.h>
#include <WtCore/WtHftTicker.h>
#include <WtCore/WtSelTicker.h>

using namespace wtp;

namespace
{
struct S1RunGuardMarket
{
	S1RunGuardMarket()
	{
		// Full-day session (card 2b: 0-0 is the only full-day form).
		bd.regisSessionWithTime("TRADING", 0, 0);
		WTSCommodityInfo* comm = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
		comm->setSessionInfo(bd.session("TRADING"));
		bd.regis(comm, q01sel::makeCt("SHFE", "ag", "ag2512"));
	}

	WTSVariant* cfg() const
	{
		return WTSCfgLoader::load_from_content(
			"{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"}");
	}

	q01sel::BaseDataMgr bd;
	q01sel::HotMgr hot;
	WtDtMgr dm;
	TraderAdapterMgr adapters;
};

template <typename Engine>
void prepareEngine(Engine& engine, S1RunGuardMarket& mkt)
{
	WTSVariant* cfg = mkt.cfg();
	ASSERT_NE(nullptr, cfg);
	engine.init(cfg, &mkt.bd, &mkt.dm, &mkt.hot, NULL);
	cfg->release();
	engine.set_adapter_mgr(&mkt.adapters);
	const uint32_t today = TimeUtils::getCurDate();
	engine.set_date_time(today, 930, 0);
	engine.set_trading_date(today);
}

// A ticker left behind by a repeated run() would keep its worker running after
// the engine is gone; stop and free it so a red run fails cleanly instead of crashing.
template <typename Ticker>
void stopLeakedTicker(Ticker* first, Ticker* current)
{
	if (first != nullptr && first != current)
	{
		first->stop();
		delete first;
	}
}
}	// namespace

TEST(S1EngineRunGuard, SelSecondRunIsRejectedAndKeepsTheRunningTicker)
{
	q01sel::Q01TempEnv tmp("s1_run_guard_sel");
	S1RunGuardMarket mkt;
	WtSelEngine engine;
	prepareEngine(engine, mkt);

	engine.run();
	WtSelRtTicker* first = engine._tm_ticker;
	ASSERT_NE(nullptr, first);

	engine.run();
	WtSelRtTicker* second = engine._tm_ticker;
	EXPECT_EQ(first, second) << "a repeated run() must not replace (and leak) the running ticker";
	stopLeakedTicker(first, second);

	engine.release();
	EXPECT_EQ(nullptr, engine._tm_ticker);
}

TEST(S1EngineRunGuard, CtaSecondRunIsRejectedAndKeepsTheRunningTicker)
{
	q01sel::Q01TempEnv tmp("s1_run_guard_cta");
	S1RunGuardMarket mkt;
	WtCtaEngine engine;
	prepareEngine(engine, mkt);

	engine.run();
	WtCtaRtTicker* first = engine._tm_ticker;
	ASSERT_NE(nullptr, first);

	engine.run();
	WtCtaRtTicker* second = engine._tm_ticker;
	EXPECT_EQ(first, second) << "a repeated run() must not replace (and leak) the running ticker";
	stopLeakedTicker(first, second);

	engine.release();
	EXPECT_EQ(nullptr, engine._tm_ticker);
}

TEST(S1EngineRunGuard, HftSecondRunIsRejectedAndKeepsTheRunningTicker)
{
	q01sel::Q01TempEnv tmp("s1_run_guard_hft");
	S1RunGuardMarket mkt;
	WtHftEngine engine;
	prepareEngine(engine, mkt);

	engine.run();
	WtHftRtTicker* first = engine._tm_ticker;
	ASSERT_NE(nullptr, first);

	engine.run();
	WtHftRtTicker* second = engine._tm_ticker;
	EXPECT_EQ(first, second) << "a repeated run() must not replace (and leak) the running ticker";
	stopLeakedTicker(first, second);

	engine.release();
	EXPECT_EQ(nullptr, engine._tm_ticker);
}
