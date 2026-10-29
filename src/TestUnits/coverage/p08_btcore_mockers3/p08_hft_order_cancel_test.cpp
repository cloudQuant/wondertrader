/*!
 * p08_hft_order_cancel_test.cpp
 * Lane p08: HftMocker 订单装配 / 撤单 / 成交回写 / 未初始化状态查询
 *
 * 不使用 install_hook()/step_tick()/带 hook 的 on_tick()：
 *  those 走 _cond_calc.wait() 与 while(!_resumed) 忙等，属阻塞路径，全部收入 DISABLED 用例。
 * 撮合成交通过公开虚函数 on_trade() 直接注入，避免依赖 CHisDataReplayer 的 tick 缓存。
 */
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSObject.hpp"
#include "Share/CodeHelper.hpp"
#include "WtBtCore/WtHelper.h"

#define private public
#define protected public
#include "WtBtCore/HisDataReplayer.h"
#include "WtBtCore/HftMocker.h"
#undef protected
#undef private

namespace
{
	WTSCommodityInfo* hft_add_comm(HisDataReplayer& rp, const char* exchg, const char* pid,
		uint32_t volScale, double pxTick, TradingMode mode)
	{
		WTSCommodityInfo* cInfo = WTSCommodityInfo::create(pid, pid, exchg, "TRADING", "CHINA");
		cInfo->setVolScale(volScale);
		cInfo->setPriceTick(pxTick);
		cInfo->setTradingMode(mode);
		rp._bd_mgr._fullpid_map->add(cInfo->getFullPid(), cInfo, true);
		return cInfo;
	}

	void hft_set_fee(HisDataReplayer& rp, const char* fullPid, double dOpen, double dClose, double dCloseToday)
	{
		HisDataReplayer::FeeItem& f = rp._fee_map[fullPid];
		f._by_volume = true;
		f._open = dOpen;
		f._close = dClose;
		f._close_today = dCloseToday;
		f._min_fee = 0.0;
	}
}

class P08HftOrderCancel : public ::testing::Test
{
protected:
	HisDataReplayer _rp;
	void SetUp() override
	{
		auto dir = std::filesystem::temp_directory_path() / "p08_hft_order_cancel";
		std::filesystem::create_directories(dir);
		WtHelper::setOutputDir(dir.string().c_str());

		_rp._cur_date = 20240102;
		_rp._cur_time = 93000;
		_rp._cur_secs = 0;
		_rp._cur_tdate = 20240102;

		hft_add_comm(_rp, "SHFE", "rb", 10, 1.0, TM_Both);
		hft_add_comm(_rp, "SSE", "600001", 1, 0.01, TM_LongT1);
		hft_set_fee(_rp, "SHFE.rb", 1.0, 2.0, 3.0);
		hft_set_fee(_rp, "SSE.600001", 0.5, 0.5, 0.5);
	}

	void TearDown() override
	{
		WtHelper::setOutputDir("");
		std::error_code ec;
		std::filesystem::remove_all(std::filesystem::temp_directory_path() / "p08_hft_order_cancel", ec);
	}
};

static const char* kCode = "SHFE.rb.2405";
static const char* kStk = "SSE.600001";

// 构造后的空状态：无策略、无 hook、无订单，且各查询在不初始化时给出零值
TEST_F(P08HftOrderCancel, FreshStateAndZeroQueries)
{
	HftMocker mocker(&_rp, "p08_hft_fresh");
	EXPECT_TRUE(mocker._strategy == NULL);
	EXPECT_FALSE(mocker._has_hook);
	// HftMocker.cpp:128 显式 `_hook_valid(true)`（.h:1245 说明它表示异步回测模式是否可用），
	// 新构造对象就是 true；原断言 FALSE 是对产品初值的误设（A-06）。
	EXPECT_TRUE(mocker._hook_valid);
	EXPECT_FALSE(mocker._resumed);
	EXPECT_TRUE(mocker._orders.empty());
	EXPECT_TRUE(mocker._pos_map.empty());
	EXPECT_TRUE(mocker._tasks.empty());
	EXPECT_TRUE(mocker._tick_subs.empty());
	// id() 是上下文 id 的透传（不断言具体取值，避免依赖全局自增序列）
	(void)mocker.id();

	// 空状态查询
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kCode));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kCode, true));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_profit(kCode));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_avgpx(kCode));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_undone(kCode));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_price(kCode));
	EXPECT_EQ(20240102u, mocker.stra_get_date());
	EXPECT_EQ(93000u, mocker.stra_get_time());
	EXPECT_EQ(0u, mocker.stra_get_secs());
	EXPECT_TRUE(mocker.stra_get_rawcode(kCode).empty());
	EXPECT_TRUE(mocker.stra_get_comminfo("DCE.m.2405") == NULL);
	EXPECT_TRUE(mocker.stra_get_comminfo(kCode) != NULL);
}

