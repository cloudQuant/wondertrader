/**
 * r41_cta_engine_test.cpp  (lane R41 / wave10)
 *
 * 覆盖 src/WtCore/WtCtaEngine.cpp 中需要策略上下文的路径:
 *   on_init / on_schedule(无池 + 线程池) / on_bar / on_tick(ready + 复权订阅) /
 *   on_session_begin/end / IExecuterStub 访问器。
 *
 * 用真实的 CtaStraBaseCtx 作为上下文(公开构造函数), 用记录型 IExecCommand
 * 断言引擎提交的目标仓位, 不触碰任何柜台/行情。
 */
#include "r41_wtcore_fixture.h"

#include "WtCore/WtCtaEngine.h"
#include "WtCore/CtaStraBaseCtx.h"
#include "Includes/WTSStruct.h"
#include "Share/TimeUtils.hpp"

#include <cstring>
#include <memory>

namespace {

using namespace r41;

const char* const kCode = "SHFE.rb.2310";

//////////////////////////////////////////////////////////////////
// 记录型执行器: 只记最后一次 set_position 的目标仓位
//////////////////////////////////////////////////////////////////
class RecExecuter : public IExecCommand
{
public:
    explicit RecExecuter(const char* name) : IExecCommand(name) {}
    virtual ~RecExecuter() {}

    void set_position(const wt_hashmap<std::string, double>& targets) override
    {
        _last = targets;
        ++_sets;
    }

    wt_hashmap<std::string, double> _last;
    uint32_t _sets = 0;
};

typedef std::shared_ptr<RecExecuter> RecExecPtr;

// CtaStraBaseCtx 只差策略侧两个纯虚函数; 用空实现即可作为引擎上下文替身
class CtaCtxFake : public CtaStraBaseCtx
{
public:
    CtaCtxFake(WtCtaEngine* engine, const char* name, int32_t slippage)
        : CtaStraBaseCtx(engine, name, slippage) {}

    void on_bar_close(const char*, const char*, WTSBarStruct*) override {}
    void on_calculate(uint32_t, uint32_t) override {}

    void on_tick(const char* stdCode, WTSTickData* newTick, bool bEmitStrategy = true) override
    {
        _last_code = stdCode;
        _last_tick_price = newTick->price();
        _last_tick_open = newTick->getTickStruct().open;
        ++_ticks;
        CtaStraBaseCtx::on_tick(stdCode, newTick, bEmitStrategy);
    }

    std::string _last_code;
    double _last_tick_price = 0.0;
    double _last_tick_open = 0.0;
    uint32_t _ticks = 0;
};

typedef std::shared_ptr<CtaCtxFake> CtaCtxPtr;

// 给无规则股票复权用例提供真实的 Stock 商品类别和确定的非 1 因子。
class R41StockBaseDataMgr final : public IBaseDataMgr
{
public:
	R41StockBaseDataMgr()
	{
		_stock = WTSCommodityInfo::create("600000", "R41 stock", "SSE", "TRADING", "CHINA");
		_stock->setCategory(CC_Stock);
		_stock->setTradingMode(TM_Both);
		_stock->setVolScale(1);
		_stock->setSessionInfo(_fallback.session());

		_stock_contract = WTSContractInfo::create("600000", "600000", "SSE", "600000");
		_stock_contract->setVolScale(1);
		_stock_contract->setCommInfo(_stock);
	}

	~R41StockBaseDataMgr()
	{
		_stock_contract->release();
		_stock->release();
	}

	WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		if (exchgpid != NULL && std::string(exchgpid) == "SSE.600000")
			return _stock;
		return _fallback.getCommodity(exchgpid);
	}

	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		if (exchg != NULL && pid != NULL && std::string(exchg) == "SSE" && std::string(pid) == "600000")
			return _stock;
		return _fallback.getCommodity(exchg, pid);
	}

	WTSContractInfo* getContract(const char* code, const char* exchg, uint32_t date = 0) override
	{
		if (code != NULL && exchg != NULL && std::string(code) == "600000" && std::string(exchg) == "SSE")
			return _stock_contract;
		return _fallback.getContract(code, exchg, date);
	}

	WTSArray* getContracts(const char* exchg = "", uint32_t date = 0) override
	{
		return _fallback.getContracts(exchg, date);
	}

	WTSSessionInfo* getSession(const char* sid) override { return _fallback.getSession(sid); }
	WTSSessionInfo* getSessionByCode(const char* code, const char* exchg = "") override
	{
		return _fallback.getSessionByCode(code, exchg);
	}
	WTSArray* getAllSessions() override { return _fallback.getAllSessions(); }
	bool isHoliday(const char* pid, uint32_t date, bool isTpl = false) override
	{
		return _fallback.isHoliday(pid, date, isTpl);
	}
	uint32_t calcTradingDate(const char* stdPID, uint32_t date, uint32_t time, bool isSession = false) override
	{
		return _fallback.calcTradingDate(stdPID, date, time, isSession);
	}
	uint64_t getBoundaryTime(const char* stdPID, uint32_t date, bool isSession = false, bool isStart = true) override
	{
		return _fallback.getBoundaryTime(stdPID, date, isSession, isStart);
	}

	WTSContractInfo* stockContract() const { return _stock_contract; }

