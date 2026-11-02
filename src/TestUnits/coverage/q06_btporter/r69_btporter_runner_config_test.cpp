/*
 * R69 - WtBtRunner 配置/回测器补测：hft/sel/exec 三种 mocker、增量 CTA、
 *       SEL 替换、raw bar 周期映射、分发 default 支与 WtBtRunner.h 内联事件
 *
 * 目标：src/WtBtPorter/WtBtRunner.cpp
 *   - config() 的 hft/sel/exec 三支（L480-506），sel 的 task 子段（L495-498）
 *   - initEvtNotifier() 对象支（L626-634）
 *   - loadRawHisBars() 的 KP_DAY/KP_Minute1 映射与 default 拒绝（L96-119）
 *   - initCtaMocker() 的 bIncremental 支（L276-279）
 *   - initSelMocker() 的「替换旧实例」支（L302-306）
 *   - ctx_on_bar / ctx_on_session_event / ctx_on_tick 的 EngineType default 支
 *     （L315-325、L371-381、L383-393）
 *   - WtBtRunner.h 的 registerEvtCallback / on_initialize_event / on_schedule_event /
 *     on_session_event / on_backtest_end（L202-205、L548-596）
 *
 * 与既有 lane 的分工：q06 覆盖分派有效支与 feed 守卫；R46 覆盖 config 的空/cta 支、
 * initCtaMocker/initSelMocker 首次创建、stop 早退；R9 覆盖 mocker 接线与
 * run/stop 线程面。本文件只取上述未覆盖支，不重复这些行段。
 *
 * 多数用例沿用堆上 new 而不 delete；exec 正例在 config_v2 成功后显式 release 清理插件，
 * 只断言配置合同，不覆盖其它生命周期行为。临时目录按 pid 隔离，用例末尾 remove_all。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <unistd.h>

#include "WtBtCore/WtHelper.h"
#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtPorter.h"
#include "WtBtPorter/WtBtRunner.h"

#include "Includes/WTSStruct.h"
#include "Includes/WTSVariant.hpp"

USING_NS_WTP;

namespace {

	std::string prepare_dir()
	{
		std::filesystem::path p = std::filesystem::temp_directory_path()
			/ ("r69_bt_runner_" + std::to_string(::getpid()));
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

	/* 堆上构造、永不 delete；init() 用不存在的日志文件，不启日志线程。 */
	WtBtRunner* new_runner(const std::string& dir)
	{
		WtBtRunner* r = new WtBtRunner();
		r->init((dir + "/r69_absent_logcfg.yaml").c_str(), true, dir.c_str());
		return r;
	}

	std::string replayer_cfg(const std::string& dir)
	{
		return "\"replayer\":{\"mode\":\"none\",\"basefiles\":{},\"path\":\"" + dir +
			"\",\"stime\":20240102000000,\"etime\":20240103000000}";
	}

	std::filesystem::path executable_dir()
	{
#ifdef __APPLE__
		uint32_t size = 0;
		_NSGetExecutablePath(nullptr, &size);
		std::vector<char> path(size + 1, 0);
		if (_NSGetExecutablePath(path.data(), &size) != 0)
			return std::filesystem::current_path();
		return std::filesystem::path(path.data()).parent_path();
#else
		char path[4096] = { 0 };
		const ssize_t size = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
		return size <= 0 ? std::filesystem::current_path()
			: std::filesystem::path(std::string(path, static_cast<size_t>(size))).parent_path();
#endif
	}

	std::string exec_valid_plugin_path()
	{
#ifdef __APPLE__
		const char* suffix = ".dylib";
#else
		const char* suffix = ".so";
#endif
		std::filesystem::path dir = executable_dir();
		for (int depth = 0; depth != 8 && !dir.empty(); ++depth)
		{
			const std::filesystem::path candidates[] = {
				dir / "exec_v2_plugins" / (std::string("valid") + suffix),
				dir / "coverage" / "exec_v2_plugins" / (std::string("valid") + suffix),
				dir / "TestUnits" / "coverage" / "exec_v2_plugins" / (std::string("valid") + suffix),
				dir / "src" / "TestUnits" / "coverage" / "exec_v2_plugins" / (std::string("valid") + suffix)
			};
			for (const std::filesystem::path& candidate : candidates)
				if (std::filesystem::exists(candidate))
					return candidate.string();
			dir = dir.parent_path();
		}
		return std::string();
	}

	/* ---- IHisDataLoader 替身 ---- */
	std::string g_raw_code;
	std::string g_raw_period;
	uint32_t g_raw_calls = 0;

	bool raw_loader(const char* stdCode, const char* period)
	{
		g_raw_calls++;
		g_raw_code = stdCode;
		g_raw_period = period;
		return true;
	}

	bool fnl_loader(const char* /*stdCode*/, const char* /*period*/) { return true; }
	bool fct_loader(const char* /*stdCode*/) { return true; }
	bool tick_loader(const char* /*stdCode*/, uint32_t /*date*/) { return true; }

	void bars_feeder(void* /*obj*/, WTSBarStruct* /*bars*/, uint32_t /*count*/) {}

	/* ---- 引擎事件探针（WtBtRunner.h 内联回调） ---- */
	struct EvtRec
	{
		uint32_t	count = 0;
		WtUInt32	evt = 0;
		WtUInt32	date = 0;
		WtUInt32	time = 0;
		void clear() { *this = EvtRec(); }
	};

	EvtRec g_evt;

	void evt_cb(WtUInt32 evtId, WtUInt32 curDate, WtUInt32 curTime)
	{
		g_evt.count++;
		g_evt.evt = evtId;
		g_evt.date = curDate;
		g_evt.time = curTime;
	}

} // namespace

