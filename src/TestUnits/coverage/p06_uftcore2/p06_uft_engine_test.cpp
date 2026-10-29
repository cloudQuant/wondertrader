/*!
 * \file p06_uft_engine_test.cpp
 * \brief WonderTrader 测试覆盖率迭代01 第三波 / lane p06 —— WtUftEngine + WtUftDtMgr + WtUftRtTicker
 *
 * 覆盖范围（全部为不依赖 UFT 柜台 / 共享内存 / DLL / 线程的纯逻辑分支）：
 *   - WtUftEngine 构造/析构、init 指针装配、时间状态查询与设置、基础数据委派、
 *     订阅表写入、on_tick/on_bar/handle_push_* 分发、addContext/getContext、
 *     on_session_begin/end、notify_params_update、on_minute_end
 *   - WtUftDtMgr 默认构造、init、handle_push_quote、grab_last_tick、全部 slice 接口
 *   - WtUftRtTicker 构造、init（时段取不到时的空 _s_info 分支）、on_tick 的未起线程转发分支
 *
 * 明确不覆盖（见 manifest 排除清单）：
 *   - WtUftEngine::run()（new WtUftRtTicker + 线程启动 + _cfg->has("product") 配置解析）
 *   - WtUftRtTicker::run()/stop()（StdThread + sleep + join，属阻塞路径）
 *   - WtUftRtTicker::on_tick 的 _thrd != NULL 分支（只能由 run() 产生）
 *   - WtUftDtMgr 的历史 bar/tick 缓存装载（需要真实数据文件与 WtUftDtMgr 配置）
 *
 * 纪律：未初始化/空指针解引用这类会段错误的输入整类收进一个 DISABLED_ 用例；
 *       不依赖任何构造函数的默认值（先写后读）；不断言墙钟与本地时区。
 */
#include <gtest/gtest.h>

#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/WtUftDtMgr.h"
#include "WtUftCore/WtUftTicker.h"
#include "WtUftCore/UftStraContext.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSMarcos.h"

#include <cstring>
#include <memory>
#include <string>

USING_NS_WTP;

namespace {

//////////////////////////////////////////////////////////////////////////
// 假基础数据管理器：只返回调用方登记过的对象，其余一律 nullptr
class P06BaseDataMgr : public IBaseDataMgr
{
public:
	WTSCommodityInfo* getCommodity(const char*) override { return nullptr; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return nullptr; }

	WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t = 0) override
	{
		++_contractCalls;
		auto it = _maps.find(std::string(exchg) + "." + code);
		return it == _maps.end() ? nullptr : it->second;
	}

	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return WTSArray::create(); }

	WTSSessionInfo* getSession(const char* sid) override
	{
		_lastSessionId = sid == nullptr ? "" : sid;
		++_sessionCalls;
		return nullptr;
	}

	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return nullptr; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate + 1; }
	uint64_t getBoundaryTime(const char*, uint32_t, bool = false, bool = true) override { return 0; }

	void reg(WTSContractInfo* cInfo) { _maps[std::string(cInfo->getFullCode())] = cInfo; }

	int _contractCalls = 0;
	int _sessionCalls = 0;
	std::string _lastSessionId;

private:
	wt_hashmap<std::string, WTSContractInfo*> _maps;
};

//////////////////////////////////////////////////////////////////////////
// 引擎夹具：构造完成后立刻 init() 写入全部管理器指针，避免读到未初始化成员
struct EngineFixture
{
	EngineFixture() : engine()
	{
		// 构造函数只初始化 _tm_ticker/_cfg/_notifier；
		// _base_data_mgr/_data_mgr 必须由 init() 写入后才能查询
		engine.init(nullptr, &bdMgr, &dtMgr, nullptr);
	}

	P06BaseDataMgr bdMgr;
	WtUftDtMgr dtMgr;
	WtUftEngine engine;
};

// 上下文工厂：shared_ptr<UftStraContext> -> shared_ptr<IUftStraCtx>（别名构造，析构正确）
inline UftContextPtr makeCtx(WtUftEngine* engine, const char* name)
{
	std::shared_ptr<UftStraContext> sp(new UftStraContext(engine, name));
	return UftContextPtr(sp);
}

}  // namespace

