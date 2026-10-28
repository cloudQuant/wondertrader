// R7 lane：WtVWapExeUnit（期货 VWAP 分片执行单元）
// 目标文件: src/WtExeFact/WtVWapExeUnit.cpp / .h
// 覆盖面: init 的 VWAP 分布文件读取（缺文件早退 / 解析循环）、do_calc 的每一条早退与主发单路径、
//         calTmSecs / calTmStamp 的时间分桶、on_tick 首笔与后续、on_order 撤单计数与重发、
//         on_channel_ready 三个分支、on_entrust 拒单重算、fire_at_once 价格模式与涨跌停收敛。
//
// 引用计数约定（实测 Includes/WTSObject.hpp:23 m_uRefs 起始为 1）：
//   WTSTickData::create -> 1；on_tick L244 retain -> 2；do_calc 发单成功后 L445 有一处
//   **未配对的 release** -> 1；单元析构 L40-41 再 release -> 0（回池）。
//   所以凡是喂进 on_tick 且触发过发单的 tick，用例侧**不再 release**，否则会二次释放踩到池化对象。
//   从未被单元持有的 tick（代码不匹配）才由用例自己 release。
//
// 时序：本文件不调用挂钟。on_tick 的非首笔分支里 `now - _last_fire_time >= _fire_span*1000`
// 依赖产品侧 TimeUtils；init 在 L120 计算 _fire_span 时 _total_secs 仍是 0（L123 才赋值），
// 因此 tail_secs=0 时 _fire_span 恒为 0，该判定恒真 → 分支变得可判定
// （这是缺陷，见 bugs/R7_exefact.md BUG-R7-001）。
//
// 驱动顺序固定为 init -> set_position -> on_channel_ready -> on_tick：
// set_position 在 L498 用 _code 比对，init 之前 _code 为空串，目标会被静默丢弃。
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "Includes/ExecuteDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "WtExeFact/WtVWapExeUnit.h"

using namespace wtp;

namespace
{
const char* const kCode = "tv2403";

class FakeCtx : public ExecuteContext
{
public:
	struct Rec { std::string code; double price; double qty; };

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return NULL; }
	WTSTickData* grabLastTick(const char*) override { return NULL; }
	double getPosition(const char*, bool = true, int32_t = 3) override { return pos; }
	OrderMap* getOrders(const char*) override { return NULL; }
	double getUndoneQty(const char*) override { return undone; }

	OrderIDs buy(const char* stdCode, double price, double qty, bool) override
	{
		buys.push_back({ stdCode, price, qty });
		OrderIDs ids{ nextId++ };
		return ids;
	}
	OrderIDs sell(const char* stdCode, double price, double qty, bool) override
	{
		sells.push_back({ stdCode, price, qty });
		OrderIDs ids{ nextId++ };
		return ids;
	}
	bool cancel(uint32_t localid) override { byidCancels.push_back(localid); return true; }
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
	uint64_t getCurTime() override { return 20240101093000000ull; }

