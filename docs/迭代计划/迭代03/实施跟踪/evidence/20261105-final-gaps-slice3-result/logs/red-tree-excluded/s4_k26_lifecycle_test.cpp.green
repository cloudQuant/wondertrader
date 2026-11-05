/**
 * BUG-I02-NEW-155 (iteration 03, 2026-11-05): lifecycle coverage for decisions
 * K26=B1 / B2 and K25=A that S5MainK26Sel (a direct handle_pos_change call on a
 * hand-attached engine) does not give.
 *   K26=B1  a portfolio increment without a valid price accumulates onto the
 *           pending target (WtEngine::pending_target_or); when a valid price in
 *           trading time arrives the accumulated target is applied exactly once.
 *   K26=B2  on a HOT roll the old contract's pending target is kept and executed
 *           on the old contract; increments after the roll go to the new one.
 *   K25=A   SEL work runs on the ticker strand; a tick arriving from another
 *           thread while the strand runs portfolio work waits for it and then
 *           applies the accumulated target once.
 * Both live engines are covered (WtSelEngine and WtCtaEngine use the same
 * handle_pos_change base). Engines are initialised through init() with base
 * data, hot manager, data manager and a recording executer; no provider or
 * network is involved.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <Includes/IBaseDataMgr.h>
#include <Includes/IHotMgr.h>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSSessionInfo.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WtCore/IExecCommand.h>
#include <WtCore/TraderAdapter.h>
#include <WtCore/WtCtaEngine.h>
#include <WtCore/WtDtMgr.h>
#include <WtCore/WtHelper.h>
#include <WtCore/WtSelEngine.h>
#include <WtCore/WtSelTicker.h>

using namespace wtp;
namespace stdfs = std::filesystem;

namespace {

const char* const kOld = "SHFE.ag.2512";
const char* const kNew = "SHFE.ag.2602";
const char* const kHot = "SHFE.ag.HOT";
const uint32_t kDay1 = 20250110;
const uint32_t kDay2 = 20250113;

class S4BaseData final : public IBaseDataMgr
{
public:
	S4BaseData()
	{
		_session = WTSSessionInfo::create("TRADING", "day-session");
		_session->addTradingSection(900, 1500);
		_ag = WTSCommodityInfo::create("ag", "silver", "SHFE", "TRADING", "CHINA");
		_ag->setVolScale(15);
		_ag->setPriceTick(1.0);
		_ag->setCategory(CC_Future);
		_ag->setTradingMode(TM_Both);
		_ag->setSessionInfo(_session);
		_old = WTSContractInfo::create("ag2512", "ag2512", "SHFE", "ag");
		_old->setCommInfo(_ag);
		_new = WTSContractInfo::create("ag2602", "ag2602", "SHFE", "ag");
		_new->setCommInfo(_ag);
	}
	~S4BaseData() override
	{
		_new->release();
		_old->release();
		_ag->release();
		_session->release();
	}
	WTSCommodityInfo* getCommodity(const char* fullPid) override
	{
		return fullPid != NULL && std::string(fullPid) == "SHFE.ag" ? _ag : NULL;
	}
	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		return exchg != NULL && pid != NULL && std::string(exchg) == "SHFE" && std::string(pid) == "ag" ? _ag : NULL;
	}
	WTSContractInfo* getContract(const char* code, const char* exchg, uint32_t = 0) override
	{
		if (code == NULL || exchg == NULL || std::string(exchg) != "SHFE")
			return NULL;
		if (std::string(code) == "ag2512")
			return _old;
		if (std::string(code) == "ag2602")
			return _new;
		return NULL;
	}
	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return NULL; }
	WTSSessionInfo* getSession(const char* sid) override
	{
		return sid != NULL && std::string(sid) == "TRADING" ? _session : NULL;
	}
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return _session; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate; }
	uint64_t getBoundaryTime(const char*, uint32_t, bool = false, bool = true) override { return 0; }

private:
	WTSSessionInfo* _session = NULL;
	WTSCommodityInfo* _ag = NULL;
	WTSContractInfo* _old = NULL;
	WTSContractInfo* _new = NULL;
};

/** HOT rule for SHFE.ag; the mapped month contract is switchable to model a roll. */
class S4HotMgr final : public IHotMgr
{
public:
	std::string current = "ag2512";

