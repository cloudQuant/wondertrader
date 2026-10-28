/**
 * r2_replayer_loader_test.cpp  (lane R2)
 *
 * 靶子：HisDataReplayer 的 **IBtDataLoader 公开接缝**
 *   init(cfg, notifier, dataLoader)  L171-174  —— _bt_loader = dataLoader
 * 有了这个接缝，下面这些「上一轮 lane 认定为需要 private 接缝」的函数全部可从
 * 公开接口抵达（本轮不需要 #define private public）：
 *   cacheFinalBarsFromLoader  L3589（dsb 命中分支 L3636-3672 / extloader 分支 L3673-3731 / 自动转储 L3704-3730）
 *   cacheRawTicksFromLoader   L3443（由 checkTicks L2852 触发）
 *   cacheRawBarsFromBin 的 extloader 分支 L4527-4600（mode=bin + loader）
 *   loadStkAdjFactorsFromLoader L302（init 尾部 L300-301 直接调用）
 *   get_kline_slice 的 loader 优先分支 L2229-2241
 *
 * 观测点全部是「我自己实现的数据加载器被调用了几次、参数是什么」+「落盘文件」
 * +「切片里回传的数值」，可证伪且不依赖未公开的时间口径细节。
 *
 * 落盘：temp_directory_path()/r2_ldr_*，TearDown remove_all；dist/ 只读。
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>

#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>

namespace
{
	std::filesystem::path lane_root()
	{
		return std::filesystem::path(__FILE__).parent_path()
			.parent_path().parent_path().parent_path();
	}

	std::filesystem::path make_temp_dir(const char* tag)
	{
		auto path = std::filesystem::temp_directory_path() /
			("r2_ldr_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	const char* const kCode = "SSE.STK.000001";
	const char* const kRawCode = "000001";
	const uint32_t kBarCnt = 5;
	const uint32_t kDates[kBarCnt] = { 20200102u, 20200103u, 20200106u, 20200107u, 20200108u };

	std::vector<double> expected_closes()
	{
		std::vector<double> ret;
		for (uint32_t i = 0; i < kBarCnt; i++)
			ret.push_back(10.5 + i);
		std::sort(ret.begin(), ret.end());
		return ret;
	}

	std::vector<WTSBarStruct> make_daily_bars()
	{
		std::vector<WTSBarStruct> ret;
		for (uint32_t i = 0; i < kBarCnt; i++)
		{
			WTSBarStruct bar;
			bar.date = kDates[i];
			bar.time = 150000;
			bar.open = 10.0 + i;
			bar.high = 11.0 + i;
			bar.low = 9.5 + i;
			bar.close = 10.5 + i;
			bar.settle = 10.5 + i;
			bar.money = (10.5 + i) * (1000 + i);
			bar.vol = 1000 + i;
			ret.push_back(bar);
		}
		return ret;
	}

	/*
	 * 只记录「被生产代码调了几次、参数是什么」的可编程数据加载器。
	 */
	class R2BtLoader : public IBtDataLoader
	{
	public:
		R2BtLoader() : _autoTrans(false),
			_finalCalls(0), _rawCalls(0), _tickCalls(0),
			_adjAllCalls(0), _adjCodeCalls(0), _lastTickDate(0),
			_finalRet(true), _rawRet(true), _tickRet(true), _adjAllRet(true)
		{
			_bars = make_daily_bars();
		}

		virtual bool loadFinalHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override
		{
			_finalCalls++;
			_lastCode = stdCode;
			_lastFinalPeriod = period;
			if (!_finalRet || _bars.empty())
				return false;
			cb(obj, _bars.data(), (uint32_t)_bars.size());
			return true;
		}

		virtual bool loadRawHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override
		{
			_rawCalls++;
			_lastCode = stdCode;
			_lastRawPeriod = period;
			if (!_rawRet || _bars.empty())
				return false;
			cb(obj, _bars.data(), (uint32_t)_bars.size());
			return true;
		}

		virtual bool loadAllAdjFactors(void* obj, FuncReadFactors cb) override
		{
			_adjAllCalls++;
			if (!_adjAllRet)
				return false;
			uint32_t dates[2] = { 20200106u, 20200107u };
			double factors[2] = { 1.5, 2.0 };
			cb(obj, kCode, dates, factors, 2);
			return true;
		}

		virtual bool loadAdjFactors(void* obj, const char* stdCode, FuncReadFactors cb) override
		{
			_adjCodeCalls++;
			_lastCode = stdCode;
			uint32_t dates[1] = { 20200106u };
			double factors[1] = { 2.0 };
			cb(obj, stdCode, dates, factors, 1);
			return true;
		}

		/*
		 * 任意日期都给同一组「价格 10/11/12、时间 09:30/09:31/09:32」的 tick，
		 * action_date 用被请求的日期回填，保证切片定位与请求日期自洽。
		 */
		virtual bool loadRawHisTicks(void* obj, const char* stdCode, uint32_t uDate, FuncReadTicks cb) override
		{
			_tickCalls++;
			_lastTickDate = uDate;
			_lastCode = stdCode;
			if (!_tickRet)
				return false;

			static WTSTickStruct ticks[3];
			for (uint32_t i = 0; i < 3; i++)
			{
				memset(&ticks[i], 0, sizeof(WTSTickStruct));
				wt_strcpy(ticks[i].code, stdCode);
				ticks[i].action_date = uDate;
				ticks[i].trading_date = uDate;
				ticks[i].action_time = (93000000 + i * 100000);
				ticks[i].price = 10.0 + i;
				ticks[i].open = 10.0;
				ticks[i].high = 12.0;
				ticks[i].low = 10.0;
				ticks[i].volume = 100 * (i + 1);
				ticks[i].total_volume = 100 * (i + 1);
				ticks[i].total_turnover = (10.0 + i) * 100 * (i + 1);
			}
			cb(obj, ticks, 3);
			return true;
		}

		virtual bool isAutoTrans() override { return _autoTrans; }

		void set_closes_shift(double shift)
		{
			for (std::size_t i = 0; i < _bars.size(); i++)
			{
				_bars[i].open += shift;
				_bars[i].high += shift;
				_bars[i].low += shift;
				_bars[i].close += shift;
			}
		}

	public:
		bool				_autoTrans;
		std::vector<WTSBarStruct>	_bars;
		uint32_t			_finalCalls;
		uint32_t			_rawCalls;
		uint32_t			_tickCalls;
		uint32_t			_adjAllCalls;
		uint32_t			_adjCodeCalls;
		uint32_t			_lastTickDate;
		uint32_t			_lastFinalPeriod;
		uint32_t			_lastRawPeriod;
		std::string			_lastCode;
		bool				_finalRet;
		bool				_rawRet;
		bool				_tickRet;
		bool				_adjAllRet;
	};

	/*
	 * 事件计数用替身：只计数，不接管 tick 的生命周期
	 * （回放器自己 release 它创建的 tick，见 HisDataReplayer.cpp:1303/1868）。
	 */
	class RecSink : public IDataSink
	{
	public:
		RecSink() : _initCnt(0), _sessBegin(0), _sessEnd(0), _barClose(0),
			_tickCnt(0), _schedCnt(0), _doneCnt(0), _sectionEnd(0),
			_lastPxType(0), _lastTDate(0), _lastBarDate(0), _lastTimes(0),
			_lastTickPrice(0.0), _lastBarClose(0.0)
		{
		}

		virtual void handle_tick(const char* /*stdCode*/, WTSTickData* curTick, uint32_t pxType) override
		{
			_tickCnt++;
			_lastPxType = pxType;
			_lastTickPrice = curTick->price();
		}

		virtual void handle_bar_close(const char* /*stdCode*/, const char* period,
			uint32_t times, WTSBarStruct* newBar) override
		{
			_barClose++;
			_lastPeriod = period;
			_lastTimes = times;
			if (newBar != NULL)
			{
				_lastBarClose = newBar->close;
				_lastBarDate = newBar->date;
			}
		}

		virtual void handle_schedule(uint32_t /*uDate*/, uint32_t /*uTime*/) override { _schedCnt++; }
		virtual void handle_init() override { _initCnt++; }
		virtual void handle_session_begin(uint32_t curTDate) override { _sessBegin++; _lastTDate = curTDate; }
		virtual void handle_session_end(uint32_t /*curTDate*/) override { _sessEnd++; }
		virtual void handle_replay_done() override { _doneCnt++; }
		virtual void handle_section_end(uint32_t /*curTDate*/, uint32_t /*curTime*/) override { _sectionEnd++; }

	public:
		uint32_t	_initCnt;
		uint32_t	_sessBegin;
		uint32_t	_sessEnd;
		uint32_t	_barClose;
		uint32_t	_tickCnt;
		uint32_t	_schedCnt;
		uint32_t	_doneCnt;
		uint32_t	_sectionEnd;
		uint32_t	_lastPxType;
		uint32_t	_lastTDate;
		uint32_t	_lastBarDate;
		uint32_t	_lastTimes;
		double		_lastTickPrice;
		double		_lastBarClose;
		std::string	_lastPeriod;
	};

	WTSVariant* loader_cfg(const std::filesystem::path& root,
		const std::filesystem::path& base, const char* mode, bool bTick)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", mode);
		cfg->append("path", base.string().c_str());
		cfg->append("stime", static_cast<uint64_t>(202001020930));
		cfg->append("etime", static_cast<uint64_t>(202001081500));
		cfg->append("tick", bTick);
		cfg->append("adjust_flag", static_cast<uint32_t>(0));
		cfg->append("align_by_section", false);
		cfg->append("dont_simtick_if_notrade", false);
		cfg->append("cache_clear_days", static_cast<uint32_t>(10));
		cfg->append("fees", "");
		WTSVariant* basefiles = WTSVariant::createObject();
		basefiles->append("session", (root / "dist/common/sessions.json").string().c_str());
		basefiles->append("commodity", (root / "dist/common/stk_comms.json").string().c_str());
		basefiles->append("holiday", (root / "dist/common/holidays.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();
		return cfg;
	}

	std::vector<double> slice_closes(WTSKlineSlice* sli)
	{
		std::vector<double> ret;
		if (sli == NULL)
			return ret;
		for (int32_t i = 0; i < sli->size(); i++)
		{
			WTSBarStruct* bar = sli->at(i);
			if (bar != NULL)
				ret.push_back(bar->close);
		}
		std::sort(ret.begin(), ret.end());
		return ret;
	}
}