//////////////////////////////////////////////////////////////////////////
TEST(p06_uft_engine, init_wires_managers_and_state_queries)
{
	EngineFixture fx;

	EXPECT_EQ(&fx.bdMgr, fx.engine.get_basedata_mgr());

	// 时间状态：先写后读，rawTime 缺省时等于 curTime
	fx.engine.set_date_time(20240301, 931, 45, 0);
	EXPECT_EQ(20240301u, fx.engine.get_date());
	EXPECT_EQ(931u, fx.engine.get_min_time());
	EXPECT_EQ(931u, fx.engine.get_raw_time());
	EXPECT_EQ(45u, fx.engine.get_secs());

	// 显式 rawTime 走另一分支
	fx.engine.set_date_time(20240301, 932, 30, 93159);
	EXPECT_EQ(932u, fx.engine.get_min_time());
	EXPECT_EQ(93159u, fx.engine.get_raw_time());
	EXPECT_EQ(30u, fx.engine.get_secs());

	fx.engine.set_trading_date(20240305);
	EXPECT_EQ(20240305u, fx.engine.get_trading_date());

	// 再次覆盖，确认 setter 无缓存残留
	fx.engine.set_date_time(20240304, 1000, 0, 1000);
	EXPECT_EQ(20240304u, fx.engine.get_date());
	EXPECT_EQ(1000u, fx.engine.get_min_time());
}

TEST(p06_uft_engine, contract_and_commodity_queries)
{
	EngineFixture fx;

	WTSContractInfo* cInfo = WTSContractInfo::create("600000", "p06name", "SSE", "SSE.600000");
	ASSERT_NE(nullptr, cInfo);
	fx.bdMgr.reg(cInfo);

	// 命中
	EXPECT_EQ(cInfo, fx.engine.get_contract_info("SSE.600000"));
	EXPECT_EQ(1, fx.bdMgr._contractCalls);

	// 未命中
	EXPECT_EQ(nullptr, fx.engine.get_contract_info("SSE.000001"));

	// 合约不存在 -> 品种早退返回 NULL
	EXPECT_EQ(nullptr, fx.engine.get_commodity_info("SSE.000001"));

	// 合约存在但 create() 出来的合约没有挂品种 -> 直接透传 NULL（无解引用）
	EXPECT_EQ(nullptr, fx.engine.get_commodity_info("SSE.600000"));
}

TEST(p06_uft_engine, session_info_two_lookup_paths)
{
	EngineFixture fx;

	// isCode = false：直接按 sid 查，假实现返回 NULL 但 sid 被透传
	EXPECT_EQ(nullptr, fx.engine.get_session_info("ALLDAY"));
	EXPECT_EQ("ALLDAY", fx.bdMgr._lastSessionId);
	EXPECT_EQ(1, fx.bdMgr._sessionCalls);

	// isCode = true 且合约不存在：早退返回 NULL
	WTSContractInfo* cInfo = WTSContractInfo::create("600000", "p06name", "SSE", "SSE.600000");
	ASSERT_NE(nullptr, cInfo);
	fx.bdMgr.reg(cInfo);
	EXPECT_EQ(nullptr, fx.engine.get_session_info("SSE.999999", true));

	// isCode = true 且合约存在：会解引用 cInfo->getCommInfo()（create() 后为 NULL）
	// 属于空指针缺陷类，收进 DISABLED 用例
}

TEST(p06_uft_engine, data_mgr_delegation_and_price)
{
	EngineFixture fx;
	fx.engine.set_date_time(20240301, 930, 0, 0);

	// 空数据管理器：grab 返回 NULL，价格回落到 0.0
	EXPECT_EQ(nullptr, fx.engine.get_last_tick(1, "SSE.600000"));
	EXPECT_DOUBLE_EQ(0.0, fx.engine.get_cur_price("SSE.600000"));

	// NULL tick 早退
	fx.dtMgr.handle_push_quote("SSE.600000", nullptr);
	EXPECT_EQ(nullptr, fx.engine.get_last_tick(1, "SSE.600000"));

	// 推一条真实 tick 前，init 只写引擎指针并返回 true
	EXPECT_TRUE(fx.dtMgr.init(nullptr, &fx.engine));
}

