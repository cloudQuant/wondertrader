/*
 * r124_porter_expctx_test.cpp  (lane R124 · wave20)
 *
 * 靶子：WtPorter 导出上下文在「已配置引擎 + 已订阅」时的剩余转发行
 *   - ExpCtaContext.cpp:97-106  订阅后 on_tick_updated 的 ctx_on_tick(ET_CTA)
 *   - ExpSelContext.cpp:117-126 订阅后 on_tick_updated 的 ctx_on_tick(ET_SEL)
 *   - ExpHftContext.cpp:173-188 订阅后 on_tick 的 ctx_on_tick(ET_HFT)
 *   - ExpHftContext.cpp:235-242 on_trade 的 hft_on_trade + 基类持仓更新
 *
 * 与既有 lane 的分工：R13 只覆盖了「未订阅 -> 丢弃」的早退支；
 * R46 只覆盖未订阅 tick 与不触及 _trader 的回调。本文件用堆上 runner
 * （规避 BUG-R46-001 的空 _config/_engine 析构）先完成 init + config，
 * 使 _hot_mgr/_bd_mgr 可用，再 stra_sub_ticks 后驱动订阅支。
 *
 * 边界：只走公开 API；不开线程、不跑回测；临时文件仅写系统临时目录并清理。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "WtCore/WtHelper.h"
#include "WtPorter/ExpHftContext.h"
#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"

USING_NS_WTP;

extern WtRtRunner& getRunner();

namespace
{
	namespace stdfs = std::filesystem;

	struct Rec
	{
		uint32_t	cta_tick = 0;
		uint32_t	sel_tick = 0;
		uint32_t	hft_tick = 0;
		uint32_t	hft_trd = 0;
		CtxHandler	last_handle = 0;
		WtUInt32	last_localid = 0;
		std::string	last_code;
		bool		last_isbuy = false;
		double		last_vol = 0.0;
		double		last_price = 0.0;

		void clear() { *this = Rec(); }
	};

	Rec g;

	void cta_tick_cb(CtxHandler h, const char* code, WTSTickStruct*)
	{
		g.cta_tick++;
		g.last_handle = h;
		g.last_code = code ? code : "";
	}

	void sel_tick_cb(CtxHandler h, const char* code, WTSTickStruct*)
	{
		g.sel_tick++;
		g.last_handle = h;
		g.last_code = code ? code : "";
	}

	void hft_tick_cb(CtxHandler h, const char* code, WTSTickStruct*)
	{
		g.hft_tick++;
		g.last_handle = h;
		g.last_code = code ? code : "";
	}

	void hft_trd_cb(CtxHandler h, WtUInt32 localid, const char* code, bool isBuy,
		double vol, double price, const char*)
	{
		g.hft_trd++;
		g.last_handle = h;
		g.last_localid = localid;
		g.last_code = code ? code : "";
		g.last_isbuy = isBuy;
		g.last_vol = vol;
		g.last_price = price;
	}

	std::string prepare_dir(const char* tag)
	{
		stdfs::path p = stdfs::temp_directory_path()
			/ (std::string("r124_porter_") + tag + "_" + std::to_string((long)::getpid()));
		std::error_code ec;
		stdfs::remove_all(p, ec);
		stdfs::create_directories(p, ec);
		return p.string();
	}

	void write_text(const std::string& path, const std::string& body)
	{
		std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
		ofs << body;
	}

	WtRtRunner* new_runner(const std::string& dir)
	{
		/* 堆上构造、永不 delete：WtRtRunner 构造列表漏初始化 _config/_engine，
		 * 栈对象析构可能踩空指针（BUG-R46-001）。 */
		WtRtRunner* r = new WtRtRunner();
		r->init((dir + "/r124_absent_logcfg.yaml").c_str(), true, dir.c_str());
		return r;
	}

	std::string simple_cfg(const std::string& dir, const char* env)
	{
		return "{\"basefiles\":{},\"env\":{\"name\":\"" + std::string(env) + "\"},"
			"\"data\":{},\"bspolicy\":\"" + dir + "/actions.json\"}";
	}

	const char* kSessions =
		"{\"TRADING\":{\"name\":\"Trading\",\"offset\":0,"
		"\"sections\":[{\"from\":0,\"to\":2359}]}}";

	const char* kCommodities =
		"{\"SHFE\":{\"rb\":{\"name\":\"Rebar\",\"session\":\"TRADING\",\"pricetick\":1,"
		"\"volscale\":10,\"covermode\":0,\"pricemode\":0,\"trademode\":0,\"category\":1}}}";

	const char* kContracts =
		"{\"SHFE\":{\"rb2510\":{\"name\":\"rb2510\",\"product\":\"rb\",\"exchg\":\"SHFE\"}}}";

	std::string hft_cfg(const std::string& dir)
	{
		return "{\"basefiles\":{"
			"\"session\":\"" + dir + "/sessions.json\","
			"\"commodity\":\"" + dir + "/commodities.json\","
			"\"contract\":\"" + dir + "/contracts.json\"},"
			"\"env\":{\"name\":\"hft\"},\"data\":{},"
			"\"bspolicy\":\"" + dir + "/actions.json\"}";
	}

	WTSTickData* make_tick(const char* code)
	{
		WTSTickStruct ts;
		memset(&ts, 0, sizeof(ts));
		wt_strcpy(ts.code, code);
		ts.action_date = 20240102;
		ts.action_time = 93000000;
		ts.price = 4500.0;
		return WTSTickData::create(ts);
	}
}

