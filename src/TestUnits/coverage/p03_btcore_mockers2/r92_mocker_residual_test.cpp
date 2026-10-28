/**
 * r92_mocker_residual_test.cpp  (lane R92)
 *
 * 靶子：WtBtCore 五个 Mocker 在 R43/R62/R72 之后残余的**公开面**：
 *   CtaMocker.cpp
 *     - 条件单动作在反手仓上的分支（L858 OL 空头、L872 OS 多头）
 *     - 同算法 Smaller/SmallerOrEqual 的择优分支（L831-838）
 *     - 各种 stop 条件单的构造分支（L1390-1394、L1441-1445、L1492-1496、L1574-1578）
 *     - 简单开平仓的符号分支（L1319、L1370）、平仓守卫（L1418、L1470）
 *     - 非交易时段的调度（L1202-1205）、零仓位会话汇总（L1282）
 *     - 零价成交回退（L1608）、空明细查询（L1958/1971/1992-1997/2052/2090/2109/2140）
 *     - 明细查询 switch 全分支（L2118-2141）、空回放器价格查询（L1509/1517）
 *   SelMocker.cpp
 *     - 持仓递增的 open 分支 + T+1 冻结 + 绝对滑点（L667-711、L729-730）
 *     - 空头平仓盈亏符号（L755）、单字符周期（L825-826）
 *     - stra_get_* 查询矩阵（L899-901、L995-1005、L1021-1026、L1036、L1047-1052、
 *       L1072、L1085、L1103-1104、L1113-1174）
 *     - 未订阅 tick 的价格缓存回退（L332-333）
 *     - 信号枚举/落盘（L127-139、L541-545）、bar 回调（L387-399）、on_tick 空体（L453-456）
 *   UftMocker.cpp
 *     - match_this_tick=true 的撮合顺序（L250-263）、procOrder 成交（L953-970）
 *     - CM_CoverToday 的平今/平昨分支（L505-522、L568-585、L697-712、L758-773）
 *     - 撤单释放今仓（L428-430）、随机错单（L902-905）、askprice=0（L927-930）、
 *       卖限价不越过（L943-948）、无行情（L920）、无品种成交回写（L1172）
 *     - 平仓后 0 量明细残留导致的 continue（L1218）、空头平仓符号（L1291）
 *   HftMocker.cpp
 *     - match_this_tick=true 的撮合顺序（L359-389）
 *     - 非 match 分支的部分成交/清空（L393-408、L765-786）
 *     - 随机错单（L713-717）、askqty=0（L732）、askprice=0（L739-742）、
 *       卖限价不越过（L755-759）
 *     - 空仓/空明细查询（L916、L926）、零价成交（L1097）、0 量成交早退（L1103）、
 *       T+1 同向加仓（L1121-1125）、会话末持仓日志（L521-523）
 *   ExecMocker.cpp
 *     - 模块可装载但缺 createExecFact 的失败分支（L64-69）
 *     - handle_schedule 的 15:00 早退（L164-166）
 *
 * 现有 R43/R62/R72 已覆盖的路径不再重复；只走公开 API：
 *   - 真实 HisDataReplayer（csv 模式，dist/common 只读）+ r72_common 的纯内存 Loader
 *   - WTSTickData::create/release、WTSVariant、WtHelper::setOutputDir
 *   - 各 Mocker 的 IDataSink / I*StraCtx 公共方法
 *
 * 不使用 #define private public、不镜像类布局、不改编译宏。
 * 落盘只写系统临时目录，由 r72::Fixture 的 SetUp/TearDown 清理。
 */
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <WtBtCore/CtaMocker.h>
#include <WtBtCore/ExecMocker.h>
#include <WtBtCore/HftMocker.h>
#include <WtBtCore/SelMocker.h>
#include <WtBtCore/UftMocker.h>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSVariant.hpp>

#include "r72_common.h"

namespace
{
	const char* const kStk = "SSE.STK.000001";
	const char* const kRb = "SHFE.rb.2405";
	const char* const kCu = "SHFE.cu.2406";
	const char* const kAg = "SHFE.ag.2406";
	const char* const kRu = "SHFE.ru.2406";
	const char* const kHc = "SHFE.hc.2405";

	// 能被 dlopen、但不导出 create*StrategyFact / createExecFact 的系统库。
	// 找不到时退回第一个候选（load 失败同样让 init 返回 false，只是少覆盖 creator==NULL 一支）。
	std::string pick_library()
	{
		const char* candidates[] = {
			"/opt/homebrew/lib/libboost_system.dylib",
			"/usr/local/lib/libboost_system.dylib",
			"/opt/homebrew/lib/libboost_filesystem.dylib",
			"/usr/local/lib/libboost_filesystem.dylib",
		};
		for (const char* path : candidates)
		{
			if (std::filesystem::exists(path))
				return path;
		}
		return candidates[0];
	}

	void feed(CtaMocker& mocker, const char* code, double px, uint32_t pxType = 0)
	{
		WTSTickData* tick = r72::make_tick(code, 20200108u, 100000000u, px, px - 0.01, px + 0.01);
		mocker.handle_tick(code, tick, pxType);
		tick->release();
	}

	void feed(SelMocker& mocker, const char* code, double px, uint32_t pxType = 0)
	{
		WTSTickData* tick = r72::make_tick(code, 20200108u, 100000000u, px, px - 0.01, px + 0.01);
		mocker.handle_tick(code, tick, pxType);
		tick->release();
	}

	WTSVariant* factory_cfg(const std::string& module)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("module", module.c_str());
		return cfg;
	}

	WTSVariant* uft_cfg(const std::string& module, uint32_t errorRate, bool matchThisTick)
	{
		WTSVariant* cfg = factory_cfg(module);
		cfg->append("error_rate", errorRate);
		cfg->append("match_this_tick", matchThisTick);
		return cfg;
	}

	WTSVariant* hft_cfg(const std::string& module, uint32_t errorRate, bool matchThisTick)
	{
		WTSVariant* cfg = factory_cfg(module);
		cfg->append("error_rate", errorRate);
		cfg->append("match_this_tick", matchThisTick);
		return cfg;
	}
}

class R92MockerResidual : public r72::Fixture
{
};

