/**
 * Direct `get_last_tick` boundary tests for BUG-R103-002.
 *
 * Replay time is set before the first query so each test exercises the first
 * tick-cache lookup through the public replayer API.
 */
#include "r91_common.h"
#include <atomic>
#include <thread>

namespace
{
	const char* const kCode = "SSE.STK.000001";
	const uint32_t kDate = 20200102u;

	class TickLoader : public r91::Loader
	{
	public:
		std::vector<WTSTickStruct> ticks;
		uint32_t calls { 0 };

		bool loadRawHisTicks(void* obj, const char*, uint32_t date, FuncReadTicks cb) override
		{
			++calls;
			if (!ticks.empty())
			{
				for (WTSTickStruct& tick : ticks)
				{
					tick.action_date = date;
					tick.trading_date = date;
				}
				cb(obj, ticks.data(), static_cast<uint32_t>(ticks.size()));
			}
			return true;
		}
	};

	class QueryingSink : public r91::Sink
	{
	public:
		std::vector<double> queriedPrices;
		void handle_tick(const char* code, WTSTickData* tick, uint32_t type) override
		{
			r91::Sink::handle_tick(code, tick, type);
			// A history lookup uses a separate query cursor and must not affect
			// the visibility of the tick currently being dispatched.
			WTSTickSlice* history = rp->get_tick_slice(code, 10u, 0u);
			if (history) history->release();
			WTSTickData* current = rp->get_last_tick(code);
			queriedPrices.push_back(current ? current->price() : -1.0);
			if (current) current->release();
		}
	};

	class AdjustedTickLoader : public TickLoader
	{
	public:
		bool loadRawHisTicks(void* obj, const char* code, uint32_t date, FuncReadTicks cb) override
		{
			if (code != nullptr && std::string(code) == std::string(kCode) + "+")
			{
				WTSTickStruct adjusted = r91::mk_tick(date, 931u, 20.0, 100.0);
				cb(obj, &adjusted, 1);
				return true;
			}
			return TickLoader::loadRawHisTicks(obj, code, date, cb);
		}
	};

	class AdjustedQueryingSink : public QueryingSink
	{
	public:
		std::vector<double> adjustedPrices;
		void handle_tick(const char* code, WTSTickData* tick, uint32_t type) override
		{
			QueryingSink::handle_tick(code, tick, type);
			const std::string adjusted = std::string(code) + "+";
			WTSTickData* current = rp->get_last_tick(adjusted.c_str());
			adjustedPrices.push_back(current ? current->price() : -1.0);
			if (current) current->release();
		}
	};

	class AdjustedBarsLoader : public r91::Loader
	{
	public:
		bool allowRawTicks = false;
		std::vector<WTSBarStruct> bars = {
			r91::mk_bar(20191231u, 1500u, 19.0, 20.0, 18.0, 20.0, 10.0),
			r91::mk_bar(kDate, 1500u, 20.0, 22.0, 19.0, 21.0, 20.0),
			r91::mk_bar(20200103u, 1500u, 21.0, 23.0, 20.0, 22.0, 30.0)
		};
		bool isAutoTrans() override { return false; }
		bool loadFinalHisBars(void* obj, const char*, WTSKlinePeriod, FuncReadBars cb) override
		{
			cb(obj, bars.data(), static_cast<uint32_t>(bars.size()));
			return true;
		}
		bool loadRawHisTicks(void* obj, const char*, uint32_t date, FuncReadTicks cb) override
		{
			if (!allowRawTicks) return true;
			WTSTickStruct raw[] = {
				r91::mk_tick(date, 930u, 10.0, 100.0),
				r91::mk_tick(date, 1501u, 11.0, 100.0)
			};
			cb(obj, raw, 2);
			return true;
		}
	};

	class AdjustedBarQueryingSink : public r91::Sink
	{
	public:
		AdjustedBarsLoader* loader = nullptr;
		std::vector<double> rawPrices;
		void handle_tick(const char* code, WTSTickData* tick, uint32_t type) override
		{
			r91::Sink::handle_tick(code, tick, type);
			loader->allowRawTicks = true;
			WTSTickData* raw = rp->get_last_tick(code);
			rawPrices.push_back(raw ? raw->price() : -1.0);
			if (raw) raw->release();
		}
	};