TEST(p06_uft_engine, price_after_push_and_slice_interfaces)
{
	EngineFixture fx;
	fx.engine.set_date_time(20240301, 930, 0, 0);

	WTSTickStruct ts1;
	memset(&ts1, 0, sizeof(WTSTickStruct));
	wt_strcpy(ts1.exchg, "SSE");
	wt_strcpy(ts1.code, "600000");
	ts1.trading_date = 20240301;
	ts1.action_date = 20240301;
	ts1.action_time = 93015500;
	ts1.price = 33.25;
	WTSTickData* tick = WTSTickData::create(ts1);
	ASSERT_NE(nullptr, tick);
	fx.dtMgr.handle_push_quote("SSE.600000", tick);

	// get_cur_price 内部 release()，引用计数语义由生产代码持有
	EXPECT_DOUBLE_EQ(33.25, fx.engine.get_cur_price("SSE.600000"));

	// 覆盖同合约的第二次推送（更新最新价）
	WTSTickStruct ts2 = ts1;
	ts2.price = 34.5;
	WTSTickData* tick2 = WTSTickData::create(ts2);
	ASSERT_NE(nullptr, tick2);
	fx.dtMgr.handle_push_quote("SSE.600000", tick2);
	EXPECT_DOUBLE_EQ(34.5, fx.engine.get_cur_price("SSE.600000"));

	// 未知合约仍然是 0.0
	EXPECT_DOUBLE_EQ(0.0, fx.engine.get_cur_price("SSE.600001"));

	// 引擎与数据管理器的全部 slice 接口当前实现都是空返回
	EXPECT_EQ(nullptr, fx.engine.get_tick_slice(1, "SSE.600000", 10));
	EXPECT_EQ(nullptr, fx.engine.get_kline_slice(1, "SSE.600000", "m", 10, 1, 0));
	EXPECT_EQ(nullptr, fx.engine.get_order_queue_slice(1, "SSE.600000", 10));
	EXPECT_EQ(nullptr, fx.engine.get_order_detail_slice(1, "SSE.600000", 10));
	EXPECT_EQ(nullptr, fx.engine.get_transaction_slice(1, "SSE.600000", 10));

	EXPECT_EQ(nullptr, fx.dtMgr.get_tick_slice("SSE.600000", 10));
	EXPECT_EQ(nullptr, fx.dtMgr.get_kline_slice("SSE.600000", KP_Minute1, 1, 10));
	EXPECT_EQ(nullptr, fx.dtMgr.get_order_queue_slice("SSE.600000", 10));
	EXPECT_EQ(nullptr, fx.dtMgr.get_order_detail_slice("SSE.600000", 10));
	EXPECT_EQ(nullptr, fx.dtMgr.get_transaction_slice("SSE.600000", 10));
}

TEST(p06_uft_engine, context_map_add_and_get)
{
	EngineFixture fx;

	// 空表查询
	EXPECT_EQ(nullptr, fx.engine.getContext(777).get());

	UftContextPtr ctx1 = makeCtx(&fx.engine, "p06ctx_add");
	ASSERT_NE(nullptr, ctx1.get());
	const uint32_t cid = ctx1->id();
	fx.engine.addContext(ctx1);

	UftContextPtr got = fx.engine.getContext(cid);
	ASSERT_NE(nullptr, got.get());
	EXPECT_EQ(cid, got->id());
	EXPECT_STREQ("p06ctx_add", got->name());

	// 不同 id 的第二路添加
	UftContextPtr ctx2 = makeCtx(&fx.engine, "p06ctx_add2");
	fx.engine.addContext(ctx2);
	EXPECT_STREQ("p06ctx_add2", fx.engine.getContext(ctx2->id())->name());
	EXPECT_NE(nullptr, fx.engine.getContext(cid).get());

	// 不存在的 id 仍然是空
	EXPECT_EQ(nullptr, fx.engine.getContext(cid + 987654u).get());
}