// 五个 Mocker 的工厂装载：模块能 dlopen、但缺 creator 符号 -> false，且不得留下半初始化状态。
// Cta L426-431、Uft L147-152、Sel L245-250、Hft L232-237、Exec L64-69。
TEST_F(R92MockerResidual, FactoriesRejectLibraryWithoutCreator)
{
	const std::string module = pick_library();
	HisDataReplayer rp;

	{
		WTSVariant* cfg = factory_cfg(module);
		CtaMocker mocker(&rp, "r92_fact_cta");
		EXPECT_FALSE(mocker.init_cta_factory(cfg));
		cfg->release();
	}
	{
		WTSVariant* cfg = factory_cfg(module);
		UftMocker mocker(&rp, "r92_fact_uft");
		EXPECT_FALSE(mocker.init_uft_factory(cfg));
		cfg->release();
	}
	{
		WTSVariant* cfg = factory_cfg(module);
		SelMocker mocker(&rp, "r92_fact_sel");
		EXPECT_FALSE(mocker.init_sel_factory(cfg));
		cfg->release();
	}
	{
		WTSVariant* cfg = factory_cfg(module);
		HftMocker mocker(&rp, "r92_fact_hft");
		EXPECT_FALSE(mocker.init_hft_factory(cfg));
		cfg->release();
	}
	{
		WTSVariant* cfg = factory_cfg(module);
		cfg->append("code", kStk);
		cfg->append("period", "m1");
		cfg->append("volunit", 1.0);
		cfg->append("volmode", static_cast<int32_t>(0));
		ExecMocker mocker(&rp);
		EXPECT_FALSE(mocker.init(cfg));
		cfg->release();
	}
}

// 条件单动作在反向持仓上的绝对值语义 + 同算法 SmallerOrEqual 取更紧的目标价。
// Cta L858、L872、L831-838、L1319、L1370。
TEST_F(R92MockerResidual, CtaConditionActionsAndSmallerTie)
{
	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, nullptr));
	cfg->release();

	CtaMocker mocker(&rp, "r92_cta_cond", 0, false);

	// OL：已有 2 手空仓时，条件开多 1 手应直接给目标 +1（不是 curQty+qty=-1）
	mocker.stra_enter_short(kRb, 2, "short");
	feed(mocker, kRb, 3800.0);
	ASSERT_DOUBLE_EQ(-2.0, mocker.stra_get_position(kRb));
	mocker.stra_enter_long(kRb, 1, "ol-cover", 0.0, 3810.0);
	feed(mocker, kRb, 3811.0);
	EXPECT_DOUBLE_EQ(1.0, mocker.stra_get_position(kRb));
	EXPECT_STREQ("ol-cover", mocker.stra_get_last_entertag(kRb));

	// OS：已有 2 手多仓时，条件开空 1 手应直接给目标 -1
	mocker.stra_enter_long(kCu, 2, "long");
	feed(mocker, kCu, 70000.0);
	ASSERT_DOUBLE_EQ(2.0, mocker.stra_get_position(kCu));
	mocker.stra_enter_short(kCu, 1, "os-cover", 69900.0, 0.0);
	feed(mocker, kCu, 70000.0);
	EXPECT_DOUBLE_EQ(-1.0, mocker.stra_get_position(kCu));
	EXPECT_STREQ("os-cover", mocker.stra_get_last_entertag(kCu));

	// 同算法 SmallerOrEqual：后登记的更高止损价更紧，应替换先登记的宽价
	mocker.stra_enter_long(kAg, 1, "loose", 0.0, 6000.0);
	mocker.stra_enter_long(kAg, 1, "tight", 0.0, 6010.0);
	feed(mocker, kAg, 5999.0);
	EXPECT_DOUBLE_EQ(1.0, mocker.stra_get_position(kAg));
	EXPECT_STREQ("tight", mocker.stra_get_last_entertag(kAg));

	// 简单开多：当前为空仓时给 curQty+qty，当前为净空时给 qty 的绝对值
	mocker.stra_enter_short(kRu, 1, "ru-short");
	feed(mocker, kRu, 10000.0);
	ASSERT_DOUBLE_EQ(-1.0, mocker.stra_get_position(kRu));
	mocker.stra_enter_long(kRu, 1, "ru-flip");
	feed(mocker, kRu, 10010.0);
	EXPECT_DOUBLE_EQ(1.0, mocker.stra_get_position(kRu));

	// 简单开空：当前为净多时给 -qty 的绝对值
	mocker.stra_enter_long(kHc, 2, "hc-long");
	feed(mocker, kHc, 3500.0);
	ASSERT_DOUBLE_EQ(2.0, mocker.stra_get_position(kHc));
	mocker.stra_enter_short(kHc, 1, "hc-flip");
	feed(mocker, kHc, 3510.0);
	EXPECT_DOUBLE_EQ(-1.0, mocker.stra_get_position(kHc));
}

