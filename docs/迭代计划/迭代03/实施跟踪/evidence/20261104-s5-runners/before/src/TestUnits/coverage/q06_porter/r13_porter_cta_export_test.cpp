/*
 * R13 - WtPorter C 导出族：CTA 分支（句柄已建立的真分支）
 *
 * 目标：src/WtPorter/WtPorter.cpp（分派口径 missed=435/848）。
 * q06 lane 只走过 `ctx == NULL` 的拒绝支（见 q06_porter_capi_test.cpp），
 * 本文件用 create_cta_context() 拿到**真实句柄**，把 cta_* 导出函数的
 * 「命中上下文」那一支（也就是 WtPorter.cpp 里被计入 missed 的主体）打通。
 *
 * 边界：
 *  - 只用 WtPorter 这一族 extern "C" 入口，不引用 WtBtPorter 同名工厂；
 *  - 不起线程、不 sleep、不连柜台、不开共享内存、不读挂钟；
 *  - 会解引用未绑定指针（HFT 的 _trader）、或会落文件/建目录（on_init/save_data）
 *    的入口一律整条 DISABLED_，触发条件写在用例内注释里；
 *  - 生产代码只读，缺陷见 docs/迭代计划/迭代01/bugs/R13_wtporter.md。
 *
 * 断言口径：期望值一律由本用例自己喂进去的输入算出，且只断言源码里
 * 已逐行确认过的返回值（`_pos_map`/`_sig_map` 为空 -> 0；`_user_datas` 纯 map 读写）。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "WtCore/WtHelper.h"
#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

#include "Includes/WTSStruct.h"
#include "Share/CodeHelper.hpp"

USING_NS_WTP;

/* WtPorter.cpp:33 的进程单例；本族导出入口全部经它转发 */
extern WtRtRunner& getRunner();

namespace {

	/* 三段式分月合约键（主控规则 1：两段式会把点号后整段当 product） */
	const char* kMonthCode = "SHFE.rb.2405";
	/* 只做 map 键用、不要求夹具里注册过的原始串 */
	const char* kAbsentCode = "ZZZ.Nope.0000";

	struct CtaRec
	{
		uint32_t	init = 0;
		uint32_t	tick = 0;
		uint32_t	calc = 0;
		uint32_t	bar = 0;
		uint32_t	sess = 0;
		uint32_t	cond = 0;
		CtxHandler	last_handle = 0;
		uint32_t	last_date = 0;
		uint32_t	last_time = 0;
		uint32_t	last_tdate = 0;
		bool		last_isbegin = false;
		std::string	last_code, last_period, last_usertag;
		double		last_target = 0;
		double		last_price = 0;
		void clear() { *this = CtaRec(); }
	};

	CtaRec g_cta;

	void cta_cb_init(CtxHandler cHandle) { g_cta.init++; g_cta.last_handle = cHandle; }
	void cta_cb_tick(CtxHandler cHandle, const char* stdCode, WTSTickStruct*)
	{
		g_cta.tick++; g_cta.last_handle = cHandle;
		g_cta.last_code = (stdCode == NULL) ? "<null>" : stdCode;
	}
	void cta_cb_calc(CtxHandler cHandle, WtUInt32 curDate, WtUInt32 curTime)
	{
		g_cta.calc++; g_cta.last_handle = cHandle;
		g_cta.last_date = curDate; g_cta.last_time = curTime;
	}
	void cta_cb_bar(CtxHandler cHandle, const char* stdCode, const char* period, WTSBarStruct*)
	{
		g_cta.bar++; g_cta.last_handle = cHandle;
		g_cta.last_code = (stdCode == NULL) ? "<null>" : stdCode;
		g_cta.last_period = (period == NULL) ? "<null>" : period;
	}
	void cta_cb_sess(CtxHandler cHandle, WtUInt32 curTDate, bool isBegin)
	{
		g_cta.sess++; g_cta.last_handle = cHandle;
		g_cta.last_tdate = curTDate; g_cta.last_isbegin = isBegin;
	}
	void cta_cb_cond(CtxHandler cHandle, const char* stdCode, double target, double price, const char* usertag)
	{
		g_cta.cond++; g_cta.last_handle = cHandle;
		g_cta.last_code = (stdCode == NULL) ? "<null>" : stdCode;
		g_cta.last_target = target; g_cta.last_price = price;
		g_cta.last_usertag = (usertag == NULL) ? "<null>" : usertag;
	}

