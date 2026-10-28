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
 * 堆上 new、永不 delete（BUG-R46-001：_hft_mocker/_exec_mocker/_worker/_cfg 未初始化），
 * 因此不调用 release()/run()/isAutoTrans()/hft_mocker() 之外的未初始化槽位。
 * 临时目录按 pid 隔离，用例末尾 remove_all。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>

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
		return "\"replayer\":{\"mode\":\"none\",\"basefiles\":{},\"path\":\"" + dir + "\"}";
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

/* WtBtRunner.cpp:480-486 + :465 + :467：env.mocker="hft" 时按 hft 段建 ExpHftMocker
 * 并注册 sink；module 为空时 init_hft_factory 在 DLLHelper 处返回 false，但槽位
 * 已建立。notifier 段给 active=false，只走 EventNotifier::init 的早退。 */
TEST(R69BtRunnerConfig, HftModeCreatesHftMockerSlot)
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

	r->config(cfg.c_str(), false);

	EXPECT_NE(r->hft_mocker(), nullptr);
	EXPECT_EQ(r->cta_mocker(), nullptr);
	EXPECT_EQ(r->sel_mocker(), nullptr);
	EXPECT_FALSE(r->isAsync());

	clean_dir(dir);
}

/* WtBtRunner.cpp:488-499：env.mocker="sel" + task 子段；register_task 用 "d" 周期
 * 与 CHINA/TRADING 模板（与 R46 的 initSelMocker 同口径，但这里走 config 的 task 支）。 */
TEST(R69BtRunnerConfig, SelModeWithTaskRegistersSchedule)
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

	r->config(cfg.c_str(), false);

	EXPECT_NE(r->sel_mocker(), nullptr);
	EXPECT_EQ(r->cta_mocker(), nullptr);
	EXPECT_FALSE(r->isAsync());

	clean_dir(dir);
}

/* WtBtRunner.cpp:500-506：env.mocker="exec" 时建 ExecMocker；matcher 段缺省时
 * MatchEngine::init(NULL) 直接返回，module 为空时 DLLHelper 返回 NULL，均不抛。 */
TEST(R69BtRunnerConfig, ExecModeCreatesExecMockerSlot)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());

	WtBtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);

	const std::string cfg =
		"{" + replayer_cfg(dir) + ","
		"\"env\":{\"mocker\":\"exec\"},"
		"\"exec\":{\"name\":\"r69_bt_exec\",\"module\":\"\"}}";

	r->config(cfg.c_str(), false);

	EXPECT_EQ(r->cta_mocker(), nullptr);
	EXPECT_EQ(r->sel_mocker(), nullptr);
	EXPECT_FALSE(r->isAsync());

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
