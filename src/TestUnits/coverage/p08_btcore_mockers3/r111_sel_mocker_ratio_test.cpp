/**
 * r111_sel_mocker_ratio_test.cpp  (lane R111)
 *
 * 靶子：src/WtBtCore/SelMocker.cpp 比率滑点分支
 *   do_set_position 的 _ratio_slippage（L681-689）
 *   明细查询未命中（L1123、L1142）
 *
 * 无策略工厂需求：SelMocker 的信号→tick 成交链不依赖 _strategy。
 * 临时文件由 r62 fixture 写入系统临时目录并清理。
 */
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

#include <Includes/WTSContractInfo.hpp>
#include <WtBtCore/SelMocker.h>

#include "r111_common.h"

namespace
{
	const char* const kStk = "SSE.STK.000001";
}

class R111SelMocker : public r62::ReplayFixture
{
};

// ratio=true：滑点按成交价百分比计算，再按 pricetick 取整（L681-689）
TEST_F(R111SelMocker, RatioSlippageAndMissedDetailQueries)
{
	WTSCommodityInfo* comm = _rp.get_commodity_info(kStk);
	ASSERT_NE(nullptr, comm);
	const double price_tick = comm->getPriceTick();
	ASSERT_GT(price_tick, 0.0);

	SelMocker sel(&_rp, "r111_sel_ratio", 100, true);
	sel.stra_set_position(kStk, 100.0, "ratio");

	WTSTickData* tick = r62::make_tick(kStk, 20200102u, 93100000u, 10.0, 9.99, 10.01);
	sel.handle_tick(kStk, tick, 0);
	tick->release();

	ASSERT_DOUBLE_EQ(100.0, sel.stra_get_position(kStk, false));
	const double expected_slip = std::round(100.0 * 10.0 / 10000.0 / price_tick) * price_tick;
	EXPECT_DOUBLE_EQ(10.0 + expected_slip, sel.stra_get_last_enterprice(kStk));
	EXPECT_GT(sel.stra_get_last_enterprice(kStk), 10.0);

	// 明细存在但 tag 不命中：循环走完返回 0（L1123、L1142）
	EXPECT_EQ(0ull, sel.stra_get_detail_entertime(kStk, "missing-tag"));
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_detail_cost(kStk, "missing-tag"));
	EXPECT_STREQ("ratio", sel.stra_get_last_entertag(kStk));
}
