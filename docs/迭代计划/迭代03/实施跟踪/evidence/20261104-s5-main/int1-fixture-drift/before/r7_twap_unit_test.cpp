// R7 lane：WtTWapExeUnit（TWAP 分片执行单元）
// 目标文件: src/WtExeFact/WtTWapExeUnit.cpp / .h
// 覆盖面: init 参数与派生量（calTmSecs/_fire_span 走日志断言）、do_calc 的均分/尾桶打板/
//         minopenlots 兜底/价格模式/价格偏移/零价回退/涨跌停收敛、每条早退分支、
//         on_tick 首笔与定时分支、on_order 撤单计数与重发、on_channel_ready 三分支、
//         on_entrust、fire_at_once 的价格模式与涨停收敛。
//
// 分片公式（L705）：curQty = max(minopenlots, round(|diffQty| / leftTimes)) * |diffQty|/diffQty
//   leftTimes = total_times - _fired_times（L675）；leftTimes==0 且 diffQty!=0 时走打板：
//   curQty = max(minopenlots, diffQty)（L700），价格另加 5 跳（L734）。
// 时序：on_tick 非首笔分支用产品侧挂钟比对 _fire_span*1000。本文件把 total_secs/tail_secs
//   配成 (19800-18000)/10 = 180 秒，或 tail==total 使 _fire_span=0，两条都能判定。
// 引用计数：on_tick 保留一份最新 tick；回调方仍需释放自己持有的那份引用。
#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Includes/ExecuteDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "WtExeFact/WtOrdMon.h"
#define private public
#include "WtExeFact/WtTWapExeUnit.h"
#undef private
#include "r7_init_probe.h"

using namespace wtp;

namespace
{
const char* const kTCode = "tw2403";
WTSSessionInfo* mkTSession();

class TFakeCtx : public ExecuteContext
{
public:
	struct Rec { std::string code; double price; double qty; };

	TFakeCtx() : sess(mkTSession()) {}
	~TFakeCtx() override { if (sess) sess->release(); }
	void setSession(WTSSessionInfo* session)
	{
		if (sess) sess->release();
		sess = session;
	}

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return NULL; }
	WTSTickData* grabLastTick(const char*) override { return NULL; }
	double getPosition(const char*, bool = true, int32_t = 3) override { return pos; }
	OrderMap* getOrders(const char*) override { return NULL; }
	double getUndoneQty(const char*) override { return undone; }
	OrderIDs buy(const char* c, double p, double q, bool) override
	{
		buys.push_back({ c, p, q });
		OrderIDs ids{ nextId++ };
		return ids;
	}
	OrderIDs sell(const char* c, double p, double q, bool) override
	{
		sells.push_back({ c, p, q });
		OrderIDs ids{ nextId++ };
		return ids;
	}
	bool cancelByIdRet = true;
	bool cancel(uint32_t id) override { byidCancels.push_back(id); return cancelByIdRet; }
	OrderIDs cancel(const char* c, bool isBuy, double = 0) override
	{
		bydirCancels.push_back({ c, isBuy });
		OrderIDs ids;
		for (uint32_t i = 0; i < cancelBydirRetCnt; i++)
			ids.push_back(nextId++);
		return ids;
	}
	void writeLog(const char* l) override { logs.push_back(l ? l : ""); }
	WTSCommodityInfo* getCommodityInfo(const char*) override { return comm; }
	WTSSessionInfo* getSessionInfo(const char*) override { return sess; }
	uint64_t getCurTime() override { return curTime; }

	double pos = 0;
	double undone = 0;
	uint64_t curTime = 20240101093000000ull;
	uint32_t nextId = 8000;
	uint32_t cancelBydirRetCnt = 2;
	WTSCommodityInfo* comm = NULL;
	WTSSessionInfo* sess = NULL;
	std::vector<Rec> buys;
	std::vector<Rec> sells;
	std::vector<uint32_t> byidCancels;
	std::vector<std::pair<std::string, bool>> bydirCancels;
	std::vector<std::string> logs;

	bool hasLog(const std::string& sub) const { return countLog(sub) > 0; }
	uint32_t countLog(const std::string& sub) const
	{
		uint32_t n = 0;
		for (const std::string& l : logs)
			if (l.find(sub) != std::string::npos)
				n++;
		return n;
	}
};

WTSCommodityInfo* mkTComm()
{
	WTSCommodityInfo* c = WTSCommodityInfo::create("tw", "R7TWAP", "CFFEX", "NIGHT_DAY", "CHN");
	c->setVolScale(1);
	c->setPriceTick(0.01);
	c->setLotsTick(1.0);
	c->setMinLots(1.0);
	c->setCategory(CC_Future);
	c->setCoverMode(CM_OpenCover);
	c->setPriceMode(PM_Both);
	c->setTradingMode(TM_Both);
	return c;
}

WTSTickData* mkTTick(const char* code, uint32_t actTime, double lastPx,
	double bidPx, double askPx, double upper = 0, double lower = 0)
{
	WTSTickStruct ts;
	snprintf(ts.code, sizeof(ts.code), "%s", code);
	ts.action_date = 20240102;
	ts.trading_date = 20240102;
	ts.action_time = actTime;
	ts.price = lastPx;
	ts.pre_close = 10.0;
	ts.bid_prices[0] = bidPx;
	ts.bid_qty[0] = 100;
	ts.ask_prices[0] = askPx;
	ts.ask_qty[0] = 80;
	ts.upper_limit = upper;
	ts.lower_limit = lower;
	return WTSTickData::create(ts);
}

