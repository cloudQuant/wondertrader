/**
 * I03-NEW-115: WtEngine position / fund / clock data races.
 *
 * The engine thread inserts new contract positions (an insert may relocate
 * the storage of _pos_map), trades, closes and saves, while the engine's own
 * task worker keeps running the on_tick dynamic-profit tasks and the fund
 * update. Before the fix the worker looked positions up without _pos_mtx
 * and the fund fields were written by both threads without a lock.
 *
 * The workload is fixed (no random input); only the interleaving varies.
 * Every number checked below is an integral multiple of the fee/profit
 * units, so the expected totals are exact in double arithmetic.
 *
 * Uses the R41 lane helpers (TempDir, HotMgrFake) and the lane generate
 * directory set by R41GenerateDirEnv (r41_engine_base_test.cpp, same
 * executable). Sanitizer runs: see the slice2-result evidence README.
 */
#include "r41_wtcore_fixture.h"

#include "WtCore/WtCtaEngine.h"

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace r41;

// One commodity (SHFE.rb, volume scale 10) and kCount monthly contracts
// rb1000 .. rb(1000 + kCount - 1). All metadata is created up front and is
// only read afterwards, also from the engine task worker.
class New115BaseData final : public IBaseDataMgr
{
public:
	explicit New115BaseData(uint32_t count)
	{
		_session = WTSSessionInfo::create("TRADING", "day-session");
		_session->addTradingSection(900, 1500);
		_rb = WTSCommodityInfo::create("rb", "rebar", "SHFE", "TRADING", "CHINA");
		_rb->setVolScale(10);
		_rb->setPriceTick(1.0);
		_rb->setCategory(CC_Future);
		_rb->setTradingMode(TM_Both);
		_rb->setSessionInfo(_session);
		for (uint32_t i = 0; i < count; i++)
		{
			const std::string code = "rb" + std::to_string(1000 + i);
			WTSContractInfo* contract = WTSContractInfo::create(code.c_str(), code.c_str(), "SHFE", "rb");
			contract->setVolScale(10);
			contract->setCommInfo(_rb);
			_contracts[code] = contract;
		}
	}

	~New115BaseData()
	{
		for (auto& v : _contracts)
			v.second->release();
		_rb->release();
		_session->release();
	}

	WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		std::string s(exchgpid == NULL ? "" : exchgpid);
		std::string::size_type pos = s.find('.');
		if (pos == std::string::npos)
			return NULL;
		return getCommodity(s.substr(0, pos).c_str(), s.substr(pos + 1).c_str());
	}

	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		if (exchg != NULL && pid != NULL && std::string(exchg) == "SHFE" && std::string(pid) == "rb")
			return _rb;
		return NULL;
	}

	WTSContractInfo* getContract(const char* code, const char* exchg, uint32_t) override
	{
		if (code == NULL || exchg == NULL || std::string(exchg) != "SHFE")
			return NULL;
		auto it = _contracts.find(code);
		return it == _contracts.end() ? NULL : it->second;
	}

	WTSArray* getContracts(const char*, uint32_t) override { return NULL; }
	WTSSessionInfo* getSession(const char* sid) override
	{
		if (sid != NULL && std::string(sid) == "TRADING")
			return _session;
		return NULL;
	}
	WTSSessionInfo* getSessionByCode(const char*, const char*) override { return _session; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool) override { return uDate; }
	uint64_t getBoundaryTime(const char*, uint32_t, bool, bool) override { return 0; }

	WTSContractInfo* contract(const std::string& code) const
	{
		auto it = _contracts.find(code);
		return it == _contracts.end() ? NULL : it->second;
	}

private:
	WTSSessionInfo* _session = nullptr;
	WTSCommodityInfo* _rb = nullptr;
	std::map<std::string, WTSContractInfo*> _contracts;
};

// Exposes the protected engine entry points this test drives directly.
class New115Engine : public WtCtaEngine
{
public:
	using WtEngine::do_set_position;
	using WtEngine::save_datas;
	using WtEngine::update_fund_dynprofit;

