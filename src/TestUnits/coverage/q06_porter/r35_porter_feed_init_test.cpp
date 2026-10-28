/*
 * R35 - WtPorter 数据馈送链 + reg_*_factories 导出层 + init_porter 一次性门
 *
 * 目标：src/WtPorter/WtPorter.cpp（feed_raw_bars L193-196、feed_adj_factors L209-212、
 *   reg_{cta,sel,hft,exe}_factories L352-391、init_porter L239-249）
 *   src/WtPorter/WtRtRunner.cpp（feedAdjFactors L277-280、feedRawBars L283-292、
 *   loadAllAdjFactors/loadAdjFactors L253-275 的「喂给 feeder」尾段）
 *   src/WtCore/CtaStrategyMgr.cpp L30-93 / SelStrategyMgr / HftStrategyMgr / WtExecuterFactory 的
 *   loadFactories「空目录 -> true」支（R6 只取过「目录不存在 -> false」支）
 *
 * 与既有 lane 的分工：
 *  - q06（q06_porter_runner_test.cpp）取到 load* 的周期映射与守卫支，但它唯一的
 *    feed 链用例 `FeedRawBarsUsesCachedFeeder` 在未加载因子 feeder 的情况下调用
 *    feedAdjFactors，命中 `_feeder_fcts` 空指针（既有红，已登记 bugs/q06_porters.md BUG-Q06-004）。
 *    本文件走「先 loadAdjFactors 再 feed_adj_factors」的正确顺序，把同一段生产代码
 *    （WtPorter.cpp:209-212 + WtRtRunner.cpp:277-280）在不崩的前提下打通。
 *  - R6 取的是 WtRtRunner 侧 addXxxFactories 的 false 支；本文件取 WtPorter 导出层
 *    L352-391 的转发（false 与 true 两个返回值都钉住）。
 *  - 本文件一律使用 getRunner() 单例，绝不栈构造 WtRtRunner
 *    （栈实例的成员 `_cfg/_config/_engine/_feeder_*` 无初始化即进析构，是 R6 族 23 条红的
 *    落点，见 manifest 与 BUG-R35-001；静态单例的同一批成员先被零初始化，因此安全）。
 *
 * 不起线程、不 sleep、不连网络；唯一的磁盘动作是 std::filesystem 在系统临时目录里
 * 建/删一个空目录（供 loadFactories 的 directory_iterator 走空循环）。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "WtCore/WtHelper.h"
#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

#include "Includes/IDataReader.h"
#include "Includes/WTSStruct.h"
#include "Share/ModuleHelper.hpp"

USING_NS_WTP;

/* WtPorter.cpp:34-38 的进程单例；导出入口与 IHisDataLoader 都经它转发 */
extern WtRtRunner& getRunner();

namespace {

	/* ---- 外部数据加载器（FuncLoadFnlBars/FuncLoadRawBars/FuncLoadAdjFactors）---- */
	uint32_t g_fnl_calls = 0, g_raw_calls = 0, g_fct_calls = 0;
	std::string g_load_code, g_load_period;

	bool fnl_loader(const char* stdCode, const char* period)
	{
		g_fnl_calls++;
		g_load_code = stdCode;
		g_load_period = period;
		return true;
	}

	bool raw_loader(const char* stdCode, const char* period)
	{
		g_raw_calls++;
		g_load_code = stdCode;
		g_load_period = period;
		return true;
	}

	bool adj_loader(const char* stdCode)
	{
		g_fct_calls++;
		g_load_code = stdCode;
		return true;
	}

	/* ---- 落地 feeder（FuncReadBars/FuncReadFactors），按函数指针签名必须是外部链接函数 ---- */
	void* g_barA_obj = NULL; WTSBarStruct* g_barA_ptr = NULL; uint32_t g_barA_cnt = 0, g_barA_calls = 0;
	void* g_barB_obj = NULL; WTSBarStruct* g_barB_ptr = NULL; uint32_t g_barB_cnt = 0, g_barB_calls = 0;

	void bars_feeder_a(void* obj, WTSBarStruct* bars, uint32_t count)
	{
		g_barA_calls++; g_barA_obj = obj; g_barA_ptr = bars; g_barA_cnt = count;
	}
	void bars_feeder_b(void* obj, WTSBarStruct* bars, uint32_t count)
	{
		g_barB_calls++; g_barB_obj = obj; g_barB_ptr = bars; g_barB_cnt = count;
	}

