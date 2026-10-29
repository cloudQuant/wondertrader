// Lane R53 - src/Includes/WTSStruct.h
// 覆盖：新旧 bar/tick 结构体构造清零、跨版本 operator= 全字段复制、
//       tick 联合体别名（settle/iopv、open_interest/sedimentary、pre_interest/pre_sedimentary）、
//       HFT 三个结构体（委托队列/逐笔委托/逐笔成交）构造清零。
// 说明：全部栈上结构体，不访问共享内存；无 NUL 固定宽字符串的赋值支持合同仍待 R53-003 确认。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "Includes/WTSStruct.h"
#include "Includes/WTSMarcos.h"
#include "Includes/WTSTypes.h"

using namespace wtp;

namespace
{
bool buffer_has_nul(const char* buf, std::size_t len)
{
	return std::memchr(buf, '\0', len) != nullptr;
}
}  // namespace

// WTSStruct.h:26-46、94-146：旧 bar 构造清零 + 新 bar 赋值
TEST(r53_struct_old_bar, ctor_zeroes_and_assignment_copies)
{
	WTSBarStructOld ob;
	EXPECT_EQ(ob.date, 0u);
	EXPECT_EQ(ob.time, 0u);
	EXPECT_DOUBLE_EQ(ob.open, 0.0);
	EXPECT_DOUBLE_EQ(ob.high, 0.0);
	EXPECT_DOUBLE_EQ(ob.low, 0.0);
	EXPECT_DOUBLE_EQ(ob.close, 0.0);
	EXPECT_DOUBLE_EQ(ob.settle, 0.0);
	EXPECT_DOUBLE_EQ(ob.money, 0.0);
	EXPECT_EQ(ob.vol, 0u);
	EXPECT_EQ(ob.hold, 0u);
	EXPECT_EQ(ob.add, 0);

	ob.date = 20190104;
	ob.time = 1330;
	ob.open = 11.0;
	ob.high = 12.0;
	ob.low = 10.0;
	ob.close = 11.5;
	ob.settle = 11.2;
	ob.money = 1234.5;
	ob.vol = 600;
	ob.hold = 700;
	ob.add = 30;

	WTSBarStruct nb;
	EXPECT_DOUBLE_EQ(nb.open, 0.0);	// 新结构体构造清零
	nb = ob;
	EXPECT_EQ(nb.date, 20190104u);
	EXPECT_EQ(nb.time, 1330u);
	EXPECT_DOUBLE_EQ(nb.open, 11.0);
	EXPECT_DOUBLE_EQ(nb.high, 12.0);
	EXPECT_DOUBLE_EQ(nb.low, 10.0);
	EXPECT_DOUBLE_EQ(nb.close, 11.5);
	EXPECT_DOUBLE_EQ(nb.settle, 11.2);
	EXPECT_DOUBLE_EQ(nb.money, 1234.5);
	EXPECT_DOUBLE_EQ(nb.vol, 600.0);
	EXPECT_DOUBLE_EQ(nb.hold, 700.0);
	EXPECT_DOUBLE_EQ(nb.add, 30.0);
	EXPECT_EQ(nb.reserve_, 0u);
}

// WTSStruct.h:48-85：旧 tick 构造清零与字段写入
TEST(r53_struct_old_tick, ctor_zeroes_all_fields)
{
	WTSTickStructOld ot;
	EXPECT_STREQ(ot.exchg, "");
	EXPECT_STREQ(ot.code, "");
	EXPECT_DOUBLE_EQ(ot.price, 0.0);
	EXPECT_DOUBLE_EQ(ot.open, 0.0);
	EXPECT_DOUBLE_EQ(ot.high, 0.0);
	EXPECT_DOUBLE_EQ(ot.low, 0.0);
	EXPECT_DOUBLE_EQ(ot.settle_price, 0.0);
	EXPECT_DOUBLE_EQ(ot.upper_limit, 0.0);
	EXPECT_DOUBLE_EQ(ot.lower_limit, 0.0);
	EXPECT_EQ(ot.total_volume, 0u);
	EXPECT_EQ(ot.volume, 0u);
	EXPECT_DOUBLE_EQ(ot.total_turnover, 0.0);
	EXPECT_DOUBLE_EQ(ot.turn_over, 0.0);
	EXPECT_EQ(ot.open_interest, 0u);
	EXPECT_EQ(ot.diff_interest, 0);
	EXPECT_EQ(ot.trading_date, 0u);
	EXPECT_EQ(ot.action_date, 0u);
	EXPECT_EQ(ot.action_time, 0u);
	EXPECT_DOUBLE_EQ(ot.pre_close, 0.0);
	EXPECT_DOUBLE_EQ(ot.pre_settle, 0.0);
	EXPECT_EQ(ot.pre_interest, 0);
	EXPECT_DOUBLE_EQ(ot.bid_prices[0], 0.0);
	EXPECT_DOUBLE_EQ(ot.ask_prices[9], 0.0);
	EXPECT_EQ(ot.bid_qty[5], 0u);
	EXPECT_EQ(ot.ask_qty[5], 0u);

	wt_strcpy(ot.exchg, "CFFEX");
	wt_strcpy(ot.code, "IF2406");
	ot.price = 3800.0;
	ot.volume = 12;
	ot.bid_prices[0] = 3799.8;
	EXPECT_STREQ(ot.exchg, "CFFEX");
	EXPECT_STREQ(ot.code, "IF2406");
	EXPECT_DOUBLE_EQ(ot.price, 3800.0);
	EXPECT_EQ(ot.volume, 12u);
	EXPECT_DOUBLE_EQ(ot.bid_prices[0], 3799.8);
}

