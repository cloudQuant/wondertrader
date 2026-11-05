/**
 * I03-NEW-075: CTA/SEL/HFT ticker run() with a missing trading session.
 *
 * init() with an unknown session id leaves the ticker without session info.
 * Before the fix run() dereferenced the null session (calcTradingDate(
 * _s_info->id(), ...)) and crashed. Contract checked here with only the
 * pre-existing ticker API (init/run/stop): run() does not crash, does not
 * initialize the engine (no on_session_begin) and starts no worker; stop()
 * returns. Each case runs in a child process so that the red control reports
 * a failed assertion instead of terminating the test binary.
 */
#include "r41_wtcore_fixture.h"

#include "WtCore/WtCtaEngine.h"
#include "WtCore/WtSelEngine.h"
#include "WtCore/WtHftEngine.h"
#include "WtCore/WtCtaTicker.h"
#include "WtCore/WtSelTicker.h"
#include "WtCore/WtHftTicker.h"

#include <cstdlib>

namespace {

using namespace r41;

const char* const kNoSession = "S5_NO_SUCH_SESSION";

template <typename Engine, typename Ticker>
int runWithoutSession(WTSVariant* cfg, IBaseDataMgr* bd, WtDtMgr* dt, IHotMgr* hot)
{
	Engine engine;
	engine.init(cfg, bd, dt, hot, NULL);
	EvtListenerFake listener;
	engine.regEventListener(&listener);
	{
		Ticker ticker(&engine);
		ticker.init(NULL, kNoSession);
		ticker.run();
		ticker.stop();
	}
	return (listener._session == 0 && listener._init == 0) ? 0 : 3;
}

} // namespace

TEST_F(R41EngineFixture, S5CtaTickerRunWithoutSessionStartsNothing)
{
	WTSVariant* cfg = makeCfg();
	ASSERT_NE(nullptr, cfg);
	EXPECT_EXIT(std::exit((runWithoutSession<WtCtaEngine, WtCtaRtTicker>(cfg, _bd.get(), _dt.get(), _hot.get()))),
		::testing::ExitedWithCode(0), "");
}

TEST_F(R41EngineFixture, S5SelTickerRunWithoutSessionStartsNothing)
{
	WTSVariant* cfg = makeCfg();
	ASSERT_NE(nullptr, cfg);
	EXPECT_EXIT(std::exit((runWithoutSession<WtSelEngine, WtSelRtTicker>(cfg, _bd.get(), _dt.get(), _hot.get()))),
		::testing::ExitedWithCode(0), "");
}

TEST_F(R41EngineFixture, S5HftTickerRunWithoutSessionStartsNothing)
{
	WTSVariant* cfg = makeCfg();
	ASSERT_NE(nullptr, cfg);
	EXPECT_EXIT(std::exit((runWithoutSession<WtHftEngine, WtHftRtTicker>(cfg, _bd.get(), _dt.get(), _hot.get()))),
		::testing::ExitedWithCode(0), "");
}

// The failure is observable to the caller: run() reports false, repeatedly.
TEST_F(R41EngineFixture, S5TickerRunWithoutSessionReportsFailure)
{
	WTSVariant* cfg = makeCfg();
	ASSERT_NE(nullptr, cfg);
	{
		WtCtaEngine engine;
		engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
		WtCtaRtTicker ticker(&engine);
		ticker.init(NULL, kNoSession);
		EXPECT_FALSE(ticker.run());
		EXPECT_FALSE(ticker.run());
		ticker.stop();
	}
	{
		WtSelEngine* engine = new WtSelEngine();
		engine->init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
		{
			WtSelRtTicker ticker(engine);
			ticker.init(NULL, kNoSession);
			EXPECT_FALSE(ticker.run());
			ticker.stop();
		}
		delete engine;
	}
	{
		WtHftEngine engine;
		engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
		WtHftRtTicker ticker(&engine);
		ticker.init(NULL, kNoSession);
		EXPECT_FALSE(ticker.run());
		EXPECT_FALSE(ticker.run());
		ticker.stop();
	}
}
