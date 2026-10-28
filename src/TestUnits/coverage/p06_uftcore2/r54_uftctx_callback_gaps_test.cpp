/*!
 * \file r54_uftctx_callback_gaps_test.cpp
 * \brief 第十一波 R54: UftStraContext 策略回调/盯市/明细边界/切片查询补测
 *
 * 目标: src/WtUftCore/UftStraContext.cpp
 *  - on_init/on_tick/on_order_queue/on_order_detail/on_transaction/on_bar 的 _strategy 转发
 *  - on_tick 对多/空净持仓的盯市 _dynprofit 与明细 _position_profit
 *  - on_channel_ready 的持仓通知、on_channel_lost/on_entrust/session/params
 *  - load_local_data 的 mannual.yaml 导入(持仓块新建/合约缺失 continue/文件改名)
 *  - on_order 撤单状态、on_trade 明细遍历的 valid_idx/反向明细/left==0 边界
 *  - stra_get_bars/ticks/order_detail/order_queue/transaction/last_tick 转发
 *
 * 落盘全部在 WtHelper::getOutputDir() 下的唯一策略子目录, 用例结束 remove_all。
 * 不启动 shm/网络/线程; 策略替身只计数, 不解引用行情。
 */
#include "../q04_uftcore3/q04_trader_fixture.h"

#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/WtUftDtMgr.h"
#include "WtUftCore/WtHelper.h"

#include "Includes/UftStrategyDefs.h"
#include "Includes/WTSDataDef.hpp"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace wtp;

namespace {

const char* kAg = "SHFE.ag2406";
const char* kAu = "SHFE.au2406";

// 纯计数策略替身: 所有回调不碰 ctx/行情, 只记录次数
struct R54UftStrategy : UftStrategy
{
	uint32_t initCnt = 0;
	uint32_t tickCnt = 0;
	uint32_t oqCnt = 0;
	uint32_t odCnt = 0;
	uint32_t transCnt = 0;
	uint32_t barCnt = 0;
	uint32_t tradeCnt = 0;
	uint32_t orderCnt = 0;
	uint32_t posCnt = 0;
	uint32_t readyCnt = 0;
	uint32_t lostCnt = 0;
	uint32_t entrustCnt = 0;
	uint32_t sessionBeginCnt = 0;
	uint32_t sessionEndCnt = 0;
	uint32_t paramsCnt = 0;
	uint32_t lastEntrustLocalid = 0;
	bool lastEntrustOk = false;

	explicit R54UftStrategy(const char* id) : UftStrategy(id) {}

	const char* getName() override { return "R54UftStrategy"; }
	const char* getFactName() override { return "R54Fact"; }

	void on_init(IUftStraCtx* /*ctx*/) override { initCnt++; }
	void on_tick(IUftStraCtx* /*ctx*/, const char* /*stdCode*/, WTSTickData* /*newTick*/) override { tickCnt++; }
	void on_order_queue(IUftStraCtx* /*ctx*/, const char* /*stdCode*/, WTSOrdQueData* /*newOrdQue*/) override { oqCnt++; }
	void on_order_detail(IUftStraCtx* /*ctx*/, const char* /*stdCode*/, WTSOrdDtlData* /*newOrdDtl*/) override { odCnt++; }
	void on_transaction(IUftStraCtx* /*ctx*/, const char* /*stdCode*/, WTSTransData* /*newTrans*/) override { transCnt++; }
	void on_bar(IUftStraCtx* /*ctx*/, const char* /*stdCode*/, const char* /*period*/,
		uint32_t /*times*/, WTSBarStruct* /*newBar*/) override { barCnt++; }
	void on_trade(IUftStraCtx* /*ctx*/, uint32_t /*localid*/, const char* /*stdCode*/, bool /*isLong*/,
		uint32_t /*offset*/, double /*vol*/, double /*price*/) override { tradeCnt++; }
	void on_order(IUftStraCtx* /*ctx*/, uint32_t /*localid*/, const char* /*stdCode*/, bool /*isLong*/,
		uint32_t /*offset*/, double /*totalQty*/, double /*leftQty*/, double /*price*/,
		bool /*isCanceled*/) override { orderCnt++; }
	void on_position(IUftStraCtx* /*ctx*/, const char* /*stdCode*/, bool /*isLong*/,
		double /*prevol*/, double /*preavail*/, double /*newvol*/, double /*newavail*/) override { posCnt++; }
	void on_channel_ready(IUftStraCtx* /*ctx*/) override { readyCnt++; }
	void on_channel_lost(IUftStraCtx* /*ctx*/) override { lostCnt++; }
	void on_entrust(uint32_t localid, bool bSuccess, const char* /*message*/) override
	{
		entrustCnt++;
		lastEntrustLocalid = localid;
		lastEntrustOk = bSuccess;
	}
	void on_session_begin(IUftStraCtx* /*ctx*/, uint32_t /*uTDate*/) override { sessionBeginCnt++; }
	void on_session_end(IUftStraCtx* /*ctx*/, uint32_t /*uTDate*/) override { sessionEndCnt++; }
	void on_params_updated() override { paramsCnt++; }
};

struct CtxEnv
{
	q04::TempRoot			tmp{ "r54_ctx" };
	q04::FakeTraderApi		api;
	q04::FakeBaseDataMgr	bd;
	ActionPolicyMgr			policy;
	TraderAdapter			adapter;
	WtUftDtMgr				dtMgr;
	WtUftEngine				engine;
	std::string				name;
	std::string				folder;
	UftStraContext			ctx;
	R54UftStrategy			stra;

