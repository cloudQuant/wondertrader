/*
 * R46 - 导出上下文的初始化/交易日生命周期补测
 *
 * 目标：
 *   src/WtPorter/ExpCtaContext.cpp  L48-58（on_init）、L66-73（on_session_begin）、
 *                                   L81-88（on_session_end）
 *   src/WtPorter/ExpSelContext.cpp  L48-55（on_init）、L62-69（on_session_begin）、
 *                                   L76-83（on_session_end）
 *   src/WtPorter/ExpHftContext.cpp  L29-44（on_bar）、L80-87（on_entrust）、
 *                                   L93-100（on_init）、L107-114（on_session_begin）、
 *                                   L121-128（on_session_end）、L141-148（on_order）、
 *                                   L161-165（on_position）、L173-188（on_tick）、
 *                                   L196-224（on_order_queue/detail/transaction）
 *
 * 与既有 lane 的分工：r13 只覆盖了纯转发段（on_bar_close/on_calculate/
 * on_condition_triggered/on_strategy_schedule 与未订阅 tick 丢弃），并把
 * on_init/on_session_end 整段 DISABLED（理由：要落文件）。本文件把
 * WtHelper::setGenerateDir 指到系统临时目录后驱动这三段，断言回调确实带
 * 上下文句柄与 isBegin 标记到达 runner。
 *
 * 不驱动的段：ExpHftContext::on_channel_ready/on_channel_lost 与
 * ExpHftContext::on_trade 都要解引用未绑定 _trader（创建上下文时 trader 必不存在），
 * 属已知崩溃路径（r13 BUG），本 lane 不重复制造。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "WtCore/WtHelper.h"
#include "WtPorter/ExpHftContext.h"
#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"

USING_NS_WTP;

extern WtRtRunner& getRunner();

namespace {

	const char* kGenLeaf = "wt_r46_porter_gen";

	struct Recorder
	{
		uint32_t	init = 0, tick = 0, calc = 0, bar = 0, sess = 0, cond = 0;
		uint32_t	chnl = 0, ord = 0, trd = 0, entrust = 0, pos = 0;
		uint32_t	ordque = 0, orddtl = 0, trans = 0;

		CtxHandler	last_handle = 0;
		WtUInt32	last_tdate = 0;
		bool		last_isbegin = false;
		std::string	last_code, last_period;

		void clear() { *this = Recorder(); }
	};

	Recorder g;

	void cb_init(CtxHandler c) { g.init++; g.last_handle = c; }
	void cb_tick(CtxHandler c, const char* code, WTSTickStruct*) { g.tick++; g.last_handle = c; g.last_code = code; }
	void cb_calc(CtxHandler c, WtUInt32, WtUInt32) { g.calc++; g.last_handle = c; }
	void cb_bar(CtxHandler c, const char* code, const char* period, WTSBarStruct*)
	{
		g.bar++; g.last_handle = c; g.last_code = code; g.last_period = period;
	}
	void cb_sess(CtxHandler c, WtUInt32 tdate, bool isBegin)
	{
		g.sess++; g.last_handle = c; g.last_tdate = tdate; g.last_isbegin = isBegin;
	}
	void cb_cond(CtxHandler c, const char*, double, double, const char*) { g.cond++; g.last_handle = c; }
	void cb_chnl(CtxHandler c, const char*, WtUInt32) { g.chnl++; g.last_handle = c; }
	void cb_ord(CtxHandler c, WtUInt32, const char*, bool, double, double, double, bool, const char*)
	{
		g.ord++; g.last_handle = c;
	}
	void cb_trd(CtxHandler c, WtUInt32, const char*, bool, double, double, const char*) { g.trd++; g.last_handle = c; }
	void cb_entrust(CtxHandler c, WtUInt32, const char*, bool, const char*, const char*) { g.entrust++; g.last_handle = c; }
	void cb_pos(CtxHandler c, const char*, bool, double, double, double, double) { g.pos++; g.last_handle = c; }
	void cb_ordque(CtxHandler c, const char*, WTSOrdQueStruct*) { g.ordque++; g.last_handle = c; }
	void cb_orddtl(CtxHandler c, const char*, WTSOrdDtlStruct*) { g.orddtl++; g.last_handle = c; }
	void cb_trans(CtxHandler c, const char*, WTSTransStruct*) { g.trans++; g.last_handle = c; }

	void wire_all()
	{
		WtRtRunner& r = getRunner();
		r.registerCtaCallbacks(cb_init, cb_tick, cb_calc, cb_bar, cb_sess, cb_cond);
		r.registerSelCallbacks(cb_init, cb_tick, cb_calc, cb_bar, cb_sess);
		r.registerHftCallbacks(cb_init, cb_tick, cb_bar, cb_chnl, cb_ord, cb_trd, cb_entrust,
			cb_orddtl, cb_ordque, cb_trans, cb_sess, cb_pos);
	}

	void unwire_all()
	{
		WtRtRunner& r = getRunner();
		r.registerCtaCallbacks(NULL, NULL, NULL, NULL, NULL, NULL);
		r.registerSelCallbacks(NULL, NULL, NULL, NULL, NULL);
		r.registerHftCallbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
	}

} // namespace

class R46PorterCtx : public ::testing::Test
{
protected:
	std::string _genDir;

	void SetUp() override
	{
		std::filesystem::path p = std::filesystem::temp_directory_path() / kGenLeaf;
		std::error_code ec;
		std::filesystem::create_directories(p, ec);
		_genDir = p.string();
		WtHelper::setGenerateDir(_genDir.c_str());

		g.clear();
		wire_all();
	}

	void TearDown() override
	{
		unwire_all();
		g.clear();
	}
};

/* ExpCtaContext.cpp:48-58 / 66-73 / 81-88：基类初始化和 dump_chart_info 都会落盘，
 * 目录由 WtHelper::setGenerateDir 指到临时目录。 */
