/**
 * n04_exec_mocker_test.cpp
 * 覆盖 src/WtBtCore/ExecMocker.{h,cpp} 中与执行器 DLL 无关的纯状态逻辑：
 * 构造/析构默认值、init 的参数解析与模块装载失败分支、ExecuteContext 查询接口
 * (getPosition/getOrders/getUndoneQty/grabLastTick/getTicks/getCommodityInfo/
 *  getSessionInfo/getCurTime)、buy/sell/cancel 的委托计数与未完成量回写、
 * handle_session_end/handle_bar_close/handle_session_begin/writeLog。
 *
 * 未覆盖（依赖 createExecUnit 生成的 _exec_unit，见 DISABLED_ 用例说明）：
 * handle_init/handle_schedule/handle_trade/handle_order/handle_entrust、
 * handle_replay_done 的落盘分支。
 */
#include <gtest/gtest.h>

#include <cfloat>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/ExecMocker.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSTradeDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <Share/TimeUtils.hpp>

USING_NS_WTP;

namespace {
const char* kStdCode = "SSE.600000";

WTSTickData* makeTick(const char* code, double price, double vol,
	double bidpx, double bidqty, double askpx, double askqty)
{
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	strcpy(ts.exchg, "SSE");
	strcpy(ts.code, code);
	ts.trading_date = 20240102;
	ts.action_date = 20240102;
	ts.action_time = 93000000;
	ts.price = price;
	ts.open = price - 0.05;
	ts.high = price + 0.1;
	ts.low = price - 0.1;
	ts.total_volume = vol;
	ts.volume = vol;
	ts.pre_close = price - 0.02;
	ts.upper_limit = price * 1.1;
	ts.lower_limit = price * 0.9;
	if (bidpx > 0.0)
	{
		ts.bid_prices[0] = bidpx;
		ts.bid_qty[0] = bidqty;
	}
	if (askpx > 0.0)
	{
		ts.ask_prices[0] = askpx;
		ts.ask_qty[0] = askqty;
	}
	return WTSTickData::create(ts);
}
}

class ExecMockerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		exec = new ExecMocker(&replayer);
	}

	void TearDown() override
	{
		delete exec;
		exec = nullptr;
	}

	// 仅投递一笔被动委托，不撮合（撮合需要后续 tick，会走到未初始化的 _exec_unit）
	void feedTick(WTSTickData* tick)
	{
		exec->handle_tick(kStdCode, tick, 0);
		tick->release();
	}

	void expectEmptyOrders(const char* code)
	{
		OrderMap* orders = exec->getOrders(code);
		ASSERT_NE(nullptr, orders);
		EXPECT_EQ(0u, orders->size());
		orders->release();
	}

	WTSTickData* newBook()
	{
		return makeTick(kStdCode, 10.0, 1000, 10.0, 500, 10.1, 600);
	}

	static WTSVariant* baseCfg()
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("code", kStdCode);
		cfg->append("period", "m1");
		cfg->append("volunit", 10.0);
		cfg->append("volmode", (int32_t)0);
		return cfg;
	}

	HisDataReplayer replayer;
	ExecMocker* exec;
};

TEST_F(ExecMockerTest, freshUnitHasFlatAccountState)
{
	EXPECT_DOUBLE_EQ(0.0, exec->getPosition(kStdCode));
	EXPECT_DOUBLE_EQ(0.0, exec->getPosition(kStdCode, false, 1));
	EXPECT_DOUBLE_EQ(0.0, exec->getPosition(kStdCode, true, 2));
	EXPECT_DOUBLE_EQ(0.0, exec->getUndoneQty(kStdCode));
	expectEmptyOrders(kStdCode);
	EXPECT_EQ(nullptr, exec->grabLastTick(kStdCode));
}

TEST_F(ExecMockerTest, dataSinkQueriesFallThroughToEmptyReplayer)
{
	EXPECT_EQ(nullptr, exec->getCommodityInfo("SSE.999999"));
	EXPECT_EQ(nullptr, exec->getSessionInfo("SSE.999999"));

	WTSTickSlice* s1 = exec->getTicks(kStdCode, 3);
	WTSTickSlice* s2 = exec->getTicks(kStdCode, 3, 20240102093000000ULL);
	// 只断言空/非空的一致性，不假定回放器内部缓存策略
	EXPECT_EQ((s1 == nullptr), (s2 == nullptr));
	if (s1) s1->release();
	if (s2) s2->release();

	// 回放器时钟为 0 时，getCurTime 必须与撮合时间公式一致且稳定
	uint64_t expect = TimeUtils::makeTime(replayer.get_date(),
		replayer.get_raw_time() * 100000 + replayer.get_secs());
	EXPECT_EQ(expect, exec->getCurTime());
	EXPECT_EQ(exec->getCurTime(), exec->getCurTime());
}

