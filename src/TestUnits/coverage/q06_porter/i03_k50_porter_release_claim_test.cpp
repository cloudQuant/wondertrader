/*
 * 迭代03 K50（I03-NEW-089，用户裁定 2026-11-06）：WtPorter 的人工合约归属释放 C 接口。
 * 未知 trader 返回 InvalidRequest（2），不崩溃；完整的释放语义见 wtcore 侧 i03_k50 用例。
 */
#include <gtest/gtest.h>

#include "WtPorter/WtPorter.h"

TEST(I03K50PorterReleaseClaim, UnknownTraderIsAnInvalidRequest)
{
	EXPECT_EQ(2, release_manual_claim("i03_k50_no_such_trader", "SHFE.ag.2512", "alice", "closed", "T-1"));
	EXPECT_EQ(2, release_manual_claim(nullptr, nullptr, nullptr, nullptr, nullptr));
}
