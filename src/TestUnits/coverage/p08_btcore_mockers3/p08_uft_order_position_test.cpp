/*!
 * p08_uft_order_position_test.cpp
 * Lane p08: UftMocker 下单—委托回报—成交回写—持仓/资金推进
 *
 * 关键发现：UftMocker::postTask 只做 _tasks.push()（线程分支已被源码注释掉），
 * procTask() 同步排空队列，因此整条下单链可以在单线程、无 hook、无行情线程的环境下驱动。
 * 本文件不使用任何 tick 对象（procOrder 走 "无最新行情 -> 不成交" 分支），
 * 成交回写通过公开虚函数 on_trade()/on_order() 直接注入。
 */
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
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
	// 注册品种：期货(双向、非T+1) + 股票(多头T+1)
	WTSCommodityInfo* add_comm(HisDataReplayer& rp, const char* exchg, const char* pid,
		uint32_t volScale, double pxTick, TradingMode mode)
	{
		WTSCommodityInfo* cInfo = WTSCommodityInfo::create(pid, pid, exchg, "TRADING", "CHINA");
		cInfo->setVolScale(volScale);
		cInfo->setPriceTick(pxTick);
		cInfo->setTradingMode(mode);
		rp._bd_mgr._fullpid_map->add(cInfo->getFullPid(), cInfo, true);
		return cInfo;
	}

	void set_fee(HisDataReplayer& rp, const char* fullPid, double dOpen, double dClose, double dCloseToday)
	{
		HisDataReplayer::FeeItem& f = rp._fee_map[fullPid];
		f._by_volume = true;
		f._open = dOpen;
		f._close = dClose;
		f._close_today = dCloseToday;
		f._min_fee = 0.0;
	}
}

class P08UftOrderPosition : public ::testing::Test
{
protected:
	HisDataReplayer _rp;
	WTSCommodityInfo* _rbComm;
	void SetUp() override
	{
		auto dir = std::filesystem::temp_directory_path() / "p08_uft_order_position";
		std::filesystem::create_directories(dir);
		WtHelper::setOutputDir(dir.string().c_str());

		// 固定模拟时钟：2024-01-02 09:30:00，交易日同为 20240102
		_rp._cur_date = 20240102;
		_rp._cur_time = 93000;
		_rp._cur_secs = 0;
		_rp._cur_tdate = 20240102;

		_rbComm = add_comm(_rp, "SHFE", "rb", 10, 1.0, TM_Both);
		add_comm(_rp, "SSE", "600001", 1, 0.01, TM_LongT1);
		set_fee(_rp, "SHFE.rb", 1.0, 2.0, 3.0);
		set_fee(_rp, "SSE.600001", 0.5, 0.5, 0.5);
	}

	void TearDown() override
	{
		WtHelper::setOutputDir("");
		std::error_code ec;
		std::filesystem::remove_all(std::filesystem::temp_directory_path() / "p08_uft_order_position", ec);
	}
};

static const char* kFut = "SHFE.rb.2405";

// 构造后的空状态：无订单、无持仓、无资金，且不会触碰任何行情/策略对象
TEST_F(P08UftOrderPosition, FreshStateIsEmpty)
{
	UftMocker mocker(&_rp, "p08_fresh");
	EXPECT_TRUE(mocker._orders.empty());
	EXPECT_TRUE(mocker._pos_map.empty());
	EXPECT_TRUE(mocker._tasks.empty());
	EXPECT_TRUE(mocker._tick_subs.empty());
	EXPECT_TRUE(mocker._user_datas.empty());
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_undone(kFut));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kFut));
	EXPECT_EQ(0u, mocker._error_rate);
	EXPECT_FALSE(mocker._use_newpx);
	EXPECT_FALSE(mocker._match_this_tick);
	EXPECT_TRUE(mocker._strategy == NULL);
	// id() 只是 _context_id 的透传
	EXPECT_EQ(mocker._context_id, mocker.id());
	EXPECT_GT(mocker.id(), 0u);
}

