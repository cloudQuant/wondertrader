/*
 * R69 - WtRtRunner 配置成功臂：basefiles 全段、parsers/traders/executers
 *       外部文件、initDataMgr 空配置、initEvtNotifier、initEngine 非标准 env
 *
 * 目标：src/WtPorter/WtRtRunner.cpp
 *   - basefiles 每一段：session / commodity(string|array) / contract(string|array) /
 *     holiday / hot / second / rules + 主力代码回填循环（L493-591）
 *   - parsers：内联 inactive 项成功跳过（L620-623、L1687-1748）、active 项缺模块失败，
 *     外部文件缺 "parsers" 键或 JSON 损坏均失败（L1190-1227）
 *   - traders 外部文件数组合法项（L630-647、L1071-1087）
 *   - executers 外部文件 + routers 两处入口（L668-685、L702-704）、文件缺失（L691-694）
 *   - initDataMgr() 的空 data/no-store 配置与 reader-null 查询守卫
 *   - initEngine() 空 name -> CTA（L846-850）与未知 name -> HFT（L855-865）
 *   - initEvtNotifier() 对象段（L1054-1063）
 *
 * 与既有 lane 的分工：R46 已覆盖 env=cta/hft/sel 三支与 traders/executers 数组、
 * 空 strategies；本文件不重复这些，只补 R46 没碰的 basefiles 段与外部文件路径。
 * 所有 runner 都在堆上 new 且故意不 delete（BUG-R46-001：构造列表漏初始化
 * _config/_engine/_feed_*），绝不动进程单例 getRunner() 的状态。
 * 临时文件只写系统临时目录的 r69_porter_cfg_<pid>，每个用例末尾 remove_all。
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "WtCore/WtHelper.h"
#include "WtCore/ExecUnitInitStatus.h"
#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

#include "Includes/WTSVariant.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"
#include "WTSTools/WTSHotMgr.h"

USING_NS_WTP;

namespace {

	std::string prepare_dir()
	{
		std::filesystem::path p = std::filesystem::temp_directory_path()
			/ ("r69_porter_cfg_" + std::to_string(::getpid()));
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

	/* 堆上构造、永不 delete：见 BUG-R46-001。init() 用不存在的日志文件，
	 * WTSLogger::init 在 exists 守卫处直接返回，不起日志线程。 */
	WtRtRunner* new_runner(const std::string& dir)
	{
		WtRtRunner* r = new WtRtRunner();
		r->init((dir + "/r69_absent_logcfg.yaml").c_str(), true, dir.c_str());
		return r;
	}

	const char* kSessions =
		"{\"TRADING\":{\"name\":\"Trading\",\"offset\":0,"
		"\"sections\":[{\"from\":900,\"to\":1500}]}}";

	// 卡1（DateMappingV1）：按品种映射交易日须有交易日历；品种缺 holiday 模板或未加载节假日文件即 MissingCalendar，K 线读取为空。
	const char* kCommodityOne =
		"{\"SHFE\":{\"au\":{\"name\":\"Gold\",\"session\":\"TRADING\",\"holiday\":\"CHINA\","
		"\"pricetick\":0.02,\"volscale\":1000,\"covermode\":0,\"pricemode\":0}}}";

	const char* kCommodityTwo =
		"{\"SHFE\":{\"au\":{\"name\":\"Gold\",\"session\":\"TRADING\","
		"\"pricetick\":0.02,\"volscale\":1000,\"covermode\":0,\"pricemode\":0}},"
		"\"DCE\":{\"m\":{\"name\":\"SoybeanMeal\",\"session\":\"TRADING\","
		"\"pricetick\":1,\"volscale\":10,\"covermode\":0,\"pricemode\":0}}}";

	const char* kContractOne =
		"{\"SHFE\":{\"au2406\":{\"name\":\"au2406\",\"product\":\"au\",\"exchg\":\"SHFE\"}}}";

	const char* kContractTwo =
		"{\"SHFE\":{\"au2406\":{\"name\":\"au2406\",\"product\":\"au\",\"exchg\":\"SHFE\"}},"
		"\"DCE\":{\"m2409\":{\"name\":\"m2409\",\"product\":\"m\",\"exchg\":\"DCE\"}}}";

	const char* kHotRules =
		"{\"SHFE\":{\"au\":[{\"from\":\"au2406\",\"to\":\"au2406\",\"date\":20240101,"
		"\"oldclose\":100.0,\"newclose\":100.0}]}}";

	const char* kActions = "{\"default\":{}}";

	WtRtRunner* g_data_store_loader_runner = nullptr;
	uint32_t g_data_store_loader_calls = 0;
	WTSBarStruct g_data_store_bars[2];

	bool data_store_loader(const char* /*stdCode*/, const char* /*period*/)
	{
		if (g_data_store_loader_runner == nullptr)
			return false;

		++g_data_store_loader_calls;
		g_data_store_loader_runner->feedRawBars(g_data_store_bars, 2);
		return true;
	}

	void write_query_basefiles(const std::string& dir)
	{
		write_text(dir + "/sessions.json", kSessions);
		write_text(dir + "/commodity.json", kCommodityOne);
		write_text(dir + "/contract.json", kContractOne);
		write_text(dir + "/query_holidays.json", "{\"CHINA\":[]}");
	}

	std::string query_basefiles(const std::string& dir)
	{
		return "\"basefiles\":{\"session\":\"" + dir + "/sessions.json\","
			"\"commodity\":\"" + dir + "/commodity.json\","
			"\"contract\":\"" + dir + "/contract.json\","
			"\"holiday\":\"" + dir + "/query_holidays.json\"}";
	}

	std::string bin_dir()
	{
		Dl_info info;
		if (dladdr(reinterpret_cast<void*>(&bin_dir), &info) == 0 || info.dli_fname == nullptr)
			return std::string();
		std::filesystem::path p = std::filesystem::path(info.dli_fname).parent_path().parent_path();
		std::string ret = p.string();
		if (!ret.empty() && ret.back() != '/')
			ret.push_back('/');
		return ret;
	}

	/* config 的公共尾巴：env/data/bspolicy。envName 为空串时命中 CTA 默认支。 */
	std::string tail(const std::string& dir, const std::string& envName)
	{
		return "\"env\":{\"name\":\"" + envName + "\"},\"data\":{},"
			"\"bspolicy\":\"" + dir + "/actions.json\"";
	}

} // namespace