	void* g_fct_obj = NULL;
	std::string g_fct_code;
	uint32_t* g_fct_dates = NULL; double* g_fct_vals = NULL;
	uint32_t g_fct_feed_calls = 0, g_fct_feed_cnt = 0;

	void fct_feeder(void* obj, const char* stdCode, uint32_t* dates, double* factors, uint32_t count)
	{
		g_fct_feed_calls++;
		g_fct_obj = obj;
		g_fct_code = (stdCode == NULL) ? "<null>" : stdCode;
		g_fct_dates = dates;
		g_fct_vals = factors;
		g_fct_feed_cnt = count;
	}

	/* 第二套加载器：只为钉 registerExtDataLoader 的「整组替换」语义 */
	uint32_t g_fnl2_calls = 0;
	bool fnl_loader2(const char*, const char*) { g_fnl2_calls++; return true; }

	void reset_all()
	{
		g_fnl_calls = g_raw_calls = g_fct_calls = 0;
		g_load_code.clear(); g_load_period.clear();
		g_barA_calls = g_barA_cnt = 0; g_barA_obj = NULL; g_barA_ptr = NULL;
		g_barB_calls = g_barB_cnt = 0; g_barB_obj = NULL; g_barB_ptr = NULL;
		g_fct_feed_calls = g_fct_feed_cnt = 0;
		g_fct_obj = NULL; g_fct_dates = NULL; g_fct_vals = NULL; g_fct_code.clear();
		g_fnl2_calls = 0;
	}

	WTSBarStruct makeBar()
	{
		WTSBarStruct bar;
		memset(&bar, 0, sizeof(bar));
		bar.date = 20240603;
		bar.time = 145500;
		bar.close = 3521.25;
		bar.hold = 1234;
		return bar;
	}

	const char* kAbsentDir = "./r35_absent_factory_dir_zzz/";
}

class R35PorterFeed : public ::testing::Test
{
protected:
	void SetUp() override
	{
		reset_all();
		getRunner().registerExtDataLoader(fnl_loader, raw_loader, adj_loader, NULL);
	}

	void TearDown() override
	{
		getRunner().registerExtDataLoader(NULL, NULL, NULL, NULL);
		reset_all();
	}
};

/*
 * feed_raw_bars（WtPorter.cpp:193-196）→ WtRtRunner::feedRawBars（L283-292 的命中支）：
 * 前置是 loadFinalHisBars（L203-226）把 (obj, feeder) 缓存进 _feed_obj/_feeder_bars。
 * 断言 feeder 收到的是**同一根对象指针、同一根 bar 数组指针、原样 count**。
 */
TEST_F(R35PorterFeed, FeedRawBarsFiresCachedBarFeederWithExactPointers)
{
	WTSBarStruct bar = makeBar();
	int feedObj = 42;

	ASSERT_TRUE(getRunner().loadFinalHisBars(&feedObj, "SHFE.rb.2510", KP_DAY, bars_feeder_a));
	// WtRtRunner.cpp:214-215 把 KP_DAY 映射成 "d1" 后调用 loader（此处新钉导出层没走过的入口返回值）
	EXPECT_EQ(g_fnl_calls, 1u);
	EXPECT_EQ(g_load_code, "SHFE.rb.2510");
	EXPECT_EQ(g_load_period, "d1");

	feed_raw_bars(&bar, 2);

	EXPECT_EQ(g_barA_calls, 1u);
	EXPECT_EQ(g_barA_obj, (void*)&feedObj);
	EXPECT_EQ(g_barA_ptr, &bar);
	EXPECT_EQ(g_barA_cnt, 2u);
}

/*
 * feed_adj_factors（WtPorter.cpp:209-212）→ WtRtRunner::feedAdjFactors（L277-280，
 * 既红 Q06PorterRunner.FeedRawBarsUsesCachedFeeder 的崩点，缺的就是先 loadAdjFactors）。
 * 正确顺序下：stdCode/dates/factors/count 五个参数逐一到 feeder，指针原样透传。
 */
TEST_F(R35PorterFeed, FeedAdjFactorsFiresLoadedFactorFeeder)
{
	uint32_t dates[3] = { 20240101, 20240601, 20240901 };
	double fcts[3] = { 1.0, 1.25, 0.75 };
	int feedObj = 7;

	ASSERT_TRUE(getRunner().loadAdjFactors(&feedObj, "SSE.600519", fct_feeder));
	EXPECT_EQ(g_fct_calls, 1u);
	EXPECT_EQ(g_load_code, "SSE.600519");

	feed_adj_factors("SSE.600519", dates, fcts, 3);

	EXPECT_EQ(g_fct_feed_calls, 1u);
	EXPECT_EQ(g_fct_obj, (void*)&feedObj);
	EXPECT_EQ(g_fct_code, "SSE.600519");
	EXPECT_EQ(g_fct_dates, dates);
	EXPECT_EQ(g_fct_vals, fcts);
	EXPECT_EQ(g_fct_feed_cnt, 3u);
}

