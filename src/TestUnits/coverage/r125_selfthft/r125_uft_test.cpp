/*!
 * \file r125_uft_test.cpp
 * \brief lane R125: WtUftStraDemo 的 on_init/on_tick/check_orders/回报与通道回调
 *
 * 覆盖依据（本轮读源码确认）：
 * - WtUftStraDemo.cpp L78-94 init 参数；L117-135 on_init 参数监视 + K 线 + 订阅；
 *   L145-254 on_tick：非本品种/有挂单/通道未就绪早退、分钟更新、频率控制、
 *   微价信号、买卖下单与订单号入集合；L261-284 check_orders 超时撤单；
 *   L296-314 on_bar/on_trade 空实现；L328-441 on_position/on_order/通道/参数更新。
 * 假上下文见 r125_selfthft_fake.h；tick 为真实 WTSTickData，K 线为真实 WTSKlineSlice。
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Includes/WTSVariant.hpp"
#include "Includes/UftStrategyDefs.h"
#include "WtUftStraFact/WtUftStraDemo.h"

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

WTSVariant* makeUftCfg()
{
	WTSVariant* root = WTSVariant::createObject();
	root->append("code", "SSE.600000");
	root->append("second", (uint32_t)2);
	root->append("freq", (uint32_t)1);
	root->append("offset", (uint32_t)2);
	root->append("lots", 3.0);
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

TEST(R125UftDemo, InitTickEntryAndOrderExpiry)
{
	// L117-135 on_init（bars 非空/为空两支）；L145-254 on_tick 全路径；L261-284 超时撤单
	WtUftStraDemo demo("r125_uft_a");
	VarHolder cfg(makeUftCfg());
	ASSERT_TRUE(demo.init(cfg.get()));

	r125_sh::UftCtx ctx;
	std::vector<WTSBarStruct> store;
	ctx.bars = r125_sh::make_kline(store, {5, 6, 7, 8, 9}, 9);
	demo.on_init(&ctx);
	EXPECT_EQ(4, ctx.watch_calls);
	EXPECT_EQ(1, ctx.commit_calls);
	ASSERT_EQ(1u, ctx.subs.size());
	EXPECT_STREQ("SSE.600000", ctx.subs[0].c_str());
	ctx.bars = NULL;

	// 再走一次 bars==NULL 分支（L126-128 的 if 不成立）
	demo.on_init(&ctx);
	EXPECT_EQ(8, ctx.watch_calls);

	ctx.undone = 0.0;
	demo.on_channel_ready(&ctx);          // 无未完成单 → 只置 ready

	// 非本品种 tick 直接返回（L147-148）
	WTSTickData* wrong = r125_sh::make_tick("OTHER", 100, 101, 102, 10, 10, 150030500);
	demo.on_tick(&ctx, "OTHER", wrong);
	wrong->release();
	EXPECT_TRUE(ctx.buys.empty());

	// 通道断开时 tick 早退（L158-159）
	demo.on_channel_lost(&ctx);
	WTSTickData* tick = r125_sh::make_tick("SSE.600000", 100, 101, 102, 10, 10, 150030500);
	demo.on_tick(&ctx, "SSE.600000", tick);
	tick->release();
	EXPECT_TRUE(ctx.buys.empty());

	demo.on_channel_ready(&ctx);

	// 微价 101.5 > 100 → 做多；targetPx = 100 + 0.5*2 = 101
	tick = r125_sh::make_tick("SSE.600000", 100, 101, 102, 10, 10, 150030500);
	demo.on_tick(&ctx, "SSE.600000", tick);
	tick->release();
	ASSERT_EQ(1u, ctx.buys.size());
	EXPECT_DOUBLE_EQ(101.0, ctx.buys[0].price);
	EXPECT_DOUBLE_EQ(3.0, ctx.buys[0].qty);
	EXPECT_EQ(UFT_OrderFlag_Nor, ctx.buys[0].flag);

	// 有挂单 → 本次 tick 只走 check_orders，未超时不撤单（L151-155 / L263-283）
	tick = r125_sh::make_tick("SSE.600000", 100, 101, 102, 10, 10, 150030500);
	demo.on_tick(&ctx, "SSE.600000", tick);
	tick->release();
	EXPECT_EQ(1u, ctx.buys.size());
	EXPECT_TRUE(ctx.cancels.empty());

	// 成交回报清掉挂单（L353-375）；cancel_cnt 为 0 时不打印递减日志
	demo.on_order(&ctx, 11, "SSE.600000", true, 0, 3.0, 0.0, 101.0, true);
	EXPECT_TRUE(hasString(ctx.logs, "cancelcnt") == false);

	// 挂单清空但同一时刻再次 tick → 频率控制早退（L179-183）
	tick = r125_sh::make_tick("SSE.600000", 100, 101, 102, 10, 10, 150030500);
	demo.on_tick(&ctx, "SSE.600000", tick);
	tick->release();
	EXPECT_EQ(1u, ctx.buys.size());

	// 前进 3 秒后再入场，挂单 11；再前进 3 秒触发超时撤单
	ctx.secs = 33000;
	tick = r125_sh::make_tick("SSE.600000", 100, 101, 102, 10, 10, 150030500);
	demo.on_tick(&ctx, "SSE.600000", tick);
	tick->release();
	ASSERT_EQ(2u, ctx.buys.size());

	ctx.secs = 36000;
	tick = r125_sh::make_tick("SSE.600000", 100, 101, 102, 10, 10, 150030500);
	demo.on_tick(&ctx, "SSE.600000", tick);
	tick->release();
	ASSERT_EQ(1u, ctx.cancels.size());
	EXPECT_EQ(11u, ctx.cancels[0]);
	EXPECT_TRUE(hasString(ctx.logs, "Order expired, cancelcnt updated to 1"));

	// 撤单回报：已知 localid + cancel_cnt>0 → 递减并打印（L360-373）
	demo.on_order(&ctx, 11, "SSE.600000", true, 0, 3.0, 0.0, 101.0, true);
	EXPECT_TRUE(hasString(ctx.logs, "cancelcnt -> 0"));

	// 微价 101.5 < 103 → 做空；targetPx = 103 - 0.5*2 = 102（L235-252）
	ctx.secs = 39000;
	ctx.tick_price = 103;
	tick = r125_sh::make_tick("SSE.600000", 103, 101, 102, 10, 10, 150030500);
	demo.on_tick(&ctx, "SSE.600000", tick);
	tick->release();
	ASSERT_EQ(1u, ctx.sells.size());
	EXPECT_DOUBLE_EQ(102.0, ctx.sells[0].price);
	EXPECT_DOUBLE_EQ(3.0, ctx.sells[0].qty);

	// 委托失败且 localid 在集合中 → 移除（L100-113）
	demo.on_entrust(12, false, "rejected");
	EXPECT_TRUE(ctx.cancels.empty());
	demo.on_entrust(12, true, "ok");
	demo.on_entrust(999, false, "unknown");
}

TEST(R125UftDemo, PositionChannelAndParamCallbacks)
{
	// L328-337 on_position 过滤与日志；L383-408 on_channel_ready 撤未知挂单；
	// L353-375 on_order 已知/未知；L426-441 on_params_updated；L296-314/416-419 空实现。
	WtUftStraDemo demo("r125_uft_b");
	VarHolder cfg(makeUftCfg());
	ASSERT_TRUE(demo.init(cfg.get()));

	r125_sh::UftCtx ctx;
	demo.on_init(&ctx);                    // bars==NULL 分支

	demo.on_position(&ctx, "OTHER", true, 7.0, 7.0, 7.0, 7.0);
	EXPECT_TRUE(ctx.logs.empty());
	demo.on_position(&ctx, "SSE.600000", true, 7.0, 7.0, 7.0, 7.0);
	EXPECT_TRUE(hasString(ctx.logs, "There are 7 of SSE.600000 before today"));

	// 通道就绪时发现 5 手不在管理中的未完成单 → 全部撤销并入集合
	ctx.undone = 5.0;
	demo.on_channel_ready(&ctx);
	ASSERT_EQ(1u, ctx.cancel_all_calls.size());
	EXPECT_STREQ("SSE.600000", ctx.cancel_all_calls[0].c_str());
	EXPECT_TRUE(hasString(ctx.logs, "有不在管理中的未完成单 5 手,全部撤销"));
	EXPECT_TRUE(hasString(ctx.logs, "cancelcnt -> 2"));

	// 撤单回报已知 localid → 清集合、递减 cancel_cnt
	demo.on_order(&ctx, 21, "SSE.600000", true, 0, 1.0, 0.0, 101.0, true);
	EXPECT_TRUE(hasString(ctx.logs, "cancelcnt -> 1"));
	demo.on_order(&ctx, 999, "SSE.600000", true, 0, 1.0, 0.0, 101.0, true);

	// 参数热更新：read_param 返回上下文里的新值
	ctx.u_params["second"] = 5;
	ctx.u_params["freq"] = 7;
	ctx.i_params["offset"] = 3;
	ctx.d_params["lots"] = 9.0;
	demo.on_params_updated();
	EXPECT_TRUE(hasString(ctx.logs, "Params updated, second: 5, freq: 7, offset: 3, lots: 9"));

	// 空回调和已断线回调不产生副作用
	demo.on_trade(&ctx, 1, "SSE.600000", true, 0, 1.0, 100.0);
	WTSBarStruct bar;
	demo.on_bar(&ctx, "SSE.600000", "m1", 1, &bar);
	demo.on_channel_lost(&ctx);
	demo.on_entrust(12345, true, "ignored");
	EXPECT_TRUE(ctx.cancels.empty());
}
