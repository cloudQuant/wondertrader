/**
 * I03-NEW-124: engine signal map / price cache / helper clock data races.
 *
 * With a CTA thread pool, several strategies run at once and each can call
 * WtCtaEngine::handle_pos_change() (condition orders, bFireAtOnce), which
 * reads the pending target or the position, adds the increment and stores
 * the new target (append_signal: _sig_map, get_cur_price: _price_map).
 * Strategy threads also read the price cache while the engine thread
 * inserts new codes into it, and trader threads read the WtHelper clock
 * while the ticker thread publishes it. Before the fix none of these was
 * locked: _sig_map and _price_map are vector-backed maps whose storage an
 * insert may relocate, and concurrent increments on one code could be lost.
 *
 * The workload is fixed (no random input); only the interleaving varies.
 * Every number checked below is an integral multiple of the fee unit, so
 * the expected totals are exact in double arithmetic.
 *
 * Uses the R41 lane helpers (TempDir, HotMgrFake) and the lane generate
 * directory set by R41GenerateDirEnv (r41_engine_base_test.cpp, same
 * executable). Sanitizer runs: see the slice2-result evidence README.
 */
#include "r41_wtcore_fixture.h"

#include "WtCore/WtCtaEngine.h"
#include "WtCore/WtHelper.h"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace r41;

// One commodity (SHFE.rb, volume scale 10, session 09:00-15:00) and count
// monthly contracts rb1000 .. rb(1000 + count - 1), created up front and
// only read afterwards.
class New124BaseData final : public IBaseDataMgr
{
public:
	explicit New124BaseData(uint32_t count)
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

	~New124BaseData()
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

// Exposes the protected engine state this test checks.
class New124Engine : public WtCtaEngine
{
public:
	using WtEngine::save_datas;

	// The engine-side part of a tick: price cache and pending signals (and,
	// for a tick with volume, the two task-worker jobs).
	void base_tick(const char* stdCode, WTSTickData* tick) { WtEngine::on_tick(stdCode, tick); }

	struct PosView
	{
		bool found = false;
		double volume = 0;
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
		ret.details = pInfo->_details.size();
		return ret;
	}

	// Pending target of code, or -1 when no signal is pending.
	double pending(const std::string& code)
	{
		SpinLock lock(_sig_mtx);
		auto it = _sig_map.find(code);
		return it == _sig_map.end() ? -1.0 : it->second._volume;
	}

	size_t signal_count()
	{
		SpinLock lock(_sig_mtx);
		return _sig_map.size();
	}

	WTSFundStruct fund()
	{
		SpinLock lock(_fund_mtx);
		return _port_fund->fundInfo();
	}
};

// Releases all workers at once so their calls overlap from the first one.
class StartGate
{
public:
	explicit StartGate(uint32_t parties) : _parties(parties) {}
	void arrive_and_wait()
	{
		_arrived.fetch_add(1);
		while (_arrived.load() < _parties)
			std::this_thread::yield();
	}

private:
	const uint32_t _parties;
	std::atomic<uint32_t> _arrived{0};
};

class I03New124EngineRace : public ::testing::Test
{
protected:
	static constexpr uint32_t kCount = 8000;	// rb1000 .. rb8999
	static constexpr uint32_t kTDate = 20231010;
	static constexpr double kOpenFee = 2.0;	// per lot, by volume

	void SetUp() override
	{
		_portfolio_dir = WtHelper::getPortifolioDir();
		_bd.reset(new New124BaseData(kCount));
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
			stdfs::remove(_portfolio_dir + f, ec);
	}

	void initEngine(New124Engine& engine)
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
		engine.set_date_time(kTDate, 931, 0);	// inside the trading section
		engine.set_trading_date(kTDate);
	}

	static std::string code(uint32_t i) { return "SHFE.rb." + std::to_string(1000 + i); }
	static double price(uint32_t i) { return 3000.0 + i; }

	// A quote with volume 0: WtEngine::on_tick caches the price and checks
	// pending signals, but queues no task-worker job.
	WTSTickData* makeQuote(uint32_t i) const
	{
		const std::string stdCode = code(i);
		WTSTickData* tick = WTSTickData::create(stdCode.c_str());
		WTSTickStruct& ts = tick->getTickStruct();
		ts.price = price(i);
		ts.volume = 0;
		ts.trading_date = kTDate;
		ts.action_date = kTDate;
		ts.action_time = 93100000;
		tick->setContractInfo(_bd->contract("rb" + std::to_string(1000 + i)));
		return tick;
	}

	uint32_t tradeRows() const
	{
		std::ifstream ifs((_portfolio_dir + "trades.csv").c_str());
		std::string line;
		uint32_t rows = 0;
		while (std::getline(ifs, line))
		{
			if (line.find("SHFE.rb.") != std::string::npos)
				rows++;
		}
		return rows;
	}

	std::string _portfolio_dir;
	TempDir _tmp{"new124"};
	std::unique_ptr<New124BaseData> _bd;
	std::unique_ptr<HotMgrFake> _hot;
	std::unique_ptr<WtDtMgr> _dt;
	std::vector<WTSVariant*> _cfg_owned;
	TraderAdapterMgr _adapter_mgr;
};

