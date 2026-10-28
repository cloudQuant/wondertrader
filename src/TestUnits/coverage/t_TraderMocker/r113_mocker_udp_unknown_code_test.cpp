/**
 * r113_mocker_udp_unknown_code_test.cpp  (lane R113 · wave19)
 *
 * 靶子：UDP tick 包在「合约未进 _codes」时的丢弃支
 *   - extract_buffer 的 _codes.find 未命中 return     TraderMocker.cpp:985-996
 *
 * 与 R103 的「未知包头」不同：这里是合法 0x200 包 + 未知合约代码，
 * 必须走到 996 行的早退，且不能污染 _ticks，io 线程仍可继续查询。
 */
#include "r113_mocker_common.h"

using R113MockerUdp = r113m::MockerTest;

/*
 * 生产: TraderMocker.cpp:985-996（合法包头、fullcode 不在 _codes 即 return）
 *       935-962（handle_read 继续 arm 接收链）
 */
TEST_F(R113MockerUdp, R113Mocker_UnknownFullCodeTickIsIgnoredAndIoSurvives)
{
	const uint16_t port = r103m::pick_free_udp_port();
	ASSERT_NE(0, port);
	set_udp_port((int)port);

	init_api(9814, (int)port);
	connect_api();

	/* 未下过任何单：_codes 为空，合法 tick 包应在 _codes.find 处被丢弃 */
	send_tick(7000.0, 7000.0, 5.0, 7000.0, 5.0, "ag2599", "SHFE");

	/* 给 io 线程一个确定性的完成点：queryAccount 走同一 io_context 串行队列 */
	ASSERT_EQ(0, api_.queryAccount());
	ASSERT_TRUE(r113m::wait_until([this]() { return sink_.n_account() >= 1u; }));

	EXPECT_EQ(0u, sink_.n_push_order());
	EXPECT_EQ(0u, sink_.n_trade());
	EXPECT_EQ(0u, sink_.n_error());
}
