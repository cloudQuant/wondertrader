// R24 lane（第二批）：WtStockVWapExeUnit / WtStockMinImpactExeUnit 的残余分支
// 目标文件: src/WtExeFact/WtStockVWapExeUnit.cpp（本目标基线残余 missed=109/482）
//           src/WtExeFact/WtStockMinImpactExeUnit.cpp（残余 missed=63/437）
//
// 与 R7 同名文件的分工（先普查过 r7_stock_vwap_unit_test.cpp / r7_stock_minimpact_unit_test.cpp
// 的用例头与 manifest R7_exefact.md）：R7 已覆盖 init 的科创板/可转债钳位、碎股、价格上下限、
// on_channel_ready 三分支、on_order/on_entrust/on_trade/clear_all_position 的主链。
// 本轮只打它们的残余（行号取自 llvm-cov show 对本目标实跑的 missed 清单）：
//   StockVWap: L107-108/115-116/128-129(分桶映射) L175 L229-232(缺分布文件) L271-274 L300-302
//              L382 L396 L426-436(超时扫描) L487 L505-508 L515 L522-526 L532-546 L624-634
//              L684 L703-746(fire_at_once 卖侧/涨跌停) L795-797
//   StockMinImpact: L95 L108-111 L119 L303-304 L329-347 L351-366 L402-404 L449 L459-462
//                   L532-535 L547-551 L625-627 L638
//
// 本轮的关键夹具纠正（R7 的 10 条 r7_stkvw / 8 条 r7_stkmin 用例就是因此红着）：
//  1) `WTSCommodityInfo::create` 的签名是 (pid, name, exchg, session, trdtpl, currency)
//     （src/Includes/WTSContractInfo.hpp:22-34，第 1 个参数才写进 m_strProduct）。
//     get_minOrderQty 用 strcmp(getProduct(), "STK"/"CBOND") 判品种
//     （WtStockVWapExeUnit.h:375-378 / WtStockMinImpactExeUnit.h:336-338），
//     把 "STK" 放到第 5 个参数（trdtpl）会让 min_order 恒为 1，整条取整链失真。
//  2) getPosition 的真实语义按形参名 `bValidOnly`：true=可用（昨仓），false=总数
//     （Includes/ExecuteDefs.h 的 ExecuteContext::getPosition）。
//     本文件按真实语义建替身，不复刻 R7 那份反向映射。
//  3) 标准化代码三段式 SHSE.STK.600000：init L201 / get_minOrderQty 都取 split('.')[2]，
//     两段串越界（BUG-R7-009），且 CodeHelper 会把两段串整段当 product。
//
// 超时扫描的确定性输入同 r24_vwap_twap_gap_test.cpp 顶部说明：
// WtOrdMon::check_orders 的 `expiresecs*1000` 在 uint32 里回绕（BUG-R24-003）→ 立刻判超时。
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <unistd.h>
#include <vector>

#include "Includes/ExecuteDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "WtExeFact/WtStockVWapExeUnit.h"
#include "WtExeFact/WtStockMinImpactExeUnit.h"

using namespace wtp;

namespace
{
const uint32_t kStickyOverflow = 536870912u;   // *1000 在 uint32 里回绕成 0
const char* const kStk = "SHSE.STK.600000";    // 普通股：<688000 -> min_hands 100
const char* const kKcb = "SHSE.STK.688001";    // 科创板：>=688000 -> min_hands 200

class SFakeCtx : public ExecuteContext
{
public:
	struct Rec { std::string code; double price; double qty; };

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return NULL; }
	// 替身按真实语义给行情：grabLastTick 返回 lastTickOwned 的一次 retain 副本（用例负责释放）
	WTSTickData* grabLastTick(const char*) override
	{
		if (!grabReturnsTick || lastTick == NULL)
			return NULL;
		lastTick->retain();
		grabHits++;
		return lastTick;
	}
	// ExecuteContext::getPosition(stdCode, bValidOnly=true, flag=3)
	// bValidOnly=true -> 可用（昨仓）；false -> 总仓；flag==1 -> 今仓多头
	double getPosition(const char*, bool bValidOnly = true, int32_t flag = 3) override
	{
		if (flag == 1)
			return today;
		return bValidOnly ? valid : total;
	}
	OrderMap* getOrders(const char*) override { return NULL; }
	double getUndoneQty(const char*) override { return undone; }

	OrderIDs buy(const char* c, double p, double q, bool) override
	{
		buys.push_back({ c, p, q });
		if (onAfterBuy)
			onAfterBuy();
		OrderIDs ids{ nextId++ };
		return ids;
	}
	OrderIDs sell(const char* c, double p, double q, bool) override
	{
		sells.push_back({ c, p, q });
		OrderIDs ids{ nextId++ };
		return ids;
	}
	bool cancel(uint32_t id) override { byidCancels.push_back(id); return true; }
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
	uint64_t getCurTime() override { return 20240103093000000ull; }

