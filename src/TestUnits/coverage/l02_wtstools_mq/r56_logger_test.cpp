// Lane R56 - src/WTSTools/WTSLogger.{h,cpp} 日志级别/分类/落盘/格式化
// 覆盖：init 成功路径（basic_file_sink / console_sink / ostream_sink / async_logger）、
//       str_to_level 与 str_to_ll 的 warn/fatal/未知级别分支、debug/info/warn/error/fatal
//       五个模板入口、log/log_by_cat/log_by_cat_prefix/log_dyn/log_dyn_prefix、
//       *_imp 的 root 与分类双写、ILogHandler 回调、动态 logger 创建/释放/重复释放、
//       init 二次调用早退、stop 关停。
// 每个用例由 case_runner 单独进程执行，因此静态标志位不会污染其他用例。
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "Includes/ILogHandler.h"
#include "WTSTools/WTSLogger.h"
#include "r56_tmp.h"

namespace {

class R56LogHandler : public ILogHandler
{
public:
	void handleLogAppend(WTSLogLevel ll, const char* msg) override
	{
		_levels.push_back(ll);
		_msgs.emplace_back(msg != NULL ? msg : "");
	}

	std::vector<WTSLogLevel> _levels;
	std::vector<std::string> _msgs;
};

std::string r56_basic_sink(const std::string& file, bool truncate = true)
{
	return std::string("{\"type\":\"basic_file_sink\",\"filename\":\"") + file +
		"\",\"truncate\":" + (truncate ? "true" : "false") + ",\"pattern\":\"%v\"}";
}

std::string r56_console_sink()
{
	return "{\"type\":\"console_sink\",\"pattern\":\"%v\"}";
}

std::string r56_ostream_sink()
{
	return "{\"type\":\"ostream_sink\",\"pattern\":\"%v\"}";
}

std::string r56_cfg_root(const std::string& level, const std::string& sink)
{
	return std::string("{\"root\":{\"level\":\"") + level +
		"\",\"async\":false,\"sinks\":[" + sink + "]}}";
}

}	// namespace

class R56Logger : public ::testing::Test
{
protected:
	void SetUp() override { m_dir = r56tmp::unique_dir("r56_log"); }
	void TearDown() override { r56tmp::clean_dir(m_dir); }

	std::string write_cfg(const char* name, const std::string& body)
	{
		return r56tmp::write_file(m_dir, name, body);
	}

	std::string root_file(const char* name = "root.log") const
	{
		return (m_dir / name).string();
	}

	std::filesystem::path	m_dir;
};

