/**
 * r201_mocker_residual_test.cpp  (lane R201)
 *
 * 靶子：WtBtCore 三个 Mocker 的公开残余面
 *   UftMocker.cpp：CM_CoverToday 昨仓可用分支（509-512/572-575/771-772），
 *     撤单还原昨仓可用（431-432），短仓会话日志（384），
 *     enum_position 非匹配合约 continue（1038），平今跳过昨仓明细与零量明细
 *     （1269/1273）
 *   CtaMocker.h：id()（334）——CtaMocker.cpp 的 46 行缺口全部落在
 *     hook/步进等待、私有条件单 default 分支和恒不可达的建目录/空品种/零量明细上
 *   HftMocker.cpp：插件工厂装载成功但配置无 strategy 的返回路径（239-245、252）
 *
 * 安全口径：
 *   - 不启动线程、不 sleep、不读挂钟；不调用 install_hook/step_calc/on_schedule
 *     的 hook 分支。
 *   - 插件工厂的析构在本机构建上会 SIGSEGV（BUG-R191-001），插件用例一律
 *     new 后故意不 delete，进程正常退出以留下 profraw。
 *   - 平今/零量用例直接走公开 IDataSink 回调 on_trade，不依赖撮合线程。
 *   - 只走公开 API；不 #define private public、不镜像类布局、不改宏。
 */
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include <Includes/WTSContractInfo.hpp>
#include <WtBtCore/CtaMocker.h>
#include <WtBtCore/HftMocker.h>
#include <WtBtCore/UftMocker.h>

#include "r121_common.h"
#include "r191_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
	const char* const kCu = "SHFE.cu.2406";
	const char* const kStk = "SSE.STK.000001";
}

class R201MockerFixture : public r62::ReplayFixture
{
protected:
	// UftMocker::procTask 是 private，公开路径里只有 on_tick 会排空任务队列。
	// 喂一笔策略不关注的 kCu tick，即可执行 entrust/cancel 任务而不触发 kStk 撮合。
	void pump(UftMocker& m)
	{
		WTSTickData* t = r62::make_tick(kCu, 20200102u, 93100000u, 70000.0, 69990.0, 70010.0);
		m.handle_tick(kCu, t, 0);
		t->release();
	}
};

// Uft 平昨可用：撤单把 left 还原到 _preavail 后，
// stra_sell 走多头昨仓（572-575）、stra_exit_short 扣减空头昨仓（771-772）、
// stra_buy 先平空头昨仓（509-512）。默认 CM_OpenCover 只动 _newavail，
// 因此可在同一对象上人工制造“昨仓可用”而避开 on_session_begin 的短仓缺陷。
TEST_F(R201MockerFixture, R201UftMocker_CancelRestoresPreavailAndCoverTodayCloses)
{
	WTSCommodityInfo* comm = _rp.get_commodity_info(kStk);
	ASSERT_NE(nullptr, comm);
	comm->setCoverMode(CM_OpenCover);

	UftMocker m(&_rp, "r201_uft_preavail");
	m.on_trade(1u, kStk, true, 0u, 50.0, 10.0);
	ASSERT_DOUBLE_EQ(50.0, m.stra_get_position(kStk, true, 1));

	// 多头：CM_OpenCover 平仓单只扣 newavail；改回 CM_CoverToday 后撤单，
	// procTask 的撤单任务把 left 加回 preavail。
	uint32_t longExit = m.stra_exit_long(kStk, 10.0, 50.0, false);
	ASSERT_NE(0u, longExit);
	comm->setCoverMode(CM_CoverToday);
	EXPECT_TRUE(m.stra_cancel(longExit));
	pump(m);
	ASSERT_DOUBLE_EQ(50.0, m.stra_get_position(kStk, true, 1));

	OrderIDs sells = m.stra_sell(kStk, 10.0, 40.0);
	EXPECT_EQ(static_cast<std::size_t>(1), sells.size());
	EXPECT_DOUBLE_EQ(10.0, m.stra_get_position(kStk, true, 1));

	// 空头：同样先扣 newavail，再靠撤单补回 preavail。
	comm->setCoverMode(CM_OpenCover);
	m.on_trade(2u, kStk, false, 0u, 50.0, 10.0);
	ASSERT_DOUBLE_EQ(50.0, m.stra_get_position(kStk, true, 2));
	uint32_t shortExit = m.stra_exit_short(kStk, 10.0, 50.0, false);
	ASSERT_NE(0u, shortExit);
	comm->setCoverMode(CM_CoverToday);
	EXPECT_TRUE(m.stra_cancel(shortExit));
	pump(m);
	ASSERT_DOUBLE_EQ(50.0, m.stra_get_position(kStk, true, 2));

	// 空头昨仓 50：先平 20（771-772），再买 60（509-512 平 30 昨仓 + 开 30 多）。
	uint32_t exitPart = m.stra_exit_short(kStk, 10.0, 20.0, false);
	EXPECT_NE(0u, exitPart);
	EXPECT_DOUBLE_EQ(30.0, m.stra_get_position(kStk, true, 2));

	OrderIDs buys = m.stra_buy(kStk, 10.0, 60.0);
	EXPECT_EQ(static_cast<std::size_t>(2), buys.size());
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_position(kStk, true, 2));
	pump(m);
}

