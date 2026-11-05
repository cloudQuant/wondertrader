/*
 * R35 - WtPorter CTA/SEL 时间线读数族：真实句柄支里 R13 没取到的那 13 个入口
 *
 * 目标：src/WtPorter/WtPorter.cpp
 *   cta_get_detail_entertime L595-602、cta_get_detail_cost L615-622、
 *   cta_get_detail_profit L637-644、cta_get_first_entertime L763-770、
 *   cta_get_last_entertime L782-789、cta_get_last_exittime L801-808、
 *   cta_get_last_enterprice L820-827、cta_get_last_entertag L839-846、
 *   cta_get_all_position 的真句柄支 L682-686、
 *   sel_get_detail_entertime L1474-1481、sel_get_last_entertime L1572-1579、
 *   sel_get_last_exittime L1591-1598、sel_get_last_enterprice L1610-1617、
 *   sel_get_last_entertag L1629-1636
 *
 * 分工：q06 取的是全族 `ctx == NULL` 拒绝支；R13 取的是 cta 的
 * get_position/profit/avgpx（L701/578/655）与 fund_data（L720），SEL 侧取了
 * L1275/1454/1534/1494/1515/1553/1436 + all_position（L1249-1263）。
 * 本文件只补上面两张清单之外、且已逐行确认「只读 _pos_map、find-miss 即返回」
 * 的入口（实现落点 WtCore/CtaStraBaseCtx.cpp L1761-1822/L1897-1964、
 * SelStraBaseCtx.cpp L1081-1143/L1206 起）。
 *
 * 关键区分点：`*get_last_entertag` 的拒绝支返回 `return 0;`（NULL 指针，
 * WtPorter.cpp:843/:1633），真句柄支返回 `stra_get_last_entertag` 的 `""`
 * （CtaStraBaseCtx.cpp:1774-1786 find-miss 返回字面量空串）——
 * 「非空指针且内容为空串」只有走真句柄支才成立，这条断言不可与拒绝支混淆。
 *
 * 不构造栈上 WtRtRunner（R6 族 23 条红的落点，见 manifest）；句柄一律来自
 * create_cta_context/create_sel_context，落在 getRunner() 单例的引擎上下文表里。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

#include "Includes/WTSStruct.h"

USING_NS_WTP;

extern WtRtRunner& getRunner();

namespace {

	/* 三段式分月合约键（主控规则 4：两段串会把点号后整段当 product） */
	const char* kMonthCode = "SHFE.rb.2510";
	const CtxHandler kBadHandle = 0x7FFF0001UL;   // 本进程绝不会 create 出的 id

	struct PosCbRec
	{
		uint32_t calls = 0;
		std::string last_code;
		double last_qty = -1;
		bool last_islast = false;
		void clear() { *this = PosCbRec(); }
	};
	PosCbRec g_poscb;

	void pos_cb(CtxHandler, const char* stdCode, double position, bool isLast)
	{
		g_poscb.calls++;
		g_poscb.last_code = (stdCode == NULL) ? "<null>" : stdCode;
		g_poscb.last_qty = position;
		g_poscb.last_islast = isLast;
	}

	/* 探针：六族 CTA 槽 + SEL 槽，任何「读数入口顺带派发事件」都会让计数离开 0 */
	uint32_t g_cta_init = 0, g_cta_tick = 0, g_cta_calc = 0, g_cta_bar = 0, g_cta_sess = 0, g_cta_cond = 0;
	uint32_t g_sel_init = 0, g_sel_tick = 0, g_sel_calc = 0, g_sel_bar = 0, g_sel_sess = 0;

	void cb_cta_init(CtxHandler) { g_cta_init++; }
	void cb_cta_tick(CtxHandler, const char*, WTSTickStruct*) { g_cta_tick++; }
	void cb_cta_calc(CtxHandler, WtUInt32, WtUInt32) { g_cta_calc++; }
	void cb_cta_bar(CtxHandler, const char*, const char*, WTSBarStruct*) { g_cta_bar++; }
	void cb_sess(CtxHandler, WtUInt32, bool) { g_cta_sess++; }
	void cb_cond(CtxHandler, const char*, double, double, const char*) { g_cta_cond++; }
	void cb_sel_init(CtxHandler) { g_sel_init++; }
	void cb_sel_tick(CtxHandler, const char*, WTSTickStruct*) { g_sel_tick++; }
	void cb_sel_calc(CtxHandler, WtUInt32, WtUInt32) { g_sel_calc++; }
	void cb_sel_bar(CtxHandler, const char*, const char*, WTSBarStruct*) { g_sel_bar++; }

	void reset_cb_counters()
	{
		g_cta_init = g_cta_tick = g_cta_calc = g_cta_bar = g_cta_sess = g_cta_cond = 0;
		g_sel_init = g_sel_tick = g_sel_calc = g_sel_bar = g_sel_sess = 0;
	}

} // namespace