	void register_cta_recorder()
	{
		register_cta_callbacks(cta_cb_init, cta_cb_tick, cta_cb_calc, cta_cb_bar,
			cta_cb_sess, cta_cb_cond);
	}

	/* 未知句柄：本进程里绝不会 createCtaContext 出这个 id */
	const CtxHandler kBadHandle = 0x7FFF0001UL;
}

/*
 * 夹具：每个用例新建一个 CTA 上下文（句柄互不相同），
 * 注册六个 CTA 槽位，用例结束后撤销槽位（不改单例的上下文表，
 * 因为 WtPorter 没有公开的销毁入口 —— 见 BUG-R13-006）。
 */
class R13PorterCtaExport : public ::testing::Test
{
protected:
	CtxHandler	_h = 0;

	void SetUp() override
	{
		g_cta.clear();
		register_cta_recorder();

		_h = create_cta_context("r13_cta_export", 3);
		ASSERT_NE(_h, 0U) << "create_cta_context 返回 0 说明上下文没进 _cta_engine 的 _ctx_map";
		ASSERT_TRUE(getRunner().getCtaContext(_h) != NULL);

		// 规则 1 要求的键形自检：夹具没注册品种，所以只断言 CodeHelper 的字符串分层
		// （源码 src/Share/CodeHelper.hpp:601-655），不要求能查到合约。
		// CodeInfo 的字段是 char 数组（CodeHelper.hpp:101 收尾），一律用 STREQ 比内容。
		CodeHelper::CodeInfo three = CodeHelper::extractStdCode(kMonthCode, NULL);
		EXPECT_STREQ(three._exchg, "SHFE");
		EXPECT_STREQ(three._product, "rb");
		// 末段是 4 位数字 -> 分月合约，product 拼月份（CodeHelper.hpp:636-641）
		EXPECT_STREQ(three._code, "rb2405");

		CodeHelper::CodeInfo two = CodeHelper::extractStdCode("SHFE.rb2405", NULL);
		// 两段式：点号后整段当 product，且 code 与 product 相同（:616-621）
		EXPECT_STREQ(two._product, "rb2405");
		EXPECT_STREQ(two._code, "rb2405");

		CodeHelper::CodeInfo plain = CodeHelper::extractStdCode("SSE.STK.600000", NULL);
		EXPECT_STREQ(plain._product, "STK");
		EXPECT_STREQ(plain._code, "600000");
	}

	void TearDown() override
	{
		register_cta_callbacks(NULL, NULL, NULL, NULL, NULL, NULL);
		g_cta.clear();
	}
};

/*
 * 真实句柄必须能查到上下文，且同一个句柄反复查是同一个对象。
 * WtCtaEngine::getContext（WtCore/WtCtaEngine.cpp:139-146）返回 it->second（拷贝，非 std::move），
 * 所以第三次取仍然非空 —— 这条用来钉住「不被 R4 那类掏空缓存的写法替换掉」。
 */
TEST_F(R13PorterCtaExport, handle_resolves_to_the_same_context_on_repeated_lookups)
{
	CtaContextPtr first = getRunner().getCtaContext(_h);
	CtaContextPtr second = getRunner().getCtaContext(_h);
	CtaContextPtr third = getRunner().getCtaContext(_h);

	ASSERT_TRUE(first != NULL);
	ASSERT_TRUE(second != NULL);
	ASSERT_TRUE(third != NULL);
	EXPECT_EQ(first.get(), second.get());
	EXPECT_EQ(second.get(), third.get());
	EXPECT_EQ(third->id(), (uint32_t)_h);
}

