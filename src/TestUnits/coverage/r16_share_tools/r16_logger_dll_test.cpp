/*!
 * \file r16_logger_dll_test.cpp
 * \brief R16 覆盖率用例：src/WTSTools/WTSLogger.h（fmt 模板入口 + 未初始化分支）
 *                         + src/Share/DLLHelper.hpp + src/Share/ModuleHelper.hpp
 *
 * 约束：不启动 spdlog 线程池（配置里 async 一律 false），不调用会走到
 *       spdlog::flush_every 的成功 init 分支（那会拉起后台 flush 线程），
 *       因此这里用「缺 root 分类 → init 抛 runtime_error」这条路径，
 *       既能覆盖 init/initLogger/getLogger/freeAllDynLoggers，又不产生任何线程。
 *       断言的可观测面是 spdlog 注册表（spdlog::get/drop），不依赖挂钟。
 */
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <spdlog/spdlog.h>
#include "WTSTools/WTSLogger.h"
#include "Share/DLLHelper.hpp"
#include "Share/ModuleHelper.hpp"
#include "r16_tmp.h"

namespace {

// 只有 syscat（没有 root）：init 会先注册 syscat，再在取 root 时抛异常
const char* CFG_NO_ROOT = R"({
	"syscat": {
		"level": "info",
		"async": false,
		"sinks": [ { "type": "console_sink", "pattern": "%v" } ]
	}
})";

// dyn_pattern 里放一个模板；root 依然缺失 → init 末尾抛异常，但 m_mapPatterns 已经建好
const char* CFG_WITH_DYN = R"({
	"syscat": {
		"level": "info",
		"async": false,
		"sinks": [ { "type": "console_sink", "pattern": "%v" } ]
	},
	"dyn_pattern": {
		"mypatt": {
			"level": "debug",
			"async": false,
			"sinks": [ { "type": "console_sink", "pattern": "%v" } ]
		}
	}
})";

}	// namespace

//////////////////////////////////////////////////////////////////////////
// WTSLogger
TEST(r16_wtslogger, init_returns_silently_when_config_file_is_missing)
{
	const std::string path = r16tmp::file("log1", "no_such_logcfg.json");
	r16tmp::clean("log1");
	// isFile=true 且文件不存在 → 直接 return，既不抛也不置 m_bInited
	EXPECT_NO_THROW(WTSLogger::init(path.c_str(), true, NULL));
	EXPECT_EQ(nullptr, spdlog::get("root").get());
	spdlog::drop("syscat");
	r16tmp::clean("log1");
}

TEST(r16_wtslogger, init_registers_logger_then_throws_without_root)
{
	// 抛异常之前 initLogger 已经把 syscat 注册进 spdlog，注册表就是可证伪的观测点
	EXPECT_NO_THROW(spdlog::get("syscat"));
	ASSERT_EQ(nullptr, spdlog::get("syscat").get());
	EXPECT_THROW(WTSLogger::init(CFG_NO_ROOT, false, NULL), std::runtime_error);
	SpdLoggerPtr cat = spdlog::get("syscat");
	ASSERT_NE(nullptr, cat.get());
	EXPECT_EQ(std::string("syscat"), cat->name());
	spdlog::drop("syscat");
	EXPECT_EQ(nullptr, spdlog::get("syscat").get());
}

TEST(r16_wtslogger, register_handler_accepts_null_and_real_handler)
{
	// 只是把指针存进静态成员；不崩溃、能反复调用即是本用例的可证伪点
	EXPECT_NO_THROW(WTSLogger::registerHandler(NULL));
	SUCCEED();
}