TEST_F(ExecMockerTest, initParsesConfigAndFailsWhenModuleMissing)
{
	WTSVariant* empty = WTSVariant::createObject();
	EXPECT_FALSE(exec->init(empty));
	empty->release();

	// 模块解析成功但装载失败 -> 直接返回 false，配置字段仍被读取
	WTSVariant* cfg = baseCfg();
	cfg->append("module", "/nonexistent_n04_dir/wt_fake_exec.so");
	EXPECT_FALSE(exec->init(cfg));
	cfg->release();

	// init 失败不得改变账户状态
	EXPECT_DOUBLE_EQ(0.0, exec->getPosition(kStdCode));
	EXPECT_DOUBLE_EQ(0.0, exec->getUndoneQty(kStdCode));
	expectEmptyOrders(kStdCode);
}

TEST_F(ExecMockerTest, buySellWithoutCachedTickCreateNoOrders)
{
	OrderIDs buy = exec->buy(kStdCode, 10.0, 100);
	OrderIDs sell = exec->sell(kStdCode, 10.1, 50);
	EXPECT_TRUE(buy.empty());
	EXPECT_TRUE(sell.empty());
	EXPECT_DOUBLE_EQ(0.0, exec->getUndoneQty(kStdCode));
	// 计数未被污染的旁证：撤单同样无事可做
	EXPECT_FALSE(exec->cancel(99999u));
}

TEST_F(ExecMockerTest, passiveBuyRegistersOrderAndTracksUndoneQty)
{
	feedTick(newBook());

	OrderIDs ids = exec->buy(kStdCode, 10.0, 100);
	ASSERT_EQ(1u, ids.size());
	EXPECT_DOUBLE_EQ(100.0, exec->getUndoneQty(kStdCode));
	// 尚未撮合，持仓不应变化
	EXPECT_DOUBLE_EQ(0.0, exec->getPosition(kStdCode));

	EXPECT_TRUE(exec->cancel(ids[0]));
	EXPECT_DOUBLE_EQ(0.0, exec->getUndoneQty(kStdCode));
	// state 9 remains pending until a later tick; a duplicate request must not debit twice.
	EXPECT_FALSE(exec->cancel(ids[0]));
	EXPECT_DOUBLE_EQ(0.0, exec->getUndoneQty(kStdCode));
}

TEST_F(ExecMockerTest, passiveSellUsesNegativeUndoneSign)
{
	feedTick(newBook());

	OrderIDs ids = exec->sell(kStdCode, 10.1, 60);
	ASSERT_EQ(1u, ids.size());
	EXPECT_DOUBLE_EQ(-60.0, exec->getUndoneQty(kStdCode));

	// 按方向批量撤单：卖出方向无挂单，不应改变未完成量
	OrderIDs none = exec->cancel(kStdCode, true, 0);
	EXPECT_TRUE(none.empty());
	EXPECT_DOUBLE_EQ(-60.0, exec->getUndoneQty(kStdCode));

	OrderIDs sold = exec->cancel(kStdCode, false, 0);
	EXPECT_EQ(1u, sold.size());
	EXPECT_DOUBLE_EQ(0.0, exec->getUndoneQty(kStdCode));
}

TEST_F(ExecMockerTest, getOrdersReturnsIndependentGrossSnapshotsForOpposingOrders)
{
	feedTick(newBook());
	OrderIDs buys = exec->buy(kStdCode, 10.0, 100);
	OrderIDs sells = exec->sell(kStdCode, 10.1, 100);
	ASSERT_EQ(1u, buys.size());
	ASSERT_EQ(1u, sells.size());
	EXPECT_DOUBLE_EQ(0.0, exec->getUndoneQty(kStdCode));

	OrderMap* beforeCancel = exec->getOrders(kStdCode);
	ASSERT_NE(nullptr, beforeCancel);
	ASSERT_EQ(2u, beforeCancel->size());
	WTSOrderInfo* buy = static_cast<WTSOrderInfo*>(beforeCancel->get(buys.front()));
	WTSOrderInfo* sell = static_cast<WTSOrderInfo*>(beforeCancel->get(sells.front()));
	ASSERT_NE(nullptr, buy);
	ASSERT_NE(nullptr, sell);
	EXPECT_STREQ(kStdCode, buy->getCode());
	EXPECT_EQ(buys.front(), static_cast<uint32_t>(std::strtoul(buy->getOrderID(), nullptr, 10)));
	EXPECT_EQ(WDT_LONG, buy->getDirection());
	EXPECT_TRUE(buy->isBuyAction());
	EXPECT_DOUBLE_EQ(100.0, buy->getVolume());
	EXPECT_DOUBLE_EQ(100.0, buy->getVolLeft());
	EXPECT_DOUBLE_EQ(0.0, buy->getVolTraded());
	EXPECT_DOUBLE_EQ(10.0, buy->getPrice());
	EXPECT_EQ(WOS_Submitting, buy->getOrderState());
	EXPECT_EQ(sells.front(), static_cast<uint32_t>(std::strtoul(sell->getOrderID(), nullptr, 10)));
	EXPECT_EQ(WDT_SHORT, sell->getDirection());
	EXPECT_FALSE(sell->isBuyAction());
	EXPECT_DOUBLE_EQ(100.0, sell->getVolume());
	EXPECT_DOUBLE_EQ(100.0, sell->getVolLeft());

	// Cancellation remains pending until a future tick acknowledges it. The
	// already-returned map stays detached from subsequent matcher mutations.
	EXPECT_TRUE(exec->cancel(buys.front()));
	EXPECT_EQ(WOS_Submitting, buy->getOrderState());
	OrderMap* afterCancel = exec->getOrders(kStdCode);
	ASSERT_NE(nullptr, afterCancel);
	ASSERT_EQ(2u, afterCancel->size());
	WTSOrderInfo* cancelPending = static_cast<WTSOrderInfo*>(afterCancel->get(buys.front()));
	ASSERT_NE(nullptr, cancelPending);
	EXPECT_EQ(WOS_Cancelling, cancelPending->getOrderState());
	EXPECT_DOUBLE_EQ(100.0, cancelPending->getVolLeft());

	beforeCancel->release();
	afterCancel->release();
}