/* WtRtRunner.cpp:495-560 + 565-579 + 846-850：basefiles 的 session、commodity 单串、
 * contract 单串、holiday/hot/second/rules 五个可选项全部给值；env.name="" 走 CTA。
 * 主力代码回填循环使用当前 WTSHotMgr 实例已加载的标签。 */
TEST(R69PorterRunnerConfig, BasefilesStringFormsAndEmptyEnvNameBuildCtaEngine)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());

	write_text(dir + "/sessions.json", kSessions);
	write_text(dir + "/commodity_one.json", kCommodityOne);
	write_text(dir + "/contract_one.json", kContractOne);
	write_text(dir + "/holidays.json", "{\"CHINA\":[20240101,20240210]}");
	write_text(dir + "/hot.json", kHotRules);
	write_text(dir + "/second.json", kHotRules);
	write_text(dir + "/custom.json", kHotRules);
	write_text(dir + "/actions.json", kActions);

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);

	const std::string cfg =
		"{\"basefiles\":{"
		"\"session\":\"" + dir + "/sessions.json\","
		"\"commodity\":\"" + dir + "/commodity_one.json\","
		"\"contract\":\"" + dir + "/contract_one.json\","
		"\"holiday\":\"" + dir + "/holidays.json\","
		"\"hot\":\"" + dir + "/hot.json\","
		"\"second\":\"" + dir + "/second.json\","
		"\"rules\":{\"CUSTOM\":\"" + dir + "/custom.json\"}},"
		+ tail(dir, "") + "}";

	ASSERT_TRUE(r->config(cfg.c_str(), false));
	ASSERT_NE(r->getEngine(), nullptr);
	EXPECT_NE(dynamic_cast<WtCtaEngine*>(r->getEngine()), nullptr);

	clean_dir(dir);
}

