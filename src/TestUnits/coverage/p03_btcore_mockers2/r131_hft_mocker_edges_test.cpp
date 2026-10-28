/**
 * r131_hft_mocker_edges_test.cpp  (lane R131 · wave21)
 *
 * 靶子：src/WtBtCore/HftMocker.cpp 的公开残余分支
 *   - step_tick 在未 install_hook 时的早退（328-331）；不进入条件变量等待。
 *
 * 不喂 tick、不产生委托，避免 splitVolume 的随机拆单与 _replayer 无 tick 缓存
 * 导致 procOrder 在 stra_get_last_tick 处提前返回。
 */
#include <gtest/gtest.h>

#include <WtBtCore/HftMocker.h>

#include "r131_common.h"

class R131HftMocker : public r62::ReplayFixture
{
};

// 生产: HftMocker.cpp:328-331
TEST_F(R131HftMocker, StepTickWithoutHookReturnsImmediately)
{
	HftMocker m(&_rp, "r131_hft_step");

	EXPECT_NO_THROW(m.step_tick());
	m.enable_hook(false);
	EXPECT_NO_THROW(m.step_tick());
}