WTSSessionInfo* mkTSession()
{
	WTSSessionInfo* session = WTSSessionInfo::create("R7_TWAP", "R7_TWAP", 0);
	session->addTradingSection(930, 1500);
	return session;
}

void feedTWapTick(WtTWapExeUnit& unit, WTSTickData* tick)
{
	unit.on_tick(tick);
	tick->release();
}

// L147-159 实际读取的键；默认 tail_secs=total_secs(19800) 使 _fire_span=0
WTSVariant* mkTCfg(uint32_t totalTimes = 4, uint32_t priceMode = 0, uint32_t priceOffset = 0,
	double minOpenLots = 1.0, double lots = 10.0, uint32_t ordSticky = 0,
	uint32_t tailSecs = 19800, uint32_t beginTime = 930, uint32_t endTime = 1500,
	bool includeTotalTimes = true, bool includeBeginTime = true, bool includeEndTime = true,
	bool includeMinOpenLots = true)
{
	WTSVariant* cfg = WTSVariant::createObject();
	if (includeBeginTime)
		cfg->append("begin_time", beginTime);
	if (includeEndTime)
		cfg->append("end_time", endTime);
	cfg->append("ord_sticky", ordSticky);
	cfg->append("total_secs", (uint32_t)3600);          // L150 读到的值会被 L162 覆盖
	cfg->append("tail_secs", tailSecs);
	if (includeTotalTimes)
		cfg->append("total_times", totalTimes);
	cfg->append("price_mode", priceMode);
	cfg->append("price_offset", priceOffset);
	cfg->append("lots", lots);
	if (includeMinOpenLots)
		cfg->append("minopenlots", minOpenLots);
	return cfg;
}

void bootT(WtTWapExeUnit& unit, TFakeCtx& ctx, double target, WTSVariant* cfg)
{
	unit.init(&ctx, kTCode, cfg);
	cfg->release();
	unit.set_position(kTCode, target);
	unit.on_channel_ready();
}

void expectTWapConfigRejected(WTSVariant* cfg, const char* diagnostic)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	ctx.undone = 3.0;
	ctx.cancelBydirRetCnt = 1;
	{
		WtTWapExeUnit unit;
		unit.init(&ctx, kTCode, cfg);
		cfg->release();
		EXPECT_FALSE(unit.initializationSucceeded());
		unit.set_position(kTCode, 100);
		unit.on_channel_ready();
		feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
		EXPECT_TRUE(ctx.hasLog(diagnostic));
		EXPECT_TRUE(ctx.buys.empty());
		EXPECT_TRUE(ctx.sells.empty());
		EXPECT_TRUE(ctx.bydirCancels.empty());
	}
	ctx.comm->release();
}

void expectTWapSessionReleased(WTSVariant* cfg, const char* diagnostic)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	ctx.setSession(mkTSession());
	WTSSessionInfo* session = ctx.sess;
	const uint32_t baseline = session->retainCount();
	{
		WtTWapExeUnit unit;
		unit.init(&ctx, kTCode, cfg);
		cfg->release();
		EXPECT_EQ(session->retainCount(), baseline + 1);
		if (diagnostic != NULL)
			EXPECT_TRUE(ctx.hasLog(diagnostic));
	}
	EXPECT_EQ(session->retainCount(), baseline);
	ctx.comm->release();
}


int run_twap_init_probe(uint32_t totalTimes, int markerFd)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	int exitCode = 0;
	{
		WtTWapExeUnit unit;
		WTSVariant* cfg = mkTCfg(totalTimes, 0, 0, 1.0, 10.0, 0, 0);
		if (!r7_test::write_marker(markerFd))
			exitCode = 4;
		else
		{
			unit.init(&ctx, kTCode, cfg);
			const bool initialized = unit.initializationSucceeded();
			const bool rejectedZeroTotalTimes = ctx.hasLog("total_times must be greater than zero");
			const bool completedInit = ctx.hasLog("初始化完成");
			if (totalTimes == 0)
			{
				if (initialized || !rejectedZeroTotalTimes || completedInit)
					exitCode = 10;
			}
			else if (!initialized || rejectedZeroTotalTimes || !completedInit)
				exitCode = 11;
		}
		cfg->release();
	}
	ctx.comm->release();
	return exitCode;
}
}

