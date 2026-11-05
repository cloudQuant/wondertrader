// R7 lane：WtVWapExeUnit（期货 VWAP 分片执行单元）
// 目标文件: src/WtExeFact/WtVWapExeUnit.cpp / .h
// 覆盖面: init 的 VWAP 分布文件读取（缺文件早退 / 解析循环）、do_calc 的每一条早退与主发单路径、
//         按 session 的交易分钟时间分桶、on_tick 首笔与后续、on_order 撤单计数与重发、
//         on_channel_ready 三个分支、on_entrust 拒单重算、fire_at_once 价格模式与涨跌停收敛。
//
// 引用计数约定（实测 Includes/WTSObject.hpp:23 m_uRefs 起始为 1）：
//   create 的引用由调用方持有；on_tick retain 一份作为缓存；调用方随即 release 自己的引用。
//   下一笔 on_tick 释放旧缓存并 retain 新 tick；单元析构释放最后一份缓存引用。
//   do_calc 只借用缓存 tick；fire_at_once 的 retain/release 则是配对的临时引用。
//
// 时序：on_tick 的非首笔分支使用产品侧 TimeUtils 检查 `_fire_span` 节流；测试不睡眠，
// 通过初始化日志核对配置推导间隔，并在同一测试内连续送入行情验证短时间内不重复发单。
//
// 驱动顺序固定为 init -> set_position -> on_channel_ready -> on_tick：
// set_position 在 L498 用 _code 比对，init 之前 _code 为空串，目标会被静默丢弃。
#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "Includes/ExecuteDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "WtExeFact/WtOrdMon.h"
#define private public
#include "WtExeFact/WtVWapExeUnit.h"
#undef private
#include "r7_init_probe.h"

using namespace wtp;

namespace
{
const char* const kCode = "tv2403";

class FakeCtx : public ExecuteContext
{
public:
	struct Rec { std::string code; double price; double qty; bool force = false; };
	FakeCtx()
	{
		sess = WTSSessionInfo::create("R7_DAY", "R7_DAY", 0);
		sess->addTradingSection(930, 1130);
		sess->addTradingSection(1300, 1500);
	}
	~FakeCtx() override
	{
		if (sess)
			sess->release();
	}
	void setSession(WTSSessionInfo* value)
	{
		if (sess)
			sess->release();
		sess = value;
	}

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return NULL; }
	WTSTickData* grabLastTick(const char*) override { return NULL; }
	double getPosition(const char*, bool validOnly = true, int32_t flag = 3) override
	{
		const double longTotal = pos > 0 ? pos : 0;
		const double shortTotal = pos < 0 ? pos : 0;
		const double longAvailable = availableLong >= 0 ? availableLong : longTotal;
		const double shortAvailable = availableShort >= 0 ? -availableShort : shortTotal;
		if (flag == 1)
			return validOnly ? longAvailable : longTotal;
		if (flag == 2)
			return validOnly ? shortAvailable : shortTotal;
		return validOnly ? longAvailable + shortAvailable : pos;
	}
	OrderMap* getOrders(const char*) override { return NULL; }
	double getUndoneQty(const char*) override { return undone; }

	OrderIDs buy(const char* stdCode, double price, double qty, bool forceClose) override
	{
		buys.push_back({ stdCode, price, qty, forceClose });
		OrderIDs ids{ nextId++ };
		return ids;
	}
	OrderIDs sell(const char* stdCode, double price, double qty, bool forceClose) override
	{
		sells.push_back({ stdCode, price, qty, forceClose });
		OrderIDs ids{ nextId++ };
		return ids;
	}
	bool cancelByIdRet = true;
	bool cancel(uint32_t localid) override { byidCancels.push_back(localid); return cancelByIdRet; }
	OrderIDs cancel(const char* stdCode, bool isBuy, double = 0) override
	{
		bydirCancels.push_back({ stdCode, isBuy });
		OrderIDs ids;
		for (uint32_t i = 0; i < cancelBydirRetCnt; i++)
			ids.push_back(nextId++);
		return ids;
	}
	void writeLog(const char* log) override { logs.push_back(log ? log : ""); }
	WTSCommodityInfo* getCommodityInfo(const char*) override { return comm; }
	WTSSessionInfo* getSessionInfo(const char*) override { return sess; }
	uint64_t getCurTime() override { return curTime; }

	double pos = 0;
	uint64_t curTime = 20240101093000000ull;
	double availableLong = -1;
	double availableShort = -1;
	double undone = 0;
	uint32_t nextId = 9000;
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

// 分桶名里带 pid，避免同一 CWD 下并行的其它 lane 撞文件
inline std::string commName()
{
	return "R7VWAP" + std::to_string((long long)getpid());
}

// WtVWapExeUnit.cpp L125-127：文件名 = "Vwap_" + comm->getName() + ".txt"（相对进程 CWD）。
// V2 profile 的下标由 WTSSessionInfo 累计交易分钟映射；legacy 日盘文件恰好 241 格。
class VwapFile
{
public:
	struct V2Tag {};

