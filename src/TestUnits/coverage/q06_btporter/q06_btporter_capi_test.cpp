/*
 * Q06 - WtBtPorter 回测侧 C 导出层：未初始化 mocker / 空加载器的拒绝路径
 *
 * 覆盖 src/WtBtPorter/WtBtPorter.cpp 的导出入口：
 * get_version / write_log / feed_raw_bars / feed_raw_ticks / feed_adj_factors /
 * set_time_range / enable_tick / clear_cache，以及 cta_* / sel_* / hft_* 三族里
 * 全部 `mocker == NULL` 即返回中性值的分支；
 * 以及 src/WtBtPorter/WtBtRunner.cpp 的 replayer() 直取分支（get_date / get_min_time /
 * get_trading_date / get_cur_price 在未回放时恒为 0）。
 *
 * 不跑回测：run_backtest / stop_backtest / release_backtest / config_backtest /
 * init_backtest / init_*_mocker 全部跳过（见文末 DISABLED_ 与 manifest）。
 * 只读生产代码；缺陷记录见 docs/迭代计划/迭代01/bugs/q06_porters.md。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtPorter.h"
#include "WtBtPorter/WtBtRunner.h"

#include "Includes/WTSStruct.h"

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace {

	const CtxHandler kAnyHandle = 0x00AA00AAUL;	// 回测侧句柄本身就被忽略，这里只保证非 0

	uint32_t g_pos_cb_calls = 0;
	std::string g_pos_cb_code;
	double g_pos_cb_value = -1.0;
	bool g_pos_cb_islast = false;

	void bt_pos_cb(CtxHandler /*cHandle*/, const char* stdCode, double position, bool isLast)
	{
		g_pos_cb_calls++;
		g_pos_cb_code = (stdCode == NULL) ? "<null>" : stdCode;
		g_pos_cb_value = position;
		g_pos_cb_islast = isLast;
	}

	void bt_bars_cb(CtxHandler /*cHandle*/, const char* /*stdCode*/, const char* /*period*/,
		WTSBarStruct* /*bar*/, WtUInt32 /*count*/, bool /*isLast*/)
	{
	}

	void bt_ticks_cb(CtxHandler /*cHandle*/, const char* /*stdCode*/,
		WTSTickStruct* /*tick*/, WtUInt32 /*count*/, bool /*isLast*/)
	{
	}

	void bt_ordque_cb(CtxHandler /*cHandle*/, const char* /*stdCode*/,
		WTSOrdQueStruct* /*item*/, WtUInt32 /*count*/, bool /*isLast*/)
	{
	}

	void bt_orddtl_cb(CtxHandler /*cHandle*/, const char* /*stdCode*/,
		WTSOrdDtlStruct* /*item*/, WtUInt32 /*count*/, bool /*isLast*/)
	{
	}

	void bt_trans_cb(CtxHandler /*cHandle*/, const char* /*stdCode*/,
		WTSTransStruct* /*item*/, WtUInt32 /*count*/, bool /*isLast*/)
	{
	}
}

TEST(Q06BtPorterCapi, VersionStringHasPlatformPrefix)
{
	const char* ver = get_version();
	ASSERT_STREQ(ver, get_version());	// 命中缓存分支

	std::string s(ver);
	EXPECT_EQ(s.compare(0, 5, "UNIX "), 0) << s;
	EXPECT_NE(s.find("Build@"), std::string::npos) << s;
}

TEST(Q06BtPorterCapi, WriteLogAcceptsNamedAndEmptyCategory)
{
	write_log(LOG_LEVEL_DEBUG, "q06 bt debug", "");
	write_log(LOG_LEVEL_INFO, "q06 bt info", "btporter");
	write_log(LOG_LEVEL_WARN, "q06 bt warn", "btporter");
	write_log(LOG_LEVEL_ERROR, "q06 bt error", "");
	SUCCEED();
}