/* 未知句柄仍然要被 ctx==NULL 拦住（对照支，防止我把上面那条写反） */
TEST_F(R13PorterCtaExport, unknown_handle_still_resolves_to_null_context)
{
	EXPECT_TRUE(getRunner().getCtaContext(kBadHandle) == NULL);
	EXPECT_DOUBLE_EQ(cta_get_position(kBadHandle, kMonthCode, true, ""), 0.0);
}

/*
 * cta_save/cta_load_userdata：CtaStraBaseCtx::stra_save/load_user_data
 * （WtCore/CtaStraBaseCtx.cpp:1746-1759）就是 _user_datas 这个 map 的读写，
 * 因此精确回读是硬期望；缺失键必须回到 defVal。
 */
TEST_F(R13PorterCtaExport, userdata_roundtrips_exact_value_and_falls_back_for_absent_key)
{
	cta_save_userdata(_h, "k1", "r13-value-one");
	cta_save_userdata(_h, "k2", "");
	cta_save_userdata(_h, "k3", "0.123456789");

	EXPECT_STREQ(cta_load_userdata(_h, "k1", "DEF"), "r13-value-one");
	EXPECT_STREQ(cta_load_userdata(_h, "k2", "DEF"), "");
	EXPECT_STREQ(cta_load_userdata(_h, "k3", "DEF"), "0.123456789");
	// 未写入的键：stra_load_user_data 的 find 末支 return defVal
	EXPECT_STREQ(cta_load_userdata(_h, "never_written", "DEF"), "DEF");

	// 同键覆盖：map 赋值语义，最后一次写胜出
	cta_save_userdata(_h, "k1", "r13-value-two");
	EXPECT_STREQ(cta_load_userdata(_h, "k1", "DEF"), "r13-value-two");
}

/*
 * 上下文之间的用户数据必须互相隔离：
 * 若导出层误把 _user_datas 提到静态/全局（R4 口径的静默串数据），这条立刻红。
 */
TEST_F(R13PorterCtaExport, userdata_is_per_context_not_shared_between_two_ctas)
{
	const CtxHandler other = create_cta_context("r13_cta_export_b", 5);
	ASSERT_NE(other, 0U);
	ASSERT_NE(other, _h);

	cta_save_userdata(_h, "owner", "ctx-a");
	cta_save_userdata(other, "owner", "ctx-b");

	EXPECT_STREQ(cta_load_userdata(_h, "owner", "X"), "ctx-a");
	EXPECT_STREQ(cta_load_userdata(other, "owner", "X"), "ctx-b");
	// b 里没有的键不受 a 影响
	EXPECT_STREQ(cta_load_userdata(other, "only_in_a", "X"), "X");
}

/*
 * 新建上下文（未 on_init、未成交）的持仓族读数必须精确为 0：
 * stra_get_position: _sig_map 与 _pos_map 都 miss -> return totalPos(0)（CtaStraBaseCtx.cpp:1824-1836）
 * stra_get_position_profit: find miss -> return 0（:1887-1895）
 * stra_get_position_avgpx:  find miss -> return 0（:1867-1871）
 * 0.0 是「整数量级」的返回值，不涉及累加误差，所以用 EXPECT_DOUBLE_EQ。
 */
TEST_F(R13PorterCtaExport, virgin_context_position_getters_are_exactly_zero)
{
	EXPECT_DOUBLE_EQ(cta_get_position(_h, kMonthCode, true, ""), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_position(_h, kMonthCode, false, ""), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_position(_h, kMonthCode, true, "tag-a"), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_position(_h, kAbsentCode, false, ""), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_position_profit(_h, kMonthCode), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_position_avgpx(_h, kMonthCode), 0.0);
}

/*
 * 用户数据不能污染持仓读数，持仓读数也不该反过来改状态：
 * 这条同时覆盖 cta_log_text 的四个级别 + default 支（WtPorter.cpp:918-941，
 * 落到 CtaStraBaseCtx::stra_log_* -> WTSLogger::log_dyn_raw，不建文件、不起线程），
 * 并断言跑完一圈后状态与之前完全一致。
 */
