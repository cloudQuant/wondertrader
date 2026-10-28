/**
 * r113_btporter_null_mocker_test.cpp  (lane R113 · wave19)
 *
 * 靶子：WtBtPorter 的 sel_get_last_entertag 在 mocker 为空时的中性返回
 *   - WtBtPorter.cpp:1450-1457 的 `ctx == NULL -> return 0`（NULL 指针）
 *
 * q06 覆盖了多数未知句柄 getter，但唯独没有调用 sel_get_last_entertag；
 * R83 只走 mocker 非空支。本用例在重置后的进程单例上走空支。
 * 重置方式沿用 R83：公开析构 + placement new，不读写任何私有成员。
 */
#include <gtest/gtest.h>

#include <new>

#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtPorter.h"
#include "WtBtPorter/WtBtRunner.h"

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace
{
	void reset_singleton_runner()
	{
		WtBtRunner& r = getRunner();
		r.~WtBtRunner();
		new (&r) WtBtRunner();
	}
}

/*
 * 生产: WtBtPorter.cpp:1450-1457（mocker 为空 -> return 0）
 */
TEST(R113BtPorterNullMocker, SelLastEnterTagReturnsNullOnNullMocker)
{
	reset_singleton_runner();
	ASSERT_EQ(getRunner().sel_mocker(), nullptr);

	const WtString tag = sel_get_last_entertag(0x11311311UL, "SSE.600000");
	EXPECT_EQ(tag, nullptr);
}