	explicit VwapFile(double bucketVal, uint32_t rows = 241, bool malformedLast = false)
	{
		_path = std::string("Vwap_") + commName() + ".txt";
		std::ofstream f(_path.c_str());
		for (uint32_t i = 0; i < rows; i++)
		{
			if (i)
				f << ",";
			if (malformedLast && i + 1 == rows)
				f << "invalid-value";
			else
				f << bucketVal;
		}
		f.close();
	}
	VwapFile(V2Tag, const std::string& sessionId, int32_t offset,
		const std::vector<std::pair<uint32_t, uint32_t>>& sections, size_t valuesCount)
	{
		_path = std::string("Vwap_") + commName() + ".txt";
		std::ofstream f(_path.c_str());
		f << "{\"version\":2,\"profiles\":[{\"session_id\":\"" << sessionId
			<< "\",\"offset_minutes\":" << offset << ",\"sections\":[";
		for (size_t i = 0; i < sections.size(); ++i)
		{
			if (i)
				f << ",";
			f << "{\"from\":" << sections[i].first << ",\"to\":" << sections[i].second << "}";
		}
		f << "],\"bucket_unit\":\"trading_minute\",\"value_mode\":\"cumulative\",\"close_inclusive\":true,\"values\":[";
		for (size_t i = 0; i < valuesCount; ++i)
		{
			if (i)
				f << ",";
			f << (i + 1);
		}
		f << "]}]}";
		f.close();
	}
	~VwapFile() { std::remove(_path.c_str()); }

private:
	std::string _path;
};

WTSCommodityInfo* mkComm()
{
	WTSCommodityInfo* c = WTSCommodityInfo::create("tv", commName().c_str(), "CFFEX", "NIGHT_DAY", "CHN");
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

WTSTickData* mkTick(const char* code, uint32_t actTime, double lastPx,
	double bidPx, double askPx, double upper = 0, double lower = 0,
	uint32_t actionDate = 20240101, uint32_t tradingDate = 0)
{
	WTSTickStruct ts;
	snprintf(ts.code, sizeof(ts.code), "%s", code);
	ts.action_date = actionDate;
	ts.trading_date = tradingDate == 0 ? actionDate : tradingDate;
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

void sendTick(WtVWapExeUnit& unit, WTSTickData* tick)
{
	unit.on_tick(tick);
	if (tick)
		tick->release();
}

// 见 L110-119 实际读取的键
WTSVariant* mkCfg(uint32_t totalTimes = 5, uint32_t priceMode = 0, uint32_t priceOffset = 0,
	double minOpenLots = 1.0, double lots = 10.0, uint32_t ordSticky = 0,
	uint32_t tailSecs = 0, uint32_t beginTime = 930, uint32_t endTime = 1500,
	bool includeMinOpenLots = true)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("begin_time", beginTime);
	cfg->append("end_time", endTime);
	cfg->append("ord_sticky", ordSticky);
	cfg->append("tail_secs", tailSecs);
	cfg->append("total_times", totalTimes);
	cfg->append("price_mode", priceMode);
	cfg->append("price_offset", priceOffset);
	cfg->append("lots", lots);
	if (includeMinOpenLots)
		cfg->append("minopenlots", minOpenLots);
	return cfg;
}

// init -> 落目标 -> 通道就绪（此时无 tick，do_calc 在 L341 早退）
void boot(WtVWapExeUnit& unit, FakeCtx& ctx, double target, WTSVariant* cfg)
{
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	unit.set_position(kCode, target);
	unit.on_channel_ready();
}


int run_vwap_init_probe(uint32_t totalTimes, int markerFd)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	int exitCode = 0;
	{
		WtVWapExeUnit unit;
		WTSVariant* cfg = mkCfg(totalTimes, 0, 0, 1.0, 10.0, 0, 0);
		if (!r7_test::write_marker(markerFd))
			exitCode = 4;
		else
			unit.init(&ctx, kCode, cfg);
		cfg->release();
	}
	ctx.comm->release();
	return exitCode;
}
}

TEST(r7_vwap, failed_concurrent_calc_guard_does_not_release_owner)
{
	WtVWapExeUnit unit;
	EXPECT_FALSE(unit._in_calc.load(std::memory_order_acquire));
	{
		WtVWapExeUnit::CalcFlag owner(&unit._in_calc);
		ASSERT_FALSE(static_cast<bool>(owner));

		bool contenderSawBusy = false;
		std::thread contender([&]() {
			WtVWapExeUnit::CalcFlag nested(&unit._in_calc);
			contenderSawBusy = static_cast<bool>(nested);
		});
		contender.join();

		EXPECT_TRUE(contenderSawBusy);
		EXPECT_TRUE(unit._in_calc.load(std::memory_order_acquire));
	}
	EXPECT_FALSE(unit._in_calc.load(std::memory_order_acquire));
}

TEST(r7_vwap, startup_cancel_without_explicit_target_does_not_retry_default_zero)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	ctx.pos = 100.0;
	ctx.undone = 5.0;
	ctx.cancelBydirRetCnt = 1;
	{
		WtVWapExeUnit unit;
		WTSVariant* cfg = mkCfg();
		unit.init(&ctx, kCode, cfg);
		cfg->release();
		ASSERT_TRUE(unit.initializationSucceeded());
		unit.on_channel_ready();
		sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
		ASSERT_EQ(ctx.bydirCancels.size(), 1u);
		ASSERT_EQ(unit._cancel_cnt, 1u);
		EXPECT_FALSE(unit._target_pos_valid);

		ctx.undone = 0.0;
		unit.on_order(9000, kCode, false, 5.0, 10.50, true);
		sendTick(unit, mkTick(kCode, 100100000, 10.50, 10.49, 10.51));
		EXPECT_EQ(unit._cancel_cnt, 0u);
		EXPECT_FALSE(unit._target_pos_valid);
		EXPECT_TRUE(ctx.buys.empty());
		EXPECT_TRUE(ctx.sells.empty());
	}
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 名字契约 / 非本合约目标被忽略（L498-499）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, names_and_foreign_code_target_ignored)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	EXPECT_STREQ(unit.getName(), "WtVWapExeUnit");         // L96
	EXPECT_STREQ(unit.getFactName(), "WtExeFact");         // L91
	unit.init(&ctx, kCode, mkCfg());
	unit.set_position("other2403", 100);                   // 代码不同 -> 直接 return
	unit.on_channel_ready();
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// init：分布文件缺失 -> L128-131 早退（后续主发单路径不可用，见 BUG-R7-003）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, init_missing_distribution_file_returns_early)
{
	std::string name = "R7NOFILE" + std::to_string((long long)getpid());
	std::string missing = std::string("Vwap_") + name + ".txt";
	std::remove(missing.c_str());
	FakeCtx ctx;
	ctx.comm = WTSCommodityInfo::create("tv", name.c_str(), "CFFEX", "NIGHT_DAY", "CHN");
	ctx.comm->setPriceTick(0.01);
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg();
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	unit.on_channel_ready();
	EXPECT_TRUE(ctx.hasLog("cannot be opened"));
	EXPECT_EQ(ctx.countLog("cannot be opened"), 1u);
	EXPECT_FALSE(ctx.hasLog("初始化完成"));
	EXPECT_FALSE(unit.initializationSucceeded());
	unit.set_position(kCode, 100);
	WTSTickData* tick = mkTick(kCode, 100000000, 10.50, 10.49, 10.51);
	unit.on_tick(tick);
	tick->release();
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 主发单路径：分桶量 = max(VwapAim[30] - pos, minopenlots)（L371-388）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, init_loads_distribution_and_first_bucket_buys)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	// 通道未就绪期间（boot 里 set_position 先于 on_channel_ready）不该有单
	EXPECT_TRUE(ctx.buys.empty());
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	// 10:00 maps to cumulative trading-minute bucket 30; its profile target is 25.
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 25.0);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.50);      // price_mode=0 -> curTick->price()（L397）
	EXPECT_EQ(ctx.buys[0].code, kCode);
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_TRUE(ctx.hasLog("第 1 次发单"));           // L375
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// minopenlots 兜底：分桶预测量为 0 时取最小开仓量（L385 的 max 取右值）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, bucket_zero_falls_back_to_min_open_lots)
{
	VwapFile vf(0.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg(5, 0, 0, 7.0));
	EXPECT_DOUBLE_EQ(unit._min_open_lots, 7.0);
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	// _Vwap_vol = 0-0 = 0 -> max(0, minopenlots=7) = 7 -> *|100|/100 = 7
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 7.0);
	ctx.comm->release();
}