TEST(r16_wtslogger, log_at_none_level_reaches_uninited_print_path)
{
	// m_logLevel 初值是 LL_NONE，模板里 `m_logLevel > ll` 只对 ll < LL_NONE 成立，
	// 所以未初始化状态下唯一可达的分支是 ll == LL_NONE（走 print_message）。
	// 该路径只往 stdout 打印，不建日志器、不起线程。
	std::vector<int> ignored;
	EXPECT_NO_THROW(WTSLogger::log(LL_NONE, "r16 plain message"));
	EXPECT_NO_THROW(WTSLogger::log(LL_NONE, "r16 formatted %d/%d", 7, 42));
	EXPECT_NO_THROW(WTSLogger::log_by_cat("syscat", LL_NONE, "r16 cat %s", "x"));
	EXPECT_NO_THROW(WTSLogger::debug("never printed %d", 1));
	EXPECT_NO_THROW(WTSLogger::info("never printed %d", 2));
	EXPECT_NO_THROW(WTSLogger::warn("never printed %d", 3));
	EXPECT_NO_THROW(WTSLogger::error("never printed %d", 4));
	EXPECT_NO_THROW(WTSLogger::fatal("never printed %d", 5));
	SUCCEED();
}

TEST(r16_wtslogger, log_dyn_creates_dynamic_logger_and_free_all_drops_it)
{
	ASSERT_EQ(nullptr, spdlog::get("dyn1").get());
	EXPECT_THROW(WTSLogger::init(CFG_WITH_DYN, false, NULL), std::runtime_error);

	// 未初始化 + LL_NONE → log_dyn_raw → getLogger("dyn1","mypatt") → 按模板动态建日志器
	WTSLogger::log_dyn("mypatt", "dyn1", LL_NONE, "hello %d", 9);
	SpdLoggerPtr dyn = spdlog::get("dyn1");
	ASSERT_NE(nullptr, dyn.get());
	EXPECT_EQ(std::string("dyn1"), dyn->name());

	WTSLogger::freeAllDynLoggers();
	EXPECT_EQ(nullptr, spdlog::get("dyn1").get());

	// 未知模板：getLogger 走 m_mapPatterns->get 未命中分支，不会新建日志器
	WTSLogger::log_dyn("no_such_pattern", "dyn2", LL_NONE, "hello");
	EXPECT_EQ(nullptr, spdlog::get("dyn2").get());

	spdlog::drop("syscat");
}

TEST(r16_wtslogger, log_dyn_prefix_builds_bracket_prefix)
{
	EXPECT_THROW(WTSLogger::init(CFG_WITH_DYN, false, NULL), std::runtime_error);
	// catName 会被写进 m_buffer 头部 "[cat]"，再交给 log_dyn_raw
	EXPECT_NO_THROW(WTSLogger::log_dyn_prefix("mypatt", "dynp", LL_NONE, "v=%d", 1));
	ASSERT_NE(nullptr, spdlog::get("dynp").get());
	WTSLogger::freeAllDynLoggers();
	EXPECT_EQ(nullptr, spdlog::get("dynp").get());
	spdlog::drop("syscat");
}

TEST(r16_wtslogger, log_by_cat_prefix_should_format_the_message)
{
	testing::internal::CaptureStdout();
	WTSLogger::log_by_cat_prefix("cat", LL_NONE, "value={}", 5);
	const std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(std::string::npos, output.find("[cat]value=5"));
}

TEST(r16_wtslogger, log_by_cat_prefix_ignores_null_category)
{
	testing::internal::CaptureStdout();
	EXPECT_NO_THROW(WTSLogger::log_by_cat_prefix(nullptr, LL_NONE, "ignored"));
	const std::string output = testing::internal::GetCapturedStdout();

	EXPECT_TRUE(output.empty());
}

TEST(r16_wtslogger, log_by_cat_prefix_truncates_long_category_without_overflow)
{
	const std::string category(MAX_LOG_BUF_SIZE * 2, 'c');
	const std::string body = "tail";
	const size_t expectedCategoryLength = MAX_LOG_BUF_SIZE - 1 - 2 - body.size();
	const std::string expected = "[" + category.substr(0, expectedCategoryLength) + "]" + body;

	testing::internal::CaptureStdout();
	EXPECT_NO_THROW(WTSLogger::log_by_cat_prefix(category.c_str(), LL_NONE, "{}", body));
	const std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(std::string::npos, output.find(expected));
	EXPECT_LE(output.size(), static_cast<size_t>(MAX_LOG_BUF_SIZE + 64));
}

