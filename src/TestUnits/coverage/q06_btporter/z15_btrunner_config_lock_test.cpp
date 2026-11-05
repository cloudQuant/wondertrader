/*
 * BUG-R9-001（卡15最终复查）：WtBtRunner::config_v2 的一次性配置状态机。
 *
 * r205 已覆盖“成功后重复配置被拒绝”和“preflight 失败可重试”。本文件补
 * replayer 已初始化后模式工厂失败的分支：按卡9c=A（重复/再次初始化拒绝并保留原实例）
 * 与卡9（无法证明可安全回收的状态保留、fail-closed），该次失败后再提交任何配置
 * 都必须以明确诊断拒绝，而不是重建 replayer。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <system_error>

#include <unistd.h>

#include "WtBtPorter/WtBtRunner.h"

namespace
{
	namespace z15fs = std::filesystem;

	std::string z15_prepare_dir(const char* tag)
	{
		z15fs::path p = z15fs::temp_directory_path()
			/ (std::string("z15_btrunner_") + tag + "_" + std::to_string((long)::getpid()));
		std::error_code ec;
		z15fs::remove_all(p, ec);
		z15fs::create_directories(p, ec);
		return p.string();
	}

	std::string z15_replayer(const std::string& dir)
	{
		return std::string("\"replayer\":{\"mode\":\"none\",\"path\":\"") + dir +
			"\",\"stime\":20240102000000,\"etime\":20240103000000,\"tick\":false,\"basefiles\":{}}";
	}
}

TEST(Z15BtRunnerConfigLock, FactoryFailureAfterReplayerInitRejectsLaterConfigExplicitly)
{
	const std::string dir = z15_prepare_dir("factory_lock");
	ASSERT_FALSE(dir.empty());

	// preflight 全部通过（cta 段与 name 齐全），replayer 以 none 模式初始化成功，
	// 随后 CTA 策略工厂模块不存在 -> 模式初始化失败。
	const std::string failing = "{" + z15_replayer(dir) + ","
		"\"env\":{\"mocker\":\"cta\"},"
		"\"cta\":{\"name\":\"z15_cta\",\"module\":\"" + dir + "/Z15NoSuchStrategyModule.dylib\"}}";
	// 与 r205 成功用例相同形态、无需插件的有效配置；若无锁定会成功。
	const std::string valid = "{" + z15_replayer(dir) + ",\"env\":{\"mocker\":\"none\"}}";

	WtBtRunner runner;
	std::string error;
	EXPECT_FALSE(runner.config_v2(failing.c_str(), false, &error));
	EXPECT_NE(std::string::npos, error.find("initializing CTA strategy factory failed")) << error;

	EXPECT_FALSE(runner.config_v2(valid.c_str(), false, &error));
	EXPECT_NE(std::string::npos, error.find("retry is unsupported")) << error;

	// 正控：同一有效配置在全新 runner 上成功，证明上面的拒绝来自锁定而非配置本身。
	WtBtRunner fresh;
	std::string freshError;
	EXPECT_TRUE(fresh.config_v2(valid.c_str(), false, &freshError)) << freshError;

	std::error_code ec;
	z15fs::remove_all(dir, ec);
}