	explicit CtxEnv(const char* tag)
		: name(std::string("r54ctx_") + tag + "_" + std::to_string((long)::getpid())
			+ "_" + std::to_string(++s_seq()))
		, ctx(&engine, name.c_str())
		, stra(name.c_str())
	{
		bd.add("SHFE", "ag", "ag2406", 0, 0, CM_OpenCover, TM_Both, 1);
		bd.add("SHFE", "au", "au2406", 0, 0, CM_OpenCover, TM_Both, 1);
		const std::string path = tmp.file("policy.json");
		tmp.write(path, q04::policyJsonOpenCloseLimited());
		EXPECT_TRUE(policy.init(path.c_str()));
		engine.init(nullptr, &bd, &dtMgr, nullptr);
		EXPECT_TRUE(adapter.initExt(name.c_str(), &api, &bd, &policy));
		EXPECT_TRUE(adapter.run());
		adapter.onLoginResult(true, "ok", 20240315);
		adapter.addSink(&ctx);
		ctx.setTrader(&adapter);
		ctx.set_strategy(&stra);

		folder = std::string(WtHelper::getOutputDir()) + name;
		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
	}

	~CtxEnv()
	{
		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
	}

	CtxEnv(const CtxEnv&) = delete;
	CtxEnv& operator=(const CtxEnv&) = delete;

	static std::atomic<uint32_t>& s_seq()
	{
		static std::atomic<uint32_t> seq(0);
		return seq;
	}

	void ready(uint32_t tdate = 20240315) { ctx.on_channel_ready(tdate); }

	std::string mannualFile() const { return folder + "/mannual.yaml"; }
};

WTSTickData* makeTick(const char* exchg, const char* code, double price, WTSContractInfo* cInfo)
{
	WTSTickStruct st;
	memset(&st, 0, sizeof(st));
	wt_strcpy(st.exchg, exchg);
	wt_strcpy(st.code, code);
	st.price = price;
	st.action_date = 20240315;
	st.action_time = 93000000;
	WTSTickData* ret = WTSTickData::create(st);
	ret->setContractInfo(cInfo);
	return ret;
}

}	// namespace

