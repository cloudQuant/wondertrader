/*
 * R83 - WtBtPorter C API 的 mocker 非空面：K线/tick 切片、下单/撤单、
 *       userdata/log、图表指标、订阅，以及 WtBtRunner::init*Mocker 的剩余支
 *
 * 目标：src/WtBtPorter/WtBtPorter.cpp
 *   - CTA 族 mocker 非空支（L354-1006）：五个下单入口、13 个持仓/明细/资金 getter、
 *     cta_get_bars/cta_get_ticks 的 NULL 切片支、日志四级+default、userdata 往返、
 *     订阅、图表/指标、cta_step 的非异步早退
 *   - SEL 族 mocker 非空支（L1018-1457）：userdata/log、持仓/明细 getter、
 *     sel_get_bars/sel_get_ticks 的 NULL 切片支、sel_set_position、订阅、枚举持仓
 *   - HFT 族 mocker 非空支（L1470-2001）：四个 getter、bars/ticks/ordque/orddtl/trans
 *     的 NULL 切片支、buy/sell/cancel/cancel_all、订阅、日志、userdata、hft_step
 *   目标：src/WtBtPorter/WtBtRunner.cpp
 *   - L266-283 initCtaMocker 的 hook / 比例滑点参数支
 *   - L285-297 initHftMocker 整段（含二次创建 delete 旧实例）
 *   - L299-313 initSelMocker 的比例滑点参数支
 *
 * 与既有 lane 的分工：q06 只取 `_mocker == NULL` 的中性返回；R9 只取了 CTA/HFT 的
 * 少量空簿 getter 与 init 接线；R46/R69 取 config/生命周期方法。本文件全部走
 * mocker 非空支，不重复上述行段。
 *
 * 单例清理：CTA/SEL 用例在 TearDown 中显式调用公开析构 + placement new 把
 * WtBtRunner 还原成刚构造状态（`_cta_mocker`/`_sel_mocker` 置 NULL、`_async=false`），
 * 这样同一个二进制整体运行时，后序 R9 的「mocker 为空」断言仍然成立。
 * 全程不碰 `_hft_mocker`（构造列表未初始化该成员，BUG-R46-001）：HFT 用例在
 * fork 子进程里初始化并在子进程退出时落计数器，父进程保持零初始化单例。
 * 所有 mocker 都以 persistData=false 创建，不起线程、不写文件、不跑回测。
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtPorter.h"
#include "WtBtPorter/WtBtRunner.h"

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace {

	const CtxHandler kHandle = 0x00AA00AAUL;
	const char* kFut = "SHFE.rb2510";
	const char* kStk = "SSE.600000";

	uint32_t g_bars_calls = 0;
	uint32_t g_ticks_calls = 0;
	uint32_t g_ordque_calls = 0;
	uint32_t g_orddtl_calls = 0;
	uint32_t g_trans_calls = 0;
	uint32_t g_pos_calls = 0;
	bool g_pos_last = false;

	void bars_cb(CtxHandler, const char*, const char*, WTSBarStruct*, WtUInt32, bool) { g_bars_calls++; }
	void ticks_cb(CtxHandler, const char*, WTSTickStruct*, WtUInt32, bool) { g_ticks_calls++; }
	void ordque_cb(CtxHandler, const char*, WTSOrdQueStruct*, WtUInt32, bool) { g_ordque_calls++; }
	void orddtl_cb(CtxHandler, const char*, WTSOrdDtlStruct*, WtUInt32, bool) { g_orddtl_calls++; }
	void trans_cb(CtxHandler, const char*, WTSTransStruct*, WtUInt32, bool) { g_trans_calls++; }

	void pos_cb(CtxHandler, const char* code, double qty, bool isLast)
	{
		g_pos_calls++;
		g_pos_last = isLast;
		(void)code;
		(void)qty;
	}

	void reset_counters()
	{
		g_bars_calls = g_ticks_calls = 0;
		g_ordque_calls = g_orddtl_calls = g_trans_calls = 0;
		g_pos_calls = 0;
		g_pos_last = false;
	}

	/* 用公开构造函数把 WtBtRunner 还原到初始状态（`_cta_mocker`/`_sel_mocker`
	 * 由构造列表置 NULL、`_async=false`）；不读/写任何私有成员。旧 mocker 与
	 * R46/R69 的堆实例一样故意不析构，避免依赖其析构实现的副作用。 */
	void reset_singleton_runner()
	{
		WtBtRunner& r = getRunner();
		r.~WtBtRunner();
		new (&r) WtBtRunner();
	}

	/* CTA/SEL 用例共用：TearDown 无条件还原单例。 */
	class R83BtPorterMockerBase : public ::testing::Test
	{
	protected:
		void TearDown() override
		{
			reset_singleton_runner();
			reset_counters();
		}
	};

	/* HFT 子进程回传的结果；父进程只校验这些值，不共享任何 mocker 状态。 */
	struct HftChildReport
	{
		int		fails;
		uint32_t	id1;
		uint32_t	id2;
		double	pos_valid;
		double	pos_all;
		double	pnl;
		double	avgpx;
		double	undone;
		int		buy_len;
		int		sell_len;
		int		cancel_ok;
		int		cancel_all_len;
		uint32_t	bars;
		uint32_t	ticks;
		uint32_t	ordque;
		uint32_t	orddtl;
		uint32_t	trans;
		int		userdata_ok;
		int		default_ok;
		int		logged;
	};

	inline void child_expect(HftChildReport& r, bool cond)
	{
		if (!cond)
			r.fails++;
	}

} // namespace