/* WtRtRunner.cpp:501-531 的 commodity/contract 数组支 + :620-623 内联 parser 数组支。
 * inactive parser 会被跳过；basefiles 数组和 CTA engine 保持有效正向覆盖。 */
TEST(R69PorterRunnerConfig, BasefilesArrayFormsAndInactiveInlineParserBuildCtaEngine)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());

	write_text(dir + "/sessions.json", kSessions);
	write_text(dir + "/commodities.json", kCommodityTwo);
	write_text(dir + "/contracts.json", kContractTwo);
	write_text(dir + "/actions.json", kActions);

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);

	const std::string cfg =
		"{\"basefiles\":{"
		"\"session\":\"" + dir + "/sessions.json\","
		"\"commodity\":[\"" + dir + "/commodities.json\"],"
		"\"contract\":[\"" + dir + "/contracts.json\"]},"
		"\"parsers\":[{\"active\":false,\"id\":\"r69_parser_off\"}],"
		+ tail(dir, "cta") + "}";

	ASSERT_TRUE(r->config(cfg.c_str(), false));
	ASSERT_NE(r->getEngine(), nullptr);
	EXPECT_NE(dynamic_cast<WtCtaEngine*>(r->getEngine()), nullptr);

	clean_dir(dir);
}

TEST(R69PorterRunnerConfig, ActiveInlineParserWithoutModuleFailsConfig)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", kActions);

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);
	const std::string cfg =
		"{\"basefiles\":{},\"parsers\":[{\"active\":true,"
		"\"id\":\"r69_parser_without_module\"}],"
		+ tail(dir, "cta") + "}";

	EXPECT_FALSE(r->config(cfg.c_str(), false));
	clean_dir(dir);
}

TEST(R69PorterRunnerConfig, ParserFileMissingKeyFailsConfig)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/parsers_missing.json", "{}");
	write_text(dir + "/actions.json", kActions);

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);
	const std::string cfg =
		"{\"basefiles\":{},\"parsers\":\"" + dir + "/parsers_missing.json\","
		+ tail(dir, "cta") + "}";

	EXPECT_FALSE(r->config(cfg.c_str(), false));
	clean_dir(dir);
}

TEST(R69PorterRunnerConfig, BrokenParserFileFailsConfig)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/parsers_broken.json", "{ this is not a json document");
	write_text(dir + "/actions.json", kActions);

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);
	const std::string cfg =
		"{\"basefiles\":{},\"parsers\":\"" + dir + "/parsers_broken.json\","
		+ tail(dir, "cta") + "}";

	EXPECT_FALSE(r->config(cfg.c_str(), false));

	clean_dir(dir);
}

/* WtRtRunner.cpp:630-647 + :665-704：traders 外部文件（inactive 项被跳过）、executers 外部文件
 * （空数组 + routers 对象）与顶层 routers 对象两个 loadRouterRules 入口。 */
TEST(R69PorterRunnerConfig, TraderAndExecuterFilesLoadWithRouterSections)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());

	write_text(dir + "/traders.json",
		"{\"traders\":[{\"active\":false,\"id\":\"r69_trd_off\"}]}");
	// 迭代03（BUG-R134-003/WSN06-001）：router 规则加载结果现传播为配置失败，执行器管理器
	// 只接受数组（N02ExecMgrTest.LoadRouterRulesRejectsNullAndNonArray）；原夹具用 {} 依赖旧的
	// 忽略返回值行为，改为空数组仍到达两处 load_router_rules 入口（.before 见 s5-runners 证据）。
	write_text(dir + "/executers.json",
		"{\"executers\":[],\"routers\":[]}");
	write_text(dir + "/actions.json", kActions);

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);

	const std::string cfg =
		"{\"basefiles\":{},"
		"\"traders\":\"" + dir + "/traders.json\","
		"\"executers\":\"" + dir + "/executers.json\","
		"\"routers\":[],"
		+ tail(dir, "cta") + "}";

	ASSERT_TRUE(r->config(cfg.c_str(), false));
	ASSERT_NE(r->getEngine(), nullptr);

	clean_dir(dir);
}

