/**
 * r20_mocker_inline_id_test.cpp  (lane R20)
 *
 * 靶子: 四个 Mocker.h 的内联段。可无接缝直达的头部内联体只有
 * CtaMocker.h:334 / SelMocker.h:254 的 `id()`（`{ return _context_id; }`）
 * 与 UftMocker.h PosItem 内联访问器（经公开成交链 p08 已锚定 valid()/volume()，
 * 其 frozen()/closeprofit()/dynprofit() 组合臂本轮用 stra_get_position 族再补一次）。
 * 其余 Mocker.h 内联体（PosInfo/DetailInfo 等嵌套私有结构）无公开接缝可达，登记留白。
 *
 * 不新增除零路径：SelMocker::handle_replay_done 沿用 q02 整类 DISABLED，本文件不调用。
 */
#include <gtest/gtest.h>

#include <string>

#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/CtaMocker.h>
#include <WtBtCore/SelMocker.h>
#include <WtBtCore/UftMocker.h>

class R20MockerInlineId : public ::testing::Test
{
protected:
	HisDataReplayer _rp;	// 默认构造即可：id() 与零值查询不触碰基础数据表
};

// CtaMocker.h:334 内联 id()：透传 _context_id，且两个实例的 id 互不相同（构造计数分配）
TEST_F(R20MockerInlineId, CtaMockerIdInlinePassesThroughContextId)
{
	CtaMocker m1(&_rp, "r20_cta_id_1");
	CtaMocker m2(&_rp, "r20_cta_id_2");
	const uint32_t a = m1.id();
	const uint32_t b = m2.id();
	EXPECT_NE(a, b);
	// 同一实例重复读稳定（内联体只读一个字段，无副作用）
	EXPECT_EQ(a, m1.id());
	EXPECT_EQ(b, m2.id());
}

// SelMocker.h:254 内联 id()：同形断言；并与 CTA 实例共用同一分配器时不重叠
TEST_F(R20MockerInlineId, SelMockerIdInlineDistinctFromCta)
{
	CtaMocker cta(&_rp, "r20_cta_vs_sel");
	SelMocker sel(&_rp, "r20_sel_vs_cta");
	const uint32_t a = cta.id();
	const uint32_t b = sel.id();
	EXPECT_NE(a, b);
	EXPECT_EQ(b, sel.id());
}

// UftMocker 头部内联访问器的公开补测：零持仓下 stra_get_position 族
// 应全为 0（valid()=volume()-frozen() 组合在空 PosItem 上恒 0；
// 注意本用例只喂「未发生任何成交」的输入，期望值 0 由输入直接算得）
TEST_F(R20MockerInlineId, UftZeroPositionAccessorsAgreeOnEmptyState)
{
	UftMocker uft(&_rp, "r20_uft_zero");
	// 三段式合约键形（CodeHelper.hpp:601-616）；未注册品种 → 查询零值，不查 comminfo
	const char* code = "SHFE.rb.2405";
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_position(code, false, 3));
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_position(code, true, 1));
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_position(code, true, 2));
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_local_position(code));
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_undone(code));
	EXPECT_EQ(0u, uft.stra_get_date());
	EXPECT_EQ(0u, uft.stra_get_time());
	EXPECT_EQ(0u, uft.stra_get_secs());
}