	double pos = 0;
	double undone = 0;
	uint32_t nextId = 9000;
	uint32_t cancelBydirRetCnt = 2;
	WTSCommodityInfo* comm = NULL;
	WTSSessionInfo* sess = NULL;      // 置 NULL -> on_tick L239 的在交易时段过滤被跳过
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
// do_calc L372 用 calTmStamp 的返回值（0..240，见 L76-81 的上界）直接下标 VwapAim，
// 所以正常用例要铺满 241 格。
class VwapFile
{
public:
	explicit VwapFile(double bucketVal, uint32_t rows = 241)
	{
		_path = std::string("Vwap_") + commName() + ".txt";
		std::ofstream f(_path.c_str());
		for (uint32_t i = 0; i < rows; i++)
		{
			if (i)
				f << ",";
			f << bucketVal;
		}
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
	double bidPx, double askPx, double upper = 0, double lower = 0)
{
	WTSTickStruct ts;
	snprintf(ts.code, sizeof(ts.code), "%s", code);
	ts.action_date = 20240101;
	ts.trading_date = 20240101;
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

// 见 L110-119 实际读取的键
WTSVariant* mkCfg(uint32_t totalTimes = 5, uint32_t priceMode = 0, uint32_t priceOffset = 0,
	double minOpenLots = 1.0, double lots = 10.0, uint32_t ordSticky = 0,
	uint32_t tailSecs = 0, uint32_t beginTime = 930, uint32_t endTime = 1500)
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
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
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
	unit.init(&ctx, kCode, mkCfg());
	unit.on_channel_ready();
	// L130 原文： "Vwap file %s not exists ,check and return."
	EXPECT_TRUE(ctx.hasLog("not exists"));
	EXPECT_EQ(ctx.countLog("not exists"), 1u);
	// 初始化日志（L122）仍然先于早退打出
	EXPECT_TRUE(ctx.hasLog("初始化完成"));
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
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	// calTmStamp(100000000) -> "100000000": hour=10,minute=0 -> L70 -> (10-9)*60+0-30 = 30
	// VwapAim[30] = 25 -> _Vwap_vol = 25-0 = 25 -> curQty = max(25,1)*|100|/100 = 25
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
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	// _Vwap_vol = 0-0 = 0 -> max(0, minopenlots=7) = 7 -> *|100|/100 = 7
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 7.0);
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
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
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
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
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
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51, 10.60, 0));
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
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51, 0, 10.20));
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
	boot(unit, ctx, 100, mkCfg(1, 0, 0, 1.0, 10.0));   // total_times=1
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51));   // 第 1 次：25 手
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.on_tick(mkTick(kCode, 100100000, 10.60, 10.59, 10.61));   // 第 2 次：尾桶
	// _fired_times=1 == _total_times -> leftTimes=0 且 diffQty=100 != 0
	// curQty = max(100, 1) = 100；targetPx = 10.60 + 0.01*5 = 10.65
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 100.0);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.65);
	EXPECT_EQ(ctx.countLog("第 2 次发单"), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 同一笔 tick 不重复下单（L364-369 tick 时间闸门），拒单重算也走这里
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, stale_tick_gate_blocks_second_order)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	// L286-297 拒单 -> erase_order -> do_calc；没有新 tick：
	// curTickTime = 20240101*1e9 + 100000000 与 L370 记录的 _last_tick_time 相等 -> L365 拦下
	unit.on_entrust(9000, kCode, false, "rejected");
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.hasLog("No tick of"));
	unit.on_entrust(9000, kCode, true, "ok");          // L288 成功回报不进这条链
	EXPECT_EQ(ctx.buys.size(), 1u);
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
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
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
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.set_position(kCode, DBL_MAX);                 // 清仓标记 -> newVol=0 == realPos=0
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.sells.empty());
	unit.on_tick(mkTick(kCode, 100100000, 10.60, 10.59, 10.61));
	// L348 的 curPos==newVol 分支在 L319 之后不可达（见 BUG-R7-004），所以这里仍然没有清仓卖单
	EXPECT_TRUE(ctx.sells.empty());
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
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);                    // OMS 里有本地单 9000
	unit.on_channel_ready();                           // undone==0 && has_order
	EXPECT_TRUE(ctx.hasLog("not confirmed in trading channel, clear all"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_channel_ready：有外部未完成单且 OMS 空 -> 全撤并抬起 _cancel_cnt（L189-204）
// 随后的 do_calc 必须因「尚有未完成的撤单指令」退出（L314-318）
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
	unit.on_channel_ready();                           // 撤外部单 -> 9001/9002, _cancel_cnt=2
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
// 撤单回报 + 目标未达成 -> fire_at_once 重发，并按 _cancel_times 加价（L167-177、L448-493）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, cancel_report_refires_at_once_with_cancel_times_offset)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);                    // 本地单 9000，_this_target = 25
	// leftover!=0 且 isCanceled -> 摘单、_cancel_cnt 仍为 0 -> L167 命中
	unit.on_order(9000, kCode, true, 5, 10.50, true);
	// _cancel_times=1；fire_at_once(max(1, 25-0)=25)
	// price_mode=0 -> curTick->price()=10.50，L470 再加 priceTick*_cancel_times = 0.01
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 25.0);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.51);
	EXPECT_TRUE(ctx.hasLog("re_fire will be done"));   // L172
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// fire_at_once 的 price_mode==1 取 ask，与 do_calc 的同一模式取 bid 相反（L401 vs L463）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, fire_at_once_price_mode_1_takes_ask_price)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg(5, 1));
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.49);        // do_calc 模式1买入 -> bid
	unit.on_order(9000, kCode, true, 5, 10.49, true);  // -> fire_at_once
	ASSERT_EQ(ctx.buys.size(), 2u);
	// fire_at_once: price_mode==1 && isBuy -> askprice(0)=10.51，再 +0.01*1
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.52);
	EXPECT_LT(ctx.buys[0].price, ctx.buys[1].price);
	ctx.comm->release();
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
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51, 10.60, 0));
	// do_calc: 10.50 + 0.01*300 = 13.50 -> 收敛到 10.60
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.60);
	unit.on_order(9000, kCode, true, 5, 10.60, true);
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
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.on_order(4242, kCode, true, 0, 10.50, true);  // 不在监控中 -> return
	EXPECT_FALSE(ctx.hasLog("has filled"));
	unit.on_order(9000, kCode, true, 0, 10.50, false); // 全额成交
	EXPECT_TRUE(ctx.hasLog("has filled"));             // L164
	EXPECT_EQ(ctx.buys.size(), 1u);                    // 未触发重发（isCanceled=false）
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_tick 入参防御 + on_channel_lost 空实现（L228-229、L511-513）
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
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
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
	boot(unit, ctx, 100, mkCfg());                     // boot 期间 undone=0 -> 走 else 分支
	ctx.undone = -3.0;                                 // 场内 3 手卖单，目标是买
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
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
	boot(unit, ctx, 100, mkCfg());
	ctx.undone = 3.0;
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
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
	EXPECT_TRUE(ctx.hasLog("没有最新的tick数据"));
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// calTmStamp 的收盘后上界：时间戳位数不足/超过 15:00 都夹到 240（L79-81）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, timestamp_after_close_clamps_to_last_bucket)
{
	VwapFile vf(12.0);                                 // 每格都是 12
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	unit.on_tick(mkTick(kCode, 93000000, 10.50, 10.49, 10.51));
	// "93000000" -> hour=93 -> 全部 if 落空 -> total_minute=240 -> VwapAim[240]=12
	// _Vwap_vol = 12-0 = 12 -> curQty = max(12,1)*1 = 12
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 12.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// calTmSecs 的跨段计算通过 begin/end 配置走进 init 日志（L123、L56-60）
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, init_logs_total_seconds_from_begin_end_window)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	unit.init(&ctx, kCode, mkCfg(5, 0, 0, 1.0, 10.0, 0, 0, 930, 1030));
	unit.on_channel_ready();
	// calTmSecs(930,1030) = (10*3600+30*60) - (9*3600+30*60) = 3600
	EXPECT_TRUE(ctx.hasLog("执行时限 3600 秒"));
	unit.init(&ctx, kCode, mkCfg(5, 0, 0, 1.0, 10.0, 0, 0, 1330, 1500));
	// calTmSecs(1330,1500) = 54000 - (13*3600+30*60) = 5400-... = 5400
	EXPECT_TRUE(ctx.hasLog("执行时限 5400 秒"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 缺陷复现（SIGFPE，不能启用）：total_times=0 时 L120 整数除零
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, DISABLED_init_zero_total_times_divide_by_zero)
{
	VwapFile vf(25.0);
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	unit.init(&ctx, kCode, mkCfg(0, 0, 0, 1.0, 10.0, 0, 60));   // 期望 SIGFPE（BUG-R7-002）
	unit.on_channel_ready();
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 缺陷复现（堆外读，不能启用）：分布文件格子数 < 241 时 L372 下标越界
//////////////////////////////////////////////////////////////////////////
TEST(r7_vwap, DISABLED_short_distribution_file_out_of_range_read)
{
	VwapFile vf(25.0, 10);                            // 只有 10 格，但下标取到 30
	FakeCtx ctx;
	ctx.comm = mkComm();
	WtVWapExeUnit unit;
	boot(unit, ctx, 100, mkCfg());
	unit.on_tick(mkTick(kCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.buys.empty());                   // 期望：越界读/崩（BUG-R7-003）
	ctx.comm->release();
}