	class DetailQueryingSink : public r91::Sink
	{
	public:
		bool queried = false;
		bool gotTick = false;
		void handle_order_detail(const char* code, WTSOrdDtlData* detail) override
		{
			r91::Sink::handle_order_detail(code, detail);
			queried = true;
			WTSTickData* tick = rp->get_last_tick(code);
			gotTick = tick != nullptr;
			if (tick) tick->release();
		}
	};

	class SessionQueryingSink : public QueryingSink
	{
	public:
		std::vector<bool> visibleAtBegin;
		void handle_session_begin(uint32_t date) override
		{
			r91::Sink::handle_session_begin(date);
			WTSTickData* tick = rp->get_last_tick(kCode);
			visibleAtBegin.push_back(tick != nullptr);
			if (tick) tick->release();
		}
	};

	class RetainingSink : public r91::Sink
	{
	public:
		WTSTickData* first = nullptr;
		~RetainingSink() { if (first) first->release(); }
		void handle_tick(const char* code, WTSTickData* tick, uint32_t type) override
		{
			r91::Sink::handle_tick(code, tick, type);
			if (!first)
				first = rp->get_last_tick(code);
		}
	};

	class AutoResubscribeSink : public SessionQueryingSink
	{
	public:
		void handle_init() override
		{
			r91::Sink::handle_init();
			rp->sub_tick(1u, kCode);
		}
	};

	class ConcurrentQueryingSink : public r91::Sink
	{
	public:
		std::thread reader;
		std::atomic<bool> entered { false }, finish { false }, invalid { false };
		~ConcurrentQueryingSink()
		{
			finish.store(true);
			if (reader.joinable()) reader.join();
		}
		void handle_tick(const char* code, WTSTickData* tick, uint32_t type) override
		{
			r91::Sink::handle_tick(code, tick, type);
			if (ticks.size() == 1)
			{
				const std::string queryCode(code);
				reader = std::thread([this, queryCode]() {
					while (!finish.load())
					{
						WTSTickData* last = rp->get_last_tick(queryCode.c_str());
						if (!last || (last->price() != 10.0 && last->price() != 11.0))
							invalid.store(true);
						if (last) last->release();
						entered.store(true);
					}
				});
				while (!entered.load()) std::this_thread::yield();
			}
			else if (ticks.size() == 2)
			{
				finish.store(true);
				reader.join();
			}
		}
	};

	std::vector<WTSTickStruct> make_ticks()
	{
		return {
			r91::mk_tick(kDate, 931u, 10.0, 100.0),
			r91::mk_tick(kDate, 932u, 11.0, 200.0),
			r91::mk_tick(kDate, 933u, 12.0, 300.0)
		};
	}

	class R103LastTickBoundaryTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			_root = r91::root();
			_base = r91::temp_dir("r103_last_tick_data");
			_out = r91::temp_dir("r103_last_tick_out");
			WtHelper::setInstDir("");
			WtHelper::setOutputDir(_out.string().c_str());
		}

		void TearDown() override
		{
			WtHelper::setOutputDir("");
			WtHelper::setInstDir("");
			std::error_code ec;
			std::filesystem::remove_all(_base, ec);
			std::filesystem::remove_all(_out, ec);
		}

		bool init(uint64_t stime, IBtDataLoader& loader, HisDataReplayer& replayer, r91::Sink& sink)
		{
			r91::CfgOpts opts;
			opts.mode = "csv";
			opts.tick = true;
			replayer.set_time_range(stime, 202001021500ull);
			WTSVariant* cfg = r91::make_cfg(_root, _base, stime, 202001021500ull, opts);
			const bool initialized = replayer.init(cfg, nullptr, &loader);
			cfg->release();
			if (!initialized)
				return false;
			replayer.register_sink(&sink, "r103_last_tick_boundary");
			return replayer.prepare();
		}

		std::filesystem::path _root;
		std::filesystem::path _base;
		std::filesystem::path _out;
	};
}