TEST(R69PorterRunnerConfig, TraderInitFailureInExternalFileFailsConfig)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());

	write_text(dir + "/traders.json",
		"{\"traders\":[{\"active\":true,\"id\":\"r69_missing_trd\","
		"\"module\":\"r69_module_that_does_not_exist\"}]}");
	write_text(dir + "/actions.json", kActions);

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);
	const std::string cfg =
		"{\"basefiles\":{},\"traders\":\"" + dir + "/traders.json\","
		+ tail(dir, "cta") + "}";

	EXPECT_FALSE(r->config(cfg.c_str(), false));
	clean_dir(dir);
}

/* WtRtRunner.cpp:691-694 + initDataMgr()：executers 指向不存在的文件只告警；
 * tail() 固定附加 data:{}，此用例覆盖空 data/no-store 可选模式；不会声称覆盖缺失 data 键。
 * data.store 不存在时，WtDtMgr::init 的 false 保持可选 reader 行为，
 * Engine 公共 K 线查询最终由 WtDtMgr 的空 reader 守卫安全返回 NULL；
 * notifier 是对象（active=false，EventNotifier::init 内部返回 false，不加载 MQ 模块），
 * initEvtNotifier() 仍返回 true。 */
TEST(R69PorterRunnerConfig, MissingExecuterFileAndEmptyDataStillReturnTrue)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());

	write_query_basefiles(dir);
	write_text(dir + "/actions.json", kActions);

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);

	// 卡8b=A（迭代03 s5-runners，BUG-R134-003/WSN06-001）：配置了却不存在的执行器
	// 侧文件是配置错误，在任何副作用之前拒绝且 config 仍可重试；原用例在此处
	// 断言成功，与已裁定的全表预检冲突（.before 见 s5-runners 证据）。空 data /
	// no-store 可选模式的对照保留在随后的重试中。
	const std::string missingExecFile =
		"{" + query_basefiles(dir) + ","
		"\"executers\":\"" + dir + "/r69_absent_executers.json\","
		"\"notifier\":{\"active\":false},"
		+ tail(dir, "cta") + "}";
	EXPECT_FALSE(r->config(missingExecFile.c_str(), false));
	EXPECT_EQ(nullptr, r->getEngine());

	const std::string cfg =
		"{" + query_basefiles(dir) + ","
		"\"notifier\":{\"active\":false},"
		+ tail(dir, "cta") + "}";

	ASSERT_TRUE(r->config(cfg.c_str(), false));
	ASSERT_NE(r->getEngine(), nullptr);
	ASSERT_NE(nullptr, r->getEngine()->get_commodity_info("SHFE.au.2406"));
	EXPECT_EQ(nullptr, r->getEngine()->get_kline_slice(0, "SHFE.au.2406", "d", 1));

	clean_dir(dir);
}

TEST(R69PorterRunnerConfig, ExplicitMissingDataStoreModuleFailsConfigAndLeavesRunnerNotReady)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());
	write_query_basefiles(dir);
	write_text(dir + "/actions.json", kActions);

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);
	const std::string cfg =
		"{" + query_basefiles(dir) + ","
		"\"env\":{\"name\":\"cta\"},"
		"\"data\":{\"store\":{\"module\":\"r69_no_such_data_storage\"}},"
		"\"bspolicy\":\"" + dir + "/actions.json\"}";

	EXPECT_FALSE(r->config(cfg.c_str(), false));
	ASSERT_NE(r->getEngine(), nullptr);
	ASSERT_NE(nullptr, r->getEngine()->get_commodity_info("SHFE.au.2406"));
	EXPECT_EQ(nullptr, r->getEngine()->get_kline_slice(0, "SHFE.au.2406", "d", 1));

	ExecUnitInitStatus status;
	EXPECT_EQ(EXEC_UNIT_STATUS_API_NOT_READY,
		r->queryExecUnitInitStatus(ET_CTA, "r69_missing_exec", "SHFE.rb.2510", status));

	clean_dir(dir);
}