TEST(r7_vwap, minopenlots_absent_defaults_to_zero)
{
	VwapFile vf(0.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg(5, 0, 0, 0.0, 10.0, 0, 0, 930, 1500, false);
	ASSERT_FALSE(cfg->has("minopenlots"));
	boot(unit, ctx, 100, cfg);
	ASSERT_TRUE(unit.initializationSucceeded());
	EXPECT_DOUBLE_EQ(unit._min_open_lots, 0.0);
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_TRUE(ctx.hasLog("no executable quantity after position availability checks"));
	ctx.comm->release();
}

TEST(r7_vwap, explicit_zero_minopenlots_is_accepted_without_floor)
{
	VwapFile vf(0.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg(5, 0, 0, 0.0));
	ASSERT_TRUE(unit.initializationSucceeded());
	EXPECT_DOUBLE_EQ(unit._min_open_lots, 0.0);
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_TRUE(ctx.hasLog("no executable quantity after position availability checks"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// price_mode=1（最优价）：do_calc 买入取 bidprice(0)（L399-402）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, calc_price_mode_1_buy_uses_bid_price)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 60, mkCfg(5, 1));
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.49);       // bidprice(0)
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 25.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// price_mode=2（对手价）+ 目标向下：卖出取 bidprice(0)，量按符号回退到 1 手
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, calc_price_mode_2_sell_uses_bid_and_sign_trick)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	ctx.pos = 100;                                    // 实仓 100
	WtVWapExeUnit unit;
	boot(unit, ctx, 50, mkCfg(5, 2));                 // 目标 50 -> diffQty = -50
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	// _Vwap_vol = 25-100 = -75 -> max(-75, minopenlots=1) = 1 -> curQty = 1*|-50|/(-50) = -1
	// curQty<=0 -> sell(abs(-1))（L438-439）；price_mode 非 0/1 -> else -> bidprice(0)（L405）
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 1.0);
	EXPECT_DOUBLE_EQ(ctx.sells[0].price, 10.49);
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// price_offset 把买入价顶过涨停 -> 收敛为涨停价且标记不可撤（L410-413、L421-426）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, buy_price_capped_to_upper_limit)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg(5, 0, 200));
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51, 10.60, 0));
	// targetPx = 10.50 + 0.01*200 = 12.50 > upper_limit(10.60) -> 10.60
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.60);
	EXPECT_TRUE(ctx.hasLog("modified to upper limit price"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// price_offset 把卖出价砸穿跌停 -> 收敛为跌停价（L427-432）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, sell_price_floored_to_lower_limit)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	ctx.pos = 100;
	WtVWapExeUnit unit;
	boot(unit, ctx, 50, mkCfg(5, 0, 200));
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51, 0, 10.20));
	// curQty=-1 -> sell；targetPx = 10.50 - 0.01*200 = 8.50 < lower_limit(10.20) -> 10.20
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].price, 10.20);
	EXPECT_TRUE(ctx.hasLog("modified to lower limit price"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 最后一个分桶（leftTimes==0）走「显示手数」兜底：量=diffQty，价 +5 个 tick（L379-383、L407-409）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, last_bucket_uses_showhand_price)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg(1, 0, 0, 1.0, 10.0, 0, 14400)); // _fire_span=0 so the next tick reaches the tail bucket
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));   // 第 1 次：25 手
	ASSERT_EQ(ctx.buys.size(), 1u);
	sendTick(unit, mkTick(kCode, 100100000, 10.60, 10.59, 10.61));   // 第 2 次：尾桶
	// _fired_times=1 == _total_times -> leftTimes=0 且 diffQty=100 != 0
	// curQty = max(100, 1) = 100；targetPx = 10.60 + 0.01*5 = 10.65
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 100.0);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.65);
	EXPECT_EQ(ctx.countLog("第 2 次发单"), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 拒单回报只登记待重试；更新且有效的交易 tick 才触发下一次计算。
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, rejected_order_retry_waits_for_fresh_tick)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	EXPECT_TRUE(unit.initializationSucceeded());
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	// 回报本身不能拿缓存 tick 重发。
	unit.on_entrust(9000, kCode, false, "rejected");
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.hasLog("deferred until a fresh in-session tick"));
	unit.on_entrust(9000, kCode, true, "ok");          // L288 成功回报不进这条链
	EXPECT_EQ(ctx.buys.size(), 1u);
	sendTick(unit, mkTick(kCode, 100000000, 10.55, 10.54, 10.56)); // duplicate timestamp is stale
	EXPECT_EQ(ctx.buys.size(), 1u);
	sendTick(unit, mkTick(kCode, 100100000, 10.60, 10.59, 10.61)); // only a fresh eligible tick retries
	EXPECT_EQ(ctx.buys.size(), 2u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_entrust：非本单元发出的订单直接忽略（L291-292）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, on_entrust_ignores_unknown_order)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	unit.on_entrust(55555, kCode, false, "not mine");
	EXPECT_EQ(ctx.buys.size(), 1u);                    // 没有因为别人的拒单而重算
	EXPECT_FALSE(ctx.hasLog("No tick of"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// diffQty==0 早退（L319-320），同时验证 get_real_target 的 DBL_MAX -> 0（L46-51）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, clear_target_dbl_max_exits_without_order)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.set_position(kCode, DBL_MAX);                 // 清仓标记 -> newVol=0 == realPos=0
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.sells.empty());
	sendTick(unit, mkTick(kCode, 100100000, 10.60, 10.59, 10.61));
	// 平仓后无持仓：清仓分支可达但可用多头为 0，仍不发单（BUG-R7-004 修复后的平仓对照）
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// BUG-R7-004（卡15最终复核的明确缺陷，迭代03 2026-11-04）：净持仓为零但有锁仓
// （多 2、空 2）时清仓指令原被“差额为零即返回”挡住；修复后先卖出可用多头。
//////////////////////////////////////////////////////////////////////////
namespace
{
class LockedVwapCtx : public FakeCtx
{
public:
	double getPosition(const char*, bool validOnly = true, int32_t flag = 3) override
	{
		const double lng = validOnly ? lockedLongAvail : lockedLong;
		const double sht = validOnly ? lockedShortAvail : lockedShort;
		if (flag == 1)
			return lng;
		if (flag == 2)
			return sht;
		return lng + sht;
	}
	double lockedLong = 2.0;
	double lockedLongAvail = 2.0;
	double lockedShort = -2.0;
	double lockedShortAvail = -2.0;
};
}

TEST(r7_vwap, clear_target_flattens_net_zero_locked_position)
{
	VwapFile vf(25.0);
	LockedVwapCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, DBL_MAX, mkCfg());
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.sells.size(), 1u) << "net zero must not hide the gross long under a clear command";
	EXPECT_GT(ctx.sells[0].qty, 0.0);
	EXPECT_LE(ctx.sells[0].qty, 2.0) << "never more than the available long";
	EXPECT_TRUE(ctx.sells[0].force) << "K58: a clear-branch order is an explicit close";
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