/* 三个 feed 入口在未注册外部加载器时都应只记错误日志就返回 */
TEST(Q06BtPorterCapi, FeedEntriesRefuseWithoutExtLoader)
{
	getRunner().registerExtDataLoader(NULL, NULL, NULL, NULL, false);

	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	feed_raw_bars(&bar, 1);
	feed_raw_bars(NULL, 0);

	WTSTickStruct tick;
	memset(&tick, 0, sizeof(tick));
	feed_raw_ticks(&tick, 1);

	uint32_t dates[1] = { 20240601 };
	double factors[1] = { 1.0 };
	feed_adj_factors("SSE.600000", dates, factors, 1);
	SUCCEED();
}

/* CTA 族：cta_mocker() 为空时的中性返回值 */
TEST(Q06BtPorterCapi, CtaEntriesRefuseWithoutMocker)
{
	ASSERT_EQ(getRunner().cta_mocker(), nullptr);

	EXPECT_DOUBLE_EQ(cta_get_position(kAnyHandle, "SSE.600000", true, ""), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_position_profit(kAnyHandle, "SSE.600000"), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_position_avgpx(kAnyHandle, "SSE.600000"), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_detail_cost(kAnyHandle, "SSE.600000", "t1"), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_detail_profit(kAnyHandle, "SSE.600000", "t1", 0), 0.0);
	EXPECT_EQ(cta_get_detail_entertime(kAnyHandle, "SSE.600000", "t1"), 0ULL);
	EXPECT_EQ(cta_get_first_entertime(kAnyHandle, "SSE.600000"), 0ULL);
	EXPECT_EQ(cta_get_last_entertime(kAnyHandle, "SSE.600000"), 0ULL);
	EXPECT_EQ(cta_get_last_exittime(kAnyHandle, "SSE.600000"), 0ULL);
	EXPECT_DOUBLE_EQ(cta_get_last_enterprice(kAnyHandle, "SSE.600000"), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_fund_data(kAnyHandle, 0), 0.0);
	EXPECT_EQ(cta_get_bars(kAnyHandle, "SSE.600000", "m1", 10, false, bt_bars_cb), 0U);
	EXPECT_EQ(cta_get_ticks(kAnyHandle, "SSE.600000", 10, bt_ticks_cb), 0U);
	EXPECT_STREQ(cta_load_userdata(kAnyHandle, "k", "bt_fallback"), "bt_fallback");
	// 见缺陷 Q06-003：与 cta_load_userdata 不同，entertag 直接 return 0
	const char* tag = cta_get_last_entertag(kAnyHandle, "SSE.600000");
	EXPECT_TRUE(tag == NULL || strlen(tag) == 0) << (tag == NULL ? "<null>" : tag);
	EXPECT_FALSE(cta_register_index_line(kAnyHandle, "idx", "line", 0));
	EXPECT_FALSE(cta_add_index_baseline(kAnyHandle, "idx", "line", 1.0));
	EXPECT_FALSE(cta_set_index_value(kAnyHandle, "idx", "line", 1.0));

	cta_enter_long(kAnyHandle, "SSE.600000", 100, "q06", 0, 0);
	cta_exit_long(kAnyHandle, "SSE.600000", 100, "q06", 0, 0);
	cta_enter_short(kAnyHandle, "SSE.600000", 100, "q06", 0, 0);
	cta_exit_short(kAnyHandle, "SSE.600000", 100, "q06", 0, 0);
	cta_set_position(kAnyHandle, "SSE.600000", 100, "q06", 0, 0);
	cta_log_text(kAnyHandle, LOG_LEVEL_INFO, "q06 bt cta log");
	cta_log_text(kAnyHandle, 99, "q06 bt cta unknown level");
	cta_save_userdata(kAnyHandle, "k", "v");
	cta_sub_ticks(kAnyHandle, "SSE.600000");
	cta_sub_bar_events(kAnyHandle, "SSE.600000", "m1");
	cta_set_chart_kline(kAnyHandle, "SSE.600000", "m1");
	cta_add_chart_mark(kAnyHandle, 10.0, "icon", "tag");
	cta_register_index(kAnyHandle, "idx", 0);
	SUCCEED();
}