// UftStraContext.cpp:62-66   on_init 转发
// UftStraContext.cpp:69-97   on_tick 无持仓时只转发
// UftStraContext.cpp:99-121  队列/明细/成交/K线转发
// UftStraContext.cpp:596-600  on_order 非本单早退
// UftStraContext.cpp:631-656  on_channel_ready 无持仓时仍回调策略
// UftStraContext.cpp:659-696  断线/委托/session/params 回调
TEST(R54UftCtx, callbacks_forward_without_blocks)
{
	CtxEnv env("nocb");

	env.ctx.on_init();
	EXPECT_EQ(1u, env.stra.initCnt);

	WTSTickData* tick = makeTick("SHFE", "ag2406", 5000.0, nullptr);
	env.ctx.on_tick(kAg, tick);
	tick->release();
	EXPECT_EQ(1u, env.stra.tickCnt);

	env.ctx.on_order_queue(kAg, nullptr);
	env.ctx.on_order_detail(kAg, nullptr);
	env.ctx.on_transaction(kAg, nullptr);
	env.ctx.on_bar(kAg, "m1", 1, nullptr);
	EXPECT_EQ(1u, env.stra.oqCnt);
	EXPECT_EQ(1u, env.stra.odCnt);
	EXPECT_EQ(1u, env.stra.transCnt);
	EXPECT_EQ(1u, env.stra.barCnt);

	// tradingday 0 不触发 load_local_data, 但策略回调仍要发
	env.ready(0);
	EXPECT_EQ(1u, env.stra.readyCnt);
	EXPECT_EQ(0u, env.stra.posCnt);

	env.ctx.on_channel_lost();
	env.ctx.on_session_begin(20240315);
	env.ctx.on_session_end(20240315);
	env.ctx.on_params_updated();
	EXPECT_EQ(1u, env.stra.lostCnt);
	EXPECT_EQ(1u, env.stra.sessionBeginCnt);
	EXPECT_EQ(1u, env.stra.sessionEndCnt);
	EXPECT_EQ(1u, env.stra.paramsCnt);

	// 登记一张本策略单, 再推非本单/本单委托回报
	uint32_t id = env.ctx.stra_enter_long(kAg, 5000.0, 1.0, 0);
	ASSERT_NE(UINT_MAX, id);
	env.ctx.on_entrust(id + 1000, kAg, false, "not mine");
	EXPECT_EQ(0u, env.stra.entrustCnt);
	env.ctx.on_entrust(id, kAg, false, "rejected");
	EXPECT_EQ(1u, env.stra.entrustCnt);
	EXPECT_EQ(id, env.stra.lastEntrustLocalid);
	EXPECT_FALSE(env.stra.lastEntrustOk);

	// 非本单的成交/委托回报必须早退
	env.ctx.on_order(id + 1000, kAg, true, 0, 1.0, 1.0, 5000.0, false);
	env.ctx.on_trade(id + 1000, kAg, true, 0, 1.0, 5000.0);
	EXPECT_EQ(0u, env.stra.orderCnt);
	EXPECT_EQ(0u, env.stra.tradeCnt);
}

// UftStraContext.cpp:1031-1126 load_local_data 手工持仓导入:
// 检测/解析/合约缺失 continue/明细写入/文件改名
// UftStraContext.cpp:1128-1206 持仓块建立与明细读回 _positions
// UftStraContext.cpp:69-97 on_tick 多头/空头盯市
// UftStraContext.cpp:639-656 on_channel_ready 逐持仓通知策略
TEST(R54UftCtx, manual_import_and_tick_marks_profit)
{
	CtxEnv env("manual");
	std::error_code ec;
	std::filesystem::create_directories(env.folder, ec);
	ASSERT_TRUE(std::filesystem::is_directory(env.folder, ec));

	env.tmp.write(env.mannualFile(), R"YAML(
details:
  - exchg: SHFE
    code: ag2406
    direct: 0
    volume: 3
    openprice: 5000
  - exchg: SHFE
    code: au2406
    direct: 1
    volume: 2
    openprice: 5200
  - exchg: SHFE
    code: ag2406
    direct: 0
    volume: 0
    openprice: 0
  - exchg: DCE
    code: m2509
    direct: 0
    volume: 1
    openprice: 3000
)YAML");

	env.ready(20240315);

	EXPECT_DOUBLE_EQ(3.0, env.ctx.stra_get_local_position(kAg));
	EXPECT_DOUBLE_EQ(-2.0, env.ctx.stra_get_local_position(kAu));
	// 两条有效明细各通知一次, 未知合约被 continue
	EXPECT_EQ(2u, env.stra.posCnt);
	EXPECT_EQ(1u, env.stra.readyCnt);
	// 原文件被改名, 不会二次导入
	EXPECT_FALSE(std::filesystem::exists(env.mannualFile(), ec));

	WTSTickData* agTick = makeTick("SHFE", "ag2406", 5100.0,
		env.bd.getContract("ag2406", "SHFE"));
	env.ctx.on_tick(kAg, agTick);
	agTick->release();
	// (5100-5000)*3
	EXPECT_DOUBLE_EQ(300.0, env.ctx.stra_get_local_posprofit(kAg));

	WTSTickData* auTick = makeTick("SHFE", "au2406", 5100.0,
		env.bd.getContract("au2406", "SHFE"));
	env.ctx.on_tick(kAu, auTick);
	auTick->release();
	// (5100-5200)*2*(-1)
	EXPECT_DOUBLE_EQ(200.0, env.ctx.stra_get_local_posprofit(kAu));

	// 切交易日: 覆盖 position 块按 _date 清理/零量明细 continue/成交与回合块清空。
	// 内存净持仓是否会翻倍属 BUG-Q04-007, 这里不断言。
	env.ready(20240318);
	// _tradingday 归零: load_local_data 首行早退
	env.ready(0);
}

