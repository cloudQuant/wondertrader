/**
 * r191_mocker_residual_test.cpp  (lane R191)
 *
 * 靶子：WtBtCore 三个 Mocker 的残余公开面
 *   UftMocker.cpp：插件工厂/生命周期/会话/回调/枚举（154-166、206-225、296-390、
 *     804-825、1031-1052）；CM_CoverToday 的昨仓平仓/撤单/买卖分支
 *     （431-432、509-512、572-575、710-711、771-772）
 *   CtaMocker.cpp：插件工厂与五路回调（433-451、968、1024、1034、1040、1238、1267）；
 *     do_set_position 同量早退（1614）
 *   HftMocker.cpp：撤单任务体（561）与按合约按量撤单重载
 *
 * 关键安全口径：
 *   - 真实插件工厂的 Mocker 析构会 SIGSEGV（R121 的 R135ProbeMinimalDtor /
 *     R135ProbeStrategyNoCalls 日志，BUG-R191-001），因此插件用例用 new 创建后
 *     故意不 delete；进程退出由 OS 回收，目的是拿到可度量的 profraw，而不是跳过断言。
 *   - 不调用 Uft/Cta 的 `handle_schedule`、`step_calc`、`install_hook`，不进入 R111/R121
 *     记录的空指针/线程等待崩溃面；不启动任何线程、不读挂钟。
 *   - Uft 插件用例只喂策略不关注的 kCu tick（策略 code=kStk），避免 SimpleUft 下单。
 *   - Cta 插件用例的策略 code=kAg 没有 csv 数据，on_init/on_calculate 在
 *     `stra_get_bars` 返回 NULL 后早退，不会触发交易信号。
 *
 * 只走公开 API；不 #define private public、不镜像类布局、不改宏。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include <Includes/WTSContractInfo.hpp>
#include <WtBtCore/CtaMocker.h>
#include <WtBtCore/HftMocker.h>
#include <WtBtCore/UftMocker.h>
#include <WtBtCore/WtHelper.h>

#include "r191_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
	const char* const kCu = "SHFE.cu.2406";
	const char* const kAg = "SHFE.ag.2412";
	const char* const kStk = "SSE.STK.000001";
}

class R191MockerFixture : public r62::ReplayFixture
{
protected:
	void feed(CtaMocker& m, const char* code, double px)
	{
		WTSTickData* t = r62::make_tick(code, 20200102u, 93100000u, px, px - 1.0, px + 1.0);
		m.handle_tick(code, t, 0);
		t->release();
	}

	void feed(HftMocker& m, const char* code, double px)
	{
		WTSTickData* t = r62::make_tick(code, 20200102u, 93100000u, px, px - 1.0, px + 1.0);
		m.handle_tick(code, t, 0);
		t->release();
	}

	// UftMocker::procTask 是 private，公开路径里只有 on_tick 会排空任务队列。
	// 喂策略不关注的 kCu tick 即可执行 entrust/cancel 任务，不触碰 kStk 的撮合。
	void pump(UftMocker& m)
	{
		WTSTickData* t = r62::make_tick(kCu, 20200102u, 93100000u, 70000.0, 69990.0, 70010.0);
		m.handle_tick(kCu, t, 0);
		t->release();
	}
};

// Uft 插件工厂 + on_init/channel_ready + 会话冻结释放 + 事件分发 + 枚举 + 收尾落盘
TEST_F(R191MockerFixture, R191UftMocker_PluginLifecycleAndSessionCallbacks)
{
	WTSCommodityInfo* comm = _rp.get_commodity_info(kStk);
	ASSERT_NE(nullptr, comm);
	comm->setTradingMode(TM_LongT1);

	// BUG-R191-001：插件工厂的析构会崩，故意泄漏到进程退出，见 bugs/R191.md
	auto* m = new UftMocker(&_rp, "r191_uft_life");
	WTSVariant* cfg = r191::make_uft_cfg(
		r111::plugin_path("WtUftStraFact", "WtUftRunner"), "r191_uft_life", kStk);
	ASSERT_TRUE(m->init_uft_factory(cfg));
	cfg->release();

	m->handle_init();
	EXPECT_DOUBLE_EQ(0.0, m->stra_get_undone(kStk));

	// T+1 开 100 股：总持仓 100、当日可用 0
	m->on_trade(1u, kStk, true, 0u, 100.0, 10.0);
	EXPECT_DOUBLE_EQ(100.0, m->stra_get_position(kStk, false, 1));
	EXPECT_DOUBLE_EQ(0.0, m->stra_get_position(kStk, true, 1));

	// 跨日解冻：preavail=100，并进入策略 on_session_begin
	m->handle_session_begin(20200103u);
	EXPECT_DOUBLE_EQ(100.0, m->stra_get_position(kStk, true, 1));
	EXPECT_DOUBLE_EQ(100.0, m->stra_enum_position(kStk));

	// K 线 / Level2 三类事件：SimpleUft 对 kStk 的 on_bar 为空，Level2 走基类空实现
	WTSBarStruct bar = r62::make_bar(20200102u, 1500u, 10.0);
	m->handle_bar_close(kStk, "m1", 1, &bar);
	m->handle_order_queue(kStk, nullptr);
	m->handle_order_detail(kStk, nullptr);
	m->handle_transaction(kStk, nullptr);

	// 订阅并喂策略不关注的 kCu tick：覆盖 on_tick 转发，策略首行即返回
	m->stra_sub_ticks(kCu);
	WTSTickData* tick = r62::make_tick(kCu, 20200102u, 93100000u, 70000.0, 69990.0, 70010.0);
	m->handle_tick(kCu, tick, 0);
	tick->release();

	// 三个回报入口直调：订单不在 SimpleUft 内部表，转发后安全返回
	m->on_entrust(999u, kCu, true, "ok");
	m->on_order(999u, kCu, true, 0u, 1.0, 1.0, 70000.0, false);
	m->on_channel_ready();

	m->handle_session_end(20200103u);
	m->handle_replay_done();

	EXPECT_DOUBLE_EQ(0.0, m->stra_get_undone(kStk));
	EXPECT_TRUE(std::filesystem::exists(_out / "r191_uft_life" / "positions.csv"));
}

// Uft TM_LongT1 + CM_CoverToday 多头昨仓分支：平昨 -> 撤单还原
TEST_F(R191MockerFixture, R191UftMocker_CoverTodayLongPreavailExitCancel)
{
	WTSCommodityInfo* comm = _rp.get_commodity_info(kStk);
	ASSERT_NE(nullptr, comm);
	comm->setTradingMode(TM_LongT1);
	comm->setCoverMode(CM_CoverToday);

	// BUG-R191-001：同上，故意泄漏
	auto* m = new UftMocker(&_rp, "r191_uft_preavail");
	WTSVariant* cfg = r191::make_uft_cfg(
		r111::plugin_path("WtUftStraFact", "WtUftRunner"), "r191_uft_preavail", kStk);
	ASSERT_TRUE(m->init_uft_factory(cfg));
	cfg->release();

	// 开 100 股并跨日，得到昨仓可用 100
	m->on_trade(1u, kStk, true, 0u, 100.0, 10.0);
	m->handle_session_begin(20200103u);
	ASSERT_DOUBLE_EQ(100.0, m->stra_get_position(kStk, true, 1));

	// 平昨 40（offset=1）-> 撤单把 left 加回 preavail
	uint32_t lid = m->stra_exit_long(kStk, 20.0, 40.0, false, 0);
	ASSERT_NE(0u, lid);
	EXPECT_DOUBLE_EQ(40.0, m->stra_get_undone(kStk));
	pump(*m);
	EXPECT_TRUE(m->stra_cancel(lid));
	pump(*m);
	EXPECT_DOUBLE_EQ(100.0, m->stra_get_position(kStk, true, 1));
}

// Cta 插件工厂 + on_init/会话/bar/tick/on_calculate 五路回调（不调度、不起线程）
TEST_F(R191MockerFixture, R191CtaMocker_PluginFactoryAndDispatch)
{
	// BUG-R191-001：插件工厂的析构会崩，故意泄漏到进程退出
	auto* m = new CtaMocker(&_rp, "r191_cta_plugin");
	WTSVariant* cfg = r191::make_cta_cfg(
		r111::plugin_path("WtCtaStraFact"), "r191_cta_plugin", kAg);
	ASSERT_TRUE(m->init_cta_factory(cfg));
	cfg->release();

	// kAg 没有 csv 数据：on_init/on_calculate 的 stra_get_bars 返回 NULL 后早退
	m->handle_init();
	m->handle_session_begin(20200103u);
	m->handle_session_end(20200103u);

	m->stra_sub_ticks(kRb);
	WTSTickData* tick = r62::make_tick(kRb, 20200102u, 93100000u, 3800.0, 3799.0, 3801.0);
	m->handle_tick(kRb, tick, 0);
	tick->release();

	m->stra_sub_bar_events(kRb, "d1");
	WTSBarStruct bar = r62::make_bar(20200102u, 1500u, 10.0);
	m->handle_bar_close(kRb, "d", 1, &bar);

	m->on_calculate(20200102u, 931u);
	EXPECT_DOUBLE_EQ(0.0, m->stra_get_position(kAg));

	m->handle_replay_done();
	EXPECT_TRUE(std::filesystem::exists(_out / "r191_cta_plugin" / "btchart.json"));
}

// Cta do_set_position 同量早退：仓位已 1，再发 qty=0 的开多信号，目标仍为 1
TEST_F(R191MockerFixture, R191CtaMocker_EqualTargetEnterLongShortCircuits)
{
	CtaMocker m(&_rp, "r191_cta_equal", 0, false);
	m.stra_set_position(kRb, 1.0, "open");
	feed(m, kRb, 3800.0);
	ASSERT_DOUBLE_EQ(1.0, m.stra_get_position(kRb));

	const double enterPx = m.stra_get_last_enterprice(kRb);
	m.stra_enter_long(kRb, 0.0, "dup");
	feed(m, kRb, 3800.0);

	EXPECT_DOUBLE_EQ(1.0, m.stra_get_position(kRb));
	EXPECT_DOUBLE_EQ(enterPx, m.stra_get_last_enterprice(kRb));
}

// Hft 撤单任务体：卖限价 4000 高于买一 3799，不成交；撤单任务把 left 置 0 并摘单
TEST_F(R191MockerFixture, R191HftMocker_CancelQueuedOrderClearsUndone)
{
	HftMocker m(&_rp, "r191_hft_cancel");
	OrderIDs ids = m.stra_sell(kRb, 4000.0, 1.0, "limit");
	ASSERT_EQ(static_cast<uint32_t>(1), static_cast<uint32_t>(ids.size()));
	EXPECT_DOUBLE_EQ(-1.0, m.stra_get_undone(kRb));

	EXPECT_TRUE(m.stra_cancel(ids[0]));
	feed(m, kRb, 3800.0);

	EXPECT_DOUBLE_EQ(0.0, m.stra_get_undone(kRb));
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_position(kRb));
}

// Hft 按合约按量撤单重载：两笔卖单 qty=3 覆盖 1+2，tick 后排空
TEST_F(R191MockerFixture, R191HftMocker_CancelByCodeDrainsMultipleOrders)
{
	HftMocker m(&_rp, "r191_hft_cancel_code");
	OrderIDs first = m.stra_sell(kRb, 4000.0, 1.0, "a");
	OrderIDs second = m.stra_sell(kRb, 4010.0, 2.0, "b");
	ASSERT_EQ(static_cast<uint32_t>(1), static_cast<uint32_t>(first.size()));
	ASSERT_EQ(static_cast<uint32_t>(1), static_cast<uint32_t>(second.size()));

	OrderIDs canceled = m.stra_cancel(kRb, false, 3.0);
	EXPECT_EQ(static_cast<uint32_t>(2), static_cast<uint32_t>(canceled.size()));

	feed(m, kRb, 3800.0);
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_undone(kRb));
}

// BUG-R191-001 复现占位：插件工厂加载成功后，UftMocker 析构在 _StraFactInfo::~_StraFactInfo
// 调用 _remover(_fact) 时 SIGSEGV（R121 R135ProbeMinimalDtor 日志亦停在 leaving scope）。
// 启用即崩溃，故禁用；本轮启用用例改用 new 后不 delete 的写法规避。
TEST_F(R191MockerFixture, DISABLED_R191Uft_PluginFactoryDestructorCrashes)
{
	UftMocker m(&_rp, "r191_uft_dtor");
	WTSVariant* cfg = r191::make_uft_cfg(
		r111::plugin_path("WtUftStraFact", "WtUftRunner"), "r191_uft_dtor", kStk);
	ASSERT_TRUE(m.init_uft_factory(cfg));
	cfg->release();
}
