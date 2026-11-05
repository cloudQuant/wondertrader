// R7 lane：WtExecRunner（执行监控运行器，报表里该文件 0.00%）
// 目标文件: src/WtExecMon/WtExecRunner.cpp / .h
// 覆盖面: 构造、init 的两条日志配置分支、config 的拒绝分支（文件缺失/内容非法/空内容/缺少 basefiles）、
//         handle_push_quote 的空指针防御、仓位缓存提交、run 的空适配器路径、
//         get_comm_info / get_sess_info / get_session_info 在基础数据未加载时的 NULL 返回。
//
// 刻意不碰的面（详见 bugs/R7_exefact.md）：
//  * WTSLogger 成功初始化会启动周期 flush worker；常规用例不在本进程初始化成功，
//    BUG-R7-011 的真实日志断言仅在 threadsafe 子进程内初始化并显式 stop/flush；
//  * release() -> WTSLogger::stop() 的未初始化状态行为不确定 -> 仍不在普通用例中调用；
//  * initParsers/initExecuters 会 new ParserAdapter 或 dlopen 执行器工厂，initDataMgr 会按
//    配置打开行情存储（可能涉及共享内存）-> 全部留给主循环评审；交易通道仅使用本地
//    TraderMocker 插件做 config/init 正控，不调用 run/connect；
//  * addExeFactories 会 dlopen executer 目录下的插件，与 WtCtaStraFact 等同名符号冲突
//    （BUG-L00-003 实测 34 个 duplicate symbol）-> 不调用。
#include <unistd.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>

#include "Includes/ILogHandler.h"
#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "WTSTools/WTSLogger.h"
#include "WtExecMon/WtExecRunner.h"
#include "../trader_mocker_fixture.h"

using namespace wtp;

namespace {

	std::string r7_temp_dir()
	{
		const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
		std::filesystem::path dir = std::filesystem::temp_directory_path()
			/ ("r7_trader_init_" + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(nonce));
		std::filesystem::create_directories(dir);
		return dir.string();
	}

	void r7_write_text(const std::string& path, const std::string& text)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << text;
	}

	void r7_remove_dir(const std::string& dir)
	{
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}

	class R7LogCapture : public ILogHandler
	{
	public:
		void handleLogAppend(WTSLogLevel, const char* message) override
		{
			if (message != nullptr)
				messages.emplace_back(message);
		}

		std::vector<std::string> messages;
	};

	std::string r7_json_quote(const std::string& value)
	{
		std::string quoted = "\"";
		for (char ch : value)
		{
			if (ch == '\\' || ch == '"')
				quoted += '\\';
			quoted += ch;
		}
		quoted += '"';
		return quoted;
	}

} // namespace

//////////////////////////////////////////////////////////////////////////
// 构造 + 基础数据未加载时的查询接口（L36-38、L434-448、L371-382）
//////////////////////////////////////////////////////////////////////////
TEST(r7_execrunner, queries_return_null_before_base_data_loaded)
{
	WtExecRunner runner;
	// 没有 loadSessions/loadCommodities，按 exchg+product 查品种必然落空
	EXPECT_EQ(runner.get_comm_info("SHFE.rb2401"), nullptr);          // L437 -> NULL
	EXPECT_EQ(runner.get_sess_info("SHFE.rb2401"), nullptr);          // L443-445
	// isCode=true 走同一条抽取+查品种路径
	EXPECT_EQ(runner.get_session_info("SHFE.rb2401", true), nullptr); // L376-379
	// 格式有效但基础数据中没有该品种时返回 NULL
	EXPECT_EQ(runner.get_sess_info("SHFE.unknown.2401"), nullptr);
}

TEST(r7_execrunner, unconfigured_runner_destroys_safely)
{
	{
		WtExecRunner runner;
	}
	SUCCEED();
}

//////////////////////////////////////////////////////////////////////////
// init：日志配置文件不存在（isFile=true 分支，L46-50 + L57-58）
// 实测同型的 r6 用例（m_WtRunner/r6_wtrunner_main_test.cpp:109）证明这条路径不抛异常
//////////////////////////////////////////////////////////////////////////
TEST(r7_execrunner, init_with_missing_logcfg_file_returns_true)
{
	WtExecRunner runner;
	EXPECT_TRUE(runner.init("./r7_missing_logcfgexec.json", true));
	// init 只做日志与 setInstDir，不应建立任何适配器
	EXPECT_EQ(runner.get_comm_info("SHFE.rb2401"), nullptr);
}

//////////////////////////////////////////////////////////////////////////
// init：把内容当日志配置（isFile=false 分支，L51-54），非法内容 -> 不初始化成功
//////////////////////////////////////////////////////////////////////////
TEST(r7_execrunner, init_with_invalid_inline_logcfg_returns_true)
{
	WtExecRunner runner;
	// 不是合法 JSON，WTSLogger::init 内部告警后返回；init 本身恒 return true（L58）
	EXPECT_TRUE(runner.init("{not a valid logcfg", false));
}