TEST_F(R103LastTickBoundaryTest, EmptyTickListReturnsNull)
{
	TickLoader loader;
	HisDataReplayer replayer;
	r91::Sink sink;
	ASSERT_TRUE(init(202001020930ull, loader, replayer, sink));

	EXPECT_EQ(nullptr, replayer.get_last_tick(kCode));
	EXPECT_EQ(1u, loader.calls);
}

TEST_F(R103LastTickBoundaryTest, BeforeFirstTickReturnsNull)
{
	TickLoader loader;
	loader.ticks = make_ticks();
	HisDataReplayer replayer;
	r91::Sink sink;
	ASSERT_TRUE(init(202001020930ull, loader, replayer, sink));

	EXPECT_EQ(nullptr, replayer.get_last_tick(kCode));
	EXPECT_EQ(1u, loader.calls);
}

TEST_F(R103LastTickBoundaryTest, ExactCurrentTickIsReturned)
{
	TickLoader loader;
	loader.ticks = make_ticks();
	HisDataReplayer replayer;
	r91::Sink sink;
	ASSERT_TRUE(init(202001020932ull, loader, replayer, sink));

	WTSTickData* tick = replayer.get_last_tick(kCode);
	ASSERT_NE(nullptr, tick);
	EXPECT_EQ(kDate, tick->actiondate());
	EXPECT_EQ(93200000u, tick->actiontime());
	EXPECT_DOUBLE_EQ(11.0, tick->price());
	tick->release();
	EXPECT_EQ(1u, loader.calls);
}

TEST_F(R103LastTickBoundaryTest, BetweenTicksReturnsPreviousTick)
{
	TickLoader loader;
	loader.ticks = {
		r91::mk_tick(kDate, 931u, 10.0, 100.0),
		r91::mk_tick(kDate, 933u, 12.0, 300.0)
	};
	HisDataReplayer replayer;
	r91::Sink sink;
	ASSERT_TRUE(init(202001020932ull, loader, replayer, sink));

	WTSTickData* tick = replayer.get_last_tick(kCode);
	ASSERT_NE(nullptr, tick);
	EXPECT_EQ(93100000u, tick->actiontime());
	EXPECT_DOUBLE_EQ(10.0, tick->price());
	tick->release();
}

TEST_F(R103LastTickBoundaryTest, EmptyHistoricalSliceDoesNotHideCurrentTick)
{
	TickLoader loader;
	loader.ticks = make_ticks();
	HisDataReplayer replayer;
	r91::Sink sink;
	ASSERT_TRUE(init(202001020932ull, loader, replayer, sink));

	// An earlier slice has an empty prefix but must not change a later query.
	WTSTickSlice* slice = replayer.get_tick_slice(kCode, 1u, 202001020930ull);
	ASSERT_EQ(nullptr, slice);
	WTSTickData* tick = replayer.get_last_tick(kCode);
	ASSERT_NE(nullptr, tick);
	EXPECT_EQ(93200000u, tick->actiontime());
	EXPECT_DOUBLE_EQ(11.0, tick->price());
	tick->release();
	EXPECT_EQ(1u, loader.calls);
}

TEST_F(R103LastTickBoundaryTest, SameTimestampQueriesFollowDispatchedOrder)
{
	TickLoader loader;
	loader.ticks = {
		r91::mk_tick(kDate, 931u, 10.0, 100.0),
		r91::mk_tick(kDate, 931u, 11.0, 200.0),
		r91::mk_tick(kDate, 933u, 12.0, 300.0)
	};
	HisDataReplayer replayer;
	QueryingSink sink;
	sink.rp = &replayer;
	ASSERT_TRUE(init(202001020930ull, loader, replayer, sink));
	replayer.sub_tick(1u, kCode);
	replayer.run(false);
	ASSERT_EQ(3u, sink.queriedPrices.size());
	EXPECT_DOUBLE_EQ(10.0, sink.queriedPrices[0]);
	EXPECT_DOUBLE_EQ(11.0, sink.queriedPrices[1]);
	EXPECT_DOUBLE_EQ(12.0, sink.queriedPrices[2]);
}

