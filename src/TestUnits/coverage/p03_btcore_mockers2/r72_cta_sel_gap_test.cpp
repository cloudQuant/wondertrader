/**
 * r72_cta_sel_gap_test.cpp  (lane R72)
 *
 * 靶子：
 *   CtaMocker
 *     - proc_tick 的非模拟 tick 条件单比较矩阵（CtaMocker.cpp L749-L778）
 *     - do_set_position 的比例滑点开/平仓（L1637-L1650、L1674-L1688）
 *     - stra_exit_long 的收盘总持仓分支 + 股票禁做空守卫（L1411-L1418、L1462-L1466）
 *   SelMocker
 *     - handle_schedule 的跨日进位（SelMocker.cpp L285-L291）
 *     - stra_get_bars 的分钟分支与价格表播种（L815-L859）
 *     - do_set_position 的比例滑点开/平仓（L678-L692、L717-L731）
 *
 * 条件单 alg 0~4 只能由 load_incremental_data 的 JSON 注入（公开 API），
 * 非模拟标记由 tick-enabled 的 run_by_bars 产生。
 */
#include <gtest/gtest.h>

#include <WtBtCore/CtaMocker.h>
#include <WtBtCore/SelMocker.h>

#include "r72_common.h"

namespace
{
	const char* const kCode = "SSE.STK.000001";
	const uint32_t kDates[5] = { 20200102u, 20200103u, 20200106u, 20200107u, 20200108u };

	const char* const kEq = "SSE.STK.000002";
	const char* const kLarger = "SSE.STK.000003";
	const char* const kSmaller = "SSE.STK.000004";
	const char* const kGe = "SSE.STK.000005";
	const char* const kLe = "SSE.STK.000006";

	std::vector<WTSTickStruct> day_ticks(uint32_t date, double base)
	{
		std::vector<WTSTickStruct> ret;
		ret.emplace_back(r72::make_tick_struct(kCode, date, 93000000u, base, base - 0.01, base + 0.01));
		ret.emplace_back(r72::make_tick_struct(kCode, date, 93100000u, base + 0.5, base + 0.49, base + 0.51));
		// 15:00 这笔让 replayHftDatas 由 etime 正常 break（返回 true）。
		// 再放一笔 15:01：getNextTickTime 的游标差一（p07 BUG-P07-001/005）会把
		// 最后一笔当「没有下一笔」，这样 15:00 仍然能被看到。
		ret.emplace_back(r72::make_tick_struct(kCode, date, 150000000u, base + 1.0, base + 0.99, base + 1.01));
		ret.emplace_back(r72::make_tick_struct(kCode, date, 150100000u, base + 1.5, base + 1.49, base + 1.51));
		return ret;
	}

	void feed(CtaMocker& m, const char* code, double px)
	{
		WTSTickData* tick = r72::make_tick(code, 20200108u, 100000000u, px, px - 0.01, px + 0.01);
		m.handle_tick(code, tick, 1);
		tick->release();
	}

	std::string conditions_json()
	{
		std::stringstream ss;
		ss << "{\"conditions\":{\"settime\":202001020930,\"items\":{";
		ss << "\"" << kEq << "\":[{\"usertag\":\"eq\",\"field\":0,\"alg\":0,\"target\":10.0,\"qty\":2.0,\"action\":0}],";
		ss << "\"" << kLarger << "\":[{\"usertag\":\"gt\",\"field\":0,\"alg\":1,\"target\":10.0,\"qty\":1.0,\"action\":0}],";
		ss << "\"" << kSmaller << "\":[{\"usertag\":\"lt\",\"field\":0,\"alg\":2,\"target\":10.0,\"qty\":1.0,\"action\":0}],";
		ss << "\"" << kGe << "\":[{\"usertag\":\"ge\",\"field\":0,\"alg\":3,\"target\":10.0,\"qty\":1.0,\"action\":4}],";
		ss << "\"" << kLe << "\":[{\"usertag\":\"le\",\"field\":0,\"alg\":4,\"target\":10.0,\"qty\":2.0,\"action\":0}]";
		ss << "}}}";
		return ss.str();
	}
}

class R72CtaSelGap : public r72::Fixture
{
};

/*
 * 条件单五算法在「非模拟 tick」下的比较分支（CtaMocker.cpp L749-L778）：
 * 先用 loader 的 bar+tick 跑一次 run_by_bars，把 _tick_simulated 置 false，
 * 再通过 load_incremental_data 注入 alg=0..4 的条件单并逐笔触发。
 * 写死 20200108 的日期，确保与回放结束后的时钟无关。
 */