	double total = 0;
	double valid = 0;
	double today = 0;
	double undone = 0;
	uint32_t nextId = 7000;
	uint32_t cancelBydirRetCnt = 2;
	uint32_t grabHits = 0;
	bool grabReturnsTick = true;
	WTSCommodityInfo* comm = NULL;
	WTSSessionInfo* sess = NULL;
	WTSTickData* lastTick = NULL;         // 用例持有；grabLastTick 自己 retain 一份
	std::function<void()> onAfterBuy;
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

inline std::string sName(const char* prefix)
{
	return std::string(prefix) + std::to_string((long long)getpid());
}

// "Vwap_" + comm->getName() + ".txt"（StockVWap.cpp:225-227），按行号给不同分桶值
class SVwapFile
{
public:
	SVwapFile(const std::string& commName, const std::vector<double>& buckets)
	{
		_path = std::string("Vwap_") + commName + ".txt";
		std::ofstream f(_path.c_str());
		for (size_t i = 0; i < buckets.size(); i++)
		{
			if (i)
				f << ",";
			f << buckets[i];
		}
		f.close();
	}
	~SVwapFile() { std::remove(_path.c_str()); }

private:
	std::string _path;
};

static inline std::vector<double> flatBuckets(double v, uint32_t rows = 241)
{
	return std::vector<double>(rows, v);
}

// 第 1 个参数才是 product（WTSContractInfo.hpp:27），所以 "STK"/"CBOND" 必须写在第 1 位。
// 额外 retain()：get_minOrderQty 会对借来的指针多做一次 release（StockVWap.h:386-388），
// 抵消掉它，否则单元析构时引用计数净少 1。
WTSCommodityInfo* mkSComm(const char* product, TradingMode tm = TM_Long)
{
	WTSCommodityInfo* c = WTSCommodityInfo::create(product, sName("R24S").c_str(), "SHSE", "R24DAY", "CHN");
	c->retain();
	c->setVolScale(1);
	c->setPriceTick(0.01);
	c->setLotsTick(1.0);
	c->setMinLots(1.0);
	c->setCategory(CC_Stock);
	c->setCoverMode(CM_OpenCover);
	c->setPriceMode(PM_Both);
	c->setTradingMode(tm);
	return c;
}

WTSSessionInfo* mkSSess()
{
	WTSSessionInfo* s = WTSSessionInfo::create("R24DAY", "R24日盘", 0);
	s->addTradingSection(930, 1130);
	s->addTradingSection(1300, 1500);
	return s;   // isInTradingTime(900)=false、(1030)=true、(1200)=false
}

WTSTickData* mkSTick(const char* code, uint32_t actTime, double lastPx,
	double bidPx, double askPx, double upper = 0, double lower = 0)
{
	WTSTickStruct ts;
	snprintf(ts.code, sizeof(ts.code), "%s", code);
	ts.action_date = 20240103;
	ts.trading_date = 20240103;
	ts.action_time = actTime;
	ts.price = lastPx;
	ts.pre_close = 10.0;
	ts.bid_prices[0] = bidPx;
	ts.bid_qty[0] = 1000;
	ts.ask_prices[0] = askPx;
	ts.ask_qty[0] = 900;
	ts.upper_limit = upper;
	ts.lower_limit = lower;
	return WTSTickData::create(ts);
}

// StockVWap init L178-189 实际读的键（价格偏移的键名是 "offset"）
WTSVariant* mkSCfg(uint32_t totalTimes = 5, uint32_t priceMode = 0, uint32_t offset = 0,
	double minOpenLots = 100.0, uint32_t ordSticky = 0,
	uint32_t beginTime = 930, uint32_t endTime = 1500)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("begin_time", beginTime);
	cfg->append("end_time", endTime);
	cfg->append("ord_sticky", ordSticky);
	cfg->append("tail_secs", (uint32_t)0);
	cfg->append("total_times", totalTimes);
	cfg->append("price_mode", priceMode);
	cfg->append("offset", offset);
	cfg->append("lots", (double)1000);
	cfg->append("minopenlots", minOpenLots);
	return cfg;
}

// StockMinImpact init L98-119 实际读的键
WTSVariant* mkMinCfg(int32_t priceMode = 0, uint32_t expire = 0, uint32_t span = 0,
	double lots = 100.0, double minOrder = 100.0, bool byRate = false, double rate = 0.0,
	bool withTotalMoney = false, int32_t maxCancelTime = -1)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("pricemode", priceMode);
	cfg->append("offset", (int32_t)0);
	cfg->append("expire", expire);
	cfg->append("span", span);
	cfg->append("byrate", byRate);
	cfg->append("lots", lots);
	cfg->append("rate", rate);
	cfg->append("min_order", minOrder);
	if (withTotalMoney)
		cfg->append("total_money", 500000.0);
	if (maxCancelTime >= 0)
		cfg->append("max_cancel_time", maxCancelTime);
	return cfg;
}

// StockVWap 的安全驱动顺序（BUG-R7-005：do_calc L517-518 在 L547 的空检查之前解引用 _last_tick）
void bootS(WtStockVWapExeUnit& unit, SFakeCtx& ctx, const char* code, double target, WTSVariant* cfg,
	uint32_t actTime = 100000000, double lastPx = 10.50, double bidPx = 10.49, double askPx = 10.51)
{
	unit.init(&ctx, code, cfg);
	cfg->release();
	unit.set_position(code, target);
	unit.on_tick(mkSTick(code, actTime, lastPx, bidPx, askPx));
	unit.on_channel_ready();
}
}