// K58（BUG-R7-004，用户裁定 2026-11-06）：净零锁仓、多头全部冻结时先买回可用空头（带平仓标志），
// 不卖冻结的多头。
TEST(r7_vwap_clear_k58, net_zero_with_frozen_long_buys_back_available_short)
{
	VwapFile vf(25.0);
	LockedVwapCtx ctx;
	ctx.lockedLongAvail = 0.0;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, DBL_MAX, mkCfg());
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.sells.empty()) << "a frozen long must not be sold";
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_GT(ctx.buys[0].qty, 0.0);
	EXPECT_LE(ctx.buys[0].qty, 2.0) << "never more than the available short";
	EXPECT_TRUE(ctx.buys[0].force);
	ctx.comm->release();
}

// K58：两边都不可用时不发单、记诊断，清仓目标保留；可用后在后续 tick 继续。
TEST(r7_vwap_clear_k58, unavailable_legs_wait_with_diagnostic_and_retry_later)
{
	VwapFile vf(25.0);
	LockedVwapCtx ctx;
	ctx.lockedLongAvail = 0.0;
	ctx.lockedShortAvail = 0.0;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, DBL_MAX, mkCfg());
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.hasLog("not yet available"));

	ctx.lockedLongAvail = 2.0;
	sendTick(unit, mkTick(kCode, 100100000, 10.60, 10.59, 10.61));
	ASSERT_EQ(ctx.sells.size(), 1u) << "the clear target must survive and be retried";
	EXPECT_TRUE(ctx.sells[0].force);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_channel_ready：未完成单为 0 但 OMS 有单 -> 判错单清空（L206-216）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, channel_ready_clears_unconfirmed_local_orders)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);                    // OMS 里有本地单 9000
	unit.on_channel_ready();                           // undone==0 && has_order
	EXPECT_FALSE(ctx.hasLog("clear all"));
	sendTick(unit, mkTick(kCode, 100100000, 10.60, 10.59, 10.61));
	EXPECT_TRUE(ctx.hasLog("not confirmed in trading channel, clear all"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_channel_ready：外部未完成单在下一笔有效行情中才清理。
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, channel_ready_cancels_unmanaged_and_blocks_calc)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	ctx.undone = 5.0;                                  // 买向外部挂单 5 手
	ctx.cancelBydirRetCnt = 2;
	WtVWapExeUnit unit;
	unit.init(&ctx, kCode, mkCfg());
	unit.set_position(kCode, 100);
	unit.on_channel_ready();
	EXPECT_TRUE(ctx.bydirCancels.empty());
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	// L199: isBuy = (undone > 0) -> true
	ASSERT_EQ(ctx.bydirCancels.size(), 1u);
	EXPECT_TRUE(ctx.bydirCancels[0].second);
	EXPECT_EQ(ctx.bydirCancels[0].first, kCode);
	EXPECT_TRUE(ctx.hasLog("cancelcnt -> 2"));         // L204
	EXPECT_TRUE(ctx.hasLog("尚有未完成的撤单指令"));      // L316
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 撤单回报逐笔收敛：_cancel_cnt 2 -> 1（L152-159）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, cancel_reports_decrement_cancel_cnt)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	ctx.undone = 5.0;
	ctx.cancelBydirRetCnt = 2;
	WtVWapExeUnit unit;
	unit.init(&ctx, kCode, mkCfg());
	unit.on_channel_ready();
	EXPECT_TRUE(ctx.bydirCancels.empty());
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51)); // fresh tick -> cancel external orders
	ASSERT_EQ(ctx.bydirCancels.size(), 1u);
	const uint32_t firstId = 9001;
	unit.on_order(firstId, kCode, true, 0, 10.50, true);
	EXPECT_TRUE(ctx.hasLog("cancelling done, cancelcnt -> 1"));   // L158
	EXPECT_EQ(ctx.countLog("cancelling done"), 1u);
	unit.on_order(firstId, kCode, true, 0, 10.50, true);          // 已摘除 -> L150 忽略
	EXPECT_EQ(ctx.countLog("cancelling done"), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 撤单回报不使用缓存 tick；下一笔新鲜有效 tick 才 fire_at_once。
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, cancel_report_defers_refire_until_next_fresh_in_session_tick)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	sendTick(unit, mkTick(kCode, 113000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);                    // 本地单 9000，_this_target = 25
	// leftover!=0 且 isCanceled -> 摘单、_cancel_cnt 仍为 0 -> L167 命中
	unit.on_order(9000, kCode, true, 5, 10.50, true);
	EXPECT_EQ(ctx.buys.size(), 1u); // without a new tick, do not reuse the 11:30 tick
	sendTick(unit, mkTick(kCode, 130000000, 10.60, 10.59, 10.61));
	// _cancel_times=1；fire_at_once(max(1, 25-0)=25)，按新的行情价重发。
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 25.0);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.61);
	EXPECT_TRUE(ctx.hasLog("deferred until a fresh in-session tick"));
	ctx.comm->release();
}

TEST(r7_vwap, canceled_retry_recalculates_from_current_total_position_and_sellable_quantity)
{
	VwapFile vf(100.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, -100, mkCfg());
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 100.0);

	unit.on_order(9000, kCode, false, 70, 10.50, true);
	ctx.pos = 30;             // position changed after the canceled slice
	ctx.availableLong = 5;    // only five contracts remain sellable
	sendTick(unit, mkTick(kCode, 100100000, 10.60, 10.59, 10.61));
	ASSERT_EQ(ctx.sells.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.sells[1].qty, 5.0);
	EXPECT_DOUBLE_EQ(ctx.sells[1].price, 10.59);
	ctx.comm->release();
}

TEST(r7_vwap, canceled_retry_waits_for_remaining_undone_quantity)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.on_order(9000, kCode, true, 5, 10.50, true);

	ctx.undone = 3;
	sendTick(unit, mkTick(kCode, 100100000, 10.60, 10.59, 10.61));
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.hasLog("unfinished quantity remains"));

	ctx.undone = 0;
	sendTick(unit, mkTick(kCode, 100200000, 10.70, 10.69, 10.71));
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 25.0);
	ctx.comm->release();
}

TEST(r7_vwap, out_of_session_quote_advances_tick_high_water_and_blocks_delayed_session_tick)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	sendTick(unit, mkTick(kCode, 110000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);

	// The valid-format 11:45 quote is outside the session, but it still moves
	// event-time high-water; the delayed 11:30 quote cannot then trigger retry.
	sendTick(unit, mkTick(kCode, 114500000, 10.60, 10.59, 10.61));
	unit.on_order(9000, kCode, true, 5, 10.50, true);
	sendTick(unit, mkTick(kCode, 113000000, 10.70, 10.69, 10.71));
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.hasLog("out-of-order tick"));
	ctx.comm->release();
}

