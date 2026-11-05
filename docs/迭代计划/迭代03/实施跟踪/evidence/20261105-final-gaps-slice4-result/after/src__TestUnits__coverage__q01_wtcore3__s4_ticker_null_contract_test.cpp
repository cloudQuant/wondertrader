/**
 * I03-NEW-094 (iteration 03, 2026-11-05) regression cases for the three fix
 * sites that had no red/green control: WtSelRtTicker::trigger_price,
 * WtHftRtTicker::trigger_price and WtEngine::handle_push_quote dereferenced
 * curTick->getContractInfo() for the hot-code fan-out without a null check.
 * Contract: a tick without contract metadata is still delivered for its own
 * code, and no hot-code fan-out happens. (The CTA path is covered by
 * R211CtaTicker.LastMinuteOfFiniteSessionIsNotClosedByData.)
 */
#include "r66_fixture.h"

#include "WtCore/WtCtaEngine.h"
#include "WtCore/WtHftEngine.h"
#include "WtCore/WtHftTicker.h"
#include "WtCore/WtSelEngine.h"
#include "WtCore/WtSelTicker.h"

#include <map>
#include <string>

namespace {

using namespace r66;

const char* const kAg = "SHFE.ag.2512";
const char* const kAgHot = "SHFE.ag.HOT";

template <typename Base>
class S4CountingEngine : public Base
{
public:
	void on_tick(const char* stdCode, WTSTickData* curTick) override
	{
		++delivered[stdCode == NULL ? std::string() : std::string(stdCode)];
		Base::on_tick(stdCode, curTick);
	}
	uint32_t count(const char* code) const
	{
		auto it = delivered.find(code);
		return it == delivered.end() ? 0u : it->second;
	}
	std::map<std::string, uint32_t> delivered;
};

} // namespace

// Unstarted SEL ticker forwards straight to trigger_price.
TEST_F(R66Fixture, S4SelTickerTickWithoutContractSkipsHotFanOut)
{
	WTSVariant* cfg = makeCfg();
	ASSERT_NE(nullptr, cfg);
	// Heap-allocated and not destroyed, mirroring R66SelTickerFlatContractSkipsHotLoop.
	S4CountingEngine<WtSelEngine>* engine = new S4CountingEngine<WtSelEngine>();
	engine->init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
	engine->set_date_time(20231010, 900, 0);

	FakeReader reader;
	WtSelRtTicker ticker(engine);
	ticker.init(&reader, "TRADING");

	WTSTickData* orphan = makeTick(kAg, 3500.0, NULL);
	ASSERT_NE(nullptr, orphan);
	ASSERT_EQ(nullptr, orphan->getContractInfo());
	ticker.on_tick(orphan, 1);
	orphan->release();
	EXPECT_EQ(1u, engine->count(kAg));
	EXPECT_EQ(0u, engine->count(kAgHot));

	// Positive control: the same code with contract metadata fans out to HOT.
	WTSTickData* withContract = makeAgTick(3501.0);
	ASSERT_NE(nullptr, withContract);
	ticker.on_tick(withContract, 1);
	withContract->release();
	EXPECT_EQ(2u, engine->count(kAg));
	EXPECT_EQ(1u, engine->count(kAgHot));
}

// HFT ticker without a worker thread forwards straight to trigger_price.
TEST_F(R66Fixture, S4HftTickerTickWithoutContractSkipsHotFanOut)
{
	WTSVariant* cfg = makeCfg();
	ASSERT_NE(nullptr, cfg);
	S4CountingEngine<WtHftEngine> engine;
	engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
	engine.set_date_time(20231010, 900, 0);

	FakeReader reader;
	WtHftRtTicker ticker(&engine);
	ticker.init(&reader, "TRADING");

	WTSTickData* orphan = makeTick(kAg, 3500.0, NULL);
	ASSERT_NE(nullptr, orphan);
	ticker.on_tick(orphan);
	orphan->release();
	EXPECT_EQ(1u, engine.count(kAg));
	EXPECT_EQ(0u, engine.count(kAgHot));

	WTSTickData* withContract = makeAgTick(3501.0);
	ASSERT_NE(nullptr, withContract);
	ticker.on_tick(withContract);
	withContract->release();
	EXPECT_EQ(2u, engine.count(kAg));
	EXPECT_EQ(1u, engine.count(kAgHot));
}

// WtEngine::handle_push_quote for a tick without contract. The three live engines
// override handle_push_quote to forward to their ticker, so the base fix site is
// called explicitly (it is what an engine without its own ticker uses).
TEST_F(R66Fixture, S4EnginePushQuoteWithoutContractSkipsHotFanOut)
{
	WTSVariant* cfg = makeCfg();
	ASSERT_NE(nullptr, cfg);
	S4CountingEngine<WtCtaEngine> engine;
	engine.set_adapter_mgr(&_adapter_mgr);
	engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
	engine.set_date_time(20231010, 900, 0);

	WTSTickData* orphan = makeTick(kAg, 3500.0, NULL);
	ASSERT_NE(nullptr, orphan);
	engine.WtEngine::handle_push_quote(orphan);
	orphan->release();
	EXPECT_EQ(1u, engine.count(kAg));
	EXPECT_EQ(0u, engine.count(kAgHot));
	EXPECT_DOUBLE_EQ(3500.0, engine.get_cur_price(kAg));

	WTSTickData* withContract = makeAgTick(3501.0);
	ASSERT_NE(nullptr, withContract);
	engine.WtEngine::handle_push_quote(withContract);
	withContract->release();
	EXPECT_EQ(2u, engine.count(kAg));
	EXPECT_EQ(1u, engine.count(kAgHot));
	engine.release();
}
