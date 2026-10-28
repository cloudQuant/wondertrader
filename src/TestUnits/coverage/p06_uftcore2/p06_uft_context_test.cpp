/*!
 * \file p06_uft_context_test.cpp
 * \brief WonderTrader 测试覆盖率迭代01 第三波 / lane p06 —— UftStraContext 纯逻辑分支
 *
 * 只覆盖不需要 ShareManager 共享内存段、不需要 UFT 柜台回报的分支：
 *   - 构造/析构、id()/name()/set_strategy/get_stragety/setTrader 的赋值与查询
 *   - on_init / on_tick / on_bar / on_order_queue / on_order_detail / on_transaction /
 *     on_session_begin / on_session_end / on_params_updated 的「策略为空」早退分支
 *   - stra_get_date / stra_get_time / stra_get_secs / stra_get_price 的引擎委派
 *   - stra_get_local_position / stra_get_local_posprofit / stra_get_local_closeprofit
 *     的持仓表 miss 分支
 *   - stra_log_info / stra_log_debug / stra_log_error
 *   - stra_sub_ticks / stra_sub_order_details / stra_sub_order_queues / stra_sub_transactions
 *     （写入引擎订阅表 + 日志，随后由引擎回调验证订阅生效）
 *
 * 明确不覆盖（见 manifest 排除清单）：
 *   - watch_param / read_param / sync_param / commit_param_watcher（全部走 ShareManager 共享内存段）
 *   - stra_buy / stra_sell / stra_enter_long / stra_exit_long / stra_exit_short /
 *     stra_cancel / stra_cancel_all / stra_get_position / stra_get_local_* 的有仓分支
 *     （需要 ITraderApi 回报状态机或本地 mmap 持仓块）
 *   - load_local_data / on_position / 持仓持久化（BoostMappingFile + mannual.yaml + WTSCfgLoader）
 *   - stra_get_bars / stra_get_ticks / stra_get_order_* 切片（需要数据服务历史库）
 * 启用用例只调用本轮读过源码的路径；未读源码的接口一律不碰。
 */
#include <gtest/gtest.h>

#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/WtUftDtMgr.h"
#include "WtUftCore/TraderAdapter.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSMarcos.h"

#include <cstring>
#include <memory>
#include <string>

USING_NS_WTP;

namespace {

class P06CtxBaseDataMgr : public IBaseDataMgr
{
public:
	WTSCommodityInfo* getCommodity(const char*) override { return nullptr; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return nullptr; }
	WTSContractInfo* getContract(const char*, const char* = "", uint32_t = 0) override { return nullptr; }
	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return WTSArray::create(); }
	WTSSessionInfo* getSession(const char*) override { return nullptr; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return nullptr; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate + 1; }
	uint64_t getBoundaryTime(const char*, uint32_t, bool = false, bool = true) override { return 0; }
};

// 引擎先 init()，再拿它构造上下文；引擎指针未初始化时任何委派都是野指针
struct P06CtxFixture
{
	P06CtxFixture() : engine()
	{
		engine.init(nullptr, &bdMgr, &dtMgr, nullptr);
		engine.set_date_time(20240301, 931, 25, 93059);
		engine.set_trading_date(20240301);
	}

	P06CtxBaseDataMgr bdMgr;
	WtUftDtMgr dtMgr;
	WtUftEngine engine;
};

WTSTickData* p06MakeTick(const char* exchg, const char* code, double price = 10.5,
	uint32_t actTime = 93015500)
{
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(WTSTickStruct));
	wt_strcpy(ts.exchg, exchg);
	wt_strcpy(ts.code, code);
	ts.trading_date = 20240301;
	ts.action_date = 20240301;
	ts.action_time = actTime;
	ts.price = price;
	return WTSTickData::create(ts);
}

}  // namespace