TEST(p06_uft_engine, tick_subscribe_dispatch_and_miss)
{
	EngineFixture fx;
	fx.engine.set_date_time(20240301, 930, 0, 0);

	UftContextPtr ctx = makeCtx(&fx.engine, "p06ctx_tick");
	fx.engine.addContext(ctx);
	const uint32_t cid = ctx->id();

	// 已订阅合约 + 有上下文：走完整分发（上下文 _strategy 为 NULL，回调是安全空操作）
	fx.engine.sub_tick(cid, "SSE.600000");
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(WTSTickStruct));
	wt_strcpy(ts.exchg, "SSE");
	wt_strcpy(ts.code, "600000");
	ts.trading_date = 20240301;
	ts.action_date = 20240301;
	ts.action_time = 93015500;
	ts.price = 11.0;
	WTSTickData* t1 = WTSTickData::create(ts);
	ASSERT_NE(nullptr, t1);
	fx.engine.on_tick("SSE.600000", t1);
	EXPECT_DOUBLE_EQ(11.0, fx.engine.get_cur_price("SSE.600000"));

	// 订阅了但上下文不存在：命中订阅表、未命中上下文表
	fx.engine.sub_tick(cid + 424242u, "SSE.600001");
	WTSTickStruct ts2 = ts;
	wt_strcpy(ts2.code, "600001");
	ts2.price = 12.0;
	WTSTickData* t2 = WTSTickData::create(ts2);
	ASSERT_NE(nullptr, t2);
	fx.engine.on_tick("SSE.600001", t2);

	// 未订阅任何合约的 code：订阅表 miss 分支
	WTSTickStruct ts3 = ts;
	wt_strcpy(ts3.code, "600002");
	WTSTickData* t3 = WTSTickData::create(ts3);
	ASSERT_NE(nullptr, t3);
	fx.engine.on_tick("SSE.600002", t3);

	// 同一合约重复订阅同一 sid：SubList 去重，不应重复回调
	fx.engine.sub_tick(cid, "SSE.600000");
	WTSTickStruct ts4 = ts;
	ts4.price = 13.0;
	WTSTickData* t4 = WTSTickData::create(ts4);
	ASSERT_NE(nullptr, t4);
	fx.engine.on_tick("SSE.600000", t4);
	EXPECT_DOUBLE_EQ(13.0, fx.engine.get_cur_price("SSE.600000"));
}

TEST(p06_uft_engine, bar_and_l2_push_dispatch)
{
	EngineFixture fx;
	fx.engine.set_date_time(20240301, 930, 0, 0);

	UftContextPtr ctx = makeCtx(&fx.engine, "p06ctx_l2");
	fx.engine.addContext(ctx);
	const uint32_t cid = ctx->id();

	// on_bar 的订阅表只由 get_kline_slice 写入，而后者是死代码，因此走空表分支
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(WTSBarStruct));
	bar.date = 20240301;
	fx.engine.on_bar("SSE.600000", "m", 1, &bar);
	fx.engine.on_bar("SSE.600000", "d", 1, &bar);
	fx.engine.on_minute_end(20240301, 931);

	// 订阅三张 L2 表并推送，覆盖订阅表命中 + 上下文命中/未命中两支
	fx.engine.sub_order_detail(cid, "SSE.600000");
	fx.engine.sub_order_queue(cid, "SSE.600000");
	fx.engine.sub_transaction(cid, "SSE.600000");
	fx.engine.sub_order_detail(cid + 999u, "SSE.600000");
	fx.engine.sub_order_queue(cid + 999u, "SSE.600000");
	fx.engine.sub_transaction(cid + 999u, "SSE.600000");

	fx.engine.handle_push_order_detail(WTSOrdDtlData::create("SSE.600000"));
	fx.engine.handle_push_order_queue(WTSOrdQueData::create("SSE.600000"));
	fx.engine.handle_push_transaction(WTSTransData::create("SSE.600000"));

	// 未订阅合约：三张表都 miss
	fx.engine.handle_push_order_detail(WTSOrdDtlData::create("SSE.600009"));
	fx.engine.handle_push_order_queue(WTSOrdQueData::create("SSE.600009"));
	fx.engine.handle_push_transaction(WTSTransData::create("SSE.600009"));

	// 没有 ticker 时 handle_push_quote 直接早退
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(WTSTickStruct));
	wt_strcpy(ts.exchg, "SSE");
	wt_strcpy(ts.code, "600000");
	ts.action_date = 20240301;
	ts.action_time = 93015500;
	WTSTickData* tick = WTSTickData::create(ts);
	ASSERT_NE(nullptr, tick);
	fx.engine.handle_push_quote(tick);

	// 会话事件 + 参数更新通知（上下文策略为空，均为安全空操作）
	fx.engine.set_trading_date(20240301);
	fx.engine.on_session_begin();
	fx.engine.on_session_end();
	fx.engine.on_init();
	fx.engine.notify_params_update("p06ctx_l2");
	fx.engine.notify_params_update("nobody_matches_this_name");
}