/* WtBtRunner.cpp:480-486：有效回放范围使配置进入 HFT factory；缺少 module
 * 时 factory 失败且 fail-closed，不应留下半初始化 mocker。 */
TEST(R69BtRunnerConfig, HftModeFactoryFailureLeavesMockerEmpty)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());

	WtBtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);

	const std::string cfg =
		"{" + replayer_cfg(dir) + ","
		"\"env\":{\"mocker\":\"hft\"},"
		"\"notifier\":{\"active\":false},"
		"\"hft\":{\"name\":\"r69_bt_hft\",\"module\":\"\"}}";

	std::string error;
	EXPECT_FALSE(r->config_v2(cfg.c_str(), false, &error));
	EXPECT_NE(error.find("strategy factory failed"), std::string::npos) << error;

	EXPECT_EQ(r->hft_mocker(), nullptr);
	EXPECT_EQ(r->cta_mocker(), nullptr);
	EXPECT_EQ(r->sel_mocker(), nullptr);
	EXPECT_FALSE(r->isAsync());

	clean_dir(dir);
}

/* WtBtRunner.cpp:488-499：有效回放范围使配置经过 SEL task 参数校验；缺少 module
 * 时 factory 失败且 fail-closed，不应留下 mocker 或注册 task。 */
TEST(R69BtRunnerConfig, SelModeWithTaskFactoryFailureLeavesMockerEmpty)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());

	WtBtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);

	const std::string cfg =
		"{" + replayer_cfg(dir) + ","
		"\"env\":{\"mocker\":\"sel\"},"
		"\"sel\":{\"name\":\"r69_bt_sel\",\"slippage\":1,\"module\":\"\","
		"\"task\":{\"date\":20240102,\"time\":93000,\"period\":\"d\","
		"\"trdtpl\":\"CHINA\",\"session\":\"TRADING\"}}}";

	std::string error;
	EXPECT_FALSE(r->config_v2(cfg.c_str(), false, &error));
	EXPECT_NE(error.find("strategy factory failed"), std::string::npos) << error;

	EXPECT_EQ(r->sel_mocker(), nullptr);
	EXPECT_EQ(r->cta_mocker(), nullptr);
	EXPECT_FALSE(r->isAsync());

	clean_dir(dir);
}