// 参数校验：未知合约/非正数量一律拒绝，不产生订单，也不产生持仓条目
TEST_F(P08UftOrderPosition, OrderParamGuards)
{
	UftMocker mocker(&_rp, "p08_guards");

	EXPECT_EQ(0u, mocker.stra_enter_long("DCE.m.2405", 3000.0, 1));
	EXPECT_EQ(0u, mocker.stra_enter_short("DCE.m.2405", 3000.0, 1));
	EXPECT_TRUE(mocker._orders.empty());

	// 已知合约但数量非法
	EXPECT_EQ(0u, mocker.stra_enter_long(kFut, 3000.0, 0));
	EXPECT_EQ(0u, mocker.stra_enter_short(kFut, 3000.0, -2));
	EXPECT_TRUE(mocker.stra_buy("DCE.m.2405", 3000.0, 1).empty());
	EXPECT_TRUE(mocker.stra_sell("DCE.m.2405", 3000.0, 1).empty());
	EXPECT_TRUE(mocker.stra_buy(kFut, 3000.0, 0).empty());
	EXPECT_TRUE(mocker.stra_sell(kFut, 3000.0, 0).empty());
	EXPECT_TRUE(mocker._orders.empty());
}

// 开仓下单：订单结构体装配 + postTask/procTask 同步排空 + procOrder 无行情分支
TEST_F(P08UftOrderPosition, EnterLongAssemblesOrderAndDrainsTasks)
{
	UftMocker mocker(&_rp, "p08_enter");

	uint32_t lid = mocker.stra_enter_long(kFut, 3800.0, 3);
	ASSERT_NE(0u, lid);
	ASSERT_EQ(1u, mocker._orders.count(lid));

	const UftMocker::OrderInfoPtr& ord = mocker._orders[lid];
	EXPECT_EQ(lid, ord->_localid);
	EXPECT_STREQ(kFut, ord->_code);
	EXPECT_TRUE(ord->_isLong);
	EXPECT_EQ(0u, ord->_offset);			// 0 = 开仓
	EXPECT_DOUBLE_EQ(3800.0, ord->_price);
	EXPECT_DOUBLE_EQ(3.0, ord->_total);
	EXPECT_DOUBLE_EQ(3.0, ord->_left);
	EXPECT_FALSE(ord->_proced_after_placed);

	// 未成交前挂单量为正（多头 +，空头 -）
	EXPECT_DOUBLE_EQ(3.0, mocker.stra_get_undone(kFut));
	// 未知合约没有挂单
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_undone("DCE.m.2405"));

	// postTask 只入队，不启动线程；procTask 同步排空
	// （stra_enter_long 自身已投递一个 entrust 任务）
	mocker.postTask([](){});
	ASSERT_EQ(2u, mocker._tasks.size());
	mocker.procTask();
	EXPECT_TRUE(mocker._tasks.empty());
	// procTask 对空队列直接返回（不锁 _mtx_control）
	mocker.procTask();

	// 无最新 tick -> 不撮合，但首轮回写会置位 _proced_after_placed
	EXPECT_FALSE(mocker.procOrder(lid));
	EXPECT_TRUE(ord->_proced_after_placed);
	EXPECT_FALSE(mocker.procOrder(lid));
	EXPECT_DOUBLE_EQ(3.0, ord->_left);

	// 空/未知 localid 的 procOrder 安全返回 false
	EXPECT_FALSE(mocker.procOrder(lid + 999999));
}

// 卖单撤单：撤单任务释放持仓可用量并把订单摘除
TEST_F(P08UftOrderPosition, CancelShortOpenReleasesFrozenAvail)
{
	UftMocker mocker(&_rp, "p08_cancel");

	// 手工造 4 手昨空仓，使两种 CoverMode 分支下的可用量语义一致
	UftMocker::PosItem& sItem = mocker._pos_map[kFut]._short;
	sItem._prevol = 4.0;
	sItem._preavail = 4.0;
	ASSERT_DOUBLE_EQ(4.0, sItem.volume());
	ASSERT_DOUBLE_EQ(4.0, sItem.valid());

	uint32_t clid = mocker.stra_exit_short(kFut, 3710.0, 2, false, 0);
	ASSERT_NE(0u, clid);
	// 平空订单被标记为 _isLong=false -> 挂单量按负数累计
	EXPECT_DOUBLE_EQ(-2.0, mocker.stra_get_undone(kFut));
	// 平仓下单已占用 2 手可用量
	EXPECT_DOUBLE_EQ(2.0, sItem.valid());
	EXPECT_EQ(1u, mocker._tasks.size());
	mocker.procTask();		// 排空 entrust 任务

	// 撤单：postTask -> procTask 同步执行释放逻辑
	EXPECT_TRUE(mocker.stra_cancel(clid));
	EXPECT_EQ(1u, mocker._tasks.size());
	mocker.procTask();
	EXPECT_TRUE(mocker._tasks.empty());
	EXPECT_EQ(0u, mocker._orders.count(clid));
	EXPECT_TRUE(mocker._orders.empty());
	// 可用量原额归还
	EXPECT_DOUBLE_EQ(4.0, sItem.valid());
	EXPECT_DOUBLE_EQ(4.0, sItem.volume());

	// 未知/重复 localid 撤单：任务内 early-return，不破坏队列
	EXPECT_TRUE(mocker.stra_cancel(clid));
	mocker.procTask();
	EXPECT_TRUE(mocker._tasks.empty());
	EXPECT_TRUE(mocker._orders.empty());
}

