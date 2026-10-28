/**
 * r151_mocker_edges_test.cpp  (lane R151)
 *
 * 靶子：src/WtBtCore 下四个 Mockers 的剩余公开分支
 *   CtaMocker.cpp:1638-1651   do_set_position 比率滑点（开仓/平仓）
 *   SelMocker.cpp:681-689     do_set_position 比率滑点
 *   HftMocker.cpp:561         stra_cancel 任务体把 left 置 0
 *   HftMocker.cpp:739-742     procOrder 对手价为 0 早退
 *   HftMocker.cpp:756-759     procOrder 卖限价高于买一价早退
 *   UftMocker.cpp:274-280     on_tick 成交后摘单
 *   UftMocker.cpp:969         procOrder 部分成交返回 false（订单保留）
 *
 * Hft/Uft 的 procOrder 通过回放器的 get_last_tick 取价，因此用 r111 的纯内存
 * TickLoader 构造已 prepare 的 tick 回放器（stime=0930 早于首笔 0931，避免
 * get_last_tick 的末笔边界，见 bugs/R151.md BUG-R151-001）。
 *
 * 只走公开 API；不 #define private public、不镜像类布局、不改宏。
 * 不启动线程/不读挂钟；临时文件由夹具在系统临时目录创建并清理。
 */
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>

#include <Includes/WTSContractInfo.hpp>
#include <WtBtCore/CtaMocker.h>
#include <WtBtCore/HftMocker.h>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/SelMocker.h>
#include <WtBtCore/UftMocker.h>
#include <WtBtCore/WtHelper.h>

#include "r111_common.h"
#include "r151_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
	const char* const kCu = "SHFE.cu.2406";
	const char* const kAg = "SHFE.ag.2412";
	const char* const kStk = "SSE.STK.000001";
}

class R151CtaSelEdges : public r62::ReplayFixture
{
protected:
	void feed(CtaMocker& m, const char* code, double px)
	{
		WTSTickData* t = r62::make_tick(code, 20200102u, 93100000u, px, px - 1.0, px + 1.0);
		m.handle_tick(code, t, 0);
		t->release();
	}

	void feed(SelMocker& m, const char* code, double px)
	{
		WTSTickData* t = r62::make_tick(code, 20200102u, 93000000u, px, px - 0.01, px + 0.01);
		m.handle_tick(code, t, 0);
		t->release();
	}
};

// CtaMocker 比率滑点：开仓 3800 -> 3800 + round(10*3800/10000/pricetick)*pricetick
TEST_F(R151CtaSelEdges, CtaRatioSlippageOpenAndClose)
{
	WTSCommodityInfo* comm = _rp.get_commodity_info(kRb);
	ASSERT_NE(nullptr, comm);
	const double price_tick = comm->getPriceTick();
	ASSERT_GT(price_tick, 0.0);

	CtaMocker m(&_rp, "r151_cta_ratio", 10, false, nullptr, true);
	m.stra_set_position(kRb, 1.0, "open");
	feed(m, kRb, 3800.0);

	ASSERT_DOUBLE_EQ(1.0, m.stra_get_position(kRb));
	const double expected = 3800.0 + std::round(10.0 * 3800.0 / 10000.0 / price_tick) * price_tick;
	EXPECT_DOUBLE_EQ(expected, m.stra_get_last_enterprice(kRb));

	// 平仓同样走比率滑点分支
	m.stra_set_position(kRb, 0.0, "close");
	feed(m, kRb, 3810.0);
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_position(kRb));
}

// SelMocker 比率滑点：100 股 @10.0、slippage=100bp -> 10.0 + round(0.1/pricetick)*pricetick
TEST_F(R151CtaSelEdges, SelRatioSlippageOpenAndClose)
{
	WTSCommodityInfo* comm = _rp.get_commodity_info(kStk);
	ASSERT_NE(nullptr, comm);
	const double price_tick = comm->getPriceTick();
	ASSERT_GT(price_tick, 0.0);

	SelMocker m(&_rp, "r151_sel_ratio", 100, true);
	m.stra_set_position(kStk, 100.0, "open");
	feed(m, kStk, 10.0);

	ASSERT_DOUBLE_EQ(100.0, m.stra_get_position(kStk));
	const double expected = 10.0 + std::round(100.0 * 10.0 / 10000.0 / price_tick) * price_tick;
	EXPECT_DOUBLE_EQ(expected, m.stra_get_last_enterprice(kStk));

	m.stra_set_position(kStk, 0.0, "close");
	feed(m, kStk, 10.5);
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_position(kStk));
}

// Hft/Uft 的 procOrder 依赖回放器 get_last_tick：纯内存 tick，stime 早于首笔
class R151HftUftEdges : public ::testing::Test
{
protected:
	HisDataReplayer _rp;
	r111::TickLoader _loader;
	r151::Sink _sink;
	std::filesystem::path _root;
	std::filesystem::path _base;
	std::filesystem::path _out;