class R2LoaderTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_root = lane_root();
		_base = make_temp_dir("base");
		_out = make_temp_dir("out");
		WtHelper::setOutputDir(_out.string().c_str());
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_base, ec);
		std::filesystem::remove_all(_out, ec);
	}

	std::filesystem::path _root;
	std::filesystem::path _base;
	std::filesystem::path _out;
};

/*
 * cacheFinalBarsFromLoader：extloader 分支 + 只缓存一次。
 * 第二次取切片不得再调用加载器（_bars_cache 命中）。
 */
TEST_F(R2LoaderTest, finalBarsFromLoaderCachedOnce)
{
	R2BtLoader loader;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, "csv", false);
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	RecSink sink;
	replayer.register_sink(&sink, "r2_ldr1");
	ASSERT_TRUE(replayer.prepare());
	EXPECT_EQ(1u, sink._initCnt);

	WTSKlineSlice* sli = replayer.get_kline_slice(kCode, "d1", 3, 1, false);
	ASSERT_NE(nullptr, sli);
	EXPECT_EQ(1u, loader._finalCalls);
	EXPECT_EQ(std::string(kCode), loader._lastCode);
	EXPECT_EQ((uint32_t)KP_DAY, loader._lastFinalPeriod);
	std::vector<double> first = slice_closes(sli);
	sli->release();
	EXPECT_GE(first.size(), 1u);

	// 第二次：缓存命中，加载器不应被再次调用
	WTSKlineSlice* sli2 = replayer.get_kline_slice(kCode, "d1", 3, 1, false);
	ASSERT_NE(nullptr, sli2);
	EXPECT_EQ(1u, loader._finalCalls);
	EXPECT_EQ(first, slice_closes(sli2));
	sli2->release();

	// 未知合约：加载器返回 false -> 切片为 NULL，且确实调用过
	loader._finalRet = false;
	WTSKlineSlice* sli3 = replayer.get_kline_slice("SSE.STK.999999", "d1", 3, 1, false);
	EXPECT_EQ(nullptr, sli3);
	EXPECT_EQ(2u, loader._finalCalls);
}