TEST_F(ExecMockerTest, getOrdersInitializesEveryFieldAndClearsReusedPoolStorage)
{
	feedTick(newBook());
	OrderIDs firstIds = exec->buy(kStdCode, 10.1, 100);
	ASSERT_EQ(1u, firstIds.size());
	OrderMap* firstMap = exec->getOrders(kStdCode);
	ASSERT_NE(nullptr, firstMap);
	WTSOrderInfo* first = static_cast<WTSOrderInfo*>(firstMap->get(firstIds.front()));
	ASSERT_NE(nullptr, first);
	const uintptr_t pooledAddress = reinterpret_cast<uintptr_t>(first);
	EXPECT_STREQ("SSE", first->getExchg());
	EXPECT_STREQ(kStdCode, first->getCode());
	EXPECT_EQ(WDT_LONG, first->getDirection());
	EXPECT_EQ(WPT_LIMITPRICE, first->getPriceType());
	EXPECT_EQ(WOF_NOR, first->getOrderFlag());
	EXPECT_EQ(WOT_OPEN, first->getOffsetType());
	EXPECT_EQ(0u, first->getOffsetFlag());
	EXPECT_TRUE(first->isBuyAction());
	EXPECT_EQ(BT_CASH, first->getBusinessType());
	EXPECT_DOUBLE_EQ(100.0, first->getVolume());
	EXPECT_DOUBLE_EQ(10.1, first->getPrice());
	EXPECT_STREQ("", first->getEntrustID());
	EXPECT_STREQ("", first->getUserTag());
	EXPECT_FALSE(first->isNet());
	EXPECT_TRUE(first->isNetBuy());
	EXPECT_EQ(nullptr, first->getContractInfo());
	EXPECT_EQ(nullptr, first->getExtras());
	EXPECT_EQ(0u, first->getOrderDate());
	EXPECT_EQ(0u, first->getOrderTime());
	EXPECT_DOUBLE_EQ(0.0, first->getVolTraded());
	EXPECT_DOUBLE_EQ(100.0, first->getVolLeft());
	EXPECT_EQ(WOS_Submitting, first->getOrderState());
	EXPECT_EQ(WORT_Normal, first->getOrderType());
	EXPECT_STREQ(std::to_string(firstIds.front()).c_str(), first->getOrderID());
	EXPECT_STREQ("", first->getStateMsg());
	EXPECT_TRUE(first->isAlive());
	EXPECT_FALSE(first->isError());
	EXPECT_FALSE(first->isSelfTrade());
	firstMap->release();

	// Return the first snapshot to WTSOrderInfo's object pool, then request an
	// order with a code lacking an exchange prefix so stale pooled data is visible.
	exec->handle_session_end(20240102);
	const char* rawCode = "600001";
	WTSTickData* rawTick = makeTick(rawCode, 10.0, 0, 9.9, 500, 10.1, 600);
	exec->handle_tick(rawCode, rawTick, 0);
	rawTick->release();
	OrderIDs secondIds = exec->sell(rawCode, 9.9, 75);
	ASSERT_EQ(1u, secondIds.size());
	OrderMap* secondMap = exec->getOrders(rawCode);
	ASSERT_NE(nullptr, secondMap);
	WTSOrderInfo* second = static_cast<WTSOrderInfo*>(secondMap->get(secondIds.front()));
	ASSERT_NE(nullptr, second);
	EXPECT_EQ(pooledAddress, reinterpret_cast<uintptr_t>(second));
	EXPECT_STREQ("", second->getExchg());
	EXPECT_STREQ(rawCode, second->getCode());
	EXPECT_EQ(WDT_SHORT, second->getDirection());
	EXPECT_EQ(WPT_LIMITPRICE, second->getPriceType());
	EXPECT_EQ(WOF_NOR, second->getOrderFlag());
	EXPECT_EQ(WOT_OPEN, second->getOffsetType());
	EXPECT_EQ(0u, second->getOffsetFlag());
	EXPECT_FALSE(second->isBuyAction());
	EXPECT_EQ(BT_CASH, second->getBusinessType());
	EXPECT_DOUBLE_EQ(75.0, second->getVolume());
	EXPECT_DOUBLE_EQ(9.9, second->getPrice());
	EXPECT_STREQ("", second->getEntrustID());
	EXPECT_STREQ("", second->getUserTag());
	EXPECT_FALSE(second->isNet());
	EXPECT_FALSE(second->isNetBuy());
	EXPECT_EQ(nullptr, second->getContractInfo());
	EXPECT_EQ(nullptr, second->getExtras());
	EXPECT_EQ(0u, second->getOrderDate());
	EXPECT_EQ(0u, second->getOrderTime());
	EXPECT_DOUBLE_EQ(0.0, second->getVolTraded());
	EXPECT_DOUBLE_EQ(75.0, second->getVolLeft());
	EXPECT_EQ(WOS_Submitting, second->getOrderState());
	EXPECT_EQ(WORT_Normal, second->getOrderType());
	EXPECT_STREQ(std::to_string(secondIds.front()).c_str(), second->getOrderID());
	EXPECT_STREQ("", second->getStateMsg());
	EXPECT_TRUE(second->isAlive());
	EXPECT_FALSE(second->isError());
	EXPECT_FALSE(second->isSelfTrade());
	secondMap->release();
}