//////////////////////////////////////////////////////////////////////////
// config：文件不存在 -> L64-68 拒绝分支
//////////////////////////////////////////////////////////////////////////
TEST(r7_execrunner, config_missing_file_returns_false)
{
	WtExecRunner runner;
	EXPECT_FALSE(runner.config("./r7_missing_execcfg.json", true));
	// 拒绝后不应残留任何配置对象，二次调用仍然拒绝
	EXPECT_FALSE(runner.config("./r7_missing_execcfg.json", true));
	EXPECT_NO_THROW(runner.run());
}

//////////////////////////////////////////////////////////////////////////
// config：内容为空串 -> load_from_content 返回 NULL -> 拒绝分支
//////////////////////////////////////////////////////////////////////////
TEST(r7_execrunner, config_empty_content_returns_false)
{
	WtExecRunner runner;
	EXPECT_FALSE(runner.config("", false));
	EXPECT_FALSE(runner.config("{invalid json]", false));
}

TEST(r7_execrunner, trader_init_failure_fails_config)
{
	const std::string dir = r7_temp_dir();
	r7_write_text(dir + "/actions.json", "{\"default\":{}}");
	r7_write_text(dir + "/traders.json",
		"{\"traders\":[{\"active\":true,\"id\":\"r7_missing_trader\","
		"\"module\":\"r7_module_that_does_not_exist\"}]}");

	WtExecRunner runner;
	ASSERT_TRUE(runner.init((dir + "/r7_missing_logcfg.json").c_str(), true));
	const std::string cfg = "{\"basefiles\":{},\"bspolicy\":\"" + dir
		+ "/actions.json\",\"traders\":\"" + dir + "/traders.json\"}";
	EXPECT_FALSE(runner.config(cfg.c_str(), false));
	EXPECT_NO_THROW(runner.run());
	r7_remove_dir(dir);
}

TEST(r7_execrunner, trader_config_file_missing_or_invalid_fails_config)
{
	const std::string dir = r7_temp_dir();
	r7_write_text(dir + "/actions.json", "{\"default\":{}}");
	r7_write_text(dir + "/bad_traders.json", "{invalid json]");

	const std::string prefix = "{\"basefiles\":{},\"bspolicy\":\"" + dir
		+ "/actions.json\",\"traders\":\"";
	{
		WtExecRunner runner;
		ASSERT_TRUE(runner.init((dir + "/r7_missing_logcfg.json").c_str(), true));
		const std::string cfg = prefix + dir + "/missing_traders.json\"}";
		EXPECT_FALSE(runner.config(cfg.c_str(), false));
		EXPECT_NO_THROW(runner.run());
	}
	{
		WtExecRunner runner;
		ASSERT_TRUE(runner.init((dir + "/r7_missing_logcfg.json").c_str(), true));
		const std::string cfg = prefix + dir + "/bad_traders.json\"}";
		EXPECT_FALSE(runner.config(cfg.c_str(), false));
	}
	r7_remove_dir(dir);
}

TEST(r7_execrunner, trader_mocker_initializes_and_registers_as_control)
{
	const std::string dir = r7_temp_dir();
	r7_write_text(dir + "/actions.json", "{\"default\":{}}");
	r7_write_text(dir + "/traders.json",
		"{\"traders\":[{\"active\":true,\"id\":\"r7_mocker_trader\","
		"\"module\":\"TraderMocker\",\"mockerid\":70001,\"udp_port\":0}]}");

	{
		wtp_test::TraderMockerFixture trader_mocker("r7");
		WtExecRunner runner;
		ASSERT_TRUE(runner.init((dir + "/r7_missing_logcfg.json").c_str(), true));
		if (!trader_mocker.install())
		{
			r7_remove_dir(dir);
			FAIL() << "Could not prepare TraderMocker from product output at "
				<< trader_mocker.product_plugin() << ": " << trader_mocker.error();
			return;
		}
		const std::string cfg = "{\"basefiles\":{},\"bspolicy\":\"" + dir
			+ "/actions.json\",\"traders\":\"" + dir + "/traders.json\"}";
		ASSERT_TRUE(runner.config(cfg.c_str(), false));
	}

	r7_remove_dir(dir);
	std::error_code ec;
	std::filesystem::remove_all("mocker_70001", ec);
}

