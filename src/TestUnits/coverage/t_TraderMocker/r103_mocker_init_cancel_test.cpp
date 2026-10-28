/**
 * r103_mocker_init_cancel_test.cpp  (lane R103 · wave18)
 *
 * 靶子：TraderMocker 的公开 init/connect + 回调分发 + 无 listener 的撤单守卫
 *   - load_positions 的 JSON 解析失败即返回            TraderMocker.cpp:549-560
 *   - orderAction 对空 listener 的 write_log 提前返回   :24-32、750-772
 *   - 无 listener 时 orderInsert 仍建表/落盘            :268-312
 *
 * 不调 login，不进入私有 match_once。
 */
#include "r103_mocker_common.h"

using R103Mocker = r103m::MockerTest;

/*
 * 生产: TraderMocker.cpp:549-560（畸形 positions.json 的 HasParseError 返回）
 *       689-703（connect 的 load_positions + handleEvent）
 */
TEST_F(R103Mocker, R103Mocker_InitMalformedPositionsIgnored)
{
	init_api(9701);
	write_positions(9701, "{\"positions\":[ this is not json");

	connect_api();
	EXPECT_EQ(1u, sink_.n_event());

	ASSERT_EQ(0, api_.queryPositions());
	ASSERT_TRUE(r103m::wait_until([this]() { return sink_.n_position() >= 1u; }));
	EXPECT_TRUE(sink_.last_positions().empty());
}

/*
 * 生产: TraderMocker.cpp:24-32（sink==NULL 的 write_log 直接返回）
 *       146-151（未注册 listener 时仍按 contractInfo 下单）
 *       268-312（成功订单建 _orders/_awaits 并 save_positions）
 *       750-772（订单不存在：write_log(NULL) + 无 listener 错误回报）
 */
TEST_F(R103Mocker, R103Mocker_NullListenerOrderAndCancelGuard)
{
	TraderMocker silent;
	WTSVariant* cfg = r103m::make_cfg(9702);
	ASSERT_TRUE(silent.init(cfg));
	cfg->release();
	silent.connect();

	WTSEntrust* entrust = r103m::make_entrust("ag2512", "SHFE", ag_ct_,
		7000.0, 2.0, WDT_LONG, WOT_OPEN);
	ASSERT_EQ(0, silent.orderInsert(entrust));
	entrust->release();

	const std::filesystem::path posFile = "mocker_9702/positions.json";
	ASSERT_TRUE(r103m::wait_until([&posFile]() { return std::filesystem::exists(posFile); }));

	WTSEntrustAction* action = WTSEntrustAction::create();
	action->setOrderID("mo.9702.99999999");
	action->setEntrustID("r103-entrust");
	ASSERT_EQ(0, silent.orderAction(action));
	action->release();

	// orderAction 与 orderInsert 在同一 io_context 串行；给 handler 一个短窗口后收线程
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	silent.release();
}
