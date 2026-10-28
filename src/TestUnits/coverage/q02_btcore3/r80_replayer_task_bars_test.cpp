/**
 * r80_replayer_task_bars_test.cpp  (lane R80)
 *
 * 靶子：src/WtBtCore/HisDataReplayer.cpp 的分钟任务回放主体
 *   - L1104-L1140 入口 session_begin、timeToMinutes 对齐、逐分钟 simTicks + onMinuteEnd
 *   - L1141-L1164 收盘分支（mins 到达 getTradingMins）与 session_end
 *   - L1167-L1201 换日：getNextTDate / getOffsetMins==0 / minuteToTime / 新交易日 begin
 *   - L1203-L1224 小节内步进与 1130 -> 1305 的小节跳跃
 *   - L1243-L1387 simTicks 分钟臂与 L2013-L2057 onMinuteEnd 分钟闭盘
 *
 * 用 49 根与任务时刻完全对齐的 m1 K 线（930..1130 / 1305..1500，步长 5），
 * 断言闭盘时刻序列，不依赖墙钟或随机数。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <vector>

#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WtBtCore/HisDataReplayer.h>

#include "r80_common.h"

namespace
{
	// SD0930: 09:30-11:30 + 13:00-15:00 = 240 分钟；把「交易分钟序号」换成 HHMM
	uint32_t section_hhmm(uint32_t mins)
	{
		const uint32_t base = (mins <= 120) ? (9 * 60 + 30) : (13 * 60);
		const uint32_t off = (mins <= 120) ? mins : (mins - 120);
		const uint32_t total = base + off;
		return (total / 60) * 100 + total % 60;
	}

	std::vector<WTSBarStruct> task_minute_bars()
	{
		std::vector<WTSBarStruct> bars;
		double px = 10.0;
		for (uint32_t mins = 0; mins <= 240; mins += 5)
		{
			const uint32_t hhmm = section_hhmm(mins);
			bars.push_back(r80::mk_bar(20200102u, hhmm, px, px + 0.2, px - 0.2, px + 0.1, 100.0));
			px += 0.1;
		}
		return bars;
	}
}

class R80TaskBars : public r80::Fixture
{
};

/*
 * TPT_Minute + 已订阅 m1 K 线：
 * 任务从 930 步进到 1500 共 49 次调度；首根 930 被初始定位当作已回放，
 * 其余 48 根按分钟闭盘，每根同步模拟 4 笔开高低收 tick。
 */
TEST_F(R80TaskBars, MinuteTaskReplaysSubscribedBars)
{
	const std::vector<WTSBarStruct> bars = task_minute_bars();
	ASSERT_EQ(49u, bars.size());
	ASSERT_TRUE(r80::write_bars_csv(_base, "SSE.STK.000001_m1", bars, true));

	HisDataReplayer rp;
	r80::CfgOpts opts;
	WTSVariant* cfg = r80::make_cfg(_root, _base, 202001020930ull, 202001030930ull, opts);
	ASSERT_TRUE(rp.init(cfg));
	cfg->release();

	r80::Sink sink;
	sink.rp = &rp;
	rp.register_sink(&sink, "r80_task_min");
	sink.onInit = [&rp, &sink]() {
		sink.slice = rp.get_kline_slice("SSE.STK.000001", "m", 1, 1, true);
	};
	rp.register_task(1, 5, 5, "min", "CHINA", "SD0930");

	ASSERT_TRUE(rp.prepare());
	ASSERT_NE(nullptr, sink.slice);
	ASSERT_EQ(1u, sink.slice->size());
	EXPECT_EQ(930u, (uint32_t)(sink.slice->at(0)->time % 10000u));

	rp.run(false);

	// 49 次调度：930、935 ... 1130、1305 ... 1500
	ASSERT_EQ(49u, sink.scheduleCalls);
	EXPECT_EQ(20200102u, sink.scheduleDates.front());
	EXPECT_EQ(20200102u, sink.scheduleDates.back());
	EXPECT_EQ(930u, sink.scheduleTimes.front());
	EXPECT_EQ(1130u, sink.scheduleTimes[24]);
	EXPECT_EQ(1305u, sink.scheduleTimes[25]);
	EXPECT_EQ(1500u, sink.scheduleTimes.back());

	// 首根 930 被初始游标吃掉，其余 48 根按分钟闭盘
	ASSERT_EQ(48u, sink.bars.size());
	for (const r80::BarRec& rec : sink.bars)
	{
		EXPECT_EQ("SSE.STK.000001", rec.code);
		EXPECT_EQ("m", rec.period);
		EXPECT_EQ(1u, rec.times);
		EXPECT_EQ(20200102u, rec.date);
	}
	EXPECT_EQ(935u, sink.bars.front().time);
	EXPECT_EQ(1500u, sink.bars.back().time);

	// 每根闭盘对应 4 笔模拟 tick
	ASSERT_EQ(48u * 4u, sink.ticks.size());
	for (std::size_t i = 0; i < sink.ticks.size(); i += 4)
	{
		EXPECT_EQ(0u, sink.ticks[i + 0].pxType);
		EXPECT_EQ(1u, sink.ticks[i + 1].pxType);
		EXPECT_EQ(2u, sink.ticks[i + 2].pxType);
		EXPECT_EQ(3u, sink.ticks[i + 3].pxType);
	}
	EXPECT_EQ(2u, sink.beginCalls);
	EXPECT_EQ(2u, sink.endCalls);
	EXPECT_EQ(1u, sink.doneCalls);

	sink.slice->release();
	sink.slice = NULL;
}

/*
 * 期望语义：与 run_by_bars 一致——日频任务在 15:00 触发时，至少应把当前回放游标
 * 指向的日线（此处 1/3）闭盘一次。实际：run_by_tasks 在 bEndSession=false
 * （SD0930 的 1459 < 1500）时把 preTDate 传给 simTicks/onMinuteEnd，任何一根日线
 * 都不会闭盘（0 根），见 bugs/R80.md BUG-R80-001。该行为需要产品口径确认，
 * 故保持 DISABLED_。
 */
TEST_F(R80TaskBars, DISABLED_DailyTaskClosesCurrentDayBars)
{
	const std::vector<WTSBarStruct> bars = {
		r80::mk_bar(20200102u, 0, 10.0, 11.0, 9.5, 10.5, 100.0),
		r80::mk_bar(20200103u, 0, 10.5, 12.0, 10.4, 11.5, 110.0)
	};
	ASSERT_TRUE(r80::write_bars_csv(_base, "SSE.STK.000001_d", bars, false));

	HisDataReplayer rp;
	r80::CfgOpts opts;
	WTSVariant* cfg = r80::make_cfg(_root, _base, 202001020930ull, 202001031500ull, opts);
	ASSERT_TRUE(rp.init(cfg));
	cfg->release();

	r80::Sink sink;
	rp.register_sink(&sink, "r80_task_daily");
	sink.onInit = [&rp, &sink]() {
		sink.slice = rp.get_kline_slice("SSE.STK.000001", "d", 10, 1, true);
	};
	rp.register_task(1, 0, 1500, "d", "CHINA", "SD0930");

	ASSERT_TRUE(rp.prepare());
	ASSERT_NE(nullptr, sink.slice);
	rp.run(false);

	ASSERT_EQ(1u, sink.bars.size());
	EXPECT_EQ(20200103u, sink.bars[0].date);

	sink.slice->release();
	sink.slice = NULL;
}