	// The engine-side part of a tick (price cache, pending signals, and the
	// two task-worker jobs: per-position dynamic profit and fund update).
	void base_tick(const char* stdCode, WTSTickData* tick) { WtEngine::on_tick(stdCode, tick); }

	void set_fund_update_span(uint32_t span) { _fund_udt_span = span; }

	// Returns after every task queued so far has run on the worker.
	void drain_tasks()
	{
		std::promise<void> done;
		std::future<void> waiter = done.get_future();
		push_task([&done]() { done.set_value(); });
		waiter.wait();
	}

	struct PosView
	{
		bool found = false;
		double volume = 0;
		double dynprofit = 0;
		size_t details = 0;
	};

	PosView position(const std::string& code)
	{
		PosView ret;
		PosInfoPtr pInfo;
		{
			SpinLock lock(_pos_mtx);
			auto it = _pos_map.find(code);
			if (it == _pos_map.end())
				return ret;
			pInfo = it->second;
		}
		SpinLock lock(pInfo->_mtx);
		ret.found = true;
		ret.volume = pInfo->_volume;
		ret.dynprofit = pInfo->_dynprofit;
		ret.details = pInfo->_details.size();
		return ret;
	}

	size_t position_count()
	{
		SpinLock lock(_pos_mtx);
		return _pos_map.size();
	}

	WTSFundStruct fund()
	{
		SpinLock lock(_fund_mtx);
		return _port_fund->fundInfo();
	}
};

class I03New115EngineRace : public ::testing::Test
{
protected:
	// Contracts rb1000 .. rb8999 (four-digit month codes).
	static constexpr uint32_t kCount = 8000;
	static constexpr uint32_t kTDate = 20231010;
	// Open 2.0 per lot, close-today 3.0 per lot (by volume).
	static constexpr double kOpenFee = 2.0;
	static constexpr double kCloseTodayFee = 3.0;
	// Every tick and every close is 5 price units above the open price,
	// volume scale 10: 50 per lot.
	static constexpr double kLotProfit = 50.0;

	void SetUp() override
	{
		g_portfolio_dir = WtHelper::getPortifolioDir();
		_bd.reset(new New115BaseData(kCount));
		_hot.reset(new HotMgrFake());
		_dt.reset(new WtDtMgr());
		removePortfolioFiles();
	}

	void TearDown() override
	{
		for (WTSVariant* cfg : _cfg_owned)
			cfg->release();
		_cfg_owned.clear();
		_dt.reset();
		_hot.reset();
		_bd.reset();
		removePortfolioFiles();
	}

	void removePortfolioFiles() const
	{
		static const char* const files[] = {"datas.json", "trades.csv", "closes.csv", "funds.csv"};
		std::error_code ec;
		for (const char* f : files)
			stdfs::remove(g_portfolio_dir + f, ec);
	}

	void initEngine(New115Engine& engine)
	{
		_tmp.write("fees.json",
			"{\n"
			"  \"SHFE.rb\": { \"open\": 2.0, \"close\": 2.0, \"closetoday\": 3.0,"
			" \"byvolume\": true, \"margin\": 0.12 }\n"
			"}\n");
		_tmp.write("engine_cfg.json",
			"{\n"
			"  \"product\": { \"session\": \"TRADING\" },\n"
			"  \"poolsize\": 0,\n"
			"  \"fees\": \"" + _tmp.path("fees.json") + "\"\n"
			"}\n");
		WTSVariant* cfg = WTSCfgLoader::load_from_file(_tmp.path("engine_cfg.json").c_str());
		ASSERT_NE(nullptr, cfg);
		_cfg_owned.push_back(cfg);

		engine.set_adapter_mgr(&_adapter_mgr);
		engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
		ASSERT_TRUE(engine.fee_config_valid());
		engine.set_date_time(kTDate, 931, 0);
		engine.set_trading_date(kTDate);
	}

	static std::string code(uint32_t i) { return "SHFE.rb." + std::to_string(1000 + i); }
	static double openPrice(uint32_t i) { return 3000.0 + (i % 7); }

