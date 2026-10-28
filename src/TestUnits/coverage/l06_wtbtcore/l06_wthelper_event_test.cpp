/**
 * l06_wthelper_event_test.cpp
 * 覆盖 src/WtBtCore/WtHelper.{h,cpp} 全部静态工具:
 * getCWD / setOutputDir+getOutputDir(含目录自动创建) / setInstDir+getInstDir,
 * 以及 src/WtBtCore/EventNotifier 的安全路径: 构造 / 析构 / init(active=false)。
 * 注意: EventNotifier::init 对 NULL cfg 无判空(已记录待审核 bug), 故此处只传有效 cfg。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>

#include <WtBtCore/WtHelper.h>
#include <WtBtCore/EventNotifier.h>
#include <Includes/WTSVariant.hpp>

USING_NS_WTP;

namespace
{
std::filesystem::path lane_temp(const char* tag)
{
	return std::filesystem::temp_directory_path() /
		(std::string("l06_wthelper_") + tag + "_" + std::to_string(::getpid()));
}
}

class WtHelperTest : public ::testing::Test
{
protected:
	void TearDown() override
	{
		// 恢复默认输出目录, 避免影响同二进制内其他用例
		WtHelper::setOutputDir("./outputs_bt/");
		WtHelper::setInstDir("");
		for (const auto& p : _cleanup)
		{
			std::error_code ec;
			std::filesystem::remove_all(p, ec);
		}
		_cleanup.clear();
	}

	std::vector<std::filesystem::path> _cleanup;
};

TEST_F(WtHelperTest, instDirRoundTrip)
{
	WtHelper::setInstDir("/tmp/l06_inst_dir_marker/");
	EXPECT_EQ(std::string("/tmp/l06_inst_dir_marker/"), WtHelper::getInstDir());
	WtHelper::setInstDir("relative/path");
	EXPECT_EQ(std::string("relative/path"), WtHelper::getInstDir());
}

TEST_F(WtHelperTest, cwdIsExistingDirectory)
{
	std::string cwd = WtHelper::getCWD();
	EXPECT_FALSE(cwd.empty());
	std::error_code ec;
	EXPECT_TRUE(std::filesystem::is_directory(cwd, ec)) << cwd;
	// 二次调用走静态缓存分支
	EXPECT_EQ(cwd, WtHelper::getCWD());
}

TEST_F(WtHelperTest, outputDirIsCreatedOnDemand)
{
	auto dir = lane_temp("outdir");
	_cleanup.push_back(dir);
	// setOutputDir 本身不落盘
	WtHelper::setOutputDir(dir.string().c_str());
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	ASSERT_FALSE(std::filesystem::exists(dir));
	// getOutputDir 在目录缺失时自动创建
	const char* got = WtHelper::getOutputDir();
	ASSERT_NE(nullptr, got);
	EXPECT_TRUE(std::filesystem::exists(dir)) << got;
	// standardisePath 会补尾部路径分隔符
	std::string gotStr(got);
	EXPECT_EQ(0u, gotStr.find(dir.string()));
}

TEST_F(WtHelperTest, outputDirKeepsCustomValue)
{
	auto dir = lane_temp("outdir2");
	_cleanup.push_back(dir);
	WtHelper::setOutputDir(dir.string().c_str());
	std::string first = WtHelper::getOutputDir();
	std::string second = WtHelper::getOutputDir();
	// 已存在时不再重复创建, 返回值稳定
	EXPECT_EQ(first, second);
}

TEST(EventNotifierSafePath, inactiveConfigSkipsModuleLoading)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("active", false);
	cfg->append("url", "tcp://127.0.0.1:5555");

	EventNotifier notifier;
	EXPECT_FALSE(notifier.init(cfg));
	cfg->release();
	// 未激活时析构不应尝试销毁服务器 (_remover 为空)
}

TEST(EventNotifierSafePath, missingModuleYieldsFailureOrSkip)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("active", true);
	cfg->append("url", "tcp://127.0.0.1:5556");
	// 指向不存在的安装目录, 让 wrap_module 查找失败, 走 "MQ module loading failed" 分支
	std::string fakeInst = (std::filesystem::temp_directory_path() / "l06_no_such_dir").string() + "/";
	WtHelper::setInstDir(fakeInst.c_str());

	EventNotifier notifier;
	// 本机若恰好存在 WtMsgQue 模块则可能成功, 这里只要求不崩溃且布尔返回
	bool inited = notifier.init(cfg);
	(void)inited;
	cfg->release();
}