private:
	BaseDataMgrFake _fallback;
	WTSCommodityInfo* _stock = NULL;
	WTSContractInfo* _stock_contract = NULL;
};

class R41StockFactorDtMgr final : public WtDtMgr
{
public:
	double get_adjusting_factor(const char* stdCode, uint32_t date) override
	{
		_last_code = stdCode == NULL ? "" : stdCode;
		_last_date = date;
		return 2.0;
	}

	std::string _last_code;
	uint32_t _last_date = 0;
};

// 通过公开 API 造一手理论持仓: stra_set_position 挂信号, 再喂一笔 tick 让上下文成交。
// ctx 里的 do_set_position 是 private, 这里不绕过访问控制。
void seedPosition(R41EngineFixture& fx, CtaCtxFake* ctx, double qty)
{
    WTSTickData* seed = fx.makeTick(kCode, 3500.0);
    ctx->stra_set_position(kCode, qty);
    ctx->on_tick(kCode, seed);
    seed->release();
}

//////////////////////////////////////////////////////////////////
// on_init: 上下文持仓 -> 执行器目标仓位, 无风控缩放
// src/WtCore/WtCtaEngine.cpp:148-217
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41OnInitEnumeratesContextPosition)
{
    WTSVariant* cfg = makeCfg("", 0);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 930, 0);
    engine.set_trading_date(20231010);

    EvtListenerFake listener;
    engine.regEventListener(&listener);

    RecExecPtr exec(new RecExecuter("r41_exec"));
    engine.addExecuter(ExecCmdPtr(exec));

    CtaCtxPtr ctx(new CtaCtxFake(&engine, "r41ctx", 0));
    engine.addContext(ctx);

    EXPECT_EQ(ctx, engine.getContext(ctx->id()));
    EXPECT_EQ(CtaContextPtr(), engine.getContext(99999));

    // 直接构造理论持仓, on_init 的 enum_position 会把它交给执行器
    seedPosition(*this, ctx.get(), 3.0);
    engine.on_init();

    EXPECT_EQ(1u, listener._init);
    ASSERT_EQ(1u, exec->_sets);
    ASSERT_NE(exec->_last.end(), exec->_last.find(kCode));
    EXPECT_DOUBLE_EQ(3.0, exec->_last.find(kCode)->second);
}

//////////////////////////////////////////////////////////////////
// on_init: riskmon 缩放后的目标仓位
// src/WtCore/WtCtaEngine.cpp:191-213
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41OnInitAppliesRiskScaleToCachedTargets)
{
    WTSVariant* cfg = makeCfg("", 0);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 930, 0);
    engine.set_trading_date(20231010);

    // setVolScale 把风险日期固定为当前交易日 -> on_init 走 bRiskEnabled 分支
    engine.setVolScale(2.0);

    RecExecPtr exec(new RecExecuter("r41_exec"));
    engine.addExecuter(ExecCmdPtr(exec));

    CtaCtxPtr ctx(new CtaCtxFake(&engine, "r41ctx", 0));
    engine.addContext(ctx);
    seedPosition(*this, ctx.get(), 3.0);

    engine.on_init();

    ASSERT_EQ(1u, exec->_sets);
    ASSERT_NE(exec->_last.end(), exec->_last.find(kCode));
    EXPECT_DOUBLE_EQ(6.0, exec->_last.find(kCode)->second);
}