	void SetUp() override
	{
		_root = r62::repo_root();
		ASSERT_FALSE(_root.empty());
		_base = r62::make_temp_dir("r151_tick_base");
		_out = r62::make_temp_dir("r151_tick_out");
		WtHelper::setOutputDir(_out.string().c_str());

		// 09:31 的首笔 tick；stime=09:30 让 get_last_tick 的 lower_bound 落在首笔
		_loader.add(kRb, 20200102u, 93100000u, 3800.0, 3799.0, 3801.0, 100, 100);
		_loader.add(kCu, 20200102u, 93100000u, 70000.0, 69990.0, 70010.0, 100, 3);
		_loader.add(kAg, 20200102u, 93100000u, 3500.0, 3499.0, 0.0, 100, 100);

		WTSVariant* cfg = r111::make_tick_cfg(_root, _base, 202001020930ull, 202001021500ull);
		ASSERT_TRUE(_rp.init(cfg, nullptr, &_loader));
		cfg->release();
		_rp.register_sink(&_sink, "r151_hft_uft");
		ASSERT_TRUE(_rp.prepare());
	}

	void TearDown() override
	{
		WtHelper::setOutputDir("");
		std::error_code ec;
		std::filesystem::remove_all(_base, ec);
		std::filesystem::remove_all(_out, ec);
	}

	void feed_hft(HftMocker& m, const char* code, double last, double bid, double ask,
		uint32_t bidQty, uint32_t askQty)
	{
		WTSTickData* t = r62::make_tick(code, 20200102u, 93100000u, last, bid, ask, bidQty, askQty);
		m.handle_tick(code, t, 0);
		t->release();
	}

	void feed_uft(UftMocker& m, const char* code, double last, double bid, double ask,
		uint32_t bidQty, uint32_t askQty)
	{
		WTSTickData* t = r62::make_tick(code, 20200102u, 93100000u, last, bid, ask, bidQty, askQty);
		m.handle_tick(code, t, 0);
		t->release();
	}
};

// 卖限价 4000 高于买一 3799：procOrder 不成交；随后撤单任务把 left 置 0
TEST_F(R151HftUftEdges, SellLimitNoFillThenCancelDrains)
{
	HftMocker m(&_rp, "r151_hft_limit");
	OrderIDs ids = m.stra_sell(kRb, 4000.0, 1.0, "limit");
	ASSERT_EQ(1u, ids.size());
	EXPECT_DOUBLE_EQ(-1.0, m.stra_get_undone(kRb));

	feed_hft(m, kRb, 3800.0, 3799.0, 3801.0, 100, 100);
	EXPECT_DOUBLE_EQ(-1.0, m.stra_get_undone(kRb));

	EXPECT_TRUE(m.stra_cancel(ids[0]));
	feed_hft(m, kRb, 3800.0, 3799.0, 3801.0, 100, 100);
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_undone(kRb));
}

// 对手价 askprice(0)==0：procOrder 释放 tick 后返回 false，订单保留
TEST_F(R151HftUftEdges, HftZeroCounterPriceRejectsFill)
{
	HftMocker m(&_rp, "r151_hft_zeropx");
	OrderIDs ids = m.stra_buy(kAg, 3900.0, 1.0, "zeropx");
	ASSERT_EQ(1u, ids.size());
	EXPECT_DOUBLE_EQ(1.0, m.stra_get_undone(kAg));

	feed_hft(m, kAg, 3500.0, 3499.0, 0.0, 100, 100);
	EXPECT_DOUBLE_EQ(1.0, m.stra_get_undone(kAg));
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_position(kAg));
}

// Uft 部分成交：对手量 3 < 委托 10 -> 每笔成交 3，订单保留；第 4 笔补满后摘单
TEST_F(R151HftUftEdges, UftPartialFillKeepsOrderUntilFilled)
{
	UftMocker m(&_rp, "r151_uft_partial");
	OrderIDs ids = m.stra_buy(kCu, 0.0, 10.0);
	ASSERT_EQ(1u, ids.size());
	EXPECT_DOUBLE_EQ(10.0, m.stra_get_undone(kCu));

	feed_uft(m, kCu, 70000.0, 69990.0, 70010.0, 100, 3);
	EXPECT_DOUBLE_EQ(3.0, m.stra_get_position(kCu));
	EXPECT_DOUBLE_EQ(7.0, m.stra_get_undone(kCu));

	feed_uft(m, kCu, 70000.0, 69990.0, 70010.0, 100, 3);
	EXPECT_DOUBLE_EQ(6.0, m.stra_get_position(kCu));

	feed_uft(m, kCu, 70000.0, 69990.0, 70010.0, 100, 3);
	EXPECT_DOUBLE_EQ(9.0, m.stra_get_position(kCu));

	feed_uft(m, kCu, 70000.0, 69990.0, 70010.0, 100, 3);
	EXPECT_DOUBLE_EQ(10.0, m.stra_get_position(kCu));
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_undone(kCu));
}