//////////////////////////////////////////////////////////////////////////
// StockVWap：分布文件缺失 -> L229-232 记日志并早退（此时 VwapAim 为空，
// 后续任何发单路径都不该再碰 VwapAim，也不该有单）
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkvw, init_without_distribution_file_returns_before_calc)
{
	std::string name = sName("R24SNONE");
	std::string missing = std::string("Vwap_") + name + ".txt";
	std::remove(missing.c_str());
	SFakeCtx ctx;
	ctx.comm = WTSCommodityInfo::create("STK", name.c_str(), "SHSE", "R24DAY", "CHN");
	ctx.comm->retain();
	ctx.comm->setPriceTick(0.01);
	ctx.comm->setVolScale(1);
	ctx.comm->setCategory(CC_Stock);
	ctx.comm->setTradingMode(TM_Long);
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg());
	ctx.comm->release();                        // 抵掉 init L170 的 retain
	unit.set_position(kStk, 500);
	unit.on_channel_ready();
	// L230 原文
	EXPECT_EQ(ctx.countLog("not exists ,check and return"), 1u);
	EXPECT_TRUE(ctx.hasLog("初始化完成"));        // L195 早退前已打
	ctx.comm->retain();
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// StockVWap：init L201-218 的科创板钳位（max(cfg, min_hands=200)）与 L221-222 的 T+0 判定
// （可转债 min_hands=10 那条 R7 已有用例，本轮不重复投）
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkvw, kcb_clamps_min_open_lots_up_and_t0_mode_widens_sellable)
{
	// 科创板：cfg 给 100，_min_hands=200 -> L213 max -> _min_open_lots=200
	const std::string name = sName("R24SKC");
	SVwapFile vf(name, flatBuckets(30.0));
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK", TM_Long);
	ctx.total = 500;
	ctx.valid = 100;                             // T+0 被 TM_Long 打开（L221-222）-> vailyPos 用总仓
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kKcb, mkSCfg(5, 0, 0, 100.0));
	unit.set_position(kKcb, 200);
	unit.on_tick(mkSTick(kKcb, 100000000, 10.50, 10.49, 10.51));
	unit.on_channel_ready();
	// 目标 200 / 总仓 500 -> 走卖向；L609 的量恒等于 min(vailyPos, _min_open_lots)
	// _is_t0 为真时 vailyPos=realPos=500（L501-502）-> min(500, 200)=200，即钳位后的 200
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 200.0);
	// 对照：普通股同 cfg 只能到 100（L216 min(cfg, hands=100)）
	const std::string name2 = sName("R24SNRK");
	SVwapFile vf2(name2, flatBuckets(30.0));
	SFakeCtx ctx2;
	ctx2.comm = mkSComm("STK", TM_Long);
	ctx2.total = 500;
	ctx2.valid = 100;
	WtStockVWapExeUnit unit2;
	unit2.init(&ctx2, kStk, mkSCfg(5, 0, 0, 100.0));
	unit2.set_position(kStk, 200);
	unit2.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit2.on_channel_ready();
	ASSERT_EQ(ctx2.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx2.buys[0].qty, 100.0);
	ctx.comm->release();
	ctx2.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// StockVWap：分桶映射的三段边界（L106-108 开盘前 / L110-112 上午 / L114-116+L127-129 午休）
// 断言的不是分桶量（L609 无条件覆盖，BUG-R7-007），而是「每一笔新 tick 都恰好下一单、
// 量=min(可用=min(total,100))」——分桶值在这里被产品丢弃。
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkvw, tick_buckets_across_morning_and_lunch_all_fire_same_qty)
{
	const std::string name = sName("R24SBK");
	// 分桶值刻意各不相同：0 格 5、60 格 50、120 格 150
	std::vector<double> buckets(241, 0.0);
	buckets[0] = 5.0;
	buckets[60] = 50.0;
	buckets[120] = 150.0;
	SVwapFile vf(name, buckets);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.total = 500;
	ctx.valid = 500;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 200, mkSCfg(5, 0, 0, 100.0), 90000000);   // 0900 -> L107-108 桶 0
	unit.on_tick(mkSTick(kStk, 90000000, 10.50, 10.49, 10.51));
	unit.on_tick(mkSTick(kStk, 103000000, 10.50, 10.49, 10.51));     // 1030 -> L110-112 桶 60
	unit.on_tick(mkSTick(kStk, 120000000, 10.50, 10.49, 10.51));     // 1200 -> L114-116 后 L127-129 强制 120
	// 三笔都是 min(total=500, minopenlots=100) = 100，与分桶值 5/50/150 无关
	ASSERT_EQ(ctx.buys.size(), 3u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 100.0);
	EXPECT_DOUBLE_EQ(ctx.buys[2].qty, 100.0);
	EXPECT_EQ(ctx.countLog("第 1 次发单"), 1u);
	EXPECT_EQ(ctx.countLog("第 2 次发单"), 1u);
	EXPECT_EQ(ctx.countLog("第 3 次发单"), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// StockVWap：首笔 tick 不在交易时段被丢（L395-396），代码不匹配/NULL 被丢（L381-382）
// 以及 init 的 _sess_info retain（L173-175）
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkvw, session_filter_drops_pre_open_and_foreign_ticks)
{
	const std::string name = sName("R24SSESS");
	SVwapFile vf(name, flatBuckets(30.0));
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.sess = mkSSess();
	ctx.total = 500;
	ctx.valid = 500;
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg());
	unit.set_position(kStk, 200);
	unit.on_channel_ready();                     // 此时还没有 tick：BUG-R7-005 的解引用在 L517
	EXPECT_TRUE(ctx.buys.empty());

	unit.on_tick(NULL);                          // L381-382
	WTSTickData* foreign = mkSTick("SHSE.STK.600999", 100000000, 10.50, 10.49, 10.51);
	unit.on_tick(foreign);
	foreign->release();                          // 代码不匹配 -> 单元没接手
	WTSTickData* pre = mkSTick(kStk, 90000000, 10.50, 10.49, 10.51);
	unit.on_tick(pre);                           // 900 不在 930-1130 -> L395-396 丢弃
	pre->release();
	EXPECT_TRUE(ctx.buys.empty());

	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));   // 1000 在时段内
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);
	ctx.comm->release();
	ctx.sess->release();
}