//////////////////////////////////////////////////////////////////
// on_schedule: 无池分支, 上下文枚举 -> 缓存 -> 提交 -> 待触发信号
// src/WtCore/WtCtaEngine.cpp:316-420
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41OnScheduleWithoutPoolSubmitsContextTarget)
{
    WTSVariant* cfg = makeCfg("", 0);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 0);
    engine.set_trading_date(20231010);

    EvtListenerFake listener;
    engine.regEventListener(&listener);

    RecExecPtr exec(new RecExecuter("r41_exec"));
    engine.addExecuter(ExecCmdPtr(exec));

    CtaCtxPtr ctx(new CtaCtxFake(&engine, "r41ctx", 0));
    engine.addContext(ctx);
    seedPosition(*this, ctx.get(), 2.0);

    engine.on_schedule(20231010, 931);

    EXPECT_EQ(1u, listener._sched);
    EXPECT_EQ(931u, listener._last_sched_time);
    ASSERT_EQ(1u, exec->_sets);
    ASSERT_NE(exec->_last.end(), exec->_last.find(kCode));
    EXPECT_DOUBLE_EQ(2.0, exec->_last.find(kCode)->second);

    // on_schedule 里的 append_signal(..., true) 要等下一笔 tick 才落到组合持仓
    WTSTickData* tick = makeTick(kCode, 3510.0);
    engine.on_tick(kCode, tick);
    std::string saved = readPortfolio("datas.json");
    EXPECT_NE(std::string::npos, saved.find("\"volume\": 2")) << saved;

    tick->release();
}

//////////////////////////////////////////////////////////////////
// on_schedule: 有线程池分支
// src/WtCore/WtCtaEngine.cpp:255-314
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41OnScheduleWithPoolUsesThreadPool)
{
    WTSVariant* cfg = makeCfg("", 2);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 0);
    engine.set_trading_date(20231010);

    RecExecPtr exec(new RecExecuter("r41_exec"));
    engine.addExecuter(ExecCmdPtr(exec));

    CtaCtxPtr ctx(new CtaCtxFake(&engine, "r41ctx", 0));
    engine.addContext(ctx);
    // commit_cached_targets 会按 decimal::rnd 取整（WtExecMgr.cpp:143-148），
    // 这里用整数手数，避免把取整语义混进线程池分支的断言。
    seedPosition(*this, ctx.get(), 2.0);

    engine.on_schedule(20231010, 931);

    ASSERT_EQ(1u, exec->_sets);
    ASSERT_NE(exec->_last.end(), exec->_last.find(kCode));
    EXPECT_DOUBLE_EQ(2.0, exec->_last.find(kCode)->second);
}

//////////////////////////////////////////////////////////////////
// on_tick: _ready + 订阅表(opt=0 原始 / opt=2 后复权)
// src/WtCore/WtCtaEngine.cpp:490-637
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41OnTickDispatchesRawAndHfqAdjusted)
{
    WTSVariant* cfg = makeCfg("", 0);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 0);
    engine.set_trading_date(20231010);

    CtaCtxPtr rawCtx(new CtaCtxFake(&engine, "r41raw", 0));
    CtaCtxPtr hfqCtx(new CtaCtxFake(&engine, "r41hfq", 0));
    engine.addContext(rawCtx);
    engine.addContext(hfqCtx);

    // ready 置位后 on_tick 才会查订阅表
    engine.on_session_begin();

    engine.sub_tick(rawCtx->id(), kCode);
    // 后复权代码以 '+' 结尾, 订阅表键是去掉后缀的代码
    engine.sub_tick(hfqCtx->id(), "SHFE.rb.2310+");

    WTSTickData* tick = makeTick(kCode, 10.0);
    engine.on_tick(kCode, tick);

    EXPECT_EQ(std::string(kCode), rawCtx->_last_code);
    EXPECT_EQ(std::string("SHFE.rb.2310+"), hfqCtx->_last_code);
    EXPECT_DOUBLE_EQ(10.0, rawCtx->stra_get_price(kCode));
    EXPECT_DOUBLE_EQ(10.0, hfqCtx->stra_get_price("SHFE.rb.2310+"));

    tick->release();
}