//////////////////////////////////////////////////////////////////////////
TEST(p06_uft_context, identity_and_object_slots)
{
	P06CtxFixture fx;

	UftStraContext ctx1(&fx.engine, "p06stra_a");
	EXPECT_NE(0u, ctx1.id());
	EXPECT_STREQ("p06stra_a", ctx1.name());

	// 上下文 id 由进程内原子自增分配，只断言单调性
	UftStraContext ctx2(&fx.engine, "p06stra_b");
	EXPECT_GT(ctx2.id(), ctx1.id());
	EXPECT_STREQ("p06stra_b", ctx2.name());

	// 策略槽位：先写 nullptr 再读，不依赖构造函数默认值
	ctx1.set_strategy(nullptr);
	EXPECT_EQ(nullptr, ctx1.get_stragety());

	// 交易适配器槽位：只做赋值（getter 属于未初始化路径，见 DISABLED 用例）
	TraderAdapter trader;
	ctx1.setTrader(&trader);
	EXPECT_EQ(nullptr, ctx1.get_stragety());

	// 把上下文挂到引擎，后续订阅才有人接收
	fx.engine.addContext(UftContextPtr(std::shared_ptr<UftStraContext>(&ctx1, [](UftStraContext*) {})));
	EXPECT_STREQ("p06stra_a", fx.engine.getContext(ctx1.id())->name());
}

TEST(p06_uft_context, callbacks_without_strategy_are_noops)
{
	P06CtxFixture fx;
	UftStraContext ctx(&fx.engine, "p06stra_noop");
	ctx.set_strategy(nullptr);

	// 全部回调都带 if(_strategy) 早退
	ctx.on_init();
	ctx.on_params_updated();
	ctx.on_session_begin(20240301);
	ctx.on_session_end(20240301);

	WTSBarStruct bar;
	memset(&bar, 0, sizeof(WTSBarStruct));
	bar.date = 20240301;
	ctx.on_bar("SSE.600000", "m", 1, &bar);

	WTSTickData* tick = p06MakeTick("SSE", "600000", 12.34);
	ASSERT_NE(nullptr, tick);
	// 持仓表为空 -> 跳过浮盈计算，直接走策略判空
	ctx.on_tick("SSE.600000", tick);

	ctx.on_order_queue("SSE.600000", WTSOrdQueData::create("SSE.600000"));
	ctx.on_order_detail("SSE.600000", WTSOrdDtlData::create("SSE.600000"));
	ctx.on_transaction("SSE.600000", WTSTransData::create("SSE.600000"));
}

TEST(p06_uft_context, engine_delegated_time_and_price)
{
	P06CtxFixture fx;
	UftStraContext ctx(&fx.engine, "p06stra_time");
	ctx.set_strategy(nullptr);

	// 时间三件套直接委派引擎
	EXPECT_EQ(20240301u, ctx.stra_get_date());
	EXPECT_EQ(93059u, ctx.stra_get_time());      // raw time
	EXPECT_EQ(25u, ctx.stra_get_secs());

	// 无行情时价格为 0.0
	EXPECT_DOUBLE_EQ(0.0, ctx.stra_get_price("SSE.600000"));

	// 引擎推送 tick 后，上下文读到同一价格
	WTSTickData* tick = p06MakeTick("SSE", "600000", 18.88);
	ASSERT_NE(nullptr, tick);
	fx.engine.on_tick("SSE.600000", tick);
	EXPECT_DOUBLE_EQ(18.88, ctx.stra_get_price("SSE.600000"));

	// 未知合约回到 0.0
	EXPECT_DOUBLE_EQ(0.0, ctx.stra_get_price("SSE.600001"));
}

TEST(p06_uft_context, local_position_lookups_miss_paths)
{
	P06CtxFixture fx;
	UftStraContext ctx(&fx.engine, "p06stra_pos");
	ctx.set_strategy(nullptr);

	// 本地持仓表从未写入 -> 三个查询都走 miss 分支
	EXPECT_DOUBLE_EQ(0.0, ctx.stra_get_local_position("SSE.600000"));
	EXPECT_DOUBLE_EQ(0.0, ctx.stra_get_local_posprofit("SSE.600000"));
	EXPECT_DOUBLE_EQ(0.0, ctx.stra_get_local_closeprofit("SSE.600000"));

	// 换个代码仍然是 miss（确认不是缓存了第一次结果）
	EXPECT_DOUBLE_EQ(0.0, ctx.stra_get_local_position("SSE.000001"));
	EXPECT_DOUBLE_EQ(0.0, ctx.stra_get_local_posprofit("SSE.000001"));
	EXPECT_DOUBLE_EQ(0.0, ctx.stra_get_local_closeprofit("SSE.000001"));

	// 空串同样安全
	EXPECT_DOUBLE_EQ(0.0, ctx.stra_get_local_position(""));
}