// UftStraContext.cpp:592-594  on_trade 转发策略
// UftStraContext.cpp:604-624  on_order 建块 + 撤单状态 + 策略转发
// UftStraContext.cpp:631-656 同日二次 ready 的幂等/持仓通知
TEST(R54UftCtx, order_trade_callbacks_with_blocks)
{
	CtxEnv env("blocks");
	env.ready(20240315);

	uint32_t id = env.ctx.stra_enter_long(kAg, 5000.0, 4.0, 0);
	ASSERT_NE(UINT_MAX, id);
	env.ctx.on_trade(id, kAg, true, 0, 4.0, 5000.0);
	EXPECT_DOUBLE_EQ(4.0, env.ctx.stra_get_local_position(kAg));
	EXPECT_EQ(1u, env.stra.tradeCnt);

	// 撤单状态写块 + 策略回调
	env.ctx.on_order(id, kAg, true, 0, 4.0, 4.0, 5000.0, true);
	EXPECT_EQ(1u, env.stra.orderCnt);
	// 非本单委托不回调
	env.ctx.on_order(id + 999, kAg, true, 0, 1.0, 1.0, 5000.0, false);
	EXPECT_EQ(1u, env.stra.orderCnt);

	// 平多 4 手: 收盘价 5100, 回合盈亏 (5100-5000)*4
	uint32_t id2 = env.ctx.stra_exit_long(kAg, 5100.0, 4.0, false, 0);
	ASSERT_NE(UINT_MAX, id2);
	env.ctx.on_trade(id2, kAg, true, 1, 4.0, 5100.0);
	EXPECT_DOUBLE_EQ(0.0, env.ctx.stra_get_local_position(kAg));
	EXPECT_DOUBLE_EQ(400.0, env.ctx.stra_get_local_closeprofit(kAg));
	EXPECT_EQ(2u, env.stra.tradeCnt);

	// 同一交易日二次 ready: 不再 load, 但策略 ready 回调照发
	env.ready(20240315);
	EXPECT_EQ(2u, env.stra.readyCnt);
}