	const char* getRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isHot(const char*, const char*, uint32_t) override { return false; }
	bool splitHotSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isSecond(const char*, const char*, uint32_t) override { return false; }
	bool splitSecondSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getCustomRawCode(const char* tag, const char* stdCommID, uint32_t) override
	{
		if (tag != NULL && std::string(tag) == "HOT" && stdCommID != NULL && std::string(stdCommID) == "SHFE.ag")
			return current.c_str();
		return "";
	}
	const char* getPrevCustomRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isCustomHot(const char*, const char*, uint32_t) override { return false; }
	bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getRuleTag(const char* ext) override
	{
		return ext != NULL && std::string(ext) == "HOT" ? "HOT" : "";
	}
	double getRuleFactor(const char*, const char*, uint32_t) override { return 1.0; }
};

/** Records every portfolio increment routed to the executers. */
class S4RecExecuter final : public IExecCommand
{
public:
	explicit S4RecExecuter(const char* name) : IExecCommand(name) {}
	void on_position_changed(const char* stdCode, double diffPos) override
	{
		std::lock_guard<std::mutex> lock(mtx);
		diffs.emplace_back(stdCode ? stdCode : "", diffPos);
	}
	double sum(const char* code)
	{
		std::lock_guard<std::mutex> lock(mtx);
		double s = 0;
		for (const auto& d : diffs)
			if (d.first == code)
				s += d.second;
		return s;
	}
	std::size_t count()
	{
		std::lock_guard<std::mutex> lock(mtx);
		return diffs.size();
	}
	std::mutex mtx;
	std::vector<std::pair<std::string, double> > diffs;
};

template <typename Base>
class S4Engine : public Base
{
public:
	static constexpr double kNoPending = -999.0;
	double pendingTarget(const char* code) const { return this->pending_target_or(code, kNoPending); }
	double portfolio(const char* code)
	{
		auto it = this->_pos_map.find(code);
		return it == this->_pos_map.end() || !it->second ? 0.0 : it->second->_volume;
	}
};

std::string tempDir(const char* tag)
{
	const stdfs::path dir = stdfs::temp_directory_path()
		/ ("s4_k26_" + std::string(tag) + "_" + std::to_string((long)::getpid()));
	std::error_code ec;
	stdfs::remove_all(dir, ec);
	stdfs::create_directories(dir, ec);
	return dir.string() + "/";
}

WTSVariant* engineCfg()
{
	return WTSCfgLoader::load_from_content(
		"{\"product\":{\"session\":\"TRADING\"},\"poolsize\":0,\"fees\":\"\",\"filters\":\"\"}");
}

WTSTickData* tick(const char* stdCode, double price, uint32_t date, uint32_t time)
{
	WTSTickData* t = WTSTickData::create(stdCode);
	WTSTickStruct& ts = t->getTickStruct();
	ts.price = price;
	ts.open = price;
	ts.high = price;
	ts.low = price;
	ts.volume = 1;
	ts.total_volume = 1;
	ts.trading_date = date;
	ts.action_date = date;
	ts.action_time = time;
	return t;
}

template <typename Engine>
struct S4EngineEnv
{
	S4BaseData bd;
	S4HotMgr hot;
	WtDtMgr dm;
	TraderAdapterMgr adapters;
	std::unique_ptr<Engine> engine{ new Engine() };
	std::shared_ptr<S4RecExecuter> exec{ new S4RecExecuter("s4_k26_exec") };

	explicit S4EngineEnv(const char* tag)
	{
		WtHelper::setGenerateDir(tempDir(tag).c_str());
		WTSVariant* cfg = engineCfg();
		EXPECT_NE(nullptr, cfg);
		engine->set_adapter_mgr(&adapters);
		engine->init(cfg, &bd, &dm, &hot, NULL);
		if (cfg != NULL)
			cfg->release();
		ExecCmdPtr cmd = exec;
		engine->addExecuter(cmd);
		engine->set_date_time(kDay1, 930, 0);
		engine->set_trading_date(kDay1);
	}
	~S4EngineEnv() { engine->release(); }
};