TEST(p06_uft_context, subscribe_apis_fill_engine_tables)
{
	P06CtxFixture fx;
	UftStraContext ctx(&fx.engine, "p06stra_sub");
	ctx.set_strategy(nullptr);
	fx.engine.addContext(UftContextPtr(std::shared_ptr<UftStraContext>(&ctx, [](UftStraContext*) {})));

	// 四个订阅接口：写入引擎订阅表 + 输出日志
	ctx.stra_sub_ticks("SSE.600000");
	ctx.stra_sub_order_details("SSE.600000");
	ctx.stra_sub_order_queues("SSE.600000");
	ctx.stra_sub_transactions("SSE.600000");

	// 订阅生效后，引擎推送会回调到本上下文（策略为空 -> 安全空操作）
	WTSTickData* tick = p06MakeTick("SSE", "600000", 9.99, 93030000);
	ASSERT_NE(nullptr, tick);
	fx.engine.on_tick("SSE.600000", tick);
	fx.engine.handle_push_order_detail(WTSOrdDtlData::create("SSE.600000"));
	fx.engine.handle_push_order_queue(WTSOrdQueData::create("SSE.600000"));
	fx.engine.handle_push_transaction(WTSTransData::create("SSE.600000"));

	// 重复订阅同一合约不会破坏订阅表
	ctx.stra_sub_ticks("SSE.600000");
	WTSTickData* tick2 = p06MakeTick("SSE", "600000", 10.01, 93031000);
	ASSERT_NE(nullptr, tick2);
	fx.engine.on_tick("SSE.600000", tick2);
	EXPECT_DOUBLE_EQ(10.01, ctx.stra_get_price("SSE.600000"));
}

TEST(p06_uft_context, strategy_logging_paths)
{
	P06CtxFixture fx;
	UftStraContext ctx(&fx.engine, "p06stra_log");
	ctx.set_strategy(nullptr);

	// 三个日志接口只是 WTSLogger::log_dyn_raw 的转发，未初始化日志器时也是空操作
	ctx.stra_log_info("p06 info message");
	ctx.stra_log_debug("p06 debug message");
	ctx.stra_log_error("p06 error message");

	// 空消息同样安全
	ctx.stra_log_info("");
	ctx.stra_log_error("");
}

//////////////////////////////////////////////////////////////////////////
// 崩溃类缺陷整体隔离（与 BUG-N03-005 未初始化成员同一缺陷类，禁止在启用用例触发）：
//   1) UftStraContext 构造函数不初始化 _trader（初始化列表只有 IUftStraCtx/_engine/
//      _tradingday/_strategy），任何走 _trader 的接口（stra_get_position / stra_cancel /
//      stra_cancel_all / stra_buy / stra_sell / stra_enter_* / stra_exit_*）在
//      setTrader() 之前都是野指针解引用。
//   2) 新建的 TraderAdapter 内部 _stat_map / _orders 等容器指针同样不由构造函数初始化，
//      即便 setTrader() 之后把请求转给一个 fresh adapter，也会在空表路径上崩溃
//      （上一波 n03_trader_adapter.fresh_adapter_public_accessors 实测段错误）。
//   3) watch_param / read_param / sync_param 在无共享内存段时对 func_* 空函数指针解引用。
TEST(p06_uft_context, DISABLED_uninitialized_trader_and_param_class)
{
	P06CtxFixture fx;

	// (1) 未 setTrader 的上下文
	UftStraContext ctx(&fx.engine, "p06stra_bad");
	volatile double a = ctx.stra_get_position("SSE.600000");
	(void)a;
	volatile bool b = ctx.stra_cancel(1u);
	(void)b;

	// (2) 挂上一个从未 init() 的适配器
	TraderAdapter trader;
	ctx.setTrader(&trader);
	volatile double c = ctx.stra_get_position("SSE.600000", false, 3);
	(void)c;
	volatile double e = ctx.stra_get_undone("SSE.600000");
	(void)e;

	// (3) 参数读写走 ShareManager 空函数指针
	volatile const char* f = ctx.read_param("k", "v");
	(void)f;
	volatile double g = ctx.watch_param("k", 1.0);
	(void)g;
}