TEST_F(R103LastTickBoundaryTest, SubscribedTickIsUnavailableUntilDispatched)
{
	TickLoader loader;
	loader.ticks = {r91::mk_tick(kDate, 930u, 10.0, 100.0)};
	HisDataReplayer replayer;
	SessionQueryingSink sink;
	sink.rp = &replayer;
	ASSERT_TRUE(init(202001020930ull, loader, replayer, sink));
	// The same lookup is a historical query until this code is subscribed.
	WTSTickData* historical = replayer.get_last_tick(kCode);
	ASSERT_NE(nullptr, historical);
	historical->release();
	replayer.sub_tick(1u, kCode);
	EXPECT_EQ(nullptr, replayer.get_last_tick(kCode));
	replayer.run(false);
	ASSERT_EQ(1u, sink.visibleAtBegin.size());
	EXPECT_FALSE(sink.visibleAtBegin[0]);
	ASSERT_EQ(1u, sink.queriedPrices.size());
	EXPECT_DOUBLE_EQ(10.0, sink.queriedPrices[0]);
}

TEST_F(R103LastTickBoundaryTest, ReturnedSnapshotSurvivesLaterTicksAndReset)
{
	TickLoader loader;
	loader.ticks = {
		r91::mk_tick(kDate, 931u, 10.0, 100.0),
		r91::mk_tick(kDate, 932u, 11.0, 200.0)
	};
	HisDataReplayer replayer;
	RetainingSink sink;
	sink.rp = &replayer;
	ASSERT_TRUE(init(202001020930ull, loader, replayer, sink));
	replayer.sub_tick(1u, kCode);
	replayer.run(false);
	ASSERT_EQ(2u, sink.ticks.size());
	ASSERT_NE(nullptr, sink.first);
	EXPECT_DOUBLE_EQ(10.0, sink.first->price());
	ASSERT_TRUE(replayer.prepare());
	EXPECT_DOUBLE_EQ(10.0, sink.first->price());
}

TEST_F(R103LastTickBoundaryTest, ConcurrentSnapshotReadDuringNextDispatch)
{
	TickLoader loader;
	loader.ticks = {
		r91::mk_tick(kDate, 931u, 10.0, 100.0),
		r91::mk_tick(kDate, 932u, 11.0, 200.0)
	};
	HisDataReplayer replayer;
	ConcurrentQueryingSink sink;
	sink.rp = &replayer;
	ASSERT_TRUE(init(202001020930ull, loader, replayer, sink));
	replayer.sub_tick(1u, kCode);
	replayer.run(false);
	ASSERT_EQ(2u, sink.ticks.size());
	EXPECT_FALSE(sink.invalid.load());
}

TEST_F(R103LastTickBoundaryTest, NewTradingDayAndPrepareDoNotReuseDispatchedTick)
{
	TickLoader loader;
	loader.ticks = {r91::mk_tick(kDate, 931u, 10.0, 100.0)};
	HisDataReplayer replayer;
	SessionQueryingSink sink;
	sink.rp = &replayer;
	r91::CfgOpts opts;
	opts.mode = "csv";
	opts.tick = true;
	replayer.set_time_range(202001020930ull, 202001031500ull);
	WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020930ull, 202001031500ull, opts);
	ASSERT_TRUE(replayer.init(cfg, nullptr, &loader));
	cfg->release();
	replayer.register_sink(&sink, "r103_snapshot_reset");
	ASSERT_TRUE(replayer.prepare());
	replayer.sub_tick(1u, kCode);
	replayer.run(false);
	ASSERT_EQ(2u, sink.visibleAtBegin.size());
	EXPECT_FALSE(sink.visibleAtBegin[0]);
	EXPECT_FALSE(sink.visibleAtBegin[1]);
	ASSERT_EQ(2u, sink.queriedPrices.size());

	ASSERT_TRUE(replayer.prepare());
	replayer.sub_tick(1u, kCode);
	EXPECT_EQ(nullptr, replayer.get_last_tick(kCode));
}

