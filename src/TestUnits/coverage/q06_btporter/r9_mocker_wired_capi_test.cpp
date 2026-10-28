/*
 * R9 - WtBtPorter 第七波：init_*_mocker 接线后的 C 导出真实分支
 *
 * q06 的两个文件只打了 `_mocker == NULL` 的中性返回值分支；本文件把
 * WtBtPorter.cpp L309-312 / L321-324 的 init_cta_mocker、init_hft_mocker 接进
 * WtBtRunner.cpp L266-283 / L285-297（已核对：唯一副作用是 Exp*Mocker 构造 +
 * HisDataReplayer.h:804-807 的 register_sink，两行赋值，不起线程、不碰存储），
 * 再打 WtBtPorter.cpp 里 mocker 非空那一条：
 *   L589-603 cta_get_all_position（空持仓表仍必须补终止标记）
 *   L614-...  cta_get_position（CtaMocker.cpp:1999 起，纯 _sig_map/_pos_map 查找）
 *   L1472-... hft_get_position（HftMocker.cpp:899 `_pos_map[code]` → 0）
 *   L1521-... hft_get_undone（HftMocker.cpp:533 遍历空 _orders → 0）
 *   L243-246  get_raw_stdcode（HisDataReplayer.cpp:2921-2931，空热点表 → ""）
 *
 * 上下文 id 由 CtaMocker.cpp:55-56、HftMocker.cpp:114-118 的 static atomic fetch_add(1)
 * 发出，所以「第二次初始化必须拿到 id1+1」是可推导的期望值，用来同时覆盖 L269-273 的
 * delete-旧实例分支。persistData 传 false、hook 传 false、bIncremental 传 false，
 * 避免 load_incremental_data / install_hook 的落盘与钩子线程。
 * 文件名排在 r9_exp_mocker_bridge_test.cpp 之后：它会把 mocker 槽位永久置为非空，
 * 不能反过来污染前面那组「未初始化」断言。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtPorter.h"
#include "WtBtPorter/WtBtRunner.h"
// 经包装层头文件取到 WtBtCore 侧 CtaMocker/HftMocker 的完整类型（WtBtRunner.h 只有前向声明）
#include "WtBtPorter/ExpCtaMocker.h"
#include "WtBtPorter/ExpHftMocker.h"

#include "Includes/WTSStruct.h"

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace {

	uint32_t g_r9_pos_calls = 0;
	std::string g_r9_pos_code = "<unset>";
	double g_r9_pos_qty = -1.0;
	bool g_r9_pos_islast = false;

	void r9_pos_cb(CtxHandler /*cHandle*/, const char* stdCode, double qty, bool isLast)
	{
		g_r9_pos_calls++;
		g_r9_pos_code = (stdCode == NULL) ? "<null>" : stdCode;
		g_r9_pos_qty = qty;
		g_r9_pos_islast = isLast;
	}

	void r9_reset_pos()
	{
		g_r9_pos_calls = 0;
		g_r9_pos_code = "<unset>";
		g_r9_pos_qty = -1.0;
		g_r9_pos_islast = false;
	}
}

/* init_cta_mocker 必须新建实例、写槽位并返回它的 context id（WtBtRunner.cpp:275-282） */
TEST(R9WiredMockerCapi, InitCtaMockerWiresSlotAndReturnsContextId)
{
	const uint32_t id = init_cta_mocker("r9_cta_first", 0, false, false, false, false);
	EXPECT_GT(id, 0U);

	CtaMocker* mocker = getRunner().cta_mocker();
	ASSERT_NE(mocker, nullptr);
	EXPECT_EQ(mocker->id(), id);
	// 未 config 成功，回放器时钟仍为初值（HisDataReplayer 成员未推进）
	EXPECT_EQ(cta_get_date(), 0U);
	EXPECT_EQ(cta_get_time(), 0U);
}

/* 重复初始化走 delete 旧实例的分支，id 必须是 fetch_add 的下一个（CtaMocker.cpp:55-56） */
TEST(R9WiredMockerCapi, InitCtaMockerTwiceReplacesPreviousContext)
{
	const uint32_t id1 = init_cta_mocker("r9_cta_a", 0, false, false, false, false);
	const uint32_t id2 = init_cta_mocker("r9_cta_b", 0, false, false, false, false);

	EXPECT_EQ(id2, id1 + 1U);
	CtaMocker* mocker = getRunner().cta_mocker();
	ASSERT_NE(mocker, nullptr);
	EXPECT_EQ(mocker->id(), id2);
	// 只有 _cta_mocker 一个槽位被换掉，SEL 槽位不受影响（构造列表 L82-83）
	EXPECT_EQ(getRunner().sel_mocker(), nullptr);
}