/*
 * loadAllAdjFactors（WtRtRunner.cpp:253-263）用**空串**请求「全品种」因子，
 * 随后 feed_adj_factors 走同一缓存的 _feeder_fcts。
 */
TEST_F(R35PorterFeed, LoadAllAdjFactorsRequestsEmptyCodeThenFeedsPerCode)
{
	uint32_t dates[2] = { 20240101, 20240701 };
	double fcts[2] = { 1.1, 0.9 };
	int feedObj = 9;

	ASSERT_TRUE(getRunner().loadAllAdjFactors(&feedObj, fct_feeder));
	EXPECT_EQ(g_fct_calls, 1u);
	EXPECT_TRUE(g_load_code.empty()) << "WtRtRunner.cpp:262 固定以 \"\" 请求全品种";

	feed_adj_factors("SHFE.rb.2510", dates, fcts, 2);

	EXPECT_EQ(g_fct_feed_calls, 1u);
	EXPECT_EQ(g_fct_code, "SHFE.rb.2510");
	EXPECT_EQ(g_fct_feed_cnt, 2u);
	EXPECT_NEAR(fcts[0] + fcts[1], 2.0, 1e-9);
}

/*
 * _feeder_bars 的语义是「最后一次 load 胜出」：loadFinalHisBars 放 feeder_a，
 * loadRawHisBars（WtRtRunner.cpp:228-251，:235 同写 _feeder_bars）放 feeder_b 后，
 * feed_raw_bars 只能打到 b，a 必须保持 0 次。
 */
TEST_F(R35PorterFeed, LaterLoadReplacesCachedBarFeeder)
{
	WTSBarStruct bar = makeBar();
	int feedObj = 11;

	ASSERT_TRUE(getRunner().loadFinalHisBars(&feedObj, "SHFE.rb.2510", KP_Minute1, bars_feeder_a));
	EXPECT_EQ(g_load_period, "m1");
	ASSERT_TRUE(getRunner().loadRawHisBars(&feedObj, "SHFE.rb.2510", KP_Minute5, bars_feeder_b));
	EXPECT_EQ(g_load_period, "m5");
	EXPECT_EQ(g_raw_calls, 1u);

	feed_raw_bars(&bar, 1);

	EXPECT_EQ(g_barB_calls, 1u);
	EXPECT_EQ(g_barA_calls, 0u);
	EXPECT_EQ(g_barB_obj, (void*)&feedObj);
}

/*
 * registerExtDataLoader（WtRtRunner.h:242-247 内联）是「三个槽位一起换」的赋值：
 * 换入第二组后，旧的 fnl loader 不得再被调用。
 */
TEST_F(R35PorterFeed, RegisterExtDataLoaderReplacesAllLoaderSlots)
{
	int feedObj = 5;
	ASSERT_TRUE(getRunner().loadFinalHisBars(&feedObj, "SHFE.rb.2510", KP_DAY, bars_feeder_a));
	EXPECT_EQ(g_fnl_calls, 1u);

	getRunner().registerExtDataLoader(fnl_loader2, raw_loader, adj_loader, NULL);
	ASSERT_TRUE(getRunner().loadFinalHisBars(&feedObj, "SHFE.rb.2510", KP_DAY, bars_feeder_a));

	EXPECT_EQ(g_fnl2_calls, 1u);
	EXPECT_EQ(g_fnl_calls, 1u) << "WtRtRunner.h:244 是覆盖赋值，旧 loader 不应再被引用";
}

/*
 * 导出层 reg_*_factories（WtPorter.cpp:352/364/376/388）只做一行转发：
 * 目录不存在时四个入口都必须把各 *Mgr::loadFactories 的 false 原样带回来
 * （守卫支 WtCore/CtaStrategyMgr.cpp:32-36 等四处；R6 只从 WtRtRunner 侧取过 false）。
 */