//////////////////////////////////////////////////////////////////////////
// DLLHelper
TEST(r16_dllhelper, wrap_module_prefixes_lib_and_dylib)
{
#ifdef __APPLE__
	const char* ext = ".dylib";
#else
	const char* ext = ".so";
#endif
	const std::string expect = std::string("lib") + "parser_ctp" + ext;
	EXPECT_EQ(expect, DLLHelper::wrap_module("parser_ctp"));

	// 自定义前缀参数直接拼在名字前面
	const std::string expect2 = std::string("wt") + "yd" + ext;
	EXPECT_EQ(expect2, DLLHelper::wrap_module("yd", "wt"));

	// 已经带 lib 前缀的名字会被再加一次 lib（调用方常见的坑）
	const std::string expect3 = std::string("lib") + "libfoo" + ext;
	EXPECT_EQ(expect3, DLLHelper::wrap_module("libfoo"));
}

TEST(r16_dllhelper, wrap_module_moves_leading_non_alpha_before_prefix)
{
#ifdef __APPLE__
	// "_sm" → 首个字母在下标 1 → std::string(name, 1) 取到 "_"，再拼 "lib"+"sm"
	const std::string got = DLLHelper::wrap_module("_sm");
	EXPECT_EQ(std::string("_libsm.dylib"), got);
#endif
}

TEST(r16_dllhelper, wrap_module_without_alpha_uses_numeric_prefix)
{
#ifdef _WIN32
	EXPECT_EQ(std::string("123.dll"), DLLHelper::wrap_module("123"));
#else
#ifdef __APPLE__
	const char* ext = ".dylib";
#else
	const char* ext = ".so";
#endif
	EXPECT_EQ(std::string("lib123") + ext, DLLHelper::wrap_module("123"));
#endif
}

TEST(r16_dllhelper, wrap_module_without_alpha_inputs_terminate)
{
#ifdef _WIN32
	const char* ext = ".dll";
#else
#ifdef __APPLE__
	const char* ext = ".dylib";
#else
	const char* ext = ".so";
#endif
#endif
	const char nonAsciiName[] = { static_cast<char>(0xE9), '\0' };
	const char* names[] = { "", "_.-", nonAsciiName };
	for (const char* name : names)
	{
		std::string wrapped;
		EXPECT_NO_THROW(wrapped = DLLHelper::wrap_module(name));
		EXPECT_GE(wrapped.size(), std::strlen(ext));
		EXPECT_EQ(std::string(ext), wrapped.substr(wrapped.size() - std::strlen(ext)));
	}
}

TEST(r16_dllhelper, load_missing_library_returns_null_and_helpers_tolerate_it)
{
	const std::string missing = r16tmp::file("dll1", "no_such_module.dylib");
	r16tmp::clean("dll1");
	DllHandle h = DLLHelper::load_library(missing.c_str());
	EXPECT_EQ(nullptr, h);
	// NULL 句柄上的两个入口都must安全返回
	EXPECT_NO_THROW(DLLHelper::free_library(h));
	EXPECT_EQ(nullptr, DLLHelper::get_symbol(h, "anything"));
}

//////////////////////////////////////////////////////////////////////////
// ModuleHelper
TEST(r16_modulehelper, get_bin_dir_returns_existing_directory_with_trailing_slash)
{
	const char* dir = getBinDir();
	ASSERT_NE(nullptr, dir);
	const std::string s(dir);
	EXPECT_LT((size_t)0, s.size());
	EXPECT_EQ('/', s[s.size() - 1]);
	EXPECT_TRUE(std::filesystem::is_directory(s));

	// 第二次调用直接返回缓存（同一个 string 对象，地址相同）
	EXPECT_EQ((const void*)dir, (const void*)getBinDir());
}
