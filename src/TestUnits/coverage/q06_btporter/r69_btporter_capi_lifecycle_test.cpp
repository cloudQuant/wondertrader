/*
 * R69 - WtBtPorter 生命周期导出：init_backtest / config_backtest / stop_backtest
 *
 * 目标：src/WtBtPorter/WtBtPorter.cpp
 *   - init_backtest() L155-165：首调把 logProfile/outDir 交给 WtBtRunner::init，
 *     二次调用被函数内 static inited 吃掉（用 setOutputDir 哨兵证伪）
 *   - config_backtest() L174-185：cfgfile 为空串时回落到 "configbt.json"（本进程
 *     CWD 下不存在 -> WtBtRunner::config 在 _cfg==NULL 处打日志返回，不建 mocker）
 *   - stop_backtest() L223-226：未 run 时转 WtBtRunner::stop 的早退
 *
 * 与既有 lane 的分工：R46 从 WtBtRunner::init/stop 方法层取过同一路径；R9 用
 * 非空路径取过 config_backtest 的「文件不存在」早退，但空串回落支（L181-182）
 * 与 init_backtest 的一次性门（L157-164）都没有用例。q06 明确跳过这三个入口。
 *
 * 只动进程单例 getRunner()，不建 mocker/不跑回测/不起线程；run_backtest 与
 * release_backtest 见文末 DISABLED（前者需要已配置回放器，后者会 WTSLogger::stop
 * 把本进程后续日志永久静音，同 BUG-R6-006 的裁定）。
 * 临时目录只在系统 tmp 下创建并清理。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include <unistd.h>

#include "WtBtCore/WtHelper.h"
#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtPorter.h"
#include "WtBtPorter/WtBtRunner.h"

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace {

	std::string prepare_dir(const std::string& leaf)
	{
		std::filesystem::path p = std::filesystem::temp_directory_path()
			/ (leaf + "_" + std::to_string(::getpid()));
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

} // namespace

/* WtBtPorter.cpp:155-165 + WtBtRunner.cpp:437-447：首次 init_backtest 把
 * WtHelper 的 inst/output 目录写到临时目录；随后用哨兵覆盖 output 目录，
 * 再调一次 init_backtest（换一个 outDir），哨兵必须原样保留，证明 static inited
 * 门生效。logProfile 用解析不了的字符串，WTSLogger::init 在 cfg==NULL 处返回。 */
TEST(R69BtPorterLifecycle, InitBacktestStampsDirsOnlyOnce)
{
	const std::string dir1 = prepare_dir("r69_bt_life_a");
	const std::string dir2 = prepare_dir("r69_bt_life_b");
	const std::string sentinel = prepare_dir("r69_bt_life_sentinel");
	ASSERT_FALSE(dir1.empty());
	ASSERT_FALSE(dir2.empty());
	ASSERT_FALSE(sentinel.empty());

	init_backtest("{ r69 not a valid log config", false, dir1.c_str());
	EXPECT_FALSE(WtHelper::getInstDir().empty());
	EXPECT_EQ(std::string(WtHelper::getOutputDir()), dir1 + "/");

	WtHelper::setOutputDir(sentinel.c_str());
	init_backtest("{ another invalid log config", false, dir2.c_str());
	EXPECT_EQ(std::string(WtHelper::getOutputDir()), sentinel + "/")
		<< "init_backtest 的 static inited 门失效";

	/* 还原为临时目录，避免把后续用例的输出指到哨兵目录 */
	WtHelper::setOutputDir(dir1.c_str());
	clean_dir(dir1);
	clean_dir(dir2);
	clean_dir(sentinel);
}

/* WtBtPorter.cpp:174-185 的空串回落支：cfgfile=="" 时走 config("configbt.json", true)。
 * 本进程工作目录下没有该文件，WtBtRunner::config 在 load 失败后打日志返回，
 * 单例的三个 mocker 槽位保持为空（与 R9 的非空路径早退互为对照）。 */
TEST(R69BtPorterLifecycle, ConfigBacktestEmptyPathFallsBackToMissingConfigbt)
{
	WtBtRunner& r = getRunner();
	ASSERT_EQ(r.cta_mocker(), nullptr);
	ASSERT_EQ(r.sel_mocker(), nullptr);
	ASSERT_EQ(r.hft_mocker(), nullptr);

	config_backtest("", false);

	EXPECT_EQ(r.cta_mocker(), nullptr);
	EXPECT_EQ(r.sel_mocker(), nullptr);
	EXPECT_EQ(r.hft_mocker(), nullptr);
}

/* WtBtPorter.cpp:223-226 + WtBtRunner.cpp:550-559：未 run（_running=false、
 * _worker 为空）时 stop 只走早退，可重复调用。 */
TEST(R69BtPorterLifecycle, StopBacktestBeforeRunIsRepeatableNoOp)
{
	EXPECT_NO_THROW(stop_backtest());
	EXPECT_NO_THROW(stop_backtest());

	WtBtRunner& r = getRunner();
	EXPECT_EQ(r.cta_mocker(), nullptr);
	EXPECT_EQ(r.sel_mocker(), nullptr);
	EXPECT_EQ(r.hft_mocker(), nullptr);
}

/* WtBtPorter.cpp:213-217：run_backtest 会进 WtBtRunner::run -> _replayer.prepare()，
 * 要求已 config 过回放器与存储；单例从未 config，直接调会解引用未初始化的
 * HisDataReplayer 成员。保留为缺陷/进程级取样位，不在本文件启用。 */
TEST(R69BtPorterLifecycle, DISABLED_RunBacktestNeedsConfiguredReplayer)
{
	run_backtest(false, false);
}

/* WtBtPorter.cpp:228-235：release_backtest -> WtBtRunner::release ->
 * WTSLogger::stop()，会把静态 m_bStopped 置位并 spdlog::shutdown()，本进程后续
 * 用例的日志通道被永久关闭（BUG-R6-006 已登记同一副作用）。留给进程级取样。 */
TEST(R69BtPorterLifecycle, DISABLED_ReleaseBacktestStopsGlobalLogger)
{
	release_backtest();
}
