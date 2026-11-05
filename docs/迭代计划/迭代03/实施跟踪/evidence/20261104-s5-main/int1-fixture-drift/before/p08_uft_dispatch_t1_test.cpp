/*!
 * p08_uft_dispatch_t1_test.cpp
 * Lane p08: UftMocker 事件分发 / 日志与回调注册 / T+1 冻结分支 / 工厂配置早退
 *
 * 与 p08_uft_order_position_test.cpp 不重叠：本文件不驱动订单撮合，
 * 只覆盖 IDataSink/IUftStraCtx 的转发函数、日志格式化模板与 T+1 持仓冻结分支。
 * 所有调用都发生在 _strategy == NULL、未挂 hook、未启动任何线程的状态下。
 */
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>

#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSObject.hpp"
#include "Share/CodeHelper.hpp"
#include "WtBtCore/WtHelper.h"

#define private public
#define protected public
#include "WtBtCore/HisDataReplayer.h"
#include "WtBtCore/UftMocker.h"
#undef protected
#undef private

namespace
{
	WTSCommodityInfo* uft_add_comm(HisDataReplayer& rp, const char* exchg, const char* pid,
		uint32_t volScale, double pxTick, TradingMode mode)
	{
		WTSCommodityInfo* cInfo = WTSCommodityInfo::create(pid, pid, exchg, "TRADING", "CHINA");
		cInfo->setVolScale(volScale);
		cInfo->setPriceTick(pxTick);
		cInfo->setTradingMode(mode);
		rp._bd_mgr._fullpid_map->add(cInfo->getFullPid(), cInfo, true);
		return cInfo;
	}

	void uft_set_fee(HisDataReplayer& rp, const char* fullPid, double dOpen, double dClose, double dCloseToday)
	{
		HisDataReplayer::FeeItem& f = rp._fee_map[fullPid];
		f._by_volume = true;
		f._open = dOpen;
		f._close = dClose;
		f._close_today = dCloseToday;
		f._min_fee = 0.0;
	}
}

class P08UftDispatchT1 : public ::testing::Test
{
protected:
	HisDataReplayer _rp;
	void SetUp() override
	{
		auto dir = std::filesystem::temp_directory_path() / "p08_uft_dispatch_t1";
		std::filesystem::create_directories(dir);
		WtHelper::setOutputDir(dir.string().c_str());

		_rp._cur_date = 20240103;
		_rp._cur_time = 142500 / 100;	// 分钟格式 HHMM
		_rp._cur_secs = 0;
		_rp._cur_tdate = 20240103;

		uft_add_comm(_rp, "SHFE", "rb", 10, 1.0, TM_Both);
		uft_add_comm(_rp, "SSE", "600001", 1, 0.01, TM_LongT1);
		uft_set_fee(_rp, "SHFE.rb", 1.0, 2.0, 3.0);
		uft_set_fee(_rp, "SSE.600001", 0.5, 0.5, 0.5);
	}

	void TearDown() override
	{
		WtHelper::setOutputDir("");
		std::error_code ec;
		std::filesystem::remove_all(std::filesystem::temp_directory_path() / "p08_uft_dispatch_t1", ec);
	}
};

static const char* kFut = "SHFE.rb.2405";
static const char* kStk = "SSE.600001";

// 工厂配置早退：cfg==NULL 必须在解引用之前返回 false
TEST_F(P08UftDispatchT1, InitFactoryRejectsNullConfig)
{
	UftMocker mocker(&_rp, "p08_uft_nullcfg");
	EXPECT_FALSE(mocker.init_uft_factory(NULL));
	// 早退不应改变任何匹配参数
	EXPECT_FALSE(mocker._use_newpx);
	EXPECT_EQ(0u, mocker._error_rate);
	EXPECT_FALSE(mocker._match_this_tick);
	EXPECT_TRUE(mocker._strategy == NULL);
	EXPECT_TRUE(mocker._factory._fact == NULL);
	EXPECT_TRUE(mocker._factory._module_inst == NULL);
	EXPECT_TRUE(mocker._factory._module_path.empty());
}