// WTSStruct.h:197-201、158-191：新 tick 构造清零 + 联合体别名
TEST(r53_struct_tick, ctor_zeroes_and_union_aliases)
{
	WTSTickStruct ts;
	EXPECT_STREQ(ts.exchg, "");
	EXPECT_STREQ(ts.code, "");
	EXPECT_DOUBLE_EQ(ts.price, 0.0);
	EXPECT_DOUBLE_EQ(ts.total_volume, 0.0);
	EXPECT_DOUBLE_EQ(ts.open_interest, 0.0);
	EXPECT_DOUBLE_EQ(ts.pre_interest, 0.0);
	EXPECT_EQ(ts.trading_date, 0u);
	EXPECT_EQ(ts.reserve_, 0u);
	EXPECT_DOUBLE_EQ(ts.bid_qty[9], 0.0);

	ts.settle_price = 1.5;
	EXPECT_DOUBLE_EQ(ts.iopv, 1.5);
	ts.iopv = 2.5;
	EXPECT_DOUBLE_EQ(ts.settle_price, 2.5);

	ts.open_interest = 3000.0;
	EXPECT_DOUBLE_EQ(ts.sedimentary, 3000.0);
	ts.sedimentary = 4000.0;
	EXPECT_DOUBLE_EQ(ts.open_interest, 4000.0);

	ts.pre_interest = 2500.0;
	EXPECT_DOUBLE_EQ(ts.pre_sedimentary, 2500.0);
	ts.pre_sedimentary = 2600.0;
	EXPECT_DOUBLE_EQ(ts.pre_interest, 2600.0);
}

// WTSStruct.h:202-240：旧 tick -> 新 tick 全字段复制
TEST(r53_struct_tick, assignment_copies_every_field_and_levels)
{
	WTSTickStructOld ot;
	wt_strcpy(ot.exchg, "CFFEX");
	wt_strcpy(ot.code, "IF2406");
	ot.price = 3800.0;
	ot.open = 3790.0;
	ot.high = 3810.0;
	ot.low = 3780.0;
	ot.settle_price = 3805.0;
	ot.upper_limit = 4000.0;
	ot.lower_limit = 3600.0;
	ot.total_volume = 100000;
	ot.volume = 500;
	ot.total_turnover = 380000000.0;
	ot.turn_over = 1900000.0;
	ot.open_interest = 12000;
	ot.diff_interest = -300;
	ot.trading_date = 20240506;
	ot.action_date = 20240506;
	ot.action_time = 93000000;
	ot.pre_close = 3770.0;
	ot.pre_settle = 3765.0;
	ot.pre_interest = 12300;
	for (int i = 0; i < 10; i++)
	{
		ot.bid_prices[i] = 3799.0 - i;
		ot.ask_prices[i] = 3801.0 + i;
		ot.bid_qty[i] = static_cast<uint32_t>(10 + i);
		ot.ask_qty[i] = static_cast<uint32_t>(20 + i);
	}

	WTSTickStruct nt;
	nt = ot;
	EXPECT_STREQ(nt.exchg, "CFFEX");
	EXPECT_STREQ(nt.code, "IF2406");
	EXPECT_DOUBLE_EQ(nt.price, 3800.0);
	EXPECT_DOUBLE_EQ(nt.open, 3790.0);
	EXPECT_DOUBLE_EQ(nt.high, 3810.0);
	EXPECT_DOUBLE_EQ(nt.low, 3780.0);
	EXPECT_DOUBLE_EQ(nt.settle_price, 3805.0);
	EXPECT_DOUBLE_EQ(nt.upper_limit, 4000.0);
	EXPECT_DOUBLE_EQ(nt.lower_limit, 3600.0);
	EXPECT_DOUBLE_EQ(nt.total_volume, 100000.0);
	EXPECT_DOUBLE_EQ(nt.volume, 500.0);
	EXPECT_DOUBLE_EQ(nt.total_turnover, 380000000.0);
	EXPECT_DOUBLE_EQ(nt.turn_over, 1900000.0);
	EXPECT_DOUBLE_EQ(nt.open_interest, 12000.0);
	EXPECT_DOUBLE_EQ(nt.diff_interest, -300.0);
	EXPECT_EQ(nt.trading_date, 20240506u);
	EXPECT_EQ(nt.action_date, 20240506u);
	EXPECT_EQ(nt.action_time, 93000000u);
	EXPECT_DOUBLE_EQ(nt.pre_close, 3770.0);
	EXPECT_DOUBLE_EQ(nt.pre_settle, 3765.0);
	EXPECT_DOUBLE_EQ(nt.pre_interest, 12300.0);
	for (int i = 0; i < 10; i++)
	{
		EXPECT_DOUBLE_EQ(nt.bid_prices[i], 3799.0 - i);
		EXPECT_DOUBLE_EQ(nt.ask_prices[i], 3801.0 + i);
		EXPECT_DOUBLE_EQ(nt.bid_qty[i], 10.0 + i);
		EXPECT_DOUBLE_EQ(nt.ask_qty[i], 20.0 + i);
	}
}