//////////////////////////////////////////////////////////////////
// 无规则标签的 '-' 订阅保持前复权标记，并在实际 tick 分发时收到 '-' 代码
// src/WtCore/WtEngine.cpp:842-849 (length-- 后缀位于 stdCode[length])
// 期望: 前复权代码 -> 上下文收到 "SHFE.rb.2310-"。
// 见 bugs/R41.md BUG-R41-002
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41PlainQfqSubscriptionDispatchesFrontAdjusted)
{
    WTSVariant* cfg = makeCfg("", 0);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 0);
    engine.set_trading_date(20231010);

    CtaCtxPtr ctx(new CtaCtxFake(&engine, "r41ctx", 0));
    engine.addContext(ctx);
    engine.on_session_begin();

    engine.sub_tick(ctx->id(), "SHFE.rb.2310-");

    WTSTickData* tick = makeTick(kCode, 10.0);
    engine.on_tick(kCode, tick);

	// 前复权订阅应保留前复权后缀。
    EXPECT_EQ(std::string("SHFE.rb.2310-"), ctx->_last_code);

    tick->release();
}

//////////////////////////////////////////////////////////////////
// QFQ/HFQ/plain 回调在直连与线程池模式下保留代码后缀与价格语义。
// 普通代码覆盖非主力订阅分支（含 R41- 修复）；.R41 使用 HotMgrFake 的
// 非 1 复权因子 2.0，确认 HFQ 回调 Tick 价格字段确实转换。
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41AdjustedTickDispatchMatchesDirectAndPoolModes)
{
	for (uint32_t poolsize : { 0u, 2u })
	{
		SCOPED_TRACE(::testing::Message() << "poolsize=" << poolsize);
		WTSVariant* cfg = makeCfg("", poolsize);
		ASSERT_NE(nullptr, cfg);

		WtCtaEngine engine;
		engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
		engine.set_date_time(20231010, 931, 0);
		engine.set_trading_date(20231010);

		CtaCtxPtr rawCtx(new CtaCtxFake(&engine, "r41raw", 0));
		CtaCtxPtr qfqCtx(new CtaCtxFake(&engine, "r41qfq", 0));
		CtaCtxPtr hfqCtx(new CtaCtxFake(&engine, "r41hfq", 0));
		CtaCtxPtr ruleRawCtx(new CtaCtxFake(&engine, "r41rule_raw", 0));
		CtaCtxPtr ruleQfqCtx(new CtaCtxFake(&engine, "r41rule_qfq", 0));
		CtaCtxPtr ruleHfqCtx(new CtaCtxFake(&engine, "r41rule_hfq", 0));
		engine.addContext(rawCtx);
		engine.addContext(qfqCtx);
		engine.addContext(hfqCtx);
		engine.addContext(ruleRawCtx);
		engine.addContext(ruleQfqCtx);
		engine.addContext(ruleHfqCtx);
		engine.on_session_begin();

		engine.sub_tick(rawCtx->id(), kCode);
		engine.sub_tick(qfqCtx->id(), "SHFE.rb.2310-");
		engine.sub_tick(hfqCtx->id(), "SHFE.rb.2310+");
		engine.sub_tick(ruleRawCtx->id(), "SHFE.rb.R41");
		engine.sub_tick(ruleQfqCtx->id(), "SHFE.rb.R41-");
		engine.sub_tick(ruleHfqCtx->id(), "SHFE.rb.R41+");

		WTSTickData* tick = makeTick(kCode, 10.0);
		engine.on_tick(kCode, tick);
		tick->release();

		WTSTickData* ruleTick = makeTick("SHFE.rb.R41", 10.0);
		engine.on_tick("SHFE.rb.R41", ruleTick);
		ruleTick->release();

		EXPECT_EQ(kCode, rawCtx->_last_code);
		EXPECT_EQ("SHFE.rb.2310-", qfqCtx->_last_code);
		EXPECT_EQ("SHFE.rb.2310+", hfqCtx->_last_code);
		EXPECT_EQ("SHFE.rb.R41", ruleRawCtx->_last_code);
		EXPECT_EQ("SHFE.rb.R41-", ruleQfqCtx->_last_code);
		EXPECT_EQ("SHFE.rb.R41+", ruleHfqCtx->_last_code);
		EXPECT_EQ(1u, rawCtx->_ticks);
		EXPECT_EQ(1u, qfqCtx->_ticks);
		EXPECT_EQ(1u, hfqCtx->_ticks);
		EXPECT_EQ(1u, ruleRawCtx->_ticks);
		EXPECT_EQ(1u, ruleQfqCtx->_ticks);
		EXPECT_EQ(1u, ruleHfqCtx->_ticks);
		EXPECT_DOUBLE_EQ(10.0, rawCtx->_last_tick_price);
		EXPECT_DOUBLE_EQ(9.0, rawCtx->_last_tick_open);
		EXPECT_DOUBLE_EQ(10.0, qfqCtx->_last_tick_price);
		EXPECT_DOUBLE_EQ(9.0, qfqCtx->_last_tick_open);
		EXPECT_DOUBLE_EQ(10.0, hfqCtx->_last_tick_price);
		EXPECT_DOUBLE_EQ(9.0, hfqCtx->_last_tick_open);
		EXPECT_DOUBLE_EQ(10.0, ruleRawCtx->_last_tick_price);
		EXPECT_DOUBLE_EQ(9.0, ruleRawCtx->_last_tick_open);
		EXPECT_DOUBLE_EQ(10.0, ruleQfqCtx->_last_tick_price);
		EXPECT_DOUBLE_EQ(9.0, ruleQfqCtx->_last_tick_open);
		EXPECT_DOUBLE_EQ(20.0, ruleHfqCtx->_last_tick_price);
		EXPECT_DOUBLE_EQ(18.0, ruleHfqCtx->_last_tick_open);
		EXPECT_DOUBLE_EQ(10.0, rawCtx->stra_get_price(kCode));
		EXPECT_DOUBLE_EQ(10.0, qfqCtx->stra_get_price("SHFE.rb.2310-"));
		EXPECT_DOUBLE_EQ(10.0, hfqCtx->stra_get_price("SHFE.rb.2310+"));
		EXPECT_DOUBLE_EQ(10.0, ruleRawCtx->stra_get_price("SHFE.rb.R41"));
		EXPECT_DOUBLE_EQ(10.0, ruleQfqCtx->stra_get_price("SHFE.rb.R41-"));
		EXPECT_DOUBLE_EQ(20.0, ruleHfqCtx->stra_get_price("SHFE.rb.R41+"));
	}
}