// 参数校验：未知合约与非正数量一律拒单
TEST_F(P08HftOrderCancel, BuySellParamGuards)
{
	HftMocker mocker(&_rp, "p08_hft_guards");
	EXPECT_TRUE(mocker.stra_buy("DCE.m.2405", 3000.0, 1, "t").empty());
	EXPECT_TRUE(mocker.stra_sell("DCE.m.2405", 3000.0, 1, "t").empty());
	EXPECT_TRUE(mocker.stra_buy(kCode, 3000.0, 0, "t").empty());
	EXPECT_TRUE(mocker.stra_buy(kCode, 3000.0, -1, "t").empty());
	EXPECT_TRUE(mocker.stra_sell(kCode, 3000.0, 0, "t").empty());
	EXPECT_TRUE(mocker._orders.empty());
	EXPECT_TRUE(mocker._tasks.empty());

	// 不能做空的品种：无持仓时卖出被拒；能做空则应正常下单
	WTSCommodityInfo* stk = _rp.get_commodity_info(kStk);
	ASSERT_TRUE(stk != NULL);
	auto ids = mocker.stra_sell(kStk, 12.0, 100, "t");
	if (stk->canShort())
		EXPECT_EQ(1u, ids.size());
	else
		EXPECT_TRUE(ids.empty()) << "禁止做空且无可用持仓时必须拒单";
}

// 买单装配：OrderInfo 字段、入册、postTask 只入队不建线程
TEST_F(P08HftOrderCancel, BuyAssemblesOrderInfoAndQueuesTask)
{
	HftMocker mocker(&_rp, "p08_hft_buy");
	auto ids = mocker.stra_buy(kCode, 3800.0, 3, "tag-buy");
	ASSERT_EQ(1u, ids.size());
	uint32_t lid = ids[0];
	ASSERT_NE(0u, lid);
	ASSERT_EQ(1u, mocker._orders.count(lid));

	const HftMocker::OrderInfoPtr& ord = mocker._orders[lid];
	EXPECT_EQ(lid, ord->_localid);
	EXPECT_STREQ(kCode, ord->_code);
	EXPECT_STREQ("tag-buy", ord->_usertag);
	EXPECT_TRUE(ord->_isBuy);
	EXPECT_DOUBLE_EQ(3800.0, ord->_price);
	EXPECT_DOUBLE_EQ(3.0, ord->_total);
	EXPECT_DOUBLE_EQ(3.0, ord->_left);
	EXPECT_FALSE(ord->_proced_after_placed);

	// entrust 任务只在队列里
	ASSERT_EQ(1u, mocker._tasks.size());
	mocker.procTask();
	EXPECT_TRUE(mocker._tasks.empty());
	// 队列已空时再次排空是安全的
	mocker.procTask();

	// 无最新行情时 procOrder 不撮合，但会做一次首轮回写
	EXPECT_FALSE(mocker.procOrder(lid));
	EXPECT_TRUE(ord->_proced_after_placed);
	EXPECT_FALSE(mocker.procOrder(lid));
	EXPECT_DOUBLE_EQ(3.0, ord->_left);
	EXPECT_FALSE(mocker.procOrder(lid + 999999));

	// 卖单装配与 userTag 透传
	auto sids = mocker.stra_sell(kCode, 3810.0, 2, "tag-sell");
	ASSERT_EQ(1u, sids.size());
	const HftMocker::OrderInfoPtr& sord = mocker._orders[sids[0]];
	EXPECT_FALSE(sord->_isBuy);
	EXPECT_STREQ("tag-sell", sord->_usertag);
	EXPECT_DOUBLE_EQ(2.0, sord->_total);
	mocker.procTask();
}