TEST_F(R46PorterCtx, CtaInitAndSessionEventsReachRunnerCallbacks)
{
	CtxHandler id = getRunner().createCtaContext("r46_ctx_cta", 0);
	ASSERT_NE(id, 0u) << "createCtaContext 返回 0 句柄";
	CtaContextPtr ctx = getRunner().getCtaContext(id);
	ASSERT_TRUE(ctx != nullptr);

	ctx->on_init();
	EXPECT_EQ(g.init, 1u);
	EXPECT_EQ(g.last_handle, id);

	ctx->on_session_begin(20240102);
	EXPECT_EQ(g.sess, 1u);
	EXPECT_EQ(g.last_handle, id);
	EXPECT_EQ(g.last_tdate, 20240102u);
	EXPECT_TRUE(g.last_isbegin);

	ctx->on_session_end(20240103);
	EXPECT_EQ(g.sess, 2u);
	EXPECT_EQ(g.last_tdate, 20240103u);
	EXPECT_FALSE(g.last_isbegin);
}

/* ExpSelContext.cpp:48-55 / 62-69 / 76-83：与 CTA 同形，但 sel 槽位独立。 */
TEST_F(R46PorterCtx, SelInitAndSessionEventsReachRunnerCallbacks)
{
	CtxHandler id = getRunner().createSelContext("r46_ctx_sel", 20240102, 93000, "d", 0);
	ASSERT_NE(id, 0u);
	SelContextPtr ctx = getRunner().getSelContext(id);
	ASSERT_TRUE(ctx != nullptr);

	ctx->on_init();
	EXPECT_EQ(g.init, 1u);
	EXPECT_EQ(g.last_handle, id);

	ctx->on_session_begin(20240105);
	EXPECT_EQ(g.sess, 1u);
	EXPECT_EQ(g.last_tdate, 20240105u);
	EXPECT_TRUE(g.last_isbegin);

	ctx->on_session_end(20240105);
	EXPECT_EQ(g.sess, 2u);
	EXPECT_FALSE(g.last_isbegin);
}

/* ExpHftContext.cpp:29-33 的空 K 线守卫：newBar==NULL 时必须在转发之前 return。
 * 同时确认空 bar 不会消费掉周期串、也不会触发 runner 的 bar 回调。 */