/* WtBtPorter.cpp:354-621 + 744-789：CTA mocker 非空时的持仓/明细/资金/时间 getter。
 * 空持仓簿上的返回值全部可从 CtaMocker 的 map 查找推出；回放器时钟未推进，
 * 时间/价格类为成员初值 0。init 传 hook=true、bRatioSlp=true，顺带覆盖
 * WtBtRunner.cpp:275-282 的两个构造参数支。 */
TEST_F(R83BtPorterMockerBase, CtaGettersRunOnWiredMocker)
{
	CtxHandler h = init_cta_mocker("r83_cta_getter", 0, true, false, false, true);
	ASSERT_GT(h, 0UL);
	ASSERT_NE(getRunner().cta_mocker(), nullptr);

	EXPECT_DOUBLE_EQ(cta_get_position(kHandle, kFut, true, ""), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_position(kHandle, kFut, false, "tag"), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_position_profit(kHandle, kFut), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_position_avgpx(kHandle, kFut), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_detail_cost(kHandle, kFut, "tag"), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_detail_profit(kHandle, kFut, "tag", 0), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_detail_profit(kHandle, kFut, "tag", 1), 0.0);
	EXPECT_EQ(cta_get_detail_entertime(kHandle, kFut, "tag"), 0ULL);
	EXPECT_EQ(cta_get_first_entertime(kHandle, kFut), 0ULL);
	EXPECT_EQ(cta_get_last_entertime(kHandle, kFut), 0ULL);
	EXPECT_EQ(cta_get_last_exittime(kHandle, kFut), 0ULL);
	EXPECT_DOUBLE_EQ(cta_get_last_enterprice(kHandle, kFut), 0.0);
	EXPECT_STREQ(cta_get_last_entertag(kHandle, kFut), "");

	/* 4 个有效 flag + default 支（CtaMocker.cpp:1885-1898） */
	EXPECT_DOUBLE_EQ(cta_get_fund_data(kHandle, 0), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_fund_data(kHandle, 1), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_fund_data(kHandle, 2), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_fund_data(kHandle, 3), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_fund_data(kHandle, 99), 0.0);

	EXPECT_EQ(cta_get_tdate(), 0U);
	EXPECT_EQ(cta_get_date(), 0U);
	EXPECT_EQ(cta_get_time(), 0U);
	EXPECT_DOUBLE_EQ(cta_get_price(kFut), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_day_price(kFut, 0), 0.0);

	/* WtBtPorter.cpp:589-603：空持仓表也必须补 isLast=true 的终止标记 */
	reset_counters();
	cta_get_all_position(kHandle, pos_cb);
	EXPECT_EQ(g_pos_calls, 1U);
	EXPECT_TRUE(g_pos_last);
}

/* WtBtPorter.cpp:431-497：mocker 非空但历史数据读取器未初始化时，
 * stra_get_bars/stra_get_ticks 都返回 NULL，导出层走 `return 0` 支。
 * d1/m5 两种周期分别走 HisDataReplayer 的日线与分钟线分派。 */