/* enum_position 的终止标记：即使 mocker 为空也要收口 */
TEST(Q06BtPorterCapi, EnumPositionStillEmitsTerminatorWithoutMocker)
{
	g_pos_cb_calls = 0;
	g_pos_cb_code.clear();
	g_pos_cb_value = -1.0;
	g_pos_cb_islast = false;

	cta_get_all_position(kAnyHandle, bt_pos_cb);
	EXPECT_EQ(g_pos_cb_calls, 1U);
	EXPECT_EQ(g_pos_cb_code, "");
	EXPECT_DOUBLE_EQ(g_pos_cb_value, 0.0);
	EXPECT_TRUE(g_pos_cb_islast);

	g_pos_cb_calls = 0;
	sel_get_all_position(kAnyHandle, bt_pos_cb);
	EXPECT_EQ(g_pos_cb_calls, 1U);
	EXPECT_TRUE(g_pos_cb_islast);
}

/* SEL 族 */
TEST(Q06BtPorterCapi, SelEntriesRefuseWithoutMocker)
{
	ASSERT_EQ(getRunner().sel_mocker(), nullptr);

	EXPECT_DOUBLE_EQ(sel_get_position(kAnyHandle, "SSE.600000", true, ""), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_position_profit(kAnyHandle, "SSE.600000"), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_position_avgpx(kAnyHandle, "SSE.600000"), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_detail_cost(kAnyHandle, "SSE.600000", "t1"), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_detail_profit(kAnyHandle, "SSE.600000", "t1", 0), 0.0);
	EXPECT_EQ(sel_get_detail_entertime(kAnyHandle, "SSE.600000", "t1"), 0ULL);
	EXPECT_EQ(sel_get_first_entertime(kAnyHandle, "SSE.600000"), 0ULL);
	EXPECT_EQ(sel_get_last_entertime(kAnyHandle, "SSE.600000"), 0ULL);
	EXPECT_EQ(sel_get_last_exittime(kAnyHandle, "SSE.600000"), 0ULL);
	EXPECT_DOUBLE_EQ(sel_get_last_enterprice(kAnyHandle, "SSE.600000"), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_fund_data(kAnyHandle, 2), 0.0);
	EXPECT_EQ(sel_get_bars(kAnyHandle, "SSE.600000", "d1", 5, bt_bars_cb), 0U);
	EXPECT_EQ(sel_get_ticks(kAnyHandle, "SSE.600000", 5, bt_ticks_cb), 0U);
	EXPECT_STREQ(sel_load_userdata(kAnyHandle, "k", "sel_bt_default"), "sel_bt_default");

	sel_set_position(kAnyHandle, "SSE.600000", 100, "q06");
	sel_log_text(kAnyHandle, LOG_LEVEL_ERROR, "q06 bt sel log");
	sel_save_userdata(kAnyHandle, "k", "v");
	sel_sub_ticks(kAnyHandle, "SSE.600000");
	SUCCEED();
}

/*
 * HFT 族：_hft_mocker 未出现在 WtBtRunner 构造列表里（缺陷 Q06-004），
 * 静态单例靠「静态存储先零初始化」才恰好为 NULL；这里把该前提写成显式断言，
 * 前提不成立时直接失败而不是走未定义行为。
 */