TEST(R69BtRunnerConfig, SelModeRejectsOversizedTaskBeforeCreatingMocker)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());

	WtBtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);
	const std::string oversizedTemplate(16, 'T');
	const std::string cfg =
		"{\"replayer\":{\"mode\":\"none\",\"basefiles\":{},\"path\":\"" + dir + "\","
		"\"stime\":\"202401020930\",\"etime\":\"202401021000\"},"
		"\"env\":{\"mocker\":\"sel\"},"
		"\"sel\":{\"name\":\"r69_bad_sel_task\",\"module\":\"\","
		"\"task\":{\"date\":20240102,\"time\":93000,\"period\":\"d\","
		"\"trdtpl\":\"" + oversizedTemplate + "\",\"session\":\"TRADING\"}}}";
	std::string error;
	EXPECT_FALSE(r->config_v2(cfg.c_str(), false, &error));
	EXPECT_NE(error.find("invalid SEL task parameters"), std::string::npos) << error;
	EXPECT_EQ(r->sel_mocker(), nullptr);

	clean_dir(dir);
}

/* 有效的执行插件应使 exec 配置通过公开 config_v2 契约；这里验证初始化成功，
 * 不通过测试专用 accessor 检查 ExecMocker 内部槽位。 */
TEST(R69BtRunnerConfig, ExecModeValidPluginConfigurationSucceeds)
{
	const std::string module = exec_valid_plugin_path();
	ASSERT_FALSE(module.empty()) << "missing coverage execution-plugin fixture valid";

	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());

	WtBtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);

	const std::string cfg =
		"{" + replayer_cfg(dir) + ","
		"\"env\":{\"mocker\":\"exec\"},"
		"\"exec\":{\"name\":\"r69_bt_exec\",\"module\":\"" + module +
		"\",\"code\":\"SHFE.rb2510\",\"period\":\"m1\",\"volunit\":0,\"volmode\":0,"
		"\"executer\":{\"name\":\"R69Unit\",\"id\":\"r69_exec\",\"params\":{}},"
		"\"matcher\":{\"cancelrate\":0}}}";

	std::string error;
	EXPECT_TRUE(r->config_v2(cfg.c_str(), false, &error)) << error;

	// Explicitly close the plugin after the public configuration assertion.
	r->release();

	clean_dir(dir);
}

/* WtBtRunner.cpp:626-634：initEvtNotifier 只校验「非 NULL 且为对象」；对象里
 * active=false 时 EventNotifier::init 内部返回 false，但外层仍返回 true。 */
TEST(R69BtRunnerConfig, NotifierObjectIsAcceptedEvenWhenInactive)
{
	WtBtRunner* r = new WtBtRunner();
	ASSERT_NE(r, nullptr);

	WTSVariant* cfg = WTSVariant::createObject();
	ASSERT_NE(cfg, nullptr);
	cfg->append("active", false);

	EXPECT_TRUE(r->initEvtNotifier(cfg));

	cfg->release();
}

/* WtBtRunner.cpp:96-119：loadRawHisBars 的 KP_DAY->"d1"、KP_Minute1->"m1" 两条
 * 映射，以及未支持周期的 default 拒绝；loader 的返回值/参数原样透传。 */
TEST(R69BtRunnerConfig, RawBarLoaderMapsPeriodsAndRejectsUnknown)
{
	WtBtRunner* r = new WtBtRunner();
	ASSERT_NE(r, nullptr);
	r->registerExtDataLoader(fnl_loader, raw_loader, fct_loader, tick_loader, false);

	g_raw_calls = 0;
	g_raw_code.clear();
	g_raw_period.clear();
	int feedObj = 7;

	EXPECT_TRUE(r->loadRawHisBars(&feedObj, "SHFE.rb.2510", KP_DAY, bars_feeder));
	EXPECT_EQ(g_raw_calls, 1u);
	EXPECT_EQ(g_raw_code, "SHFE.rb.2510");
	EXPECT_EQ(g_raw_period, "d1");

	EXPECT_TRUE(r->loadRawHisBars(&feedObj, "SHFE.rb.2510", KP_Minute1, bars_feeder));
	EXPECT_EQ(g_raw_calls, 2u);
	EXPECT_EQ(g_raw_period, "m1");

	EXPECT_FALSE(r->loadRawHisBars(&feedObj, "SHFE.rb.2510", (WTSKlinePeriod)77, bars_feeder));
	EXPECT_EQ(g_raw_calls, 2u) << "不支持的周期不得调用 loader";
}

