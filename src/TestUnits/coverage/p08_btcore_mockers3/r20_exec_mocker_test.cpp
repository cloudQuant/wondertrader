/**
 * r20_exec_mocker_test.cpp  (lane R20)
 *
 * 靶子: src/WtBtCore/ExecMocker.cpp（113/230 未覆盖，50.87%）
 *
 * 接缝事实（逐条读源码确认）：
 * - ExecMocker 全部数据成员 private（ExecMocker.h L264-325），禁止 #define private public，
 *   故 _exec_unit 无法注入桩件：handle_init / handle_schedule / handle_entrust /
 *   handle_order / handle_trade 五条解引用 _exec_unit（且 schedule/order/trade 还解引用
 *   _last_tick）的路径整条 DISABLED_ 登记留白。
 * - MatchEngine::handle_tick 对 _tick_cache 懒创建（MatchEngine.cpp L425-426），
 *   _orders 为空时 fire_orders/match_orders 不解引用 _sink →
 *   未 init 的 ExecMocker 可安全喂真实 WTSTickData::create tick（R2 实测的公开工厂）。
 * - 但一旦 _orders 非空（buy/sell 之后），下一个 handle_tick 会进 fire_orders
 *   `_sink->handle_entrust` → _sink==NULL 崩溃（regisSink 只在 init() 里做）。
 *   该「先有单再喂 tick 必崩」序列整条 DISABLED_。
 * - MatchEngine::cancel(localid) 只置 state=9 不 erase、也不判 state，重复撤单会
 *   二次扣减 _undone 并二次计数 _cacl_cnt —— 以启用用例固化现行为，缺陷另记。
 * - buy/sell 只依赖撮合器内部 tick 缓存，不查 comminfo；合约键仍按三段式喂
 *   （CodeHelper.hpp:601-616），SetUp 自检 getCommodityInfo 可判定注册与否。
 * - handle_replay_done 不碰 _exec_unit（L337-347）：output dir 由 WtHelper::setOutputDir
 *   指向唯一临时目录，_id 为空 → 产物为 <dir>/exec/trades_.csv（此时 _trade_logs 为空，
 *   handle_init 才会写表头，而 handle_init 崩溃不可达 → 期望文件 0 字节）。
 * 不起线程、不 sleep、不读挂钟（getCurTime 完全由 replayer 三字段推导）。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>

#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <Share/TimeUtils.hpp>
#include <WtBtCore/WtHelper.h>
#include <WtBtCore/ExecMocker.h>

namespace
{
	const char* const kCode = "SHFE.rb.2405";	// 三段式分月合约

	std::filesystem::path lane_dir()
	{
		auto path = std::filesystem::temp_directory_path() /
			("r20_exec_mocker_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::create_directories(path, ec);
		return path;
	}

	WTSTickData* make_exec_tick(const char* stdCode, double last, double bid1, double ask1,
		uint32_t bidQty, uint32_t askQty, uint32_t uDate, uint32_t uMsTime)
	{
		WTSTickStruct ts;
		memset(&ts, 0, sizeof(WTSTickStruct));
		strcpy(ts.code, stdCode);
		ts.action_date = uDate;
		ts.action_time = uMsTime;
		ts.trading_date = uDate;
		ts.price = last;
		ts.pre_close = last - 5.0;
		ts.open = last - 1.0;
		ts.high = last + 1.0;
		ts.low = last - 2.0;
		ts.volume = 100;
		ts.total_volume = 1000;
		ts.turn_over = last * 100;
		ts.total_turnover = last * 1000;
		for (int32_t i = 0; i < 10; i++)
		{
			ts.bid_prices[i] = bid1 - i;
			ts.ask_prices[i] = ask1 + i;
			ts.bid_qty[i] = bidQty;
			ts.ask_qty[i] = askQty;
		}
		return WTSTickData::create(ts);
	}

	WTSVariant* exec_cfg()
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("module", "r20_no_such_executer");
		cfg->append("code", kCode);
		cfg->append("period", "m1");
		cfg->append("volunit", 1.0);
		cfg->append("volmode", static_cast<int32_t>(0));
		return cfg;
	}
}

class R20ExecMocker : public ::testing::Test
{
protected:
	HisDataReplayer _rp;
	std::filesystem::path _dir;
	void SetUp() override
	{
		_dir = lane_dir();
		WtHelper::setOutputDir(_dir.string().c_str());
		// 夹具自检：本 lane 不注册品种，三段式键查 comminfo 必须为 NULL（未污染基础表）
		EXPECT_TRUE(_rp.get_commodity_info(kCode) == NULL);
	}
	void TearDown() override
	{
		WtHelper::setOutputDir("");
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}
};

// 构造后零值面：持仓/挂单为 0，订单表 NULL，未注册合约的 comminfo/session 均 NULL
TEST_F(R20ExecMocker, DefaultsAfterCtor)
{
	ExecMocker m(&_rp);
	EXPECT_TRUE(m.getOrders(kCode) == NULL);
	EXPECT_DOUBLE_EQ(0.0, m.getPosition(kCode));
	EXPECT_DOUBLE_EQ(0.0, m.getPosition(kCode, true, 1));
	EXPECT_DOUBLE_EQ(0.0, m.getUndoneQty(kCode));
	EXPECT_TRUE(m.getCommodityInfo(kCode) == NULL);
	EXPECT_TRUE(m.getSessionInfo(kCode) == NULL);
}

// getCurTime 完全由 replayer 三字段算得（默认构造全 0 → makeTime(0,0)，不读挂钟）
TEST_F(R20ExecMocker, CurTimeDerivedFromReplayerClock)
{
	ExecMocker m(&_rp);
	const uint64_t expect = TimeUtils::makeTime(
		_rp.get_date(), _rp.get_raw_time() * 100000 + _rp.get_secs());
	EXPECT_EQ(m.getCurTime(), expect);
}

// tick 换入换出：第二次 handle_tick 先 release 旧 tick（L124-128），无订单时全链安全
TEST_F(R20ExecMocker, TickSwapReleasesPreviousTick)
{
	ExecMocker m(&_rp);
	WTSTickData* t1 = make_exec_tick(kCode, 3800.0, 3799.0, 3801.0, 5, 7, 20240103, 142500000);
	m.handle_tick(kCode, t1, 0);
	WTSTickData* t2 = make_exec_tick(kCode, 3810.0, 3809.0, 3811.0, 6, 4, 20240103, 142503000);
	m.handle_tick(kCode, t2, 0);
	// 无成交事件 → 持仓/挂单不变
	EXPECT_DOUBLE_EQ(0.0, m.getPosition(kCode));
	EXPECT_DOUBLE_EQ(0.0, m.getUndoneQty(kCode));
	// 析构会 release _last_tick（t2），t1 已由第二次 handle_tick release，同池回收合法
}

// 撮合器无 tick 缓存时 buy/sell 早退（L224/L241 false 臂），计数器不动
TEST_F(R20ExecMocker, BuySellRefusedBeforeAnyTick)
{
	ExecMocker m(&_rp);
	EXPECT_TRUE(m.buy(kCode, 3801.0, 3).empty());
	EXPECT_TRUE(m.sell(kCode, 3799.0, 2).empty());
	EXPECT_DOUBLE_EQ(0.0, m.getUndoneQty(kCode));
	// 撤未知 localid：matcher 查无此单 → change==0 → false（L255-257）
	EXPECT_FALSE(m.cancel(999999u));
}

// 有 tick 后 buy 挂单成功：_ord_cnt/_undone 更新（L226-229），undone 取整数量
TEST_F(R20ExecMocker, BuyAfterTickAccumulatesUndone)
{
	ExecMocker m(&_rp);
	WTSTickData* t1 = make_exec_tick(kCode, 3800.0, 3799.0, 3801.0, 5, 7, 20240103, 142500000);
	m.handle_tick(kCode, t1, 0);
	OrderIDs ret = m.buy(kCode, 3801.0, 3);	// 对手价 → positive 排队臂
	ASSERT_EQ(1u, ret.size());
	EXPECT_DOUBLE_EQ(3.0, m.getUndoneQty(kCode));
	EXPECT_DOUBLE_EQ(0.0, m.getPosition(kCode));
	// 清场避免后续 tick 触发 fire_orders 的 _sink 空指针（见 DISABLED 说明）
	m.handle_session_end(20240103);
}

// sell 方向 undone 取负（L246），与 buy 混合累加
TEST_F(R20ExecMocker, SellDecrementsUndone)
{
	ExecMocker m(&_rp);
	WTSTickData* t1 = make_exec_tick(kCode, 3800.0, 3799.0, 3801.0, 5, 7, 20240103, 142500000);
	m.handle_tick(kCode, t1, 0);
	OrderIDs rs = m.sell(kCode, 3799.0, 5);	// 买一价 → 非 positive、bidqty 排队臂
	ASSERT_EQ(1u, rs.size());
	EXPECT_DOUBLE_EQ(-5.0, m.getUndoneQty(kCode));
	OrderIDs rb = m.buy(kCode, 3800.0, 2);	// 最新价 → 加权排队臂（分母 3799*5+3801*7 非零）
	ASSERT_EQ(1u, rb.size());
	EXPECT_DOUBLE_EQ(-3.0, m.getUndoneQty(kCode));
	m.handle_session_end(20240103);
}

// 按 localid 撤单：state=9、_undone 归还、撤单计数更新；重复撤单不判 state，
// 现行为是二次归还（_undone 变负、_cacl_cnt 双计）——缺陷已记 bugs/R20_wtbtcore.md，
// 本用例按源码实际行为固化，非期望语义。
TEST_F(R20ExecMocker, CancelByIdRestoresUndoneAndDoubleCancelQuirk)
{
	ExecMocker m(&_rp);
	WTSTickData* t1 = make_exec_tick(kCode, 3800.0, 3799.0, 3801.0, 5, 7, 20240103, 142500000);
	m.handle_tick(kCode, t1, 0);
	OrderIDs rb = m.buy(kCode, 3801.0, 3);
	ASSERT_EQ(1u, rb.size());
	EXPECT_DOUBLE_EQ(3.0, m.getUndoneQty(kCode));

	EXPECT_TRUE(m.cancel(rb[0]));			// change=+3 → undone 0（L259-264）
	EXPECT_DOUBLE_EQ(0.0, m.getUndoneQty(kCode));

	EXPECT_TRUE(m.cancel(rb[0]));			// 同一单再撤：仍返回 left=3
	EXPECT_DOUBLE_EQ(-3.0, m.getUndoneQty(kCode));

	m.handle_session_end(20240103);			// 清 _orders，之后撤单查无
	EXPECT_FALSE(m.cancel(rb[0]));
}

// 按合约+方向批量撤单：未激活(state!=1)的单被 continue 跳过 → 返回空、_undone 不变
TEST_F(R20ExecMocker, CancelByCodeSkipsUnfiredOrders)
{
	ExecMocker m(&_rp);
	WTSTickData* t1 = make_exec_tick(kCode, 3800.0, 3799.0, 3801.0, 5, 7, 20240103, 142500000);
	m.handle_tick(kCode, t1, 0);
	OrderIDs rb = m.buy(kCode, 3801.0, 4);
	ASSERT_EQ(1u, rb.size());
	// fire_orders 未跑（无后续 tick），state 仍 0 → 全部 continue
	OrderIDs cancelled = m.cancel(kCode, true, 0);
	EXPECT_TRUE(cancelled.empty());
	EXPECT_DOUBLE_EQ(4.0, m.getUndoneQty(kCode));
	// 空订单表时撤未知合约同样空返回（循环不进入，回调不触发）
	m.handle_session_end(20240103);
	OrderIDs none = m.cancel("SZSE.000001", false, 0);
	EXPECT_TRUE(none.empty());
}

// 会话收尾：_matcher.clear() 清全部订单、_undone 归零、统计日志（L113-120）
TEST_F(R20ExecMocker, SessionEndClearsMatcherAndUndone)
{
	ExecMocker m(&_rp);
	WTSTickData* t1 = make_exec_tick(kCode, 3800.0, 3799.0, 3801.0, 5, 7, 20240103, 142500000);
	m.handle_tick(kCode, t1, 0);
	OrderIDs rb = m.buy(kCode, 3801.0, 2);
	ASSERT_EQ(1u, rb.size());
	EXPECT_DOUBLE_EQ(2.0, m.getUndoneQty(kCode));

	m.handle_session_end(20240103);
	EXPECT_DOUBLE_EQ(0.0, m.getUndoneQty(kCode));
	EXPECT_FALSE(m.cancel(rb[0]));			// _orders 已清空
	// 重复收尾安全（clear 空表、undone 保持 0）
	m.handle_session_end(20240104);
	EXPECT_DOUBLE_EQ(0.0, m.getUndoneQty(kCode));
}

// 空实现事件与日志面：handle_bar_close/handle_session_begin 为注释掉的空体，
// writeLog 走 log_dyn_raw("executer", 空id, LL_INFO)
TEST_F(R20ExecMocker, NoopEventsAndWriteLogAreSideEffectFree)
{
	ExecMocker m(&_rp);
	m.handle_bar_close(kCode, "m1", 1, NULL);
	m.handle_session_begin(20240103);
	m.writeLog("r20 executer log line");
	EXPECT_DOUBLE_EQ(0.0, m.getPosition(kCode));
	EXPECT_DOUBLE_EQ(0.0, m.getUndoneQty(kCode));
	EXPECT_TRUE(m.getOrders(kCode) == NULL);
}

// init：MatchEngine::init(NULL 子配置) 安全返回 + 缺失模块 load_library==NULL → false（L51-62）
TEST_F(R20ExecMocker, InitFailsFastWhenModuleMissing)
{
	ExecMocker m(&_rp);
	WTSVariant* cfg = exec_cfg();
	EXPECT_FALSE(m.init(cfg));
	// 早退发生在 _factory 装配之前；查询面保持默认值
	EXPECT_DOUBLE_EQ(0.0, m.getPosition(kCode));
	EXPECT_DOUBLE_EQ(0.0, m.getUndoneQty(kCode));
	EXPECT_TRUE(m.getOrders(kCode) == NULL);
}

// 回放收尾落盘：exec/trades_.csv（_id 为空串），_trade_logs 未写表头 → 0 字节
TEST_F(R20ExecMocker, ReplayDoneWritesEmptyTradeCsv)
{
	ExecMocker m(&_rp);
	m.handle_replay_done();
	const std::filesystem::path csv = _dir / "exec" / "trades_.csv";
	ASSERT_TRUE(std::filesystem::exists(csv));
	std::ifstream in(csv, std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	EXPECT_TRUE(ss.str().empty());
	// 幂等：重复调用重写同内容
	m.handle_replay_done();
	std::ifstream in2(csv, std::ios::binary);
	std::stringstream ss2;
	ss2 << in2.rdbuf();
	EXPECT_TRUE(ss2.str().empty());
}

// getTicks 委托：replayer 公共开关关闭 tick 后，get_tick_slice 第一守卫必返 NULL（确定性）
TEST_F(R20ExecMocker, GetTicksNullWhenTickFeedDisabled)
{
	_rp.enable_tick(false);
	ExecMocker m(&_rp);
	EXPECT_TRUE(m.getTicks(kCode, 5) == NULL);
	EXPECT_TRUE(m.getTicks(kCode, 1, 20240103142500000ull) == NULL);
	_rp.enable_tick(true);
	// grabLastTick 未测：get_last_tick→checkTicks 在未 init replayer 下走 bin 文件臂，
	// 本 lane 无 loader 夹具，登记留白（R2 的 loader 臂接缝见 manifests/R2_wtbtcore.md）。
}

// DISABLED_: _exec_unit NULL 解引用族。触发条件：任何未成功 init()（模块缺失/未调用）的
// ExecMocker 收到以下公开事件：
// - handle_init()：L155 `_exec_unit->on_channel_ready()` 崩溃（且 L151 先调 replayer sub_tick）
// - handle_schedule(d,t)：L168 `_last_tick->price()` 无 tick 时先崩；有 tick 时 L189 再崩
// - handle_entrust/handle_order/handle_trade：L287/L311/L334 `_exec_unit->on_*` 崩溃
// 无公开接缝可注入 ExecuteUnit*（ExecMocker.h 全 private + 禁 define private public）。
TEST_F(R20ExecMocker, DISABLED_ExecUnitNullDerefFamily)
{
	GTEST_SKIP() << "handle_init/handle_schedule/handle_entrust/handle_order/handle_trade deref NULL _exec_unit";
}

// DISABLED_: _sink NULL 的 fire_orders 崩溃。触发条件：未调用 init()（regisSink 只在
// ExecMocker::init L57 执行）时，先 buy/sell 产生 state0 订单，再喂第二个 handle_tick →
// MatchEngine::fire_orders 对 NULL _sink 调 handle_entrust（MatchEngine.cpp fire_orders
// 体内无 _sink 判空）。init() 失败（模块缺失）时同样已 regisSink，崩溃点后移到
// ExecMocker::handle_entrust 的 _exec_unit。
TEST_F(R20ExecMocker, DISABLED_FireOrdersSinkNullAfterPendingOrderAndSecondTick)
{
	GTEST_SKIP() << "tick -> buy -> tick derefs NULL MatchEngine::_sink in fire_orders";
}