TEST(r7_vwap, cancellation_callback_after_close_does_not_reuse_last_valid_tick)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 1000, mkCfg());
	sendTick(unit, mkTick(kCode, 145900000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	sendTick(unit, mkTick(kCode, 150000000, 10.60, 10.59, 10.61)); // valid close endpoint, throttled
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.on_order(9000, kCode, true, 5, 10.50, true);
	EXPECT_EQ(ctx.buys.size(), 1u); // no post-close tick arrived
	EXPECT_TRUE(ctx.hasLog("deferred until a fresh in-session tick"));
	ctx.comm->release();
}

TEST(r7_vwap, rejection_callback_after_close_does_not_recalculate_from_cached_tick)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	sendTick(unit, mkTick(kCode, 145900000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	sendTick(unit, mkTick(kCode, 150000000, 10.60, 10.59, 10.61)); // newer cached tick, throttled
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.on_entrust(9000, kCode, false, "rejected after close");
	EXPECT_EQ(ctx.buys.size(), 1u); // retry waits for the next valid trading tick
	EXPECT_TRUE(ctx.hasLog("rejected order retry deferred"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 重发沿用常规下单的价格模式；撤单次数增加一个 tick 的方向性偏移。
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, price_mode_matrix_matches_initial_and_canceled_retry_prices)
{
	VwapFile vf(25.0);
	const double buyInitial[] = { 10.50, 10.49, 10.51 };
	const double sellInitial[] = { 10.50, 10.51, 10.49 };
	const double buyRetry[] = { 10.61, 10.60, 10.62 };
	const double sellRetry[] = { 10.59, 10.60, 10.58 };
	for (uint32_t mode = 0; mode <= 2; ++mode)
	{
		for (int side = 0; side < 2; ++side)
		{
			const bool isBuy = side == 0;
			SCOPED_TRACE(::testing::Message() << "price_mode=" << mode << ", isBuy=" << isBuy);
			FakeCtx ctx;
			ctx.comm = mkComm();
			if (!isBuy)
				ctx.pos = 100;
			WtVWapExeUnit unit;
			boot(unit, ctx, isBuy ? 100 : 0, mkCfg(5, mode));
			sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
			const auto& initialOrders = isBuy ? ctx.buys : ctx.sells;
			ASSERT_EQ(initialOrders.size(), 1u);
			EXPECT_DOUBLE_EQ(initialOrders[0].price, isBuy ? buyInitial[mode] : sellInitial[mode]);
			unit.on_order(9000, kCode, isBuy, 5, initialOrders[0].price, true);
			sendTick(unit, mkTick(kCode, 100100000, 10.60, 10.59, 10.61));
			const auto& retryOrders = isBuy ? ctx.buys : ctx.sells;
			ASSERT_EQ(retryOrders.size(), 2u);
			EXPECT_DOUBLE_EQ(retryOrders[1].price, isBuy ? buyRetry[mode] : sellRetry[mode]);
			EXPECT_TRUE(isBuy ? ctx.sells.empty() : ctx.buys.empty());
			ctx.comm->release();
		}
	}
}

TEST(r7_vwap, canceled_retry_keeps_upper_and_lower_limit_caps)
{
	VwapFile vf(25.0);
	for (int side = 0; side < 2; ++side)
	{
		const bool isBuy = side == 0;
		SCOPED_TRACE(::testing::Message() << "isBuy=" << isBuy);
		FakeCtx ctx;
		ctx.comm = mkComm();
		if (!isBuy)
			ctx.pos = 100;
		WtVWapExeUnit unit;
		boot(unit, ctx, isBuy ? 100 : 0, mkCfg(5, 2));
		sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
		const auto& initialOrders = isBuy ? ctx.buys : ctx.sells;
		ASSERT_EQ(initialOrders.size(), 1u);
		unit.on_order(9000, kCode, isBuy, 5, initialOrders[0].price, true);
		if (isBuy)
			sendTick(unit, mkTick(kCode, 100100000, 10.60, 10.59, 10.60, 10.60, 0));
		else
			sendTick(unit, mkTick(kCode, 100100000, 10.20, 10.19, 10.21, 0, 10.20));
		const auto& retryOrders = isBuy ? ctx.buys : ctx.sells;
		ASSERT_EQ(retryOrders.size(), 2u);
		EXPECT_DOUBLE_EQ(retryOrders[1].price, isBuy ? 10.60 : 10.20);
		EXPECT_TRUE(ctx.hasLog(isBuy ? "modified to upper limit price" : "modified to lower limit price"));
		ctx.comm->release();
	}
}

//////////////////////////////////////////////////////////////////////////
// 重发价被涨停收敛时 fire_at_once 也走同一条约束（L472-476）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, fire_at_once_upper_limit_cap)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg(5, 0, 300));
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51, 10.60, 0));
	// do_calc: 10.50 + 0.01*300 = 13.50 -> 收敛到 10.60
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.60);
	unit.on_order(9000, kCode, true, 5, 10.60, true);
	EXPECT_EQ(ctx.buys.size(), 1u);
	sendTick(unit, mkTick(kCode, 100100000, 10.50, 10.49, 10.51, 10.60, 0));
	// fire_at_once: 10.50 + 0.01*1 = 10.51 < 10.60 -> 不被收敛，正常发出
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.51);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_order 未知 id 被忽略；全额成交回报复位 _cancel_times（L150-151、L162-165）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, on_order_unknown_id_and_filled_report)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.on_order(4242, kCode, true, 0, 10.50, true);  // 不在监控中 -> return
	EXPECT_FALSE(ctx.hasLog("has filled"));
	unit.on_order(9000, kCode, true, 0, 10.50, false); // 全额成交
	EXPECT_TRUE(ctx.hasLog("has filled"));             // L164
	EXPECT_EQ(ctx.buys.size(), 1u);                    // 未触发重发（isCanceled=false）
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_tick 入参防御 + channel_lost 阻止后续回报重发（L228-229、L511-513）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, on_tick_rejects_null_and_foreign_code)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	unit.init(&ctx, kCode, mkCfg());
	unit.set_position(kCode, 100);
	unit.on_channel_ready();
	unit.on_tick(NULL);
	WTSTickData* foreign = mkTick("if2403", 100000000, 10.50, 10.49, 10.51);
	unit.on_tick(foreign);
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_FALSE(ctx.hasLog("次发单"));
	foreign->release();                                // 未被 retain，用例侧回收
	unit.on_channel_lost();
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_tick 首笔：目标 == 未完成 + 实仓 -> 不触发计算（L252 反例）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, first_tick_skips_calc_when_target_already_covered)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	ctx.pos = 40;
	ctx.undone = 10;                                   // 40 + 10 == 目标 50
	WtVWapExeUnit unit;
	unit.init(&ctx, kCode, mkCfg());
	unit.set_position(kCode, 50);
	unit.on_channel_ready();                           // 撤外部单（顺带覆盖 L189-204）
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 反向未完成单：先撤反向挂单且本轮不发新单（L324-335）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, opposite_live_order_is_canceled_before_firing)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	ctx.cancelBydirRetCnt = 1;
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg();
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	unit.set_position(kCode, 0);
	unit.on_channel_ready();
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51)); // reconcile with no outstanding quantity
	ctx.undone = -3.0;                                 // 场内 3 手卖单，目标是买
	unit.set_position(kCode, 100);
	sendTick(unit, mkTick(kCode, 100100000, 10.50, 10.49, 10.51));
	// diffQty=100, undone=-3 -> 100*(-3)<0 -> L326 isBuy=(undone>0)=false
	ASSERT_EQ(ctx.bydirCancels.size(), 1u);
	EXPECT_FALSE(ctx.bydirCancels[0].second);
	EXPECT_TRUE(ctx.hasLog("live opposite order of")); // L332
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 同向未完成单：本轮放弃（L336-340）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, same_direction_live_order_blocks_this_round)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg();
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	unit.set_position(kCode, 0);
	unit.on_channel_ready();
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	ctx.undone = 3.0;
	unit.set_position(kCode, 100);
	sendTick(unit, mkTick(kCode, 100100000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.hasLog("上一轮有挂单未完成"));         // L338
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 没有 tick 时的早退日志（L341-345），且 boot 前不该出现
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, calc_without_tick_exits)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	unit.init(&ctx, kCode, mkCfg());
	unit.set_position(kCode, 100);
	EXPECT_FALSE(ctx.hasLog("没有最新的tick数据"));       // 通道未就绪时先 return
	unit.on_channel_ready();
	EXPECT_TRUE(ctx.hasLog("reconciliation deferred until the next fresh in-session tick"));
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 收盘后的 tick 不属于任何分布桶，必须跳过。
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, timestamp_after_close_is_rejected)
{
	VwapFile vf(12.0);                                 // 每格都是 12
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	sendTick(unit, mkTick(kCode, 150100000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.hasLog("outside the trading sections"));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

TEST(r7_vwap, late_afternoon_uses_session_bucket_inside_241_value_distribution)
{
	VwapFile vf(VwapFile::V2Tag{}, "R7_DAY", 0, { {930, 1130}, {1300, 1500} }, 241);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 1000, mkCfg());
	sendTick(unit, mkTick(kCode, 145900000, 10.50, 10.49, 10.51));
	// 14:59 is trading-minute bucket 239 (09:30-11:30 plus 13:00-14:59).
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 240.0);
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

TEST(r7_vwap, lunch_break_does_not_advance_profile_and_next_session_segment_runs)
{
	VwapFile vf(VwapFile::V2Tag{}, "R7_DAY", 0, { {930, 1130}, {1300, 1500} }, 241);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 1000, mkCfg());
	sendTick(unit, mkTick(kCode, 120000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.hasLog("outside the trading sections"));
	sendTick(unit, mkTick(kCode, 130000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 121.0); // profile bucket 120
	ctx.comm->release();
}

TEST(r7_vwap, v2_profile_maps_a_noncanonical_session_from_its_sections)
{
	VwapFile vf(VwapFile::V2Tag{}, "R7_ALT", 0, { {900, 1100}, {1230, 1400} }, 211);
	FakeCtx ctx;
	ctx.setSession(WTSSessionInfo::create("R7_ALT", "R7_ALT", 0));
	ctx.sess->addTradingSection(900, 1100);
	ctx.sess->addTradingSection(1230, 1400);
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg(5, 0, 0, 1.0, 10.0, 0, 0, 900, 1400);
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	ASSERT_TRUE(unit.initializationSucceeded());
	unit.set_position(kCode, 1000);
	unit.on_channel_ready();
	sendTick(unit, mkTick(kCode, 135900000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 210.0); // 120 + 89 elapsed trading minutes
	ctx.comm->release();
}

TEST(r7_vwap, v2_profile_and_execution_window_support_offset_overnight_session)
{
	VwapFile vf(VwapFile::V2Tag{}, "R7_NIGHT", 720, { {2100, 230} }, 331);
	FakeCtx ctx;
	ctx.setSession(WTSSessionInfo::create("R7_NIGHT", "R7_NIGHT", 720));
	ctx.sess->addTradingSection(2100, 230);
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg(5, 0, 0, 1.0, 10.0, 0, 0, 2100, 230);
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	ASSERT_TRUE(unit.initializationSucceeded());
	EXPECT_TRUE(ctx.hasLog("执行时限 19800 秒"));
	unit.set_position(kCode, 1000);
	unit.on_channel_ready();
	sendTick(unit, mkTick(kCode, 210000000, 10.40, 10.39, 10.41, 0, 0, 20240101, 20240102)); // 21:00, bucket 0, next trading date
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 1.0);
	sendTick(unit, mkTick(kCode, 3000000, 10.50, 10.49, 10.51, 0, 0, 20240102, 20240102)); // 00:30, bucket 210
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 211.0);
	sendTick(unit, mkTick(kCode, 23000000, 10.60, 10.59, 10.61, 0, 0, 20240102, 20240102)); // exact close endpoint
	ASSERT_EQ(ctx.buys.size(), 3u);
	EXPECT_DOUBLE_EQ(ctx.buys[2].qty, 331.0);
	sendTick(unit, mkTick(kCode, 23000001, 10.70, 10.69, 10.71, 0, 0, 20240102, 20240102)); // close + 1 ms
	EXPECT_EQ(ctx.buys.size(), 3u);
	ctx.comm->release();
}

TEST(r7_vwap, overnight_tick_with_action_date_equal_to_next_trading_date_fails_closed)
{
	VwapFile vf(VwapFile::V2Tag{}, "R7_NIGHT", 720, { {2100, 230} }, 331);
	FakeCtx ctx;
	ctx.setSession(WTSSessionInfo::create("R7_NIGHT", "R7_NIGHT", 720));
	ctx.sess->addTradingSection(2100, 230);
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg(5, 0, 0, 1.0, 10.0, 0, 0, 2100, 230);
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	ASSERT_TRUE(unit.initializationSucceeded());
	unit.set_position(kCode, 100);
	unit.on_channel_ready();
	// This is the ParserYD convention for night quotes: action_date is the
	// future TradingDay. It cannot be safely normalized by VWAP's session date.
	sendTick(unit, mkTick(kCode, 210000000, 10.40, 10.39, 10.41, 0, 0, 20240102, 20240102));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.hasLog("action_date and trading_date do not match"));
	ctx.comm->release();
}