//////////////////////////////////////////////////////////////////////////
// 名字契约 / init 派生量（calTmSecs 与 _fire_span 从 L169 日志里读出来断言）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, names_and_init_derived_span)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	EXPECT_STREQ(unit.getName(), "WtTWapExeUnit");      // L120
	EXPECT_STREQ(unit.getFactName(), "WtExeFact");      // L110
	// calTmSecs(930,1500) = 54000-34200 = 19800；(19800-18000)/10 = 180
	unit.init(&ctx, kTCode, mkTCfg(10, 0, 0, 1.0, 10.0, 0, 18000));
	EXPECT_TRUE(unit.initializationSucceeded());
	unit.on_channel_ready();
	EXPECT_TRUE(ctx.hasLog("执行时限 19800 秒"));
	EXPECT_TRUE(ctx.hasLog("间隔时间 180 秒"));
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 超时订单没有版本化身份时保留监控记录，不走旧 localid 自动撤单。
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, timeout_without_verified_identity_keeps_monitor_and_never_cancels)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	{
		WtTWapExeUnit unit;
		WTSVariant* cfg = mkTCfg(4, 0, 0, 1.0, 10.0, 1);
		unit.init(&ctx, kTCode, cfg);
		cfg->release();
		unit.set_position(kTCode, 0);

		const uint32_t id = 8351;
		unit._orders_mon.push_order(&id, 1, ctx.curTime);
		feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
		ctx.curTime += 999;
		feedTWapTick(unit, mkTTick(kTCode, 100001000, 10.50, 10.49, 10.51));
		EXPECT_EQ(ctx.countLog("Expired order 8351"), 0u);
		ctx.curTime += 1;
		feedTWapTick(unit, mkTTick(kTCode, 100002000, 10.50, 10.49, 10.51));
		EXPECT_TRUE(ctx.byidCancels.empty());
		EXPECT_TRUE(ctx.bydirCancels.empty());
		EXPECT_TRUE(unit._orders_mon.has_order(id));
		EXPECT_EQ(ctx.countLog("Expired order 8351"), 1u);

		unit.on_channel_ready(); // clear_orders may clear the local entry; it must not clear the unit quarantine
		EXPECT_TRUE(unit._orders_mon.has_unverified_timeout());
		unit.set_position(kTCode, 1.0);
		unit.fire_at_once(1.0);
		feedTWapTick(unit, mkTTick(kTCode, 100003000, 10.50, 10.49, 10.51));
		EXPECT_TRUE(ctx.byidCancels.empty());
		EXPECT_TRUE(ctx.bydirCancels.empty());
		EXPECT_FALSE(unit._orders_mon.has_order(id));
		EXPECT_TRUE(unit._orders_mon.has_unverified_timeout());
		EXPECT_TRUE(ctx.buys.empty());
		EXPECT_TRUE(ctx.sells.empty());
		EXPECT_EQ(ctx.countLog("Expired order 8351"), 1u);
	}
	ctx.comm->release();
}

TEST(r7_twap, init_rejects_missing_trading_session)
{
	{
		TFakeCtx ctx;
		ctx.comm = mkTComm();
		ctx.setSession(NULL);
		const uint32_t commBaseline = ctx.comm->retainCount();
		{
			WtTWapExeUnit unit;
			WTSVariant* cfg = mkTCfg(4);
			unit.init(&ctx, kTCode, cfg);
			cfg->release();
			EXPECT_FALSE(unit.initializationSucceeded());
			EXPECT_TRUE(ctx.hasLog("commodity or trading session is unavailable"));
		}
		EXPECT_EQ(ctx.comm->retainCount(), commBaseline);
		ctx.comm->release();
	}
}

TEST(r7_twap, init_rejects_null_context_code_and_configuration)
{
	{
		WtTWapExeUnit unit;
		WTSVariant* cfg = mkTCfg(4);
		unit.init(NULL, kTCode, cfg);
		cfg->release();
		EXPECT_FALSE(unit.initializationSucceeded());
	}

	TFakeCtx ctx;
	ctx.comm = mkTComm();
	{
		WtTWapExeUnit unit;
		WTSVariant* cfg = mkTCfg(4);
		unit.init(&ctx, NULL, cfg);
		cfg->release();
		EXPECT_FALSE(unit.initializationSucceeded());
		EXPECT_TRUE(ctx.hasLog("standard code is empty"));
	}
	{
		WtTWapExeUnit unit;
		unit.init(&ctx, kTCode, NULL);
		EXPECT_FALSE(unit.initializationSucceeded());
		EXPECT_TRUE(ctx.hasLog("configuration is null"));
	}
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 非法调度配置必须记录原因、公开失败状态并 fail-closed。
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, invalid_schedule_configs_fail_closed_without_orders)
{
	expectTWapConfigRejected(mkTCfg(0), "total_times must be greater than zero");
	expectTWapConfigRejected(
		mkTCfg(4, 0, 0, 1.0, 10.0, 0, 19800, 930, 1500, false),
		"total_times must be greater than zero");
	expectTWapConfigRejected(
		mkTCfg(4, 0, 0, 1.0, 10.0, 0, 19801),
		"tail_secs exceeds execution duration");
	expectTWapConfigRejected(
		mkTCfg(4, 0, 0, 1.0, 10.0, 0, 19800, 960),
		"begin_time/end_time must be valid HHMM values");
	expectTWapConfigRejected(
		mkTCfg(4, 0, 0, 1.0, 10.0, 0, 19800, 930, 2400),
		"begin_time/end_time must be valid HHMM values");
	expectTWapConfigRejected(
		mkTCfg(4, 0, 0, 1.0, 10.0, 0, 19800, 930, 930),
		"execution duration must be greater than zero");
	expectTWapConfigRejected(
		mkTCfg(4, 0, 0, 1.0, 10.0, 0, 19800, 930, 1500, true, false),
		"begin_time/end_time must be valid HHMM values");
	expectTWapConfigRejected(
		mkTCfg(4, 0, 0, 1.0, 10.0, 0, 19800, 930, 1500, true, true, false),
		"begin_time/end_time must be valid HHMM values");
	expectTWapConfigRejected(
		mkTCfg(4, 0, 0, 1.0, 10.0, 0, 19800, 2100, 230),
		"overnight schedules are unsupported pending explicit session semantics");
}