//////////////////////////////////////////////////////////////////////////
// StockVWap：可卖不足的目标修正（L503-508）+ diffQty 为 0 的早退（L515-516）
// 替身按真实语义给数：realPos=getPosition(code) 是「可用」，
// vailyPos=getPosition(code,false) 是「总数」。
// 可用 1000 > 总数 500 是不一致的柜台数据，正是 L503 那句 max(realPos-vailyPos, target) 的入口
// （真实柜台里可用不会大于总数，所以这条校验几乎恒不触发，见 bugs/R24_exefact.md BUG-R24-004）
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkvw, holdpos_adjust_and_zero_diff_calc_paths)
{
	const std::string name = sName("R24SADJ");
	SVwapFile vf(name, flatBuckets(30.0));
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK", TM_Both);          // 非 TM_Long -> _is_t0 保持 false，L501-502 不覆盖
	ctx.total = 500;
	ctx.valid = 1000;                            // 可用 > 总数（刻意不一致的柜台数据）
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg(5, 0, 0, 100.0));
	unit.set_position(kStk, 300);
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit.on_channel_ready();
	// max(realPos-vailyPos, _target_pos) = max(1000-500, 300) = 500 != 300 -> L505-508
	EXPECT_EQ(ctx.countLog("can sell hold pos not enough"), 1u);
	EXPECT_TRUE(ctx.hasLog("target adjust 300->500"));
	// 修正后 diffQty = 500-1000 = -500，取整判定 L521 两边都是 round_hands(500/1000,100) 不等 -> 继续
	// L609 无条件 min(vailyPos=500, minopenlots=100) = 100 -> curQty>0 -> 走 buy（BUG-R7-008）
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);

	// 第二条路径：目标 == 可用仓位 -> diffQty==0 -> L515-516 静默早退（不打 set finish）
	const std::string name2 = sName("R24SZERO");
	SVwapFile vf2(name2, flatBuckets(30.0));
	SFakeCtx ctx2;
	ctx2.comm = mkSComm("STK", TM_Both);
	ctx2.total = 400;
	ctx2.valid = 400;
	WtStockVWapExeUnit unit2;
	unit2.init(&ctx2, kStk, mkSCfg(5, 0, 0, 100.0));
	unit2.set_position(kStk, 400);              // 可用==目标 -> diffQty = 0
	unit2.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit2.on_channel_ready();
	EXPECT_TRUE(ctx2.buys.empty());
	EXPECT_FALSE(ctx2.hasLog("set finish"));     // 与 L521-525 的取整完成路径区分
	EXPECT_FALSE(ctx2.hasLog("can sell hold pos"));
	ctx.comm->release();
	ctx2.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// StockVWap：碎股取整命中 -> 标记完成并停止（L521-526），此后任何新 tick 都不再发单
// target=1003、可用=1000：round_hands(1003,100)=(int)(1053/100)*100=1000 = round_hands(1000,100)
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkvw, rounding_match_marks_finish_and_blocks_later_ticks)
{
	const std::string name = sName("R24SFIN");
	SVwapFile vf(name, flatBuckets(30.0));
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK", TM_Both);
	ctx.total = 1000;
	ctx.valid = 1000;
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg(5, 0, 0, 100.0));
	unit.set_position(kStk, 1003);
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit.on_channel_ready();                    // do_calc -> L522-526
	EXPECT_EQ(ctx.countLog("set finish"), 1u);
	EXPECT_TRUE(ctx.buys.empty());

	unit.on_tick(mkSTick(kStk, 103000000, 10.60, 10.59, 10.61));   // _is_finish -> L499-500
	unit.on_tick(mkSTick(kStk, 110000000, 10.70, 10.69, 10.71));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_EQ(ctx.countLog("set finish"), 1u);   // 没有重复标记
	// 换目标会重置 _is_finish（set_position L787），完成态不是一去不回
	unit.set_position(kStk, 1300);
	unit.on_tick(mkSTick(kStk, 113000000, 10.80, 10.79, 10.81));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// StockVWap：反向未完成单先撤（L531-541）、同向未完成单本轮放弃（L542-546）