// stop 条件单的四个构造分支、平仓守卫、零价成交以及持仓明细查询族。
// Cta L1390-1394、L1418、L1441-1445、L1470、L1473-1476、L1492-1496、L1574-1578、
// L1608、L1937-2141。
TEST_F(R92MockerResidual, CtaStopConditionsAndPositionQueries)
{
	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, nullptr));
	cfg->release();

	CtaMocker mocker(&rp, "r92_cta_stop", 0, false);

	// 多头 -> 止损平多（WCT_SmallerOrEqual）
	mocker.stra_enter_long(kRb, 2, "mkt");
	feed(mocker, kRb, 3800.0);
	ASSERT_DOUBLE_EQ(2.0, mocker.stra_get_position(kRb));
	mocker.stra_exit_long(kRb, 1, "cl-stop", 0.0, 3790.0);
	feed(mocker, kRb, 3789.0);
	ASSERT_DOUBLE_EQ(1.0, mocker.stra_get_position(kRb));

	// 明细查询：tag 命中/未命中、首末时间/价格、平仓时间、明细盈亏五个 flag
	EXPECT_DOUBLE_EQ(1.0, mocker.stra_get_position(kRb, false, "mkt"));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kRb, false, "no-such-tag"));
	EXPECT_DOUBLE_EQ(3800.0, mocker.stra_get_position_avgpx(kRb));
	EXPECT_EQ(0ull, mocker.stra_get_first_entertime(kRb));
	EXPECT_EQ(0ull, mocker.stra_get_last_entertime(kRb));
	EXPECT_STREQ("mkt", mocker.stra_get_last_entertag(kRb));
	EXPECT_DOUBLE_EQ(3800.0, mocker.stra_get_last_enterprice(kRb));
	EXPECT_EQ(0ull, mocker.stra_get_last_exittime(kRb));
	EXPECT_EQ(0ull, mocker.stra_get_detail_entertime(kRb, "mkt"));
	EXPECT_DOUBLE_EQ(3800.0, mocker.stra_get_detail_cost(kRb, "mkt"));
	EXPECT_DOUBLE_EQ(-110.0, mocker.stra_get_detail_profit(kRb, "mkt", 0));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_detail_profit(kRb, "mkt", 1));
	EXPECT_DOUBLE_EQ(-220.0, mocker.stra_get_detail_profit(kRb, "mkt", -1));
	EXPECT_DOUBLE_EQ(3800.0, mocker.stra_get_detail_profit(kRb, "mkt", 2));
	EXPECT_DOUBLE_EQ(3789.0, mocker.stra_get_detail_profit(kRb, "mkt", -2));
	EXPECT_DOUBLE_EQ(-110.0, mocker.stra_get_position_profit(kRb));

	// 空头 -> 止损平空（WCT_LargerOrEqual）
	mocker.stra_enter_short(kCu, 2, "sm");
	feed(mocker, kCu, 70000.0);
	ASSERT_DOUBLE_EQ(-2.0, mocker.stra_get_position(kCu));
	mocker.stra_exit_short(kCu, 1, "cs-stop", 0.0, 70100.0);
	feed(mocker, kCu, 70101.0);
	EXPECT_DOUBLE_EQ(-1.0, mocker.stra_get_position(kCu));

	// 空仓时条件开空（L1390-1394）；止损价 6000，价格跌到 5999 触发
	mocker.stra_enter_short(kAg, 1, "os-stop", 0.0, 6000.0);
	feed(mocker, kAg, 5999.0);
	EXPECT_DOUBLE_EQ(-1.0, mocker.stra_get_position(kAg));

	// stra_set_position 的 stop 分支：目标上调买、目标下调卖（L1574-1578 两个方向）
	mocker.stra_set_position(kRu, 2, "sp-up", 0.0, 10100.0);
	feed(mocker, kRu, 10101.0);
	ASSERT_DOUBLE_EQ(2.0, mocker.stra_get_position(kRu));
	mocker.stra_set_position(kRu, 1, "sp-down", 0.0, 9990.0);
	feed(mocker, kRu, 9989.0);
	EXPECT_DOUBLE_EQ(1.0, mocker.stra_get_position(kRu));

	// 平仓守卫：无多仓平多、无空仓平空直接返回
	mocker.stra_exit_long(kHc, 1, "no-long");
	mocker.stra_exit_short(kHc, 1, "no-short");
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kHc));

	// 简单平空（L1473-1476）
	mocker.stra_enter_short(kHc, 2, "hc-short");
	feed(mocker, kHc, 3500.0);
	ASSERT_DOUBLE_EQ(-2.0, mocker.stra_get_position(kHc));
	mocker.stra_exit_short(kHc, 1, "hc-cover");
	feed(mocker, kHc, 3490.0);
	EXPECT_DOUBLE_EQ(-1.0, mocker.stra_get_position(kHc));

	// 零价成交：signal 的 desprice=0，tick.price()=0 -> do_set_position 回退到 _price_map
	mocker.stra_enter_long(kRb, 1, "zero-px");
	feed(mocker, kRb, 0.0);
	EXPECT_DOUBLE_EQ(2.0, mocker.stra_get_position(kRb));

	// 平掉 kAg 的全部空仓，留下 volume=0/明细为空的持仓条目，覆盖空明细查询
	mocker.stra_set_position(kAg, 0, "ag-flat");
	feed(mocker, kAg, 6005.0);
	ASSERT_DOUBLE_EQ(0.0, mocker.stra_get_position(kAg));
	EXPECT_EQ(0ull, mocker.stra_get_last_entertime(kAg));
	EXPECT_STREQ("", mocker.stra_get_last_entertag(kAg));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_last_enterprice(kAg));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_avgpx(kAg));
	EXPECT_EQ(0ull, mocker.stra_get_detail_entertime(kAg, "gone"));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_detail_cost(kAg, "gone"));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_detail_profit(kAg, "gone", 0));

	// 空回放器：stra_get_price / stra_get_day_price 的守卫分支
	CtaMocker nullMocker(nullptr, "r92_cta_null");
	EXPECT_DOUBLE_EQ(0.0, nullMocker.stra_get_price(kRb));
	EXPECT_DOUBLE_EQ(0.0, nullMocker.stra_get_day_price(kRb, 0));
}

// 非交易时段的 on_schedule 返回 false；零仓位条目在会话结束时被跳过。
// Cta L1096-1212（L1202-1205）、L1264-1295（L1282）。
TEST_F(R92MockerResidual, CtaNonTradingScheduleAndZeroVolumeSessionEnd)
{
	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, nullptr));
	cfg->release();

	CtaMocker mocker(&rp, "r92_cta_sched", 0, false);

	// 主K线登记 + 闭合，16:00 已超过股票 15:00 收盘 -> 非交易时段分支
	EXPECT_EQ(nullptr, mocker.stra_get_bars(kStk, "d1", 5, true));
	mocker.stra_sub_bar_events(kStk, "d1");
	WTSBarStruct bar = r72::make_bar(20200108u, 1500u, 10.0);
	mocker.handle_bar_close(kStk, "d", 1, &bar);
	EXPECT_FALSE(mocker.on_schedule(20200108u, 1600u));

	// 先开后平，留下 volume=0 的持仓条目；session_end 必须跳过它
	mocker.stra_enter_long(kCu, 1, "flat-open");
	feed(mocker, kCu, 70000.0);
	ASSERT_DOUBLE_EQ(1.0, mocker.stra_get_position(kCu));
	mocker.stra_exit_long(kCu, 1, "flat-close");
	feed(mocker, kCu, 70100.0);
	ASSERT_DOUBLE_EQ(0.0, mocker.stra_get_position(kCu));

	mocker.handle_session_end(20200108u);
	mocker.handle_replay_done();
}