TEST_F(R35PorterFeed, RegFactoriesReturnFalseForAbsentDirectory)
{
	EXPECT_FALSE(reg_cta_factories(kAbsentDir));
	EXPECT_FALSE(reg_sel_factories(kAbsentDir));
	EXPECT_FALSE(reg_hft_factories(kAbsentDir));
	EXPECT_FALSE(reg_exe_factories(kAbsentDir));
}

/*
 * 同一批入口的存在支：真实存在的**空目录**里没有任何 .dylib，
 * directory_iterator 空循环后 loadFactories 返回 true
 * （CtaStrategyMgr.cpp:41-93、SelStrategyMgr、HftStrategyMgr、WtExecuterFactory 同形）。
 * 这条同时证明 false 不是「恒 false 的桩」。目录建在系统临时目录，用例尾部删除。
 */
TEST_F(R35PorterFeed, RegFactoriesReturnTrueForExistingEmptyDirectory)
{
	std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) / "r35_empty_fact_dir";
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	ASSERT_TRUE(std::filesystem::create_directories(dir, ec)) << ec.message();
	const std::string p = dir.string() + "/";

	EXPECT_TRUE(reg_cta_factories(p.c_str()));
	EXPECT_TRUE(reg_sel_factories(p.c_str()));
	EXPECT_TRUE(reg_hft_factories(p.c_str()));
	EXPECT_TRUE(reg_exe_factories(p.c_str()));

	std::filesystem::remove_all(dir, ec);
}

/*
 * init_porter（WtPorter.cpp:239-249）：
 *  - 第一次调用走到 WtRtRunner::init（L124），把 getBinDir() 写进 WtHelper；
 *    日志配置用**解析不了的垃圾串**，WTSLogger::init 在 cfg==NULL 守卫处返回
 *    （WTSLogger.cpp:211-215），不起日志线程、不 throw（有效 JSON 但缺 root 才 throw，:239-242）。
 *  - 第二次调用必须被 `static bool inited` 的门（:241-244）整体吃掉：
 *    用 setInstDir 放一个哨兵值，二次 init_porter 后哨兵必须原样还在。
 */
TEST_F(R35PorterFeed, InitPorterStampsInstDirOnceAndSecondCallIsNoOp)
{
	WtHelper::setInstDir("");

	init_porter("{ r35 not a valid log config", false, "");

	const std::string& inst = WtHelper::getInstDir();
	ASSERT_FALSE(inst.empty()) << "WtRtRunner.cpp:124 应把 getBinDir() 写进 WtHelper";
	EXPECT_EQ(inst, std::string(getBinDir()));
	EXPECT_EQ(inst.substr(inst.size() - 1), "/");

	WtHelper::setInstDir("r35_sentinel_value/");
	init_porter("{ another invalid config", false, "");
	EXPECT_EQ(WtHelper::getInstDir(), "r35_sentinel_value/")
		<< "WtPorter.cpp:241-244 的一次性门失效（inited 已置位，第二次调用不得再动目录）";

	WtHelper::setInstDir("");
}

/*
 * 复现登记（不启用）：栈构造 WtRtRunner 必崩在**析构**，不在构造。
 * 实测证据：macOS crash report
 *   ~/Library/Logs/DiagnosticReports/CoverageTests_porter-2026-10-28-012210.ips
 *   EXC_BAD_ACCESS (KERN_INVALID_ADDRESS at 0x10)，故障线程栈顶帧序：
 *   R6PorterSilence_cta_dispatch..._Test::TestBody -> WtRtRunner::~WtRtRunner
 *   -> wtp::WtCtaEngine::~WtCtaEngine（连续两帧）。
 * 嫌疑位置：WtCore/WtCtaEngine.cpp:41-47 `if (_cfg) _cfg->release();`——
 * `_cfg`（WtCtaEngine.h:92）不在构造列表（:35-39 只初始化 _tm_ticker），栈实例=垃圾指针；
 * 有证据的只有「崩在该析构内」，`_cfg` 是否唯一成因未证明（见 BUG-R35-001）。
 * 静态单例 getRunner() 的同一批成员在构造前先被零初始化，所以本 lane 全部 21 条
 * 启用用例走单例、0 崩溃；R6 族 23 条红就是逐条在栈上 `WtRtRunner r;` 的产物，
 * 交主控处置，本 lane 不代改兄弟用例。
 */
TEST(R35PorterRunnerLocal, DISABLED_stack_instance_destructor_crashes_after_testbody)
{
	WtRtRunner r;
	EXPECT_TRUE(r.init("./r35_absent_logcfg_zzz.yaml", true, ""));
}