// WTSLogger.cpp:48-102,136-202,263-396,438-459,461-470；WTSLogger.h:160-397
TEST_F(R56Logger, InitSuccessWritesAllLevelsAndCategories)
{
	const std::string rootLog = root_file("root.log");
	const std::string dynLog = root_file("dyn.log");
	std::string cfg = "{";
	cfg += "\"root\":{\"level\":\"debug\",\"async\":false,\"sinks\":[" + r56_basic_sink(rootLog) + "]},";
	cfg += "\"r56info\":{\"level\":\"info\",\"async\":false,\"sinks\":[" + r56_console_sink() + "]},";
	cfg += "\"r56warn\":{\"level\":\"warn\",\"async\":false,\"sinks\":[" + r56_ostream_sink() + "]},";
	cfg += "\"r56error\":{\"level\":\"error\",\"async\":false,\"sinks\":[" + r56_console_sink() + "]},";
	cfg += "\"r56fatal\":{\"level\":\"fatal\",\"async\":false,\"sinks\":[" + r56_console_sink() + "]},";
	cfg += "\"r56off\":{\"level\":\"trace\",\"async\":false,\"sinks\":[" + r56_console_sink() + "]},";
	cfg += "\"dyn_pattern\":{\"r56patt\":{\"level\":\"debug\",\"async\":false,\"sinks\":[" +
		r56_basic_sink(dynLog) + "]}}";
	cfg += "}";
	const std::string file = write_cfg("ok.json", cfg);

	R56LogHandler handler;
	ASSERT_NO_THROW(WTSLogger::init(file.c_str(), true, &handler));
	ASSERT_NE(nullptr, spdlog::get("root").get());
	EXPECT_NE(nullptr, spdlog::get("r56info").get());
	EXPECT_NE(nullptr, spdlog::get("r56warn").get());
	EXPECT_NE(nullptr, spdlog::get("r56off").get());

	// 五个具名模板入口。注意：WTSLogger.h 的模板走 fmtutil(fmtlib)，占位符是 {}，
	// 不是 printf 的 %d；写成 %d 时 fmt 会原样输出（多余参数被忽略），不会替换。
	WTSLogger::debug("dbg-{}", 1);
	WTSLogger::info("info-{}", 2);
	WTSLogger::warn("warn-{}", 3);
	WTSLogger::error("error-{}", 4);
	WTSLogger::fatal("fatal-{}", 5);

	// 指定级别 + 分类（含分类前缀入口）
	WTSLogger::log(LL_DEBUG, "raw-debug");
	WTSLogger::log(LL_INFO, "raw-info");
	WTSLogger::log(LL_WARN, "raw-warn");
	WTSLogger::log(LL_ERROR, "raw-error");
	WTSLogger::log(LL_FATAL, "raw-fatal");
	WTSLogger::log_by_cat("r56warn", LL_DEBUG, "cat-debug");
	WTSLogger::log_by_cat("r56warn", LL_INFO, "cat-info");
	WTSLogger::log_by_cat("r56warn", LL_WARN, "cat-warn");
	WTSLogger::log_by_cat("r56warn", LL_ERROR, "cat-error");
	WTSLogger::log_by_cat("r56warn", LL_FATAL, "cat-fatal");
	WTSLogger::log_by_cat_prefix("r56warn", LL_INFO, "prefix-payload-{}", 99);

	// 动态 logger（首次按 dyn_pattern 创建）
	WTSLogger::log_dyn("r56patt", "r56dyn", LL_DEBUG, "dyn-debug");
	WTSLogger::log_dyn("r56patt", "r56dyn", LL_INFO, "dyn-info");
	WTSLogger::log_dyn("r56patt", "r56dyn", LL_WARN, "dyn-warn");
	WTSLogger::log_dyn("r56patt", "r56dyn", LL_ERROR, "dyn-error");
	WTSLogger::log_dyn("r56patt", "r56dyn", LL_FATAL, "dyn-fatal");
	WTSLogger::log_dyn_prefix("r56patt", "r56dyn", LL_INFO, "dynprefix-{}", 5);
	ASSERT_NE(nullptr, spdlog::get("r56dyn").get());

	// handler 每个级别都收到回调（5 具名 + 5 raw + 5 cat + 1 prefix + 5 dyn + 1 dynprefix）
	EXPECT_EQ(22u, handler._levels.size());
	EXPECT_EQ(LL_INFO, handler._levels[1]);
	EXPECT_EQ(LL_FATAL, handler._levels[4]);
	bool bHasFatal = false;
	for (WTSLogLevel ll : handler._levels)
	{
		if (ll == LL_FATAL)
			bHasFatal = true;
	}
	EXPECT_TRUE(bHasFatal);

	spdlog::get("root")->flush();
	spdlog::get("r56dyn")->flush();
	const std::string rootContent = r56tmp::read_file(rootLog);
	EXPECT_NE(std::string::npos, rootContent.find("dbg-1"))
		<< "root level=" << (int)spdlog::get("root")->level()
		<< " size=" << rootContent.size() << " content=[" << rootContent << "]";
	EXPECT_NE(std::string::npos, rootContent.find("info-2"));
	EXPECT_NE(std::string::npos, rootContent.find("warn-3"));
	EXPECT_NE(std::string::npos, rootContent.find("error-4"));
	EXPECT_NE(std::string::npos, rootContent.find("fatal-5"));
	EXPECT_NE(std::string::npos, rootContent.find("raw-debug"));
	EXPECT_NE(std::string::npos, rootContent.find("raw-fatal"));
	EXPECT_NE(std::string::npos, rootContent.find("cat-info"));
	EXPECT_NE(std::string::npos, rootContent.find("[r56warn]"));
	EXPECT_NE(std::string::npos, rootContent.find("dyn-debug"));
	EXPECT_NE(std::string::npos, rootContent.find("dyn-fatal"));

	WTSLogger::freeAllDynLoggers();
	EXPECT_EQ(nullptr, spdlog::get("r56dyn").get());
	WTSLogger::stop();
	EXPECT_EQ(nullptr, spdlog::get("root").get());
}