TEST_F(R103LastTickBoundaryTest, PrepareRerunResubscribesWithoutReusingOldSnapshot)
{
	TickLoader loader;
	loader.ticks = {r91::mk_tick(kDate, 930u, 10.0, 100.0)};
	HisDataReplayer replayer;
	AutoResubscribeSink sink;
	sink.rp = &replayer;
	ASSERT_TRUE(init(202001020930ull, loader, replayer, sink));
	EXPECT_EQ(nullptr, replayer.get_last_tick(kCode));
	replayer.run(false);
	ASSERT_EQ(1u, sink.queriedPrices.size());
	EXPECT_DOUBLE_EQ(10.0, sink.queriedPrices[0]);
	loader.ticks = {r91::mk_tick(kDate, 930u, 20.0, 100.0)};
	ASSERT_TRUE(replayer.prepare());
	EXPECT_EQ(nullptr, replayer.get_last_tick(kCode));
	replayer.run(false);
	ASSERT_EQ(2u, sink.queriedPrices.size());
	EXPECT_DOUBLE_EQ(20.0, sink.queriedPrices[1]);
	ASSERT_EQ(2u, sink.visibleAtBegin.size());
	EXPECT_FALSE(sink.visibleAtBegin[0]);
	EXPECT_FALSE(sink.visibleAtBegin[1]);
}

TEST_F(R103LastTickBoundaryTest, AdjustedQueryPreservesSeparateHistorySource)
{
	AdjustedTickLoader loader;
	loader.ticks = {r91::mk_tick(kDate, 931u, 10.0, 100.0)};
	HisDataReplayer replayer;
	AdjustedQueryingSink sink;
	sink.rp = &replayer;
	ASSERT_TRUE(init(202001020930ull, loader, replayer, sink));
	const std::string adjusted = std::string(kCode) + "+";
	replayer.sub_tick(1u, adjusted.c_str());
	EXPECT_EQ(nullptr, replayer.get_last_tick(adjusted.c_str()));
	replayer.run(false);
	ASSERT_EQ(1u, sink.queriedPrices.size());
	EXPECT_DOUBLE_EQ(10.0, sink.queriedPrices[0]);
	ASSERT_EQ(1u, sink.adjustedPrices.size());
	EXPECT_DOUBLE_EQ(20.0, sink.adjustedPrices[0]);
}

TEST_F(R103LastTickBoundaryTest, SyntheticFallbackIsTheCurrentDispatchedSnapshot)
{
	const std::vector<WTSBarStruct> bars = {
		r91::mk_bar(20191231u, 1500u, 99.0, 100.0, 98.0, 100.0, 10.0),
		r91::mk_bar(kDate, 1500u, 100.0, 102.0, 99.0, 101.0, 20.0),
		r91::mk_bar(20200103u, 1500u, 101.0, 103.0, 100.0, 102.0, 30.0)
	};
	ASSERT_TRUE(r91::write_bars(_base, "SSE", "000001", "day", bars));
	r91::CfgOpts opts;
	opts.mode = "bin";
	opts.tick = true;
	HisDataReplayer replayer;
	replayer.set_time_range(202001020930ull, 202001031500ull);
	WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020930ull, 202001031500ull, opts);
	ASSERT_TRUE(replayer.init(cfg, nullptr, nullptr));
	cfg->release();
	QueryingSink sink;
	sink.rp = &replayer;
	replayer.register_sink(&sink, "r103_synthetic");
	ASSERT_TRUE(replayer.prepare());
	replayer.sub_tick(1u, kCode);
	WTSKlineSlice* slice = replayer.get_kline_slice(kCode, "d", 10, 1, true);
	ASSERT_NE(nullptr, slice);
	slice->release();
	replayer.run(false);
	ASSERT_FALSE(sink.ticks.empty());
	ASSERT_EQ(sink.ticks.size(), sink.queriedPrices.size());
	for (std::size_t i = 0; i < sink.ticks.size(); ++i)
		EXPECT_DOUBLE_EQ(sink.ticks[i].price, sink.queriedPrices[i]);
}