TEST(R69PorterRunnerConfig, ValidDataStoreModuleLoadsAndServesKlineQuery)
{
	const std::string bin = bin_dir();
	ASSERT_FALSE(bin.empty());
	ASSERT_TRUE(std::filesystem::exists(bin + "libWtDataStorage.dylib"))
		<< "CoverageTests_porter must be paired with its built WtDataStorage module";

	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());
	write_query_basefiles(dir);
	write_text(dir + "/actions.json", kActions);

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);
	const std::string oldInstDir = WtHelper::getInstDir();
	WtHelper::setInstDir(bin.c_str());
	memset(g_data_store_bars, 0, sizeof(g_data_store_bars));
	for (uint32_t i = 0; i < 2; ++i)
	{
		g_data_store_bars[i].date = 20240102 + i;
		g_data_store_bars[i].time = 1500;
		g_data_store_bars[i].open = 4500.0;
		g_data_store_bars[i].high = 4510.0;
		g_data_store_bars[i].low = 4490.0;
		g_data_store_bars[i].close = 4505.0 + i * 15.0;
		g_data_store_bars[i].vol = 10;
	}
	g_data_store_loader_runner = r;
	g_data_store_loader_calls = 0;
	r->registerExtDataLoader(&data_store_loader, &data_store_loader, nullptr, nullptr);

	const std::string cfg =
		"{" + query_basefiles(dir) + ","
		"\"env\":{\"name\":\"cta\"},"
		"\"data\":{\"store\":{\"module\":\"WtDataStorage\",\"path\":\"" + dir + "\"}},"
		"\"bspolicy\":\"" + dir + "/actions.json\"}";
	const bool configured = r->config(cfg.c_str(), false);
	WtHelper::setInstDir(oldInstDir.c_str());
	if (!configured)
		g_data_store_loader_runner = nullptr;
	ASSERT_TRUE(configured);
	ASSERT_NE(r->getEngine(), nullptr);
	ASSERT_NE(nullptr, r->getEngine()->get_commodity_info("SHFE.au.2406"));

	WTSKlineSlice* bars = r->getEngine()->get_kline_slice(0, "SHFE.au.2406", "d", 2);
	g_data_store_loader_runner = nullptr;
	ASSERT_NE(nullptr, bars) << "the real WtDataStorage reader should return the fed history bars";
	EXPECT_EQ(2, bars->size());
	ASSERT_NE(nullptr, bars->at(1));
	EXPECT_DOUBLE_EQ(4520.0, bars->at(1)->close);
	bars->release();
	EXPECT_EQ(1u, g_data_store_loader_calls);

	clean_dir(dir);
}

/* WtRtRunner.cpp:846-865 的 else 支：env.name 既不是空/cta 也不是 sel 时一律按 HFT
 * 建引擎（源码注释里写的是 //if (name=="hft")）。strategies 缺省时
 * initHftStrategies() 返回 false（:794-799），config 忽略后仍返回 true。 */
TEST(R69PorterRunnerConfig, UnknownEnvNameFallsBackToHftEngine)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());

	write_text(dir + "/actions.json", kActions);

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);

	const std::string cfg =
		"{\"basefiles\":{}," + tail(dir, "r69_not_an_engine") + "}";

	ASSERT_TRUE(r->config(cfg.c_str(), false));
	ASSERT_NE(r->getEngine(), nullptr);
	EXPECT_NE(dynamic_cast<WtHftEngine*>(r->getEngine()), nullptr);

	clean_dir(dir);
}

/* BUG-R69-001：getHotTags 返回引用，因此标签缓存必须属于当前 WTSHotMgr 实例，
 * 并在查询时反映该实例已加载的全部规则。 */