TEST(r7_twap, total_times_out_of_range_is_rejected_before_unsigned_conversion)
{
	for (const char* value : { "-1", "4294967296", "inf", "nan" })
	{
		SCOPED_TRACE(value);
		WTSVariant* cfg = mkTCfg(4, 0, 0, 1.0, 10.0, 0, 18000, 930, 1500, false);
		cfg->append("total_times", value);
		expectTWapConfigRejected(cfg, "total_times must be greater than zero");
	}
	WTSVariant* negative = mkTCfg(4, 0, 0, 1.0, 10.0, 0, 18000, 930, 1500, false);
	negative->append("total_times", static_cast<int32_t>(-1));
	expectTWapConfigRejected(negative, "total_times must be greater than zero");
}

TEST(r7_twap, positive_total_times_one_and_n_derive_expected_spans)
{
	for (const auto& example : { std::pair<uint32_t, uint32_t>{1, 1800}, {10, 180} })
	{
		SCOPED_TRACE(example.first);
		TFakeCtx ctx;
		ctx.comm = mkTComm();
		{
			WtTWapExeUnit unit;
			WTSVariant* cfg = mkTCfg(example.first, 0, 0, 1.0, 10.0, 0, 18000);
			unit.init(&ctx, kTCode, cfg);
			cfg->release();
			EXPECT_TRUE(unit.initializationSucceeded());
			EXPECT_EQ(unit._fire_span, example.second);
		}
		ctx.comm->release();
	}
}

TEST(r7_twap, session_reference_released_after_invalid_config)
{
	expectTWapSessionReleased(mkTCfg(0), "total_times must be greater than zero");
}

TEST(r7_twap, session_reference_released_after_normal_destruction)
{
	expectTWapSessionReleased(mkTCfg(4), NULL);
}

//////////////////////////////////////////////////////////////////////////
// 均分切片：diffQty=100 / leftTimes=4 -> 25 手（L705）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, splits_remaining_qty_evenly_across_left_times)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4));
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 25.0);            // max(1, round(100/4)) = 25
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.50);         // price_mode=0 -> 最新价（L720）
	EXPECT_EQ(ctx.buys[0].code, kTCode);
	EXPECT_TRUE(ctx.hasLog("第 1 次发单"));              // L680
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// TWAP 保留缓存 tick 的所有权：调用方可在回调后释放自己的引用；后续 tick 替换缓存，
// 单元析构释放最后一份引用。
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, tick_ownership_survives_caller_release_later_tick_and_unit_destruction)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WTSTickData* finalTick = NULL;
	{
		WtTWapExeUnit unit;
		bootT(unit, ctx, 100, mkTCfg(2));

		WTSTickData* firstTick = mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51);
		unit.on_tick(firstTick);
		ASSERT_EQ(firstTick->retainCount(), 2u);            // 调用方 + TWAP 缓存
		firstTick->release();                              // 调用方归还自己的引用
		EXPECT_EQ(firstTick->retainCount(), 1u);            // 缓存仍持有对象

		ctx.pos = 50;                                      // 模拟首单部分成交
		finalTick = mkTTick(kTCode, 100100000, 10.60, 10.59, 10.61);
		unit.on_tick(finalTick);                           // 替换并释放 firstTick 的缓存引用
		ASSERT_EQ(finalTick->retainCount(), 2u);            // 调用方 + TWAP 缓存
		EXPECT_EQ(ctx.buys.size(), 2u);
	} // 析构释放 finalTick 的缓存引用，保留调用方引用以观察计数
	ASSERT_NE(finalTick, nullptr);
	EXPECT_EQ(finalTick->retainCount(), 1u);
	finalTick->release();
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 尾差归整：round(|−10|/4)=round(2.5)=3（std::round 远离零）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, rounds_half_away_from_zero_when_remainder_not_even)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 10, mkTCfg(4));
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 3.0);             // max(1, round(2.5)) = 3
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 0 量兜底：diffQty 小于 leftTimes 时均分结果为 0 -> 按 minopenlots 发
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, tiny_diffqty_floors_to_min_open_lots)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 3, mkTCfg(10, 0, 0, 1.0));
	EXPECT_DOUBLE_EQ(unit._min_open_lots, 1.0);
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	// round(3/10)=0 -> max(minopenlots=1, 0) = 1
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 1.0);
	ctx.comm->release();
}

TEST(r7_twap, minopenlots_absent_defaults_to_zero)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	WTSVariant* cfg = mkTCfg(4, 0, 0, 1.0, 10.0, 0, 19800, 930, 1500,
		true, true, true, false);
	ASSERT_FALSE(cfg->has("minopenlots"));
	bootT(unit, ctx, 10, cfg);
	ASSERT_TRUE(unit.initializationSucceeded());
	EXPECT_DOUBLE_EQ(unit._min_open_lots, 0.0);
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 3.0); // round(10/4)=3; default adds no minimum floor
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