/*
 * 生产: ExpCtaContext.cpp:97-106（订阅 -> ctx_on_tick(ET_CTA)）
 */
TEST(R124PorterExpCta, SubscribedTickIsForwarded)
{
	const std::string dir = prepare_dir("cta");
	write_text(dir + "/actions.json", "{}");

	g.clear();
	register_cta_callbacks(NULL, cta_tick_cb, NULL, NULL, NULL, NULL);

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);
	ASSERT_TRUE(r->config(simple_cfg(dir, "cta").c_str(), false));

	const uint32_t id = r->createCtaContext("r124_cta_sub", 0);
	ASSERT_NE(id, 0U);
	CtaContextPtr ctx = r->getCtaContext(id);
	ASSERT_TRUE(ctx != nullptr);

	const char* code = "SHFE.rb.2510";
	ctx->stra_sub_ticks(code);
	WTSTickData* tick = make_tick(code);
	ASSERT_NE(tick, nullptr);
	ctx->on_tick_updated(code, tick);
	tick->release();

	EXPECT_EQ(1U, g.cta_tick);
	EXPECT_EQ(id, g.last_handle);
	EXPECT_EQ(std::string(code), g.last_code);

	register_cta_callbacks(NULL, NULL, NULL, NULL, NULL, NULL);
	std::error_code ec;
	stdfs::remove_all(dir, ec);
}

/*
 * 生产: ExpSelContext.cpp:117-126（订阅 -> ctx_on_tick(ET_SEL)）
 */
TEST(R124PorterExpSel, SubscribedTickIsForwarded)
{
	const std::string dir = prepare_dir("sel");
	write_text(dir + "/actions.json", "{}");

	g.clear();
	register_sel_callbacks(NULL, sel_tick_cb, NULL, NULL, NULL);

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);
	ASSERT_TRUE(r->config(simple_cfg(dir, "sel").c_str(), false));

	const uint32_t id = r->createSelContext("r124_sel_sub", 20240102, 93000, "d",
		0, "CHINA", "TRADING");
	ASSERT_NE(id, 0U);
	SelContextPtr ctx = r->getSelContext(id);
	ASSERT_TRUE(ctx != nullptr);

	const char* code = "SHFE.rb.2510";
	ctx->stra_sub_ticks(code);
	WTSTickData* tick = make_tick(code);
	ASSERT_NE(tick, nullptr);
	ctx->on_tick_updated(code, tick);
	tick->release();

	EXPECT_EQ(1U, g.sel_tick);
	EXPECT_EQ(id, g.last_handle);
	EXPECT_EQ(std::string(code), g.last_code);

	register_sel_callbacks(NULL, NULL, NULL, NULL, NULL);
	std::error_code ec;
	stdfs::remove_all(dir, ec);
}

/*
 * 生产: ExpHftContext.cpp:173-188（订阅 -> ctx_on_tick(ET_HFT)）
 *       ExpHftContext.cpp:235-242（hft_on_trade + 基类 do_set_position）
 */
TEST(R124PorterExpHft, SubscribedTickAndTradeAreForwarded)
{
	const std::string dir = prepare_dir("hft");
	write_text(dir + "/actions.json", "{}");
	write_text(dir + "/sessions.json", kSessions);
	write_text(dir + "/commodities.json", kCommodities);
	write_text(dir + "/contracts.json", kContracts);

	g.clear();
	register_hft_callbacks(NULL, hft_tick_cb, NULL, NULL, NULL, hft_trd_cb, NULL,
		NULL, NULL, NULL, NULL, NULL);

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);
	ASSERT_TRUE(r->config(hft_cfg(dir).c_str(), false));

	const uint32_t id = r->createHftContext("r124_hft_sub", "R124_TRADER_ABSENT", false, 0);
	ASSERT_NE(id, 0U);
	HftContextPtr base = r->getHftContext(id);
	ASSERT_TRUE(base != nullptr);
	ExpHftContext* ctx = dynamic_cast<ExpHftContext*>(base.get());
	ASSERT_NE(ctx, nullptr);

	const char* code = "SHFE.rb.2510";
	ctx->stra_sub_ticks(code);
	WTSTickData* tick = make_tick(code);
	ASSERT_NE(tick, nullptr);
	ctx->on_tick(code, tick);
	tick->release();

	EXPECT_EQ(1U, g.hft_tick);
	EXPECT_EQ(id, g.last_handle);
	EXPECT_EQ(std::string(code), g.last_code);

	ctx->on_trade(9001, code, true, 2.0, 4500.0);
	EXPECT_EQ(1U, g.hft_trd);
	EXPECT_EQ(id, g.last_handle);
	EXPECT_EQ(9001U, g.last_localid);
	EXPECT_EQ(std::string(code), g.last_code);
	EXPECT_TRUE(g.last_isbuy);
	EXPECT_DOUBLE_EQ(2.0, g.last_vol);
	EXPECT_DOUBLE_EQ(4500.0, g.last_price);
	/* 基类 on_trade 必须真的执行到 do_set_position */
	EXPECT_DOUBLE_EQ(2.0, ctx->stra_get_position(code));

	register_hft_callbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL);
	std::error_code ec;
	stdfs::remove_all(dir, ec);
}