TEST_F(R46PorterCtx, HftNullBarIsDroppedBeforeForwarding)
{
	CtxHandler id = getRunner().createHftContext("r46_ctx_hft_null", "R46_TRADER_ABSENT", false, 0);
	ASSERT_NE(id, 0u);
	HftContextPtr ctxBase = getRunner().getHftContext(id);
	ASSERT_TRUE(ctxBase != nullptr);
	ExpHftContext* ctx = dynamic_cast<ExpHftContext*>(ctxBase.get());
	ASSERT_NE(ctx, nullptr);

	ctx->on_bar("SHFE.rb.2405", "m1", 1, NULL);
	EXPECT_EQ(g.bar, 0u) << "空 K 线不该转发";
	EXPECT_TRUE(g.last_period.empty());

	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	ctx->on_bar("SHFE.rb.2405", "d1", 1, &bar);
	EXPECT_EQ(g.bar, 1u);
	EXPECT_EQ(g.last_period, "d11");
}

/* ExpHftContext.cpp:93-100 / 107-114 / 121-128 / 29-44 / 80-87 / 141-148 /
 * 161-165 / 173-188 / 196-224。bAgent=false，基类的 trades/closes 日志不落盘；
 * trader 故意给不存在的 id，走 WtRtRunner.cpp:333 的告警支，_trader 保持 NULL，
 * 因此只调用不解引用 _trader 的入口。 */
TEST_F(R46PorterCtx, HftInitSessionAndMicroStructureForwardWithoutTrader)
{
	CtxHandler id = getRunner().createHftContext("r46_ctx_hft", "R46_TRADER_ABSENT", false, 0);
	ASSERT_NE(id, 0u);
	HftContextPtr ctxBase = getRunner().getHftContext(id);
	ASSERT_TRUE(ctxBase != nullptr);
	ExpHftContext* ctx = dynamic_cast<ExpHftContext*>(ctxBase.get());
	ASSERT_NE(ctx, nullptr) << "createHftContext 应创建 ExpHftContext";

	ctx->on_init();
	EXPECT_EQ(g.init, 1u);
	EXPECT_EQ(g.last_handle, id);

	ctx->on_session_begin(20240102);
	ctx->on_session_end(20240102);
	EXPECT_EQ(g.sess, 2u);

	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	wt_strcpy(ts.code, "SHFE.rb.2405");
	ts.action_date = 20240102;
	ts.action_time = 93000000;
	ts.price = 100.5;
	WTSTickData* tick = WTSTickData::create(ts);
	ASSERT_NE(tick, nullptr);
	ctx->on_tick("SHFE.rb.2405", tick);
	EXPECT_EQ(g.tick, 0u) << "未订阅的合约不该转发 tick";
	tick->release();

	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	ctx->on_bar("SHFE.rb.2405", "m1", 5, &bar);
	EXPECT_EQ(g.bar, 1u);
	EXPECT_EQ(g.last_code, "SHFE.rb.2405");
	EXPECT_EQ(g.last_period, "m15");

	ctx->on_order(1001, "SHFE.rb.2405", true, 2, 2, 100.5, false);
	EXPECT_EQ(g.ord, 1u);

	ctx->on_entrust(1001, "SHFE.rb.2405", true, "accepted");
	EXPECT_EQ(g.entrust, 1u);

	ctx->on_position("SHFE.rb.2405", true, 1, 1, 2, 2, 20240102);
	EXPECT_EQ(g.pos, 1u);

	WTSOrdQueData* oq = WTSOrdQueData::create("SHFE.rb.2405");
	ASSERT_NE(oq, nullptr);
	ctx->on_order_queue("SHFE.rb.2405", oq);
	EXPECT_EQ(g.ordque, 1u);
	oq->release();

	WTSOrdDtlData* od = WTSOrdDtlData::create("SHFE.rb.2405");
	ASSERT_NE(od, nullptr);
	ctx->on_order_detail("SHFE.rb.2405", od);
	EXPECT_EQ(g.orddtl, 1u);
	od->release();

	WTSTransData* tr = WTSTransData::create("SHFE.rb.2405");
	ASSERT_NE(tr, nullptr);
	ctx->on_transaction("SHFE.rb.2405", tr);
	EXPECT_EQ(g.trans, 1u);
	tr->release();
}