TEST(r7_twap, explicit_zero_minopenlots_is_accepted_and_adds_no_floor)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	WTSVariant* cfg = mkTCfg(4, 0, 0, 0.0);
	bootT(unit, ctx, 10, cfg);
	ASSERT_TRUE(unit.initializationSucceeded());
	EXPECT_DOUBLE_EQ(unit._min_open_lots, 0.0);
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 3.0);
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 卖出方向：均分后保持符号（*|diff|/diff = -1），走 sell 分支
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, sell_side_keeps_sign_and_splits_evenly)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	ctx.pos = 100;
	WtTWapExeUnit unit;
	bootT(unit, ctx, 20, mkTCfg(4, 2));                 // diffQty = -80, mode2 -> bid
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	// max(1, round(80/4)) = 20 -> *(-1) = -20 -> sell 20
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 20.0);
	EXPECT_DOUBLE_EQ(ctx.sells[0].price, 10.49);        // L728: isBuy=false -> bidprice(0)
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 尾桶打板：同一目标下首笔常规单部分成交，余单终结后新 tick 触发 leftTimes==0，
// 买入剩余 diffQty 并在最新价上加 5 跳（L675、L694-700、L732-735）。
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, last_slice_showhand_buys_remaining_with_5_tick_premium)
{
	TFakeCtx ctx;
	auto releaseComm = [](WTSCommodityInfo* comm) { if (comm) comm->release(); };
	std::unique_ptr<WTSCommodityInfo, decltype(releaseComm)> comm(mkTComm(), releaseComm);
	ctx.comm = comm.get();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(1));
	auto sendTick = [&unit](WTSTickData* rawTick) {
		auto releaseTick = [](WTSTickData* tick) { if (tick) tick->release(); };
		std::unique_ptr<WTSTickData, decltype(releaseTick)> tick(rawTick, releaseTick);
		unit.on_tick(tick.get());
		EXPECT_EQ(tick->retainCount(), 2u);                  // 调用方 + TWAP 缓存
	};
	sendTick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	// 首次发单：_fired_times=0、total_times=1，leftTimes=1，常规切片为 100 手。
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.50);
	EXPECT_TRUE(ctx.hasLog("第 1 次发单"));

	// 模拟该单成交 20 手、余下挂单已终结：仓位更新为 20，当前无未完成委托。
	// 通道状态同步会清理本地的旧订单记录，但不会重置本轮 _fired_times。
	ctx.pos = 20;
	ctx.undone = 0;
	unit.on_channel_ready();
	ASSERT_EQ(ctx.buys.size(), 1u);

	// 新 tick：_fired_times 仍为 1，leftTimes=1-1=0，剩余 diffQty=80 走 showhand。
	sendTick(mkTTick(kTCode, 100100000, 10.60, 10.59, 10.61));
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 80.0);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.65);         // 最新价 10.60 + 5 * priceTick 0.01
	EXPECT_TRUE(ctx.hasLog("第 2 次发单"));
}

