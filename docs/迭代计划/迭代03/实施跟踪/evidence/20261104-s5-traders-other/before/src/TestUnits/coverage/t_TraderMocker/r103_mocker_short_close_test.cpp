/**
 * r103_mocker_short_close_test.cpp  (lane R103 · wave18)
 *
 * 靶子：TraderMocker 的空头平仓冻结与撤单释放（公开 orderInsert/orderAction/queryPositions）
 *   - orderInsert 冻结空头持仓                        TraderMocker.cpp:233-261
 *   - orderAction 释放空头冻结                        :787-805
 *   - queryPositions 的 short 臂与 avail              :888-897
 *
 * 不调 login，不进入私有 match_once。
 */
#include "r103_mocker_common.h"

using R103Mocker = r103m::MockerTest;

namespace
{
	const r103m::PosRec* find_short(const std::vector<r103m::PosRec>& recs)
	{
		for (const r103m::PosRec& rec : recs)
		{
			if (rec.direction == (int)WDT_SHORT)
				return &rec;
		}
		return NULL;
	}
}

/*
 * 生产: TraderMocker.cpp:258-261（空头冻结）
 *       787-805（非开仓空头撤单释放冻结）
 *       862-909（queryPositions 空头回报）
 */
TEST_F(R103Mocker, R103Mocker_ShortCloseFreezeAndCancelRelease)
{
	init_api(9703);
	write_positions(9703,
		"{\"positions\":[{\"exchg\":\"SHFE\",\"code\":\"ag2512\","
		"\"long\":{\"volume\":0},\"short\":{\"volume\":5}}]}");
	connect_api();

	ASSERT_EQ(0, api_.queryPositions());
	ASSERT_TRUE(r103m::wait_until([this]() { return sink_.n_position() >= 1u; }));
	{
		const std::vector<r103m::PosRec> recs = sink_.last_positions();
		ASSERT_EQ(1u, recs.size());
		const r103m::PosRec* rec = find_short(recs);
		ASSERT_NE(nullptr, rec);
		EXPECT_DOUBLE_EQ(5.0, rec->avail);
	}

	const uint32_t beforePush = sink_.n_push_order();
	WTSEntrust* close = r103m::make_entrust("ag2512", "SHFE", ag_ct_,
		7000.0, 4.0, WDT_SHORT, WOT_CLOSE);
	ASSERT_EQ(0, api_.orderInsert(close));
	close->release();
	ASSERT_TRUE(r103m::wait_until([this, beforePush]() {
		return sink_.n_push_order() >= beforePush + 1u;
	}));

	ASSERT_EQ(0, api_.queryPositions());
	ASSERT_TRUE(r103m::wait_until([this]() { return sink_.n_position() >= 2u; }));
	{
		const std::vector<r103m::PosRec> recs = sink_.last_positions();
		const r103m::PosRec* rec = find_short(recs);
		ASSERT_NE(nullptr, rec);
		EXPECT_DOUBLE_EQ(5.0, rec->new_pos);
		EXPECT_DOUBLE_EQ(1.0, rec->avail);	// 冻结 4 手
	}

	const std::string oid = sink_.last_order_id();
	ASSERT_FALSE(oid.empty());
	const uint32_t beforeCancel = sink_.n_push_order();
	WTSEntrustAction* action = WTSEntrustAction::create();
	action->setOrderID(oid.c_str());
	action->setEntrustID("r103-entrust");
	ASSERT_EQ(0, api_.orderAction(action));
	action->release();
	ASSERT_TRUE(r103m::wait_until([this, beforeCancel]() {
		return sink_.n_push_order() >= beforeCancel + 1u;
	}));

	ASSERT_EQ(0, api_.queryPositions());
	ASSERT_TRUE(r103m::wait_until([this]() { return sink_.n_position() >= 3u; }));
	{
		const std::vector<r103m::PosRec> recs = sink_.last_positions();
		const r103m::PosRec* rec = find_short(recs);
		ASSERT_NE(nullptr, rec);
		EXPECT_DOUBLE_EQ(5.0, rec->avail);	// 撤单释放冻结
	}
}
