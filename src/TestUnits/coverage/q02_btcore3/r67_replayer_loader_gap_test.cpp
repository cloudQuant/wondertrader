/**
 * r67_replayer_loader_gap_test.cpp  (lane R67)
 *
 * 靶子: src/WtBtCore/HisDataReplayer.cpp 的 IBtDataLoader 公开接缝上与 R2/p07 不重叠的分支：
 *   - cacheFinalBarsFromLoader(L3589) 的 dsb 尺寸校验失败(L3644-3650)、
 *     isAutoTrans()==false 时忽略已有 dsb(L3634)、bSubbed=false 的未订阅K线臂(L3674-3688)
 *   - get_kline_slice 对 loader 回来的 m5 再做小时重采样(L2298-2326)
 *   - run_by_ticks 被 stop() 终止(L741-769)
 *   - simTickWithUnsubBars(L1389) 的日线与分钟两条未订阅分支（数据来自 loader）
 *   - check_cache_days(L4607) 的 _untouch_days 淘汰与二次装载
 * 只用 IBtDataLoader（WtBtCore 自有接口）与公开 API；不触真实存储引擎/DLL/线程。
 * 落盘仅 temp_directory_path()/r67_*，TearDown 清理。
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <WtBtCore/HisDataReplayer.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>

#include "../p07_btcore_replayer2/r67_common.h"

namespace
{
	const char* const kCode = "SSE.STK.600000";
	const char* const kCodeB = "SSE.STK.600001";

	const uint32_t kDates[] = { 20200102u, 20200103u, 20200106u, 20200107u, 20200108u };

	std::vector<WTSBarStruct> daily_series(const char* code, double basePx, uint32_t count)
	{
		(void)code;
		std::vector<WTSBarStruct> bars;
		for (uint32_t i = 0; i < count; i++)
			bars.push_back(r67::mk_bar(kDates[i], 1500u,
				basePx + i, basePx + 1.0 + i, basePx - 0.5 + i, basePx + 0.5 + i, 100.0 + 10.0 * i));
		return bars;
	}

	std::vector<WTSBarStruct> m5_series(double basePx)
	{
		std::vector<WTSBarStruct> bars;
		const uint32_t times[] = { 935u, 940u, 945u, 950u, 955u, 1000u, 1005u, 1010u };
		for (uint32_t i = 0; i < sizeof(times) / sizeof(times[0]); i++)
			bars.push_back(r67::mk_bar(20200102u, times[i],
				basePx + i, basePx + 0.4 + i, basePx - 0.1 + i, basePx + 0.2 + i, 100.0 * (i + 1)));
		return bars;
	}

	std::vector<WTSBarStruct> m1_series(double basePx)
	{
		std::vector<WTSBarStruct> bars;
		const uint32_t times[] = { 932u, 933u, 934u };
		for (uint32_t i = 0; i < sizeof(times) / sizeof(times[0]); i++)
			bars.push_back(r67::mk_bar(20200102u, times[i],
				basePx + i, basePx + 0.2 + i, basePx - 0.1 + i, basePx + 0.1 + i, 10.0 + i));
		return bars;
	}

	std::vector<double> closes_of(const WTSKlineSlice* slice)
	{
		std::vector<double> ret;
		if (slice == NULL)
			return ret;
		for (int32_t i = 0; i < slice->size(); i++)
		{
			const WTSBarStruct* bar = slice->at(i);
			if (bar != NULL)
				ret.push_back(bar->close);
		}
		std::sort(ret.begin(), ret.end());
		return ret;
	}

	uint32_t ticks_of(const r67::Sink& sink, const char* code)
	{
		uint32_t cnt = 0;
		for (const r67::Sink::TickRec& rec : sink.ticks)
		{
			if (rec.code == code)
				cnt++;
		}
		return cnt;
	}
}

class R67LoaderGap : public r67::Fixture
{
};

// autoTrans=true 且已存在同名 dsb 但尺寸小于块头：走错误分支后回落到 loader，
// 并把正确数据重新转储
TEST_F(R67LoaderGap, ShortAutoTransDsbFallsBackToLoader)
{
	ASSERT_TRUE(r67::write_text(_base / "his" / "day" / "SSE" / "000001.dsb", "short"));

	r67::Loader loader;
	loader.autoTrans = true;
	loader.dayBars[kCode] = daily_series(kCode, 10.0, 3);

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001081500ull, false);
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	r67::Sink sink;
	replayer.register_sink(&sink, "r67_shortldr");
	ASSERT_TRUE(replayer.prepare());

	WTSKlineSlice* slice = replayer.get_kline_slice(kCode, "d", 3, 1, true);
	ASSERT_NE(nullptr, slice);
	// 初始定位吃掉首个交易日，cursor=1，故只返回 1 根；数值来自 loader 而非短 dsb
	const std::vector<double> expect = { 10.5 };
	EXPECT_EQ(expect, closes_of(slice));
	slice->release();
	EXPECT_EQ(1u, loader.finalCalls[kCode]);
}

// isAutoTrans()==false 时即便 dsb 存在也不读取，必须走 loader；
// dsb 里故意放 +100 的价格，若被误读会立即暴露
TEST_F(R67LoaderGap, AutoTransFalseIgnoresExistingDsb)
{
	std::vector<WTSBarStruct> dumpBars = daily_series(kCode, 100.0, 3);
	ASSERT_TRUE(r67::write_kline_dsb(_base / "his" / "day" / "SSE" / "000001.dsb",
		BT_HIS_Day, dumpBars));

	r67::Loader loader;
	loader.autoTrans = false;
	loader.dayBars[kCode] = daily_series(kCode, 10.0, 3);

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001081500ull, false);
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	r67::Sink sink;
	replayer.register_sink(&sink, "r67_noautotrans");
	ASSERT_TRUE(replayer.prepare());

	WTSKlineSlice* slice = replayer.get_kline_slice(kCode, "d", 3, 1, true);
	ASSERT_NE(nullptr, slice);
	// dsb 里是 100.5；拿到 10.5 证明 isAutoTrans=false 时确实忽略了已有 dsb
	const std::vector<double> expect = { 10.5 };
	EXPECT_EQ(expect, closes_of(slice));
	slice->release();
	EXPECT_EQ(1u, loader.finalCalls[kCode]);
}

// loader 只给 m5、请求 "h"：cacheFinalBarsFromLoader 先按 KP_Minute5 装载，
// 再由 get_kline_slice 做小时重采样
TEST_F(R67LoaderGap, HourResampledFromLoaderMinuteBars)
{
	r67::Loader loader;
	loader.autoTrans = false;
	loader.m5Bars[kCode] = m5_series(10.0);

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001081500ull, false);
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	r67::Sink sink;
	replayer.register_sink(&sink, "r67_ldrhour");
	ASSERT_TRUE(replayer.prepare());

	WTSKlineSlice* slice = replayer.get_kline_slice(kCode, "h", 1, 1, true);
	ASSERT_NE(nullptr, slice);
	EXPECT_GT(slice->size(), 0);
	slice->release();
	EXPECT_EQ(1u, loader.finalCalls[kCode]);
	EXPECT_EQ((uint32_t)KP_Minute5, (uint32_t)loader.lastFinalPeriod);
}

// tick 回放中由回调 stop()：replayHftDatasByDay 的 !_terminated 条件退出，
// 当日 session_end 与 replay_done 仍各发一次
TEST_F(R67LoaderGap, TickReplayStopsOnTerminate)
{
	r67::Loader loader;
	loader.ticks[std::string(kCode) + "#20200102"] = {
		r67::mk_tick({ 20200102u, 931u, 10.0, 100.0 }),
		r67::mk_tick({ 20200102u, 932u, 11.0, 100.0 }),
		r67::mk_tick({ 20200102u, 933u, 12.0, 100.0 }),
	};

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001031500ull, true);
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	r67::Sink sink;
	sink.rp = &replayer;
	sink.onTick = [&replayer]() { replayer.stop(); };
	replayer.register_sink(&sink, "r67_tickstop");
	ASSERT_TRUE(replayer.prepare());
	replayer.sub_tick(1, kCode);

	replayer.run(false);

	EXPECT_EQ(1u, sink.ticks.size());
	EXPECT_EQ(1u, sink.beginCalls);
	EXPECT_EQ(1u, sink.endCalls);
	EXPECT_EQ(1u, sink.doneCalls);
}

// tick 关闭时 sub_tick 把 B 放进 _unsubbed_in_need，checkUnbars 以 bSubbed=false
// 从 loader 装载日线，run 中 simTickWithUnsubBars 的日线分支按开高低收喂 4 笔
TEST_F(R67LoaderGap, UnsubscribedDailyBarsViaLoaderFeedSimTicks)
{
	r67::Loader loader;
	loader.autoTrans = false;
	loader.dayBars[kCode] = daily_series(kCode, 10.0, 3);
	loader.dayBars[kCodeB] = daily_series(kCodeB, 20.0, 3);

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001061500ull, false);
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	r67::Sink sink;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice(kCode, "d", 1, 1, true);
		if (main != NULL)
			main->release();
		replayer.sub_tick(9, kCodeB);
	};
	replayer.register_sink(&sink, "r67_unsubday");
	ASSERT_TRUE(replayer.prepare());

	replayer.run(false);

	const uint32_t unsubTicks = ticks_of(sink, kCodeB);
	EXPECT_GT(unsubTicks, 0u);
	EXPECT_EQ(0u, unsubTicks % 4);
	EXPECT_GE(loader.finalCalls[kCodeB], 1u);
}

// 主K线为 m1、B 未订阅：checkUnbars 装 min1 到 _unbars_cache，
// simTickWithUnsubBars 的分钟分支与 onMinuteEnd 的未订阅游标推进同时被驱动
TEST_F(R67LoaderGap, UnsubscribedMinuteBarsViaLoaderAdvanceCursor)
{
	r67::Loader loader;
	loader.autoTrans = false;
	loader.m1Bars[kCode] = m1_series(10.0);
	loader.m1Bars[kCodeB] = m1_series(20.0);

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001021500ull, false);
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	r67::Sink sink;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice(kCode, "m", 1, 1, true);
		if (main != NULL)
			main->release();
		replayer.sub_tick(9, kCodeB);
	};
	replayer.register_sink(&sink, "r67_unsubmin");
	ASSERT_TRUE(replayer.prepare());

	replayer.run(false);

	const uint32_t unsubTicks = ticks_of(sink, kCodeB);
	EXPECT_GT(unsubTicks, 0u);
	EXPECT_EQ(0u, unsubTicks % 4);
	EXPECT_GE(loader.finalCalls[kCodeB], 1u);
}

// cache_clear_days=1：非主K线 B 在首个 session_begin 被 check_cache_days 淘汰，
// 下一交易日 session_begin 时重新请求 B 必须重新走 loader（调用次数 1 -> 2）。
// 不能等 run() 结束后再请求：run_by_bars 收尾后 _cur_tdate 已越过 B 的最后一根日线，
// get_kline_slice 的首次定位 lower_bound 返回 end 后按契约返回 NULL
// （HisDataReplayer.cpp:2396-2405），无法区分「被淘汰」与「已过期末」。
TEST_F(R67LoaderGap, CacheClearDaysEvictsUntouchedBars)
{
	r67::Loader loader;
	loader.autoTrans = false;
	loader.dayBars[kCode] = daily_series(kCode, 10.0, 5);
	loader.dayBars[kCodeB] = daily_series(kCodeB, 20.0, 5);

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001081500ull,
		false, 1, false);
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	// 首个 session_begin 之前的 check_cache_days 已将 B 淘汰；到第二次 session_begin
	// （此时 _cur_tdate 仍停在上一交易日、_cur_time 是开盘时刻）重新请求 B，
	// 才会真正触发第二次 loader，且能正常定位到 bar。
	bool reloadTried = false;
	uint32_t sessionSeen = 0;
	int32_t reloadSize = -1;
	r67::Sink sink;
	sink.rp = &replayer;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice(kCode, "d", 5, 1, true);
		if (main != NULL)
			main->release();
		WTSKlineSlice* other = replayer.get_kline_slice(kCodeB, "d", 5, 1, false);
		if (other != NULL)
			other->release();
	};
	sink.onSessionBegin = [&](uint32_t) {
		++sessionSeen;
		if (sessionSeen != 2 || reloadTried)
			return;
		reloadTried = true;
		WTSKlineSlice* reload = replayer.get_kline_slice(kCodeB, "d", 5, 1, false);
		if (reload != NULL)
		{
			reloadSize = reload->size();
			reload->release();
		}
	};
	replayer.register_sink(&sink, "r67_evict");
	ASSERT_TRUE(replayer.prepare());

	replayer.run(false);
	ASSERT_TRUE(reloadTried);
	EXPECT_GT(reloadSize, 0);
	EXPECT_EQ(1u, loader.finalCalls[kCode]);
	// onInit 一次 + 淘汰后 session_begin 重装一次
	EXPECT_EQ(2u, loader.finalCalls[kCodeB]);
}

// DISABLED_: loader 的 loadFinalHisBars 返回 true 但一次回调都不给（count==0）时，
// cacheFinalBarsFromLoader 无条件读取 _bars[0]/_bars[_count-1]（L3699-3700），
// 空 vector 下标访问是未定义行为；见 bugs/R67.md BUG-R67-001。
TEST_F(R67LoaderGap, DISABLED_EmptyLoaderSuccessDerefsEmptyBars)
{
	GTEST_SKIP() << "cacheFinalBarsFromLoader reads _bars[0] when loader returns true with 0 rows";

	r67::Loader loader;
	loader.autoTrans = false;
	loader.emptyFinalSuccess = true;

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001081500ull, false);
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	r67::Sink sink;
	replayer.register_sink(&sink, "r67_emptyldr");
	ASSERT_TRUE(replayer.prepare());
	EXPECT_EQ(nullptr, replayer.get_kline_slice(kCode, "d", 3, 1, true));
}