//////////////////////////////////////////////////////////////////////////
// _fire_span > 0 时，非首笔 tick 不重算（L415 的间隔判定）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, fire_span_blocks_second_tick_recalc)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	// (19800-18000)/10 = 180 秒 -> 两条连续 tick 之间远小于 180000ms
	bootT(unit, ctx, 100, mkTCfg(10, 0, 0, 1.0, 10.0, 0, 18000));
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	// 首笔 tick 触发 do_calc：leftTimes=10 -> round(100/10)=10
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 10.0);
	feedTWapTick(unit, mkTTick(kTCode, 100100000, 10.60, 10.59, 10.61));
	EXPECT_EQ(ctx.buys.size(), 1u);                     // 间隔未到 -> 不再发单
	feedTWapTick(unit, mkTTick(kTCode, 100200000, 10.70, 10.69, 10.71));
	EXPECT_EQ(ctx.buys.size(), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// price_mode=1：买入取买一；price_mode=2：买入取卖一
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, price_mode_1_buy_uses_bid_and_mode_2_uses_ask)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit u1, u2;
	bootT(u1, ctx, 40, mkTCfg(2, 1));
	feedTWapTick(u1, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	bootT(u2, ctx, 40, mkTCfg(2, 2));
	feedTWapTick(u2, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	// 两个单元共享 ctx，各自 1 笔：buys[0] 属于 mode1，buys[1] 属于 mode2
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.49);         // L724: isBuy -> bidprice(0)
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.51);         // L728: isBuy -> askprice(0)
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// price_offset：非打板时按 offset 跳加价（L736-738）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, price_offset_shifts_buy_up_and_sell_down)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit u1;
	bootT(u1, ctx, 100, mkTCfg(4, 0, 3));               // offset=3
	feedTWapTick(u1, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.53);         // 10.50 + 0.01*3
	ctx.pos = 100;
	WtTWapExeUnit u2;
	bootT(u2, ctx, 20, mkTCfg(4, 0, 3));                // diffQty=-80 -> isBuy=false
	feedTWapTick(u2, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].price, 10.47);        // 10.50 - 0.01*3
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 零价回退：最新价为 0 时改用 preclose（L741-742）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, zero_last_price_falls_back_to_preclose)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4, 0, 0, 1.0, 10.0, 0, 19800, 930, 1500));
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 0.0, 0.0, 0.0));
	// price_mode=0 -> targetPx = price()=0 -> L741 命中 -> price()==0 -> preclose()=10.0
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 涨停收敛（L746-751）与跌停收敛（L752-757）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, limit_prices_cap_the_order_price)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit u1;
	bootT(u1, ctx, 100, mkTCfg(4, 0, 300));
	feedTWapTick(u1, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51, 10.60, 0));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.60);         // 10.50+3.00 -> 涨停 10.60
	EXPECT_TRUE(ctx.hasLog("modified to upper limit price"));
	ctx.pos = 100;
	WtTWapExeUnit u2;
	bootT(u2, ctx, 20, mkTCfg(4, 0, 300));
	feedTWapTick(u2, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51, 0, 10.20));
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].price, 10.20);        // 10.50-3.00 -> 跌停 10.20
	EXPECT_TRUE(ctx.hasLog("modified to lower limit price"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 通道未就绪：一切计算作废（L552-553），就绪后同一笔 tick 立刻补发
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, calc_before_channel_ready_does_nothing)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	unit.init(&ctx, kTCode, mkTCfg(4));
	unit.set_position(kTCode, 100);
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.buys.empty());                     // !_channel_ready -> L552 return
	unit.on_channel_ready();                           // undone=0、OMS 空 -> L303 else 分支
	// 就绪后 on_channel_ready 末尾的 do_calc 已经有 tick 了 -> 正常发单
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 25.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 无 tick / 同向挂单 两条早退（L627-631、L616-621）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, calc_exits_without_tick_then_blocks_on_live_order)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	unit.init(&ctx, kTCode, mkTCfg(4));
	unit.set_position(kTCode, 100);
	unit.on_channel_ready();
	EXPECT_TRUE(ctx.hasLog("没有最新tick数据"));           // L630
	EXPECT_TRUE(ctx.buys.empty());
	ctx.undone = 3.0;
	unit.set_position(kTCode, 200);
	EXPECT_TRUE(ctx.hasLog("上一轮有挂单未完成"));          // L619
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 撤单在途：_cancel_cnt != 0 -> 本轮退出（L578-583）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, pending_cancel_blocks_calc)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	ctx.undone = 5.0;
	ctx.cancelBydirRetCnt = 2;
	WtTWapExeUnit unit;
	unit.init(&ctx, kTCode, mkTCfg(4));
	unit.set_position(kTCode, 100);
	unit.on_channel_ready();
	// L262: undone!=0 且 OMS 空 -> 撤外部单，isBuy=(undone>0)=true，_cancel_cnt=2
	ASSERT_EQ(ctx.bydirCancels.size(), 1u);
	EXPECT_TRUE(ctx.bydirCancels[0].second);
	EXPECT_TRUE(ctx.hasLog("Unmanaged order updated cancelcnt to 2"));   // L282
	EXPECT_TRUE(ctx.hasLog("尚有未完成撤单指令"));                          // L581（随后的 do_calc）
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 反向未完成单先撤（L591-614）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, opposite_live_order_canceled_first_without_new_order)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	ctx.cancelBydirRetCnt = 1;
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4));                   // boot 时 undone=0 -> else 分支
	ctx.undone = -3.0;
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	// diffQty=100, undone=-3 -> L594 isBuy=(undone>0)=false
	ASSERT_EQ(ctx.bydirCancels.size(), 1u);
	EXPECT_FALSE(ctx.bydirCancels[0].second);
	EXPECT_TRUE(ctx.hasLog("live opposite order of"));   // L609
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_channel_ready：OMS 有单但柜台未完成单为 0 -> 清本地错单（L286-298）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, channel_ready_drops_unconfirmed_local_orders)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4));
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);                      // OMS 里有 8000
	unit.on_channel_ready();
	EXPECT_TRUE(ctx.hasLog("Local orders of"));
	EXPECT_TRUE(ctx.hasLog("not confirmed in trading channel, clear all"));   // L296
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 同一笔 tick 不重复下单（L657-665），拒单重算走同一条闸门
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, stale_tick_gate_and_entrust_reject)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4));
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.on_entrust(8000, kTCode, false, "rejected");    // erase -> do_calc -> 同一 tick 被拦
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.hasLog("No tick of"));
	unit.on_entrust(8000, kTCode, true, "ok");           // 成功回报不进这条链
	EXPECT_EQ(ctx.buys.size(), 1u);
	unit.on_entrust(60606, kTCode, false, "not mine");   // L451 非本单元单
	EXPECT_EQ(ctx.buys.size(), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_order：未知 id 忽略；撤单回报递减 _cancel_cnt（L186-201）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, on_order_drains_cancel_cnt)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	ctx.undone = 5.0;
	ctx.cancelBydirRetCnt = 2;
	WtTWapExeUnit unit;
	unit.init(&ctx, kTCode, mkTCfg(4));
	unit.on_channel_ready();                             // 撤外部单 -> 8001/8002，_cancel_cnt=2
	unit.on_order(4242, kTCode, true, 0, 10.50, true);   // 未知 id -> L186 return
	EXPECT_FALSE(ctx.hasLog("Order 4242 updated cancelcnt"));
	unit.on_order(8001, kTCode, true, 0, 10.50, true);
	EXPECT_TRUE(ctx.hasLog("Order 8001 updated cancelcnt -> 1"));   // L200
	unit.on_order(8001, kTCode, true, 0, 10.50, true);              // 已摘除 -> 再次忽略
	EXPECT_EQ(ctx.countLog("Order 8001 updated cancelcnt"), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 撤单回报 + 本轮目标未达成 -> fire_at_once 重发并累计加价（L213-232、L468-536）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, cancel_report_refires_with_cancel_times_price_step)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4, 0));
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);                      // _this_target = 0 + 25 = 25
	unit.on_order(8000, kTCode, true, 5, 10.50, true);   // leftover!=0 且已撤 -> L213 命中
	// _cancel_times=1 -> fire_at_once(max(1, 25-0)=25)
	// price_mode=0 -> 10.50；L508 再 + 0.01*1 = 10.51
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 25.0);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.51);
	EXPECT_TRUE(ctx.hasLog("re_fire will be done"));     // L223
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// fire_at_once 的价格模式：mode1 取买一、mode2 取卖一，均再按 _cancel_times 加价
// （与 do_calc 的 mode1/2 用同一套基准，见 L495-504 vs L722-729）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, fire_at_once_mode_1_and_2_pricing)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit u1, u2;
	bootT(u1, ctx, 100, mkTCfg(4, 1));
	feedTWapTick(u1, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	bootT(u2, ctx, 100, mkTCfg(4, 2));
	feedTWapTick(u2, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.49);         // u1 do_calc mode1 买入 -> bid
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.51);         // u2 do_calc mode2 买入 -> ask
	u1.on_order(8000, kTCode, true, 5, 10.49, true);    // u1 的首单 -> fire_at_once(mode1, bid)
	u2.on_order(8001, kTCode, true, 5, 10.51, true);    // u2 的首单 -> fire_at_once(mode2, ask)
	ASSERT_EQ(ctx.buys.size(), 4u);
	// L508: + priceTick*_cancel_times(1) = +0.01
	EXPECT_DOUBLE_EQ(ctx.buys[2].price, 10.50);         // 10.49 + 0.01
	EXPECT_DOUBLE_EQ(ctx.buys[3].price, 10.52);         // 10.51 + 0.01
	EXPECT_DOUBLE_EQ(ctx.buys[2].qty, 25.0);            // _this_target = 0 + round(100/4) = 25
	EXPECT_DOUBLE_EQ(ctx.buys[3].qty, 25.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 非法 price_mode 在 fire_at_once 里没有零价兜底（do_calc 有，L741-742 缺在此处）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, fire_at_once_unknown_price_mode_has_no_preclose_fallback)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4, 3));                // price_mode=3
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	// do_calc 的 else 分支（L726-729）仍取对手价 10.51
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.51);
	unit.on_order(8000, kTCode, true, 5, 10.51, true);  // -> fire_at_once
	// fire_at_once 的 if/elif 只覆盖 0/1/2（L490-504）-> targetPx 停在 0
	// 再按 L508 加 0.01*_cancel_times(1) -> 报出 0.01 的价
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 0.01);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 25.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 非法 price_mode 在 do_calc 落到 else 分支（与 mode 2 同价）（L726-729）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, unknown_price_mode_falls_into_counter_price)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4, 9));                 // price_mode=9 -> else
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.51);          // 买入 -> askprice(0)
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_tick 入参防御 + on_trade/on_channel_lost 空实现（L332、L432-435、L316-320）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, tick_guards_and_noop_callbacks)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	unit.init(&ctx, kTCode, mkTCfg(4));
	unit.set_position(kTCode, 100);
	unit.on_channel_ready();
	unit.on_tick(NULL);
	unit.on_trade(8000, kTCode, true, 10, 10.50);
	unit.on_channel_lost();
	WTSTickData* foreign = mkTTick("xx2403", 100000000, 10.50, 10.49, 10.51);
	unit.on_tick(foreign);
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_FALSE(ctx.hasLog("次发单"));
	foreign->release();
	unit.set_position("other", 100);                     // L782 代码不符 -> 丢弃
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 25.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 重复目标仓位被丢弃（L787-788），因此不会重算
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, repeated_target_is_ignored)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4));
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.set_position(kTCode, 100);                      // 与 _target_pos 相等 -> 直接 return
	EXPECT_EQ(ctx.countLog("第 1 次发单"), 1u);
	feedTWapTick(unit, mkTTick(kTCode, 100100000, 10.60, 10.59, 10.61));
	// _fire_span=0 -> 会重算；此时 diffQty 仍是 100（realPos 未变），_fired_times=1,
	// total_times=4 -> leftTimes=3 -> round(100/3)=33
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 33.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 清仓（DBL_MAX）与 0 实仓：diffQty==0 早退（L586-587）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, clear_target_with_flat_position_does_nothing)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	unit.init(&ctx, kTCode, mkTCfg(4));
	unit.on_channel_ready();
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	unit.set_position(kTCode, DBL_MAX);                  // get_real_target -> 0 == realPos
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// Bounded child verifies the explicit zero-total-times rejection and a valid positive control.
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, init_zero_total_times_bounded_child_probe)
{
	r7_test::ScopedTempDir probeDir("WT_R7_002_TWAP");
	ASSERT_TRUE(probeDir.ready());
	const r7_test::ProbeResult result = r7_test::run_bounded_child(probeDir.path(), 0, run_twap_init_probe);
	ASSERT_TRUE(probeDir.cleanup()) << "failed to remove private init-probe directory";
	EXPECT_TRUE(r7_test::expect_normal_return(result));
}

