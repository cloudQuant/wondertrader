/*
 * T5 lane — src/TraderYD/TraderYD.cpp 中「厂商枚举 ↔ WTS 枚举」纯映射函数的直测。
 *
 * 手法说明（重要，便于评审/回退）：
 *   TraderYD.cpp 的这批 wrap 系列与 getErrorMsg 定义在 namespace 作用域，
 *   `constexpr inline` 仍是 external 链接（实测 nm build-cov/TraderYD.dir/TraderYD.cpp.o
 *   里有 __Z17wrapDirectionTypeii / __Z11getErrorMsgi 等 T 符号），所以本文件按同样的
 *   签名声明为普通 extern 后直接调用（声明成 inline 会触发 -Wundefined-inline，
 *   -Werror 直接挂），不需要 #define private public，也不改产品代码。
 *   如果主控认为「跨 TU 声明生产 inline 函数」不可接受，删掉本文件即可，
 *   t5_yd_api_test.cpp 不依赖这里的任何符号。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "API/yd1.108.360/ydDataType.h"
#include "API/yd1.108.360/ydError.h"
#include "Includes/WTSTypes.h"

USING_NS_WTP;

extern WTSDirectionType wrapPosDirection(int dirType) noexcept;
extern WTSDirectionType wrapDirectionType(int dirType, int offsetType) noexcept;
extern int wrapDirectionType(WTSDirectionType dirType, WTSOffsetType offsetType) noexcept;
extern int wrapPriceType(WTSPriceType pType, WTSOrderFlag oFlag) noexcept;
extern WTSOffsetType wrapOffsetType(int offType) noexcept;
extern int wrapOffsetType(WTSOffsetType offType) noexcept;
extern WTSOrderState wrapOrderState(int orderState) noexcept;
extern const char* getErrorMsg(int ydError) noexcept;

namespace {

TEST(yd_map, position_direction_only_long_is_long)
{
	EXPECT_EQ((int)WDT_LONG, (int)wrapPosDirection(YD_PD_Long));
	EXPECT_EQ((int)WDT_SHORT, (int)wrapPosDirection(YD_PD_Short));
	EXPECT_EQ((int)WDT_SHORT, (int)wrapPosDirection(0));        // 未知值一律按空头
	EXPECT_EQ((int)WDT_SHORT, (int)wrapPosDirection(999));
}

TEST(yd_map, vendor_direction_and_offset_fold_into_wts_direction)
{
	EXPECT_EQ((int)WDT_LONG, (int)wrapDirectionType(YD_D_Buy, YD_OF_Open));
	EXPECT_EQ((int)WDT_SHORT, (int)wrapDirectionType(YD_D_Buy, YD_OF_Close));
	EXPECT_EQ((int)WDT_SHORT, (int)wrapDirectionType(YD_D_Sell, YD_OF_Open));
	EXPECT_EQ((int)WDT_LONG, (int)wrapDirectionType(YD_D_Sell, YD_OF_Close));
	EXPECT_EQ((int)WDT_LONG, (int)wrapDirectionType(YD_D_Sell, YD_OF_CloseToday));
}

TEST(yd_map, wts_direction_and_offset_fold_back_into_vendor_direction)
{
	EXPECT_EQ(YD_D_Buy, wrapDirectionType(WDT_LONG, WOT_OPEN));
	EXPECT_EQ(YD_D_Sell, wrapDirectionType(WDT_LONG, WOT_CLOSE));
	EXPECT_EQ(YD_D_Sell, wrapDirectionType(WDT_SHORT, WOT_OPEN));
	EXPECT_EQ(YD_D_Buy, wrapDirectionType(WDT_SHORT, WOT_CLOSE));
}

/// 双向换算必须自洽：以厂商侧 4 个组合为起点 round-trip
TEST(yd_map, direction_roundtrip_is_stable_for_the_four_combinations)
{
	const int dirs[] = { YD_D_Buy, YD_D_Sell };
	const int offs[] = { YD_OF_Open, YD_OF_Close };
	for (int d : dirs)
	{
		for (int o : offs)
		{
			const WTSDirectionType wd = wrapDirectionType(d, o);
			const WTSOffsetType wo = wrapOffsetType(o);
			EXPECT_EQ(d, wrapDirectionType(wd, wo)) << "d=" << d << " o=" << o;
		}
	}
	// 平今/平昨 round-trip 不成立：卖+平今 → WDT_LONG，但 WDT_LONG+WOT_CLOSETODAY → 卖
	EXPECT_EQ((int)WDT_LONG, (int)wrapDirectionType(YD_D_Sell, YD_OF_CloseToday));
	EXPECT_EQ(YD_D_Sell, wrapDirectionType(WDT_LONG, WOT_CLOSETODAY));
}

TEST(yd_map, offset_type_two_way_tables)
{
	EXPECT_EQ((int)WOT_OPEN, (int)wrapOffsetType(YD_OF_Open));
	EXPECT_EQ((int)WOT_CLOSE, (int)wrapOffsetType(YD_OF_Close));
	EXPECT_EQ((int)WOT_CLOSETODAY, (int)wrapOffsetType(YD_OF_CloseToday));
	EXPECT_EQ((int)WOT_CLOSEYESTERDAY, (int)wrapOffsetType(YD_OF_CloseYesterday));
	EXPECT_EQ((int)WOT_FORCECLOSE, (int)wrapOffsetType(YD_OF_ForceClose));
	EXPECT_EQ((int)WOT_FORCECLOSE, (int)wrapOffsetType(-1));        // default 分支

	EXPECT_EQ(YD_OF_Open, wrapOffsetType(WOT_OPEN));
	EXPECT_EQ(YD_OF_Close, wrapOffsetType(WOT_CLOSE));
	EXPECT_EQ(YD_OF_CloseToday, wrapOffsetType(WOT_CLOSETODAY));
	EXPECT_EQ(YD_OF_CloseYesterday, wrapOffsetType(WOT_CLOSEYESTERDAY));
	EXPECT_EQ(YD_OF_ForceClose, wrapOffsetType(WOT_FORCECLOSE));
}

TEST(yd_map, price_type_with_order_flag_precedence)
{
	EXPECT_EQ(YD_ODT_Limit, wrapPriceType(WPT_LIMITPRICE, WOF_NOR));
	EXPECT_EQ(YD_ODT_Market, wrapPriceType(WPT_ANYPRICE, WOF_NOR));
	// 最优价/最新价等非限价 → 市价
	EXPECT_EQ(YD_ODT_Market, wrapPriceType(WPT_BESTPRICE, WOF_NOR));
	// FAK/FOK 优先级高于价格类型（即便限价也返回 ODT_FAK/FOK）
	EXPECT_EQ(YD_ODT_FAK, wrapPriceType(WPT_LIMITPRICE, WOF_FAK));
	EXPECT_EQ(YD_ODT_FOK, wrapPriceType(WPT_LIMITPRICE, WOF_FOK));
	EXPECT_EQ(YD_ODT_FAK, wrapPriceType(WPT_ANYPRICE, WOF_FAK));
}

TEST(yd_map, order_state_table_and_default_is_submitting)
{
	EXPECT_EQ((int)WOS_NotTraded_NotQueuing, (int)wrapOrderState(YD_OS_Accepted));
	EXPECT_EQ((int)WOS_NotTraded_Queuing, (int)wrapOrderState(YD_OS_Queuing));
	EXPECT_EQ((int)WOS_Canceled, (int)wrapOrderState(YD_OS_Canceled));
	EXPECT_EQ((int)WOS_Canceled, (int)wrapOrderState(YD_OS_Rejected));
	EXPECT_EQ((int)WOS_AllTraded, (int)wrapOrderState(YD_OS_AllTraded));
	EXPECT_EQ((int)WOS_Submitting, (int)wrapOrderState(0));
	EXPECT_EQ((int)WOS_Submitting, (int)wrapOrderState(4242));
}

TEST(yd_map, error_msg_known_codes_and_unknown_fallback)
{
	EXPECT_STREQ("NoError", getErrorMsg(YD_ERROR_NoError));
	EXPECT_STREQ("PasswordError", getErrorMsg(YD_ERROR_PasswordError));
	EXPECT_STREQ("OrderNotFound", getErrorMsg(YD_ERROR_OrderNotFound));
	EXPECT_STREQ("NoMoneyToOpen", getErrorMsg(YD_ERROR_NoMoneyToOpen));
	EXPECT_STREQ("InvalidOrderVolume", getErrorMsg(YD_ERROR_InvalidOrderVolume));
	EXPECT_STREQ("PossibleSelfTrade", getErrorMsg(YD_ERROR_PossibleSelfTrade));
	// 柜台/交易所侧透传的两个上期所错误码
	EXPECT_STREQ("NotInTradingTime", getErrorMsg(1026));
	EXPECT_STREQ("OrderAllTraded", getErrorMsg(1028));
	// 未知码必须有回落，不能越界读表
	EXPECT_STREQ("Unknown Error", getErrorMsg(777777));
	EXPECT_STREQ("Unknown Error", getErrorMsg(-1));
	// API 自身生成的两个码也在表里
	EXPECT_STREQ("CannotSend", getErrorMsg(YD_ERROR_CannotSend));
	EXPECT_STREQ("ExchangeReportError", getErrorMsg(YD_ERROR_ExchangeReportError));
}

} // namespace
