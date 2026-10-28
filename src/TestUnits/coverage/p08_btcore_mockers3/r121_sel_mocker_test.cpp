/**
 * r121_sel_mocker_test.cpp  (lane R121)
 *
 * 靶子：src/WtBtCore/SelMocker.cpp
 *   do_set_position 的比率滑点分支（L680-689）：fill =
 *   trdPx + round(slippage*trdPx/10000/pricetick)*pricetick。
 *
 * 只走公开的 stra_set_position + handle_tick，不触碰 init_sel_factory（本机无
 * libWtSelStraFact 构建产物）与 R111 已登记的崩溃面。
 */
#include <gtest/gtest.h>

#include <cmath>

#include <Includes/WTSContractInfo.hpp>
#include <WtBtCore/SelMocker.h>

#include "r121_common.h"

namespace
{
	const char* const kStk = "SSE.STK.000001";
}

class R121SelMocker : public r62::ReplayFixture
{
protected:
	void feed(SelMocker& m, const char* code, double px)
	{
		WTSTickData* t = r62::make_tick(code, 20240102u, 93000000u, px, px - 0.01, px + 0.01);
		m.handle_tick(code, t, 0);
		t->release();
	}
};

// 100 股 @10.0、slippage=100bp、pricetick=0.01 -> 成交价 10.1
TEST_F(R121SelMocker, RatioSlippageFill)
{
	WTSCommodityInfo* comm = _rp.get_commodity_info(kStk);
	ASSERT_NE(nullptr, comm);
	const double price_tick = comm->getPriceTick();
	ASSERT_GT(price_tick, 0.0);

	SelMocker m(&_rp, "r121_sel_ratio", 100, true);
	m.stra_set_position(kStk, 100.0, "ratio");
	feed(m, kStk, 10.0);

	ASSERT_DOUBLE_EQ(100.0, m.stra_get_position(kStk));
	const double expected_slip = std::round(100.0 * 10.0 / 10000.0 / price_tick) * price_tick;
	EXPECT_DOUBLE_EQ(10.0 + expected_slip, m.stra_get_last_enterprice(kStk));
	EXPECT_GT(m.stra_get_last_enterprice(kStk), 10.0);
}