class R35PorterTrailGetters : public ::testing::Test
{
protected:
	CtxHandler _cta = 0;
	CtxHandler _sel = 0;

	void SetUp() override
	{
		g_poscb.clear();
		reset_cb_counters();
		register_cta_callbacks(cb_cta_init, cb_cta_tick, cb_cta_calc, cb_cta_bar, cb_sess, cb_cond);
		register_sel_callbacks(cb_sel_init, cb_sel_tick, cb_sel_calc, cb_sel_bar, cb_sess);

		_cta = create_cta_context("r35_cta_trail", 2);
		ASSERT_NE(_cta, 0U);
		ASSERT_TRUE(getRunner().getCtaContext(_cta) != NULL);

		_sel = create_sel_context("r35_sel_trail", 20240603, 145500, "d", "CHINA", "TRADING", 0);
		ASSERT_NE(_sel, 0U);
		ASSERT_TRUE(getRunner().getSelContext(_sel) != NULL);
	}

	void TearDown() override
	{
		register_cta_callbacks(NULL, NULL, NULL, NULL, NULL, NULL);
		register_sel_callbacks(NULL, NULL, NULL, NULL, NULL);
		g_poscb.clear();
		reset_cb_counters();
	}
};

/*
 * 空白 CTA 上下文的 7 个时间/价格读数：_pos_map find-miss，逐行返回字面 0 / ""。
 * 期望值出处：CtaStraBaseCtx.cpp:1761-1770(first)/1788-1795(exit)/1798-1808(last enter)/
 * 1811-1821(last price)/1897-1913(detail time)/1916-1932(detail cost)。
 */
TEST_F(R35PorterTrailGetters, CtaTrailGettersOnVirginContextAreExactlyZero)
{
	EXPECT_EQ(cta_get_first_entertime(_cta, kMonthCode), 0ULL);
	EXPECT_EQ(cta_get_last_entertime(_cta, kMonthCode), 0ULL);
	EXPECT_EQ(cta_get_last_exittime(_cta, kMonthCode), 0ULL);
	EXPECT_EQ(cta_get_last_enterprice(_cta, kMonthCode), 0.0);
	EXPECT_EQ(cta_get_detail_entertime(_cta, kMonthCode, "tagA"), 0ULL);
	EXPECT_EQ(cta_get_detail_cost(_cta, kMonthCode, "tagA"), 0.0);
	// 未知 openTag 与空串 openTag 同样落空（strcmp 不中即 find 完返回 0，CtaStraBaseCtx.cpp:1904-1913）
	EXPECT_EQ(cta_get_detail_cost(_cta, kMonthCode, ""), 0.0);

	EXPECT_EQ(g_cta_init, 0u);
	EXPECT_EQ(g_cta_bar, 0u);
}

/*
 * cta_get_detail_profit 把 flag 原样透传（WtPorter.cpp:637-644），
 * 基类 switch 只认 {0,1,-1,2,-2}（CtaStraBaseCtx.cpp:1944-1958），且空白上下文在
 * find-miss 处已返回 0（:1922-1925）——六个已文档 flag + 两个野 flag 全部必须是精确 0.0。
 */
TEST_F(R35PorterTrailGetters, CtaDetailProfitAcceptsEveryFlagAndReturnsExactZero)
{
	const int flags[] = { 0, 1, 2, -1, -2, 3, 99 };
	for (int f : flags)
		EXPECT_EQ(cta_get_detail_profit(_cta, kMonthCode, "tagA", f), 0.0) << "flag=" << f;
}

/*
 * 真句柄必须返回非 NULL 空串（CtaStraBaseCtx.cpp:1774-1780）。
 * BUG-Q06-001：无效句柄的公开契约尚未明确 NULL 与空串哪种表示有效；只接受
 * NULL 或合法的空字符串，不把任意非空内容当作有效结果。
 */
TEST_F(R35PorterTrailGetters, CtaLastEnterTagRealHandleIsBlankUnknownAllowsNullOrEmpty)
{
	WtString unknown = cta_get_last_entertag(kBadHandle, kMonthCode);
	if (unknown != nullptr)
		EXPECT_STREQ(unknown, "") << "非 NULL 无效句柄返回值必须是合法空字符串";

	WtString real = cta_get_last_entertag(_cta, kMonthCode);
	ASSERT_NE(real, nullptr) << "真句柄支必须返回基类的空串而不是 NULL";
	EXPECT_STREQ(real, "");
}