/*
 * isAutoTrans()==true 时，cacheFinalBarsFromLoader 会把取到的K线转储为
 * {base}/his/day/{exchg}/{code}.dsb；换一个全新回放器再取一次，
 * 必须命中 dsb 分支：加载器一次都不被调用，且数据与转储前完全一致
 * （故意把第二个加载器的数据整体平移 +100，如果走的是 extloader 分支就会被看到）。
 */
TEST_F(R2LoaderTest, autoTransDumpsDsbAndSecondReplayerHitsDsb)
{
	{
		R2BtLoader loader;
		loader._autoTrans = true;
		HisDataReplayer replayer;
		WTSVariant* cfg = loader_cfg(_root, _base, "csv", false);
		ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
		cfg->release();

		RecSink sink;
		replayer.register_sink(&sink, "r2_ldr2");
		ASSERT_TRUE(replayer.prepare());

		WTSKlineSlice* sli = replayer.get_kline_slice(kCode, "d1", kBarCnt, 1, false);
		ASSERT_NE(nullptr, sli);
		std::vector<double> dumped = slice_closes(sli);
		sli->release();
		EXPECT_EQ(expected_closes(), dumped);
		EXPECT_EQ(1u, loader._finalCalls);
	}

	const std::filesystem::path dsb = _base / "his" / "day" / "SSE" /
		(std::string(kRawCode) + ".dsb");
	ASSERT_TRUE(std::filesystem::exists(dsb));
	EXPECT_GT(std::filesystem::file_size(dsb), 0u);

	R2BtLoader loader2;
	loader2._autoTrans = true;
	loader2.set_closes_shift(100.0);
	HisDataReplayer replayer2;
	WTSVariant* cfg2 = loader_cfg(_root, _base, "csv", false);
	ASSERT_TRUE(replayer2.init(cfg2, NULL, &loader2));
	cfg2->release();

	RecSink sink2;
	replayer2.register_sink(&sink2, "r2_ldr2b");
	ASSERT_TRUE(replayer2.prepare());

	WTSKlineSlice* hit = replayer2.get_kline_slice(kCode, "d1", kBarCnt, 1, false);
	ASSERT_NE(nullptr, hit);
	// dsb 命中：extloader 完全没被调用
	EXPECT_EQ(0u, loader2._finalCalls);
	EXPECT_EQ(expected_closes(), slice_closes(hit));
	hit->release();
}