TEST(R69PorterRunnerConfig, HotTagsRefreshAfterRulesLoadedLater)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/hot.json", kHotRules);

	WTSHotMgr hot;
	ASSERT_TRUE(hot.loadCustomRules("R69FIRST", (dir + "/hot.json").c_str()));
	const std::vector<std::string>& tags = hot.getHotTags();
	ASSERT_EQ(1u, tags.size());
	EXPECT_EQ("R69FIRST", tags[0]);

	ASSERT_TRUE(hot.loadCustomRules("R69LATE", (dir + "/hot.json").c_str()));
	const std::vector<std::string>& updatedTags = hot.getHotTags();
	EXPECT_EQ(2u, updatedTags.size());
	EXPECT_NE(std::find(updatedTags.begin(), updatedTags.end(), "R69FIRST"), updatedTags.end());
	EXPECT_NE(std::find(updatedTags.begin(), updatedTags.end(), "R69LATE"), updatedTags.end())
		<< "后装载的标签未反映到当前实例的标签列表";
	// 旧引用指向同一实例缓存，第二次查询后也应看到刷新后的内容。
	EXPECT_EQ(2u, tags.size());
	EXPECT_NE(std::find(tags.begin(), tags.end(), "R69LATE"), tags.end());

	clean_dir(dir);
}

TEST(R69PorterRunnerConfig, HotTagsAreIsolatedBetweenInstances)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/hot.json", kHotRules);

	WTSHotMgr first;
	WTSHotMgr second;
	ASSERT_TRUE(first.loadCustomRules("R69INSTANCE_A", (dir + "/hot.json").c_str()));
	ASSERT_TRUE(second.loadCustomRules("R69INSTANCE_B", (dir + "/hot.json").c_str()));

	const std::vector<std::string>& firstTags = first.getHotTags();
	ASSERT_EQ(1u, firstTags.size());
	EXPECT_EQ("R69INSTANCE_A", firstTags[0]);
	const std::vector<std::string>& secondTags = second.getHotTags();
	ASSERT_EQ(1u, secondTags.size());
	EXPECT_EQ("R69INSTANCE_B", secondTags[0]);
	EXPECT_EQ("R69INSTANCE_A", firstTags[0]);

	clean_dir(dir);
}

TEST(R69PorterRunnerConfig, RepeatedHotTagQueriesDoNotDuplicateTags)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/hot.json", kHotRules);

	WTSHotMgr hot;
	ASSERT_TRUE(hot.loadCustomRules("R69REPEAT_A", (dir + "/hot.json").c_str()));
	ASSERT_TRUE(hot.loadCustomRules("R69REPEAT_B", (dir + "/hot.json").c_str()));
	const std::vector<std::string> expected = hot.getHotTags();
	ASSERT_EQ(2u, expected.size());

	for (int i = 0; i < 3; ++i)
	{
		EXPECT_EQ(expected, hot.getHotTags());
	}

	clean_dir(dir);
}

TEST(R69PorterRunnerConfig, ReleaseClearsTagsAndHotCodesBeforeReload)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/hot_before_release.json", kHotRules);
	write_text(dir + "/hot_after_release.json",
		"{\"SHFE\":{\"au\":[{\"from\":\"au2406\",\"to\":\"au2412\","
		"\"date\":20240101,\"oldclose\":100.0,\"newclose\":100.0}]}}");

	WTSHotMgr hot;
	ASSERT_TRUE(hot.loadCustomRules("R69_BEFORE_RELEASE",
		(dir + "/hot_before_release.json").c_str()));
	ASSERT_TRUE(hot.isCustomHot("R69_BEFORE_RELEASE", "SHFE.au2406", 0));
	ASSERT_EQ(1u, hot.getHotTags().size());

	hot.release();
	EXPECT_TRUE(hot.getHotTags().empty());
	EXPECT_FALSE(hot.isCustomHot("R69_BEFORE_RELEASE", "SHFE.au2406", 0));

	ASSERT_TRUE(hot.loadCustomRules("R69_AFTER_RELEASE",
		(dir + "/hot_after_release.json").c_str()));
	const std::vector<std::string>& tags = hot.getHotTags();
	ASSERT_EQ(1u, tags.size());
	EXPECT_EQ("R69_AFTER_RELEASE", tags[0]);
	EXPECT_FALSE(hot.isCustomHot("R69_BEFORE_RELEASE", "SHFE.au2406", 0));
	EXPECT_FALSE(hot.isCustomHot("R69_AFTER_RELEASE", "SHFE.au2406", 0));
	EXPECT_TRUE(hot.isCustomHot("R69_AFTER_RELEASE", "SHFE.au2412", 0));

	clean_dir(dir);
}