TEST_F(R72CtaSelGap, CtaNonSimulatedConditionMatrix)
{
	r72::Loader loader;
	std::vector<WTSBarStruct> bars;
	for (uint32_t i = 0; i < 5; i++)
	{
		bars.emplace_back(r72::make_bar(kDates[i], 1500u, 10.5 + i));
		loader.ticks[std::string(kCode) + "#" + std::to_string(kDates[i])] = day_ticks(kDates[i], 10.5 + i);
	}
	loader.finalBars[kCode] = bars;

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001081500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	CtaMocker mocker(&rp, "r72_cta_cond", 0, false);
	rp.register_sink(&mocker, "r72_cta_cond");
	ASSERT_TRUE(rp.prepare());

	WTSKlineSlice* main = mocker.stra_get_bars(kCode, "d1", 5, true);
	ASSERT_NE(nullptr, main);
	main->release();

	rp.run(false);
	ASSERT_FALSE(rp.is_tick_simulated());

	ASSERT_TRUE(r72::write_text(_out / "r72_cta_cond" / "r72_cta_cond.json", conditions_json()));
	mocker.load_incremental_data("r72_cta_cond");

	// WCT_Equal：当前价等于目标价
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kEq));
	feed(mocker, kEq, 10.0);
	EXPECT_DOUBLE_EQ(2.0, mocker.stra_get_position(kEq));

	// WCT_Larger：未越过不触发，越过触发
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kLarger));
	feed(mocker, kLarger, 9.9);
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kLarger));
	feed(mocker, kLarger, 10.1);
	EXPECT_DOUBLE_EQ(1.0, mocker.stra_get_position(kLarger));

	// WCT_Smaller：反向验证
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kSmaller));
	feed(mocker, kSmaller, 10.1);
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kSmaller));
	feed(mocker, kSmaller, 9.9);
	EXPECT_DOUBLE_EQ(1.0, mocker.stra_get_position(kSmaller));

	// WCT_LargerOrEqual：等于也触发；action=SP 直接设目标仓位
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kGe));
	feed(mocker, kGe, 9.9);
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kGe));
	feed(mocker, kGe, 10.0);
	EXPECT_DOUBLE_EQ(1.0, mocker.stra_get_position(kGe));

	// WCT_SmallerOrEqual：等于也触发
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kLe));
	feed(mocker, kLe, 10.1);
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kLe));
	feed(mocker, kLe, 10.0);
	EXPECT_DOUBLE_EQ(2.0, mocker.stra_get_position(kLe));
}

/*
 * CtaMocker 比例滑点：slippage=2 表示万分之 2，rb 的 pricetick=1。
 * 买：2*3800/10000=0.76 -> 修约到 1 跳 -> 3801；卖：3820-1=3819。
 * 平仓盈亏 (3819-3801)*2*10=360（CtaMocker.cpp L1637-L1650、L1674-L1688）。
 */
TEST_F(R72CtaSelGap, CtaRatioSlippageOpenAndClose)
{
	const char* const code = "SHFE.rb.2405";

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, nullptr));
	cfg->release();

	CtaMocker mocker(&rp, "r72_cta_ratio", 2, false, nullptr, true);
	mocker.stra_enter_long(code, 2, "ratio-open");
	feed(mocker, code, 3800.0);
	EXPECT_DOUBLE_EQ(2.0, mocker.stra_get_position(code));
	EXPECT_DOUBLE_EQ(3801.0, mocker.stra_get_position_avgpx(code));

	mocker.stra_exit_long(code, 2, "ratio-close");
	feed(mocker, code, 3820.0);
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(code));
	EXPECT_DOUBLE_EQ(360.0, mocker.stra_get_fund_data(1));
}

/*
 * 收盘时刻平仓：SD0930 的 15:00 让 stra_exit_long 的 isLastBarOfDay=true，
 * 用总持仓而不是可用持仓判断（T+1 当日买入全部冻结）。
 * 同时验证股票禁做空守卫（CtaMocker.cpp L1411-L1418、L1462-L1466、L1530-L1534）。
 */