TEST_F(R83BtPorterMockerBase, CtaKlineAndTickSlicesAreEmptyWithoutStorage)
{
	CtxHandler h = init_cta_mocker("r83_cta_slice", 0, false, false, false, false);
	ASSERT_GT(h, 0UL);

	reset_counters();
	EXPECT_EQ(cta_get_bars(kHandle, kFut, "d1", 5, false, bars_cb), 0U);
	EXPECT_EQ(cta_get_bars(kHandle, kFut, "m5", 5, false, bars_cb), 0U);
	EXPECT_EQ(cta_get_ticks(kHandle, kFut, 0, ticks_cb), 0U);
	EXPECT_EQ(cta_get_ticks(kHandle, kFut, 3, ticks_cb), 0U);
	EXPECT_EQ(g_bars_calls, 0U) << "空切片不得触发回调";
	EXPECT_EQ(g_ticks_calls, 0U) << "空切片不得触发回调";
}

/* WtBtPorter.cpp:354-418 + 649-656 + 862-1006：五个下单入口在空基础数据下
 * 找不到 commodity info，只写错误日志即返回；订阅、图表与指标入口直接写
 * mocker 内部容器。cta_step 在非异步模式返回 false（L893-897）。 */
TEST_F(R83BtPorterMockerBase, CtaOrderSubscriptionsAndChartIndex)
{
	CtxHandler h = init_cta_mocker("r83_cta_order", 0, false, false, false, false);
	ASSERT_GT(h, 0UL);

	cta_enter_long(kHandle, kFut, 1.0, "r83", 0.0, 0.0);
	cta_exit_long(kHandle, kFut, 1.0, "r83", 0.0, 0.0);
	cta_enter_short(kHandle, kFut, 1.0, "r83", 0.0, 0.0);
	cta_exit_short(kHandle, kFut, 1.0, "r83", 0.0, 0.0);
	cta_set_position(kHandle, kFut, 1.0, "r83", 0.0, 0.0);
	/* 带触发价的分支也各走一次（仍在 commodity 守卫之后早退） */
	cta_enter_long(kHandle, kFut, 1.0, "r83", 450.0, 440.0);

	cta_sub_ticks(kHandle, kFut);
	cta_sub_bar_events(kHandle, kFut, "m1");
	cta_set_chart_kline(kHandle, kFut, "m1");
	cta_add_chart_mark(kHandle, 451.5, "buy", "r83");

	cta_register_index(kHandle, "r83_idx", 1);
	EXPECT_TRUE(cta_register_index_line(kHandle, "r83_idx", "line", 2));
	EXPECT_TRUE(cta_add_index_baseline(kHandle, "r83_idx", "line", 0.0));
	/* 未注册指标：两个 false 支 */
	EXPECT_FALSE(cta_register_index_line(kHandle, "r83_missing", "line", 2));
	EXPECT_FALSE(cta_add_index_baseline(kHandle, "r83_missing", "line", 0.0));
	/* 未处于 schedule，set_index_value 在 _is_in_schedule 守卫处返回 false */
	EXPECT_FALSE(cta_set_index_value(kHandle, "r83_idx", "line", 1.0));

	EXPECT_FALSE(cta_step(kHandle));
}

/* WtBtPorter.cpp:798-854 + 893-904：日志四个有效级别 + default，
 * userdata 存/取往返与默认值回落；cta_step 的 mocker 非空支仍因非异步返回 false。 */
TEST_F(R83BtPorterMockerBase, CtaUserDataAndLogLevels)
{
	CtxHandler h = init_cta_mocker("r83_cta_ud", 0, false, false, false, false);
	ASSERT_GT(h, 0UL);

	cta_save_userdata(kHandle, "round", "trip");
	EXPECT_STREQ(cta_load_userdata(kHandle, "round", "fallback"), "trip");
	EXPECT_STREQ(cta_load_userdata(kHandle, "absent", "fallback"), "fallback");

	cta_log_text(kHandle, LOG_LEVEL_DEBUG, "r83 cta debug");
	cta_log_text(kHandle, LOG_LEVEL_INFO, "r83 cta info");
	cta_log_text(kHandle, LOG_LEVEL_WARN, "r83 cta warn");
	cta_log_text(kHandle, LOG_LEVEL_ERROR, "r83 cta error");
	cta_log_text(kHandle, 99, "r83 cta unknown level");

	EXPECT_FALSE(cta_step(kHandle));
}

