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

#include <cstddef>
#include <cstring>
#include <limits>
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

// These checks exercise the local mappings against the vendored XTP 2.2.32.2
// enum declarations; they do not claim that a linked/vendor runtime was tested.
TEST(xtp_map, wts_offset_maps_to_xtp_position_effect)
{
	EXPECT_EQ((int)XTP_POSITION_EFFECT_OPEN, (int)wrapOffsetType(WOT_OPEN));
	EXPECT_EQ((int)XTP_POSITION_EFFECT_CLOSE, (int)wrapOffsetType(WOT_CLOSE));
	EXPECT_EQ((int)XTP_POSITION_EFFECT_CLOSETODAY, (int)wrapOffsetType(WOT_CLOSETODAY));
	// XTP 2.2.32.2 declares a distinct CLOSEYESTERDAY value (5).
	EXPECT_EQ((int)XTP_POSITION_EFFECT_CLOSEYESTERDAY, (int)wrapOffsetType(WOT_CLOSEYESTERDAY));
	EXPECT_EQ((int)XTP_POSITION_EFFECT_FORCECLOSE, (int)wrapOffsetType(WOT_FORCECLOSE));
}

TEST(xtp_map, xtp_position_effect_maps_to_wts_offset)
{
	EXPECT_EQ((int)WOT_OPEN, (int)wrapOffsetType(XTP_SIDE_BUY, XTP_POSITION_EFFECT_OPEN));
	EXPECT_EQ((int)WOT_CLOSE, (int)wrapOffsetType(XTP_SIDE_BUY, XTP_POSITION_EFFECT_CLOSE));
	EXPECT_EQ((int)WOT_CLOSETODAY, (int)wrapOffsetType(XTP_SIDE_SELL, XTP_POSITION_EFFECT_CLOSETODAY));
	EXPECT_EQ((int)WOT_CLOSEYESTERDAY, (int)wrapOffsetType(XTP_SIDE_SELL, XTP_POSITION_EFFECT_CLOSEYESTERDAY));
	EXPECT_EQ((int)WOT_FORCECLOSE, (int)wrapOffsetType(XTP_SIDE_SELL, XTP_POSITION_EFFECT_FORCECLOSE));
}

TEST(xtp_map, supported_offsets_round_trip_through_xtp_effects)
{
	const WTSOffsetType offsets[] = {
		WOT_OPEN,
		WOT_CLOSE,
		WOT_CLOSETODAY,
		WOT_CLOSEYESTERDAY,
	};
	const XTP_POSITION_EFFECT_TYPE effects[] = {
		XTP_POSITION_EFFECT_OPEN,
		XTP_POSITION_EFFECT_CLOSE,
		XTP_POSITION_EFFECT_CLOSETODAY,
		XTP_POSITION_EFFECT_CLOSEYESTERDAY,
	};

	for (std::size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
	{
		const XTP_POSITION_EFFECT_TYPE effect = wrapOffsetType(offsets[i]);
		EXPECT_EQ((int)effects[i], (int)effect);
		EXPECT_EQ((int)offsets[i], (int)wrapOffsetType(XTP_SIDE_BUY, effect));
	}
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

/// Verify consecutive uint32 IDs without assuming this process has not used the generator yet.
TEST(xtp_map, make_ref_id_returns_consecutive_uint32_values)
{
	const uint32_t a = makeRefID();
	const uint32_t b = makeRefID();
	const uint32_t c = makeRefID();
	const uint32_t maxId = std::numeric_limits<uint32_t>::max();

	// fetch_add can wrap the stored value to zero after returning UINT32_MAX;
	// the next call treats zero as the uninitialized sentinel and reseeds it.
	if (a != maxId)
		EXPECT_EQ(static_cast<uint32_t>(a + uint32_t{1}), b);
	if (b != maxId)
		EXPECT_EQ(static_cast<uint32_t>(b + uint32_t{1}), c);
}

} // namespace