// The engine thread caches the prices of kCount new codes (each insert may
// relocate the cache storage) while three strategy threads keep reading the
// price of codes already published. Every read must return that code's
// price. Four rounds, each with a fresh engine.
TEST_F(I03New124EngineRace, StrategyThreadsReadPricesWhileEngineThreadCachesNewCodes)
{
	std::vector<WTSTickData*> quotes(kCount, nullptr);
	std::vector<std::string> codes(kCount);
	for (uint32_t i = 0; i < kCount; i++)
	{
		quotes[i] = makeQuote(i);
		codes[i] = code(i);
	}

	for (uint32_t round = 0; round < 4; round++)
	{
		SCOPED_TRACE(::testing::Message() << "round " << round);
		New124Engine engine;
		ASSERT_NO_FATAL_FAILURE(initEngine(engine));

		std::atomic<uint32_t> published(0);
		std::atomic<bool> done(false);
		std::atomic<uint64_t> reads(0);
		std::atomic<uint64_t> bad(0);
		StartGate gate(4);

		std::vector<std::thread> readers;
		for (uint32_t t = 0; t < 3; t++)
		{
			readers.emplace_back([&, t]() {
				gate.arrive_and_wait();
				uint32_t lcg = 12345u + t * 7919u;	// fixed sequence per reader
				uint64_t myReads = 0;
				uint64_t myBad = 0;
				while (!done.load())
				{
					const uint32_t n = published.load(std::memory_order_acquire);
					if (n == 0)
						continue;
					lcg = lcg * 1664525u + 1013904223u;
					const uint32_t j = lcg % n;
					if (engine.get_cur_price(codes[j].c_str()) != price(j))
						myBad++;
					myReads++;
				}
				reads.fetch_add(myReads);
				bad.fetch_add(myBad);
			});
		}

		gate.arrive_and_wait();
		for (uint32_t i = 0; i < kCount; i++)
		{
			engine.base_tick(codes[i].c_str(), quotes[i]);
			published.store(i + 1, std::memory_order_release);
		}
		done.store(true);
		for (std::thread& t : readers)
			t.join();

		std::printf("[I03-NEW-124] round %u: %llu concurrent price reads\n", round,
			static_cast<unsigned long long>(reads.load()));
		EXPECT_GT(reads.load(), 0u);
		EXPECT_EQ(0u, bad.load()) << "a strategy thread read a wrong cached price";

		uint32_t badAfter = 0;
		for (uint32_t i = 0; i < kCount; i++)
		{
			if (engine.get_cur_price(codes[i].c_str()) != price(i))
				badAfter++;
		}
		EXPECT_EQ(0u, badAfter);
	}

	for (WTSTickData* q : quotes)
		q->release();
}

// Four pool threads (four strategies) keep raising the targets of the same
// 16 codes by one lot. Prices are cached, so every increment is applied to
// the position at once. No increment may be lost: each code ends with
// exactly the summed lots, one detail and one trade per increment, and the
// fees and the persisted snapshot agree.
TEST_F(I03New124EngineRace, PoolThreadsIncrementSharedTargetsWithoutLosingUpdates)
{
	New124Engine engine;
	ASSERT_NO_FATAL_FAILURE(initEngine(engine));

	const uint32_t nCodes = 16;
	const uint32_t nThreads = 4;
	const uint32_t perThread = 256;
	const double expLots = static_cast<double>(nThreads * perThread / nCodes);	// 64

	std::vector<std::string> codes(nCodes);
	for (uint32_t i = 0; i < nCodes; i++)
	{
		codes[i] = code(i);
		WTSTickData* q = makeQuote(i);
		engine.base_tick(codes[i].c_str(), q);
		q->release();
		ASSERT_EQ(price(i), engine.get_cur_price(codes[i].c_str()));
	}

	StartGate gate(nThreads);
	std::vector<std::thread> workers;
	for (uint32_t t = 0; t < nThreads; t++)
	{
		workers.emplace_back([&, t]() {
			const std::string strategy = "s" + std::to_string(t);
			gate.arrive_and_wait();
			for (uint32_t k = 0; k < perThread; k++)
				engine.handle_pos_change(strategy.c_str(), codes[k % nCodes].c_str(), 1.0);
		});
	}
	for (std::thread& t : workers)
		t.join();

	EXPECT_EQ(0u, engine.signal_count());
	uint32_t badPositions = 0;
	for (uint32_t i = 0; i < nCodes; i++)
	{
		const New124Engine::PosView pos = engine.position(codes[i]);
		if (!pos.found || pos.volume != expLots || pos.details != static_cast<size_t>(expLots))
		{
			ADD_FAILURE() << codes[i] << " found=" << pos.found << " volume=" << pos.volume
				<< " details=" << pos.details << " (expected " << expLots << ")";
			badPositions++;
		}
	}
	EXPECT_EQ(0u, badPositions) << "concurrent increments were lost";

	const double lots = static_cast<double>(nThreads * perThread);
	const WTSFundStruct fund = engine.fund();
	EXPECT_DOUBLE_EQ(lots * kOpenFee, fund._fees);
	EXPECT_DOUBLE_EQ(-lots * kOpenFee, fund._balance);
	EXPECT_EQ(nThreads * perThread, tradeRows());

	// The last save of the run already holds the final state.
	WTSVariant* root = WTSCfgLoader::load_from_file((_portfolio_dir + "datas.json").c_str());
	ASSERT_NE(nullptr, root) << "datas.json is not a complete document";
	WTSVariant* jPositions = root->get("positions");
	WTSVariant* jFund = root->get("fund");
	ASSERT_NE(nullptr, jPositions);
	ASSERT_NE(nullptr, jFund);
	EXPECT_EQ(nCodes, jPositions->size());
	for (uint32_t i = 0; i < jPositions->size(); i++)
		EXPECT_DOUBLE_EQ(expLots, jPositions->get(i)->getDouble("volume")) << jPositions->get(i)->getCString("code");
	EXPECT_DOUBLE_EQ(lots * kOpenFee, jFund->getDouble("fees"));
	root->release();
}