// SelMocker：持仓从 100 增到 150 的 open 分支、T+1 冻结、绝对滑点与查询矩阵。
// Sel L646-711（L651、L667-711）、L717-730、L755、L899-901、L995-1005、
// L1021-1026、L1036、L1047-1052、L1072、L1085、L1103-1104、L1113-1174。
TEST_F(R92MockerResidual, SelOpenIncreaseSlippageAndQueryMatrix)
{
	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, nullptr));
	cfg->release();

	WTSCommodityInfo* stkComm = rp.get_commodity_info(kStk);
	ASSERT_NE(nullptr, stkComm);
	stkComm->setTradingMode(TM_LongT1);

	SelMocker sel(&rp, "r92_sel", 1, false);

	// 0 -> 100：走平仓分支的反手；1 跳绝对滑点，买价 10.0 -> 10.01
	sel.stra_set_position(kStk, 100.0, "tag1");
	feed(sel, kStk, 10.0);
	ASSERT_DOUBLE_EQ(100.0, sel.stra_get_position(kStk, false));

	// 100 -> 150：走 open 分支，T+1 冻结 +50，买价 10.2 -> 10.21
	sel.stra_set_position(kStk, 150.0, "tag2");
	feed(sel, kStk, 10.2);
	ASSERT_DOUBLE_EQ(150.0, sel.stra_get_position(kStk, false));

	EXPECT_DOUBLE_EQ(50.0, sel.stra_get_position(kStk, false, "tag2"));
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_position(kStk, false, "no-such-tag"));
	EXPECT_NEAR(10.0766667, sel.stra_get_position_avgpx(kStk), 1e-6);
	EXPECT_NEAR(18.5, sel.stra_get_position_profit(kStk), 1e-9);
	EXPECT_EQ(0ull, sel.stra_get_first_entertime(kStk));
	EXPECT_EQ(0ull, sel.stra_get_last_entertime(kStk));
	EXPECT_STREQ("tag2", sel.stra_get_last_entertag(kStk));
	EXPECT_DOUBLE_EQ(10.21, sel.stra_get_last_enterprice(kStk));
	EXPECT_EQ(0ull, sel.stra_get_detail_entertime(kStk, "tag2"));
	EXPECT_DOUBLE_EQ(10.21, sel.stra_get_detail_cost(kStk, "tag2"));
	EXPECT_NEAR(-0.5, sel.stra_get_detail_profit(kStk, "tag2", 0), 1e-9);
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_detail_profit(kStk, "tag2", 1));
	EXPECT_NEAR(-0.5, sel.stra_get_detail_profit(kStk, "tag2", -1), 1e-9);
	EXPECT_DOUBLE_EQ(10.21, sel.stra_get_detail_profit(kStk, "tag2", 2));
	EXPECT_DOUBLE_EQ(10.2, sel.stra_get_detail_profit(kStk, "tag2", -2));
	EXPECT_EQ(0u, sel.stra_get_tdate());

	// 跨日解冻后平掉全部多头
	sel.handle_session_begin(20200103u);
	sel.stra_set_position(kStk, 0.0, "flat");
	feed(sel, kStk, 10.5);
	ASSERT_DOUBLE_EQ(0.0, sel.stra_get_position(kStk, false));
	EXPECT_GT(sel.stra_get_fund_data(1), 0.0);

	// 明细已清空：最后一笔开仓时间/价格、均价都回 0
	EXPECT_EQ(0ull, sel.stra_get_last_entertime(kStk));
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_last_enterprice(kStk));
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_position_avgpx(kStk));
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_detail_profit(kStk, "tag2", 0));

	// 期货空头：0 -> -100 的反手建仓（卖价 3799）与 -100 -> 0 的平仓（买价 3791）
	sel.stra_set_position(kRb, -100.0, "short");
	feed(sel, kRb, 3800.0);
	ASSERT_DOUBLE_EQ(-100.0, sel.stra_get_position(kRb, false));
	EXPECT_STREQ("short", sel.stra_get_last_entertag(kRb));
	EXPECT_DOUBLE_EQ(3799.0, sel.stra_get_last_enterprice(kRb));
	EXPECT_EQ(0ull, sel.stra_get_first_entertime(kRb));
	EXPECT_NEAR(-1000.0, sel.stra_get_position_profit(kRb), 1e-6);
	sel.stra_set_position(kRb, 0.0, "flat-short");
	feed(sel, kRb, 3790.0);
	ASSERT_DOUBLE_EQ(0.0, sel.stra_get_position(kRb, false));
	EXPECT_GT(sel.stra_get_fund_data(1), 7000.0);

	// 零价成交：desprice=0 且 tick.price()=0 -> do_set_position 回退到 _price_map
	sel.stra_set_position(kAg, 100.0, "zero-px");
	feed(sel, kAg, 0.0);
	EXPECT_DOUBLE_EQ(100.0, sel.stra_get_position(kAg, false));

	// pxType!=0 且本地价格表没有该合约：last_px 回退到当前价（L332-333）
	WTSTickData* fresh = r72::make_tick("SSE.STK.000002", 20200108u, 93100000u, 20.0, 19.99, 20.01);
	sel.handle_tick("SSE.STK.000002", fresh, 1);
	fresh->release();
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_position("SSE.STK.000002"));

	// 空回放器的两个价格查询守卫
	SelMocker nullSel(nullptr, "r92_sel_null");
	EXPECT_DOUBLE_EQ(0.0, nullSel.stra_get_price(kStk));
	EXPECT_DOUBLE_EQ(0.0, nullSel.stra_get_day_price(kStk, 0));
}

// SelMocker：未成交信号参与 enum_position，并随 signals.csv / JSON 落盘；
// bar 回调、on_tick 空体、单字符周期也在这里走一遍。
// Sel L127-139、L387-399、L453-456、L541-545、L825-826。
TEST_F(R92MockerResidual, SelSignalEnumerationBarCallbacksAndDump)
{
	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, nullptr));
	cfg->release();

	SelMocker sel(&rp, "r92_sel_dump");

	// 先调度一次，规避零 emit 的平均耗时除法（BUG-R43-001）
	sel.handle_schedule(20200102u, 930u);

	// 只挂信号、不喂 tick：enum_position 走信号合并分支
	sel.stra_set_position(kStk, 100.0, "pending");
	uint32_t enumCnt = 0;
	double enumQty = 0.0;
	sel.enum_position([&](const char* code, double qty) {
		enumCnt++;
		if (strcmp(code, kStk) == 0)
			enumQty = qty;
	});
	EXPECT_EQ(1u, enumCnt);
	EXPECT_DOUBLE_EQ(100.0, enumQty);

	WTSBarStruct bar = r72::make_bar(20200102u, 931u, 10.0);
	sel.handle_bar_close(kStk, "d", 1, &bar);	// 日线分支 -> realPeriod d1
	sel.handle_bar_close(kStk, "m1", 5, &bar);	// 分钟分支 -> realPeriod m5
	sel.on_tick(kStk, nullptr, true);			// 空体，只验证可调用

	// 单字符周期：key 补 "1"，回放器无数据 -> 返回 NULL 但不崩溃
	EXPECT_EQ(nullptr, sel.stra_get_bars(kStk, "d", 5));

	sel.handle_replay_done();

	const std::filesystem::path dir = _out / "r92_sel_dump";
	const std::string signals = r72::read_text(dir / "signals.csv");
	EXPECT_EQ(2u, r72::line_count(signals));
	EXPECT_NE(std::string::npos, signals.find("pending"));

	const std::string json = r72::read_text(dir / "r92_sel_dump.json");
	ASSERT_FALSE(json.empty());
	EXPECT_NE(std::string::npos, json.find("\"signals\""));
	EXPECT_NE(std::string::npos, json.find("pending"));
}

