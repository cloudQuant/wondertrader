/*
 * R13 - WtPorter C 导出族：SEL 与 HFT 分支（真实句柄）
 *
 * 目标：src/WtPorter/WtPorter.cpp（missed=435/848）里 SEL 段（1120-1660）
 * 与 HFT 段（1650-2255）的「命中上下文」支；以及 create_sel_context /
 * create_hft_context 两个 C 入口（WtPorter.cpp:1135、:1652）。
 *
 * 已确认的安全边界（逐行读过实现）：
 *  - SelStraBaseCtx 的 stra_get_position/avgpx/position_profit/detail_cost/detail_profit
 *    （WtCore/SelStraBaseCtx.cpp:1144/1176/1196/1225/1244）在 _pos_map miss 时 return 0；
 *  - StraFundInfo 的构造函数 memset 全 0（SelStraBaseCtx.h:909-913、HftStraBaseCtx.h:790-794），
 *    所以资金族任意 flag 的取值都是精确 0.0；
 *  - _user_datas 是纯 map（SelStraBaseCtx.cpp:1066-1079）；
 *  - HFT 侧 _pos_map 之外的持仓/委托入口都要解引用 _trader，而 createHftContext
 *    在拿不到交易通道时**不设置也不初始化** _trader（HftStraBaseCtx.h:673 无默认初值，
 *    构造函数 HftStraBaseCtx.cpp:41-48 也没列它）-> 全部进 DISABLED_，见 BUG-R13-001。
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

	const CtxHandler kBadHandle = 0x7FFF0001UL;
	/* 三段式分月合约键（只当 map 键用；本夹具没有注册品种） */
	const char* kSelCode = "SSE.STK.600000";
	const char* kHftCode = "SHFE.rb.2405";

	struct PosRec
	{
		uint32_t	calls = 0;
		std::string	last_code;
		double		last_qty = -1;
		bool		last_islast = false;
		void clear() { *this = PosRec(); }
	};

	PosRec g_pos;

	void pos_cb(CtxHandler /*cHandle*/, const char* stdCode, double position, bool isLast)
	{
		g_pos.calls++;
		g_pos.last_code = (stdCode == NULL) ? "<null>" : stdCode;
		g_pos.last_qty = position;
		g_pos.last_islast = isLast;
	}

	struct HftRec
	{
		uint32_t	pos = 0;
		CtxHandler	last_handle = 0;
		std::string	last_code;
		bool		last_islong = false;
		double		last_prevol = 0, last_preavail = 0, last_newvol = 0, last_newavail = 0;
		void clear() { *this = HftRec(); }
	};

	HftRec g_hft;

	void hft_pos_cb(CtxHandler cHandle, const char* stdCode, bool isLong,
		double prevol, double preavail, double newvol, double newavail)
	{
		g_hft.pos++;
		g_hft.last_handle = cHandle;
		g_hft.last_code = (stdCode == NULL) ? "<null>" : stdCode;
		g_hft.last_islong = isLong;
		g_hft.last_prevol = prevol;
		g_hft.last_preavail = preavail;
		g_hft.last_newvol = newvol;
		g_hft.last_newavail = newavail;
	}

	/* HFT 其余 11 个槽位：本文件只需要持仓槽，其余一律不注册（保持静默） */
	void register_hft_pos_only()
	{
		register_hft_callbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
			NULL, hft_pos_cb);
	}
}

class R13PorterSelExport : public ::testing::Test
{
protected:
	CtxHandler	_h = 0;

	void SetUp() override
	{
		g_pos.clear();
		_h = create_sel_context("r13_sel_export", 20240601, 153000, "d",
			"CHINA", "TRADING", 2);
		ASSERT_NE(_h, 0U);
		ASSERT_TRUE(getRunner().getSelContext(_h) != NULL);
	}

	void TearDown() override { g_pos.clear(); }
};

/*
 * create_sel_context 的 period 形参在 WtRtRunner::createSelContext
 * （WtRtRunner.cpp:336-349）里映射到 TaskPeriodType，六种写法都能拿到句柄；
 * 未识别的写法落 TPT_None（:348-349）但**照样返回句柄**。
 * 这里断言的是「每个名字各一个上下文」，不是「未知周期被拒绝」。
 */