// 注意 L530 的 isBuy 取自 undone 而不是 diffQty
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkvw, opposite_undone_canceled_and_same_direction_blocks_round)
{
	const std::string name = sName("R24SOPP");
	SVwapFile vf(name, flatBuckets(30.0));
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK", TM_Both);
	ctx.total = 0;
	ctx.valid = 0;
	ctx.undone = -3.0;                            // 场内 3 手卖单
	ctx.cancelBydirRetCnt = 2;
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg(5, 0, 0, 100.0));
	unit.set_position(kStk, 500);                 // diffQty = +500 -> 与 undone 反号
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit.on_channel_ready();
	// on_channel_ready 先撤了外部单（L322-346），undone 仍是 -3 -> do_calc 进 L531
	ASSERT_GE(ctx.bydirCancels.size(), 1u);
	EXPECT_FALSE(ctx.bydirCancels[ctx.bydirCancels.size() - 1].second);   // isBuy=(undone>0)=false
	EXPECT_TRUE(ctx.hasLog("live opposite order of"));                    // L538
	EXPECT_TRUE(ctx.buys.empty());

	// 同向：undone=+3、diffQty=+500 -> 531 不成立 -> L542-546 本轮放弃
	const std::string name2 = sName("R24SSAME");
	SVwapFile vf2(name2, flatBuckets(30.0));
	SFakeCtx ctx2;
	ctx2.comm = mkSComm("STK", TM_Both);
	ctx2.undone = 3.0;
	WtStockVWapExeUnit unit2;
	unit2.init(&ctx2, kStk, mkSCfg(5, 0, 0, 100.0));
	unit2.set_position(kStk, 500);
	unit2.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit2.on_channel_ready();                    // 撤外部单 -> 之后 undone 仍 3
	EXPECT_TRUE(ctx2.hasLog("上一轮有挂单未完成"));   // L544
	EXPECT_TRUE(ctx2.buys.empty());
	ctx.comm->release();
	ctx2.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// StockVWap：超时撤单扫描（L425-436）+ 撤单回报把 _cancel_cnt 递减（L270-274）
// + 未撤完就来的非撤单回报走错误返回（L299-302）
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkvw, expired_scan_then_cancel_reports_drain_cancel_cnt)
{
	const std::string name = sName("R24SEXP");
	SVwapFile vf(name, flatBuckets(30.0));
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK", TM_Both);
	ctx.total = 500;
	ctx.valid = 500;
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg(5, 0, 0, 100.0, kStickyOverflow));
	unit.set_position(kStk, 200);
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit.on_channel_ready();
	ASSERT_EQ(ctx.buys.size(), 1u);               // 本地单 7000

	unit.on_tick(mkSTick(kStk, 103000000, 10.60, 10.59, 10.61));   // 非首笔 -> L425
	ASSERT_EQ(ctx.byidCancels.size(), 1u);
	EXPECT_EQ(ctx.byidCancels[0], 7000u);
	EXPECT_EQ(ctx.countLog("Order expired, cancelcnt updated to 1"), 1u);   // L431-432
	EXPECT_EQ(ctx.buys.size(), 1u);               // hasCancel -> L439 不进 do_calc

	// _cancel_cnt==1 时先来一条「未撤销」的回报 -> L299-302 错误返回
	unit.on_order(7000, kStk, true, 100, 10.50, false);
	EXPECT_EQ(ctx.countLog("hasn't canceled, error will be return"), 1u);
	EXPECT_EQ(ctx.buys.size(), 1u);

	// 撤单回报 -> L265-274 摘单并把 _cancel_cnt 降到 0
	unit.on_order(7000, kStk, true, 100, 10.50, true);
	EXPECT_EQ(ctx.countLog("cancelling done"), 1u);
	unit.on_order(7000, kStk, true, 100, 10.50, true);            // 已摘除 -> L261-262
	EXPECT_EQ(ctx.countLog("cancelling done"), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// StockVWap：do_calc 的价格模式（L624-627 模式1 / L628-631 模式2 / L632-634 打板 +5 跳）
// 以及末桶打板（L585-589）
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkvw, price_modes_and_last_bucket_showhand)
{
	const std::string name = sName("R24SPM");
	SVwapFile vf(name, flatBuckets(30.0));
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK", TM_Both);
	ctx.total = 500;
	ctx.valid = 500;
	WtStockVWapExeUnit unit;
	// price_mode=1：L626 `isBuy ? bid : ask`，isBuy 来自 diffQty（此处 200-500<0）-> ask 10.51
	bootS(unit, ctx, kStk, 200, mkSCfg(1, 1, 0, 100.0));          // total_times=1
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.51);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);

	// 第二次：leftTimes = 1-1 = 0 且 diffQty!=0 -> L585-589 打板 + L633 再加 5 跳
	unit.on_tick(mkSTick(kStk, 103000000, 10.60, 10.59, 10.61));
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.66);   // 10.60 + 0.01*5（isBuy=false 走 L633 的 -1? 见下）
	ctx.comm->release();

	// price_mode=2：L630 `isBuy ? ask : bid`，diffQty<0 -> bid 10.49；offset=3 -> L637 再减 3 跳
	const std::string name2 = sName("R24SPM2");
	SVwapFile vf2(name2, flatBuckets(30.0));
	SFakeCtx ctx2;
	ctx2.comm = mkSComm("STK", TM_Both);
	ctx2.total = 500;
	ctx2.valid = 500;
	WtStockVWapExeUnit unit2;
	bootS(unit2, ctx2, kStk, 200, mkSCfg(5, 2, 3, 100.0));
	ASSERT_EQ(ctx2.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx2.buys[0].price, 10.46);  // 10.49 - 0.01*3
	ctx2.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// StockVWap：fire_at_once 的 0 量早退（L683-684）与卖出侧（L703-711 价、L729-735 跌停、L746 卖）
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkvw, fire_at_once_zero_qty_and_sell_paths)
{
	// 第一轮：minopenlots=0 -> 重发量 max(0, 100-400)=0 -> L683-684 直接返回
	const std::string name = sName("R24SF0");
	SVwapFile vf(name, flatBuckets(30.0));
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK", TM_Both);
	ctx.total = 500;
	ctx.valid = 500;
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg(5, 0, 0, 0.0));
	unit.set_position(kStk, 200);
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit.on_channel_ready();
	ASSERT_EQ(ctx.buys.size(), 1u);                // L609 min(500,0)=0 -> curQty=0 -> L664 sell(0)
	// curQty==0 -> L659 不成立 -> sell(0)，本轮目标 _this_target = 500+0
	unit.on_order(ctx.buys.size() ? 7000 : 7000, kStk, false, 5, 10.50, true);
	EXPECT_TRUE(ctx.hasLog("re_fire will be done"));   // L291
	EXPECT_EQ(ctx.buys.size(), 1u);                // L683-684：qty=0 不再加仓
	ctx.comm->release();

	// 第二轮：minopenlots=-200 -> 重发量 max(-200, 100-500) = -200 -> 卖 200，跌停收敛
	const std::string name2 = sName("R24SSELL");
	SVwapFile vf2(name2, flatBuckets(30.0));
	SFakeCtx ctx2;
	ctx2.comm = mkSComm("STK", TM_Both);
	ctx2.total = 500;
	ctx2.valid = 500;
	WtStockVWapExeUnit unit2;
	unit2.init(&ctx2, kStk, mkSCfg(5, 1, 0, -200.0));
	unit2.set_position(kStk, 100);
	unit2.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51, 0, 10.50));
	unit2.on_channel_ready();
	EXPECT_GE(ctx2.buys.size(), 1u);
	ctx2.total = 500;
	ctx2.valid = 500;
	unit2.on_order(7000, kStk, false, 5, 10.50, true);
	// fire_at_once(-200)：mode1 -> L705 卖向取 bidprice(0)=10.49，L714 再减 0.01*_cancel_times(1)
	// = 10.48 < 跌停 10.50 -> L729-735 收敛，L746 sell(abs(-200))
	ASSERT_EQ(ctx2.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx2.sells[0].qty, 200.0);
	EXPECT_DOUBLE_EQ(ctx2.sells[0].price, 10.50);
	EXPECT_EQ(ctx2.countLog("modified to lower limit price"), 1u);
	ctx2.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// StockVWap：set_position 用 grabLastTick 记开始价（L793-797）
