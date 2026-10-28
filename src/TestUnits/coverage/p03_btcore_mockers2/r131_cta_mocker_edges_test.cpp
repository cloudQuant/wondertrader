/**
 * r131_cta_mocker_edges_test.cpp  (lane R131 · wave21)
 *
 * 靶子：src/WtBtCore/CtaMocker.cpp 的公开残余分支
 *   - step_calc 的「未安装 hook」早退（1060-1062）与「已安装 hook 但未在回测中」
 *     自解除路径（1069-1087）；不进入条件变量等待（_in_backtest=false）；
 *   - 同步模拟 tick 下，同 alg 的 WCT_SmallerOrEqual 条件单「更高目标价替换」
 *     分支（831-838），并断言最终成交价取替换后的目标价；
 *   - handle_session_end 在 notifier 非空但未 init MQ 时的 notifyFund（1292-1294）。
 *
 * 只使用公开 API + 真实 HisDataReplayer（r62 夹具的 csv 日线）。
 * 不安装 hook 后调用会阻塞的路径；不联网、不起线程、不读墙钟。
 */
#include <gtest/gtest.h>

#include <Includes/WTSContractInfo.hpp>
#include <WtBtCore/CtaMocker.h>
#include <WtBtCore/EventNotifier.h>

#include "r131_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
}

class R131CtaMocker : public r62::ReplayFixture
{
};

// 生产: CtaMocker.cpp:1060-1062（无 hook 直接 false）
//       1069-1087（有 hook 但 _in_backtest=false -> 自解除返回 false，不会 wait）
TEST_F(R131CtaMocker, StepCalcWithoutCalcThreadReturnsFalse)
{
	CtaMocker m(&_rp, "r131_cta_step", 0, false);

	EXPECT_FALSE(m.step_calc());

	// install_hook 只置 _has_hook；_in_backtest 仍为 false（只有 on_init 会置 true），
	// 因此第二次 step_calc 走 else 分支，不会进入 _cond_calc.wait。
	m.install_hook();
	EXPECT_FALSE(m.step_calc());

	m.enable_hook(false);
	EXPECT_FALSE(m.step_calc());
}

// 生产: CtaMocker.cpp:831-838（模拟 tick 下同 alg 的 Smaller/SmallerOrEqual
//       取更高目标价替换 matchedEntrust，成交价 = min(right_px, 新目标价)）
TEST_F(R131CtaMocker, SmallerOrEqualConditionUsesHigherTargetOnReplace)
{
	CtaMocker m(&_rp, "r131_cta_cond", 0, false);

	// 两张多头限价条件单：<=95 与 <=98。第一笔 100 不触发任何一张。
	m.stra_enter_long(kRb, 1.0, "low", 95.0);
	m.stra_enter_long(kRb, 1.0, "high", 98.0);
	r131::feed(m, kRb, 100.0, 99.0, 101.0);

	// 第二笔 100 -> 90（pxType=1 让 last_px 取 100），两张都满足；
	// 98 > 95，应替换为 98，成交价 = min(100, 98) = 98。
	r131::feed(m, kRb, 90.0, 89.0, 91.0, 1, 1, 1);

	EXPECT_DOUBLE_EQ(1.0, m.stra_get_position(kRb));
	EXPECT_DOUBLE_EQ(98.0, m.stra_get_last_enterprice(kRb));
}

// 生产: CtaMocker.cpp:1292-1294（_notifier 非空时 handle_session_end 广播资金）
//       EventNotifier 未 init 时 _publisher 为空，notifyFund 只做 JSON 组装后返回。
TEST_F(R131CtaMocker, SessionEndNotifiesFundWithoutMqInit)
{
	// 静态存储期先零初始化，规避 R20 台账「EventNotifier 函数指针未初始化」条（ctor 不写 _publisher/_remover 等指针，
	// 动态实例会读未初始化值）；_publisher 为 0 时 notifyFund 只组装 JSON 后返回。
	static EventNotifier notifier;
	CtaMocker m(&_rp, "r131_cta_fund", 0, false, &notifier);

	EXPECT_NO_THROW(m.handle_session_end(20200102u));
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_position(kRb));
}
