/*
 * R46 - WtRtRunner 配置引导链（init -> config -> initEngine/initDataMgr/
 *        initActionPolicy/initParsers/initTraders/initExecuters/init*Strategies）
 *
 * 目标：src/WtPorter/WtRtRunner.cpp
 *   - init() L107-127
 *   - config() L489-714（basefiles/parsers/traders/executers/strategies 各段）
 *   - initEngine() L838-882（cta / hft / sel 三支）
 *   - initDataMgr() L884-896、initActionPolicy() L1157-1166
 *   - initParsers() L898-931、initTraders() L1065-1089
 *   - initExecuters() L933-1052（local/diff/arbi/dist 四个分支）
 *   - initCtaStrategies() L716-743、initHftStrategies() L791-836 的空表支
 *   - createExtParser() L295-303、get_raw_stdcode() L361-366
 *   - createHftContext() 的「绑定已存在 trader」与「trader 不存在」两支 L321-336
 *
 * 与既有 lane 的分工：q06/r6/r13/r35 只驱动过分派层、init()、feed 链；
 * config() 及其 init* 辅助函数在报告里整段未覆盖（manifest wave10/R6 只列了零星 logger 行）。
 * 这里用「堆上 new、故意不 delete」的 runner 驱动成功路径：
 *   - 不能用栈对象：WtRtRunner 的成员引擎 _cfg 未初始化，析构会释放野指针（BUG-R46-001）；
 *   - 不能碰 getRunner() 单例：r6 用例断言单例 getEngine()==nullptr，config 会打破它。
 * 所有落盘都在系统临时目录的专属子目录，用例末尾 remove_all。
 *
 * 已登记缺陷见 docs/迭代计划/迭代01/bugs/R46.md。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "WtCore/WtHelper.h"
#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

USING_NS_WTP;

namespace {

	const char* kGenLeaf = "wt_r46_porter_gen";

	std::string prepare_gen_dir()
	{
		std::filesystem::path p = std::filesystem::temp_directory_path() / kGenLeaf;
		std::error_code ec;
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

	std::string tail_char(const std::string& s)
	{
		return s.empty() ? std::string() : s.substr(s.size() - 1);
	}

	/*
	 * 堆上构造、永不 delete：见文件头说明与 BUG-R46-001。
	 * init() 用不存在的日志文件走 WTSLogger::init 的 exists 守卫，不起日志线程。
	 */
	WtRtRunner* new_runner(const std::string& genDir)
	{
		WtRtRunner* r = new WtRtRunner();
		r->init((genDir + "/r46_absent_logcfg.yaml").c_str(), true, genDir.c_str());
		return r;
	}

} // namespace

/* WtRtRunner.cpp:107-127 + 489-591 + 838-882(cta) + 884-896 + 1157-1166 + 716-743
 * 最小可用 cta 配置：basefiles/env/data/bspolicy 全给，parsers/traders/executers
 * 都是 active:false，strategies.cta 也是空跑，因此不会加载任何插件。 */
TEST(R46PorterConfig, CtaEnvBootstrapsEngineDataMgrAndActionPolicy)
{
	const std::string dir = prepare_gen_dir();
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", "{}");

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);

	const std::string cfg =
		"{\"basefiles\":{},\"env\":{\"name\":\"cta\"},\"data\":{},"
		"\"bspolicy\":\"" + dir + "/actions.json\","
		"\"parsers\":[{\"active\":false,\"id\":\"r46_p_off\"}],"
		"\"traders\":[{\"active\":false,\"id\":\"r46_t_off\"}],"
		"\"executers\":[{\"active\":false,\"id\":\"r46_e_off\"}],"
		"\"strategies\":{\"cta\":[{\"active\":false,\"id\":\"r46_s_off\",\"name\":\"r46_n_off\"}]}}";

	ASSERT_TRUE(r->config(cfg.c_str(), false));
	ASSERT_NE(r->getEngine(), nullptr);
	EXPECT_NE(dynamic_cast<WtCtaEngine*>(r->getEngine()), nullptr);
	EXPECT_FALSE(WtHelper::getInstDir().empty());

	clean_dir(dir);
}

