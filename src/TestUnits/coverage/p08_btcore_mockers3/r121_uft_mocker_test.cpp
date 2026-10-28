/**
 * r121_uft_mocker_test.cpp  (lane R121)
 *
 * 靶子：src/WtBtCore/UftMocker.cpp
 *   - init_uft_factory 的插件装载成功段（L154-158/160-161/166-167）；
 *   - 无策略时 on_init/on_channel_ready 的守卫（L337-341、L822-826）；
 *   - 策略插件的事件分发：on_bar/on_tick/on_order/on_entrust/on_trade
 *     （L225、L296、L804、L813、L819）、session 冻结释放与日志（L206-213、
 *     L344-390）、析构删策略（L62-64）；
 *   - on_tick 的非 match_this_tick 订单排空分支（L269-280）与左右全成
 *     返回 true（L969）；
 *   - stra_cancel 的平今 offset==2 释放今仓（L422-432）；
 *   - update_position 未注册品种早退（L1170-1172）。
 *
 * R111 的 Uft 用例在 stra_enum_position/on_channel_ready 等依赖 _ctx 的策略回调处
 * 崩溃；本文件不调用 stra_enum_position、on_channel_ready，也不调用会取 K 线的
 * handle_init（策略插件用例），避开这些崩溃点。不联网、不起线程。
 */
#include <gtest/gtest.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <unistd.h>

namespace { void r135_segv(int) { void* bt[40]; int n = backtrace(bt, 40); backtrace_symbols_fd(bt, n, 2); _exit(99); } }
#include <filesystem>
#include <string>

#include <Includes/WTSContractInfo.hpp>
#include <WtBtCore/UftMocker.h>
#include <WtBtCore/WtHelper.h>

#include "r121_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
	const char* const kCu = "SHFE.cu.2406";
	const char* const kStk = "SSE.STK.000001";

	WTSVariant* make_uft_stra_cfg(const std::string& module, const char* code)
	{
		WTSVariant* cfg = r121::make_module_cfg(module);

		WTSVariant* stra = WTSVariant::createObject();
		stra->append("name", "SimpleUft");
		stra->append("id", "r121_uft_plug");

		WTSVariant* params = WTSVariant::createObject();
		params->append("code", code);
		params->append("second", static_cast<uint32_t>(10));
		params->append("freq", static_cast<uint32_t>(0));
		params->append("offset", static_cast<uint32_t>(0));
		params->append("lots", 1.0);
		stra->append("params", params);
		params->release();

		cfg->append("strategy", stra);
		stra->release();
		return cfg;
	}
}

class R121UftMocker : public r62::ReplayFixture
{
protected:
	std::string module() const
	{
		return r111::plugin_path("WtUftStraFact", "WtUftRunner");
	}
};

// 无 strategy 配置：插件工厂装载成功即可，后续回调全部走空守卫
TEST_F(R121UftMocker, FactoryWithoutStrategy)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	fprintf(stderr, "U1 ctor\n");
	UftMocker m(&_rp, "r121_uft_factory");
	WTSVariant* cfg = r121::make_module_cfg(module());
	fprintf(stderr, "U2 init\n");
	ASSERT_TRUE(m.init_uft_factory(cfg));
	cfg->release();
	fprintf(stderr, "U3 handle_init\n");

	// on_init/on_channel_ready 的 _strategy 空守卫（L339、L824）
	m.handle_init();
	fprintf(stderr, "U4 on_trade\n");

	// 未注册品种成交：update_position 在 commInfo==NULL 处早退（L1170-1172）
	m.on_trade(1u, "SHFE.xx.9999", true, 0u, 1.0, 100.0);
	fprintf(stderr, "U5 pos\n");
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_position("SHFE.xx.9999"));
	fprintf(stderr, "U6 replaydone\n");

	m.handle_replay_done();
	fprintf(stderr, "U7 end\n");
}