TEST(Q06BtPorterCapi, HftEntriesRefuseWithoutMocker)
{
	ASSERT_EQ(getRunner().hft_mocker(), nullptr);

	EXPECT_DOUBLE_EQ(hft_get_position(kAnyHandle, "SHFE.au2406", true), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_position_profit(kAnyHandle, "SHFE.au2406"), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_position_avgpx(kAnyHandle, "SHFE.au2406"), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_undone(kAnyHandle, "SHFE.au2406"), 0.0);
	EXPECT_FALSE(hft_cancel(kAnyHandle, 1));
	EXPECT_STREQ(hft_cancel_all(kAnyHandle, "SHFE.au2406", true), "");
	EXPECT_STREQ(hft_buy(kAnyHandle, "SHFE.au2406", 100.0, 1, "q06", 0), "");
	EXPECT_STREQ(hft_sell(kAnyHandle, "SHFE.au2406", 100.0, 1, "q06", 0), "");
	EXPECT_STREQ(hft_load_userdata(kAnyHandle, "k", "hft_bt_default"), "hft_bt_default");
	EXPECT_EQ(hft_get_bars(kAnyHandle, "SHFE.au2406", "m1", 3, bt_bars_cb), 0U);
	EXPECT_EQ(hft_get_ticks(kAnyHandle, "SHFE.au2406", 3, bt_ticks_cb), 0U);
	EXPECT_EQ(hft_get_ordque(kAnyHandle, "SHFE.au2406", 3, bt_ordque_cb), 0U);
	EXPECT_EQ(hft_get_orddtl(kAnyHandle, "SHFE.au2406", 3, bt_orddtl_cb), 0U);
	EXPECT_EQ(hft_get_trans(kAnyHandle, "SHFE.au2406", 3, bt_trans_cb), 0U);

	hft_log_text(kAnyHandle, LOG_LEVEL_DEBUG, "q06 bt hft log");
	hft_sub_ticks(kAnyHandle, "SHFE.au2406");
	hft_sub_order_queue(kAnyHandle, "SHFE.au2406");
	hft_sub_order_detail(kAnyHandle, "SHFE.au2406");
	hft_sub_transaction(kAnyHandle, "SHFE.au2406");
	hft_save_userdata(kAnyHandle, "k", "v");
	SUCCEED();
}

/*
 * 回放器还没跑过：日期/时间/价格三个直取入口都是成员初值或空表查找，
 * 因此 0 是可从 HisDataReplayer 构造列表推导出的期望值。
 */
TEST(Q06BtPorterCapi, ReplayerTimeAndPriceGettersAreZeroBeforeRun)
{
	EXPECT_EQ(cta_get_tdate(), 0U);
	EXPECT_EQ(cta_get_date(), 0U);
	EXPECT_EQ(cta_get_time(), 0U);
	EXPECT_EQ(sel_get_tdate(), 0U);
	EXPECT_EQ(sel_get_date(), 0U);
	EXPECT_EQ(sel_get_time(), 0U);
	EXPECT_EQ(hft_get_date(), 0U);
	EXPECT_EQ(hft_get_time(), 0U);
	EXPECT_EQ(hft_get_secs(), 0U);
	EXPECT_DOUBLE_EQ(cta_get_price("SSE.600000"), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_price("SSE.600000"), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_price("SHFE.au2406"), 0.0);
}

/* 时间区间与 tick 开关只是写回放器标记位，不起线程 */
TEST(Q06BtPorterCapi, TimeRangeAndTickSwitchAreSideEffectFree)
{
	set_time_range(202401010000ULL, 202406011500ULL);
	enable_tick(false);
	enable_tick(true);
	clear_cache();
	SUCCEED();
}

/*
 * 跳过目标（详见 manifest）：
 * - init_backtest / config_backtest / run_backtest / stop_backtest / release_backtest：
 *   读配置、建存储、起回放线程并 join，属阻塞路径。
 * - init_cta_mocker / init_sel_mocker / init_hft_mocker：内部 _replayer.register_sink()
 *   与 register_task() 依赖已装载的行情存储与交易时段表。
 * - get_raw_stdcode：依赖回放器的代码表。
 * - cta_get_day_price / sel_get_day_price：enable_tick 为真时会去取 last_tick。
 * - cta_step / hft_step：依赖异步回放线程的等待原语。
 */
TEST(Q06BtPorterCapi, DISABLED_BacktestLifecycleNeedsStorageAndThreads)
{
	EXPECT_TRUE(true);
}
