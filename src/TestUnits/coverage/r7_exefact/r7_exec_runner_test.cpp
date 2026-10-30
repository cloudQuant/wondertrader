// R7 lane：WtExecRunner（执行监控运行器，报表里该文件 0.00%）
// 目标文件: src/WtExecMon/WtExecRunner.cpp / .h
// 覆盖面: 构造、init 的两条日志配置分支、config 的三条拒绝分支（文件缺失/内容非法/空内容）、
//         handle_push_quote 的空指针防御、仓位缓存提交、run 的空适配器路径、
//         get_comm_info / get_sess_info / get_session_info 在基础数据未加载时的 NULL 返回。
//
// 刻意不碰的面（详见 bugs/R7_exefact.md）：
//  * WTSLogger 真正初始化成功后的 sink/async_logger 会创建线程（WTSLogger.cpp:193），
//    本轮禁止起线程 -> 只用「配置缺失/非法」这条不会初始化成功的路径；
//  * release() -> WTSLogger::stop() 会做 spdlog 关停/flush，未初始化状态下行为不确定 -> DISABLED；
//  * initParsers/initExecuters 会 new ParserAdapter 或 dlopen 执行器工厂，initDataMgr 会按
//    配置打开行情存储（可能涉及共享内存）-> 全部留给主循环评审；交易通道仅使用本地
//    TraderMocker 插件做 config/init 正控，不调用 run/connect；
//  * addExeFactories 会 dlopen executer 目录下的插件，与 WtCtaStraFact 等同名符号冲突
//    （BUG-L00-003 实测 34 个 duplicate symbol）-> 不调用。
#include <gtest/gtest.h>

#include <cstdint>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "WtExecMon/WtExecRunner.h"
#include "../trader_mocker_fixture.h"

using namespace wtp;

namespace {

	std::string r7_temp_dir()
	{
		const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
		std::filesystem::path dir = std::filesystem::temp_directory_path()
			/ ("r7_trader_init_" + std::to_string(nonce));
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
	r7_write_text(dir + "/actions.json", "{}");
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
	r7_write_text(dir + "/actions.json", "{}");
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
	r7_write_text(dir + "/actions.json", "{}");
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
// 缺陷复现（空指针解引用，不能启用）：config 用了合法 JSON 但缺 basefiles 键
// L71 `WTSVariant* cfgBF = _config->get("basefiles");` 返回 NULL，L72 立刻 cfgBF->get("session")
// 见 BUG-R7-011
//////////////////////////////////////////////////////////////////////////
TEST(r7_execrunner, DISABLED_config_without_basefiles_section_derefs_null)
{
	WtExecRunner runner;
	EXPECT_FALSE(runner.config("{}", false));         // 期望：L72 崩
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