TEST(r7_vwap, unrepresentable_session_date_does_not_advance_high_water_or_order)
{
	VwapFile vf(VwapFile::V2Tag{}, "R7_NIGHT", 720, { {2100, 230} }, 331);
	FakeCtx ctx;
	ctx.setSession(WTSSessionInfo::create("R7_NIGHT", "R7_NIGHT", 720));
	ctx.sess->addTradingSection(2100, 230);
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg(5, 0, 0, 1.0, 10.0, 0, 0, 2100, 230);
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	ASSERT_TRUE(unit.initializationSucceeded());
	unit.set_position(kCode, 1000);
	unit.on_channel_ready();
	// The +720-minute session would move this action date into year 10000,
	// outside the checked YYYYMMDD domain. A rejected tick must not poison
	// the timestamp watermark or place an order.
	// 交易日字段须是合法日期（改前为 100000101，被更早的“非法日历日期”检查拦下，到不了会话偏移检查）。
	sendTick(unit, mkTick(kCode, 210000000, 10.40, 10.39, 10.41, 0, 0, 99991231, 99991231));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_EQ(0u, unit._tick_time_high_water);
	EXPECT_TRUE(ctx.hasLog("session-adjusted date cannot be verified"));
	sendTick(unit, mkTick(kCode, 210000000, 10.40, 10.39, 10.41, 0, 0, 20240101, 20240102));
	EXPECT_EQ(1u, ctx.buys.size());
	ctx.comm->release();
}