TEST_F(R13PorterSelExport, create_sel_context_accepts_every_documented_period_and_returns_distinct_handles)
{
	const char* periods[] = { "w", "m", "y", "min", "d" };
	CtxHandler prev = _h;
	for (const char* p : periods)
	{
		const CtxHandler h = create_sel_context("r13_sel_period", 20240602, 93000, p,
			"CHINA", "TRADING", 0);
		ASSERT_NE(h, 0U) << p;
		EXPECT_NE(h, prev) << p;
		EXPECT_TRUE(getRunner().getSelContext(h) != NULL) << p;
		prev = h;
	}

	const CtxHandler bogus = create_sel_context("r13_sel_bogus_period", 20240602, 93000,
		"not-a-period", "CHINA", "TRADING", 0);
	// 现状：未识别周期不返回错误，句柄仍然可用（登记见 BUG-R13-005）
	ASSERT_NE(bogus, 0U);
	EXPECT_TRUE(getRunner().getSelContext(bogus) != NULL);
	// 未知句柄仍然查不到，证明上面的句柄不是巧合相等
	EXPECT_TRUE(getRunner().getSelContext(kBadHandle) == NULL);
}

TEST_F(R13PorterSelExport, sel_userdata_roundtrips_exact_value_and_default)
{
	sel_save_userdata(_h, "k1", "r13-sel-value");
	sel_save_userdata(_h, "k2", "");
	EXPECT_STREQ(sel_load_userdata(_h, "k1", "DEF"), "r13-sel-value");
	EXPECT_STREQ(sel_load_userdata(_h, "k2", "DEF"), "");
	EXPECT_STREQ(sel_load_userdata(_h, "absent", "DEF"), "DEF");

	sel_save_userdata(_h, "k1", "r13-sel-second");
	EXPECT_STREQ(sel_load_userdata(_h, "k1", "DEF"), "r13-sel-second");
}

TEST_F(R13PorterSelExport, sel_virgin_position_getters_are_exactly_zero)
{
	EXPECT_DOUBLE_EQ(sel_get_position(_h, kSelCode, true, ""), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_position(_h, kSelCode, false, "tag-a"), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_position_profit(_h, kSelCode), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_position_avgpx(_h, kSelCode), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_detail_cost(_h, kSelCode, "tag-a"), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_detail_profit(_h, kSelCode, "tag-a", 0), 0.0);
	EXPECT_EQ(sel_get_first_entertime(_h, kSelCode), 0ULL);
	// StraFundInfo 由 memset 归零，任意 flag 的组合都是 0
	EXPECT_DOUBLE_EQ(sel_get_fund_data(_h, 0), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_fund_data(_h, 1), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_fund_data(_h, 2), 0.0);
}

/*
 * sel_get_all_position 的真分支：ctx->enum_position 只遍历 _pos_map/_sig_map
 * （SelStraBaseCtx.cpp:659-680，两个表都空 -> 0 次内层调用），
 * 随后 WtPorter.cpp:1258 补一次 ("",0,true) 终止回调。
 * 期望次数与内容由本用例自己喂的「空上下文」算出：恰好 1 次、isLast 为真。
 */
TEST_F(R13PorterSelExport, sel_all_position_emits_only_the_terminator_for_an_empty_context)
{
	sel_get_all_position(_h, pos_cb);
	EXPECT_EQ(g_pos.calls, 1U);
	EXPECT_EQ(g_pos.last_code, "");
	EXPECT_DOUBLE_EQ(g_pos.last_qty, 0.0);
	EXPECT_TRUE(g_pos.last_islast);
}

/*
 * sel_log_text 的四个级别 + default（WtPorter.cpp:1183-1206）。
 * 用「状态不变」来证伪：日志不许改用户数据表。
 */
TEST_F(R13PorterSelExport, sel_log_text_passes_all_levels_without_touching_state)
{
	sel_save_userdata(_h, "state", "before");
	sel_log_text(_h, LOG_LEVEL_DEBUG, "r13 sel debug");
	sel_log_text(_h, LOG_LEVEL_INFO, "r13 sel info");
	sel_log_text(_h, LOG_LEVEL_WARN, "r13 sel warn");
	sel_log_text(_h, LOG_LEVEL_ERROR, "r13 sel error");
	sel_log_text(_h, LOG_LEVEL_ERROR + 5, "r13 sel unmapped");
	EXPECT_STREQ(sel_load_userdata(_h, "state", "AFTER"), "before");
}

/* 崩溃面：sel_set_position/sel_sub_ticks 进 WtEngine::do_set_position 与 sub_tick，
 * 依赖 _data_mgr/_hot_mgr；本夹具没有（也拿不到）config() 成功的路径。 */
TEST_F(R13PorterSelExport, DISABLED_sel_mutation_and_subscription_need_engine_seam)
{
	sel_set_position(_h, kSelCode, 100, "r13");
	sel_sub_ticks(_h, kSelCode);
}