TEST_F(R41EngineFixture, R41UnruledStockAdjustmentDispatchMatchesDirectAndPoolModes)
{
	const char* stockCode = "SSE.600000";
	for (uint32_t poolsize : { 0u, 2u })
	{
		SCOPED_TRACE(::testing::Message() << "poolsize=" << poolsize);
		WTSVariant* cfg = makeCfg("", poolsize);
		ASSERT_NE(nullptr, cfg);

		R41StockBaseDataMgr stockBaseData;
		R41StockFactorDtMgr dataMgr;
		WTSCommodityInfo* stockInfo = stockBaseData.getCommodity("SSE", "600000");
		ASSERT_NE(nullptr, stockInfo);
		EXPECT_TRUE(stockInfo->isStock());
		EXPECT_STREQ("", _hot->getRuleTag(stockCode));
		WtCtaEngine engine;
		engine.init(cfg, &stockBaseData, &dataMgr, _hot.get(), NULL);
		engine.set_date_time(20231010, 931, 0);
		engine.set_trading_date(20231010);

		CtaCtxPtr rawCtx(new CtaCtxFake(&engine, "r41stock_raw", 0));
		CtaCtxPtr qfqCtx(new CtaCtxFake(&engine, "r41stock_qfq", 0));
		CtaCtxPtr hfqCtx(new CtaCtxFake(&engine, "r41stock_hfq", 0));
		engine.addContext(rawCtx);
		engine.addContext(qfqCtx);
		engine.addContext(hfqCtx);
		engine.on_session_begin();

		engine.sub_tick(rawCtx->id(), stockCode);
		engine.sub_tick(qfqCtx->id(), "SSE.600000-");
		engine.sub_tick(hfqCtx->id(), "SSE.600000+");

		WTSTickData* tick = WTSTickData::create(stockCode);
		ASSERT_NE(nullptr, tick);
		WTSTickStruct& tickStruct = tick->getTickStruct();
		tickStruct.price = 10.0;
		tickStruct.open = 9.0;
		tickStruct.high = 11.0;
		tickStruct.low = 8.0;
		tickStruct.pre_close = 9.0;
		tickStruct.pre_settle = 9.0;
		tickStruct.settle_price = 10.0;
		tickStruct.volume = 0;
		tickStruct.total_volume = 0;
		tickStruct.trading_date = 20231010;
		tickStruct.action_date = 20231010;
		tickStruct.action_time = 93000000;
		tick->setContractInfo(stockBaseData.stockContract());
		engine.on_tick(stockCode, tick);
		tick->release();

		EXPECT_EQ("SSE.600000", rawCtx->_last_code);
		EXPECT_EQ("SSE.600000-", qfqCtx->_last_code);
		EXPECT_EQ("SSE.600000+", hfqCtx->_last_code);
		EXPECT_EQ(1u, rawCtx->_ticks);
		EXPECT_EQ(1u, qfqCtx->_ticks);
		EXPECT_EQ(1u, hfqCtx->_ticks);
		EXPECT_DOUBLE_EQ(10.0, rawCtx->_last_tick_price);
		EXPECT_DOUBLE_EQ(9.0, rawCtx->_last_tick_open);
		EXPECT_DOUBLE_EQ(10.0, qfqCtx->_last_tick_price);
		EXPECT_DOUBLE_EQ(9.0, qfqCtx->_last_tick_open);
		EXPECT_DOUBLE_EQ(20.0, hfqCtx->_last_tick_price);
		EXPECT_DOUBLE_EQ(18.0, hfqCtx->_last_tick_open);
		EXPECT_DOUBLE_EQ(10.0, rawCtx->stra_get_price(stockCode));
		EXPECT_DOUBLE_EQ(10.0, qfqCtx->stra_get_price("SSE.600000-"));
		EXPECT_DOUBLE_EQ(20.0, hfqCtx->stra_get_price("SSE.600000+"));
		EXPECT_EQ(stockCode, dataMgr._last_code);
		EXPECT_EQ(20231010u, dataMgr._last_date);
	}
}