TEST(r7_vwap, profile_for_a_different_session_is_rejected)
{
	VwapFile vf(VwapFile::V2Tag{}, "OTHER", 0, { {930, 1130}, {1300, 1500} }, 241);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg();
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	EXPECT_FALSE(unit.initializationSucceeded());
	EXPECT_TRUE(ctx.hasLog("no profile matching this session"));
	unit.set_position(kCode, 100);
	unit.on_channel_ready();
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

TEST(r7_vwap, short_action_time_is_rejected_without_throw_or_order)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	sendTick(unit, mkTick(kCode, 9300000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.hasLog("action time 9300000 rejected"));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

TEST(r7_vwap, invalid_hour_and_minute_are_rejected_without_order)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	sendTick(unit, mkTick(kCode, 246000000, 10.50, 10.49, 10.51)); // hour 24, minute 60
	sendTick(unit, mkTick(kCode, 126000000, 10.50, 10.49, 10.51)); // minute 60
	EXPECT_EQ(ctx.countLog("action time"), 2u);
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

TEST(r7_vwap, malformed_tick_does_not_advance_event_time_high_water)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.on_order(9000, kCode, true, 5, 10.50, true);

	// mktime would normalize this malformed 24:60 time into a later date.
	// It must be rejected before it can poison the event-time high-water.
	sendTick(unit, mkTick(kCode, 246000000, 10.60, 10.59, 10.61));
	sendTick(unit, mkTick(kCode, 100050000, 10.60, 10.59, 10.61, 0, 0, 20240231, 20240231));
	EXPECT_EQ(ctx.buys.size(), 1u);
	sendTick(unit, mkTick(kCode, 100100000, 10.70, 10.69, 10.71));
	EXPECT_EQ(ctx.buys.size(), 2u);
	EXPECT_FALSE(ctx.hasLog("out-of-order tick"));
	EXPECT_TRUE(ctx.hasLog("invalid calendar date"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 由 session 累计交易分钟计算 begin/end 窗口，并通过 init 日志检查推导结果。
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, init_logs_total_seconds_from_begin_end_window)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	{
		WtVWapExeUnit unit;
		WTSVariant* cfg = mkCfg(5, 0, 0, 1.0, 10.0, 0, 0, 930, 1030);
		unit.init(&ctx, kCode, cfg);
		cfg->release();
		unit.on_channel_ready();
		// One trading-hour window: tail=0, total_times=5 -> interval=720 seconds.
		EXPECT_TRUE(ctx.hasLog("执行时限 3600 秒"));
		EXPECT_TRUE(ctx.hasLog("间隔时间 720 秒"));
	}
	{
		WtVWapExeUnit unit;
		WTSVariant* cfg = mkCfg(5, 0, 0, 1.0, 10.0, 0, 0, 1330, 1500);
		unit.init(&ctx, kCode, cfg);
		cfg->release();
		// One afternoon trading-hour window: tail=0, total_times=5 -> interval=1080 seconds.
		EXPECT_TRUE(ctx.hasLog("执行时限 5400 秒"));
		EXPECT_TRUE(ctx.hasLog("间隔时间 1080 秒"));
	}
	ctx.comm->release();
}

TEST(r7_vwap, init_calculates_interval_from_duration_tail_and_planned_count)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg(6, 0, 0, 1.0, 10.0, 0, 600, 930, 1030);
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	EXPECT_TRUE(ctx.hasLog("执行时限 3600 秒"));
	EXPECT_TRUE(ctx.hasLog("收尾时间 600 秒"));
	EXPECT_TRUE(ctx.hasLog("间隔时间 500 秒"));
	ctx.comm->release();
}

TEST(r7_vwap, init_uses_trading_duration_across_lunch_break)
{
	VwapFile vf(VwapFile::V2Tag{}, "R7_DAY", 0, { {930, 1130}, {1300, 1500} }, 241);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg(4, 0, 0, 1.0, 10.0, 0, 0, 1100, 1330);
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	ASSERT_TRUE(unit.initializationSucceeded());
	EXPECT_TRUE(ctx.hasLog("执行时限 3600 秒")); // 11:00-11:30 + 13:00-13:30
	EXPECT_TRUE(ctx.hasLog("间隔时间 900 秒"));
	ctx.comm->release();
}

TEST(r7_vwap, configured_interval_throttles_consecutive_ticks)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg(1, 0, 0, 1.0, 10.0, 0, 0, 930, 1500));
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	sendTick(unit, mkTick(kCode, 100100000, 10.60, 10.59, 10.61));
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.hasLog("间隔时间 14400 秒"));
	ctx.comm->release();
}