TEST_F(R13PorterCtaExport, log_text_routes_four_levels_without_changing_context_state)
{
	cta_save_userdata(_h, "state", "before-log");

	cta_log_text(_h, LOG_LEVEL_DEBUG, "r13 debug line");
	cta_log_text(_h, LOG_LEVEL_INFO, "r13 info line");
	cta_log_text(_h, LOG_LEVEL_WARN, "r13 warn line");
	cta_log_text(_h, LOG_LEVEL_ERROR, "r13 error line");
	// switch 的 default 支：未知级别不映射到任何 stra_log_*，也不得崩。
	// PorterDefs.h:52-55 只声明了 DEBUG/INFO/WARN/ERROR = 0..3，4 以上是未定义值。
	cta_log_text(_h, LOG_LEVEL_ERROR + 5, "r13 unmapped level");

	EXPECT_STREQ(cta_load_userdata(_h, "state", "AFTER"), "before-log");
	EXPECT_DOUBLE_EQ(cta_get_position(_h, kMonthCode, true, ""), 0.0);
	// 日志走的是 WTSLogger，不是策略回调：六个 CTA 槽位一次都不该被调用
	EXPECT_EQ(g_cta.init, 0U);
	EXPECT_EQ(g_cta.tick, 0U);
	EXPECT_EQ(g_cta.calc, 0U);
	EXPECT_EQ(g_cta.bar, 0U);
}

/*
 * cta_get_fund_data 的三个 flag 都只读 _fund_info 这个成员（CtaStraBaseCtx.cpp:1709-1717）。
 * _cta_engine 是静态单例的成员 -> 静态存储零初始化，所以三个 flag 都是精确 0。
 * 这条的意义在于钉住「未知 flag 走 default 返回 0」而不是脏数据。
 */
TEST_F(R13PorterCtaExport, fund_data_reads_zeroed_fund_struct_for_every_flag)
{
	EXPECT_DOUBLE_EQ(cta_get_fund_data(_h, 0), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_fund_data(_h, 1), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_fund_data(_h, 2), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_fund_data(kBadHandle, 0), 0.0);
}

/*
 * 崩溃/副作用面，整条关闭。
 * 触发条件：cta_set_position / cta_enter_long / cta_exit_long / cta_enter_short /
 * cta_exit_short 会进 WtEngine::do_set_position（WtCore/WtEngine.cpp:956+），
 * 那里直接解引用未 init 的 _data_mgr/_hot_mgr（本夹具没有 config_porter，
 * 因为 config() 的 L489-494 被 BUG-R6-001/002 卡在空指针上）。
 * 留白行数与接缝需求登记在 manifests/R13_wtporter.md 第四节。
 */
TEST_F(R13PorterCtaExport, DISABLED_position_mutation_entries_deref_uninitialized_engine)
{
	cta_set_position(_h, kMonthCode, 2, "r13", 0, 0);
	cta_enter_long(_h, kMonthCode, 2, "r13", 0, 0);
	cta_exit_long(_h, kMonthCode, 1, "r13", 0, 0);
	cta_enter_short(_h, kMonthCode, 2, "r13", 0, 0);
	cta_exit_short(_h, kMonthCode, 1, "r13", 0, 0);
}

/*
 * 崩溃面，整条关闭。
 * 触发条件：cta_get_bars/cta_get_ticks 在句柄有效时进 WtEngine::get_kline_slice
 * 与 get_tick_slice（WtEngine.cpp:592-644），依赖 _data_mgr；未 config 的引擎里
 * 它是 NULL，导出层虽然包了 catch(...)（WtPorter.cpp:505/551），SIGSEGV 抓不到。
 */
TEST_F(R13PorterCtaExport, DISABLED_history_slice_entries_need_a_data_manager_seam)
{
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	const uint32_t bars = cta_get_bars(_h, kMonthCode, "m1", 3, false,
		[](CtxHandler, const char*, const char*, WTSBarStruct*, WtUInt32, bool) {});
	EXPECT_EQ(bars, 0U);
}