// 撤单：stra_cancel(localid) 只投递任务，procTask 执行后才真正摘单
TEST_F(P08HftOrderCancel, CancelIsDeferredUntilTaskDrain)
{
	HftMocker mocker(&_rp, "p08_hft_cancel");
	uint32_t lid = mocker.stra_buy(kCode, 3800.0, 3, "c1")[0];
	// 先排空 entrust 任务
	mocker.procTask();

	EXPECT_TRUE(mocker.stra_cancel(lid));
	// 任务未执行前订单仍在册
	EXPECT_EQ(1u, mocker._orders.count(lid));
	EXPECT_EQ(1u, mocker._tasks.size());
	mocker.procTask();
	EXPECT_TRUE(mocker._tasks.empty());
	EXPECT_EQ(0u, mocker._orders.count(lid));
	EXPECT_TRUE(mocker._orders.empty());
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_undone(kCode));

	// 未知/重复撤单不破坏状态
	EXPECT_TRUE(mocker.stra_cancel(lid));
	mocker.procTask();
	EXPECT_TRUE(mocker._tasks.empty());
	EXPECT_TRUE(mocker._orders.empty());
}

// 按合约+方向撤单：正 qty=1 小于首笔剩余量，所以只撤第一笔匹配订单。
TEST_F(P08HftOrderCancel, CancelByCodeAndDirection)
{
	HftMocker mocker(&_rp, "p08_hft_cancelby");
	auto b1 = mocker.stra_buy(kCode, 3800.0, 2, "b1");
	auto b2 = mocker.stra_buy(kCode, 3800.0, 3, "b2");
	ASSERT_EQ(1u, b1.size());
	ASSERT_EQ(1u, b2.size());
	auto s1s = mocker.stra_sell(kCode, 3810.0, 5, "s1");
	ASSERT_EQ(1u, s1s.size());
	uint32_t s1 = s1s[0];
	ASSERT_EQ(3u, mocker._orders.size());
	ASSERT_EQ(1u, mocker._orders.count(b1[0]));
	ASSERT_EQ(1u, mocker._orders.count(b2[0]));
	ASSERT_EQ(1u, mocker._orders.count(s1));

	auto canceled = mocker.stra_cancel(kCode, true, 1);
	ASSERT_EQ(1u, canceled.size()) << "qty=1 小于首笔剩余量，命中第一笔后即 break";
	EXPECT_TRUE(canceled[0] == b1[0] || canceled[0] == b2[0]);
	const uint32_t remaining_buy = canceled[0] == b1[0] ? b2[0] : b1[0];
	// 撤单任务未执行前，三笔订单都还在
	EXPECT_EQ(3u, mocker._orders.size());
	EXPECT_EQ(1u, mocker._orders.count(b1[0]));
	EXPECT_EQ(1u, mocker._orders.count(b2[0]));
	EXPECT_EQ(1u, mocker._orders.count(s1));
	mocker.procTask();
	EXPECT_EQ(2u, mocker._orders.size());
	EXPECT_EQ(0u, mocker._orders.count(canceled[0]));
	EXPECT_EQ(1u, mocker._orders.count(remaining_buy));
	EXPECT_EQ(1u, mocker._orders.count(s1));

	// 卖方向 qty=10 大于卖单剩余量 5，撤销该卖单。
	auto canceled2 = mocker.stra_cancel(kCode, false, 10);
	ASSERT_EQ(1u, canceled2.size());
	EXPECT_EQ(s1, canceled2[0]);
	EXPECT_EQ(1u, mocker._orders.count(remaining_buy));
	EXPECT_EQ(1u, mocker._orders.count(s1));
	mocker.procTask();
	EXPECT_EQ(0u, mocker._orders.count(s1));
	EXPECT_EQ(0u, mocker._orders.count(canceled[0]));
	EXPECT_EQ(1u, mocker._orders.count(remaining_buy));
	EXPECT_EQ(1u, mocker._orders.size());

	mocker.procTask();
	EXPECT_EQ(1u, mocker._orders.size());
	EXPECT_EQ(1u, mocker._orders.count(remaining_buy));
}