TEST(r7_twap, init_positive_total_times_control_returns)
{
	r7_test::ScopedTempDir probeDir("WT_R7_002_TWAP");
	ASSERT_TRUE(probeDir.ready());
	const r7_test::ProbeResult result = r7_test::run_bounded_child(probeDir.path(), 5, run_twap_init_probe);
	EXPECT_TRUE(r7_test::expect_normal_return(result));
	EXPECT_TRUE(probeDir.cleanup()) << "failed to remove private init-probe directory";
}

//////////////////////////////////////////////////////////////////////////
// 缺 commodity 必须在价格偏移读取 price tick 前失败关闭。
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, null_commodity_with_price_offset_fails_closed)
{
	TFakeCtx ctx;
	ctx.comm = NULL;
	WtTWapExeUnit unit;
	WTSVariant* cfg = mkTCfg(4, 0, 3);
	unit.init(&ctx, kTCode, cfg);
	cfg->release();
	EXPECT_FALSE(unit.initializationSucceeded());
	EXPECT_TRUE(ctx.hasLog("commodity or trading session is unavailable"));
	unit.set_position(kTCode, 100);
	unit.on_channel_ready();
	feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_TRUE(ctx.byidCancels.empty());
	EXPECT_TRUE(ctx.bydirCancels.empty());
}