/* WtBtPorter.cpp:1018-1457：SEL mocker 非空时的 getter / 下单守卫 / 切片 /
 * 订阅 / 枚举。init_sel_mocker 传 bRatioSlp=true，覆盖 WtBtRunner.cpp:308 的参数支。 */
TEST_F(R83BtPorterMockerBase, SelGettersRunOnWiredMocker)
{
	CtxHandler h = init_sel_mocker("r83_sel_getter", 20240102, 93000, "d", "CHINA", "TRADING", 0, true);
	ASSERT_GT(h, 0UL);
	ASSERT_NE(getRunner().sel_mocker(), nullptr);

	EXPECT_DOUBLE_EQ(sel_get_position(kHandle, kStk, true, ""), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_position(kHandle, kStk, false, "tag"), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_position_profit(kHandle, kStk), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_position_avgpx(kHandle, kStk), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_detail_cost(kHandle, kStk, "tag"), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_detail_profit(kHandle, kStk, "tag", 0), 0.0);
	EXPECT_EQ(sel_get_detail_entertime(kHandle, kStk, "tag"), 0ULL);
	EXPECT_EQ(sel_get_first_entertime(kHandle, kStk), 0ULL);
	EXPECT_EQ(sel_get_last_entertime(kHandle, kStk), 0ULL);
	EXPECT_EQ(sel_get_last_exittime(kHandle, kStk), 0ULL);
	EXPECT_DOUBLE_EQ(sel_get_last_enterprice(kHandle, kStk), 0.0);
	EXPECT_STREQ(sel_get_last_entertag(kHandle, kStk), "");

	EXPECT_DOUBLE_EQ(sel_get_fund_data(kHandle, 0), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_fund_data(kHandle, 1), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_fund_data(kHandle, 2), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_fund_data(kHandle, 3), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_fund_data(kHandle, 99), 0.0);

	EXPECT_EQ(sel_get_tdate(), 0U);
	EXPECT_EQ(sel_get_date(), 0U);
	EXPECT_EQ(sel_get_time(), 0U);
	EXPECT_DOUBLE_EQ(sel_get_price(kStk), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_day_price(kStk, 1), 0.0);
}

/* WtBtPorter.cpp:1157-1266 + 1194-1255：SEL 的空切片、下单守卫、订阅与枚举。
 * sel_set_position 在 commodity 守卫处早退；空持仓表仍补终止标记。 */
TEST_F(R83BtPorterMockerBase, SelSlicesOrderAndEnum)
{
	CtxHandler h = init_sel_mocker("r83_sel_slice", 20240102, 93000, "d");
	ASSERT_GT(h, 0UL);

	reset_counters();
	EXPECT_EQ(sel_get_bars(kHandle, kStk, "d1", 5, bars_cb), 0U);
	EXPECT_EQ(sel_get_ticks(kHandle, kStk, 5, ticks_cb), 0U);
	EXPECT_EQ(g_bars_calls, 0U);
	EXPECT_EQ(g_ticks_calls, 0U);

	sel_set_position(kHandle, kStk, 100.0, "r83");
	sel_sub_ticks(kHandle, kStk);

	sel_get_all_position(kHandle, pos_cb);
	EXPECT_EQ(g_pos_calls, 1U);
	EXPECT_TRUE(g_pos_last);
}

/* WtBtPorter.cpp:1018-1074：SEL userdata 往返与日志四级 + default。 */
TEST_F(R83BtPorterMockerBase, SelUserDataAndLogLevels)
{
	CtxHandler h = init_sel_mocker("r83_sel_ud", 20240102, 93000, "d");
	ASSERT_GT(h, 0UL);

	sel_save_userdata(kHandle, "round", "trip");
	EXPECT_STREQ(sel_load_userdata(kHandle, "round", "fallback"), "trip");
	EXPECT_STREQ(sel_load_userdata(kHandle, "absent", "fallback"), "fallback");

	sel_log_text(kHandle, LOG_LEVEL_DEBUG, "r83 sel debug");
	sel_log_text(kHandle, LOG_LEVEL_INFO, "r83 sel info");
	sel_log_text(kHandle, LOG_LEVEL_WARN, "r83 sel warn");
	sel_log_text(kHandle, LOG_LEVEL_ERROR, "r83 sel error");
	sel_log_text(kHandle, 99, "r83 sel unknown level");
}