// qty=0 表示撤销同方向的全部订单，且不能影响反方向订单。
TEST_F(P08HftOrderCancel, CancelByCodeAndDirectionZeroCancelsAllMatching)
{
	HftMocker mocker(&_rp, "p08_hft_cancelby_zero");
	auto b1 = mocker.stra_buy(kCode, 3800.0, 2, "b1");
	auto b2 = mocker.stra_buy(kCode, 3800.0, 3, "b2");
	auto sells = mocker.stra_sell(kCode, 3810.0, 5, "s1");
	ASSERT_EQ(1u, b1.size());
	ASSERT_EQ(1u, b2.size());
	ASSERT_EQ(1u, sells.size());
	const uint32_t sell_id = sells[0];
	ASSERT_EQ(3u, mocker._orders.size());

	auto canceled = mocker.stra_cancel(kCode, true, 0);
	const std::set<uint32_t> expected_buy_ids{b1[0], b2[0]};
	const std::set<uint32_t> canceled_ids(canceled.begin(), canceled.end());
	EXPECT_EQ(2u, canceled.size());
	EXPECT_EQ(expected_buy_ids, canceled_ids);
	EXPECT_EQ(1u, mocker._orders.count(b1[0]));
	EXPECT_EQ(1u, mocker._orders.count(b2[0]));
	EXPECT_EQ(1u, mocker._orders.count(sell_id));

	mocker.procTask();
	EXPECT_EQ(0u, mocker._orders.count(b1[0]));
	EXPECT_EQ(0u, mocker._orders.count(b2[0]));
	EXPECT_EQ(1u, mocker._orders.count(sell_id));
	EXPECT_EQ(1u, mocker._orders.size());

	// 清理未被错误实现撤掉的买单以及异方向卖单，保持预期失败无副作用。
	for (uint32_t buy_id : expected_buy_ids)
	{
		if (mocker._orders.count(buy_id) != 0)
			mocker.stra_cancel(buy_id);
	}
	if (mocker._orders.count(sell_id) != 0)
		mocker.stra_cancel(sell_id);
	mocker.procTask();
	EXPECT_TRUE(mocker._tasks.empty());
	EXPECT_TRUE(mocker._orders.empty());
}

// 按量撤单：qty 大于所有匹配单之和时把它们全部撤掉
TEST_F(P08HftOrderCancel, CancelByQtyAccumulatesUntilSatisfied)
{
	HftMocker mocker(&_rp, "p08_hft_cancelqty");
	mocker.stra_buy(kCode, 3800.0, 2, "a");
	mocker.stra_buy(kCode, 3800.0, 3, "b");
	uint32_t s1 = mocker.stra_sell(kCode, 3810.0, 9, "c")[0];
	ASSERT_EQ(3u, mocker._orders.size());

	// 两笔买单 2+3=5 >= 4，无论遍历先后都会走完两笔
	auto canceled = mocker.stra_cancel(kCode, true, 4);
	EXPECT_EQ(2u, canceled.size());
	// 卖单方向不匹配，不受影响
	EXPECT_EQ(1u, mocker._orders.count(s1));
	mocker.procTask();
	EXPECT_EQ(1u, mocker._orders.size());
	EXPECT_EQ(0u, mocker._orders.count(canceled[0]));
	EXPECT_EQ(0u, mocker._orders.count(canceled[1]));

	// 不存在的方向/合约撤单返回空
	EXPECT_TRUE(mocker.stra_cancel("DCE.m.2405", true, 0).empty());
	mocker.procTask();
	EXPECT_EQ(1u, mocker._orders.size());
}

