/**
 * r151_replayer_paths_test.cpp  (lane R151)
 *
 * 靶子：src/WtBtCore/HisDataReplayer.cpp 剩余公开分支
 *   1. 未显式指定主K线时 run() 的自动选主（L684-713，含同周期 _main_key.empty() L700-703）
 *   2. 日线 + tick=true 的模拟 tick 主链（L1243-1385、L1507-1508 的 low 比较臂）
 *   3. 主K线起始日晚于次K线时 simTicks 日线游标补齐（L1373-1378）
 *   4. 分钟主K线 + notifier 的 BT_STATE period 分支（L573）与 m5 游标回退（L1309-1316）
 *   5. cacheRawBarsFromCSV 每 1000 行日志（L3859-3861）
 *
 * 只走公开 API；不 #define private public、不镜像类布局、不改宏。
 * 不启动线程/不读挂钟；临时文件由 r151::ReplayFixture 在系统临时目录创建并清理。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <Includes/WTSDataDef.hpp>
#include <WtBtCore/EventNotifier.h>
#include <WtBtCore/HisDataReplayer.h>

#include "r151_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
	const char* const kCu = "SHFE.cu.2406";

	std::vector<WTSBarStruct> five_days(double base)
	{
		const uint32_t dates[5] = { 20200102u, 20200103u, 20200106u, 20200107u, 20200108u };
		std::vector<WTSBarStruct> bars;
		for (uint32_t i = 0; i < 5; i++)
		{
			const double close = base + i;
			bars.emplace_back(r151::mk_bar(dates[i], 0,
				close - 0.2, close + 0.4, close - 0.5, close, 100.0 + i));
		}
		return bars;
	}

	std::vector<WTSBarStruct> day_bars(const uint32_t* dates, uint32_t count, double base)
	{
		std::vector<WTSBarStruct> bars;
		for (uint32_t i = 0; i < count; i++)
		{
			const double close = base + i;
			bars.emplace_back(r151::mk_bar(dates[i], 0,
				close - 0.2, close + 0.4, close - 0.5, close, 100.0 + i));
		}
		return bars;
	}
}

class R151ReplayerPaths : public r151::ReplayFixture
{
};

// 未显式 isMain：prepare 后只把日线放进 _bars_cache，run() 自动选主并回放
TEST_F(R151ReplayerPaths, AutoMainKeyFromNonMainSlice)
{
	ASSERT_TRUE(r151::write_bars_csv(_base, std::string(kRb) + "_d", five_days(10.0), false));
	ASSERT_TRUE(r151::init_replay(_rp, _root, _base, 202001020930ull, 202001081500ull, false));

	r151::Sink sink;
	_rp.register_sink(&sink, "r151_auto_main");

	ASSERT_TRUE(_rp.prepare());

	// isMain=false：只装载/缓存，不设置 _main_key
	WTSKlineSlice* slice = _rp.get_kline_slice(kRb, "d", 10);
	ASSERT_NE(nullptr, slice);
	EXPECT_GT(slice->size(), 0u);
	slice->release();

	_rp.run();

	EXPECT_EQ(1u, sink.done_calls);
	EXPECT_GE(sink.bar_calls, 1u);
}

// 日线 + tick=true（无真实 tick 文件）：simTicks 按开高低收模拟 4 笔 tick
TEST_F(R151ReplayerPaths, DailyBarsSimulateOhlcTicks)
{
	ASSERT_TRUE(r151::write_bars_csv(_base, std::string(kRb) + "_d", five_days(20.0), false));
	ASSERT_TRUE(r151::init_replay(_rp, _root, _base, 202001020930ull, 202001081500ull, true));

	r151::Sink sink;
	_rp.register_sink(&sink, "r151_daily_sim");

	ASSERT_TRUE(_rp.prepare());
	WTSKlineSlice* slice = _rp.get_kline_slice(kRb, "d", 10, 1, true);
	ASSERT_NE(nullptr, slice);
	slice->release();

	_rp.run();

	// 至少 4 个交易日各 4 笔（首根可能作为历史）
	EXPECT_GE(sink.tick_codes.size(), 16u);
	EXPECT_GE(sink.bar_calls, 4u);
	ASSERT_FALSE(sink.tick_px_types.empty());
	for (uint32_t px : sink.tick_px_types)
		EXPECT_LT(px, 4u);
}

// 主K线 0102 仅作历史定位bar，实际回放首日 0106 晚于次K线起始 20191231：
// simTicks 先把次K线游标从 0103 推进到 0106（L1373-1378）
TEST_F(R151ReplayerPaths, DailySecondaryBarsCursorCatchesUp)
{
	const uint32_t mainDates[4] = { 20200102u, 20200106u, 20200107u, 20200108u };
	const uint32_t subDates[6] = { 20191231u, 20200102u, 20200103u, 20200106u, 20200107u, 20200108u };
	ASSERT_TRUE(r151::write_bars_csv(_base, std::string(kRb) + "_d",
		day_bars(mainDates, 4, 30.0), false));
	ASSERT_TRUE(r151::write_bars_csv(_base, std::string(kCu) + "_d",
		day_bars(subDates, 6, 60.0), false));
	ASSERT_TRUE(r151::init_replay(_rp, _root, _base, 202001020930ull, 202001081500ull, true));

	r151::Sink sink;
	_rp.register_sink(&sink, "r151_daily_gap");

	ASSERT_TRUE(_rp.prepare());
	WTSKlineSlice* mainSlice = _rp.get_kline_slice(kRb, "d", 10, 1, true);
	ASSERT_NE(nullptr, mainSlice);
	mainSlice->release();

	// 非主K线：仅缓存，供 simTicks 遍历时补齐游标
	WTSKlineSlice* subSlice = _rp.get_kline_slice(kCu, "d", 10);
	ASSERT_NE(nullptr, subSlice);
	subSlice->release();

	_rp.run();

	EXPECT_GE(sink.count_code(kRb), 8u);
	EXPECT_GE(sink.count_code(kCu), 8u);
	EXPECT_EQ(1u, sink.done_calls);
}

// 分钟主K线 + notifier：BT_STATE 的 KP_Minute1 分支；m5 次K线游标回退
TEST_F(R151ReplayerPaths, MinuteNotifierAndM5CursorBackoff)
{
	const std::vector<WTSBarStruct> m1 = {
		r151::mk_bar(20200102u, 930, 10.0, 10.2, 9.8, 10.1, 10.0),
		r151::mk_bar(20200102u, 931, 10.1, 10.3, 9.9, 10.2, 11.0),
		r151::mk_bar(20200102u, 932, 10.2, 10.4, 10.0, 10.3, 12.0)
	};
	const std::vector<WTSBarStruct> m5 = {
		r151::mk_bar(20200102u, 930, 10.0, 10.2, 9.8, 10.1, 10.0),
		r151::mk_bar(20200102u, 935, 10.1, 10.3, 9.9, 10.2, 11.0),
		r151::mk_bar(20200102u, 940, 10.2, 10.4, 10.0, 10.3, 12.0)
	};
	ASSERT_TRUE(r151::write_bars_csv(_base, std::string(kRb) + "_m1", m1, true));
	ASSERT_TRUE(r151::write_bars_csv(_base, std::string(kRb) + "_m5", m5, true));

	EventNotifier notifier;
	WTSVariant* ncfg = WTSVariant::createObject();
	ncfg->append("active", false);
	EXPECT_FALSE(notifier.init(ncfg));
	ncfg->release();

	ASSERT_TRUE(r151::init_replay(_rp, _root, _base, 202001020930ull, 202001021500ull, false, &notifier));

	r151::Sink sink;
	_rp.register_sink(&sink, "r151_minute_m5");
	ASSERT_TRUE(_rp.prepare());

	WTSKlineSlice* mainSlice = _rp.get_kline_slice(kRb, "m", 10, 1, true);
	ASSERT_NE(nullptr, mainSlice);
	mainSlice->release();

	WTSKlineSlice* m5Slice = _rp.get_kline_slice(kRb, "m", 10, 5);
	ASSERT_NE(nullptr, m5Slice);
	m5Slice->release();

	_rp.run();

	// 931/932 两根分钟线回放
	EXPECT_GE(sink.bar_calls, 2u);
	EXPECT_EQ(1u, sink.done_calls);
}

// 1001 根分钟线：cacheRawBarsFromCSV 在第 1000 行打印加载进度（L3859-3861）
TEST_F(R151ReplayerPaths, CsvLoadReportsEveryThousandBars)
{
	const uint32_t dates[5] = { 20200102u, 20200103u, 20200106u, 20200107u, 20200108u };
	std::vector<WTSBarStruct> bars;
	for (uint32_t d = 0; d < 5; d++)
	{
		for (uint32_t i = 0; i < 200; i++)
		{
			const uint32_t hhmm = 930u + (i / 60u) * 100u + (i % 60u);
			bars.emplace_back(r151::mk_bar(dates[d], hhmm,
				10.0 + i, 10.2 + i, 9.8 + i, 10.1 + i, 1.0));
		}
	}
	bars.emplace_back(r151::mk_bar(20200109u, 930, 10.0, 10.2, 9.8, 10.1, 1.0));
	ASSERT_EQ(1001u, bars.size());
	ASSERT_TRUE(r151::write_bars_csv(_base, std::string(kRb) + "_m1", bars, true));

	ASSERT_TRUE(r151::init_replay(_rp, _root, _base, 202001020930ull, 202001091500ull, false));

	// prepare() 在 L669 无条件回调 _listener->handle_init()（BUG-P04-002），
	// 回放前必须先 register_sink；这里顺带断言回调确实发生。
	r151::Sink sink;
	_rp.register_sink(&sink, "r151_csv_load");

	ASSERT_TRUE(_rp.prepare());
	EXPECT_EQ(1u, sink.init_calls);

	WTSKlineSlice* slice = _rp.get_kline_slice(kRb, "m", 5, 1, true);
	ASSERT_NE(nullptr, slice);
	EXPECT_GT(slice->size(), 0u);
	slice->release();

	// 装载过程会按约定生成 his/min1 目录
	EXPECT_TRUE(std::filesystem::exists(_base / "his" / "min1"));
}
