/**
 * n04_cta_mocker_state_test.cpp
 * 覆盖 src/WtBtCore/CtaMocker.{h,cpp} 中不依赖策略 DLL / 行情文件的纯查询与状态分支：
 *  - 构造与上下文 id 分配（makeCtxId 单调递增）
 *  - _pos_map / _sig_map 未命中时的持仓族接口返回值（stra_get_position /
 *    stra_get_position_avgpx / stra_get_position_profit /
 *    stra_get_detail_entertime / stra_get_detail_cost / stra_get_detail_profit）
 *  - 回放器时钟透传（stra_get_tdate / stra_get_date / stra_get_time）
 *  - 未订阅行情时的 stra_get_comminfo / stra_get_last_tick / 未知合约 rawcode
 *  - stra_log_* 与 writeLog 族在未初始化日志器时的安全路径
 * 不重复 l06 已覆盖的 MatchEngine / HisDataReplayer 时段费率接口。
 *
 * 说明：PosInfo / DetailInfo 是 CtaMocker 的私有嵌套类型，_pos_map、_sig_map、
 * _cond_entrusts 均无外部注入口，因此持仓族只能覆盖“未命中”分支；
 * “命中”分支依赖 do_set_position / proc_tick / handle_tick 全链路（需要行情与
 * 品种信息），见禁用用例与 bugs/n04_btcore_mockers.md BUG-N04-003。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/CtaMocker.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>

USING_NS_WTP;

namespace {
const char* kUnknown = "SSE.600000";
}

class CtaMockerStateTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		mocker = new CtaMocker(&replayer, "n04_cta_state");
	}

	void TearDown() override
	{
		delete mocker;
		mocker = nullptr;
	}

	HisDataReplayer replayer;
	CtaMocker* mocker;
};

TEST_F(CtaMockerStateTest, contextIdIsAllocatedMonotonically)
{
	uint32_t first = mocker->id();

	CtaMocker second(&replayer, "n04_cta_second");
	uint32_t secondId = second.id();
	EXPECT_NE(first, secondId);
	// makeCtxId 来自进程级原子自增，第二个实例必须更大
	EXPECT_GT(secondId, first);

	CtaMocker third(&replayer, "n04_cta_third", 5, false, NULL, true);
	EXPECT_GT(third.id(), secondId);
}

TEST_F(CtaMockerStateTest, positionQueriesReturnZeroWithoutAnyPosition)
{
	EXPECT_DOUBLE_EQ(0.0, mocker->stra_get_position(kUnknown));
	EXPECT_DOUBLE_EQ(0.0, mocker->stra_get_position(kUnknown, true));
	EXPECT_DOUBLE_EQ(0.0, mocker->stra_get_position(kUnknown, false, "tagA"));
	EXPECT_DOUBLE_EQ(0.0, mocker->stra_get_position("", true, "tagA"));

	EXPECT_DOUBLE_EQ(0.0, mocker->stra_get_position_avgpx(kUnknown));
	EXPECT_DOUBLE_EQ(0.0, mocker->stra_get_position_profit(kUnknown));
	EXPECT_EQ(0u, mocker->stra_get_detail_entertime(kUnknown, "tagA"));
	EXPECT_DOUBLE_EQ(0.0, mocker->stra_get_detail_cost(kUnknown, "tagA"));
	EXPECT_DOUBLE_EQ(0.0, mocker->stra_get_detail_profit(kUnknown, "tagA"));
	EXPECT_DOUBLE_EQ(0.0, mocker->stra_get_detail_profit(kUnknown, "tagA", 1));
	EXPECT_DOUBLE_EQ(0.0, mocker->stra_get_detail_profit(kUnknown, "tagA", 2));
}

TEST_F(CtaMockerStateTest, positionQueriesAreStableAndRepeatable)
{
	for (int i = 0; i < 3; i++)
	{
		EXPECT_DOUBLE_EQ(0.0, mocker->stra_get_position(kUnknown, i != 0));
	}
	// 查询接口不得产生副作用
	EXPECT_DOUBLE_EQ(0.0, mocker->stra_get_position_avgpx(kUnknown));
}

TEST_F(CtaMockerStateTest, tradeTimeQueriesMirrorTheReplayerClock)
{
	// 不断言墙钟/本地时区，只断言与回放器时钟的透传一致性
	EXPECT_EQ(replayer.get_trading_date(), mocker->stra_get_tdate());
	EXPECT_EQ(replayer.get_date(), mocker->stra_get_date());
	EXPECT_EQ(replayer.get_min_time(), mocker->stra_get_time());
	EXPECT_EQ(mocker->stra_get_tdate(), mocker->stra_get_tdate());
}

TEST_F(CtaMockerStateTest, marketDataLookupIsEmptyBeforeReplay)
{
	EXPECT_EQ(nullptr, mocker->stra_get_comminfo(kUnknown));
	EXPECT_EQ(nullptr, mocker->stra_get_last_tick(kUnknown));
	EXPECT_EQ(std::string(), mocker->stra_get_rawcode(kUnknown));
}

TEST_F(CtaMockerStateTest, loggingFamilyIsSafeWithoutLoggerInit)
{
	mocker->stra_log_info("n04 info");
	mocker->stra_log_debug("n04 debug");
	mocker->stra_log_warn("n04 warn");
	mocker->stra_log_error("n04 error");
	// 日志不得改变查询状态
	EXPECT_DOUBLE_EQ(0.0, mocker->stra_get_position(kUnknown));
}

/**
 * 缺陷/环境依赖类用例（禁用，见 bugs/n04_btcore_mockers.md BUG-N04-003）：
 * 以下 CtaMocker 接口在“无策略、无品种信息、无行情”的空态下直接解引用
 * _replayer 的查询结果（WTSCommodityInfo* / WTSTickData* 为 NULL）或依赖
 * 回放线程与输出目录约定，单进程内调用会崩溃或挂死。整个缺陷类统一收在
 * 本禁用用例中，同类输入不要拆成启用用例：
 *  - stra_get_price / stra_get_day_price：空行情缓存
 *  - stra_get_fund_data：资金统计依赖已初始化的行情/品种
 *  - do_set_position / stra_set_position / stra_enter_long / stra_exit_short：
 *    需要 get_commodity_info() 非空
 *  - append_signal / proc_tick / update_dyn_profit / get_cond_entrusts：条件委托链路
 *  - handle_tick / on_tick / handle_bar_close / handle_schedule / on_calculate：
 *    回放事件链路（需要 step_calc 线程配对）
 *  - dump_outputs / dump_stradata / dump_chartdata：输出目录约定
 *  - init_cta_factory(NULL)：空配置指针
 */
TEST_F(CtaMockerStateTest, DISABLED_emptyStateCallsCrashOrHang)
{
	mocker->stra_get_price(kUnknown);
	mocker->stra_get_day_price(kUnknown, 0);
	mocker->stra_get_fund_data(0);
	mocker->stra_get_fund_data(1);
	mocker->stra_set_position(kUnknown, 10);
	mocker->stra_enter_long(kUnknown, 10);
	mocker->stra_exit_short(kUnknown, 10);
	mocker->stra_save_user_data("k", "v");
	mocker->stra_load_user_data("k");
	mocker->handle_tick(kUnknown, nullptr, 0);
	mocker->handle_bar_close(kUnknown, "m1", 1, nullptr);
	mocker->handle_schedule(20240102, 931);
	mocker->on_init();
	mocker->handle_init();
	mocker->init_cta_factory(nullptr);
}