// 之后 do_calc 的「补一次」（L517-518）把开始价刷新成最新一笔 tick 的价格
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkvw, set_position_grabs_start_price)
{
	const std::string name = sName("R24SSP");
	SVwapFile vf(name, flatBuckets(30.0));
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK", TM_Both);
	ctx.total = 500;
	ctx.valid = 500;
	WTSTickData* grabbed = mkSTick(kStk, 93000000, 9.99, 9.98, 10.00);
	ctx.lastTick = grabbed;
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg(5, 0, 0, 100.0));
	unit.set_position(kStk, 200);                 // L793-797：_start_price = 9.99
	EXPECT_EQ(ctx.grabHits, 1u);
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit.on_channel_ready();                      // do_calc L517-518 再补一次
	EXPECT_EQ(ctx.buys.size(), 1u);

	// grabLastTick 返回 NULL 时不覆盖开始价，也不崩（L794 的 if 假分支）
	const std::string name2 = sName("R24SSP2");
	SVwapFile vf2(name2, flatBuckets(30.0));
	SFakeCtx ctx2;
	ctx2.comm = mkSComm("STK", TM_Both);
	ctx2.grabReturnsTick = false;
	ctx2.total = 500;
	ctx2.valid = 500;
	WtStockVWapExeUnit unit2;
	unit2.init(&ctx2, kStk, mkSCfg(5, 0, 0, 100.0));
	unit2.set_position(kStk, 200);
	EXPECT_EQ(ctx2.grabHits, 0u);
	unit2.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit2.on_channel_ready();
	EXPECT_EQ(ctx2.buys.size(), 1u);
	grabbed->release();
	ctx.comm->release();
	ctx2.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// StockMinImpact：init 的 total_money（L107-111）、max_cancel_time（L118-119）
// 与 sess retain + 首笔非交易时段过滤（L93-95、L302-304）
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkmin, init_optional_keys_and_pre_open_first_tick_dropped)
{
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK", TM_Both);
	ctx.sess = mkSSess();
	WtStockMinImpactExeUnit unit;
	WTSVariant* cfg = mkMinCfg(0, kStickyOverflow, 0, 100.0, 100.0, false, 0.0, true, 0);
	unit.init(&ctx, kStk, cfg);
	cfg->release();
	// L154 的初始化日志把 min_order 打出来：产品钳位 min(cfg=100, hands=100)=100
	EXPECT_TRUE(ctx.hasLog("min_order: 100.00"));
	unit.on_channel_ready();

	WTSTickData* pre = mkSTick(kStk, 90000000, 10.50, 10.49, 10.51);
	unit.on_tick(pre);                            // 900 不在时段 -> L303-304
	pre->release();
	EXPECT_TRUE(ctx.buys.empty());
	unit.set_position(kStk, 500);                 // L457-462 抓开始价（此时还没有 tick）
	EXPECT_GE(ctx.grabHits, 1u);
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);     // min(lots=100, diff=500) 且 round_hands(100,100)
	ctx.comm->release();
	ctx.sess->release();
}