//////////////////////////////////////////////////////////////////////////
// BUG-R7-011：缺少必填 basefiles 应安全失败并输出字段诊断。
// 用 threadsafe death-test 子进程初始化同步 logger 并捕获真实 ILogHandler 回调；
// 显式 stop 以排空 flush worker，避免污染本测试进程。
//////////////////////////////////////////////////////////////////////////
TEST(r7_execrunner, config_without_basefiles_returns_false_with_diagnostic)
{
	const std::string dir = r7_temp_dir();
	const std::string logger_cfg = "{\"root\":{\"level\":\"error\",\"async\":false,\"sinks\":[]}}";
	const std::string previousDeathTestStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT({
		bool init_ok = false;
		bool config_ok = true;
		bool control_init_ok = false;
		bool valid_config_ok = false;
		R7LogCapture capture;
		try
		{
			WTSLogger::init(logger_cfg.c_str(), false, &capture);
			{
				WtExecRunner runner;
				init_ok = runner.init(logger_cfg.c_str(), false);
				if (init_ok)
				{
					WTSLogger::error("R7 logger capture sentinel");
					config_ok = runner.config("{}", false);
				}
			}

			const std::string actions_path = dir + "/actions.json";
			r7_write_text(actions_path, "{\"default\":{}}");
			const std::string valid_cfg = "{\"basefiles\":{},\"bspolicy\":"
				+ r7_json_quote(actions_path) + "}";
			{
				WtExecRunner valid_runner;
				control_init_ok = valid_runner.init(logger_cfg.c_str(), false);
				if (control_init_ok)
					valid_config_ok = valid_runner.config(valid_cfg.c_str(), false);
			}
		}
		catch (...)
		{
			WTSLogger::stop();
			std::_Exit(91);
		}

		// Keep capture alive through stop(), which joins spdlog's periodic worker.
		WTSLogger::stop();
		std::string actual;
		for (const auto& message : capture.messages)
		{
			if (!actual.empty())
				actual += '\n';
			actual += message;
		}
		const bool has_logger_sentinel = actual.find("R7 logger capture sentinel") != std::string::npos;
		const bool has_diagnostic = actual.find("Required configuration field 'basefiles' is missing") != std::string::npos;
		if (!init_ok || config_ok || !control_init_ok || !valid_config_ok ||
			!has_logger_sentinel || !has_diagnostic)
		{
			std::fprintf(stderr, "init_ok=%d config_ok=%d control_init_ok=%d valid_config_ok=%d "
				"logger_cfg=%s log=%s\n", init_ok ? 1 : 0,
				config_ok ? 1 : 0, control_init_ok ? 1 : 0, valid_config_ok ? 1 : 0,
				logger_cfg.c_str(), actual.c_str());
			r7_remove_dir(dir);
			std::fflush(stderr);
			std::_Exit(92);
		}
		std::fprintf(stderr, "%s\n", actual.c_str());
		r7_remove_dir(dir);
		std::fflush(stderr);
		std::_Exit(0);
	}, ::testing::ExitedWithCode(0), "Required configuration field 'basefiles' is missing");
	GTEST_FLAG_SET(death_test_style, previousDeathTestStyle);
	r7_remove_dir(dir);
}

//////////////////////////////////////////////////////////////////////////
// handle_push_quote：空指针直接返回（L386-387）
//////////////////////////////////////////////////////////////////////////
TEST(r7_execrunner, handle_push_quote_ignores_null)
{
	WtExecRunner runner;
	runner.handle_push_quote(NULL);                 // L386-387，不落任何路由
	EXPECT_EQ(runner.get_sess_info("SHFE.rb2401"), nullptr);
}

//////////////////////////////////////////////////////////////////////////
// setPosition / commitPositions / run：空适配器集合下的提交与启动（L407-416、L181-194）
//////////////////////////////////////////////////////////////////////////
TEST(r7_execrunner, position_cache_and_empty_run)
{
	WtExecRunner runner;
	runner.setPosition("SHFE.rb2401", 10);
	runner.setPosition("SHSE.600000", -3);
	EXPECT_NO_THROW(runner.commitPositions());      // _exe_mgr 里没有执行单元 -> 空转
	// 再次提交（缓存已清空）也不应有任何副作用
	EXPECT_NO_THROW(runner.commitPositions());
	EXPECT_NO_THROW(runner.run());                  // parsers/traders 两个 mgr 都是空的
	// run() 之后仍然没有品种/会话被注册
	EXPECT_EQ(runner.get_comm_info("SHFE.rb2401"), nullptr);
}

//////////////////////////////////////////////////////////////////////////
// 缺陷复现（关停未初始化日志器，行为不确定，不能启用）：release -> WTSLogger::stop()
//////////////////////////////////////////////////////////////////////////
TEST(r7_execrunner, DISABLED_release_stops_uninited_logger)
{
	WtExecRunner runner;
	runner.release();                               // L401-404：spdlog 关停/flush
	SUCCEED();
}

//////////////////////////////////////////////////////////////////////////
// 缺陷复现（会 dlopen 插件目录，不能启用）：addExeFactories -> loadFactories
// 与 WtCtaStraFact/WtHelper 等同名符号存在 duplicate symbol 风险（BUG-L00-003）
//////////////////////////////////////////////////////////////////////////
TEST(r7_execrunner, DISABLED_add_exe_factories_dlopens_plugins)
{
	WtExecRunner runner;
	runner.addExeFactories("./executers/");          // L366-369
	SUCCEED();
}