// stra_cancel_all 遍历匹配合约并返回（当前实现恒返回空容器，见 bugs）
TEST_F(P08UftOrderPosition, CancelAllScansMatchingCodeOnly)
{
	UftMocker mocker(&_rp, "p08_cancelall");
	uint32_t a = mocker.stra_enter_long(kFut, 3800.0, 1);
	uint32_t b = mocker.stra_enter_long("SSE.600001", 10.0, 100);
	ASSERT_NE(0u, a);
	ASSERT_NE(0u, b);
	ASSERT_EQ(2u, mocker._orders.size());

	auto ids = mocker.stra_cancel_all(kFut);
	EXPECT_TRUE(ids.empty());
	// 撤单任务尚未执行，两笔订单仍在册
	EXPECT_EQ(2u, mocker._orders.size());
	mocker.procTask();
	EXPECT_EQ(0u, mocker._orders.count(a)) << "目标合约的订单应被撤掉";
	EXPECT_EQ(1u, mocker._orders.count(b)) << "其它合约订单不受影响";
	// 撤单后剩余订单的挂单量仍可查询
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_undone(kFut));
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_undone("SSE.600001"));
}

// 买卖先平后开：stra_buy 先消耗空头，剩余才开多
TEST_F(P08UftOrderPosition, StraBuyCoversShortThenOpensLong)
{
	UftMocker mocker(&_rp, "p08_buycover");
	UftMocker::PosItem& sItem = mocker._pos_map[kFut]._short;
	sItem._prevol = 2.0;
	sItem._preavail = 2.0;
	// 让平仓路径能取到品种信息
	ASSERT_TRUE(_rbComm != NULL);

	auto ids = mocker.stra_buy(kFut, 3800.0, 5);
	// 无论品种是分平昨平今还是不分，此处都只产生 2 笔：平空 2 手 + 开多 3 手
	ASSERT_EQ(2u, ids.size());
	EXPECT_EQ(2u, mocker._orders.size());

	const UftMocker::OrderInfoPtr& coverOrd = mocker._orders[ids[0]];
	EXPECT_FALSE(coverOrd->_isLong);
	EXPECT_DOUBLE_EQ(2.0, coverOrd->_total);
	EXPECT_EQ(1u, coverOrd->_offset);	// 平昨

	const UftMocker::OrderInfoPtr& openOrd = mocker._orders[ids[1]];
	EXPECT_TRUE(openOrd->_isLong);
	EXPECT_EQ(0u, openOrd->_offset);
	EXPECT_DOUBLE_EQ(3.0, openOrd->_total);
	EXPECT_DOUBLE_EQ(3.0, openOrd->_left);
	EXPECT_STREQ(kFut, openOrd->_code);

	// 可用空头被全额占用
	EXPECT_DOUBLE_EQ(0.0, sItem.valid());
	EXPECT_DOUBLE_EQ(2.0, sItem.volume());
	// 挂单量 = +3(买开) - 2(卖平)
	EXPECT_DOUBLE_EQ(1.0, mocker.stra_get_undone(kFut));
}

// 平仓可用量不足：返回 0 且不产生订单
TEST_F(P08UftOrderPosition, ExitRejectedWithoutAvailablePosition)
{
	UftMocker mocker(&_rp, "p08_insufficient");
	// 无任何持仓时平多/平空应被拒绝
	EXPECT_EQ(0u, mocker.stra_exit_long(kFut, 3800.0, 1, false, 0));
	EXPECT_EQ(0u, mocker.stra_exit_short(kFut, 3800.0, 1, false, 0));
	EXPECT_TRUE(mocker._orders.empty());

	// 造 2 手可用多头，请求平 5 手
	UftMocker::PosItem& lItem = mocker._pos_map[kFut]._long;
	lItem._prevol = 2.0;
	lItem._preavail = 2.0;
	EXPECT_EQ(0u, mocker.stra_exit_long(kFut, 3800.0, 5, false, 0));
	EXPECT_DOUBLE_EQ(2.0, lItem._preavail) << "拒绝路径不应扣减可用量";
}

