/**
 * r103_mocker_udp_packet_test.cpp  (lane R103 · wave18)
 *
 * 靶子：TraderMocker UDP 接收链的公开可达分支（connect + 本机回环）
 *   - handle_read 忽略空 UDP 包后仍重挂接收               TraderMocker.cpp:950-965
 *   - extract_buffer 的长度检查、未知包头及 tick 包分支  :990-1022
 *   - 析构对 _ticks 的释放                               :98-107
 *
 * 只向 127.0.0.1 的临时端口发包；tick 包体使用公开的 WTSTickStruct。
 * 通过公开 login() 启动撮合线程，以空包后的两笔订单状态推送证明接收持续工作。
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "Includes/FasterDefs.h"
#include "Includes/ITraderApi.h"
#include "Share/StdUtils.hpp"
#include "Includes/WTSCollection.hpp"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"

#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/executor_work_guard.hpp>

#define private public
#include "TraderMocker/TraderMocker.h"
#undef private

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

TEST(R103MockerUdpPacket, ShortFramesDoNotReplaceTheLastCompleteTick)
{
	TraderMocker mocker;
	const std::string fullCode = "SHFE.ag2512";
	mocker._codes.insert(fullCode);

	TickPacket firstPacket;
	memset(&firstPacket, 0, sizeof(firstPacket));
	firstPacket.type = 0x200u;
	strcpy(firstPacket.data.exchg, "SHFE");
	strcpy(firstPacket.data.code, "ag2512");
	firstPacket.data.price = 7000.0;
	firstPacket.data.action_date = 20200102u;
	firstPacket.data.trading_date = 20200102u;
	firstPacket.data.action_time = 100000000u;

	memcpy(mocker._b_buffer.data(), &firstPacket, sizeof(firstPacket));
	mocker.extract_buffer((uint32_t)sizeof(firstPacket), true);
	ASSERT_NE(nullptr, mocker._ticks);
	WTSObject* originalTick = mocker._ticks->get(fullCode);
	ASSERT_NE(nullptr, originalTick);
	EXPECT_DOUBLE_EQ(7000.0, static_cast<WTSTickData*>(originalTick)->price());
	EXPECT_EQ(100000000u, static_cast<WTSTickData*>(originalTick)->actiontime());
	const uintptr_t originalTickAddress = reinterpret_cast<uintptr_t>(originalTick);

	const std::size_t shortLengths[] = { 1u, 2u, 3u, sizeof(firstPacket.type) };
	for (std::size_t length : shortLengths)
	{
		// Each simulated UDP datagram overwrites only its received bytes; the rest
		// of the receive buffer retains the preceding complete, subscribed tick.
		memcpy(mocker._b_buffer.data(), &firstPacket, sizeof(firstPacket));
		memcpy(mocker._b_buffer.data(), &firstPacket, length);
		const uintptr_t cachedBeforeAddress = reinterpret_cast<uintptr_t>(mocker._ticks->get(fullCode));
		mocker.extract_buffer((uint32_t)length, true);
		EXPECT_EQ(cachedBeforeAddress,
			reinterpret_cast<uintptr_t>(mocker._ticks->get(fullCode)))
			<< "short frame length=" << length << " replaced the last complete tick";
	}

	TickPacket secondPacket = firstPacket;
	secondPacket.data.price = 6999.0;
	secondPacket.data.action_time = 100001000u;
	memcpy(mocker._b_buffer.data(), &secondPacket, sizeof(secondPacket));
	mocker.extract_buffer((uint32_t)sizeof(secondPacket), true);
	WTSObject* replacementTick = mocker._ticks->get(fullCode);
	ASSERT_NE(nullptr, replacementTick);
	EXPECT_NE(originalTickAddress, reinterpret_cast<uintptr_t>(replacementTick));
	EXPECT_DOUBLE_EQ(6999.0, static_cast<WTSTickData*>(replacementTick)->price());
	EXPECT_EQ(100001000u, static_cast<WTSTickData*>(replacementTick)->actiontime());
}

/*
 * 生产: TraderMocker.cpp:935-965（handle_read 成功/空包重挂路径）
 *       990-1003（短头与短 tick 帧被拒绝）
 *       1005-1020（完整 UDP_MSG_PUSHTICK 建 _ticks 缓存）
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

	// 合法 tick 包：exchg/code 与已下单合约一致，并提供足够的卖盘数量供开仓撮合。
	TickPacket packet;
	memset(&packet, 0, sizeof(packet));
	packet.type = 0x200u;
	strcpy(packet.data.exchg, "SHFE");
	strcpy(packet.data.code, "ag2512");
	packet.data.price = 7000.0;
	packet.data.ask_prices[0] = 7000.0;
	packet.data.ask_qty[0] = 2.0;
	packet.data.action_date = 20200102u;
	packet.data.trading_date = 20200102u;
	packet.data.action_time = 100000000u;

	ASSERT_EQ(0, api_.login("", "", ""));
	// 空数据报后立即发送合法行情。只有接收链仍然挂起，该行情才会到达撮合线程并完成订单。
	ASSERT_TRUE(r103m::send_udp(port, "", 0));
	ASSERT_TRUE(r103m::send_udp(port, &packet, sizeof(packet)));
	ASSERT_TRUE(r103m::wait_until([this, beforePush]() {
		return sink_.n_push_order() >= beforePush + 2u;
	}));

	// 第一笔成交后再挂第二笔订单，并用更低限价排除第一条 Tick 对它的撮合影响。
	const uint32_t afterFirstFill = sink_.n_push_order();
	WTSEntrust* second = r103m::make_entrust("ag2512", "SHFE", ag_ct_,
		6900.0, 1.0, WDT_LONG, WOT_OPEN);
	ASSERT_EQ(0, api_.orderInsert(second));
	second->release();
	ASSERT_TRUE(r103m::wait_until([this, afterFirstFill]() {
		return sink_.n_push_order() >= afterFirstFill + 1u;
	}));

	// 这个查询只充当同一 io_context 上的顺序屏障，确保第二笔委托已进入撮合表。
	// 行情是否仍可接收只由下面第二笔订单是否成交判断。
	const uint32_t beforeAccountBarrier = sink_.n_account();
	ASSERT_EQ(0, api_.queryAccount());
	ASSERT_TRUE(r103m::wait_until([this, beforeAccountBarrier]() {
		return sink_.n_account() >= beforeAccountBarrier + 1u;
	}));

	TickPacket secondPacket = packet;
	secondPacket.data.price = 6900.0;
	secondPacket.data.ask_prices[0] = 6900.0;
	secondPacket.data.action_time = 100001000u;
	const uint32_t beforeTruncatedTick = sink_.n_push_order();
	ASSERT_TRUE(r103m::send_udp(port, &secondPacket, sizeof(secondPacket) - 1u));
	EXPECT_FALSE(r103m::wait_until([this, beforeTruncatedTick]() {
		return sink_.n_push_order() > beforeTruncatedTick;
	}, 250));

	ASSERT_TRUE(r103m::send_udp(port, &secondPacket, sizeof(secondPacket)));
	ASSERT_TRUE(r103m::wait_until([this, beforeTruncatedTick]() {
		return sink_.n_push_order() >= beforeTruncatedTick + 1u;
	}));

	api_.release();
}