class R13PorterHftExport : public ::testing::Test
{
protected:
	CtxHandler	_h = 0;

	void SetUp() override
	{
		g_hft.clear();
		g_pos.clear();
		register_hft_pos_only();

		_h = create_hft_context("r13_hft_export", "R13_TRADER_NOT_EXISTS", false, 1);
		// 现状：绑定失败也返回可用句柄（WtRtRunner.cpp:318-332 只在 if(trdPtr) 里 setTrader）
		ASSERT_NE(_h, 0U);
		ASSERT_TRUE(getRunner().getHftContext(_h) != NULL);
	}

	void TearDown() override
	{
		register_hft_callbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
			NULL, NULL);
		g_hft.clear();
	}
};

/*
 * 只读 _pos_map 的两个 HFT 取值入口是安全的
 * （HftStraBaseCtx.cpp:601-609、:611-629 都是 find miss -> 0）。
 */
TEST_F(R13PorterHftExport, hft_map_only_position_getters_are_exactly_zero)
{
	EXPECT_DOUBLE_EQ(hft_get_position_profit(_h, kHftCode), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_position_avgpx(_h, kHftCode), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_position_profit(kBadHandle, kHftCode), 0.0);
}

TEST_F(R13PorterHftExport, hft_userdata_roundtrips_exact_value_and_default)
{
	hft_save_userdata(_h, "k1", "r13-hft-value");
	hft_save_userdata(_h, "k2", "");
	EXPECT_STREQ(hft_load_userdata(_h, "k1", "DEF"), "r13-hft-value");
	EXPECT_STREQ(hft_load_userdata(_h, "k2", "DEF"), "");
	EXPECT_STREQ(hft_load_userdata(_h, "absent", "DEF"), "DEF");
	hft_save_userdata(_h, "k1", "r13-hft-second");
	EXPECT_STREQ(hft_load_userdata(_h, "k1", "DEF"), "r13-hft-second");
}

TEST_F(R13PorterHftExport, hft_log_text_passes_all_levels_without_touching_state)
{
	hft_save_userdata(_h, "state", "before");
	hft_log_text(_h, LOG_LEVEL_DEBUG, "r13 hft debug");
	hft_log_text(_h, LOG_LEVEL_INFO, "r13 hft info");
	hft_log_text(_h, LOG_LEVEL_WARN, "r13 hft warn");
	hft_log_text(_h, LOG_LEVEL_ERROR, "r13 hft error");
	hft_log_text(_h, LOG_LEVEL_ERROR + 5, "r13 hft unmapped");
	EXPECT_STREQ(hft_load_userdata(_h, "state", "AFTER"), "before");
	// 日志不经过持仓槽位
	EXPECT_EQ(g_hft.pos, 0U);
}

/*
 * 崩溃/未初始化面（BUG-R13-001）：这几个入口都会解引用 _trader，
 * 而本夹具的 create_hft_context 因交易通道不存在而没有绑定 _trader，
 * HftStraBaseCtx.h:673 的 _trader 又没有被构造函数初始化 -> 野指针。
 * 触发条件：任一调用在 -DCOVERAGE 构建里稳定 SIGSEGV（不是可 catch 的异常）。
 */
TEST_F(R13PorterHftExport, DISABLED_hft_trader_bound_entries_deref_unbound_pointer)
{
	hft_get_position(_h, kHftCode, true);
	hft_get_undone(_h, kHftCode);
	hft_cancel(_h, 1);
	hft_cancel_all(_h, kHftCode, true);
	hft_buy(_h, kHftCode, 3500.0, 1, "r13", 0);
	hft_sell(_h, kHftCode, 3500.0, 1, "r13", 0);
	hft_sub_ticks(_h, kHftCode);
	hft_sub_order_queue(_h, kHftCode);
	hft_sub_order_detail(_h, kHftCode);
	hft_sub_transaction(_h, kHftCode);
}

/*
 * 崩溃面（BUG-R6-003/Q06-002 同族）：cta/sel/hft 三族的 getEngine() 直取入口
 * （WtPorter.cpp:857/872/883/894/905、1215/1226/1237/1421、1743/1755/1767/1779）
 * 全部无条件解引用 getRunner().getEngine()，未 config 时是 NULL。
 * 这 14 个入口约 42 行在「进程内 + 不 config」的口径下取不到样。
 */
TEST(R13PorterEngineSeam, DISABLED_engine_direct_getters_deref_null_engine)
{
	EXPECT_DOUBLE_EQ(cta_get_price(kHftCode), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_price(kSelCode), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_price(kHftCode), 0.0);
}