/* init_hft_mocker 同一套接线，id 段从 6000 起且严格递增（HftMocker.cpp:114-118） */
TEST(R9WiredMockerCapi, InitHftMockerWiresSlotAndIsRepeatable)
{
	const uint32_t id1 = init_hft_mocker("r9_hft_a", false);
	const uint32_t id2 = init_hft_mocker("r9_hft_b", false);
	EXPECT_GT(id1, 0U);
	EXPECT_EQ(id2, id1 + 1U);

	HftMocker* mocker = getRunner().hft_mocker();
	ASSERT_NE(mocker, nullptr);
	EXPECT_EQ(mocker->id(), id2);
}

/* mocker 非空 + 持仓表为空：不得进入循环，但仍要补一次 isLast=true 的终止标记 */
TEST(R9WiredMockerCapi, EnumPositionWithWiredMockerStillTerminates)
{
	init_cta_mocker("r9_cta_enum", 0, false, false, false, false);
	r9_reset_pos();

	cta_get_all_position(0x5A5A, r9_pos_cb);
	EXPECT_EQ(g_r9_pos_calls, 1U);
	EXPECT_EQ(g_r9_pos_code, "");
	EXPECT_DOUBLE_EQ(g_r9_pos_qty, 0.0);
	EXPECT_TRUE(g_r9_pos_islast);
}

/* 空持仓簿的持仓查询恒为 0：走的是 mocker 非空的真实实现而不是中性兜底 */
TEST(R9WiredMockerCapi, CtaPositionGetterRunsMockerOnEmptyBook)
{
	const uint32_t id = init_cta_mocker("r9_cta_pos", 0, false, false, false, false);
	ASSERT_GT(id, 0U);
	ASSERT_NE(getRunner().cta_mocker(), nullptr);

	EXPECT_DOUBLE_EQ(cta_get_position(id, "SSE.600000", true, ""), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_position(id, "SSE.600000", false, ""), 0.0);
}

/* hft_get_position / hft_get_undone 在空簿上的取值（HftMocker.cpp:899-907 / 533-545） */
TEST(R9WiredMockerCapi, HftPositionAndUndoneAreZeroOnEmptyBook)
{
	const uint32_t id = init_hft_mocker("r9_hft_pos", false);
	ASSERT_GT(id, 0U);
	ASSERT_NE(getRunner().hft_mocker(), nullptr);

	EXPECT_DOUBLE_EQ(hft_get_position(id, "SHFE.au2406", true), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_position(id, "SHFE.au2406", false), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_undone(id, "SHFE.au2406"), 0.0);
}

/*
 * 代码表为空时 _hot_mgr.getRuleTag() 恒为空 → CodeInfo._ruletag 为空 → hasRule() 为假
 * → HisDataReplayer::get_rawcode 落到末尾的 return ""（L2930）
 */
TEST(R9WiredMockerCapi, GetRawStdCodeIsEmptyWithoutRulesLoaded)
{
	EXPECT_STREQ(get_raw_stdcode("SSE.600000"), "");
	EXPECT_STREQ(getRunner().get_raw_stdcode("SSE.600000"), "");
}

/* mocker 已接线，但回放器没跑：时间/价格类直取入口仍然是成员初值，不受 register_sink 影响 */
TEST(R9WiredMockerCapi, ReplayerClockAndPriceStayZeroAfterSinkRegister)
{
	init_cta_mocker("r9_cta_clock", 0, false, false, false, false);
	init_hft_mocker("r9_hft_clock", false);

	EXPECT_EQ(cta_get_tdate(), 0U);
	EXPECT_EQ(cta_get_date(), 0U);
	EXPECT_EQ(cta_get_time(), 0U);
	EXPECT_EQ(hft_get_date(), 0U);
	EXPECT_EQ(hft_get_secs(), 0U);
	EXPECT_DOUBLE_EQ(cta_get_price("SSE.600000"), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_price("SHFE.au2406"), 0.0);
	EXPECT_FALSE(getRunner().isAsync());
}

/*
 * 跳过目标（触发条件）：
 * - init_sel_mocker：WtBtRunner.cpp:311 还要 register_task(..., trdtpl, session)，
 *   未加载交易时段表时进的是 WtBtCore 的模板解析路径，不在本模块可证伪范围。
 * - hft_buy / hft_sell / hft_cancel / hft_cancel_all：HftMocker 下单要 get_commodity_info，
 *   空存储下 do_set_position 在 HftMocker.cpp:1107-1109 直接 return，回报链路拿不到可核对的量价。
 * - cta_get_bars / cta_get_ticks：stra_get_bars 走 WTSKlineStore，未 init 存储会进 IO；
 *   外层虽有 catch(...)（WtBtPorter.cpp:436-457/474-496），但段错误不可捕获，故整条不启用。
 * - cta_log_text / *_save_userdata：写 ./outputs_bt 下的策略日志与 userdata 文件（副作用落盘）。
 * - run_backtest / stop_backtest / init_backtest：线程 + 全局日志单例，见 r9_exp_mocker_bridge_test.cpp。
 */
TEST(R9WiredMockerCapi, DISABLED_OrderAndSlicePathsNeedLoadedStorage)
{
	EXPECT_TRUE(true);
}