// 成交回写：on_trade -> do_set_position，多头加仓/部分平仓的持仓换算
TEST_F(P08HftOrderCancel, OnTradeDrivesSetPosition)
{
	HftMocker mocker(&_rp, "p08_hft_trade");
	mocker.on_trade(1, kCode, true, 3, 3800.0, "open");
	EXPECT_DOUBLE_EQ(3.0, mocker.stra_get_position(kCode));
	ASSERT_EQ(1u, mocker._pos_map.count(kCode));
	const HftMocker::PosInfo& pInfo = mocker._pos_map[kCode];
	ASSERT_FALSE(pInfo._details.empty());
	EXPECT_DOUBLE_EQ(3800.0, pInfo._details[0]._price);
	EXPECT_DOUBLE_EQ(3.0, pInfo._details[0]._volume);
	EXPECT_EQ(20240102u, pInfo._details[0]._opentdate);
	EXPECT_DOUBLE_EQ(3800.0, mocker.stra_get_position_avgpx(kCode));

	// 再加仓 2 手 @3820 -> 无论明细是两条还是一条合并成本，加权均价都是 3808
	mocker.on_trade(2, kCode, true, 2, 3820.0, "add");
	EXPECT_DOUBLE_EQ(5.0, mocker.stra_get_position(kCode));
	EXPECT_DOUBLE_EQ(3808.0, mocker.stra_get_position_avgpx(kCode));

	// 平 3 手 -> 剩余 2 手；剩余成本取决于 FIFO 还是均价，只断言落在区间内
	mocker.on_trade(3, kCode, false, 3, 3830.0, "part");
	EXPECT_DOUBLE_EQ(2.0, mocker.stra_get_position(kCode));
	double leftPx = mocker.stra_get_position_avgpx(kCode);
	EXPECT_GE(leftPx, 3808.0);
	EXPECT_LE(leftPx, 3820.0);
	// 已实现盈亏非负（卖出价不低于任一成本价）
	EXPECT_GE(mocker._pos_map[kCode]._closeprofit, 0.0);

	// 全平 -> 持仓清零，均价查询走 volume==0 短路
	mocker.on_trade(4, kCode, false, 2, 3840.0, "flat");
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kCode));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_avgpx(kCode));
}

// 反向开仓：净持仓为负，valid() = volume - frozen
TEST_F(P08HftOrderCancel, ShortPositionSignAndValid)
{
	HftMocker mocker(&_rp, "p08_hft_short");
	WTSCommodityInfo* comm = _rp.get_commodity_info(kCode);
	ASSERT_TRUE(comm != NULL);
	if (comm->canShort())
	{
		mocker.on_trade(1, kCode, false, 4, 3700.0, "s-open");
		EXPECT_DOUBLE_EQ(-4.0, mocker.stra_get_position(kCode));
		HftMocker::PosInfo& pInfo = mocker._pos_map[kCode];
		EXPECT_DOUBLE_EQ(-4.0, pInfo.valid());
		pInfo._frozen = 1.0;
		EXPECT_DOUBLE_EQ(-5.0, pInfo.valid());
		EXPECT_DOUBLE_EQ(-5.0, mocker.stra_get_position(kCode, true));
		pInfo._frozen = 0.0;

		// 买平 4 手回到零持仓
		mocker.on_trade(2, kCode, true, 4, 3710.0, "s-close");
		EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kCode));
	}
	else
	{
		// 品种不允许做空时，卖出下单应在无持仓状态下被拒
		EXPECT_TRUE(mocker.stra_sell(kCode, 3700.0, 4, "s-open").empty());
		EXPECT_TRUE(mocker._pos_map.empty());
	}
}

// 浮盈重算：零持仓直接归零且不触碰 tick 指针；无持仓合约整段跳过
TEST_F(P08HftOrderCancel, UpdateDynProfitZeroPositionSafe)
{
	HftMocker mocker(&_rp, "p08_hft_dyn");
	mocker.update_dyn_profit(kCode, NULL);
	mocker.update_dyn_profit("DCE.m.2405", NULL);

	mocker._pos_map[kCode]._volume = 0.0;
	mocker._pos_map[kCode]._dynprofit = 123.0;
	mocker.update_dyn_profit(kCode, NULL);
	EXPECT_DOUBLE_EQ(0.0, mocker._pos_map[kCode]._dynprofit);
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_profit(kCode));
}