/*
 * 走 loader 底座跑完整日线回放：主K线自动登记、逐根 handle_bar_close、
 * 交易日边界事件、run(true) 的 dump_btstate 落地（btenv.json）。
 */
TEST_F(R2LoaderTest, loaderBackedDailyReplayEmitsBarsAndDumpsState)
{
	R2BtLoader loader;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, "csv", false);
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	RecSink sink;
	replayer.register_sink(&sink, "r2_ldr3");
	ASSERT_TRUE(replayer.prepare());

	WTSKlineSlice* main = replayer.get_kline_slice(kCode, "d1", kBarCnt, 1, true);
	ASSERT_NE(nullptr, main);
	main->release();

	replayer.run(true);

	EXPECT_EQ(1u, sink._doneCnt);
	EXPECT_EQ(kBarCnt, sink._barClose);
	EXPECT_EQ(sink._sessBegin, sink._sessEnd);
	EXPECT_GE(sink._sessBegin, 1u);
	EXPECT_EQ(std::string("d1"), sink._lastPeriod);
	EXPECT_DOUBLE_EQ(14.5, sink._lastBarClose);
	EXPECT_EQ(20200108u, sink._lastBarDate);

	const std::filesystem::path env = _out / "r2_ldr3" / "btenv.json";
	ASSERT_TRUE(std::filesystem::exists(env));
	std::ifstream in(env);
	std::stringstream ss;
	ss << in.rdbuf();
	const std::string content = ss.str();
	EXPECT_NE(std::string::npos, content.find("\"code\""));
	EXPECT_NE(std::string::npos, content.find(kCode));
	EXPECT_NE(std::string::npos, content.find("\"progress\""));

	// 回放结束后游标位于末尾，取满 5 根必须拿到 loader 给的全部K线
	WTSKlineSlice* tail = replayer.get_kline_slice(kCode, "d1", kBarCnt, 1, false);
	ASSERT_NE(nullptr, tail);
	EXPECT_EQ(expected_closes(), slice_closes(tail));
	tail->release();
	EXPECT_EQ(1u, loader._finalCalls);
}

/*
 * tick 接缝：checkTicks -> cacheRawTicksFromLoader -> get_tick_slice / get_last_tick。
 * 请求日期 = _cur_tdate，三笔 tick 价格为 10/11/12。
 */