	WTSTickData* makeTick(uint32_t i) const
	{
		const std::string stdCode = code(i);
		WTSTickData* tick = WTSTickData::create(stdCode.c_str());
		WTSTickStruct& ts = tick->getTickStruct();
		ts.price = openPrice(i) + 5;
		ts.volume = 1;
		ts.total_volume = 1;
		ts.trading_date = kTDate;
		ts.action_date = kTDate;
		ts.action_time = 93100000;
		tick->setContractInfo(_bd->contract("rb" + std::to_string(1000 + i)));
		return tick;
	}

	// One round of the insert workload; see the test below.
	void insertRound()
	{
		New115Engine engine;
		initEngine(engine);

		std::vector<WTSTickData*> ticks(kCount, nullptr);
		for (uint32_t i = 0; i < kCount; i++)
			ticks[i] = makeTick(i);
		std::vector<std::string> codes(kCount);
		for (uint32_t i = 0; i < kCount; i++)
			codes[i] = code(i);

		std::set<uint32_t> closed;
		const auto started = std::chrono::steady_clock::now();
		for (uint32_t i = 0; i < kCount; i++)
		{
			// New key: may relocate _pos_map while the worker looks positions up.
			engine.do_set_position(codes[i].c_str(), 1, openPrice(i));
			engine.base_tick(codes[i].c_str(), ticks[i]);
			engine.base_tick(codes[i / 2].c_str(), ticks[i / 2]);
			if (i >= 10 && i % 10 == 0)
			{
				const uint32_t j = i - 5;
				engine.do_set_position(codes[j].c_str(), 0, openPrice(j) + 5);
				closed.insert(j);
			}
			if (i % 1000 == 0)
				engine.save_datas();
		}
		const auto inserted = std::chrono::steady_clock::now();
		engine.drain_tasks();
		const auto drained = std::chrono::steady_clock::now();
		std::printf("[I03-NEW-115] %u inserts in %lld ms, worker drained after %lld ms\n", kCount,
			static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(inserted - started).count()),
			static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(drained - started).count()));

		EXPECT_EQ(0u, engine.fee_error_count()) << engine.last_fee_error();
		ASSERT_EQ(static_cast<size_t>(kCount), engine.position_count());

		uint32_t badPositions = 0;
		for (uint32_t i = 0; i < kCount; i++)
		{
			const New115Engine::PosView pos = engine.position(codes[i]);
			const bool isClosed = closed.count(i) != 0;
			const double expVolume = isClosed ? 0.0 : 1.0;
			const double expDynprofit = isClosed ? 0.0 : kLotProfit;
			const size_t expDetails = isClosed ? 0 : 1;
			if (!pos.found || pos.volume != expVolume || pos.dynprofit != expDynprofit || pos.details != expDetails)
			{
				if (badPositions < 5)
					ADD_FAILURE() << codes[i] << " found=" << pos.found << " volume=" << pos.volume
						<< " dynprofit=" << pos.dynprofit << " details=" << pos.details;
				badPositions++;
			}
		}
		EXPECT_EQ(0u, badPositions);

		const double opened = kCount;
		const double closes = static_cast<double>(closed.size());
		ASSERT_EQ(799u, closed.size());

		WTSFundStruct fund = engine.fund();
		EXPECT_DOUBLE_EQ(opened * kOpenFee + closes * kCloseTodayFee, fund._fees);
		EXPECT_DOUBLE_EQ(closes * kLotProfit, fund._profit);
		EXPECT_DOUBLE_EQ(closes * kLotProfit - opened * kOpenFee - closes * kCloseTodayFee, fund._balance);

		// One more full fund update after the worker is idle: the fund's dynamic
		// profit is the sum over all open positions.
		engine.set_fund_update_span(0);
		engine.update_fund_dynprofit();
		fund = engine.fund();
		EXPECT_DOUBLE_EQ((opened - closes) * kLotProfit, fund._dynprofit);

		// The persisted snapshot holds every position and the same fund totals.
		engine.save_datas();
		WTSVariant* root = WTSCfgLoader::load_from_file((g_portfolio_dir + "datas.json").c_str());
		ASSERT_NE(nullptr, root);
		WTSVariant* jPositions = root->get("positions");
		WTSVariant* jFund = root->get("fund");
		ASSERT_NE(nullptr, jPositions);
		ASSERT_NE(nullptr, jFund);
		EXPECT_EQ(static_cast<uint32_t>(kCount), jPositions->size());
		EXPECT_DOUBLE_EQ(opened * kOpenFee + closes * kCloseTodayFee, jFund->getDouble("fees"));
		EXPECT_DOUBLE_EQ((opened - closes) * kLotProfit, jFund->getDouble("dynprofit"));
		EXPECT_EQ(kTDate, jFund->getUInt32("date"));
		root->release();

		for (WTSTickData* tick : ticks)
			tick->release();
	}

	TempDir _tmp{"new115"};
	std::unique_ptr<New115BaseData> _bd;
	std::unique_ptr<HotMgrFake> _hot;
	std::unique_ptr<WtDtMgr> _dt;
	std::vector<WTSVariant*> _cfg_owned;
	TraderAdapterMgr _adapter_mgr;
};