// WTSLogger.cpp:204-248（二次 init 早退）
TEST_F(R56Logger, SecondInitReturnsWithoutReconfiguring)
{
	const std::string firstLog = root_file("first.log");
	const std::string secondLog = root_file("second.log");
	const std::string first = write_cfg("first.json", r56_cfg_root("debug", r56_basic_sink(firstLog)));
	const std::string second = write_cfg("second.json", r56_cfg_root("info", r56_basic_sink(secondLog)));

	ASSERT_NO_THROW(WTSLogger::init(first.c_str(), true, NULL));
	WTSLogger::debug("before-{}", 1);
	spdlog::get("root")->flush();

	// m_bInited 已置位：第二次 init 直接返回，不读取第二份配置
	ASSERT_NO_THROW(WTSLogger::init(second.c_str(), true, NULL));
	WTSLogger::debug("after-{}", 2);
	spdlog::get("root")->flush();
	EXPECT_FALSE(std::filesystem::exists(secondLog));

	const std::string content = r56tmp::read_file(firstLog);
	EXPECT_NE(std::string::npos, content.find("before-1"))
		<< "root level=" << (int)spdlog::get("root")->level()
		<< " size=" << content.size() << " content=[" << content << "]";
	EXPECT_NE(std::string::npos, content.find("after-2"));
	WTSLogger::stop();
}

// WTSLogger.cpp:76-102（root=info）；WTSLogger.h:186-201
TEST_F(R56Logger, RootInfoFiltersDebug)
{
	const std::string log = root_file("info.log");
	const std::string file = write_cfg("info.json", r56_cfg_root("info", r56_basic_sink(log)));
	ASSERT_NO_THROW(WTSLogger::init(file.c_str(), true, NULL));
	WTSLogger::debug("filtered-debug");
	WTSLogger::info("kept-info");
	spdlog::get("root")->flush();
	const std::string content = r56tmp::read_file(log);
	EXPECT_EQ(std::string::npos, content.find("filtered-debug"));
	EXPECT_NE(std::string::npos, content.find("kept-info"));
	WTSLogger::stop();
}

// WTSLogger.cpp:76-102（root=warn）；WTSLogger.h:212-227
TEST_F(R56Logger, RootWarnFiltersInfo)
{
	const std::string log = root_file("warn.log");
	const std::string file = write_cfg("warn.json", r56_cfg_root("warn", r56_basic_sink(log)));
	ASSERT_NO_THROW(WTSLogger::init(file.c_str(), true, NULL));
	WTSLogger::info("filtered-info");
	WTSLogger::warn("kept-warn");
	WTSLogger::error("kept-error");
	spdlog::get("root")->flush();
	const std::string content = r56tmp::read_file(log);
	EXPECT_EQ(std::string::npos, content.find("filtered-info"));
	EXPECT_NE(std::string::npos, content.find("kept-warn"));
	EXPECT_NE(std::string::npos, content.find("kept-error"));
	WTSLogger::stop();
}

// WTSLogger.cpp:76-102（root=fatal）；WTSLogger.h:264-279
TEST_F(R56Logger, RootFatalFiltersLowerLevels)
{
	const std::string log = root_file("fatal.log");
	const std::string file = write_cfg("fatal.json", r56_cfg_root("fatal", r56_basic_sink(log)));
	ASSERT_NO_THROW(WTSLogger::init(file.c_str(), true, NULL));
	WTSLogger::error("filtered-error");
	WTSLogger::fatal("kept-fatal");
	spdlog::get("root")->flush();
	const std::string content = r56tmp::read_file(log);
	EXPECT_EQ(std::string::npos, content.find("filtered-error"));
	EXPECT_NE(std::string::npos, content.find("kept-fatal"));
	WTSLogger::stop();
}

// WTSLogger.cpp:76-102（未知级别 -> LL_NONE/off）,255-261,323-354,356-396,398-435
TEST_F(R56Logger, UnknownRootLevelDisablesAndStopShutsDown)
{
	const std::string log = root_file("off.log");
	const std::string file = write_cfg("off.json", r56_cfg_root("trace", r56_basic_sink(log)));
	ASSERT_NO_THROW(WTSLogger::init(file.c_str(), true, NULL));

	// root 级别映射为 LL_NONE：低于 NONE 的级别全部被过滤
	WTSLogger::log(LL_INFO, "filtered-info");
	WTSLogger::debug("filtered-debug");
	// LL_NONE 本身可以通过级别门，但 switch 没有对应 case，只走 default
	WTSLogger::log(LL_NONE, "none-raw");
	WTSLogger::log_by_cat("root", LL_NONE, "none-cat");
	WTSLogger::log_by_cat_prefix("root", LL_NONE, "none-prefix");
	WTSLogger::log_dyn("missing-pattern", "none-dyn", LL_NONE, "none-dyn");
	WTSLogger::log_dyn_prefix("missing-pattern", "none-dyn2", LL_NONE, "none-dyn2");
	spdlog::get("root")->flush();
	EXPECT_TRUE(r56tmp::read_file(log).empty());

	ASSERT_NO_THROW(WTSLogger::stop());
	EXPECT_EQ(nullptr, spdlog::get("root").get());
	// 关停后所有入口都直接返回
	EXPECT_NO_THROW(WTSLogger::log(LL_NONE, "after-stop"));
}