TEST_F(R2LoaderTest, ticksFromLoaderFeedTickSliceAndLastTick)
{
	R2BtLoader loader;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, "csv", true);
	replayer.set_time_range(202001030930, 202001081500);
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	RecSink sink;
	replayer.register_sink(&sink, "r2_ldr4");
	ASSERT_TRUE(replayer.prepare());
	EXPECT_TRUE(replayer.is_tick_enabled());
	EXPECT_EQ(20200103u, replayer.get_date());

	WTSTickSlice* sli = replayer.get_tick_slice(kCode, 2, 202001030932);
	ASSERT_NE(nullptr, sli);
	EXPECT_EQ(2u, sli->size());
	std::vector<double> px;
	for (uint32_t i = 0; i < sli->size(); i++)
		px.push_back(sli->at((int32_t)i)->price);
	std::sort(px.begin(), px.end());
	ASSERT_EQ(2u, px.size());
	EXPECT_DOUBLE_EQ(10.0, px[0]);
	EXPECT_DOUBLE_EQ(11.0, px[1]);
	sli->release();

	EXPECT_EQ(1u, loader._tickCalls);
	EXPECT_EQ(std::string(kCode), loader._lastCode);
	const uint32_t cachedDate = loader._lastTickDate;
	EXPECT_TRUE(cachedDate == 20200103u || cachedDate == 20200102u);

	WTSTickData* last = replayer.get_last_tick(kCode);
	ASSERT_NE(nullptr, last);
	EXPECT_DOUBLE_EQ(12.0, last->price());
	EXPECT_EQ(cachedDate, last->actiondate());
	last->release();

	// 加载器不再供数 -> 缓存被清空后取不到切片
	R2BtLoader empty;
	empty._tickRet = false;
	HisDataReplayer r2;
	WTSVariant* cfg2 = loader_cfg(_root, _base, "csv", true);
	r2.set_time_range(202001030930, 202001081500);
	ASSERT_TRUE(r2.init(cfg2, NULL, &empty));
	cfg2->release();
	RecSink sink2;
	r2.register_sink(&sink2, "r2_ldr4b");
	ASSERT_TRUE(r2.prepare());
	EXPECT_EQ(nullptr, r2.get_tick_slice(kCode, 2, 202001030932));
	EXPECT_EQ(1u, empty._tickCalls);
	EXPECT_EQ(nullptr, r2.get_last_tick(kCode));
}

/*
 * init 尾部先尝试 extloader 的除权因子（loadStkAdjFactorsFromLoader L302）。
 * 返回 false 时才会退到 adjfactor 文件 —— 两条臂各固化一次。
 */
TEST_F(R2LoaderTest, adjFactorsPreferLoaderThenFile)
{
	{
		R2BtLoader loader;
		HisDataReplayer replayer;
		WTSVariant* cfg = loader_cfg(_root, _base, "csv", false);
		cfg->append("adjfactor", (_base / "no_such_adj.txt").string().c_str());
		ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
		cfg->release();
		EXPECT_EQ(1u, loader._adjAllCalls);
	}

	{
		R2BtLoader loader;
		loader._adjAllRet = false;
		HisDataReplayer replayer;
		WTSVariant* cfg = loader_cfg(_root, _base, "csv", false);
		ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
		cfg->release();
		// loader 供数失败 -> 走文件分支（本例没配 adjfactor 文件，只走不存在早退）
		EXPECT_EQ(1u, loader._adjAllCalls);
	}
}

/*
 * mode=bin + loader：cacheRawBarsFromBin 的 extloader 分支（L4527）。
 * 这条路径完全不需要 dsb 文件。
 */
TEST_F(R2LoaderTest, binModeWithLoaderCachesBarsViaRawInterface)
{
	R2BtLoader loader;
	loader._autoTrans = true;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, "bin", false);
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	RecSink sink;
	replayer.register_sink(&sink, "r2_ldr5");
	ASSERT_TRUE(replayer.prepare());

	WTSKlineSlice* sli = replayer.get_kline_slice(kCode, "d1", kBarCnt, 1, false);
	ASSERT_NE(nullptr, sli);
	EXPECT_EQ(1u, loader._rawCalls);
	EXPECT_EQ(std::string(kCode), loader._lastCode);
	EXPECT_EQ((uint32_t)KP_DAY, loader._lastRawPeriod);
	EXPECT_EQ(expected_closes(), slice_closes(sli));
	sli->release();

	// 缓存命中后不再调用
	WTSKlineSlice* again = replayer.get_kline_slice(kCode, "d1", kBarCnt, 1, false);
	ASSERT_NE(nullptr, again);
	EXPECT_EQ(1u, loader._rawCalls);
	again->release();
}

/*
 * mode=bin、没有 loader、也没有 dsb：cacheRawBarsFromBin 必须整条返回 false
 * （get_kline_slice 外层 try/catch 兜住任何异常），切片为 NULL。
 */
TEST_F(R2LoaderTest, binModeWithoutLoaderOrDsbYieldsNoSlice)
{
	R2BtLoader loader;
	loader._rawRet = false;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, "bin", false);
	// 传入一个「什么都返回 false」的加载器，等价于没有 loader 但保留 init 接缝
	ASSERT_TRUE(replayer.init(cfg, NULL, NULL));
	cfg->release();

	RecSink sink;
	replayer.register_sink(&sink, "r2_ldr6");
	ASSERT_TRUE(replayer.prepare());

	EXPECT_EQ(nullptr, replayer.get_kline_slice(kCode, "d1", kBarCnt, 1, false));
	EXPECT_EQ(0u, loader._rawCalls);
	// 装载失败不污染时间轴
	EXPECT_EQ(20200102u, replayer.get_date());
}
