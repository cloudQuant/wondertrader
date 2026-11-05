/*!
 * 迭代03 第五会话主线程：BUG-L03-009（卡2，2026-11-03 已确认）——会话配置 offset 限定 ±1440，
 * 超域 offset 让整份会话文件被拒绝（候选整批校验后发布，非法配置不留部分注册）；运行时模一天归一化
 * 由 l03_session_info_test 覆盖。
 */
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "Includes/WTSSessionInfo.hpp"
#include "WTSTools/WTSBaseDataMgr.h"

using namespace wtp;

namespace
{
std::filesystem::path l03009File(const std::string& name, const std::string& body)
{
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path dir = std::filesystem::temp_directory_path() /
		("s5_l03009_" + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(stamp));
	std::filesystem::create_directories(dir);
	const std::filesystem::path file = dir / name;
	std::ofstream out(file);
	out << body;
	return file;
}

std::string l03009Sessions(int offset)
{
	return "{\"GOOD\":{\"name\":\"good\",\"offset\":0,\"sections\":[{\"from\":900,\"to\":1500}]},"
		"\"EDGE\":{\"name\":\"edge\",\"offset\":" + std::to_string(offset) + ",\"sections\":[{\"from\":900,\"to\":1500}]}}";
}
}

TEST(S5MainL03009OffsetConfig, OffsetsBeyondOneDayRejectTheWholeFile)
{
	for (int offset : { 1441, -1441, 2880, -2880, 2147483647 })
	{
		SCOPED_TRACE(offset);
		WTSBaseDataMgr mgr;
		EXPECT_FALSE(mgr.loadSessions(l03009File("bad.json", l03009Sessions(offset)).string().c_str()));
		EXPECT_EQ(nullptr, mgr.getSession("GOOD")) << "no partial registration";
		EXPECT_EQ(nullptr, mgr.getSession("EDGE"));
	}
}

TEST(S5MainL03009OffsetConfig, OffsetsUpToOneDayAreAccepted)
{
	for (int offset : { 1440, -1440, 0, 300, -420 })
	{
		SCOPED_TRACE(offset);
		WTSBaseDataMgr mgr;
		ASSERT_TRUE(mgr.loadSessions(l03009File("ok.json", l03009Sessions(offset)).string().c_str()));
		ASSERT_NE(nullptr, mgr.getSession("EDGE"));
		EXPECT_EQ(offset, mgr.getSession("EDGE")->getOffsetMins());
		EXPECT_NE(nullptr, mgr.getSession("GOOD"));
	}
}