//////////////////////////////////////////////////////////////////////////
// StockMinImpact：超时扫描的完整闭环
// L327-330 枚举日志、L333-347 撤单 + _cancel_map 记账、L350-366 撤单次数超限强制摘单
// 强制摘单的证据是「下一笔 tick 的枚举日志里再也看不到 7000」
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkmin, expire_scan_cancels_counts_and_force_erases)
{
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK", TM_Both);
	ctx.total = 500;
	ctx.valid = 500;
	WtStockMinImpactExeUnit unit;
	WTSVariant* cfg = mkMinCfg(0, kStickyOverflow, 0, 100.0, 100.0, false, 0.0, false, 0);
	unit.init(&ctx, kStk, cfg);
	cfg->release();
	unit.set_position(kStk, 500);
	unit.on_channel_ready();
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);               // 7000 进监控

	unit.on_tick(mkSTick(kStk, 100100000, 10.60, 10.59, 10.61));
	// 枚举日志（L329）
	EXPECT_EQ(ctx.countLog("7000 entertime:"), 1u);
	ASSERT_EQ(ctx.byidCancels.size(), 1u);
	EXPECT_EQ(ctx.byidCancels[0], 7000u);
	EXPECT_EQ(ctx.countLog("Expired order of"), 1u);        // L338
	EXPECT_TRUE(ctx.hasLog("canceled by 0 times,erase forcely"));  // L364
	// 强制摘单 + 本轮重发一笔
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 100.0);

	unit.on_tick(mkSTick(kStk, 100200000, 10.70, 10.69, 10.71));
	// 7000 已被摘掉，枚举里只剩 7001
	EXPECT_EQ(ctx.countLog("7000 entertime:"), 1u);
	EXPECT_EQ(ctx.countLog("7001 entertime:"), 1u);
	EXPECT_EQ(ctx.byidCancels.size(), 2u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// StockMinImpact：max_cancel_time=1 时，撤一次不强制摘（1>1 假），撤两次才摘（2>1 真）
// 与上一条只差这个阈值 -> 阈值那行（L119、L355）是真的可证伪输入
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkmin, max_cancel_time_one_keeps_order_until_second_cancel)
{
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK", TM_Both);
	ctx.total = 500;
	ctx.valid = 500;
	WtStockMinImpactExeUnit unit;
	WTSVariant* cfg = mkMinCfg(0, kStickyOverflow, 0, 100.0, 100.0, false, 0.0, false, 1);
	unit.init(&ctx, kStk, cfg);
	cfg->release();
	unit.set_position(kStk, 500);
	unit.on_channel_ready();
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit.on_tick(mkSTick(kStk, 100100000, 10.60, 10.59, 10.61));   // 第 1 次撤单
	EXPECT_FALSE(ctx.hasLog("erase forcely"));                      // 1 > 1 不成立
	unit.on_tick(mkSTick(kStk, 100200000, 10.70, 10.69, 10.71));   // 第 2 次撤单
	EXPECT_EQ(ctx.countLog("canceled by 1 times,erase forcely"), 1u);
	EXPECT_EQ(ctx.byidCancels.size(), 2u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// StockMinImpact：委托失败回报 -> 摘单并记日志（L399-404），随后重算
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkmin, entrust_fail_erases_monitored_order_then_recalc)
{
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK", TM_Both);
	ctx.total = 500;
	ctx.valid = 500;
	WtStockMinImpactExeUnit unit;
	WTSVariant* cfg = mkMinCfg(0, 0, 0, 100.0, 100.0);
	unit.init(&ctx, kStk, cfg);
	cfg->release();
	unit.set_position(kStk, 500);
	unit.on_channel_ready();
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);

	unit.on_entrust(7000, kStk, false, "reject");      // L399-404 + L406 重算
	EXPECT_EQ(ctx.countLog("entrust failed erase from mon"), 1u);
	// 重算受「同一笔 tick 不重复下单」闸门约束？本单元没有该闸门，只有 span（此处 0）-> 再发一笔
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 100.0);
	unit.on_entrust(7000, kStk, false, "again");       // 已摘除 -> L399-400
	EXPECT_EQ(ctx.countLog("entrust failed erase from mon"), 1u);
	unit.on_entrust(9999, kStk, false, "not mine");    // 别人的单 -> 同样落 L400
	EXPECT_EQ(ctx.countLog("entrust failed erase from mon"), 1u);
	unit.on_entrust(7001, kStk, true, "ok");           // 成功回报不进这条链（L395）
	EXPECT_EQ(ctx.buys.size(), 2u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// StockMinImpact：清仓标记后再设目标 -> L448-449 的清仓日志；
// 可卖不足（总数-可用 = 今仓 > 目标）-> L531-535 抬目标；取整命中 -> L546-551 完成
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkmin, clearing_log_and_holdpos_adjust_then_finish)
{
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK", TM_Both);
	WtStockMinImpactExeUnit unit;
	WTSVariant* cfg = mkMinCfg(0, 0, 0, 100.0, 100.0);
	unit.init(&ctx, kStk, cfg);
	cfg->release();
	unit.set_position(kStk, 500);
	unit.clear_all_position(kStk);                     // _is_clear = true
	unit.set_position(kStk, 300);                      // 清仓态下再设目标 -> L448-449
	EXPECT_EQ(ctx.countLog("is set to be in clearing processing"), 1u);

	// 换一笔干净的单元做「可卖不足 + 取整完成」的差分
	SFakeCtx ctx2;
	ctx2.comm = mkSComm("STK", TM_Both);
	ctx2.total = 1000;                                  // 总仓 1000
	ctx2.valid = 0;                                     // 可用 0（全是今天买的）
	WtStockMinImpactExeUnit unit2;
	WTSVariant* cfg2 = mkMinCfg(0, 0, 0, 100.0, 100.0);
	unit2.init(&ctx2, kStk, cfg2);
	cfg2->release();
	unit2.set_position(kStk, 500);                      // do_calc 在无 tick 时早退（L503）
	unit2.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit2.on_channel_ready();
	// target_pos = max(1000-0, 500) = 1000 != 500 -> L532-535
	EXPECT_EQ(ctx2.countLog("can sell hold pos not enough"), 1u);
	EXPECT_TRUE(ctx2.hasLog("target adjust 500->1000"));
	// round_hands(1000,100) == round_hands(1000,100) -> L546-551 完成
	EXPECT_EQ(ctx2.countLog("target position"), 1u);
	EXPECT_TRUE(ctx2.hasLog("set finish"));
	EXPECT_TRUE(ctx2.buys.empty());
	EXPECT_TRUE(ctx2.sells.empty());
	ctx.comm->release();
	ctx2.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// StockMinImpact：卖出侧的碎股与零量
// 可用 50 < min_order 100 -> L624-627 直接按可用量卖 50；
// 可用 0 -> this_qty=0 -> L637-638 不发单
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkmin, sell_odd_lot_uses_valid_position_and_zero_blocks_order)
{
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK", TM_Both);
	ctx.total = 50;
	ctx.valid = 50;
	WtStockMinImpactExeUnit unit;
	WTSVariant* cfg = mkMinCfg(0, 0, 0, 100.0, 100.0);
	unit.init(&ctx, kStk, cfg);
	cfg->release();
	unit.set_position(kStk, 0);                        // 目标 0 -> 卖向
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit.on_channel_ready();
	// curPos(总数)=50、vailyPos(可用)=50 -> target=max(50-50,0)=0 -> diffPos=-50
	// 卖向：vailyPos=50 < min_order=100 -> this_qty=50（L624-627）-> min(50,50)=50
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 50.0);
	EXPECT_DOUBLE_EQ(ctx.sells[0].price, 10.50);       // pricemode 0 -> LASTPX 最新价

	// 可用 0：目标 0、总仓 0 -> diffPos==0 -> 取整判定先把完成态打上（L546-551），
	// 所以另起一笔：目标 0、总仓 0、可用 0 且 _is_finish 未置位时才会走到 L637-638
	SFakeCtx ctx2;
	ctx2.comm = mkSComm("STK", TM_Both);
	ctx2.total = 150;
	ctx2.valid = 0;                                    // 可卖 0，全是今仓
	WtStockMinImpactExeUnit unit2;
	WTSVariant* cfg2 = mkMinCfg(0, 0, 0, 100.0, 100.0);
	unit2.init(&ctx2, kStk, cfg2);
	cfg2->release();
	unit2.set_position(kStk, 0);
	unit2.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit2.on_channel_ready();
	// target = max(150-0, 0) = 150 -> 目标被抬到 150 -> diffPos = 150-150 = 0
	// round_hands(150,100)=(int)(250/100)*100=200，round_hands(150,100)=200 -> 相等 -> 完成
	EXPECT_TRUE(ctx2.hasLog("set finish"));
	EXPECT_TRUE(ctx2.sells.empty());
	ctx.comm->release();
	ctx2.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 留白登记（不可达，不是本轮遗漏）：