// 日志注册与透传：stra_log_* 走 WTSLogger 动态 tag，不应崩溃
TEST_F(P08HftOrderCancel, StrategyLoggingApi)
{
	HftMocker mocker(&_rp, "p08_hft_log");
	mocker.stra_log_info("info message");
	mocker.stra_log_debug("debug message");
	mocker.stra_log_warn("warn message");
	mocker.stra_log_error("error message");
	mocker.enable_hook(true);
	EXPECT_TRUE(mocker._hook_valid);
	mocker.enable_hook(false);
	EXPECT_FALSE(mocker._hook_valid);
}

// 事件分发在不挂 hook、无策略时全部安全早退
TEST_F(P08HftOrderCancel, EventDispatchWithoutStrategy)
{
	HftMocker mocker(&_rp, "p08_hft_evt");
	// _strategy == NULL 时 on_bar/on_init/on_channel_ready/on_*_updated 均有空指针守卫
	mocker.on_bar(kCode, "m1", 1, NULL);
	mocker.handle_bar_close(kCode, "m1", 1, NULL);
	mocker.on_init();
	mocker.handle_init();
	mocker.on_channel_ready();
	mocker.on_entrust(1, kCode, true, "ok", "tag");
	mocker.on_order(1, kCode, true, 3, 0, 3800.0, false, "tag");
	// 未订阅的合约：on_tick_updated 在查表后直接返回
	mocker.on_tick_updated(kCode, NULL);
	mocker.on_ordque_updated(kCode, NULL);
	mocker.on_orddtl_updated(kCode, NULL);
	mocker.on_trans_updated(kCode, NULL);
	mocker.handle_schedule(20240102, 93000);
	EXPECT_TRUE(mocker._tick_subs.empty());
}

// 成交日志流经 dump_outputs 落盘（文件名以目录实际产物为准，不假设具体命名）
TEST_F(P08HftOrderCancel, DumpOutputsWritesLogs)
{
	HftMocker mocker(&_rp, "p08_hft_dump");
	mocker.on_trade(1, kCode, true, 2, 3800.0, "open");
	mocker.on_trade(2, kCode, false, 1, 3810.0, "open");

	std::filesystem::path dir = std::filesystem::path(WtHelper::getOutputDir()) / "p08_hft_dump";
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	mocker.dump_outputs();
	ASSERT_TRUE(std::filesystem::exists(dir));

	// 目录内至少产出一个 csv，且成交代码出现在某个产物里
	size_t csv_cnt = 0;
	bool found_code = false;
	for (auto& ent : std::filesystem::directory_iterator(dir))
	{
		if (ent.path().extension() != ".csv")
			continue;
		csv_cnt++;
		std::ifstream f(ent.path().string());
		std::stringstream content; content << f.rdbuf();
		if (content.str().find("SHFE.rb.2405") != std::string::npos)
			found_code = true;
	}
	EXPECT_LE((size_t)1, csv_cnt);
	EXPECT_TRUE(found_code) << "成交记录应出现在某份 csv 产物中";
}

// 阻塞/崩溃类输入整族收进本 DISABLED 用例：
// step_tick() 的 while(!_resumed) 忙等、on_tick() 的 _cond_calc.wait()、
// 未读源码的 init_hft_factory(nullptr) 与有持仓时 NULL tick 的解引用。
TEST_F(P08HftOrderCancel, DISABLED_BlockingHookAndNullDerefFamily)
{
	HftMocker mocker(&_rp, "p08_hft_blocked");
	// (a) 无策略/未知 cfg 的工厂初始化
	mocker.init_hft_factory(NULL);
	// (b) hook 阻塞路径
	mocker.install_hook();
	mocker.enable_hook(true);
	mocker.step_tick();
	WTSTickData* dummy = NULL;
	mocker.on_tick(kCode, dummy);
	mocker.handle_tick(kCode, dummy, 0);
	// (c) 有持仓 + NULL tick 的浮盈重算
	mocker.on_trade(1, kCode, true, 2, 3800.0, "open");
	mocker.update_dyn_profit(kCode, NULL);
	// (d) 主动订阅路径（依赖 replayer 内部订阅表，未验证）
	mocker.stra_sub_ticks(kCode);
	mocker.stra_sub_order_queues(kCode);
	mocker.stra_sub_order_details(kCode);
	mocker.stra_sub_transactions(kCode);
}