/* SEL 侧同形四连（SelStraBaseCtx.cpp:1094-1143，find-miss 即 0/""）。 */
TEST_F(R35PorterTrailGetters, SelTrailGettersOnVirginContextAreExactlyZero)
{
	EXPECT_EQ(sel_get_detail_entertime(_sel, kMonthCode, "tagB"), 0ULL);
	EXPECT_EQ(sel_get_last_entertime(_sel, kMonthCode), 0ULL);
	EXPECT_EQ(sel_get_last_exittime(_sel, kMonthCode), 0ULL);
	EXPECT_EQ(sel_get_last_enterprice(_sel, kMonthCode), 0.0);

	EXPECT_EQ(g_sel_calc, 0u);
	EXPECT_EQ(g_sel_bar, 0u);
}

/* SEL 的 entertag 同样是「真句柄 "" / 未知句柄 NULL」（WtPorter.cpp:1629-1636）。 */
TEST_F(R35PorterTrailGetters, SelLastEnterTagRealHandleIsBlankStringUnknownIsNull)
{
	WtString unknown = sel_get_last_entertag(kBadHandle, kMonthCode);
	EXPECT_EQ(unknown, nullptr);

	WtString real = sel_get_last_entertag(_sel, kMonthCode);
	ASSERT_NE(real, nullptr);
	EXPECT_STREQ(real, "");
}

/*
 * cta_get_all_position 真句柄支（WtPorter.cpp:682-686）：
 * enum_position 只快照 _pos_map/_sig_map（CtaStraBaseCtx.cpp:1041-1072），两张表皆空，
 * 所以必须「恰好 1 次回调、且是终结符 ("", 0, true)」，内层 0 行；
 * SEL 同位置由 R13 取过，这条是 CTA 族的第一次。
 */
TEST_F(R35PorterTrailGetters, CtaGetAllPositionEmitsTerminatorExactlyOnce)
{
	cta_get_all_position(_cta, pos_cb);

	EXPECT_EQ(g_poscb.calls, 1u);
	EXPECT_EQ(g_poscb.last_code, "");
	EXPECT_EQ(g_poscb.last_qty, 0.0);
	EXPECT_TRUE(g_poscb.last_islast);

	// 读数枚举不得顺带派发任何策略事件
	EXPECT_EQ(g_cta_init, 0u);
	EXPECT_EQ(g_cta_tick, 0u);
	EXPECT_EQ(g_cta_calc, 0u);
	EXPECT_EQ(g_cta_bar, 0u);
	EXPECT_EQ(g_cta_sess, 0u);
	EXPECT_EQ(g_cta_cond, 0u);
}

/*
 * 两个 CTA 上下文共用一个 _cta_engine._ctx_map，但持仓/信号表是每上下文一份
 * （构造于 CtaStraBaseCtx.cpp:58-70）：在 A 上写用户数据、把 A 的时间线读数钉成 0 之后，
 * B 的读数与 B 的用户数据缺省必须不受影响（若读数被提到静态即红）。
 */
TEST_F(R35PorterTrailGetters, TrailReadersArePerContextNotSharedAcrossTwoCtas)
{
	CtxHandler other = create_cta_context("r35_cta_trail_b", 0);
	ASSERT_NE(other, 0U);
	ASSERT_NE(other, _cta);

	cta_save_userdata(_cta, "r35_key", "r35_val_a");

	WtString fromB = cta_load_userdata(other, "r35_key", "r35_def");
	ASSERT_NE(fromB, nullptr);
	EXPECT_STREQ(fromB, "r35_def") << "B 不得看见 A 的用户数据（CtaStraBaseCtx.cpp:1746-1759 是实例 map）";

	EXPECT_EQ(cta_get_last_entertime(other, kMonthCode), 0ULL);
	EXPECT_EQ(cta_get_detail_cost(other, kMonthCode, "tagA"), 0.0);
	WtString tagB = cta_get_last_entertag(other, kMonthCode);
	ASSERT_NE(tagB, nullptr);
	EXPECT_STREQ(tagB, "");

	WtString fromA = cta_load_userdata(_cta, "r35_key", "r35_def");
	ASSERT_NE(fromA, nullptr);
	EXPECT_STREQ(fromA, "r35_val_a");
}