//////////////////////////////////////////////////////////////////////////
TEST(p06_uft_ticker, init_and_forward_before_thread_start)
{
	EngineFixture fx;
	fx.engine.set_date_time(20240301, 930, 0, 0);

	UftContextPtr ctx = makeCtx(&fx.engine, "p06ctx_ticker");
	fx.engine.addContext(ctx);
	fx.engine.sub_tick(ctx->id(), "SSE.600000");

	WtUftRtTicker ticker(&fx.engine);
	// 假基础数据管理器的 getSession 返回 NULL，_s_info 也为 NULL；
	// init 只赋值时段并取系统日期，不触碰 _s_info，因此可安全调用。
	ticker.init("ALLDAY");
	EXPECT_EQ("ALLDAY", fx.bdMgr._lastSessionId);

	// _thrd == NULL 的转发分支：只把 tick 转给引擎，不使用 _s_info
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(WTSTickStruct));
	wt_strcpy(ts.exchg, "SSE");
	wt_strcpy(ts.code, "600000");
	ts.trading_date = 20240301;
	ts.action_date = 20240301;
	ts.action_time = 93015500;
	ts.price = 21.5;
	WTSTickData* tick = WTSTickData::create(ts);
	ASSERT_NE(nullptr, tick);
	ticker.on_tick(tick);
	EXPECT_DOUBLE_EQ(21.5, fx.engine.get_cur_price("SSE.600000"));

	// 第二次仍然只是转发
	WTSTickStruct ts2 = ts;
	ts2.price = 22.0;
	WTSTickData* tick2 = WTSTickData::create(ts2);
	ASSERT_NE(nullptr, tick2);
	ticker.on_tick(tick2);
	EXPECT_DOUBLE_EQ(22.0, fx.engine.get_cur_price("SSE.600000"));

	// 析构不 join（未起线程）
}

TEST(p06_uft_ticker, null_engine_guard_on_forward)
{
	// 构造时传 NULL 引擎，覆盖 on_tick 的 _engine 判空分支
	WtUftRtTicker ticker(nullptr);
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(WTSTickStruct));
	wt_strcpy(ts.code, "600000");
	WTSTickData* tick = WTSTickData::create(ts);
	ASSERT_NE(nullptr, tick);
	ticker.on_tick(tick);
}

//////////////////////////////////////////////////////////////////////////
// 崩溃类缺陷的整体隔离：以下输入会让进程挂掉/越界，禁止在启用用例中触发。
//   1) 引擎构造后未 init() 即查询：_base_data_mgr / _data_mgr 在构造函数里根本不初始化，
//      读它们是野指针解引用（与 BUG-N03-005 未初始化成员同一缺陷类）。
//   2) 非标准化合约代码（没有 '.'）：StrUtil::split 结果只有 1 个元素，
//      get_contract_info / get_commodity_info / get_session_info(isCode) 直接取 ay[1] 越界读。
// P06-003 仅能通过手工注册未挂接品种的合约违反 WTSBaseDataMgr loader 不变量触发，不代表正常产品问题。
TEST(p06_uft_engine, DISABLED_uninitialized_and_malformed_stdcode_class)
{
	// (1) 未 init 的引擎直接查询 -> 野指针
	WtUftEngine bare;
	volatile WTSContractInfo* p = bare.get_contract_info("SSE.600000");
	(void)p;
	volatile double d = bare.get_cur_price("SSE.600000");
	(void)d;

	// (2) 缺少交易所前缀 -> ay[1] 越界读
	EngineFixture fx;
	volatile WTSContractInfo* q = fx.engine.get_contract_info("600000");
	(void)q;
	volatile WTSCommodityInfo* r = fx.engine.get_commodity_info("600000");
	(void)r;
	volatile WTSSessionInfo* s = fx.engine.get_session_info("600000", true);
	(void)s;
}