// UftMocker：match_this_tick=true 时先排空任务再撮合，市价开/平/反手全部由公开
// stra_buy/stra_sell + handle_tick 驱动；CM_OpenCover 路径。
// Uft L250-263、L472-595、L597-676、L953-970、L828-887。
TEST_F(R92MockerResidual, UftMatchTickFillSellAndCover)
{
	r72::Loader loader;
	loader.ticks[std::string(kStk) + "#20200102"] = {
		r72::make_tick_struct(kStk, 20200102u, 93000000u, 10.0, 9.99, 10.01, 100, 100)
	};

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	UftMocker uft(&rp, "r92_uft_fill");
	WTSVariant* fcfg = uft_cfg("r92_no_such_uft_module", 0, true);
	EXPECT_FALSE(uft.init_uft_factory(fcfg));	// 只取 match_this_tick 标志
	fcfg->release();
	rp.register_sink(&uft, "r92_uft_fill");
	ASSERT_TRUE(rp.prepare());

	ASSERT_NE(0u, uft.stra_enter_long(kStk, 0.0, 2.0));
	WTSTickData* t1 = r72::make_tick(kStk, 20200102u, 93000000u, 10.0, 9.99, 10.01);
	uft.handle_tick(kStk, t1, 0);
	t1->release();
	EXPECT_DOUBLE_EQ(2.0, uft.stra_get_position(kStk, false, 1));
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_undone(kStk));

	// 多头浮盈（买一价 10.19）
	WTSTickData* t2 = r72::make_tick(kStk, 20200102u, 93100000u, 10.2, 10.19, 10.21);
	uft.handle_tick(kStk, t2, 0);
	t2->release();

	// 平 1 手
	ASSERT_EQ(1u, uft.stra_sell(kStk, 0.0, 1.0).size());
	WTSTickData* t3 = r72::make_tick(kStk, 20200102u, 93200000u, 10.2, 10.19, 10.21);
	uft.handle_tick(kStk, t3, 0);
	t3->release();
	EXPECT_DOUBLE_EQ(1.0, uft.stra_get_position(kStk, false, 1));

	// 卖 3 手：平掉剩余 1 手多仓，剩余 2 手开空
	ASSERT_EQ(2u, uft.stra_sell(kStk, 0.0, 3.0).size());
	WTSTickData* t4 = r72::make_tick(kStk, 20200102u, 93300000u, 10.0, 9.99, 10.01);
	uft.handle_tick(kStk, t4, 0);
	t4->release();
	EXPECT_DOUBLE_EQ(-2.0, uft.stra_get_position(kStk, false, 3));

	// 空头浮盈（卖一价 9.81）
	WTSTickData* t5 = r72::make_tick(kStk, 20200102u, 93400000u, 9.8, 9.79, 9.81);
	uft.handle_tick(kStk, t5, 0);
	t5->release();

	// 买 1 手平空
	ASSERT_EQ(1u, uft.stra_buy(kStk, 0.0, 1.0).size());
	WTSTickData* t6 = r72::make_tick(kStk, 20200102u, 93500000u, 9.9, 9.89, 9.91);
	uft.handle_tick(kStk, t6, 0);
	t6->release();
	EXPECT_DOUBLE_EQ(-1.0, uft.stra_get_position(kStk, false, 3));

	// 买 3 手：平掉剩余 1 手空仓，剩余 2 手开多
	ASSERT_EQ(2u, uft.stra_buy(kStk, 0.0, 3.0).size());
	WTSTickData* t7 = r72::make_tick(kStk, 20200102u, 93600000u, 10.0, 9.99, 10.01);
	uft.handle_tick(kStk, t7, 0);
	t7->release();
	EXPECT_DOUBLE_EQ(2.0, uft.stra_get_position(kStk, false, 1));
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_undone(kStk));

	uft.handle_replay_done();
	EXPECT_GE(r72::line_count(r72::read_text(_out / "r92_uft_fill" / "trades.csv")), 3u);
}

