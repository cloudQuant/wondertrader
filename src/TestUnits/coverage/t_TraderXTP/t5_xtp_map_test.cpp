/*
 * T5 lane — src/TraderXTP/TraderXTP.cpp 中「厂商枚举 ↔ WTS 枚举」纯映射函数的直测。
 *
 * 手法说明：TraderXTP.cpp 的这批 wrap 系列 / IsErrorInfo / makeRefID 定义在 namespace
 * 作用域、链接性为 external（实测 nm build-cov/TraderXTP.dir/TraderXTP.cpp.o 里有
 * __Z17wrapDirectionTypehh、__Z11IsErrorInfoP16XTPRspInfoStruct 等 T 符号），
 * 这里按同样的签名声明为普通 extern 后直接调用（声明成 inline 会触发
 * -Wundefined-inline，-Werror 直接挂）。不改产品代码、不用 #define private public。
 * 如果主控不接受「跨 TU 声明生产 inline 函数」，删掉本文件即可，
 * t5_xtp_api_test.cpp 不依赖这里的任何符号。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "TraderXTP/TraderXTP.h"

#include "Includes/WTSTypes.h"

USING_NS_WTP;

extern bool IsErrorInfo(XTPRI* error_info);
extern WTSDirectionType wrapPosDirection(XTP_POSITION_DIRECTION_TYPE dirType);
extern XTP_SIDE_TYPE wrapDirectionType(WTSDirectionType dirType, WTSOffsetType offsetType);
extern WTSDirectionType wrapDirectionType(XTP_SIDE_TYPE side, XTP_POSITION_EFFECT_TYPE pe);
extern XTP_POSITION_EFFECT_TYPE wrapOffsetType(WTSOffsetType offType);
extern WTSOffsetType wrapOffsetType(XTP_SIDE_TYPE side, XTP_POSITION_EFFECT_TYPE offType);
extern WTSPriceType wrapPriceType(XTP_PRICE_TYPE priceType);
extern WTSOrderState wrapOrderState(XTP_ORDER_STATUS_TYPE orderState);
extern uint32_t makeRefID();

namespace {

TEST(xtp_map, is_error_info_requires_nonzero_id_and_accepts_null)
{
	XTPRI nil_check;
	memset(&nil_check, 0, sizeof(nil_check));
	nil_check.error_id = 0;
	EXPECT_FALSE(IsErrorInfo(&nil_check));          // error_id==0 → 视为无错

	nil_check.error_id = 1;
	EXPECT_TRUE(IsErrorInfo(&nil_check));

	nil_check.error_id = -3;
	EXPECT_TRUE(IsErrorInfo(&nil_check));           // 负数同样算错

	EXPECT_FALSE(IsErrorInfo(NULL));                // 空指针守卫
}

TEST(xtp_map, position_direction_short_only)
{
	EXPECT_EQ((int)WDT_SHORT, (int)wrapPosDirection(XTP_POSITION_DIRECTION_SHORT));
	EXPECT_EQ((int)WDT_LONG, (int)wrapPosDirection(XTP_POSITION_DIRECTION_LONG));
	EXPECT_EQ((int)WDT_LONG, (int)wrapPosDirection(XTP_POSITION_DIRECTION_NET));
	EXPECT_EQ((int)WDT_LONG, (int)wrapPosDirection(XTP_POSITION_DIRECTION_COVERED));
}

TEST(xtp_map, wts_side_to_vendor_side_uses_offset)
{
	EXPECT_EQ((int)XTP_SIDE_BUY, (int)wrapDirectionType(WDT_LONG, WOT_OPEN));
	EXPECT_EQ((int)XTP_SIDE_SELL, (int)wrapDirectionType(WDT_LONG, WOT_CLOSE));
	EXPECT_EQ((int)XTP_SIDE_SELL, (int)wrapDirectionType(WDT_SHORT, WOT_OPEN));
	EXPECT_EQ((int)XTP_SIDE_BUY, (int)wrapDirectionType(WDT_SHORT, WOT_CLOSE));
	// 平今/平昨/强平都按「非开仓」处理 → 与 CLOSE 同侧
	EXPECT_EQ((int)XTP_SIDE_SELL, (int)wrapDirectionType(WDT_LONG, WOT_CLOSETODAY));
	EXPECT_EQ((int)XTP_SIDE_BUY, (int)wrapDirectionType(WDT_SHORT, WOT_CLOSEYESTERDAY));
}

TEST(xtp_map, vendor_side_to_wts_direction_uses_position_effect)
{
	EXPECT_EQ((int)WDT_LONG, (int)wrapDirectionType(XTP_SIDE_BUY, XTP_POSITION_EFFECT_OPEN));
	EXPECT_EQ((int)WDT_SHORT, (int)wrapDirectionType(XTP_SIDE_BUY, XTP_POSITION_EFFECT_CLOSE));
	EXPECT_EQ((int)WDT_SHORT, (int)wrapDirectionType(XTP_SIDE_SELL, XTP_POSITION_EFFECT_OPEN));
	EXPECT_EQ((int)WDT_LONG, (int)wrapDirectionType(XTP_SIDE_SELL, XTP_POSITION_EFFECT_CLOSE));
	EXPECT_EQ((int)WDT_SHORT, (int)wrapDirectionType(XTP_SIDE_BUY, XTP_POSITION_EFFECT_INIT));   // INIT(0)!=OPEN -> 买被折成空头
}

TEST(xtp_map, offset_type_two_way_and_closeyesterday_alias)
{
	EXPECT_EQ((int)XTP_POSITION_EFFECT_OPEN, (int)wrapOffsetType(WOT_OPEN));
	EXPECT_EQ((int)XTP_POSITION_EFFECT_CLOSE, (int)wrapOffsetType(WOT_CLOSE));
	EXPECT_EQ((int)XTP_POSITION_EFFECT_CLOSETODAY, (int)wrapOffsetType(WOT_CLOSETODAY));
	// 现状：WOT_CLOSEYESTERDAY 被映射成 CLOSE（XTP 没有平昨枚举）
	EXPECT_EQ((int)XTP_POSITION_EFFECT_CLOSE, (int)wrapOffsetType(WOT_CLOSEYESTERDAY));
	EXPECT_EQ((int)XTP_POSITION_EFFECT_FORCECLOSE, (int)wrapOffsetType(WOT_FORCECLOSE));

	EXPECT_EQ((int)WOT_OPEN, (int)wrapOffsetType(XTP_SIDE_BUY, XTP_POSITION_EFFECT_OPEN));
	EXPECT_EQ((int)WOT_CLOSE, (int)wrapOffsetType(XTP_SIDE_BUY, XTP_POSITION_EFFECT_CLOSE));
	EXPECT_EQ((int)WOT_CLOSETODAY, (int)wrapOffsetType(XTP_SIDE_SELL, XTP_POSITION_EFFECT_CLOSETODAY));
	EXPECT_EQ((int)WOT_FORCECLOSE, (int)wrapOffsetType(XTP_SIDE_SELL, XTP_POSITION_EFFECT_FORCECLOSE));
	// 反向表里「XTP_POSITION_EFFECT_CLOSE → WOT_CLOSEYESTERDAY」那条分支是死代码：
	// 前面的 CLOSE→WOT_CLOSE 已经先返回了（见 bugs）
	EXPECT_EQ((int)WOT_CLOSE, (int)wrapOffsetType(XTP_SIDE_BUY, XTP_POSITION_EFFECT_CLOSE));
}

TEST(xtp_map, price_type_limit_versus_everything_else)
{
	EXPECT_EQ((int)WPT_LIMITPRICE, (int)wrapPriceType(XTP_PRICE_LIMIT));
	EXPECT_EQ((int)WPT_ANYPRICE, (int)wrapPriceType(XTP_PRICE_BEST_OR_CANCEL));
	EXPECT_EQ((int)WPT_ANYPRICE, (int)wrapPriceType(XTP_PRICE_BEST5_OR_CANCEL));
}

TEST(xtp_map, order_state_table_and_default)
{
	EXPECT_EQ((int)WOS_NotTraded_NotQueuing, (int)wrapOrderState(XTP_ORDER_STATUS_INIT));
	EXPECT_EQ((int)WOS_AllTraded, (int)wrapOrderState(XTP_ORDER_STATUS_ALLTRADED));
	EXPECT_EQ((int)WOS_PartTraded_Queuing, (int)wrapOrderState(XTP_ORDER_STATUS_PARTTRADEDQUEUEING));
	EXPECT_EQ((int)WOS_NotTraded_Queuing, (int)wrapOrderState(XTP_ORDER_STATUS_NOTRADEQUEUEING));
	EXPECT_EQ((int)WOS_Canceled, (int)wrapOrderState(XTP_ORDER_STATUS_PARTTRADEDNOTQUEUEING));
	EXPECT_EQ((int)WOS_Canceled, (int)wrapOrderState(XTP_ORDER_STATUS_CANCELED));
	EXPECT_EQ((int)WOS_Nottouched, (int)wrapOrderState(XTP_ORDER_STATUS_REJECTED));
	EXPECT_EQ((int)WOS_Nottouched, (int)wrapOrderState(XTP_ORDER_STATUS_UNKNOWN));
}

/// makeRefID：首次调用按「当前时间-20220101」播种成 100 的倍数，之后严格自增
TEST(xtp_map, make_ref_id_is_seeded_and_monotonic)
{
	const uint32_t a = makeRefID();
	const uint32_t b = makeRefID();
	const uint32_t c = makeRefID();
	EXPECT_EQ(0u, a % 100u);              // 播种值一定是 100 的整数倍
	EXPECT_EQ(b, a + 1);
	EXPECT_EQ(c, a + 2);
}

} // namespace