/* WtBtPorter.cpp:1470-1750 + WtBtRunner.cpp:285-297：在 fork 子进程里
 * 初始化 HFT mocker（hook=true），父进程单例保持零初始化。覆盖四个 getter、
 * 五类切片的 NULL 支、buy/sell/cancel/cancel_all 的 mocker 非空支。 */
TEST(R83BtPorterHft, HftGettersOrdersAndSlicesInChildProcess)
{
	int fds[2] = { -1, -1 };
	ASSERT_EQ(::pipe(fds), 0);

	pid_t pid = ::fork();
	ASSERT_GE(pid, 0);
	if (pid == 0)
	{
		::close(fds[0]);
		HftChildReport rep;
		memset(&rep, 0, sizeof(rep));

		rep.id1 = init_hft_mocker("r83_hft_order", true);
		const CtxHandler h1 = rep.id1;
		child_expect(rep, rep.id1 > 0U);

		child_expect(rep, hft_get_position(h1, kFut, true) == 0.0);
		child_expect(rep, hft_get_position(h1, kFut, false) == 0.0);
		child_expect(rep, hft_get_position_profit(h1, kFut) == 0.0);
		child_expect(rep, hft_get_position_avgpx(h1, kFut) == 0.0);
		child_expect(rep, hft_get_undone(h1, kFut) == 0.0);

		/* commodity 缺失时 stra_buy/stra_sell 返回空 OrderIDs，导出层仍走
		 * 拼串 + substr 的非空分支，结果是空串。 */
		rep.buy_len = (int)strlen(hft_buy(h1, kFut, 450.0, 1.0, "r83", 0));
		rep.sell_len = (int)strlen(hft_sell(h1, kFut, 450.0, 1.0, "r83", 0));
		rep.cancel_ok = hft_cancel(h1, 1) ? 1 : 0;
		rep.cancel_all_len = (int)strlen(hft_cancel_all(h1, kFut, true));
		child_expect(rep, rep.buy_len == 0);
		child_expect(rep, rep.sell_len == 0);
		child_expect(rep, rep.cancel_ok == 1);
		child_expect(rep, rep.cancel_all_len == 0);

		rep.bars = hft_get_bars(h1, kFut, "m1", 3, bars_cb);
		rep.ticks = hft_get_ticks(h1, kFut, 3, ticks_cb);
		/* ordque/orddtl 的失败兜底会误写 _trans_cache（BUG-R2 已登记），
		 * 因此三个切片各用一个独立合约，避免后一个复用前一个留下的空缓存。 */
		rep.ordque = hft_get_ordque(h1, "SHFE.rb2510", 3, ordque_cb);
		rep.orddtl = hft_get_orddtl(h1, "SHFE.au2406", 3, orddtl_cb);
		rep.trans = hft_get_trans(h1, "SHFE.ag2412", 3, trans_cb);
		child_expect(rep, rep.bars == 0U && rep.ticks == 0U && rep.ordque == 0U
			&& rep.orddtl == 0U && rep.trans == 0U);

		ssize_t nw = ::write(fds[1], &rep, sizeof(rep));
		::close(fds[1]);
		::exit(nw == (ssize_t)sizeof(rep) ? 0 : 3);
	}

	::close(fds[1]);
	HftChildReport rep;
	memset(&rep, 0, sizeof(rep));
	ssize_t nr = ::read(fds[0], &rep, sizeof(rep));
	::close(fds[0]);

	int status = 0;
	ASSERT_EQ(::waitpid(pid, &status, 0), pid);
	ASSERT_EQ(nr, (ssize_t)sizeof(rep)) << "HFT 子进程未回传结果";
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
	EXPECT_EQ(rep.fails, 0);
	EXPECT_GT(rep.id1, 0U);
	EXPECT_EQ(rep.buy_len, 0);
	EXPECT_EQ(rep.cancel_all_len, 0);

	/* 父进程从未初始化 HFT mocker：R9 的前置断言依赖这一点 */
	EXPECT_EQ(getRunner().hft_mocker(), nullptr);
}