// The engine thread keeps inserting new contract positions (relocating the
// map storage many times), closes every tenth one and saves periodically,
// while the task worker keeps running the dynamic-profit lookup queued by
// each tick. The fund update keeps the default no-riskmon span (5 s), so the
// worker keeps pace with the inserts and its lookups overlap them for the
// whole run. Afterwards every position, the fund totals and the persisted
// snapshot must match the fixed workload exactly.
TEST_F(I03New115EngineRace, TaskWorkerDynprofitWhileEngineThreadInsertsNewContracts)
{
	// Four rounds, each with a fresh engine (and so a fresh, growing map).
	for (uint32_t round = 0; round < 4; round++)
	{
		SCOPED_TRACE(::testing::Message() << "round " << round);
		removePortfolioFiles();
		ASSERT_NO_FATAL_FAILURE(insertRound());
	}
}

// The engine thread settles the trading day while the task worker keeps
// running queued fund updates. Positions and prices do not change during
// this phase, so every update computes the same values; the settled record
// and the reset state must be exact, and no update may land after the reset
// (the settlement date is re-checked under the fund lock).
TEST_F(I03New115EngineRace, SettlementWhileWorkerUpdatesFundKeepsTheSettledState)
{
	New115Engine engine;
	initEngine(engine);
	engine.set_fund_update_span(0);	// every queued fund update runs in full

	const uint32_t n = 400;
	std::vector<WTSTickData*> ticks(n, nullptr);
	std::vector<std::string> codes(n);
	for (uint32_t i = 0; i < n; i++)
	{
		ticks[i] = makeTick(i);
		codes[i] = code(i);
		engine.do_set_position(codes[i].c_str(), 1, openPrice(i));
		engine.base_tick(codes[i].c_str(), ticks[i]);
	}
	engine.drain_tasks();
	engine.update_fund_dynprofit();

	const double dyn = n * kLotProfit;
	const double balance = -(n * kOpenFee);
	WTSFundStruct before = engine.fund();
	ASSERT_DOUBLE_EQ(dyn, before._dynprofit);
	ASSERT_DOUBLE_EQ(n * kOpenFee, before._fees);
	ASSERT_DOUBLE_EQ(balance, before._balance);
	ASSERT_DOUBLE_EQ(balance + dyn, before._max_dyn_bal);	// dynamic balance only grows while opening
	ASSERT_EQ(0u, before._last_date);

	// Settle in the middle of a stream of ticks (each queues a dynamic-profit
	// and a fund update on the worker).
	for (uint32_t round = 0; round < 40; round++)
	{
		for (uint32_t i = 0; i < n; i++)
			engine.base_tick(codes[i].c_str(), ticks[i]);
		if (round == 20)
			engine.on_session_end();
	}
	engine.drain_tasks();

	const WTSFundStruct after = engine.fund();
	EXPECT_EQ(kTDate, after._last_date);
	EXPECT_DOUBLE_EQ(balance + dyn, after._predynbal);
	EXPECT_DOUBLE_EQ(balance, after._prebalance);
	EXPECT_DOUBLE_EQ(balance, after._balance);
	EXPECT_DOUBLE_EQ(0.0, after._profit);
	EXPECT_DOUBLE_EQ(0.0, after._fees);
	EXPECT_DOUBLE_EQ(dyn, after._dynprofit);
	EXPECT_EQ(DBL_MAX, after._max_dyn_bal) << "a fund update landed after the settlement reset";
	EXPECT_EQ(DBL_MAX, after._min_dyn_bal) << "a fund update landed after the settlement reset";
	EXPECT_EQ(0u, after._max_time);
	EXPECT_EQ(0u, after._min_time);

	// The settled record: date,predynbalance,prebalance,balance,closeprofit,
	// dynprofit,fee,maxdynbalance,...
	std::ifstream ifs((g_portfolio_dir + "funds.csv").c_str());
	ASSERT_TRUE(ifs.good());
	std::string line;
	std::string last;
	uint32_t rows = 0;
	while (std::getline(ifs, line))
	{
		if (line.empty() || line.compare(0, 5, "date,") == 0)
			continue;
		last = line;
		rows++;
	}
	ASSERT_EQ(1u, rows);
	std::vector<std::string> fields;
	std::string::size_type start = 0;
	for (;;)
	{
		const std::string::size_type comma = last.find(',', start);
		fields.push_back(last.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
	ASSERT_EQ(15u, fields.size()) << last;
	EXPECT_EQ(std::to_string(kTDate), fields[0]);
	EXPECT_DOUBLE_EQ(balance, std::stod(fields[3]));
	EXPECT_DOUBLE_EQ(0.0, std::stod(fields[4]));
	EXPECT_DOUBLE_EQ(dyn, std::stod(fields[5]));
	EXPECT_DOUBLE_EQ(n * kOpenFee, std::stod(fields[6]));
	EXPECT_DOUBLE_EQ(balance + dyn, std::stod(fields[7]));

	for (WTSTickData* tick : ticks)
		tick->release();
}

// The ticker thread keeps publishing the engine clock while another thread
// reads it through the getters and through the fund update. Each field
// must always hold one of the published values. In a normal build this is a
// smoke check; the race itself is what the TSan run of this case checks.
TEST_F(I03New115EngineRace, ClockPublishedByTickerIsReadWithoutTornFields)
{
	New115Engine engine;
	initEngine(engine);
	engine.set_fund_update_span(0);

	std::atomic<bool> stop(false);
	std::thread ticker([&engine, &stop]() {
		for (uint32_t k = 0; k < 200000; k++)
		{
			const uint32_t minute = 931 + (k % 300);
			engine.set_date_time(kTDate + (k & 1), minute, (k % 60) * 1000, minute);
			engine.set_trading_date(kTDate + (k & 1));
		}
		stop.store(true);
	});

	uint64_t reads = 0;
	uint64_t bad = 0;
	while (!stop.load())
	{
		const uint32_t date = engine.get_date();
		const uint32_t minute = engine.get_min_time();
		const uint32_t raw = engine.get_raw_time();
		const uint32_t secs = engine.get_secs();
		const uint32_t tdate = engine.get_trading_date();
		if ((date != kTDate && date != kTDate + 1) || minute < 931 || minute >= 1231 ||
			raw < 931 || raw >= 1231 || secs >= 60000 || secs % 1000 != 0 ||
			(tdate != kTDate && tdate != kTDate + 1))
			bad++;
		if (reads % 512 == 0)
			engine.update_fund_dynprofit();	// reads _cur_tdate/_cur_raw_time/_cur_secs
		reads++;
	}
	ticker.join();

	EXPECT_GT(reads, 0u);
	EXPECT_EQ(0u, bad);
	EXPECT_EQ(kTDate + 1, engine.get_trading_date());	// last publication: k = 199999
}

}  // namespace
