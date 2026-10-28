/*!
 * \file r125_cta_strategies_test.cpp
 * \brief lane R125: WtStraTwoMa / WtStraDualThrust 剩余分支 + WtCtaTwoMaFact 公开工厂接口
 *
 * 覆盖依据（本轮读源码确认）：
 * - WtStraTwoMa.cpp L112-138 on_session_begin 换月两段式移仓；L149-279 on_schedule
 *   空/缺 K 线早退、stock 交易单位、金叉/死叉与多空出场；L292-334 on_init/on_tick。
 * - WtStraDualThrust.cpp L110-128 on_session_begin；L143-255 on_schedule 同上；
 *   L268-310 on_init/on_tick。
 * - WtCtaTwoMaFact.cpp L35-53 C 导出 create/delete；L105-118 非本厂策略拒删。
 * 假上下文见 r125_cta_fake.h；K 线为真实 WTSKlineSlice，不加载任何插件。
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Includes/WTSVariant.hpp"
#include "Includes/CtaStrategyDefs.h"
#include "WtCtaTwoMaFact/WtCtaTwoMaFact.h"
#include "WtCtaTwoMaFact/WtStraTwoMa.h"
#include "WtCtaStraFact/WtStraDualThrust.h"

#include "r125_cta_fake.h"

USING_NS_WTP;

extern "C" ICtaStrategyFact* createStrategyFact();
extern "C" void deleteStrategyFact(ICtaStrategyFact* fact);

namespace {

class VarHolder
{
public:
	explicit VarHolder(WTSVariant* v = NULL) : _v(v) {}
	~VarHolder() { reset(); }
	VarHolder(const VarHolder&) = delete;
	VarHolder& operator=(const VarHolder&) = delete;
	void reset() { if (_v != NULL) { _v->release(); _v = NULL; } }
	WTSVariant* get() const { return _v; }
private:
	WTSVariant* _v;
};

WTSVariant* makeTwoMaCfg(bool stock)
{
	WTSVariant* root = WTSVariant::createObject();
	root->append("short_days", (uint32_t)2);
	root->append("long_days", (uint32_t)3);
	root->append("period", "d1");
	root->append("count", (uint32_t)10);
	root->append("code", "SHFE.au.2005");
	root->append("stock", stock);
	return root;
}

WTSVariant* makeDtCfg(bool stock)
{
	WTSVariant* root = WTSVariant::createObject();
	root->append("days", (uint32_t)2);
	root->append("k1", 0.5);
	root->append("k2", 0.5);
	root->append("period", "d1");
	root->append("count", (uint32_t)10);
	root->append("code", "SHFE.au.2005");
	root->append("stock", stock);
	return root;
}

bool hasPrefix(const std::vector<std::string>& values, const std::string& prefix)
{
	for (const auto& value : values)
	{
		if (value.compare(0, prefix.size(), prefix) == 0)
			return true;
	}
	return false;
}

} // namespace

// ---------------------------------------------------------------- WtStraTwoMa

TEST(R125CtaTwoMa, SessionBeginSwitchMatrix)
{
	// WtStraTwoMa.cpp L112-138：首次记录主力；换月无仓不挪仓；换月有仓 switchout/switchin；
	// 主力不变不重复触发。
	WtStraTwoMa stra("r125_twoma_sess");
	VarHolder cfg(makeTwoMaCfg(false));
	ASSERT_TRUE(stra.init(cfg.get()));

	r125_cta::Ctx ctx;
	ctx.rawcode = "SHFE.au.2005";
	stra.on_session_begin(&ctx, 20240102);
	EXPECT_TRUE(ctx.set_positions.empty());

	// 主力不变 → 无操作
	stra.on_session_begin(&ctx, 20240103);
	EXPECT_TRUE(ctx.set_positions.empty());

	// 换月但旧主力无仓 → 只更新 _moncode
	ctx.position = 0.0;
	ctx.rawcode = "SHFE.au.2006";
	stra.on_session_begin(&ctx, 20240104);
	EXPECT_TRUE(ctx.set_positions.empty());

	// 再次换月且旧主力有仓 → 先清旧仓再转到新主力
	ctx.position = 3.0;
	ctx.rawcode = "SHFE.au.2007";
	stra.on_session_begin(&ctx, 20240105);
	ASSERT_EQ(2u, ctx.set_positions.size());
	EXPECT_NE(std::string::npos, ctx.set_positions[0].find("2006:0.000000:switchout"));
	EXPECT_NE(std::string::npos, ctx.set_positions[1].find("2007:3.000000:switchin"));
	EXPECT_TRUE(hasPrefix(ctx.logs, "主力换月"));
}

TEST(R125CtaTwoMa, MissingAndEmptyBarsEarlyReturn)
{
	// WtStraTwoMa.cpp L155-167：stra_get_bars 返回 NULL / 空 slice 均直接返回；
	// L300-305 on_init 在 bars==NULL 时提前返回，不再注册图表与指标。
	WtStraTwoMa stra("r125_twoma_bars");
	VarHolder cfg(makeTwoMaCfg(false));
	ASSERT_TRUE(stra.init(cfg.get()));

	r125_cta::Ctx ctx;

	// on_schedule: bars == NULL
	ctx.bars = NULL;
	stra.on_schedule(&ctx, 20240102, 150000);
	EXPECT_TRUE(ctx.orders.empty());

	// on_schedule: 空 K 线（size()==0）
	ctx.bars = r125_cta::make_empty_kline();
	stra.on_schedule(&ctx, 20240102, 150000);
	ctx.bars = NULL;
	EXPECT_TRUE(ctx.orders.empty());

	// on_init: bars == NULL → 只订阅 tick，不注册图表/指标
	stra.on_init(&ctx);
	EXPECT_TRUE(hasPrefix(ctx.subs, "SHFE.au.2005"));
	EXPECT_FALSE(hasPrefix(ctx.subs, "chart:"));
	EXPECT_FALSE(hasPrefix(ctx.subs, "index:"));
}

TEST(R125CtaTwoMa, ScheduleSignalMatrix)
{
	// WtStraTwoMa.cpp L169-279：金叉做多、死叉做空、多仓死叉平多、空仓金叉平空、
	// stock 时 trdUnit=100（L170-172）；每笔委托都带图表标记（add_chart_mark）。
	std::vector<WTSBarStruct> store;

	// 金叉：pre_s=(1+50)/2=25.5 < pre_l=(100+1+50)/3，now_s=55 > now_l=37
	{
		WtStraTwoMa stra("r125_twoma_long");
		VarHolder cfg(makeTwoMaCfg(false));
		ASSERT_TRUE(stra.init(cfg.get()));
		r125_cta::Ctx ctx;
		ctx.position = 0.0;
		ctx.bars = r125_cta::make_kline(store, {0, 100, 1, 50, 60}, 60);
		stra.on_schedule(&ctx, 20240102, 150000);
		ctx.bars = NULL;
		ASSERT_EQ(1u, ctx.orders.size());
		EXPECT_NE(std::string::npos, ctx.orders[0].find("enter_long:"));
		EXPECT_NE(std::string::npos, ctx.orders[0].find(":2.000000:DT_EnterLong"));
		EXPECT_TRUE(hasPrefix(ctx.marks, "wt-mark-buy:DT_EnterLong"));
		EXPECT_TRUE(hasPrefix(ctx.logs, "金叉"));
	}

	// 死叉：pre_s=(100+0)/2=50 > pre_l=(0+100+0)/3，now_s=0 < now_l=33.3 → 空仓进场
	{
		WtStraTwoMa stra("r125_twoma_short");
		VarHolder cfg(makeTwoMaCfg(false));
		ASSERT_TRUE(stra.init(cfg.get()));
		r125_cta::Ctx ctx;
		ctx.position = 0.0;
		ctx.bars = r125_cta::make_kline(store, {0, 100, 0, 0}, 0);
		stra.on_schedule(&ctx, 20240102, 150000);
		ctx.bars = NULL;
		ASSERT_EQ(1u, ctx.orders.size());
		EXPECT_NE(std::string::npos, ctx.orders[0].find("enter_short:"));
		EXPECT_NE(std::string::npos, ctx.orders[0].find(":2.000000:DT_EnterShort"));
		EXPECT_TRUE(hasPrefix(ctx.marks, "wt-mark-sell:DT_EnterShort"));
	}

	// 多仓 + 死叉 → 平多
	{
		WtStraTwoMa stra("r125_twoma_exitlong");
		VarHolder cfg(makeTwoMaCfg(false));
		ASSERT_TRUE(stra.init(cfg.get()));
		r125_cta::Ctx ctx;
		ctx.position = 5.0;
		ctx.bars = r125_cta::make_kline(store, {0, 100, 0, 0}, 0);
		stra.on_schedule(&ctx, 20240102, 150000);
		ctx.bars = NULL;
		ASSERT_EQ(1u, ctx.orders.size());
		EXPECT_NE(std::string::npos, ctx.orders[0].find("exit_long:"));
		EXPECT_NE(std::string::npos, ctx.orders[0].find(":2.000000:DT_ExitLong"));
		EXPECT_TRUE(hasPrefix(ctx.marks, "wt-mark-sell:DT_ExitLong"));
	}

	// 空仓 + 金叉 → 平空
	{
		WtStraTwoMa stra("r125_twoma_exitshort");
		VarHolder cfg(makeTwoMaCfg(false));
		ASSERT_TRUE(stra.init(cfg.get()));
		r125_cta::Ctx ctx;
		ctx.position = -5.0;
		ctx.bars = r125_cta::make_kline(store, {0, 100, 1, 50, 60}, 60);
		stra.on_schedule(&ctx, 20240102, 150000);
		ctx.bars = NULL;
		ASSERT_EQ(1u, ctx.orders.size());
		EXPECT_NE(std::string::npos, ctx.orders[0].find("exit_short:"));
		EXPECT_TRUE(hasPrefix(ctx.marks, "wt-mark-buy:DT_ExitShort"));
	}

	// stock=true → trdUnit=100，金叉下单 2*100
	{
		WtStraTwoMa stra("r125_twoma_stock");
		VarHolder cfg(makeTwoMaCfg(true));
		ASSERT_TRUE(stra.init(cfg.get()));
		r125_cta::Ctx ctx;
		ctx.position = 0.0;
		ctx.bars = r125_cta::make_kline(store, {0, 100, 1, 50, 60}, 60);
		stra.on_schedule(&ctx, 20240102, 150000);
		ctx.bars = NULL;
		ASSERT_EQ(1u, ctx.orders.size());
		EXPECT_NE(std::string::npos, ctx.orders[0].find(":200.000000:DT_EnterLong"));
	}

	// stock=true + 死叉 → 不做空（L238 的 !_isstk 为假）
	{
		WtStraTwoMa stra("r125_twoma_stock_noshort");
		VarHolder cfg(makeTwoMaCfg(true));
		ASSERT_TRUE(stra.init(cfg.get()));
		r125_cta::Ctx ctx;
		ctx.position = 0.0;
		ctx.bars = r125_cta::make_kline(store, {0, 100, 0, 0}, 0);
		stra.on_schedule(&ctx, 20240102, 150000);
		ctx.bars = NULL;
		EXPECT_TRUE(ctx.orders.empty());
	}
}

TEST(R125CtaTwoMa, OnTickIsNoop)
{
	// WtStraTwoMa.cpp L331-334：on_tick 空实现，不触碰上下文
	WtStraTwoMa stra("r125_twoma_tick");
	r125_cta::Ctx ctx;
	WTSTickStruct tick;
	WTSTickData* data = WTSTickData::create(tick);
	stra.on_tick(&ctx, "SHFE.au.2005", data);
	data->release();
	EXPECT_TRUE(ctx.orders.empty());
	EXPECT_TRUE(ctx.logs.empty());
}

// ------------------------------------------------------------ WtStraDualThrust

TEST(R125CtaDualThrust, MissingAndEmptyBarsEarlyReturn)
{
	// WtStraDualThrust.cpp L149-161：NULL/空 K 线早退；
	// L276-281 on_init 在 bars==NULL 时提前返回。
	WtStraDualThrust stra("r125_dt_bars");
	VarHolder cfg(makeDtCfg(false));
	ASSERT_TRUE(stra.init(cfg.get()));

	r125_cta::Ctx ctx;
	ctx.bars = NULL;
	stra.on_schedule(&ctx, 20240102, 150000);
	EXPECT_TRUE(ctx.orders.empty());

	ctx.bars = r125_cta::make_empty_kline();
	stra.on_schedule(&ctx, 20240102, 150000);
	ctx.bars = NULL;
	EXPECT_TRUE(ctx.orders.empty());

	stra.on_init(&ctx);
	EXPECT_TRUE(hasPrefix(ctx.subs, "SHFE.au.2005"));
	EXPECT_FALSE(hasPrefix(ctx.subs, "chart:"));
	EXPECT_FALSE(hasPrefix(ctx.subs, "index:"));
}

TEST(R125CtaDualThrust, ScheduleBreakoutMatrix)
{
	// WtStraDualThrust.cpp L163-255：days=2/k1=k2=0.5 时
	// high[-2]=close[-2]+1、low[-2]=close[-2]-1、hc=lc=close[-2] → range=1，
	// upper=lastOpen+0.5、lower=lastOpen-0.5；配合 closes={5..9}（curPx=9）逐一命中四类分支。
	std::vector<WTSBarStruct> store;

	// 空仓 + curPx>=upper → 多仓进场
	{
		WtStraDualThrust stra("r125_dt_long");
		VarHolder cfg(makeDtCfg(false));
		ASSERT_TRUE(stra.init(cfg.get()));
		r125_cta::Ctx ctx;
		ctx.position = 0.0;
		ctx.bars = r125_cta::make_kline(store, {5, 6, 7, 8, 9}, 8.4);
		stra.on_schedule(&ctx, 20240102, 150000);
		ctx.bars = NULL;
		ASSERT_EQ(1u, ctx.orders.size());
		EXPECT_NE(std::string::npos, ctx.orders[0].find("enter_long:"));
		EXPECT_NE(std::string::npos, ctx.orders[0].find(":2.000000:DT_EnterLong"));
		EXPECT_TRUE(hasPrefix(ctx.marks, "wt-mark-buy:DT_EnterLong"));
		EXPECT_NE(ctx.index_values.end(), ctx.index_values.find("DualThrust/upper_bound"));
	}

	// 空仓 + curPx<=lower → 空仓进场
	{
		WtStraDualThrust stra("r125_dt_short");
		VarHolder cfg(makeDtCfg(false));
		ASSERT_TRUE(stra.init(cfg.get()));
		r125_cta::Ctx ctx;
		ctx.position = 0.0;
		ctx.bars = r125_cta::make_kline(store, {5, 6, 7, 8, 9}, 10.0);
		stra.on_schedule(&ctx, 20240102, 150000);
		ctx.bars = NULL;
		ASSERT_EQ(1u, ctx.orders.size());
		EXPECT_NE(std::string::npos, ctx.orders[0].find("enter_short:"));
		EXPECT_NE(std::string::npos, ctx.orders[0].find(":2.000000:DT_EnterShort"));
		EXPECT_TRUE(hasPrefix(ctx.marks, "wt-mark-sell:DT_EnterShort"));
	}

	// 多仓 + curPx<=lower → 平多
	{
		WtStraDualThrust stra("r125_dt_exitlong");
		VarHolder cfg(makeDtCfg(false));
		ASSERT_TRUE(stra.init(cfg.get()));
		r125_cta::Ctx ctx;
		ctx.position = 2.0;
		ctx.bars = r125_cta::make_kline(store, {5, 6, 7, 8, 9}, 9.6);
		stra.on_schedule(&ctx, 20240102, 150000);
		ctx.bars = NULL;
		ASSERT_EQ(1u, ctx.orders.size());
		EXPECT_NE(std::string::npos, ctx.orders[0].find("exit_long:"));
		EXPECT_TRUE(hasPrefix(ctx.marks, "wt-mark-sell:DT_ExitLong"));
	}

	// 空仓 + curPx>=upper → 平空
	{
		WtStraDualThrust stra("r125_dt_exitshort");
		VarHolder cfg(makeDtCfg(false));
		ASSERT_TRUE(stra.init(cfg.get()));
		r125_cta::Ctx ctx;
		ctx.position = -2.0;
		ctx.bars = r125_cta::make_kline(store, {5, 6, 7, 8, 9}, 8.4);
		stra.on_schedule(&ctx, 20240102, 150000);
		ctx.bars = NULL;
		ASSERT_EQ(1u, ctx.orders.size());
		EXPECT_NE(std::string::npos, ctx.orders[0].find("exit_short:"));
		EXPECT_TRUE(hasPrefix(ctx.marks, "wt-mark-buy:DT_ExitShort"));
	}

	// stock=true → trdUnit=100；空仓突破上轨下单 200
	{
		WtStraDualThrust stra("r125_dt_stock");
		VarHolder cfg(makeDtCfg(true));
		ASSERT_TRUE(stra.init(cfg.get()));
		r125_cta::Ctx ctx;
		ctx.position = 0.0;
		ctx.bars = r125_cta::make_kline(store, {5, 6, 7, 8, 9}, 8.4);
		stra.on_schedule(&ctx, 20240102, 150000);
		ctx.bars = NULL;
		ASSERT_EQ(1u, ctx.orders.size());
		EXPECT_NE(std::string::npos, ctx.orders[0].find(":200.000000:DT_EnterLong"));
	}
}

TEST(R125CtaDualThrust, SessionBeginAndTick)
{
	// WtStraDualThrust.cpp L110-128 换月移仓；L307-310 on_tick 空实现
	WtStraDualThrust stra("r125_dt_sess");
	VarHolder cfg(makeDtCfg(false));
	ASSERT_TRUE(stra.init(cfg.get()));

	r125_cta::Ctx ctx;
	ctx.rawcode = "SHFE.au.2005";
	stra.on_session_begin(&ctx, 20240102);
	EXPECT_TRUE(ctx.set_positions.empty());

	ctx.position = 4.0;
	ctx.rawcode = "SHFE.au.2006";
	stra.on_session_begin(&ctx, 20240103);
	ASSERT_EQ(2u, ctx.set_positions.size());
	EXPECT_NE(std::string::npos, ctx.set_positions[0].find("2005:0.000000:switchout"));
	EXPECT_NE(std::string::npos, ctx.set_positions[1].find("2006:4.000000:switchin"));

	WTSTickStruct tick;
	WTSTickData* data = WTSTickData::create(tick);
	stra.on_tick(&ctx, "SHFE.au.2005", data);
	data->release();
}

// ------------------------------------------------------------ WtCtaTwoMaFact

TEST(R125CtaFactory, ForeignDeleteAndCExport)
{
	// WtCtaTwoMaFact.cpp L88-94 未知策略名返回 NULL；L105-118 非本厂策略拒删；
	// L35-53 createStrategyFact/deleteStrategyFact 的 NULL 与非 NULL 两条路径。
	WtStraFact fact;
	EXPECT_EQ(nullptr, fact.createStrategy("DualThrust", "r125_unknown"));
	EXPECT_EQ(nullptr, fact.createStrategy("TwoMa2", "r125_unknown"));

	r125_cta::ForeignCtaStrategy foreign;
	EXPECT_FALSE(fact.deleteStrategy(&foreign));

	ICtaStrategyFact* api = createStrategyFact();
	ASSERT_NE(nullptr, api);
	EXPECT_STREQ("WtCtaTwoMaFact", api->getName());
	CtaStrategy* stra = api->createStrategy("TwoMa", "r125_api");
	ASSERT_NE(nullptr, stra);
	EXPECT_STREQ("TwoMa", stra->getName());
	EXPECT_TRUE(api->deleteStrategy(stra));
	deleteStrategyFact(api);
	deleteStrategyFact(NULL);
}