// 开仓成交回写：明细/今仓/费用/成交日志
TEST_F(P08UftOrderPosition, OpenTradeWritesDetailAndFee)
{
	UftMocker mocker(&_rp, "p08_openfill");
	// 价格为 0 时取 _price_map 的成交价
	mocker._price_map[kFut] = 3799.0;

	mocker.on_trade(1, kFut, true, 0, 3, 0.0);
	UftMocker::PosInfo& pInfo = mocker._pos_map[kFut];
	EXPECT_DOUBLE_EQ(3.0, pInfo._long.volume());
	EXPECT_DOUBLE_EQ(3.0, pInfo._long.valid());
	EXPECT_DOUBLE_EQ(0.0, pInfo._long.frozen());
	EXPECT_DOUBLE_EQ(0.0, pInfo._short.volume());
	ASSERT_EQ(1u, pInfo._long._details.size());

	const UftMocker::DetailInfo& d = pInfo._long._details[0];
	EXPECT_DOUBLE_EQ(3799.0, d._price);
	EXPECT_DOUBLE_EQ(3.0, d._volume);
	EXPECT_EQ((uint32_t)20240102, d._opentdate);
	// curTm = date*1e9 + min_time*1e5 + secs
	EXPECT_EQ((uint64_t)20240102 * 1000000000ULL + (uint64_t)93000 * 100000ULL, d._opentime);

	// 开仓费 1.0/手 * 3
	EXPECT_DOUBLE_EQ(3.0, mocker._fund_info._total_fees);
	EXPECT_DOUBLE_EQ(0.0, mocker._fund_info._total_profit);
	EXPECT_NE(std::string::npos, mocker._trade_logs.str().find("SHFE.rb.2405"));
	EXPECT_NE(std::string::npos, mocker._trade_logs.str().find("LONG"));
	EXPECT_NE(std::string::npos, mocker._trade_logs.str().find("3799"));
}

// 平仓成交回写：盈亏按乘数计算、当日平仓记平今费
TEST_F(P08UftOrderPosition, CloseTradeComputesProfitAndTodayFee)
{
	UftMocker mocker(&_rp, "p08_closefill");
	mocker.on_trade(1, kFut, true, 0, 3, 3800.0);
	ASSERT_DOUBLE_EQ(3.0, mocker.stra_get_position(kFut));

	// 平 2 手 @3810，多头盈亏 = (3810-3800)*2*10 = 200
	mocker.on_trade(2, kFut, true, 1, 2, 3810.0);
	UftMocker::PosItem& lItem = mocker._pos_map[kFut]._long;
	EXPECT_DOUBLE_EQ(1.0, lItem.volume());
	EXPECT_DOUBLE_EQ(200.0, lItem._closeprofit);
	EXPECT_DOUBLE_EQ(200.0, mocker._fund_info._total_profit);
	// 开仓费 3.0 + 平今费 3.0/手*2 = 9.0（成交明细 opentdate == curTDate）
	EXPECT_DOUBLE_EQ(9.0, mocker._fund_info._total_fees);
	// 平仓盈亏写入平仓日志
	EXPECT_NE(std::string::npos, mocker._close_logs.str().find("3810"));
	EXPECT_NE(std::string::npos, mocker._close_logs.str().find("SHFE.rb.2405"));
}

// 平今分支：只处理今仓明细，浮盈等比缩放
TEST_F(P08UftOrderPosition, CloseTodayBranchScalesDynProfit)
{
	UftMocker mocker(&_rp, "p08_closetoday");
	mocker.on_trade(1, kFut, true, 0, 4, 3800.0);
	UftMocker::PosItem& lItem = mocker._pos_map[kFut]._long;
	ASSERT_DOUBLE_EQ(4.0, lItem.volume());
	// 手工置一个浮盈，验证平今后的等比缩放
	lItem._dynprofit = 100.0;

	// 平今 4 手 @3805 -> 盈亏 (3805-3800)*4*10 = 200
	mocker.on_trade(2, kFut, true, 2, 4, 3805.0);
	EXPECT_DOUBLE_EQ(0.0, lItem.volume());
	EXPECT_DOUBLE_EQ(200.0, lItem._closeprofit);
	EXPECT_DOUBLE_EQ(0.0, lItem._dynprofit);
	// 开仓 4.0 + 平今 3.0*4 = 16.0
	EXPECT_DOUBLE_EQ(16.0, mocker._fund_info._total_fees);
	// 当前实现 erase(sit, eit) 在单一明细上是空区间，0 量明细会残留
	ASSERT_EQ(1u, lItem._details.size());
	EXPECT_DOUBLE_EQ(0.0, lItem._details[0]._volume);
}

