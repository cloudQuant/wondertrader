/*!
 * \file r125_hft_test.cpp
 * \brief lane R125: WtHftStraDemo 的 on_init/do_calc/check_orders/回报与通道回调
 *
 * 覆盖依据（本轮读源码确认）：
 * - WtHftStraDemo.cpp L77-89 init 参数（stock 决定 _unit=100/1）；
 *   L111-123 on_init；L131-211 do_calc：频率控制、最后一笔 tick 为空、微价信号、
 *   _reserved 修正后的多空开仓条件；L221-235 on_tick；L242-258 check_orders 超时撤单；
 *   L286-377 on_trade/on_order/on_channel_ready/on_channel_lost。
 * 假上下文见 r125_selfthft_fake.h；tick/K 线均为真实产品对象。
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Includes/WTSVariant.hpp"
#include "Includes/HftStrategyDefs.h"
#include "WtHftStraFact/WtHftStraDemo.h"

#include "r125_selfthft_fake.h"

USING_NS_WTP;

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

WTSVariant* makeHftCfg(bool stock, double reserve)
{
	WTSVariant* root = WTSVariant::createObject();
	root->append("code", "SSE.600000");
	root->append("second", (uint32_t)2);
	root->append("freq", (uint32_t)1);
	root->append("offset", (uint32_t)2);
	root->append("reserve", reserve);
	root->append("stock", stock);
	return root;
}

bool hasString(const std::vector<std::string>& values, const std::string& needle)
{
	for (const auto& value : values)
	{
		if (value.find(needle) != std::string::npos)
			return true;
	}
	return false;
}

} // namespace

TEST(R125HftDemo, InitCalcEntryAndOrderExpiry)
{
	// L111-123 on_init；L131-211 do_calc 频率控制/微价多空；L221-235 on_tick；
	// L242-258 check_orders 超时撤单；L286-340 on_trade/on_order。
	WtHftStraDemo demo("r125_hft_a");
	VarHolder cfg(makeHftCfg(false, 0.0));
	ASSERT_TRUE(demo.init(cfg.get()));

	r125_sh::HftCtx ctx;
	std::vector<WTSBarStruct> store;
	ctx.bars = r125_sh::make_kline(store, {5, 6, 7, 8, 9}, 9);
	demo.on_init(&ctx);
	ASSERT_EQ(1u, ctx.subs.size());
	EXPECT_STREQ("SSE.600000", ctx.subs[0].c_str());
	ctx.bars = NULL;
	demo.on_init(&ctx);                    // bars==NULL 分支（L116-118 的 if 不成立）

	ctx.undone = 0.0;
	demo.on_channel_ready(&ctx);           // 无未完成单 → 只置 ready

	// 非本品种 tick 早退（L222-223）
	WTSTickData* wrong = r125_sh::make_tick("OTHER", 100, 101, 102, 10, 10, 150030500);
	demo.on_tick(&ctx, "OTHER", wrong);
	wrong->release();
	EXPECT_TRUE(ctx.buys.empty());

	// 通道断开时 tick 早退（L231-232）
	demo.on_channel_lost(&ctx);
	WTSTickData* tick = r125_sh::make_tick("SSE.600000", 100, 101, 102, 10, 10, 150030500);
	demo.on_tick(&ctx, "SSE.600000", tick);
	tick->release();
	EXPECT_TRUE(ctx.buys.empty());

	demo.on_channel_ready(&ctx);

	// 微价 101.5 > 100 → 买入 1 手，targetPx = 100 + 0.5*2 = 101，userTag=enterlong
	tick = r125_sh::make_tick("SSE.600000", 100, 101, 102, 10, 10, 150030500);
	demo.on_tick(&ctx, "SSE.600000", tick);
	tick->release();
	ASSERT_EQ(1u, ctx.buys.size());
	EXPECT_DOUBLE_EQ(101.0, ctx.buys[0].price);
	EXPECT_DOUBLE_EQ(1.0, ctx.buys[0].qty);
	EXPECT_STREQ("enterlong", ctx.buys[0].tag.c_str());

	// 有挂单 → 只检查超时；未到期不撤单
	tick = r125_sh::make_tick("SSE.600000", 100, 101, 102, 10, 10, 150030500);
	demo.on_tick(&ctx, "SSE.600000", tick);
	tick->release();
	EXPECT_TRUE(ctx.local_cancels.empty());

	// on_trade 触发 do_calc，但距上次入场不足 freq*1000ms → 频率控制早退
	demo.on_trade(&ctx, 31, "SSE.600000", true, 1.0, 101.0, "enterlong");
	EXPECT_EQ(1u, ctx.buys.size());

	// 前进 3 秒 → check_orders 撤单 31
	ctx.secs = 33000;
	tick = r125_sh::make_tick("SSE.600000", 100, 101, 102, 10, 10, 150030500);
	demo.on_tick(&ctx, "SSE.600000", tick);
	tick->release();
	ASSERT_EQ(1u, ctx.local_cancels.size());
	EXPECT_EQ(31u, ctx.local_cancels[0]);
	EXPECT_TRUE(hasString(ctx.logs, "Order expired, cancelcnt updated to 1"));

	// 撤单回报清挂单；最后一笔 tick 缺失时 do_calc 早退（L141-143）
	ctx.provide_last_tick = false;
	demo.on_order(&ctx, 31, "SSE.600000", true, 1.0, 0.0, 101.0, true, "enterlong");
	EXPECT_TRUE(hasString(ctx.logs, "cancelcnt -> 0"));

	// 微价 101.5 < 103 且持多 → 卖出 1 手，targetPx = 103 - 0.5*2 = 102
	ctx.provide_last_tick = true;
	ctx.tick_price = 103;
	ctx.position = 5.0;
	tick = r125_sh::make_tick("SSE.600000", 103, 101, 102, 10, 10, 150030500);
	demo.on_tick(&ctx, "SSE.600000", tick);
	tick->release();
	ASSERT_EQ(1u, ctx.sells.size());
	EXPECT_DOUBLE_EQ(102.0, ctx.sells[0].price);
	EXPECT_STREQ("entershort", ctx.sells[0].tag.c_str());
}

TEST(R125HftDemo, StockReservedChannelAndOrderCallbacks)
{
	// L194-207 空仓卖出的 stock/reserved 组合；L348-367 on_channel_ready 撤未知挂单；
	// L320-340 on_order 已知/未知；L270-304 空实现。
	WtHftStraDemo demo("r125_hft_b");
	VarHolder cfg(makeHftCfg(true, 0.0));
	ASSERT_TRUE(demo.init(cfg.get()));

	r125_sh::HftCtx ctx;
	demo.on_init(&ctx);                    // bars==NULL
	ctx.undone = 0.0;
	demo.on_channel_ready(&ctx);

	// stock=true 且 reserve=0、空仓：反信号不卖（L194 条件为假）
	ctx.tick_price = 103;
	ctx.position = 0.0;
	WTSTickData* tick = r125_sh::make_tick("SSE.600000", 103, 101, 102, 10, 10, 150030500);
	demo.on_tick(&ctx, "SSE.600000", tick);
	tick->release();
	EXPECT_TRUE(ctx.sells.empty());

	// 改为 stock=false、reserve=1、持仓 1 → 修正后空仓，反信号卖出
	VarHolder cfg2(makeHftCfg(false, 1.0));
	ASSERT_TRUE(demo.init(cfg2.get()));
	ctx.position = 1.0;
	ctx.secs = 33000;
	tick = r125_sh::make_tick("SSE.600000", 103, 101, 102, 10, 10, 150030500);
	demo.on_tick(&ctx, "SSE.600000", tick);
	tick->release();
	ASSERT_EQ(1u, ctx.sells.size());

	// 清掉卖单，最后一笔 tick 缺失避免 on_order 尾部 do_calc 再下单
	ctx.provide_last_tick = false;
	demo.on_order(&ctx, 32, "SSE.600000", false, 1.0, 0.0, 102.0, true, "entershort");
	demo.on_order(&ctx, 999, "SSE.600000", false, 1.0, 0.0, 102.0, true, "unknown");

	// 通道就绪发现 5 手未知未完成单 → stra_cancel(code,isBuy,qty) 撤单并入集合
	ctx.undone = 5.0;
	demo.on_channel_ready(&ctx);
	ASSERT_EQ(1u, ctx.code_cancels.size());
	EXPECT_NE(std::string::npos, ctx.code_cancels[0].find("SSE.600000:buy:5.000000"));
	EXPECT_TRUE(hasString(ctx.logs, "有不在管理中的未完成单 5 手,全部撤销"));
	EXPECT_TRUE(hasString(ctx.logs, "cancelcnt -> 2"));

	// 撤单回报递减 cancel_cnt
	demo.on_order(&ctx, 41, "SSE.600000", true, 1.0, 0.0, 102.0, true, "cancel");
	EXPECT_TRUE(hasString(ctx.logs, "cancelcnt -> 1"));

	// 空实现回调不产生副作用
	demo.on_entrust(12345, true, "ignored", "tag");
	WTSBarStruct bar;
	demo.on_bar(&ctx, "SSE.600000", "m1", 1, &bar);
	demo.on_position(&ctx, "SSE.600000", true, 0.0, 0.0, 0.0, 0.0);
	demo.on_channel_lost(&ctx);
	EXPECT_TRUE(ctx.buys.empty());
}