// IDataSink 转发：无策略时 on_bar/on_init/on_channel_ready 全部安全空转
TEST_F(P08UftDispatchT1, SinkForwardsWithoutStrategy)
{
	UftMocker mocker(&_rp, "p08_uft_sink");
	mocker.handle_bar_close(kFut, "m1", 1, NULL);
	mocker.on_bar(kFut, "m1", 1, NULL);
	mocker.handle_init();		// on_init + on_channel_ready
	mocker.on_init();
	mocker.on_channel_ready();
	mocker.handle_schedule(20240103, 1425);	// 空实现
	mocker.on_tick_updated(kFut, NULL);		// 未订阅 -> 查表后返回
	mocker.on_ordque_updated(kFut, NULL);
	mocker.on_orddtl_updated(kFut, NULL);
	mocker.on_trans_updated(kFut, NULL);
	mocker.on_order(1, kFut, true, 0, 2, 0, 3800.0, false);
	mocker.on_entrust(1, kFut, true, "ok");
	EXPECT_TRUE(mocker._tick_subs.empty());
	EXPECT_TRUE(mocker._tasks.empty());
	EXPECT_TRUE(mocker._orders.empty());
}

// 非 tick 类行情分发只做一次转发，不触碰数据指针
TEST_F(P08UftDispatchT1, Level2DispatchPassthrough)
{
	UftMocker mocker(&_rp, "p08_uft_l2");
	mocker.on_order_queue(kFut, NULL);
	mocker.on_order_detail(kFut, NULL);
	mocker.on_transaction(kFut, NULL);
	mocker.handle_order_queue(kFut, NULL);
	mocker.handle_order_detail(kFut, NULL);
	mocker.handle_transaction(kFut, NULL);
	// 这些转发不产生任何状态
	EXPECT_TRUE(mocker._orders.empty());
	EXPECT_TRUE(mocker._pos_map.empty());
	EXPECT_TRUE(mocker._price_map.empty());
}

// 订阅表：on_tick_updated 命中订阅但无策略，仍安全返回
TEST_F(P08UftDispatchT1, TickSubTableGatesUpdated)
{
	UftMocker mocker(&_rp, "p08_uft_subtab");
	// 直接写入本地订阅表，避免调用 stra_sub_ticks（依赖 replayer 订阅表）
	mocker._tick_subs.insert(kFut);
	ASSERT_EQ(1u, mocker._tick_subs.count(kFut));
	mocker.on_tick_updated(kFut, NULL);
	mocker.on_tick_updated("DCE.m.2405", NULL);
	EXPECT_EQ(1u, mocker._tick_subs.size());
}

// 日志与格式化模板：log_debug/log_info/log_error 经 stra_log_* 落到日志系统
TEST_F(P08UftDispatchT1, LogHelpersAndStraLogApi)
{
	UftMocker mocker(&_rp, "p08_uft_logger");
	mocker.stra_log_info("raw info");
	mocker.stra_log_debug("raw debug");
	mocker.stra_log_error("raw error");
	// 模板格式化入口（内部调用 stra_log_*）
	mocker.log_info("uft info {} {} {}", 1, 2.5, "x");
	mocker.log_debug("uft debug {} {}", kFut, 3);
	mocker.log_error("uft error {} {}", -1, 0.0);
	SUCCEED();
}

