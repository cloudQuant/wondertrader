/*!
 * \file r123_uft_dtmgr_test.cpp
 * \brief 第二十波 R123 - WtUftDtMgr 实时 tick 缓存生命周期与空指针守卫
 *
 * 覆盖（只读生产代码）:
 *  - WtUftDtMgr.cpp:78-86：handle_push_quote(NULL) 直接返回；非空 tick 首次创建
 *    _rt_tick_map 并按 stdCode 写入（add 自动 retain）。
 *  - WtUftDtMgr.cpp:109-119：grab_last_tick 命中时返回并 retain，未命中返回 NULL。
 *  - WtUftDtMgr.cpp:54-55：析构时释放非空 _rt_tick_map。
 *
 * 不涉及历史缓存 _ticks_cache（生产代码里没有写入点，见 bugs/R123.md 的留白说明）。
 */
#include "r123_uft_common.h"

#include "WtUftCore/WtUftDtMgr.h"

#include <gtest/gtest.h>

using namespace wtp;

namespace {

const char* kAg = "SHFE.ag2406";

// WtUftDtMgr.cpp:81 NULL tick 早退；:111-112 无实时缓存时返回 NULL
TEST(R123UftDtMgr, null_tick_does_not_create_cache)
{
	WtUftDtMgr mgr;
	EXPECT_TRUE(mgr.init(nullptr, nullptr));

	mgr.handle_push_quote(kAg, nullptr);
	EXPECT_EQ(nullptr, mgr.grab_last_tick(kAg));

	// 空缓存下五个切片接口直接返回 NULL
	EXPECT_EQ(nullptr, mgr.get_tick_slice(kAg, 1));
	EXPECT_EQ(nullptr, mgr.get_order_queue_slice(kAg, 1));
	EXPECT_EQ(nullptr, mgr.get_order_detail_slice(kAg, 1));
	EXPECT_EQ(nullptr, mgr.get_transaction_slice(kAg, 1));
	EXPECT_EQ(nullptr, mgr.get_kline_slice(kAg, KP_Minute1, 1, 1));
}

// WtUftDtMgr.cpp:83-86 首次创建 Map 并 retain tick；:114-119 命中并 retain；
// :54-55 析构释放 Map（Map 释放时再释放持有的 tick）。
TEST(R123UftDtMgr, realtime_tick_roundtrip_and_release)
{
	WtUftDtMgr mgr;
	ASSERT_TRUE(mgr.init(nullptr, nullptr));

	WTSTickData* tick = r123::makeTick(20240315, 93000000, 6000.25);
	ASSERT_NE(nullptr, tick);
	mgr.handle_push_quote(kAg, tick);
	// Map 持有自己的引用，测试侧释放创建引用
	tick->release();

	WTSTickData* got = mgr.grab_last_tick(kAg);
	ASSERT_NE(nullptr, got);
	EXPECT_DOUBLE_EQ(6000.25, got->price());
	EXPECT_EQ(20240315u, got->actiondate());
	got->release();

	// 未写入的代码返回 NULL，不影响已命中的缓存
	EXPECT_EQ(nullptr, mgr.grab_last_tick("SHFE.au2406"));
}

}	// namespace