// WTSStruct.h:243-308：三个 HFT 结构体构造清零与字段写入
TEST(r53_struct_hft, ordque_orddtl_trans_ctor_and_fields)
{
	WTSOrdQueStruct oq;
	EXPECT_STREQ(oq.exchg, "");
	EXPECT_STREQ(oq.code, "");
	EXPECT_EQ(oq.trading_date, 0u);
	EXPECT_EQ(oq.action_time, 0u);
	EXPECT_EQ(oq.order_items, 0u);
	EXPECT_EQ(oq.qsize, 0u);
	EXPECT_EQ(oq.volumes[0], 0u);
	EXPECT_EQ(oq.volumes[49], 0u);
	wt_strcpy(oq.exchg, "SSE");
	wt_strcpy(oq.code, "600000");
	oq.side = BDT_Buy;
	oq.price = 10.5;
	oq.qsize = 2;
	oq.volumes[0] = 100;
	EXPECT_STREQ(oq.exchg, "SSE");
	EXPECT_STREQ(oq.code, "600000");
	EXPECT_EQ(oq.side, BDT_Buy);
	EXPECT_DOUBLE_EQ(oq.price, 10.5);
	EXPECT_EQ(oq.qsize, 2u);
	EXPECT_EQ(oq.volumes[0], 100u);

	WTSOrdDtlStruct od;
	EXPECT_EQ(od.index, 0u);
	EXPECT_EQ(od.volume, 0u);
	EXPECT_EQ(od.orderid, 0);
	EXPECT_EQ(od.otype, (WTSOrdDetailType)0);
	wt_strcpy(od.code, "000001");
	od.index = 42;
	od.side = BDT_Sell;
	od.otype = ODT_LimitPrice;
	od.price = 9.8;
	od.volume = 500;
	od.orderid = 12345;
	EXPECT_STREQ(od.code, "000001");
	EXPECT_EQ(od.index, 42u);
	EXPECT_EQ(od.side, BDT_Sell);
	EXPECT_EQ(od.otype, ODT_LimitPrice);
	EXPECT_DOUBLE_EQ(od.price, 9.8);
	EXPECT_EQ(od.volume, 500u);
	EXPECT_EQ(od.orderid, 12345);

	WTSTransStruct ts;
	EXPECT_EQ(ts.index, 0u);
	EXPECT_EQ(ts.ttype, (WTSTransType)0);
	EXPECT_EQ(ts.volume, 0u);
	EXPECT_EQ(ts.askorder, 0);
	EXPECT_EQ(ts.bidorder, 0);
	wt_strcpy(ts.exchg, "SZSE");
	ts.index = 7;
	ts.ttype = TT_Match;
	ts.side = BDT_Buy;
	ts.price = 20.0;
	ts.volume = 3;
	ts.askorder = 11;
	ts.bidorder = 12;
	EXPECT_STREQ(ts.exchg, "SZSE");
	EXPECT_EQ(ts.index, 7u);
	EXPECT_EQ(ts.ttype, TT_Match);
	EXPECT_EQ(ts.side, BDT_Buy);
	EXPECT_DOUBLE_EQ(ts.price, 20.0);
	EXPECT_EQ(ts.volume, 3u);
	EXPECT_EQ(ts.askorder, 11);
	EXPECT_EQ(ts.bidorder, 12);
}

// Terminated legacy strings copy with their full contents and bounded lengths.
// Support for fixed-width inputs without NUL remains unresolved under R53-003.
TEST(r53_struct_tick, assignment_preserves_maximum_terminated_legacy_strings)
{
	WTSTickStructOld ot;
	std::memset(ot.exchg, 'E', sizeof(ot.exchg) - 1);
	ot.exchg[sizeof(ot.exchg) - 1] = '\0';
	std::memset(ot.code, 'C', sizeof(ot.code) - 1);
	ot.code[sizeof(ot.code) - 1] = '\0';

	WTSTickStruct nt;
	nt = ot;
	ASSERT_TRUE(buffer_has_nul(nt.exchg, sizeof(nt.exchg)));
	EXPECT_EQ(std::string(9, 'E'), std::string(nt.exchg));
	EXPECT_EQ(9u, std::strlen(nt.exchg));
	ASSERT_TRUE(buffer_has_nul(nt.code, sizeof(nt.code)));
	EXPECT_EQ(std::string(31, 'C'), std::string(nt.code));
	EXPECT_EQ(31u, std::strlen(nt.code));
}