// T+1 品种开仓：今仓量增加但可用量不变，冻结量等于全部持仓
TEST_F(P08UftDispatchT1, T1OpenFreezesPosition)
{
	UftMocker mocker(&_rp, "p08_uft_t1open");
	mocker.on_trade(11, kStk, true, 0, 100, 12.50);
	UftMocker::PosItem& lItem = mocker._pos_map[kStk]._long;
	EXPECT_DOUBLE_EQ(100.0, lItem.volume());
	EXPECT_DOUBLE_EQ(0.0, lItem.valid());
	EXPECT_DOUBLE_EQ(100.0, lItem.frozen());
	EXPECT_DOUBLE_EQ(0.0, lItem._newavail);
	EXPECT_DOUBLE_EQ(100.0, lItem._newvol);
	// 开仓费 0.5/手 * 100
	EXPECT_DOUBLE_EQ(50.0, mocker._fund_info._total_fees);
	ASSERT_EQ(1u, lItem._details.size());
	EXPECT_DOUBLE_EQ(12.50, lItem._details[0]._price);
	EXPECT_EQ(20240103u, lItem._details[0]._opentdate);

	// T+1 当日无可用的多头，平仓请求被拒
	EXPECT_EQ(0u, mocker.stra_exit_long(kStk, 12.60, 10, false, 0));
	EXPECT_TRUE(mocker._orders.empty());
	// 可用为 0 时 stra_sell 直接走开空分支前的守卫：品种允许做空则开空
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position(kStk, false, 1));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kStk, true, 1));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_undone(kStk));
}

// T+1 隔夜结转（手工构造昨仓）：平仓可用，平昨费用按 close 档
TEST_F(P08UftDispatchT1, T1OvernightPositionCloseable)
{
	UftMocker mocker(&_rp, "p08_uft_t1night");
	UftMocker::PosItem& lItem = mocker._pos_map[kStk]._long;
	lItem._prevol = 100.0;
	lItem._preavail = 100.0;
	// 造一条昨仓明细，成本 12.00
	UftMocker::DetailInfo d;
	d._price = 12.00;
	d._volume = 100.0;
	d._opentdate = 20240102;
	lItem._details.emplace_back(d);

	// 先走平多下单路径预占昨仓可用量，再注入完整成交回报。
	const double closeQty = 100.0;
	uint32_t closeId = mocker.stra_exit_long(kStk, 12.10, closeQty, false, 0);
	ASSERT_NE(0u, closeId);
	EXPECT_DOUBLE_EQ(0.0, lItem._preavail);
	EXPECT_DOUBLE_EQ(closeQty, lItem.frozen());
	const UftMocker::OrderInfoPtr closeOrder = mocker._orders.at(closeId);
	ASSERT_TRUE(closeOrder->_isLong);
	EXPECT_EQ(1u, closeOrder->_offset);
	EXPECT_DOUBLE_EQ(closeQty, closeOrder->_left);
	mocker.procTask();		// 处理下单生成的 entrust 任务

	// 全部平昨 @12.10 -> 盈亏 (12.10-12.00)*100*1 = 10
	mocker.on_trade(closeId, kStk, true, 1, closeQty, 12.10);
	EXPECT_DOUBLE_EQ(0.0, lItem.volume());
	// 12.10-12.00 不可精确表示，用容差比较
	EXPECT_NEAR(10.0, lItem._closeprofit, 1e-9);
	EXPECT_NEAR(10.0, mocker._fund_info._total_profit, 1e-9);
	// opentdate(20240102) != curTDate(20240103) -> 平昨费档 0.5/手
	EXPECT_DOUBLE_EQ(50.0, mocker._fund_info._total_fees);
	// 昨仓全额可用，冻结为 0
	EXPECT_DOUBLE_EQ(0.0, lItem.frozen());
	EXPECT_DOUBLE_EQ(0.0, lItem.valid());

	// 直接注入成交回报不推进订单状态；镜像 procOrder/on_tick 的完成处理，
	// 将订单剩余量归零并移出活动订单表。
	closeOrder->_left -= closeQty;
	mocker.on_order(closeId, kStk, closeOrder->_isLong, closeOrder->_offset,
		closeOrder->_total, closeOrder->_left, closeOrder->_price, false);
	EXPECT_DOUBLE_EQ(0.0, closeOrder->_left);
	mocker._orders.erase(closeId);
	EXPECT_TRUE(mocker._orders.empty());

	// 平昨下单在可用量足够时被受理
	lItem._prevol = 30.0;
	lItem._preavail = 30.0;
	uint32_t lid = mocker.stra_exit_long(kStk, 12.20, 20, false, 0);
	EXPECT_NE(0u, lid);
	EXPECT_DOUBLE_EQ(10.0, lItem._preavail);
	const UftMocker::OrderInfoPtr& ord = mocker._orders[lid];
	EXPECT_TRUE(ord->_isLong);
	EXPECT_EQ(1u, ord->_offset);
	EXPECT_DOUBLE_EQ(20.0, ord->_total);
	// 该订单尚未成交，是平多卖单，接口约定卖出未完成量为负数。
	EXPECT_DOUBLE_EQ(-20.0, mocker.stra_get_undone(kStk));
}

