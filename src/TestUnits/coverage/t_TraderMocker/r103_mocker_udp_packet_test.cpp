/**
 * r103_mocker_udp_packet_test.cpp  (lane R103 · wave18)
 *
 * 靶子：TraderMocker UDP 接收链的公开可达分支（connect + 本机回环）
 *   - handle_read 的 bytes_transferred<=0 早退           TraderMocker.cpp:950-951
 *   - extract_buffer 的未知包头 / tick 包分支            :985-1006
 *   - 析构对 _ticks 的释放                               :98-107
 *
 * 只向 127.0.0.1 的临时端口发包；tick 包体使用公开的 WTSTickStruct。
 * 不调 login，不进入私有 match_once；release() 停止并 join io 线程。
 */
#include "r103_mocker_common.h"

using R103Mocker = r103m::MockerTest;

namespace
{
#pragma pack(push, 1)
	struct TickPacket
	{
		uint32_t type;
		WTSTickStruct data;
	};
#pragma pack(pop)
}

/*
 * 生产: TraderMocker.cpp:935-962（handle_read 成功/空包路径）
 *       985-996（未知 _type 被忽略）
 *       997-1006（UDP_MSG_PUSHTICK 建 _ticks 缓存）
 *       98-107（析构释放 _ticks）
 */
TEST_F(R103Mocker, R103Mocker_UdpUnknownTickAndEmptyDatagram)
{
	const uint16_t port = r103m::pick_free_udp_port();
	ASSERT_NE(0, port);
	init_api(9704, (int)port);
	connect_api();

	const uint32_t beforePush = sink_.n_push_order();
	WTSEntrust* open = r103m::make_entrust("ag2512", "SHFE", ag_ct_,
		7000.0, 2.0, WDT_LONG, WOT_OPEN);
	ASSERT_EQ(0, api_.orderInsert(open));
	open->release();
	ASSERT_TRUE(r103m::wait_until([this, beforePush]() {
		return sink_.n_push_order() >= beforePush + 1u;
	}));

	// 未知包头（4 字节）
	for (int i = 0; i < 3; i++)
	{
		const uint32_t word = 0x12345678u;
		ASSERT_TRUE(r103m::send_udp(port, &word, sizeof(word)));
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	// 合法 tick 包：exchg/code 与已下单合约一致
	TickPacket packet;
	memset(&packet, 0, sizeof(packet));
	packet.type = 0x200u;
	strcpy(packet.data.exchg, "SHFE");
	strcpy(packet.data.code, "ag2512");
	packet.data.price = 7000.0;
	packet.data.action_date = 20200102u;
	packet.data.trading_date = 20200102u;
	packet.data.action_time = 100000000u;
	for (int i = 0; i < 3; i++)
	{
		ASSERT_TRUE(r103m::send_udp(port, &packet, sizeof(packet)));
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	// io 线程仍存活：queryAccount 有回报
	ASSERT_EQ(0, api_.queryAccount());
	ASSERT_TRUE(r103m::wait_until([this]() { return sink_.n_account() >= 1u; }));
	const uint32_t accounts = sink_.n_account();

	// 空数据报触发 bytes_transferred<=0 的早退（之后不再 rearm，但 io_context 仍可查询）
	ASSERT_TRUE(r103m::send_udp(port, "", 0));
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	ASSERT_EQ(0, api_.queryAccount());
	ASSERT_TRUE(r103m::wait_until([this, accounts]() {
		return sink_.n_account() >= accounts + 1u;
	}));

	api_.release();
}