// UftMocker：CM_CoverToday 的平今/平昨、撤单释放今仓、随机错单与订单边界。
// Uft L428-433、L505-522、L568-585、L697-712、L758-773、L902-905、L920、
// L927-930、L943-948、L1172、L1218、L1291。
TEST_F(R92MockerResidual, UftCoverTodayCancelAndOrderEdges)
{
	r72::Loader loader;
	loader.ticks[std::string(kRb) + "#20200102"] = {
		r72::make_tick_struct(kRb, 20200102u, 93000000u, 3800.0, 3799.0, 3801.0, 100, 100)
	};
	// ask 价为 0 但 askqty>0：procOrder 的零价早退分支
	loader.ticks[std::string(kAg) + "#20200102"] = {
		r72::make_tick_struct(kAg, 20200102u, 93000000u, 6000.0, 5999.0, 0.0, 100, 100)
	};
	// askqty=0 但 ask 价正常：procOrder 的零量早退分支
	loader.ticks[std::string(kHc) + "#20200102"] = {
		r72::make_tick_struct(kHc, 20200102u, 93000000u, 3500.0, 3499.0, 3501.0, 100, 0)
	};

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	UftMocker uft(&rp, "r92_uft_today");
	WTSVariant* fcfg = uft_cfg("r92_no_such_uft_module", 0, true);
	EXPECT_FALSE(uft.init_uft_factory(fcfg));
	fcfg->release();
	rp.register_sink(&uft, "r92_uft_today");
	ASSERT_TRUE(rp.prepare());

	WTSCommodityInfo* rbComm = rp.get_commodity_info(kRb);
	ASSERT_NE(nullptr, rbComm);
	rbComm->setCoverMode(CM_CoverToday);

	// 开多 2 -> 平今 1 -> 平今 1
	ASSERT_NE(0u, uft.stra_enter_long(kRb, 0.0, 2.0));
	WTSTickData* rb1 = r72::make_tick(kRb, 20200102u, 93000000u, 3800.0, 3799.0, 3801.0);
	uft.handle_tick(kRb, rb1, 0);
	rb1->release();
	ASSERT_DOUBLE_EQ(2.0, uft.stra_get_position(kRb, false, 1));
	ASSERT_EQ(1u, uft.stra_sell(kRb, 0.0, 1.0).size());
	WTSTickData* rb2 = r72::make_tick(kRb, 20200102u, 93100000u, 3800.0, 3799.0, 3801.0);
	uft.handle_tick(kRb, rb2, 0);
	rb2->release();
	EXPECT_DOUBLE_EQ(1.0, uft.stra_get_position(kRb, false, 1));
	ASSERT_EQ(1u, uft.stra_sell(kRb, 0.0, 1.0).size());
	WTSTickData* rb3 = r72::make_tick(kRb, 20200102u, 93200000u, 3800.0, 3799.0, 3801.0);
	uft.handle_tick(kRb, rb3, 0);
	rb3->release();
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_position(kRb, false, 3));

	// 昨仓可用为 0：CM_CoverToday 下平昨被拒
	EXPECT_EQ(0u, uft.stra_exit_long(kRb, 0.0, 1.0, false, 0));

	// 开空 2 -> 买 1 平今 -> 平昨被拒
	ASSERT_NE(0u, uft.stra_enter_short(kRb, 0.0, 2.0));
	WTSTickData* rb4 = r72::make_tick(kRb, 20200102u, 93300000u, 3800.0, 3799.0, 3801.0);
	uft.handle_tick(kRb, rb4, 0);
	rb4->release();
	ASSERT_DOUBLE_EQ(-2.0, uft.stra_get_position(kRb, false, 3));
	ASSERT_EQ(1u, uft.stra_buy(kRb, 0.0, 1.0).size());
	WTSTickData* rb5 = r72::make_tick(kRb, 20200102u, 93400000u, 3800.0, 3799.0, 3801.0);
	uft.handle_tick(kRb, rb5, 0);
	rb5->release();
	EXPECT_DOUBLE_EQ(-1.0, uft.stra_get_position(kRb, false, 3));
	EXPECT_EQ(0u, uft.stra_exit_short(kRb, 0.0, 1.0, false, 0));

	// 平掉剩余空仓，再开多 2；挂平今单后撤单，撤单任务应把今仓可用量加回
	ASSERT_EQ(1u, uft.stra_buy(kRb, 0.0, 1.0).size());
	WTSTickData* rb6 = r72::make_tick(kRb, 20200102u, 93500000u, 3800.0, 3799.0, 3801.0);
	uft.handle_tick(kRb, rb6, 0);
	rb6->release();
	ASSERT_DOUBLE_EQ(0.0, uft.stra_get_position(kRb, false, 3));
	ASSERT_NE(0u, uft.stra_enter_long(kRb, 0.0, 2.0));
	WTSTickData* rb7 = r72::make_tick(kRb, 20200102u, 93600000u, 3800.0, 3799.0, 3801.0);
	uft.handle_tick(kRb, rb7, 0);
	rb7->release();
	ASSERT_DOUBLE_EQ(2.0, uft.stra_get_position(kRb, false, 1));
	uint32_t closeId = uft.stra_exit_long(kRb, 0.0, 1.0, true, 0);
	ASSERT_NE(0u, closeId);
	EXPECT_TRUE(uft.stra_cancel(closeId));
	WTSTickData* rb8 = r72::make_tick(kRb, 20200102u, 93700000u, 3800.0, 3799.0, 3801.0);
	uft.handle_tick(kRb, rb8, 0);	// match 分支先 procTask，撤单任务在此执行
	rb8->release();
	EXPECT_DOUBLE_EQ(2.0, uft.stra_get_position(kRb, false, 1));

	// askprice=0 -> 不成交
	ASSERT_NE(0u, uft.stra_enter_long(kAg, 0.0, 1.0));
	WTSTickData* ag1 = r72::make_tick(kAg, 20200102u, 93000000u, 6000.0, 5999.0, 0.0);
	uft.handle_tick(kAg, ag1, 0);
	ag1->release();
	EXPECT_DOUBLE_EQ(1.0, uft.stra_get_undone(kAg));
	EXPECT_TRUE(uft.stra_cancel_all(kAg).empty());
	WTSTickData* ag2 = r72::make_tick(kAg, 20200102u, 93100000u, 6000.0, 5999.0, 0.0);
	uft.handle_tick(kAg, ag2, 0);
	ag2->release();
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_undone(kAg));

	// 卖限价高于买一 -> 不成交
	ASSERT_NE(0u, uft.stra_enter_short(kAg, 6100.0, 1.0));
	WTSTickData* ag3 = r72::make_tick(kAg, 20200102u, 93200000u, 6000.0, 5999.0, 0.0);
	uft.handle_tick(kAg, ag3, 0);
	ag3->release();
	EXPECT_DOUBLE_EQ(-1.0, uft.stra_get_undone(kAg));
	uft.stra_cancel_all(kAg);
	WTSTickData* ag4 = r72::make_tick(kAg, 20200102u, 93300000u, 6000.0, 5999.0, 0.0);
	uft.handle_tick(kAg, ag4, 0);
	ag4->release();

	// askqty=0 -> 不成交
	ASSERT_NE(0u, uft.stra_enter_long(kHc, 0.0, 1.0));
	WTSTickData* hc1 = r72::make_tick(kHc, 20200102u, 93400000u, 3500.0, 3499.0, 3501.0);
	uft.handle_tick(kHc, hc1, 0);
	hc1->release();
	EXPECT_DOUBLE_EQ(1.0, uft.stra_get_undone(kHc));
	uft.stra_cancel_all(kHc);
	WTSTickData* hc2 = r72::make_tick(kHc, 20200102u, 93500000u, 3500.0, 3499.0, 3501.0);
	uft.handle_tick(kHc, hc2, 0);
	hc2->release();
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_undone(kHc));

	// 无行情的品种：procOrder 在 stra_get_last_tick 处早退
	ASSERT_NE(0u, uft.stra_enter_long(kCu, 0.0, 1.0));
	WTSTickData* cu = r72::make_tick(kCu, 20200102u, 93000000u, 70000.0, 69999.0, 70001.0);
	uft.handle_tick(kCu, cu, 0);
	cu->release();
	EXPECT_DOUBLE_EQ(1.0, uft.stra_get_undone(kCu));

	// 无品种信息：成交回写直接返回
	uft.on_trade(1u, "DCE.m.2405", true, 0, 1.0, 3000.0);

	// 平仓后 0 量明细残留：第二次平昨在 maxQty==0 处 continue
	uft.on_trade(10u, kCu, true, 0, 2.0, 100.0);
	uft.on_trade(11u, kCu, true, 0, 1.0, 101.0);
	uft.on_trade(12u, kCu, true, 1, 2.0, 102.0);
	uft.on_trade(13u, kCu, true, 1, 1.0, 103.0);
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_position(kCu, false, 1));

	// 空头平仓盈亏取反
	uft.on_trade(20u, kRu, false, 0, 2.0, 200.0);
	uft.on_trade(21u, kRu, false, 1, 1.0, 190.0);
	EXPECT_DOUBLE_EQ(-1.0, uft.stra_get_position(kRu, false, 3));
	// 直接注入 on_trade 时没有经过下单占用量，_newavail 仍为 2
	EXPECT_DOUBLE_EQ(2.0, uft.stra_get_position(kRu, true, 2));

	// 随机错单：error_rate=10000 时每个订单都被撤
	UftMocker err(&rp, "r92_uft_err");
	WTSVariant* ecfg = uft_cfg("r92_no_such_uft_module", 10000, true);
	EXPECT_FALSE(err.init_uft_factory(ecfg));
	ecfg->release();
	ASSERT_NE(0u, err.stra_enter_long(kRb, 0.0, 1.0));
	WTSTickData* rb9 = r72::make_tick(kRb, 20200102u, 93800000u, 3800.0, 3799.0, 3801.0);
	err.handle_tick(kRb, rb9, 0);
	rb9->release();
	EXPECT_DOUBLE_EQ(0.0, err.stra_get_position(kRb, false, 3));
	EXPECT_DOUBLE_EQ(0.0, err.stra_get_undone(kRb));
}