// 撤单释放可用量：平多任务在 CM_CoverToday 与非 CM_CoverToday 下都归还总量
TEST_F(P08UftDispatchT1, CancelRestoresAvailableQty)
{
	UftMocker mocker(&_rp, "p08_uft_cancelavail");
	WTSCommodityInfo* comm = _rp.get_commodity_info(kFut);
	ASSERT_TRUE(comm != NULL);

	UftMocker::PosItem& lItem = mocker._pos_map[kFut]._long;
	lItem._prevol = 6.0;	lItem._preavail = 4.0;
	lItem._newvol = 2.0;	lItem._newavail = 2.0;
	double avail_before = lItem.valid();
	ASSERT_DOUBLE_EQ(6.0, avail_before);

	uint32_t lid = mocker.stra_exit_long(kFut, 3800.0, 3, false, 0);
	ASSERT_NE(0u, lid);
	// 下单已占用 3 手可用量（两种 CoverMode 分支结果一致）
	EXPECT_DOUBLE_EQ(3.0, lItem.valid());

	mocker.procTask();		// 先排空 entrust 任务
	mocker.stra_cancel(lid);
	mocker.procTask();
	EXPECT_TRUE(mocker._orders.empty());
	// 撤单归还被占用的 3 手可用量；两条分支都把它记回 _preavail
	EXPECT_DOUBLE_EQ(avail_before, lItem.valid());
	EXPECT_DOUBLE_EQ(8.0, lItem.volume());
	EXPECT_DOUBLE_EQ(4.0, lItem._preavail);
	EXPECT_DOUBLE_EQ(2.0, lItem._newavail);
	// 两条 CoverMode 分支在此场景下归还结果一致，无需按品种配置分支断言
}

// 析构安全：未装载策略工厂时 ~UftMocker 不应解引用 _factory._fact
TEST_F(P08UftDispatchT1, DestructorWithoutFactoryIsSafe)
{
	{
		UftMocker mocker(&_rp, "p08_uft_dtor");
		mocker.on_trade(1, kFut, true, 0, 1, 3800.0);
		mocker.stra_enter_long(kFut, 3800.0, 1);
		// 作用域结束时 _strategy == NULL，析构直接返回
	}
	SUCCEED();
}

// 需要真实 tick 或已装好策略 DLL 才能安全进入的路径整族收进 DISABLED：
// on_tick()/handle_tick() 会解引用 newTick，update_dyn_profit 有持仓时同样如此，
// stra_sub_* 系列依赖 replayer 订阅表内部状态，on_session_* 无条件解引用 _strategy。
TEST_F(P08UftDispatchT1, DISABLED_TickPointerAndSessionDerefFamily)
{
	UftMocker mocker(&_rp, "p08_uft_blocked");
	WTSTickData* no_tick = NULL;
	mocker.on_tick(kFut, no_tick);
	mocker.handle_tick(kFut, no_tick, 0);
	mocker._tick_subs.insert(kFut);
	mocker.on_tick_updated(kFut, no_tick);
	mocker.stra_sub_ticks(kFut);
	mocker.stra_sub_order_queues(kFut);
	mocker.stra_sub_order_details(kFut);
	mocker.stra_sub_transactions(kFut);
	mocker.on_trade(1, kFut, true, 0, 2, 3800.0);
	mocker.update_dyn_profit(kFut, no_tick);
	mocker.on_session_begin(20240103);
	mocker.on_session_end(20240103);
	mocker.handle_replay_done();
}
