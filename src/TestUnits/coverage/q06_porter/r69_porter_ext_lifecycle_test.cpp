/*
 * R69 - WtPorter 外部扩展入口 + ExpHftContext 通道事件（绑定真实 TraderAdapter 后）
 *
 * 目标：
 *   src/WtPorter/WtPorter.cpp:163-166    create_ext_executer 导出转发
 *   src/WtPorter/WtRtRunner.cpp:305-312  createExtExecuter（无 _engine 依赖）
 *   src/WtPorter/ExpHftContext.cpp:50-57 on_channel_lost（_trader->id()）
 *   src/WtPorter/ExpHftContext.cpp:63-70 on_channel_ready（_trader->id()）
 *   src/WtPorter/WtRtRunner.cpp:321-336  createHftContext 绑定已存在 trader 支
 *
 * 与既有 lane 的分工：R6 只从 WtRtRunner 方法层取过 createExtExecuter，没走
 * WtPorter.cpp 的 C 导出；R46 的 HFT 上下文故意给不存在的 trader（_trader=NULL），
 * 因而 on_channel_ready/lost 整段 DISABLED。本文件用一个**堆上**配置过的 runner
 * 建出「_trader 非空」的 ExpHftContext，再驱动这两个事件。
 *
 * ExpHftContext 的事件回调硬绑进程单例 getRunner()（ExpHftContext.cpp:53/66），
 * 所以回调注册/注销都作用在单例上；上下文本身属于堆 runner，不污染单例的
 * _engine/_traders/_hft_engine。堆 runner 永不 delete（BUG-R46-001）。
 * 不启动任何线程、不连柜台；临时目录在系统 tmp 下按 pid 隔离并清理。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "WtCore/WtHelper.h"
#include "WtPorter/ExpHftContext.h"
#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

USING_NS_WTP;

extern WtRtRunner& getRunner();

namespace {

	struct ChnlRec
	{
		uint32_t	ready = 0;
		uint32_t	lost = 0;
		CtxHandler	last_handle = 0;
		std::string	last_trader;
		WtUInt32	last_evt = 0;
		void clear() { *this = ChnlRec(); }
	};

	ChnlRec g_chnl;

	void chnl_cb(CtxHandler h, const char* trader, WtUInt32 evtid)
	{
		g_chnl.last_handle = h;
		g_chnl.last_trader = (trader == NULL) ? "<null>" : trader;
		g_chnl.last_evt = evtid;
		if (evtid == CHNL_EVENT_READY)
			g_chnl.ready++;
		else if (evtid == CHNL_EVENT_LOST)
			g_chnl.lost++;
	}

	std::string prepare_dir()
	{
		std::filesystem::path p = std::filesystem::temp_directory_path()
			/ ("r69_porter_ext_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(p, ec);
		std::filesystem::create_directories(p, ec);
		return p.string();
	}

	void clean_dir(const std::string& dir)
	{
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}

	void write_text(const std::string& path, const std::string& body)
	{
		std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
		ofs << body;
	}

	WtRtRunner* new_runner(const std::string& dir)
	{
		WtRtRunner* r = new WtRtRunner();
		r->init((dir + "/r69_absent_logcfg.yaml").c_str(), true, dir.c_str());
		return r;
	}

	/* hft env + 一个 active trader（无 module，TraderAdapter::init 返回 false 但
	 * _id 已赋值且适配器入表），供 createHftContext 绑定。 */
	WtRtRunner* new_hft_runner(const std::string& dir)
	{
		write_text(dir + "/actions.json", "{}");
		WtRtRunner* r = new_runner(dir);
		const std::string cfg =
			"{\"basefiles\":{},\"env\":{\"name\":\"hft\"},\"data\":{},"
			"\"bspolicy\":\"" + dir + "/actions.json\","
			"\"traders\":[{\"active\":true,\"id\":\"r69_chnl_trd\"}]}";
		if (!r->config(cfg.c_str(), false))
			return nullptr;
		return r;
	}

	ExpHftContext* make_bound_hft(WtRtRunner* r, const std::string& name)
	{
		if (r == nullptr || r->getEngine() == nullptr)
			return nullptr;
		const uint32_t id = r->createHftContext(name.c_str(), "r69_chnl_trd", false, 0);
		HftContextPtr base = r->getHftContext(id);
		ExpHftContext* ctx = dynamic_cast<ExpHftContext*>(base.get());
		return ctx;
	}

} // namespace

/* WtPorter.cpp:163-166 + WtRtRunner.cpp:305-312：导出层只做一行转发，两次不同 id
 * 都必须返回 true；createExtExecuter 只碰 _cta_engine，不读未初始化的 _engine。 */