// 空头持仓盈亏方向取反
TEST_F(P08UftOrderPosition, ShortPositionProfitSign)
{
	UftMocker mocker(&_rp, "p08_shortsign");
	mocker.on_trade(1, kFut, false, 0, 2, 3800.0);
	EXPECT_DOUBLE_EQ(-2.0, mocker.stra_get_position(kFut, false, 3));
	EXPECT_DOUBLE_EQ(-2.0, mocker.stra_get_local_position(kFut));
	EXPECT_DOUBLE_EQ(2.0, mocker.stra_get_position(kFut, false, 2));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kFut, false, 1));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kFut, true, 1));

	// 空头价格上涨 => 亏损
	mocker.on_trade(2, kFut, false, 1, 2, 3820.0);
	UftMocker::PosItem& sItem = mocker._pos_map[kFut]._short;
	EXPECT_DOUBLE_EQ(0.0, sItem.volume());
	EXPECT_DOUBLE_EQ(-400.0, sItem._closeprofit);
	EXPECT_DOUBLE_EQ(-400.0, mocker._fund_info._total_profit);
}

// 持仓查询矩阵 + 未知合约查询（只读，_pos_map 会被 operator[] 建条目）
TEST_F(P08UftOrderPosition, PositionQueryMatrix)
{
	UftMocker mocker(&_rp, "p08_query");
	UftMocker::PosInfo& pInfo = mocker._pos_map[kFut];
	pInfo._long._prevol = 5.0;	pInfo._long._preavail = 3.0;
	pInfo._long._newvol = 2.0;	pInfo._long._newavail = 0.0;
	pInfo._short._prevol = 4.0;	pInfo._short._preavail = 4.0;

	EXPECT_DOUBLE_EQ(7.0, mocker.stra_get_position(kFut, false, 1));
	EXPECT_DOUBLE_EQ(3.0, mocker.stra_get_position(kFut, true, 1));
	EXPECT_DOUBLE_EQ(4.0, mocker.stra_get_position(kFut, false, 2));
	EXPECT_DOUBLE_EQ(4.0, mocker.stra_get_position(kFut, true, 2));
	EXPECT_DOUBLE_EQ(3.0, mocker.stra_get_position(kFut, false, 3));
	EXPECT_DOUBLE_EQ(-1.0, mocker.stra_get_position(kFut, true, 3));
	// 默认 flag=3（净头寸）
	EXPECT_DOUBLE_EQ(3.0, mocker.stra_get_position(kFut));
	EXPECT_DOUBLE_EQ(3.0, mocker.stra_get_local_position(kFut));
	// 冻结量 = 总量 - 可用量
	EXPECT_DOUBLE_EQ(4.0, pInfo._long.frozen());
	EXPECT_DOUBLE_EQ(0.0, pInfo._short.frozen());

	// 未出现过的合约：净头寸 0
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position("DCE.m.2405", true, 3));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_undone("DCE.m.2405"));

	// 品种/时钟透传
	EXPECT_TRUE(mocker.stra_get_comminfo(kFut) != NULL);
	EXPECT_TRUE(mocker.stra_get_comminfo("DCE.m.2405") == NULL);
	EXPECT_EQ(20240102u, mocker.stra_get_date());
	EXPECT_EQ(93000u, mocker.stra_get_time());
	EXPECT_EQ(0u, mocker.stra_get_secs());
}

// 空持仓的浮盈重算不触碰 tick 指针，可以在无行情对象时安全调用
TEST_F(P08UftOrderPosition, UpdateDynProfitSkipsEmptyPositions)
{
	UftMocker mocker(&_rp, "p08_dynpx");
	// 未建立持仓：直接返回
	mocker.update_dyn_profit(kFut, NULL);
	// 多头 volume()==0 分支：不解引用 newTick
	mocker._pos_map[kFut]._long._prevol = 0.0;
	mocker._pos_map[kFut]._long._dynprofit = 55.0;
	mocker.update_dyn_profit(kFut, NULL);
	EXPECT_DOUBLE_EQ(0.0, mocker._pos_map[kFut]._long._dynprofit);
	// 空头 volume()==0 分支
	mocker._pos_map[kFut]._short._dynprofit = 66.0;
	mocker.update_dyn_profit(kFut, NULL);
	EXPECT_DOUBLE_EQ(0.0, mocker._pos_map[kFut]._short._dynprofit);
	// 完全未注册过的合约：find 失败，整段跳过
	mocker.update_dyn_profit("DCE.m.2405", NULL);
}