// HftMocker：非 match_this_tick 分支用「部分成交 -> 任务排空 -> 再成交」避开
// 挂单任务悬空（BUG-R92-001），覆盖 procOrder 的成交/清空与查询。
// Hft L393-408、L521-523、L765-786、L916、L926。
TEST_F(R92MockerResidual, HftPartialFillEraseAndQueries)
{
	r72::Loader loader;
	// 对手量只有 1：第一笔 tick 部分成交，订单保留，entrust 任务才能安全排空
	loader.ticks[std::string(kStk) + "#20200102"] = {
		r72::make_tick_struct(kStk, 20200102u, 93000000u, 10.0, 9.99, 10.0, 1, 1)
	};

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	HftMocker hft(&rp, "r92_hft_fill");
	rp.register_sink(&hft, "r92_hft_fill");
	ASSERT_TRUE(rp.prepare());

	ASSERT_EQ(1u, hft.stra_buy(kStk, 10.05, 2.0, "buy").size());
	WTSTickData* t1 = r72::make_tick(kStk, 20200102u, 93000000u, 10.0, 9.99, 10.1);
	hft.handle_tick(kStk, t1, 0);
	t1->release();
	EXPECT_DOUBLE_EQ(1.0, hft.stra_get_position(kStk));
	EXPECT_DOUBLE_EQ(1.0, hft.stra_get_undone(kStk));

	WTSTickData* t2 = r72::make_tick(kStk, 20200102u, 93100000u, 10.2, 10.19, 10.21);
	hft.handle_tick(kStk, t2, 0);
	t2->release();
	EXPECT_DOUBLE_EQ(2.0, hft.stra_get_position(kStk));
	EXPECT_DOUBLE_EQ(0.0, hft.stra_get_undone(kStk));
	EXPECT_GT(hft.stra_get_position_profit(kStk), 0.0);

	// 限价卖：买一 9.99 高于限价 9.0 -> 部分/全部成交
	ASSERT_EQ(1u, hft.stra_sell(kStk, 9.0, 2.0, "sell").size());
	WTSTickData* t3 = r72::make_tick(kStk, 20200102u, 93200000u, 10.0, 9.99, 10.01);
	hft.handle_tick(kStk, t3, 0);
	t3->release();
	EXPECT_DOUBLE_EQ(1.0, hft.stra_get_position(kStk));
	WTSTickData* t4 = r72::make_tick(kStk, 20200102u, 93300000u, 10.0, 9.99, 10.01);
	hft.handle_tick(kStk, t4, 0);
	t4->release();
	EXPECT_DOUBLE_EQ(0.0, hft.stra_get_position(kStk));
	EXPECT_DOUBLE_EQ(0.0, hft.stra_get_position_avgpx(kStk));	// 空仓 -> 0
	EXPECT_DOUBLE_EQ(0.0, hft.stra_get_position_profit("SSE.STK.999999"));
	EXPECT_DOUBLE_EQ(0.0, hft.stra_get_position_avgpx("SSE.STK.999999"));

	// 会话末仍有持仓 -> positions.csv 一行
	hft.on_trade(1u, kStk, true, 1.0, 10.0, "end");
	EXPECT_DOUBLE_EQ(1.0, hft.stra_get_position(kStk));
	hft.handle_session_end(20200102u);
	hft.handle_replay_done();
	EXPECT_EQ(2u, r72::line_count(r72::read_text(_out / "r92_hft_fill" / "positions.csv")));

	// 空头浮盈（卖一价），覆盖 update_dyn_profit 的空头 max_profit 分支
	hft.on_trade(2u, kCu, false, 2.0, 70000.0, "short");
	WTSTickData* s1 = r72::make_tick(kCu, 20200102u, 93400000u, 69900.0, 69899.0, 69901.0);
	hft.handle_tick(kCu, s1, 0);
	s1->release();
	EXPECT_DOUBLE_EQ(-2.0, hft.stra_get_position(kCu));
	EXPECT_GT(hft.stra_get_position_profit(kCu), 0.0);
}