// UftStraContext.cpp:155-172 买入平空时: valid_idx 递进/跳过非空头明细/left==0 提前退出
// UftStraContext.cpp:276-293 卖出平多时的对称边界
TEST(R54UftCtx, trade_detail_edge_branches)
{
	CtxEnv env("details");
	env.ready(20240315);

	auto openLong = [&](const char* code, double qty, double price) {
		uint32_t id = env.ctx.stra_enter_long(code, price, qty, 0);
		EXPECT_NE(UINT_MAX, id);
		env.ctx.on_trade(id, code, true, 0, qty, price);
	};
	auto closeLong = [&](const char* code, double qty, double price) {
		uint32_t id = env.ctx.stra_exit_long(code, price, qty, false, 0);
		EXPECT_NE(UINT_MAX, id);
		env.ctx.on_trade(id, code, true, 1, qty, price);
	};
	auto openShort = [&](const char* code, double qty, double price) {
		uint32_t id = env.ctx.stra_enter_short(code, price, qty, 0);
		EXPECT_NE(UINT_MAX, id);
		env.ctx.on_trade(id, code, false, 0, qty, price);
	};
	auto closeShort = [&](const char* code, double qty, double price) {
		uint32_t id = env.ctx.stra_exit_short(code, price, qty, false, 0);
		EXPECT_NE(UINT_MAX, id);
		env.ctx.on_trade(id, code, false, 1, qty, price);
	};

	// ag: 先开多再平掉, 留下 direct=0 的零量明细; 再开两笔空单
	openLong(kAg, 3.0, 5000.0);
	closeLong(kAg, 3.0, 5100.0);
	openShort(kAg, 1.0, 5200.0);
	openShort(kAg, 1.0, 5210.0);
	// 平 1 手空: 先跳过零量多头明细(166-169), 再在 left==0 时提前退出(171-172)
	closeShort(kAg, 1.0, 5150.0);
	EXPECT_DOUBLE_EQ(-1.0, env.ctx.stra_get_local_position(kAg));

	// 再开一笔空, 平 1 手: 连续零量明细把 _valid_idx 推到 2(159-162)
	openShort(kAg, 1.0, 5220.0);
	closeShort(kAg, 1.0, 5150.0);
	EXPECT_DOUBLE_EQ(-1.0, env.ctx.stra_get_local_position(kAg));

	// au: 对称地留下 zero-volume 空头明细, 走卖出平多的 280-293
	openShort(kAu, 3.0, 5200.0);
	closeShort(kAu, 3.0, 5150.0);
	openLong(kAu, 1.0, 5000.0);
	openLong(kAu, 1.0, 5010.0);
	closeLong(kAu, 1.0, 5050.0);
	EXPECT_DOUBLE_EQ(1.0, env.ctx.stra_get_local_position(kAu));

	openLong(kAu, 1.0, 5020.0);
	closeLong(kAu, 1.0, 5050.0);
	EXPECT_DOUBLE_EQ(1.0, env.ctx.stra_get_local_position(kAu));

	// ag 明细表里留下多笔零量明细: on_tick 的 ds->_volume==0 分支
	WTSTickData* tick = makeTick("SHFE", "ag2406", 5150.0,
		env.bd.getContract("ag2406", "SHFE"));
	env.ctx.on_tick(kAg, tick);
	tick->release();
	// 剩余 1 手空单开仓价 5220: 5150*(-1)+5220
	EXPECT_DOUBLE_EQ(70.0, env.ctx.stra_get_local_posprofit(kAg));
}

// UftStraContext.cpp:933-990 stra_get_bars/ticks/order_detail/order_queue/transaction/last_tick
// 在数据管理器未缓存时的 NULL 透传(含 m5 的 times 解析与 d 的日线分支)
TEST(R54UftCtx, slice_queries_are_null_before_cache)
{
	CtxEnv env("slices");

	EXPECT_EQ(nullptr, env.ctx.stra_get_bars(kAg, "m5", 10));
	EXPECT_EQ(nullptr, env.ctx.stra_get_bars(kAg, "d", 1));
	EXPECT_EQ(nullptr, env.ctx.stra_get_ticks(kAg, 10));
	EXPECT_EQ(nullptr, env.ctx.stra_get_order_detail(kAg, 10));
	EXPECT_EQ(nullptr, env.ctx.stra_get_order_queue(kAg, 10));
	EXPECT_EQ(nullptr, env.ctx.stra_get_transaction(kAg, 10));
	EXPECT_EQ(nullptr, env.ctx.stra_get_last_tick(kAg));
}

// UftStraContext.cpp:71-84 只要本地有持仓就无条件
// newTick->getContractInfo()->getCommInfo(); 而 ParserAdapter::handleQuote
// (ParserAdapter.cpp:256-263) 解析出 cInfo 后只 setCode, 不回写 setContractInfo,
// 实盘 tick 的 ContractInfo 为 NULL -> 策略有持仓时首笔行情必崩。
// BUG-R54-002, 整条 DISABLED。
TEST(DISABLED_R54UftCtx, tick_without_contract_info_crashes_with_local_position)
{
	CtxEnv env("ticknull");
	env.ready(20240315);

	uint32_t id = env.ctx.stra_enter_long(kAg, 5000.0, 4.0, 0);
	ASSERT_NE(UINT_MAX, id);
	env.ctx.on_trade(id, kAg, true, 0, 4.0, 5000.0);
	ASSERT_DOUBLE_EQ(4.0, env.ctx.stra_get_local_position(kAg));

	// 与解析侧一致: tick 没有 ContractInfo
	WTSTickData* tick = makeTick("SHFE", "ag2406", 5100.0, nullptr);
	env.ctx.on_tick(kAg, tick);
	tick->release();
}