// 日志与输出落盘：dump_outputs 生成四份 csv（只读表头，不依赖 dist/）
TEST_F(P08UftOrderPosition, DumpOutputsWritesFourCsvs)
{
	UftMocker mocker(&_rp, "p08_dump");
	mocker.on_trade(1, kFut, true, 0, 2, 3800.0);
	mocker.on_trade(2, kFut, true, 1, 1, 3810.0);
	mocker.log_trade(kFut, false, 0, 20240102093000000ULL, 3800.0, 1.0, 1.25);
	mocker.log_close(kFut, false, 1ULL, 3800.0, 2ULL, 3801.0, 1.0, 10.0, 20.0, -5.0, 10.0);

	// 以 WtHelper 自身的输出目录为基准拼路径，避免对尾部分隔符的假设
	std::filesystem::path dir = std::filesystem::path(WtHelper::getOutputDir()) / "p08_dump";
	mocker.dump_outputs();
	ASSERT_TRUE(std::filesystem::exists(dir / "trades.csv"));
	ASSERT_TRUE(std::filesystem::exists(dir / "closes.csv"));
	ASSERT_TRUE(std::filesystem::exists(dir / "funds.csv"));
	ASSERT_TRUE(std::filesystem::exists(dir / "positions.csv"));

	std::ifstream tf((dir / "trades.csv").string());
	std::stringstream ts; ts << tf.rdbuf();
	EXPECT_NE(std::string::npos, ts.str().find("code,time,direct,action,price,qty,fee,usertag"));
	EXPECT_NE(std::string::npos, ts.str().find("SHORT"));
	// log_trade 的 fee 以两位小数格式化
	EXPECT_NE(std::string::npos, ts.str().find("1.25"));

	std::ifstream cf((dir / "closes.csv").string());
	std::stringstream cs; cs << cf.rdbuf();
	EXPECT_NE(std::string::npos, cs.str().find("code,direct,opentime,openprice,closetime,closeprice"));
	EXPECT_NE(std::string::npos, cs.str().find("3801"));

	// funds.csv 只有表头：on_session_end 未被调用，_fund_logs 为空
	std::ifstream ff((dir / "funds.csv").string());
	std::stringstream fs; fs << ff.rdbuf();
	EXPECT_EQ("date,closeprofit,positionprofit,dynbalance,fee\n", fs.str());

	// positions.csv 在无持仓日志时只写表头
	std::ifstream pf((dir / "positions.csv").string());
	std::stringstream ps; ps << pf.rdbuf();
	EXPECT_EQ("date,code,direct,volume,closeprofit,dynprofit\n", ps.str());
}

// 未知合约的下单入口只依赖 commInfo==NULL 的早退分支；
// 但平仓/撤单/枚举持仓入口对 commInfo 与 _strategy 无空指针防护，
// 同一类输入（空策略解引用族）整体收进本 DISABLED 用例，避免整轮空转。
TEST_F(P08UftOrderPosition, DISABLED_NullStrategyAndNullCommDerefFamily)
{
	UftMocker mocker(&_rp, "p08_nulls");
	// (a) on_session_begin / on_session_end 末尾无条件 _strategy->xxx()
	mocker.on_session_begin(20240102);
	mocker.on_session_end(20240102);
	mocker.handle_session_begin(20240102);
	mocker.handle_session_end(20240102);
	// (b) stra_enum_position 无条件 _strategy->on_position()
	mocker.stra_enum_position(kFut);
	// (c) 平仓/撤单任务无条件 commInfo->getCoverMode()
	mocker.stra_exit_long("DCE.m.2405", 3000.0, 1);
	mocker.stra_exit_short("DCE.m.2405", 3000.0, 1);
	mocker._pos_map["DCE.m.2405"]._long._preavail = 1.0;
	mocker.stra_cancel(mocker.stra_enter_long(kFut, 3800.0, 1));
	mocker.procTask();
	// (d) 有持仓时用 NULL tick 做浮盈重算
	mocker.on_trade(1, kFut, true, 0, 2, 3800.0);
	mocker.update_dyn_profit(kFut, NULL);
}