// WTSLogger.cpp:185-196,461-470；WTSLogger.h:384-397
TEST_F(R56Logger, AsyncLoggerAndDynReleaseIdempotent)
{
	const std::string rootLog = root_file("asyncroot.log");
	const std::string dynLog = root_file("asyncdyn.log");
	std::string cfg = "{";
	cfg += "\"root\":{\"level\":\"debug\",\"async\":false,\"sinks\":[" + r56_basic_sink(rootLog) + "]},";
	cfg += "\"r56async\":{\"level\":\"info\",\"async\":true,\"sinks\":[" + r56_ostream_sink() + "]},";
	cfg += "\"dyn_pattern\":{\"r56patt2\":{\"level\":\"info\",\"async\":false,\"sinks\":[" +
		r56_basic_sink(dynLog) + "]}}";
	cfg += "}";
	const std::string file = write_cfg("async.json", cfg);

	ASSERT_NO_THROW(WTSLogger::init(file.c_str(), true, NULL));
	SpdLoggerPtr asyncLogger = spdlog::get("r56async");
	ASSERT_NE(nullptr, asyncLogger.get());
	EXPECT_NO_THROW(WTSLogger::log_by_cat("r56async", LL_INFO, "async-{}", 1));

	WTSLogger::log_dyn("r56patt2", "r56dyn2", LL_INFO, "dyn2-{}", 2);
	ASSERT_NE(nullptr, spdlog::get("r56dyn2").get());
	WTSLogger::freeAllDynLoggers();
	EXPECT_EQ(nullptr, spdlog::get("r56dyn2").get());
	// 第二次释放：集合里仍留有名字，但 registry 已无对应 logger，走 continue
	EXPECT_NO_THROW(WTSLogger::freeAllDynLoggers());
	ASSERT_NO_THROW(WTSLogger::stop());
}

// WTSLogger.cpp:398-435,438-459；WTSLogger.h:361-370
TEST_F(R56Logger, UninitializedDynamicLogFallsBackToConsolePath)
{
	// 未 init 且没有 dyn_pattern：log_dyn 的 getLogger 返回空，回落 root(NULL) 后
	// 走 !m_bInited 的时间戳打印分支
	EXPECT_NO_THROW(WTSLogger::log_dyn("no-pattern", "r56uninit", LL_NONE, "uninit-{}", 7));
	EXPECT_EQ(nullptr, spdlog::get("r56uninit").get());
	EXPECT_NO_THROW(WTSLogger::log_by_cat("r56nolog", LL_NONE, "uninit-cat"));
	EXPECT_EQ(nullptr, spdlog::get("r56nolog").get());
}

// 产品缺陷占位：log_by_cat_prefix 完全忽略 format/args，只输出 "[catName]"，
// 格式化正文丢失。见 docs/迭代计划/迭代01/bugs/R56.md BUG-R56-002；默认不执行。
// WTSLogger.h:335-347
TEST_F(R56Logger, DISABLED_LogByCatPrefixKeepsFormattedPayload)
{
	const std::string log = root_file("prefix.log");
	const std::string file = write_cfg("prefix.json", r56_cfg_root("debug", r56_basic_sink(log)));
	ASSERT_NO_THROW(WTSLogger::init(file.c_str(), true, NULL));
	WTSLogger::log_by_cat_prefix("r56cat", LL_INFO, "payload-{}", 42);
	spdlog::get("root")->flush();
	const std::string content = r56tmp::read_file(log);
	EXPECT_NE(std::string::npos, content.find("[r56cat]"));
	EXPECT_NE(std::string::npos, content.find("payload-42"));
	WTSLogger::stop();
}