template <typename Engine>
void expectAccumulatedTargetAppliedOnceOnPriceRecovery(const char* tag)
{
	S4EngineEnv<Engine> env(tag);
	Engine& engine = *env.engine;

	engine.handle_pos_change("s4_a", kOld, 2.0);
	engine.handle_pos_change("s4_b", kOld, 3.0);
	EXPECT_DOUBLE_EQ(5.0, engine.pendingTarget(kOld)) << "B1: +2 then +3 without a price gives 5";
	EXPECT_DOUBLE_EQ(0.0, engine.portfolio(kOld)) << "nothing is booked without a valid price";
	EXPECT_DOUBLE_EQ(5.0, env.exec->sum(kOld)) << "the executer receives both increments";

	WTSTickData* t1 = tick(kOld, 7000.0, kDay1, 93000000);
	engine.on_tick(kOld, t1);
	t1->release();
	EXPECT_DOUBLE_EQ(5.0, engine.portfolio(kOld)) << "the accumulated target is applied on price recovery";
	EXPECT_DOUBLE_EQ(Engine::kNoPending, engine.pendingTarget(kOld)) << "the pending target is consumed";

	WTSTickData* t2 = tick(kOld, 7001.0, kDay1, 93001000);
	engine.on_tick(kOld, t2);
	t2->release();
	EXPECT_DOUBLE_EQ(5.0, engine.portfolio(kOld)) << "applied exactly once";

	engine.handle_pos_change("s4_a", kOld, 1.0);
	EXPECT_DOUBLE_EQ(6.0, engine.portfolio(kOld)) << "with a price the increment is booked on the position";
	EXPECT_DOUBLE_EQ(Engine::kNoPending, engine.pendingTarget(kOld));
}

template <typename Engine>
void expectHotRollKeepsOldPendingTarget(const char* tag)
{
	S4EngineEnv<Engine> env(tag);
	Engine& engine = *env.engine;

	engine.handle_pos_change("s4_a", kHot, 2.0);
	EXPECT_DOUBLE_EQ(2.0, engine.pendingTarget(kOld)) << "HOT resolves to the mapped month contract";

	env.hot.current = "ag2602";
	engine.set_date_time(kDay2, 930, 0);
	engine.set_trading_date(kDay2);
	engine.handle_pos_change("s4_a", kHot, 1.0);
	EXPECT_DOUBLE_EQ(2.0, engine.pendingTarget(kOld)) << "B2: the old contract keeps its pending target";
	EXPECT_DOUBLE_EQ(1.0, engine.pendingTarget(kNew)) << "the post-roll increment goes to the new contract";

	WTSTickData* oldTick = tick(kOld, 7000.0, kDay2, 93000000);
	engine.on_tick(kOld, oldTick);
	oldTick->release();
	EXPECT_DOUBLE_EQ(2.0, engine.portfolio(kOld)) << "B2: executed on the original contract";
	EXPECT_DOUBLE_EQ(1.0, engine.pendingTarget(kNew));

	WTSTickData* newTick = tick(kNew, 7100.0, kDay2, 93000000);
	engine.on_tick(kNew, newTick);
	newTick->release();
	EXPECT_DOUBLE_EQ(1.0, engine.portfolio(kNew));
	EXPECT_DOUBLE_EQ(2.0, env.exec->sum(kOld));
	EXPECT_DOUBLE_EQ(1.0, env.exec->sum(kNew));
}

/**
 * SEL engine whose on_tick for a zero-price tick models portfolio work on the
 * ticker strand (as a K25 schedule would): it blocks until released, then books
 * two increments.
 */