TEST(R69PorterExtLifecycle, CreateExtExecuterExportWorksWithoutEngine)
{
	WtRtRunner& r = getRunner();
	ASSERT_EQ(r.getEngine(), nullptr);

	EXPECT_TRUE(create_ext_executer("r69_ext_exe_a"));
	EXPECT_TRUE(create_ext_executer("r69_ext_exe_b"));

	EXPECT_EQ(r.getEngine(), nullptr) << "create_ext_executer 不得初始化引擎";
}

/* ExpHftContext.cpp:63-70 + WtRtRunner.cpp:321-336：绑定已注册 trader 后，
 * on_channel_ready 用 _trader->id() 投递 CHNL_EVENT_READY 到单例回调。 */
TEST(R69PorterExtLifecycle, BoundHftChannelReadyCarriesTraderId)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());

	WtRtRunner* r = new_hft_runner(dir);
	ASSERT_NE(r, nullptr);
	ExpHftContext* ctx = make_bound_hft(r, "r69_hft_ready");
	ASSERT_NE(ctx, nullptr);

	g_chnl.clear();
	getRunner().registerHftCallbacks(NULL, NULL, NULL, chnl_cb, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL);

	ctx->on_channel_ready();

	EXPECT_EQ(g_chnl.ready, 1u);
	EXPECT_EQ(g_chnl.lost, 0u);
	EXPECT_EQ(g_chnl.last_trader, "r69_chnl_trd");
	EXPECT_EQ(g_chnl.last_evt, (WtUInt32)CHNL_EVENT_READY);
	EXPECT_EQ(g_chnl.last_handle, (CtxHandler)ctx->id());

	getRunner().registerHftCallbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL);
	clean_dir(dir);
}

/* ExpHftContext.cpp:50-57：同上，但走 CHNL_EVENT_LOST 分支。 */
TEST(R69PorterExtLifecycle, BoundHftChannelLostCarriesTraderId)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());

	WtRtRunner* r = new_hft_runner(dir);
	ASSERT_NE(r, nullptr);
	ExpHftContext* ctx = make_bound_hft(r, "r69_hft_lost");
	ASSERT_NE(ctx, nullptr);

	g_chnl.clear();
	getRunner().registerHftCallbacks(NULL, NULL, NULL, chnl_cb, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL);

	ctx->on_channel_lost();

	EXPECT_EQ(g_chnl.ready, 0u);
	EXPECT_EQ(g_chnl.lost, 1u);
	EXPECT_EQ(g_chnl.last_trader, "r69_chnl_trd");
	EXPECT_EQ(g_chnl.last_evt, (WtUInt32)CHNL_EVENT_LOST);

	getRunner().registerHftCallbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL);
	clean_dir(dir);
}

/* ExpHftContext.cpp:51-69 的回调槽位为空时只做基类收尾（_ud_modified=false），
 * 不得因为单例未注册回调而崩溃；同时 on_init 仍然把上下文 id 送进 init 槽。 */
TEST(R69PorterExtLifecycle, BoundHftEventsAreSilentWithoutRegisteredCallbacks)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());

	WtRtRunner* r = new_hft_runner(dir);
	ASSERT_NE(r, nullptr);
	ExpHftContext* ctx = make_bound_hft(r, "r69_hft_silent");
	ASSERT_NE(ctx, nullptr);

	g_chnl.clear();
	getRunner().registerHftCallbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL);

	EXPECT_NO_THROW(ctx->on_channel_ready());
	EXPECT_NO_THROW(ctx->on_channel_lost());
	EXPECT_EQ(g_chnl.ready, 0u);
	EXPECT_EQ(g_chnl.lost, 0u);

	clean_dir(dir);
}

/* 堆 runner 上 createHftContext 绑定 trader 是纯局部动作：单例的 _engine 与
 * _hft_engine 都不应被这次绑定初始化（对照 R6 断言的单例空引擎）。 */
TEST(R69PorterExtLifecycle, HeapRunnerHftBindingKeepsSingletonEngineEmpty)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());

	WtRtRunner* r = new_hft_runner(dir);
	ASSERT_NE(r, nullptr);
	ASSERT_NE(r->getEngine(), nullptr);
	EXPECT_NE(dynamic_cast<WtHftEngine*>(r->getEngine()), nullptr);

	ExpHftContext* ctx = make_bound_hft(r, "r69_hft_isolated");
	ASSERT_NE(ctx, nullptr);

	EXPECT_EQ(getRunner().getEngine(), nullptr);
	EXPECT_EQ(getRunner().getHftContext(ctx->id()), nullptr);

	clean_dir(dir);
}