// Uft 平今：第一条明细是昨仓（opentdate != curTDate）时 continue（1269）；
// 同日明细在该轮 left==0 时 maxQty==0 也 continue（1273）。
TEST_F(R201MockerFixture, R201UftMocker_TodayCloseSkipsPriorDayAndZeroQtyDetails)
{
	WTSCommodityInfo* comm = _rp.get_commodity_info(kRb);
	ASSERT_NE(nullptr, comm);
	comm->setCoverMode(CM_CoverToday);

	UftMocker m(&_rp, "r201_uft_today");

	// 建仓发生在 get_trading_date()==0 时，明细 1 的 opentdate 为 0。
	m.on_trade(1u, kRb, true, 0u, 10.0, 3800.0);
	ASSERT_DOUBLE_EQ(10.0, m.stra_get_position(kRb, false, 3));

	// prepare() 把回放游标设到 20200102，后续明细的 opentdate 与之不同。
	r121::NullSink sink;
	_rp.register_sink(&sink, "r201_uft_today");
	ASSERT_TRUE(_rp.prepare());
	ASSERT_NE(0u, _rp.get_trading_date());

	m.on_trade(2u, kRb, true, 0u, 10.0, 3800.0);
	ASSERT_DOUBLE_EQ(20.0, m.stra_get_position(kRb, false, 3));

	// 零量平今：昨仓明细在 1269 跳过，今仓明细 maxQty==0 在 1273 跳过。
	m.on_trade(3u, kRb, true, 2u, 0.0, 3800.0);
	EXPECT_DOUBLE_EQ(20.0, m.stra_get_position(kRb, false, 3));

	m.on_trade(4u, kRb, true, 2u, 10.0, 3800.0);
	EXPECT_DOUBLE_EQ(10.0, m.stra_get_position(kRb, false, 3));
}