//  - StockVWap L547-551「没有最新tick」：L517 `decimal::ge(_start_price,0)` 恒真
//    （_start_price 头文件默认 0，StockVWapExeUnit.h:281），先解引用 _last_tick ->
//    没有 tick 时 BUG-R7-005 的崩点在 L518，永远走不到 L547。
//  - StockVWap L595-598 买向取整：L530 的 isBuy 取自 undone，而 L542 已把 undone!=0 拦掉
//    （BUG-R7-008 的另一面）。
//  - StockVWap L555-568 / StockMinImpact 对应段：BUG-R7-004 的死清仓分支。
//  - StockMinImpact L580-584：L503-504 已先 !last_tick 早退。
//  - 两边的分桶量：L609（StockVWap）无条件覆盖 curQty（BUG-R7-007）。
//////////////////////////////////////////////////////////////////////////
TEST(r24_stkvw, DISABLED_afternoon_bucket_index_out_of_range_read)
{
	// calTmStamp 下午段 L119：240 + (hour-13)*60 + minute - 30
	// 14:00 -> 240+60-30 = 270 > 240 行 -> L578 VwapAim[270] 越界读（BUG-R7-003 的下午面）
	const std::string name = sName("R24SOOB");
	SVwapFile vf(name, flatBuckets(30.0));            // 241 行
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK", TM_Both);
	ctx.total = 500;
	ctx.valid = 500;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 200, mkSCfg(5, 0, 0, 100.0), 140000000);
	unit.on_tick(mkSTick(kStk, 140000000, 10.50, 10.49, 10.51));
	SUCCEED();
	ctx.comm->release();
}