// HftMocker：match_this_tick=true 的顺序、随机错单、零价/零量回写与 T+1 加仓。
// Hft L359-389、L521-523、L713-717、L732、L739-742、L755-759、L1092-1125。
TEST_F(R92MockerResidual, HftMatchTickErrorsZeroPriceAndT1)
{
	r72::Loader loader;
	loader.ticks[std::string(kStk) + "#20200102"] = {
		r72::make_tick_struct(kStk, 20200102u, 93000000u, 10.0, 9.99, 10.01, 100, 100)
	};
	loader.ticks[std::string(kRb) + "#20200102"] = {
		r72::make_tick_struct(kRb, 20200102u, 93000000u, 3800.0, 3799.0, 3801.0, 100, 100)
	};
	// 对手量为 0 / 对手价为 0 的两个独立品种
	loader.ticks[std::string(kRu) + "#20200102"] = {
		r72::make_tick_struct(kRu, 20200102u, 93000000u, 10000.0, 9999.0, 10001.0, 100, 0)
	};
	loader.ticks[std::string(kHc) + "#20200102"] = {
		r72::make_tick_struct(kHc, 20200102u, 93000000u, 3500.0, 3499.0, 0.0, 100, 100)
	};

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	HftMocker hft(&rp, "r92_hft_match");
	WTSVariant* fcfg = hft_cfg("r92_no_such_hft_module", 0, true);
	EXPECT_FALSE(hft.init_hft_factory(fcfg));
	fcfg->release();
	rp.register_sink(&hft, "r92_hft_match");
	ASSERT_TRUE(rp.prepare());

	ASSERT_EQ(1u, hft.stra_buy(kStk, 0.0, 2.0, "buy").size());
	WTSTickData* t1 = r72::make_tick(kStk, 20200102u, 93000000u, 10.0, 9.99, 10.01);
	hft.handle_tick(kStk, t1, 0);
	t1->release();
	EXPECT_DOUBLE_EQ(2.0, hft.stra_get_position(kStk));
	EXPECT_DOUBLE_EQ(0.0, hft.stra_get_undone(kStk));

	WTSTickData* t2 = r72::make_tick(kStk, 20200102u, 93100000u, 10.2, 10.19, 10.21);
	hft.handle_tick(kStk, t2, 0);
	t2->release();
	EXPECT_GT(hft.stra_get_position_profit(kStk), 0.0);

	ASSERT_EQ(1u, hft.stra_sell(kStk, 0.0, 1.0, "sell").size());
	WTSTickData* t3 = r72::make_tick(kStk, 20200102u, 93200000u, 10.1, 10.09, 10.11);
	hft.handle_tick(kStk, t3, 0);
	t3->release();
	EXPECT_DOUBLE_EQ(1.0, hft.stra_get_position(kStk));
	hft.handle_session_end(20200102u);
	hft.handle_replay_done();
	EXPECT_EQ(2u, r72::line_count(r72::read_text(_out / "r92_hft_match" / "positions.csv")));

	// 随机错单：match 先排空 entrust，再撤单
	HftMocker err(&rp, "r92_hft_err");
	WTSVariant* ecfg = hft_cfg("r92_no_such_hft_module", 10000, true);
	EXPECT_FALSE(err.init_hft_factory(ecfg));
	ecfg->release();
	ASSERT_EQ(1u, err.stra_buy(kStk, 0.0, 1.0, "err").size());
	WTSTickData* e1 = r72::make_tick(kStk, 20200102u, 93300000u, 10.0, 9.99, 10.01);
	err.handle_tick(kStk, e1, 0);
	e1->release();
	EXPECT_DOUBLE_EQ(0.0, err.stra_get_position(kStk));
	EXPECT_DOUBLE_EQ(0.0, err.stra_get_undone(kStk));

	// askqty=0（kRu）与 askprice=0（kHc）：订单保留
	ASSERT_EQ(1u, err.stra_buy(kRu, 0.0, 1.0, "noqty").size());
	WTSTickData* q1 = r72::make_tick(kRu, 20200102u, 93400000u, 10000.0, 9999.0, 10001.0);
	err.handle_tick(kRu, q1, 0);
	q1->release();
	EXPECT_DOUBLE_EQ(1.0, err.stra_get_undone(kRu));

	ASSERT_EQ(1u, err.stra_buy(kHc, 0.0, 1.0, "nopx").size());
	WTSTickData* q2 = r72::make_tick(kHc, 20200102u, 93500000u, 3500.0, 3499.0, 3501.0);
	err.handle_tick(kHc, q2, 0);
	q2->release();
	EXPECT_DOUBLE_EQ(1.0, err.stra_get_undone(kHc));
	err.stra_cancel(kRu, true, 0);
	err.stra_cancel(kHc, true, 0);

	// 卖限价高于买一：不成交（用可做空的期货，避免股票可用持仓守卫）
	ASSERT_EQ(1u, err.stra_sell(kRb, 3900.0, 1.0, "high").size());
	WTSTickData* q3 = r72::make_tick(kRb, 20200102u, 93600000u, 3800.0, 3799.0, 3801.0);
	err.handle_tick(kRb, q3, 0);
	q3->release();
	EXPECT_DOUBLE_EQ(-1.0, err.stra_get_undone(kRb));
	err.stra_cancel(kRb, false, 0);

	// T+1 同向加仓：第二次买入进入 open 分支并累计冻结
	WTSCommodityInfo* stkComm = rp.get_commodity_info(kStk);
	ASSERT_NE(nullptr, stkComm);
	stkComm->setTradingMode(TM_LongT1);
	HftMocker t1m(&rp, "r92_hft_t1");
	t1m.on_trade(1u, kStk, true, 100.0, 10.0, "t1");
	t1m.on_trade(2u, kStk, true, 50.0, 10.1, "t1b");
	EXPECT_DOUBLE_EQ(150.0, t1m.stra_get_position(kStk, false));
	// 首笔 100 已在反手分支冻结，第二笔同向加仓再冻结 50 -> 可用仍为 0
	EXPECT_DOUBLE_EQ(0.0, t1m.stra_get_position(kStk, true));

	// 成交价为 0 时回退到本地价格表；0 量成交直接早退
	t1m.on_trade(3u, kRb, true, 1.0, 0.0, "zero");
	EXPECT_DOUBLE_EQ(1.0, t1m.stra_get_position(kRb));
	t1m.on_trade(4u, kRb, true, 0.0, 10.0, "noop");
	EXPECT_DOUBLE_EQ(1.0, t1m.stra_get_position(kRb));
	t1m.on_trade(5u, kRb, false, 1.0, 10.0, "close");
	EXPECT_DOUBLE_EQ(0.0, t1m.stra_get_position(kRb));
}

// 缺陷归档（见 bugs/R92.md BUG-R92-001）：默认 match_this_tick=false 时，
// 市价单在第一次 handle_tick 内全部成交并 erase，随后 procTask 的 entrust 任务
// 用 _orders[localid] 插入空 shared_ptr 再解引用。启用即 SIGSEGV，故只保留哨兵。
TEST_F(R92MockerResidual, DISABLED_HftMarketOrderFirstTickDerefsErasedOrder)
{
	r72::Loader loader;
	loader.ticks[std::string(kStk) + "#20200102"] = {
		r72::make_tick_struct(kStk, 20200102u, 93000000u, 10.0, 9.99, 10.01, 100, 100)
	};

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	HftMocker hft(&rp, "r92_hft_crash");
	rp.register_sink(&hft, "r92_hft_crash");
	ASSERT_TRUE(rp.prepare());

	ASSERT_EQ(1u, hft.stra_buy(kStk, 0.0, 2.0, "crash").size());
	WTSTickData* tick = r72::make_tick(kStk, 20200102u, 93000000u, 10.0, 9.99, 10.01);
	hft.handle_tick(kStk, tick, 0);	// 订单全部成交并 erase；procTask 随后解引用 _orders[id]
	tick->release();
	FAIL() << "HftMocker must not dereference an order erased before procTask";
}

// ExecMocker：模块可装载但缺 createExecFact -> false；15:00 调度早退。
// Exec L49-86（L64-69）、L163-166。
TEST_F(R92MockerResidual, ExecFactoryCreatorMissingAndScheduleGuard)
{
	HisDataReplayer rp;
	ExecMocker mocker(&rp);
	WTSVariant* cfg = factory_cfg(pick_library());
	cfg->append("code", kStk);
	cfg->append("period", "m1");
	cfg->append("volunit", 1.0);
	cfg->append("volmode", static_cast<int32_t>(0));
	EXPECT_FALSE(mocker.init(cfg));
	cfg->release();

	// uTime==1500 在解引用 _last_tick 之前返回
	mocker.handle_schedule(20200102u, 1500u);
	EXPECT_DOUBLE_EQ(0.0, mocker.getPosition(kStk));
	EXPECT_DOUBLE_EQ(0.0, mocker.getUndoneQty(kStk));
}