/* WtRtRunner.cpp:838-882 的 hft 支（L860-865）+ 791-836 的空 strategies.hft 支 */
TEST(R46PorterConfig, HftEnvSelectsHftEngineAndEmptyStrategyList)
{
	const std::string dir = prepare_gen_dir();
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", "{}");

	WtRtRunner* r = new_runner(dir);
	const std::string cfg =
		"{\"basefiles\":{},\"env\":{\"name\":\"hft\"},\"data\":{},"
		"\"bspolicy\":\"" + dir + "/actions.json\","
		"\"strategies\":{\"hft\":[{\"active\":false,\"id\":\"r46_hft_off\",\"name\":\"r46_n_off\"}]}}";

	ASSERT_TRUE(r->config(cfg.c_str(), false));
	ASSERT_NE(r->getEngine(), nullptr);
	EXPECT_NE(dynamic_cast<WtHftEngine*>(r->getEngine()), nullptr);

	clean_dir(dir);
}

/* WtRtRunner.cpp:838-882 的 sel 支（L866-871）；注意 config() 选股模式下仍走
 * initCtaStrategies()（L708-711），此处 strategies 缺省，覆盖该函数 L718-724 的早退。 */
TEST(R46PorterConfig, SelEnvSelectsSelEngine)
{
	const std::string dir = prepare_gen_dir();
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", "{}");

	WtRtRunner* r = new_runner(dir);
	const std::string cfg =
		"{\"basefiles\":{},\"env\":{\"name\":\"sel\"},\"data\":{},"
		"\"bspolicy\":\"" + dir + "/actions.json\"}";

	ASSERT_TRUE(r->config(cfg.c_str(), false));
	ASSERT_NE(r->getEngine(), nullptr);
	EXPECT_NE(dynamic_cast<WtSelEngine*>(r->getEngine()), nullptr);

	clean_dir(dir);
}

/* WtRtRunner.cpp:1065-1089（traders 数组 + active 项，模块缺失时 init 返回 false
 * 但适配器仍入表）+ 933-1052（local/diff/arbi/dist 四条构造支，含
 * 「已配置 trader」与「Trader not exists」两支）+ 321-336（HFT 绑定 trader 两分支） */
TEST(R46PorterConfig, TradersAndEveryExecuterKindAreInstantiated)
{
	const std::string dir = prepare_gen_dir();
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", "{}");

	WtRtRunner* r = new_runner(dir);
	const std::string cfg =
		"{\"basefiles\":{},\"env\":{\"name\":\"cta\"},\"data\":{},"
		"\"bspolicy\":\"" + dir + "/actions.json\","
		"\"traders\":[{\"active\":true,\"id\":\"r46_trd\"}],"
		"\"executers\":["
		"{\"active\":true,\"id\":\"r46_exe_local\",\"name\":\"local\",\"trader\":\"r46_trd\"},"
		"{\"active\":true,\"id\":\"r46_exe_diff\",\"name\":\"diff\",\"trader\":\"r46_trd\"},"
		"{\"active\":true,\"id\":\"r46_exe_arbi\",\"name\":\"arbi\",\"trader\":\"r46_trd\"},"
		"{\"active\":true,\"id\":\"r46_exe_dist\",\"name\":\"dist\"},"
		"{\"active\":true,\"id\":\"r46_exe_orphan\",\"name\":\"local\",\"trader\":\"r46_absent_trd\"}"
		"]}";

	ASSERT_TRUE(r->config(cfg.c_str(), false));
	ASSERT_NE(r->getEngine(), nullptr);

	const uint32_t bound = r->createHftContext("r46_hft_bound", "r46_trd", false, 0);
	EXPECT_NE(bound, 0u);
	EXPECT_TRUE(r->getHftContext(bound) != nullptr);

	const uint32_t unbound = r->createHftContext("r46_hft_unbound", "r46_absent_trd", false, 0);
	EXPECT_NE(unbound, 0u);
	EXPECT_TRUE(r->getHftContext(unbound) != nullptr);

	clean_dir(dir);
}

/* WtRtRunner.cpp:594-624（parsers 的 array 支与「文件不存在」告警支）、
 * 627-657（traders 的 string 支，文件不存在）、665-700（executers 的 string 支，
 * 文件存在且 executers 为空表）。三个 runner 各自独立，互不共享状态。 */