TEST_F(R72CtaSelGap, CtaExitAtCloseUsesTotalPosition)
{
	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001021500ull, 202001081500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, nullptr));
	cfg->release();
	r72::Sink sink;
	rp.register_sink(&sink, "r72_cta_close");
	ASSERT_TRUE(rp.prepare());
	EXPECT_EQ(1500u, rp.get_min_time());

	WTSCommodityInfo* comm = rp.get_commodity_info(kCode);
	ASSERT_NE(nullptr, comm);
	comm->setTradingMode(TM_LongT1);

	CtaMocker mocker(&rp, "r72_cta_close", 0, false);
	mocker.stra_enter_long(kCode, 100, "t1-open");
	feed(mocker, kCode, 10.0);
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position(kCode, false));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kCode, true));

	// 收盘时可以用冻结持仓平仓；目标 60
	mocker.stra_exit_long(kCode, 40, "t1-exit");
	feed(mocker, kCode, 10.5);
	EXPECT_DOUBLE_EQ(60.0, mocker.stra_get_position(kCode, false));
	EXPECT_DOUBLE_EQ(20.0, mocker.stra_get_fund_data(1));

	// 股票禁做空：目标仓位与平空都不得改变持仓
	mocker.stra_set_position(kCode, -10, "short");
	EXPECT_DOUBLE_EQ(60.0, mocker.stra_get_position(kCode, false));
	mocker.stra_exit_short(kCode, 10, "cover");
	EXPECT_DOUBLE_EQ(60.0, mocker.stra_get_position(kCode, false));
}

/*
 * SelMocker 分钟线分支 + 价格表播种 + 调度跨日进位（SelMocker.cpp L285-L291、
 * L815-L859）。不调用 handle_replay_done（零 emit 的除零缺陷见 BUG-R43-001）。
 */
TEST_F(R72CtaSelGap, SelMinuteBarsSeedPriceAndScheduleRollover)
{
	std::vector<WTSBarStruct> m1;
	for (uint32_t i = 0; i < 5; i++)
		m1.emplace_back(r72::make_bar(20200102u, 930u + i, 10.0 + i * 0.5));
	ASSERT_TRUE(r72::write_bars_csv(_base, std::string(kCode) + "_m1", m1, true));

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, nullptr));
	cfg->release();

	SelMocker sel(&rp, "r72_sel_seed", 0, false);
	rp.register_sink(&sel, "r72_sel_seed");
	ASSERT_TRUE(rp.prepare());

	// 23:59 的下一分钟进位到次日（无K线标签时只推进调度计数，不 emit）
	sel.handle_schedule(20200102u, 2359u);

	WTSKlineSlice* bars = sel.stra_get_bars(kCode, "m1", 5);
	ASSERT_NE(nullptr, bars);
	ASSERT_GE(bars->size(), 1u);
	EXPECT_DOUBLE_EQ(10.0, bars->at(-1)->close);
	bars->release();

	sel.stra_set_position(kCode, 100, "seed");
	EXPECT_DOUBLE_EQ(100.0, sel.stra_get_position(kCode, false));

	WTSTickData* tick = r72::make_tick(kCode, 20200102u, 93100000u, 10.2, 10.19, 10.21);
	sel.handle_tick(kCode, tick, 1);
	tick->release();
	EXPECT_DOUBLE_EQ(100.0, sel.stra_get_position(kCode, false));
	EXPECT_DOUBLE_EQ(10.2, sel.stra_get_position_avgpx(kCode));
}

/*
 * SelMocker 比例滑点：slippage=100（1%），股票 pricetick=0.01。
 * 10.0 买 -> 10.1；10.0 卖 -> 9.9；100 股平仓盈亏 -20
 * （SelMocker.cpp L678-L692、L717-L731）。
 */
TEST_F(R72CtaSelGap, SelRatioSlippageOpenAndClose)
{
	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, nullptr));
	cfg->release();

	SelMocker sel(&rp, "r72_sel_ratio", 100, true);
	sel.stra_set_position(kCode, 100, "ratio-buy");
	WTSTickData* t1 = r72::make_tick(kCode, 20200102u, 93000000u, 10.0, 9.99, 10.01);
	sel.handle_tick(kCode, t1, 1);
	t1->release();
	EXPECT_DOUBLE_EQ(100.0, sel.stra_get_position(kCode, false));
	EXPECT_DOUBLE_EQ(10.1, sel.stra_get_position_avgpx(kCode));

	sel.stra_set_position(kCode, 0, "ratio-flat");
	WTSTickData* t2 = r72::make_tick(kCode, 20200102u, 93100000u, 10.0, 9.99, 10.01);
	sel.handle_tick(kCode, t2, 1);
	t2->release();
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_position(kCode, false));
	EXPECT_NEAR(-20.0, sel.stra_get_fund_data(1), 1e-9);
}