// 真实 SimpleUft 插件：工厂创建、成交回写、tick/bar/回报分发、会话冻结释放
TEST_F(R121UftMocker, PluginStrategyLifecycle)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	WTSCommodityInfo* stkComm = _rp.get_commodity_info(kStk);
	ASSERT_NE(nullptr, stkComm);
	stkComm->setTradingMode(TM_LongT1);

	fprintf(stderr, "V1 ctor\n");
	UftMocker m(&_rp, "r121_uft_plugin");
	WTSVariant* cfg = make_uft_stra_cfg(module(), kRb);
	fprintf(stderr, "V2 init\n");
	ASSERT_TRUE(m.init_uft_factory(cfg));
	cfg->release();
	fprintf(stderr, "V3 on_trade\n");

	// T+1 开 100 股：总持仓 100、可用 0；策略 on_trade 空实现（L813）
	m.on_trade(1u, kStk, true, 0u, 100.0, 10.0);
	fprintf(stderr, "V4 pos\n");
	EXPECT_DOUBLE_EQ(100.0, m.stra_get_position(kStk, false));
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_position(kStk, true));
	fprintf(stderr, "V5 tick\n");

	// tick/bar 分发（L296、L225）：策略只盯 kRb，kCu 直接返回
	m.stra_sub_ticks(kCu);
	WTSTickData* tick = r62::make_tick(kCu, 20200102u, 93100000u, 70000.0, 69990.0, 70010.0);
	m.handle_tick(kCu, tick, 0);
	tick->release();
	fprintf(stderr, "V6 bar\n");
	WTSBarStruct bar = r62::make_bar(20200102u, 1500u, 10.0);
	m.handle_bar_close(kCu, "m1", 1, &bar);
	fprintf(stderr, "V7 reports\n");

	// 未在策略内部订单表中的回报 id：仅验证转发（L804、L819）
	m.on_entrust(999u, kCu, true, "ok");
	m.on_order(999u, kCu, true, 0u, 1.0, 1.0, 70000.0, false);
	fprintf(stderr, "V8 session_begin\n");

	// 跨日释放冻结（L350-358）+ session 包装（L206-213）
	m.handle_session_begin(20200103u);
	fprintf(stderr, "V9 after session_begin\n");
	EXPECT_DOUBLE_EQ(100.0, m.stra_get_position(kStk, true));
	fprintf(stderr, "V10 session_end\n");

	// 会话结束写持仓/资金日志（L364-390、L211-213）
	m.handle_session_end(20200103u);
	fprintf(stderr, "V11 replaydone\n");
	m.handle_replay_done();
	fprintf(stderr, "V12 end\n");
}

// 纯内存 tick 回放：订单在 _replayer->get_last_tick 上成交 / 撤单任务排空
class R121UftOrderFixture : public ::testing::Test
{
protected:
	HisDataReplayer _rp;
	r111::TickLoader _loader;
	r121::NullSink _sink;
	std::filesystem::path _root;
	std::filesystem::path _base;
	std::filesystem::path _out;

	void SetUp() override
	{
		_root = r62::repo_root();
		ASSERT_FALSE(_root.empty());
		_base = r62::make_temp_dir("r121_uft_base");
		_out = r62::make_temp_dir("r121_uft_out");
		WtHelper::setOutputDir(_out.string().c_str());

		_loader.add(kRb, 20200102u, 93100000u, 3800.0, 3799.0, 3801.0, 100, 100);
		WTSVariant* cfg = r111::make_tick_cfg(_root, _base,
			202001020932ull, 202001021500ull);
		ASSERT_TRUE(_rp.init(cfg, nullptr, &_loader));
		cfg->release();
		_rp.register_sink(&_sink, "r121_uft");
		ASSERT_TRUE(_rp.prepare());
	}

	void TearDown() override
	{
		WtHelper::setOutputDir("");
		std::error_code ec;
		std::filesystem::remove_all(_base, ec);
		std::filesystem::remove_all(_out, ec);
	}

	void feed(UftMocker& m, const char* code, double last, double bid, double ask,
		uint32_t bidQty = 100, uint32_t askQty = 100)
	{
		WTSTickData* t = r62::make_tick(code, 20200102u, 93100000u, last, bid, ask, bidQty, askQty);
		m.handle_tick(code, t, 0);
		t->release();
	}
};

// 默认 match_this_tick=false：先撮合订单再回调；1 手全成后 order 被 erase
TEST_F(R121UftOrderFixture, FillOrderOnLastTickAndErase)
{
	UftMocker m(&_rp, "r121_uft_fill");

	OrderIDs ids = m.stra_buy(kRb, 0.0, 1.0);
	ASSERT_EQ(1u, ids.size());
	EXPECT_DOUBLE_EQ(1.0, m.stra_get_undone(kRb));

	feed(m, kRb, 3800.0, 3799.0, 3801.0);
	EXPECT_DOUBLE_EQ(1.0, m.stra_get_position(kRb, false, 1));
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_undone(kRb));
}

