// Lane R195 - 头文件内联面：由生产 WtDataWriter::writeTick 驱动 WTSSessionInfo。
//
// 关键口径：不直接调用 WTSSessionInfo；所有分支由
//   WtDataWriter::writeTick -> updateCache / pipeToKlines（生产 TU WtDataWriter.cpp）
// 内部对 timeToMinutes / minuteToTime / getOffsetDate / isInAuctionTime 的调用触达。
//
// 覆盖目标（src/Includes/WTSSessionInfo.hpp 当前 HEAD）：
//   - 150        timeToMinutes 命中集合竞价 -> 0
//   - 194        timeToMinutes 不在任何交易小节 -> INVALID_UINT32（writer 跳过 K 线）
//   - 484-498    isInAuctionTime 的 auction-first<=offTime<second 命中
//   - 129        getOffsetDate 跨日（夜盘 offset 后 >= 1440）
//   - 224-242    minuteToTime 跨小节/收盘边界的偏移扣减与 getCloseTime 收尾
//   - 530-542    originalTime 带 offset 的还原
// 线程纪律：复用 r23 夹具，writer init 起的 check_loop 不可 join，故只 release 不 delete。
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "Includes/WTSSessionInfo.hpp"
#include "WtDataStorage/WtDataWriter.h"

#include "../r23_storage4/r23_writer_fixture.h"

USING_NS_WTP;

namespace {

class R195WriterSessionEdgeTest : public r23::R23WriterTest
{
};

}	// namespace

// 日盘 session 加集合竞价 09:15-09:25：
//  - 09:20 tick 让 timeToMinutes 命中 isInAuctionTime，返回 0，K 线仍能落盘
//  - 12:00 tick 不在任何小节，timeToMinutes 返回 INVALID_UINT32，writer 只告警不建 bar
TEST_F(R195WriterSessionEdgeTest, auction_tick_and_lunch_break_tick)
{
	WTSSessionInfo* day = _bd->getSession("R23_DAY");
	ASSERT_NE(day, nullptr);
	day->setAuctionTime(915, 925);

	r23::R23Writer* w = make_writer();
	ASSERT_NE(w, nullptr);

	r23::TickSpec auction;
	auction.action_time = 92000000;	// 09:20:00.000
	auction.price = 3601.0;
	auction.total_volume = 1000.0;
	write_tick(w, auction);

	WTSTickData* cached = w->getCurTick("rb2210", "SHFE");
	ASSERT_NE(cached, nullptr);
	EXPECT_DOUBLE_EQ(cached->price(), 3601.0);
	cached->release();
	// 集合竞价 tick 仍应闭合出一根 09:01 的分钟线
	EXPECT_TRUE(r23::file_exists(rt_min1_file("rb2210")));

	r23::TickSpec lunch;
	lunch.action_time = 12000000;	// 12:00:00.000，非交易时间
	lunch.price = 3602.0;
	lunch.total_volume = 1010.0;
	write_tick(w, lunch);

	WTSTickData* cached2 = w->getCurTick("rb2210", "SHFE");
	ASSERT_NE(cached2, nullptr);
	EXPECT_DOUBLE_EQ(cached2->price(), 3602.0);
	cached2->release();
	// pipeToKlines 的 "is invalid timestamp, skip this tick" 分支
	EXPECT_TRUE(_sink->has_log("is invalid timestamp", LL_WARN));
}

// 夜盘 session（offset=180，小节 21:00-23:00）：
//  - 23:00 的 action_date 是 20240108，getOffsetDate 加 offset 后越过 1440 -> 返回 20240109
//  - minuteToTime 从 21:00 起算越过小节末尾，走 offset 扣减与 getCloseTime 收尾
TEST_F(R195WriterSessionEdgeTest, night_session_cross_day_offset_and_close_time)
{
	r23::R23Writer* w = make_writer();
	ASSERT_NE(w, nullptr);

	r23::TickSpec spec;
	spec.code = "fn2201";
	spec.price = 2701.0;
	spec.total_volume = 2000.0;
	spec.turn_over = 27010.0;
	spec.action_date = 20240108;
	spec.trading_date = 20240109;
	spec.action_time = 23000000;	// 23:00:00.000
	write_tick(w, spec);

	WTSTickData* cached = w->getCurTick("fn2201", "SHFE");
	ASSERT_NE(cached, nullptr);
	EXPECT_DOUBLE_EQ(cached->price(), 2701.0);
	cached->release();

	// 第二笔同一交易日、同一时刻但总量更大：触发 updateCache 的 getOffsetDate 分支
	r23::TickSpec same;
	same.code = "fn2201";
	same.price = 2702.0;
	same.total_volume = 2010.0;
	same.turn_over = 27020.0;
	same.action_date = 20240108;
	same.trading_date = 20240109;
	same.action_time = 23000000;
	write_tick(w, same);

	WTSTickData* cached2 = w->getCurTick("fn2201", "SHFE");
	ASSERT_NE(cached2, nullptr);
	EXPECT_DOUBLE_EQ(cached2->price(), 2702.0);
	cached2->release();
	EXPECT_TRUE(r23::file_exists(rt_min1_file("fn2201")));
}