/* WtBtRunner.cpp:266-283 的 bIncremental 支 + :299-313 的替换支：
 * 增量 CTA 只检查输出目录下的 trades/closes/funds.csv，目录不存在时只记日志；
 * SEL 第二次创建会 delete 旧实例并返回新 id。 */
TEST(R69BtRunnerConfig, IncrementalCtaAndReplacedSelMocker)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());

	WtBtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);

	const uint32_t ctaId = r->initCtaMocker("r69_bt_inc", 0, false, false, true, false);
	EXPECT_NE(ctaId, 0u);
	ASSERT_NE(r->cta_mocker(), nullptr);

	const uint32_t selId1 = r->initSelMocker("r69_bt_sel_rep", 20240102, 93000, "d");
	EXPECT_NE(selId1, 0u);
	ASSERT_NE(r->sel_mocker(), nullptr);
	auto* originalSelMocker = r->sel_mocker();
	const std::string oversizedTemplate(16, 'T');
	EXPECT_EQ(0u, r->initSelMocker("r69_bt_sel_rep", 20240104, 93000, "d", oversizedTemplate.c_str()));
	EXPECT_EQ(0u, r->initSelMocker("r69_bt_sel_rep", 20240104, 93000, nullptr));
	EXPECT_EQ(originalSelMocker, r->sel_mocker()) << "invalid replacement must keep the previous owner";

	const uint32_t selId2 = r->initSelMocker("r69_bt_sel_rep", 20240103, 93000, "d");
	EXPECT_NE(selId2, 0u);
	EXPECT_NE(selId2, selId1) << "重建 SEL mocker 必须换新上下文 id";
	EXPECT_NE(r->sel_mocker(), nullptr);

	clean_dir(dir);
}

/* WtBtRunner.cpp:315-325 / 371-381 / 383-393 的 default 支 + WtBtRunner.h:202-205、
 * 548-596：未注册回调时 default 分派只 break（tick 的 NULL 不会被解引用）；
 * 注册后四个内联事件逐项带 evtId/date/time。 */
TEST(R69BtRunnerConfig, DispatchDefaultsAndInlineEngineEvents)
{
	WtBtRunner* r = new WtBtRunner();
	ASSERT_NE(r, nullptr);

	r->ctx_on_bar(1, "SHFE.rb.2510", "d1", NULL, (EngineType)777);
	r->ctx_on_session_event(2, 20240102, true, (EngineType)777);
	r->ctx_on_tick(3, "SHFE.rb.2510", NULL, (EngineType)777);
	SUCCEED();

	g_evt.clear();
	r->registerEvtCallback(evt_cb);

	r->on_initialize_event();
	EXPECT_EQ(g_evt.count, 1u);
	EXPECT_EQ(g_evt.evt, (WtUInt32)EVENT_ENGINE_INIT);

	r->on_schedule_event(20240102, 93000);
	EXPECT_EQ(g_evt.count, 2u);
	EXPECT_EQ(g_evt.evt, (WtUInt32)EVENT_ENGINE_SCHDL);
	EXPECT_EQ(g_evt.date, 20240102u);
	EXPECT_EQ(g_evt.time, 93000u);

	r->on_session_event(20240102, true);
	EXPECT_EQ(g_evt.evt, (WtUInt32)EVENT_SESSION_BEGIN);
	r->on_session_event(20240102, false);
	EXPECT_EQ(g_evt.evt, (WtUInt32)EVENT_SESSION_END);

	r->on_backtest_end();
	EXPECT_EQ(g_evt.count, 5u);
	EXPECT_EQ(g_evt.evt, (WtUInt32)EVENT_BACKTEST_END);

	r->registerEvtCallback(NULL);
}