//////////////////////////////////////////////////////////////////
// on_bar: 周期订阅表 -> 上下文回调
// src/WtCore/WtCtaEngine.cpp:639-671
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41OnBarDispatchesToSubscribedContext)
{
    WTSVariant* cfg = makeCfg("", 0);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);

    CtaCtxPtr ctx(new CtaCtxFake(&engine, "r41ctx", 0));
    engine.addContext(ctx);

    // 只登记 sid->bar 订阅, 数据读取器为空, 返回 NULL
    EXPECT_EQ(nullptr, engine.get_kline_slice(ctx->id(), kCode, "d", 10));

    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.date = 20231010;
    bar.open = 3500;
    bar.high = 3510;
    bar.low = 3490;
    bar.close = 3505;

    engine.on_bar(kCode, "d", 1, &bar);
    EXPECT_EQ(ctx, engine.getContext(ctx->id()));
}

//////////////////////////////////////////////////////////////////
// session 事件 + 无 notifier 的通知透传守卫
// src/WtCore/WtCtaEngine.cpp:219-247, 704-720
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41SessionEventsReachContextAndListener)
{
    WTSVariant* cfg = makeCfg("", 0);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 1450, 0);
    engine.set_trading_date(20231010);

    EvtListenerFake listener;
    engine.regEventListener(&listener);

    CtaCtxPtr ctx(new CtaCtxFake(&engine, "r41ctx", 0));
    engine.addContext(ctx);
    seedPosition(*this, ctx.get(), 1.0);

    engine.on_session_begin();
    EXPECT_EQ(1u, listener._session);
    EXPECT_TRUE(listener._last_session_begin);

    engine.on_session_end();
    EXPECT_EQ(2u, listener._session);
    EXPECT_FALSE(listener._last_session_begin);

    // _notifier == NULL 时三个通知入口都直接返回
    engine.notify_chart_marker(20231010093100ULL, "r41ctx", 3500.0, "icon", "tag");
    engine.notify_chart_index(20231010093100ULL, "r41ctx", "idx", "line", 1.0);
    engine.notify_trade("r41ctx", kCode, true, true, 20231010093100ULL, 3500.0, "tag");
}

//////////////////////////////////////////////////////////////////
// IExecuterStub 访问器
// src/WtCore/WtCtaEngine.cpp:683-702
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41ExecuterStubAccessors)
{
    WTSVariant* cfg = makeCfg("", 0);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 500, 931);
    engine.set_trading_date(20231010);

    EXPECT_EQ(_bd->commodity(), engine.get_comm_info(kCode));
    EXPECT_EQ(_bd->session(), engine.get_sess_info(kCode));
    EXPECT_EQ(nullptr, engine.get_sess_info("SHFE.zz.9999"));
    EXPECT_EQ(_hot.get(), engine.get_hot_mon());
    EXPECT_EQ(20231010u, engine.get_trading_day());

    // 与 TimeUtils 直接计算的毫秒时间戳一致
    uint64_t expect = TimeUtils::makeTime(20231010, 931 * 100000 + 500);
    EXPECT_EQ(expect, engine.get_real_time());
}

}  // namespace