// Uft 插件：短仓非零时 on_session_end 写短仓明细（384）；
// enum_position 传入不在 _pos_map 的合约，循环体 continue（1038）。
TEST_F(R201MockerFixture, R201UftMocker_PluginEnumMismatchAndShortSessionLog)
{
	WTSCommodityInfo* comm = _rp.get_commodity_info(kStk);
	ASSERT_NE(nullptr, comm);
	comm->setTradingMode(TM_LongT1);
	comm->setCoverMode(CM_CoverToday);

	// BUG-R191-001：插件工厂析构崩，故意泄漏到进程退出
	auto* m = new UftMocker(&_rp, "r201_uft_plugin");
	WTSVariant* cfg = r191::make_uft_cfg(
		r111::plugin_path("WtUftStraFact", "WtUftRunner"), "r201_uft_plugin", kStk);
	ASSERT_TRUE(m->init_uft_factory(cfg));
	cfg->release();

	// T+1 多仓跨日解冻，保证策略回调与会话日志路径都处于真实策略上下文。
	m->on_trade(1u, kStk, true, 0u, 100.0, 10.0);
	m->handle_session_begin(20200103u);
	EXPECT_DOUBLE_EQ(100.0, m->stra_get_position(kStk, true, 1));

	// 开 5 股空仓（不跨日），会话结束时短仓非零。
	m->on_trade(2u, kStk, false, 0u, 5.0, 10.0);
	EXPECT_DOUBLE_EQ(5.0, m->stra_get_position(kStk, false, 2));

	// kCu 不在持仓表，循环命中 continue 后返回 0。
	EXPECT_DOUBLE_EQ(0.0, m->stra_enum_position(kCu));

	m->handle_session_end(20200103u);
	m->handle_replay_done();
	EXPECT_TRUE(std::filesystem::exists(_out / "r201_uft_plugin" / "positions.csv"));
}

// BUG-R201-001 复现占位：on_session_begin 把 PosInfo& 当 PosItem& 用，
// 只解冻多头，空头 T+1 冻结永远不释放；启用后此处应得 50，实际为 0。
TEST_F(R201MockerFixture, DISABLED_R201UftMocker_ShortFrozenIsNotReleasedOnSessionBegin)
{
	WTSCommodityInfo* comm = _rp.get_commodity_info(kStk);
	ASSERT_NE(nullptr, comm);
	comm->setTradingMode(TM_LongT1);

	// BUG-R191-001：插件工厂析构崩，故意泄漏到进程退出
	auto* m = new UftMocker(&_rp, "r201_uft_short_frozen");
	WTSVariant* cfg = r191::make_uft_cfg(
		r111::plugin_path("WtUftStraFact", "WtUftRunner"), "r201_uft_short_frozen", kStk);
	ASSERT_TRUE(m->init_uft_factory(cfg));
	cfg->release();

	m->on_trade(1u, kStk, false, 0u, 50.0, 10.0);
	m->handle_session_begin(20200103u);
	EXPECT_DOUBLE_EQ(50.0, m->stra_get_position(kStk, true, 2));
}

// CtaMocker.h:334：上下文 id 只读、稳定、单调分配。
TEST_F(R201MockerFixture, R201CtaMocker_ContextIdIsStableAndMonotonic)
{
	CtaMocker first(&_rp, "r201_cta_id_a");
	CtaMocker second(&_rp, "r201_cta_id_b");

	EXPECT_GT(first.id(), 0u);
	EXPECT_EQ(first.id(), first.id());
	EXPECT_NE(first.id(), second.id());
	EXPECT_DOUBLE_EQ(0.0, first.stra_get_position(kRb));
}

// HftMocker.cpp:239-245、252：module + createStrategyFact 成功、配置无 strategy
// 段时的装载返回路径。本机构建没有 WtHftStraFact，退回同 C ABI 的 Cta 工厂；
// 不创建策略、不解引用错误类型的工厂指针，且刻意泄漏以避开 BUG-R191-001 的析构崩溃。
TEST_F(R201MockerFixture, R201HftMocker_FactoryLoadWithoutStrategySucceeds)
{
	std::string module = r111::plugin_path("WtHftStraFact");
	if (!std::filesystem::exists(module))
		module = r111::plugin_path("WtCtaStraFact");
	if (!std::filesystem::exists(module))
		GTEST_SKIP() << "no loadable strategy factory module";

	auto* m = new HftMocker(&_rp, "r201_hft_module");
	WTSVariant* cfg = r121::make_module_cfg(module);
	EXPECT_TRUE(m->init_hft_factory(cfg));
	cfg->release();
}