TEST_F(R103LastTickBoundaryTest, AdjustedSyntheticBarIsNotUsedAsRawTick)
{
	AdjustedBarsLoader loader;
	HisDataReplayer replayer;
	AdjustedBarQueryingSink sink;
	sink.rp = &replayer;
	sink.loader = &loader;
	r91::CfgOpts opts;
	opts.mode = "csv";
	opts.tick = true;
	replayer.set_time_range(202001020930ull, 202001031500ull);
	WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020930ull, 202001031500ull, opts);
	ASSERT_TRUE(replayer.init(cfg, nullptr, &loader));
	cfg->release();
	replayer.register_sink(&sink, "r103_adjusted_synthetic");
	ASSERT_TRUE(replayer.prepare());
	const std::string adjusted = std::string(kCode) + "+";
	replayer.sub_tick(1u, kCode);
	WTSKlineSlice* bars = replayer.get_kline_slice(adjusted.c_str(), "d", 10, 1, true);
	ASSERT_NE(nullptr, bars);
	bars->release();
	replayer.run(false);
	ASSERT_FALSE(sink.ticks.empty());
	ASSERT_EQ(sink.ticks.size(), sink.rawPrices.size());
	std::size_t adjustedCount = 0;
	for (std::size_t i = 0; i < sink.ticks.size(); ++i)
	{
		if (sink.ticks[i].price >= 19.0)
		{
			++adjustedCount;
			EXPECT_DOUBLE_EQ(10.0, sink.rawPrices[i]);
		}
	}
	EXPECT_GE(adjustedCount, 4u);
}

TEST_F(R103LastTickBoundaryTest, ReplayDoesNotUseStaleHistorySnapshot)
{
	TickLoader loader;
	loader.ticks = {r91::mk_tick(kDate, 931u, 100.0, 100.0)};
	HisDataReplayer replayer;
	QueryingSink sink;
	sink.rp = &replayer;
	ASSERT_TRUE(init(202001020930ull, loader, replayer, sink));
	EXPECT_EQ(nullptr, replayer.get_last_tick(kCode));
	loader.ticks = {r91::mk_tick(kDate, 931u, 10.0, 100.0)};
	replayer.sub_tick(1u, kCode);
	replayer.run(false);
	ASSERT_EQ(1u, sink.queriedPrices.size());
	EXPECT_DOUBLE_EQ(10.0, sink.queriedPrices[0]);
}

TEST_F(R103LastTickBoundaryTest, FilteredTickIsNotVisibleDuringOtherStreamCallback)
{
	TickLoader loader;
	WTSTickStruct invalid = r91::mk_tick(kDate, 931u, 99.0, 100.0);
	invalid.action_time = 93160000u; // invalid second 60, rejected by replay
	loader.ticks = {invalid, r91::mk_tick(kDate, 933u, 12.0, 200.0)};
	WTSOrdDtlStruct detail = r91::mk_orddtl(kDate, 931u, 10.0, 1u);
	detail.action_time = invalid.action_time;
	ASSERT_TRUE(r91::write_orddtl(_base, "SSE", "000001", kDate, {detail}));
	r91::CfgOpts opts;
	opts.mode = "bin";
	opts.tick = true;
	HisDataReplayer replayer;
	replayer.set_time_range(202001020930ull, 202001021500ull);
	WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020930ull, 202001021500ull, opts);
	ASSERT_TRUE(replayer.init(cfg, nullptr, &loader));
	cfg->release();
	DetailQueryingSink sink;
	sink.rp = &replayer;
	replayer.register_sink(&sink, "r103_filtered_tick");
	ASSERT_TRUE(replayer.prepare());
	replayer.sub_tick(1u, kCode);
	replayer.sub_order_detail(2u, kCode);
	replayer.run(false);
	ASSERT_TRUE(sink.queried);
	EXPECT_FALSE(sink.gotTick);
}

TEST_F(R103LastTickBoundaryTest, AfterTailReturnsTheDocumentedEmptyValue)
{
	TickLoader loader;
	loader.ticks = make_ticks();
	HisDataReplayer replayer;
	r91::Sink sink;
	ASSERT_TRUE(init(202001020935ull, loader, replayer, sink));

	EXPECT_EQ(nullptr, replayer.get_last_tick(kCode));
	EXPECT_EQ(1u, loader.calls);
}