// 平今单（offset=2）撤单：CM_CoverToday 下把 left 加回今仓可用（L431-432）
TEST_F(R121UftOrderFixture, CancelTodayOrderRestoresNewAvail)
{
	WTSCommodityInfo* comm = _rp.get_commodity_info(kRb);
	ASSERT_NE(nullptr, comm);
	comm->setCoverMode(CM_CoverToday);

	UftMocker m(&_rp, "r121_uft_cancel");
	m.on_trade(1u, kRb, true, 0u, 100.0, 3800.0);
	EXPECT_DOUBLE_EQ(100.0, m.stra_get_position(kRb, true, 1));

	uint32_t lid = m.stra_exit_long(kRb, 3900.0, 40.0, true);
	ASSERT_NE(0u, lid);
	EXPECT_DOUBLE_EQ(60.0, m.stra_get_position(kRb, true, 1));

	EXPECT_TRUE(m.stra_cancel(lid));
	// 卖限价 3900 高于买一 3799：先撮合时不会成交，随后撤单任务把今仓可用还原
	feed(m, kRb, 3800.0, 3799.0, 3801.0);
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_undone(kRb));
	EXPECT_DOUBLE_EQ(100.0, m.stra_get_position(kRb, true, 1));

	// 未注册品种成交只早退，不建立持仓
	m.on_trade(2u, "SHFE.xx.9999", true, 0u, 1.0, 100.0);
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_position("SHFE.xx.9999"));
}

TEST_F(R121UftMocker, R135ProbeMinimalDtor)
{
	{
		UftMocker m(&_rp, "r135_probe1");
		WTSVariant* cfg = r121::make_module_cfg(module());
		ASSERT_TRUE(m.init_uft_factory(cfg));
		cfg->release();
		fprintf(stderr, "PROBE1 factory only, leaving scope\n");
	}
	fprintf(stderr, "PROBE1 survived\n");
}

TEST_F(R121UftMocker, R135ProbeStrategyNoCalls)
{
	{
		UftMocker m(&_rp, "r135_probe2");
		WTSVariant* cfg = make_uft_stra_cfg(module(), kRb);
		ASSERT_TRUE(m.init_uft_factory(cfg));
		cfg->release();
		fprintf(stderr, "PROBE2 strategy created, leaving scope\n");
	}
	fprintf(stderr, "PROBE2 survived\n");
}

TEST_F(R121UftMocker, R135ProbeStrategyTrade)
{
	{
		UftMocker m(&_rp, "r135_probe3");
		WTSVariant* cfg = make_uft_stra_cfg(module(), kRb);
		ASSERT_TRUE(m.init_uft_factory(cfg));
		cfg->release();
		m.on_trade(1u, kRb, true, 0u, 1.0, 3800.0);
		fprintf(stderr, "PROBE3 trade done, leaving scope\n");
	}
	fprintf(stderr, "PROBE3 survived\n");
}

TEST_F(R121UftMocker, R135ProbeDirectFactory)
{
	signal(SIGSEGV, r135_segv);
	const std::string path = module();
	fprintf(stderr, "PROBE4 load %s\n", path.c_str());
	DllHandle h = DLLHelper::load_library(path.c_str());
	ASSERT_NE(nullptr, h);
	FuncCreateUftStraFact creator = (FuncCreateUftStraFact)DLLHelper::get_symbol(h, "createStrategyFact");
	FuncDeleteUftStraFact remover = (FuncDeleteUftStraFact)DLLHelper::get_symbol(h, "deleteStrategyFact");
	fprintf(stderr, "PROBE4 creator=%p remover=%p\n", (void*)creator, (void*)remover);
	ASSERT_NE(nullptr, creator);
	ASSERT_NE(nullptr, remover);
	IUftStrategyFact* fact = creator();
	fprintf(stderr, "PROBE4 fact=%p\n", (void*)fact);
	void** vt = *(void***)fact;
	for (int i = 0; i < 8; i++)
		fprintf(stderr, "PROBE4 vt[%d]=%p\n", i, vt[i]);
	fprintf(stderr, "PROBE4 name=%s\n", fact->getName());
	typedef void (*DelByValue)(IUftStrategyFact*);
	DelByValue dv = reinterpret_cast<DelByValue>(reinterpret_cast<void*>(remover));
	dv(fact);
	fprintf(stderr, "PROBE4 deleted ok\n");
}
