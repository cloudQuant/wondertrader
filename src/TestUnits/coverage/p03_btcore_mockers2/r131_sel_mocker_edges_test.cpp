/**
 * r131_sel_mocker_edges_test.cpp  (lane R131 · wave21)
 *
 * 靶子：src/WtBtCore/SelMocker.cpp 的公开查询空明细分支
 *   - stra_get_first_entertime（1021-1023）：_pos_map 有该合约但 details 已被全平清空
 *   - stra_get_last_entertag（1047-1049）：同上
 *
 * 用期货合约（非 T+1）走 stra_set_position 的「开 -> 全平」公开链路：
 * 全平后 do_set_position 会把已平完的 DetailInfo erase（SelMocker.cpp:779-784），
 * 但 _pos_map 仍保留 0 量条目，正好命中两个空明细早退。
 */
#include <gtest/gtest.h>

#include <Includes/WTSContractInfo.hpp>
#include <WtBtCore/SelMocker.h>

#include "r131_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
}

class R131SelMocker : public r62::ReplayFixture
{
};

// 生产: SelMocker.cpp:1021-1023、1047-1049
TEST_F(R131SelMocker, FlatPositionHasNoEntryTimeOrTag)
{
	SelMocker m(&_rp, "r131_sel_flat", 0, false);

	// 开 1 手（rb 非 T+1，不会触发 frozen 拒绝），tick 触发成交
	m.stra_set_position(kRb, 1.0, "in");
	r131::feed(m, kRb, 3800.0, 3799.0, 3801.0);
	ASSERT_DOUBLE_EQ(1.0, m.stra_get_position(kRb));

	// 全平，tick 触发平仓并清空 details
	m.stra_set_position(kRb, 0.0, "out");
	r131::feed(m, kRb, 3810.0, 3809.0, 3811.0);
	ASSERT_DOUBLE_EQ(0.0, m.stra_get_position(kRb));

	// 0 量条目仍在 _pos_map，但 details 为空 -> 两个查询都走空明细早退
	EXPECT_EQ(0u, m.stra_get_first_entertime(kRb));
	EXPECT_STREQ("", m.stra_get_last_entertag(kRb));
}
