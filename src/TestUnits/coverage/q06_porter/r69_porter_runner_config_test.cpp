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
 *   - initDataMgr() 空 data/no-store 配置（L884-896，config 不检查返回值）
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
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "WtCore/WtHelper.h"
#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

#include "Includes/WTSVariant.hpp"
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

	const char* kCommodityOne =
		"{\"SHFE\":{\"au\":{\"name\":\"Gold\",\"session\":\"TRADING\","
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
	write_text(dir + "/executers.json",
		"{\"executers\":[],\"routers\":{}}");
	write_text(dir + "/actions.json", kActions);

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);

	const std::string cfg =
		"{\"basefiles\":{},"
		"\"traders\":\"" + dir + "/traders.json\","
		"\"executers\":\"" + dir + "/executers.json\","
		"\"routers\":{},"
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

/* WtRtRunner.cpp:691-694 + :884-896 + :1054-1063：executers 指向不存在的文件只告警；
 * tail() 固定附加 data:{}，此用例覆盖空 data/no-store 配置；不会声称覆盖缺失 data 键。
 * initDataMgr() 的返回值不被 config() 检查，config() 继续走；
 * notifier 是对象（active=false，EventNotifier::init 内部返回 false，不加载 MQ 模块），
 * initEvtNotifier() 仍返回 true。 */
TEST(R69PorterRunnerConfig, MissingExecuterFileAndEmptyDataStillReturnTrue)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());

	write_text(dir + "/actions.json", kActions);

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);

	const std::string cfg =
		"{\"basefiles\":{},"
		"\"executers\":\"" + dir + "/r69_absent_executers.json\","
		"\"notifier\":{\"active\":false},"
		+ tail(dir, "cta") + "}";

	ASSERT_TRUE(r->config(cfg.c_str(), false));
	ASSERT_NE(r->getEngine(), nullptr);

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