class S4StrandSelEngine final : public S4Engine<WtSelEngine>
{
public:
	void on_tick(const char* stdCode, WTSTickData* curTick) override
	{
		const bool strandWork = curTick != NULL && curTick->price() == 0.0;
		if (strandWork)
		{
			std::unique_lock<std::mutex> lock(mtx);
			inside = true;
			cv.notify_all();
			cv.wait(lock, [this]() { return released; });
		}
		else
		{
			if (inside.load())
				overlapped = true;
			++priced_ticks;
		}
		WtSelEngine::on_tick(stdCode, curTick);
		if (strandWork)
		{
			handle_pos_change("s4_a", kOld, 2.0);
			handle_pos_change("s4_b", kOld, 3.0);
			inside = false;
		}
	}

	std::mutex mtx;
	std::condition_variable cv;
	std::atomic<bool> inside{ false };
	bool released = false;
	std::atomic<bool> overlapped{ false };
	std::atomic<int> priced_ticks{ 0 };
};

class S4NullReader final : public IDataReader
{
public:
	void init(WTSVariant*, IDataReaderSink*, IHisDataLoader* = NULL) override {}
	void onMinuteEnd(uint32_t, uint32_t, uint32_t = 0) override {}
	WTSTickSlice* readTickSlice(const char*, uint32_t, uint64_t = 0) override { return NULL; }
	WTSKlineSlice* readKlineSlice(const char*, WTSKlinePeriod, uint32_t, uint64_t = 0) override { return NULL; }
};

} // namespace

TEST(S4K26Lifecycle, SelAccumulatedTargetIsAppliedOnceOnPriceRecovery)
{
	expectAccumulatedTargetAppliedOnceOnPriceRecovery<S4Engine<WtSelEngine> >("sel_price");
}

TEST(S4K26Lifecycle, CtaAccumulatedTargetIsAppliedOnceOnPriceRecovery)
{
	expectAccumulatedTargetAppliedOnceOnPriceRecovery<S4Engine<WtCtaEngine> >("cta_price");
}

TEST(S4K26Lifecycle, SelHotRollKeepsOldPendingTargetOnOriginalContract)
{
	expectHotRollKeepsOldPendingTarget<S4Engine<WtSelEngine> >("sel_hot");
}

TEST(S4K26Lifecycle, CtaHotRollKeepsOldPendingTargetOnOriginalContract)
{
	expectHotRollKeepsOldPendingTarget<S4Engine<WtCtaEngine> >("cta_hot");
}

TEST(S4K26Lifecycle, SelTickFromAnotherThreadWaitsForStrandWorkThenAppliesOnce)
{
	S4EngineEnv<S4StrandSelEngine> env("sel_strand");
	S4StrandSelEngine& engine = *env.engine;
	S4NullReader reader;
	WtSelRtTicker ticker(&engine);
	ticker.init(&reader, "TRADING");

	std::thread strand([&]() {
		WTSTickData* zero = tick(kOld, 0.0, kDay1, 93000000);
		ticker.on_tick(zero, 0);
		zero->release();
	});
	{
		std::unique_lock<std::mutex> lock(engine.mtx);
		ASSERT_TRUE(engine.cv.wait_for(lock, std::chrono::seconds(5), [&]() { return engine.inside.load(); }));
	}

	std::thread parser([&]() {
		WTSTickData* priced = tick(kOld, 7000.0, kDay1, 93001000);
		ticker.on_tick(priced, 0);
		priced->release();
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	EXPECT_EQ(0, engine.priced_ticks.load()) << "the second tick waits for the strand owner";

	{
		std::lock_guard<std::mutex> lock(engine.mtx);
		engine.released = true;
	}
	engine.cv.notify_all();
	strand.join();
	parser.join();

	EXPECT_FALSE(engine.overlapped.load()) << "strand work and the priced tick never overlap";
	EXPECT_EQ(1, engine.priced_ticks.load());
	EXPECT_DOUBLE_EQ(5.0, engine.portfolio(kOld)) << "the accumulated target is applied once after the strand work";
	EXPECT_DOUBLE_EQ(S4StrandSelEngine::kNoPending, engine.pendingTarget(kOld));
	EXPECT_DOUBLE_EQ(5.0, env.exec->sum(kOld));
}