TEST(r7_twap, failed_concurrent_calc_guard_does_not_release_owner)
{
	WtTWapExeUnit unit;
	EXPECT_FALSE(unit._in_calc.load(std::memory_order_acquire));
	{
		WtTWapExeUnit::CalcFlag owner(&unit._in_calc);
		ASSERT_FALSE(static_cast<bool>(owner));

		bool contenderSawBusy = false;
		std::thread contender([&]() {
			WtTWapExeUnit::CalcFlag nested(&unit._in_calc);
			contenderSawBusy = static_cast<bool>(nested);
		});
		contender.join();

		EXPECT_TRUE(contenderSawBusy);
		EXPECT_TRUE(unit._in_calc.load(std::memory_order_acquire));
	}
	EXPECT_FALSE(unit._in_calc.load(std::memory_order_acquire));
}

TEST(r7_twap, startup_cancel_before_first_slice_does_not_retry_default_target)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	ctx.pos = 20.0;
	ctx.undone = 5.0;
	ctx.cancelBydirRetCnt = 1;
	{
		WtTWapExeUnit unit;
		WTSVariant* cfg = mkTCfg(4);
		unit.init(&ctx, kTCode, cfg);
		cfg->release();
		ASSERT_TRUE(unit.initializationSucceeded());
		unit.on_channel_ready();
		ASSERT_EQ(ctx.bydirCancels.size(), 1u);
		ASSERT_EQ(unit._cancel_cnt, 1u);
		EXPECT_FALSE(unit._this_target_valid);

		unit.on_order(8000, kTCode, true, 5.0, 10.50, true);
		EXPECT_EQ(unit._cancel_cnt, 0u);
		EXPECT_FALSE(unit._this_target_valid);
		EXPECT_TRUE(ctx.buys.empty());
		EXPECT_TRUE(ctx.sells.empty());
	}
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 迭代03 WP1（卡5/N05-003，2026-11-04）：宿主通过保留配置键声明已按 owner 与不可复用
// 身份核验撤单时，超时撤单经 ctx->cancel(localid) 发出且同一单只发一次；宿主拒绝核验
// （cancel 返回 false）即转为持久隔离，不回退裸 ID、不重复撤单。未声明时保持原 fail-closed。
//////////////////////////////////////////////////////////////////////////
#include "Includes/IExecPluginV2.h"
#ifndef WT_EXEC_PLUGIN_V2_HOST_ORDER_IDENTITY_KEY
#define WT_EXEC_PLUGIN_V2_HOST_ORDER_IDENTITY_KEY "__wt_host_order_identity"
#endif

TEST(r7_twap, wp1_timeout_with_host_verified_identity_cancels_once_or_latches)
{
	for (int refuse = 0; refuse < 2; ++refuse)
	{
		TFakeCtx ctx;
		ctx.comm = mkTComm();
		ctx.cancelByIdRet = refuse == 0;
		{
			WtTWapExeUnit unit;
			WTSVariant* cfg = mkTCfg(4, 0, 0, 1.0, 10.0, 1);
			cfg->append(WT_EXEC_PLUGIN_V2_HOST_ORDER_IDENTITY_KEY, (uint32_t)1);
			unit.init(&ctx, kTCode, cfg);
			cfg->release();
			unit.set_position(kTCode, 0);
			const uint32_t id = 8361;
			unit._orders_mon.push_order(&id, 1, ctx.curTime);
			feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
			ctx.curTime += 1000;
			feedTWapTick(unit, mkTTick(kTCode, 100002000, 10.50, 10.49, 10.51));
			ASSERT_EQ(1u, ctx.byidCancels.size()) << "refuse=" << refuse;
			EXPECT_EQ(id, ctx.byidCancels[0]);
			EXPECT_TRUE(unit._orders_mon.has_order(id));
			EXPECT_EQ(refuse == 1, unit._orders_mon.has_unverified_timeout());
			EXPECT_EQ(refuse == 0, ctx.countLog("canceled with host-verified identity") == 1u);
			EXPECT_EQ(refuse == 1, ctx.countLog("host could not verify its identity") == 1u);
			ctx.curTime += 2000;
			feedTWapTick(unit, mkTTick(kTCode, 100004000, 10.50, 10.49, 10.51));
			EXPECT_EQ(1u, ctx.byidCancels.size()) << "claimed or latched order is never re-cancelled";
		}
		ctx.comm->release();
	}
}