/* WtBtPorter.cpp:1759-2001 + WtBtRunner.cpp:285-297：HFT 的订阅/日志/userdata/
 * hft_step，以及 init_hft_mocker 二次创建（delete 旧实例、id 递增）。
 * 仍在 fork 子进程内完成，父进程单例不受影响。 */
TEST(R83BtPorterHft, HftSubscriptionsUserDataAndMockerReplacementInChildProcess)
{
	int fds[2] = { -1, -1 };
	ASSERT_EQ(::pipe(fds), 0);

	pid_t pid = ::fork();
	ASSERT_GE(pid, 0);
	if (pid == 0)
	{
		::close(fds[0]);
		HftChildReport rep;
		memset(&rep, 0, sizeof(rep));

		rep.id1 = init_hft_mocker("r83_hft_sub_a", false);
		rep.id2 = init_hft_mocker("r83_hft_sub_b", true);
		const CtxHandler h2 = rep.id2;
		child_expect(rep, rep.id1 > 0U);
		child_expect(rep, rep.id2 == rep.id1 + 1U);

		hft_sub_ticks(h2, kFut);
		hft_sub_order_detail(h2, kFut);
		hft_sub_order_queue(h2, kFut);
		hft_sub_transaction(h2, kFut);

		hft_log_text(h2, LOG_LEVEL_DEBUG, "r83 hft debug");
		hft_log_text(h2, LOG_LEVEL_INFO, "r83 hft info");
		hft_log_text(h2, LOG_LEVEL_WARN, "r83 hft warn");
		hft_log_text(h2, LOG_LEVEL_ERROR, "r83 hft error");
		hft_log_text(h2, 99, "r83 hft unknown level");
		rep.logged = 1;

		hft_save_userdata(h2, "round", "trip");
		rep.userdata_ok = (strcmp(hft_load_userdata(h2, "round", "fallback"), "trip") == 0) ? 1 : 0;
		rep.default_ok = (strcmp(hft_load_userdata(h2, "absent", "fallback"), "fallback") == 0) ? 1 : 0;
		child_expect(rep, rep.userdata_ok == 1);
		child_expect(rep, rep.default_ok == 1);

		/* 非异步模式：hft_step 在 isAsync() 守卫处返回 */
		hft_step(h2);

		ssize_t nw = ::write(fds[1], &rep, sizeof(rep));
		::close(fds[1]);
		::exit(nw == (ssize_t)sizeof(rep) ? 0 : 3);
	}

	::close(fds[1]);
	HftChildReport rep;
	memset(&rep, 0, sizeof(rep));
	ssize_t nr = ::read(fds[0], &rep, sizeof(rep));
	::close(fds[0]);

	int status = 0;
	ASSERT_EQ(::waitpid(pid, &status, 0), pid);
	ASSERT_EQ(nr, (ssize_t)sizeof(rep)) << "HFT 子进程未回传结果";
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
	EXPECT_EQ(rep.fails, 0);
	EXPECT_EQ(rep.id2, rep.id1 + 1U);
	EXPECT_EQ(rep.userdata_ok, 1);
	EXPECT_EQ(rep.default_ok, 1);
	EXPECT_EQ(rep.logged, 1);

	EXPECT_EQ(getRunner().hft_mocker(), nullptr);
}

/* WtBtPorter.cpp:338-341 -> WtBtRunner.cpp:299-313 -> HisDataReplayer.cpp:423-426：
 * init_sel_mocker 把 trdtpl/session 原样转给 register_task，后者用 strcpy 写进
 * TaskInfo 的 16 字节定长数组，既不判 NULL 也不限长。NULL 入参必然段错误，
 * 超长入参则堆越界；见 BUG-R83-001。属崩溃类，保留为 DISABLED 占位。 */
TEST(R83BtPorterParamGuards, DISABLED_InitSelMockerNullTemplatesCrashes)
{
	init_sel_mocker("r83_sel_null", 20240102, 93000, "d", NULL, NULL, 0, false);
}