TEST_F(ExecMockerTest, sessionEndClearsMatcherAndUndone)
{
	feedTick(newBook());
	OrderIDs ids = exec->buy(kStdCode, 10.0, 80);
	ASSERT_EQ(1u, ids.size());
	EXPECT_DOUBLE_EQ(80.0, exec->getUndoneQty(kStdCode));

	exec->handle_session_end(20240102);
	EXPECT_DOUBLE_EQ(0.0, exec->getUndoneQty(kStdCode));
	// 撮合引擎已清空
	EXPECT_FALSE(exec->cancel(ids[0]));
	EXPECT_DOUBLE_EQ(0.0, exec->getPosition(kStdCode));
}

TEST_F(ExecMockerTest, tickHandlingReplacesLastTickWithoutOrders)
{
	feedTick(newBook());
	feedTick(makeTick(kStdCode, 10.5, 2000, 10.5, 300, 10.6, 400));
	// 没有挂单时 tick 不应产生任何账户变化
	EXPECT_DOUBLE_EQ(0.0, exec->getUndoneQty(kStdCode));
	EXPECT_DOUBLE_EQ(0.0, exec->getPosition(kStdCode));
}

TEST_F(ExecMockerTest, idleCallbacksAndWriteLogAreNoop)
{
	exec->handle_bar_close(kStdCode, "m1", 1, nullptr);
	exec->handle_session_begin(20240102);
	exec->writeLog("n04 exec mocker log probe");
	EXPECT_DOUBLE_EQ(0.0, exec->getPosition(kStdCode));
}

/**
 * 缺陷类用例（禁用，见 docs/迭代计划/迭代01/bugs/n04_btcore_mockers.md BUG-N04-001）：
 * ExecMocker 的 IMatchSink 回调与 handle_init/handle_schedule 直接解引用
 * _exec_unit / _last_tick，而这两个字段只有在执行器 DLL 装载成功后才被赋值。
 * 整个缺陷类（而不是单个例子）统一放在本禁用用例中：
 *  - handle_trade / handle_order / handle_entrust：_exec_unit 为 NULL
 *  - handle_init：_period 为空串 + _exec_unit 为 NULL
 *  - handle_schedule：_last_tick 为 NULL + _exec_unit 为 NULL
 *  - init(NULL)：直接解引用空配置指针
 */
TEST_F(ExecMockerTest, DISABLED_matchSinkCallbacksCrashWithoutExecuterUnit)
{
	exec->handle_entrust(1, kStdCode, true, "ok", 20240102093000000ULL);
	exec->handle_order(1, kStdCode, true, 100, 10.0, true, 20240102093000000ULL);
	exec->handle_trade(1, kStdCode, true, 100, 10.0, 10.0, 20240102093000000ULL);
	exec->handle_init();
	exec->handle_schedule(20240102, 931);
	exec->init(nullptr);
}