TEST(R46PorterConfig, ParserAndTraderSectionsSupportInlineArrayAndExternalFile)
{
	const std::string dir = prepare_gen_dir();
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", "{}");
	write_text(dir + "/parsers.json", "{\"parsers\":[{\"active\":false,\"id\":\"r46_p_file\"}]}");
	write_text(dir + "/executers.json", "{\"executers\":[]}");

	{
		WtRtRunner* r = new_runner(dir);
		const std::string cfg =
			"{\"basefiles\":{},\"env\":{\"name\":\"cta\"},\"data\":{},"
			"\"bspolicy\":\"" + dir + "/actions.json\","
			"\"parsers\":[{\"active\":false,\"id\":\"r46_p_inline\"}],"
			"\"traders\":\"" + dir + "/r46_absent_traders.json\"}";
		EXPECT_TRUE(r->config(cfg.c_str(), false));
		EXPECT_NE(r->getEngine(), nullptr);
	}

	{
		WtRtRunner* r = new_runner(dir);
		const std::string cfg =
			"{\"basefiles\":{},\"env\":{\"name\":\"cta\"},\"data\":{},"
			"\"bspolicy\":\"" + dir + "/actions.json\","
			"\"parsers\":\"" + dir + "/parsers.json\","
			"\"executers\":\"" + dir + "/executers.json\"}";
		EXPECT_TRUE(r->config(cfg.c_str(), false));
		EXPECT_NE(r->getEngine(), nullptr);
	}

	clean_dir(dir);
}

/* WtRtRunner.cpp:1159-1161 的空 bspolicy 支：config() 在 initDataMgr 之后
 * 因 initActionPolicy 失败而返回 false，但引擎/数据管理器已经建好。 */
TEST(R46PorterConfig, MissingBspolicyStopsConfigAfterDataManager)
{
	const std::string dir = prepare_gen_dir();
	ASSERT_FALSE(dir.empty());

	WtRtRunner* r = new_runner(dir);
	const std::string cfg = "{\"basefiles\":{},\"env\":{\"name\":\"cta\"},\"data\":{}}";

	EXPECT_FALSE(r->config(cfg.c_str(), false));
	EXPECT_NE(r->getEngine(), nullptr);

	clean_dir(dir);
}

/* WtRtRunner.cpp:840-842: env 缺失时 initEngine() 返回 false，配置入口应拒绝该配置。
 * 不读取可能未初始化的 _engine；缺陷见 bugs/R46.md BUG-R46-002。 */
TEST(R46PorterConfig, MissingEnvMakesConfigFail)
{
	const std::string dir = prepare_gen_dir();
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", "{}");

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);
	const std::string cfg =
		"{\"basefiles\":{},\"data\":{},"
		"\"bspolicy\":\"" + dir + "/actions.json\"}";

	EXPECT_FALSE(r->config(cfg.c_str(), false));

	clean_dir(dir);
}

/* WtRtRunner.cpp:295-303（createExtParser，需要 _engine 非空）+ 361-366
 * （get_raw_stdcode，空基础数据下 CodeHelper 无规则码，返回空串而非 NULL）。 */
TEST(R46PorterConfig, ExtParserAndRawCodeWorkOnceEngineIsConfigured)
{
	const std::string dir = prepare_gen_dir();
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", "{}");

	WtRtRunner* r = new_runner(dir);
	const std::string cfg =
		"{\"basefiles\":{},\"env\":{\"name\":\"cta\"},\"data\":{},"
		"\"bspolicy\":\"" + dir + "/actions.json\"}";
	ASSERT_TRUE(r->config(cfg.c_str(), false));
	ASSERT_NE(r->getEngine(), nullptr);

	EXPECT_TRUE(r->createExtParser("r46_ext_parser"));

	const char* raw = r->get_raw_stdcode("IF2108.CFFEX");
	ASSERT_NE(raw, nullptr);
	EXPECT_STREQ(raw, "");
	EXPECT_EQ(tail_char(WtHelper::getInstDir()), "/");

	clean_dir(dir);
}