// No price is cached, so handle_pos_change() queues pending signals. Four
// pool threads raise the targets of disjoint new codes twice by one lot
// (the second increment accumulates onto the pending target, K26=B1) while
// the signal map keeps growing. Then a quote for each code triggers its
// signal on the engine thread. Every code ends with exactly two lots.
TEST_F(I03New124EngineRace, PoolThreadsQueueSignalsForNewCodesThenQuotesTriggerThem)
{
	New124Engine engine;
	ASSERT_NO_FATAL_FAILURE(initEngine(engine));

	const uint32_t nCodes = 800;
	const uint32_t nThreads = 4;
	std::vector<std::string> codes(nCodes);
	for (uint32_t i = 0; i < nCodes; i++)
		codes[i] = code(i);

	StartGate gate(nThreads);
	std::vector<std::thread> workers;
	for (uint32_t t = 0; t < nThreads; t++)
	{
		workers.emplace_back([&, t]() {
			const std::string strategy = "s" + std::to_string(t);
			gate.arrive_and_wait();
			for (uint32_t pass = 0; pass < 2; pass++)
			{
				for (uint32_t i = t; i < nCodes; i += nThreads)
					engine.handle_pos_change(strategy.c_str(), codes[i].c_str(), 1.0);
			}
		});
	}
	for (std::thread& t : workers)
		t.join();

	ASSERT_EQ(static_cast<size_t>(nCodes), engine.signal_count());
	uint32_t badSignals = 0;
	for (uint32_t i = 0; i < nCodes; i++)
	{
		const double pending = engine.pending(codes[i]);
		const New124Engine::PosView pos = engine.position(codes[i]);
		if (pending != 2.0 || pos.volume != 0.0)
		{
			if (badSignals < 5)
				ADD_FAILURE() << codes[i] << " pending=" << pending << " volume=" << pos.volume;
			badSignals++;
		}
	}
	EXPECT_EQ(0u, badSignals);

	for (uint32_t i = 0; i < nCodes; i++)
	{
		WTSTickData* q = makeQuote(i);
		engine.base_tick(codes[i].c_str(), q);
		q->release();
	}

	EXPECT_EQ(0u, engine.signal_count());
	uint32_t badPositions = 0;
	for (uint32_t i = 0; i < nCodes; i++)
	{
		const New124Engine::PosView pos = engine.position(codes[i]);
		if (pos.volume != 2.0 || pos.details != 1)
		{
			if (badPositions < 5)
				ADD_FAILURE() << codes[i] << " volume=" << pos.volume << " details=" << pos.details;
			badPositions++;
		}
	}
	EXPECT_EQ(0u, badPositions);
	EXPECT_DOUBLE_EQ(nCodes * 2 * kOpenFee, engine.fund()._fees);
	EXPECT_EQ(nCodes, tradeRows());
}

// The ticker thread keeps publishing the engine clock (which also sets the
// WtHelper clock that TraderAdapter reads on trader threads) while another
// thread reads the WtHelper getters. Each field must always hold one of
// the published values. In a normal build this is a smoke check; the race
// itself is what the TSan run of this case checks.
TEST_F(I03New124EngineRace, HelperClockPublishedByTickerIsReadByTraderThreads)
{
	New124Engine engine;
	ASSERT_NO_FATAL_FAILURE(initEngine(engine));

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
		const uint32_t date = WtHelper::getDate();
		const uint32_t minute = WtHelper::getTime();
		const uint32_t secs = WtHelper::getSecs();
		const uint32_t tdate = WtHelper::getTradingDate();
		if ((date != kTDate && date != kTDate + 1) || minute < 931 || minute >= 1231 ||
			secs >= 60000 || secs % 1000 != 0 || (tdate != kTDate && tdate != kTDate + 1))
			bad++;
		reads++;
	}
	ticker.join();

	EXPECT_GT(reads, 0u);
	EXPECT_EQ(0u, bad);
}

}  // namespace