TEST(r7_vwap, caller_release_tick_replacement_and_unit_destruction_preserve_ownership)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	{
		WtVWapExeUnit unit;
		boot(unit, ctx, 100, mkCfg(1, 0, 0, 1.0, 10.0, 0, 0, 930, 1500));
		WTSTickData* first = mkTick(kCode, 100000000, 10.50, 10.49, 10.51);
		unit.on_tick(first);
		first->release();
		ASSERT_EQ(ctx.buys.size(), 1u);

		WTSTickData* replacement = mkTick(kCode, 100100000, 10.60, 10.59, 10.61);
		unit.on_tick(replacement);
		replacement->release();
		EXPECT_EQ(ctx.buys.size(), 1u);
	}
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// zero/invalid schedule values fail closed and report a diagnostic before callbacks can place orders.
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, init_zero_total_times_rejected_and_inert)
{
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg(0);
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	EXPECT_TRUE(ctx.hasLog("total_times 必须大于0"));
	EXPECT_FALSE(unit.initializationSucceeded());
	unit.set_position(kCode, 100);
	unit.on_channel_ready();
	WTSTickData* tick = mkTick(kCode, 100000000, 10.50, 10.49, 10.51);
	unit.on_tick(tick);
	tick->release();
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

TEST(r7_vwap, init_without_session_fails_closed)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	ctx.setSession(NULL);
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg();
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	EXPECT_FALSE(unit.initializationSucceeded());
	EXPECT_TRUE(ctx.hasLog("commodity or trading session is unavailable"));
	unit.set_position(kCode, 100);
	unit.on_channel_ready();
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

TEST(r7_vwap, init_without_commodity_fails_closed)
{
	VwapFile vf(25.0);
	FakeCtx ctx; // The fixture supplies a valid trading session; only commodity metadata is absent.
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg();
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	EXPECT_FALSE(unit.initializationSucceeded());
	EXPECT_TRUE(ctx.hasLog("commodity or trading session is unavailable"));
	unit.set_position(kCode, 100);
	unit.on_channel_ready();
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_TRUE(ctx.byidCancels.empty());
	EXPECT_TRUE(ctx.bydirCancels.empty());
}

TEST(r7_vwap, init_tail_beyond_duration_rejected_and_inert)
{
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg(5, 0, 0, 1.0, 10.0, 0, 3601, 930, 1030);
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	EXPECT_TRUE(ctx.hasLog("tail_secs 3601 超出执行时长 3600 秒"));
	unit.set_position(kCode, 100);
	unit.on_channel_ready();
	WTSTickData* tick = mkTick(kCode, 100000000, 10.50, 10.49, 10.51);
	unit.on_tick(tick);
	tick->release();
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

TEST(r7_vwap, init_positive_total_times_control_returns)
{
	r7_test::ScopedTempDir probeDir("WT_R7_002_VWAP");
	ASSERT_TRUE(probeDir.ready());
	const r7_test::ProbeResult result = r7_test::run_bounded_child(probeDir.path(), 5, run_vwap_init_probe);
	r7_test::expect_normal_return(result);
	EXPECT_TRUE(probeDir.cleanup()) << "failed to remove private init-probe directory";
}

//////////////////////////////////////////////////////////////////////////
// 初始化完整预检分布文件；失败时执行单元保持禁用，不能触发发单。
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, init_rejects_empty_distribution_and_does_not_fire)
{
	VwapFile vf(25.0, 0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg();
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	EXPECT_TRUE(ctx.hasLog("profile input is empty"));
	EXPECT_FALSE(unit.initializationSucceeded());
	unit.set_position(kCode, 100);
	unit.on_channel_ready();
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

TEST(r7_vwap, init_rejects_short_distribution_and_does_not_fire)
{
	VwapFile vf(25.0, 10);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg();
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	EXPECT_TRUE(ctx.hasLog("exactly 241 values"));
	EXPECT_FALSE(unit.initializationSucceeded());
	unit.set_position(kCode, 100);
	unit.on_channel_ready();
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

TEST(r7_vwap, init_rejects_malformed_distribution_even_after_complete_prefix)
{
	VwapFile vf(25.0, 242, true);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	WTSVariant* cfg = mkCfg();
	unit.init(&ctx, kCode, cfg);
	cfg->release();
	EXPECT_TRUE(ctx.hasLog("invalid or non-finite number"));
	EXPECT_FALSE(unit.initializationSucceeded());
	unit.set_position(kCode, 100);
	unit.on_channel_ready();
	sendTick(unit, mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// timeout 记录没有可验证身份时，VWAP 不自动撤单且持续保留监控。
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, timeout_without_verified_identity_keeps_monitor_and_never_cancels)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	{
		WtVWapExeUnit unit;
		WTSVariant* cfg = mkCfg(5, 0, 0, 1.0, 10.0, 1);
		unit.init(&ctx, kCode, cfg);
		cfg->release();
		unit.set_position(kCode, 0);

		const uint32_t id = 9351;
		unit._orders_mon.push_order(&id, 1, ctx.curTime);
		sendTick(unit, mkTick(kCode, 93000000, 10.50, 10.49, 10.51));
		ctx.curTime += 999;
		sendTick(unit, mkTick(kCode, 93001000, 10.50, 10.49, 10.51));
		EXPECT_EQ(ctx.countLog("Expired order 9351"), 0u);
		ctx.curTime += 1;
		sendTick(unit, mkTick(kCode, 93002000, 10.50, 10.49, 10.51));
		EXPECT_TRUE(ctx.byidCancels.empty());
		EXPECT_TRUE(ctx.bydirCancels.empty());
		EXPECT_TRUE(unit._orders_mon.has_order(id));
		EXPECT_EQ(ctx.countLog("Expired order 9351"), 1u);

		unit.on_channel_ready(); // reconciliation cannot clear the quarantine latch
		EXPECT_TRUE(unit._orders_mon.has_unverified_timeout());
		unit.set_position(kCode, 10);
		unit.fire_at_once(1.0);
		sendTick(unit, mkTick(kCode, 93003000, 10.50, 10.49, 10.51));
		EXPECT_TRUE(ctx.byidCancels.empty());
		EXPECT_TRUE(ctx.bydirCancels.empty());
		EXPECT_FALSE(unit._orders_mon.has_order(id));
		EXPECT_TRUE(unit._orders_mon.has_unverified_timeout());
		EXPECT_TRUE(ctx.buys.empty());
		EXPECT_TRUE(ctx.sells.empty());
		EXPECT_EQ(ctx.countLog("Expired order 9351"), 1u);
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

TEST(r7_vwap, wp1_timeout_with_host_verified_identity_cancels_once_or_latches)
{
	for (int refuse = 0; refuse < 2; ++refuse)
	{
		VwapFile vf(25.0);
		FakeCtx ctx;
		ctx.comm = mkComm();
		ctx.cancelByIdRet = refuse == 0;
		{
			WtVWapExeUnit unit;
			WTSVariant* cfg = mkCfg(5, 0, 0, 1.0, 10.0, 1);
			cfg->append(WT_EXEC_PLUGIN_V2_HOST_ORDER_IDENTITY_KEY, (uint32_t)1);
			unit.init(&ctx, kCode, cfg);
			cfg->release();
			unit.set_position(kCode, 0);
			const uint32_t id = 9361;
			unit._orders_mon.push_order(&id, 1, ctx.curTime);
			sendTick(unit, mkTick(kCode, 93000000, 10.50, 10.49, 10.51));
			ctx.curTime += 1000;
			sendTick(unit, mkTick(kCode, 93002000, 10.50, 10.49, 10.51));
			ASSERT_EQ(1u, ctx.byidCancels.size()) << "refuse=" << refuse;
			EXPECT_EQ(id, ctx.byidCancels[0]);
			EXPECT_TRUE(unit._orders_mon.has_order(id));
			EXPECT_EQ(refuse == 1, unit._orders_mon.has_unverified_timeout());
			EXPECT_EQ(refuse == 0, ctx.countLog("canceled with host-verified identity") == 1u);
			EXPECT_EQ(refuse == 1, ctx.countLog("host could not verify its identity") == 1u);
			ctx.curTime += 2000;
			sendTick(unit, mkTick(kCode, 93004000, 10.50, 10.49, 10.51));
			EXPECT_EQ(1u, ctx.byidCancels.size()) << "claimed or latched order is never re-cancelled";
		}
		ctx.comm->release();
	}
}
