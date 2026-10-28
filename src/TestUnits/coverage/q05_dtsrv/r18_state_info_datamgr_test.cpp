/*!
 * \file r18_state_info_datamgr_test.cpp
 * \brief WtDtCore/StateMonitor.h 头文件内联面 + WtDtCore/DataManager.cpp 默认模块路径
 *        （迭代01 R18 lane）
 *
 * 与既有 q05_state_monitor_test.cpp / r5_state_monitor_bdt_test.cpp /
 * q05_dtcore_datamgr_test.cpp 不重叠：那几份都在驱动 StateMonitor::initialize
 * （.cpp 侧），因此 StateMonitor.h 里
 *  - _StateInfo 默认构造函数（StateMonitor.h:87-95）
 *  - StateInfo::isInSections（StateMonitor.h:70-81，产品侧只有 run() 线程里调用它）
 *  - 空状态表上的 isAnyInState / isAllInState / isInState 的 0 次循环与未命中分支
 *  （StateMonitor.h:169-222）从未被独立喂过输入。本文件用纯内存的 StateInfo /
 *  StateMonitor 直接覆盖，不读挂钟、不起线程、不 sleep。
 *
 * DataManager 侧补的是 init 的两条命名分支：
 *  - 配置里没有 module -> DataManager.cpp:73-74 用 WtHelper::get_module_dir() 拼
 *    DLLHelper::wrap_module("WtDataStorage") 的默认写手名
 *  - 配置写了 module   -> :75-76 用给定名字拼路径
 * 两条都在 load_library 失败处收口（:100-104），不加载任何插件。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/StateMonitor.h>
#include <WtDtCore/WtHelper.h>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>

namespace fs = std::filesystem;

namespace {

fs::path make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

void write_file(const fs::path& p, const std::string& content)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

// 本机一定存在、且肯定没有 createWriter/deleteWriter 符号的第三方 dylib
fs::path find_foreign_dylib()
{
	static const char* candidates[] = {
		"/opt/homebrew/lib/libFLAC++.dylib",
		"/opt/homebrew/lib/libzstd.1.dylib",
		"/opt/homebrew/lib/liblz4.1.dylib",
		"/usr/local/lib/libFLAC++.dylib",
	};
	for (const char* c : candidates)
	{
		std::error_code ec;
		if (fs::is_regular_file(fs::path(c), ec))
			return fs::path(c);
	}
	return fs::path();
}

class R18StateInfoTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = make_tmp_dir("r18_state");
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	fs::path _dir;
};

TEST_F(R18StateInfoTest, DefaultStateInfoFieldsAreZeroedAndOriginal)
{
	StateInfo si;
	// StateMonitor.h:89 只保证 _session[0] 为 '\0'
	EXPECT_STREQ(si._session, "");
	EXPECT_EQ(si._init_time, 0u);
	EXPECT_EQ(si._close_time, 0u);
	EXPECT_EQ(si._proc_time, 0u);
	// 枚举首位 SS_ORIGINAL（StateMonitor.h:31）
	EXPECT_EQ(si._state, SS_ORIGINAL);
	EXPECT_EQ(si._sInfo, nullptr);
	EXPECT_TRUE(si._sections.empty());

	// _sections 由本用例自己塞入，供下一条用例使用
	si._sections.push_back({ 930, 1130 });
	si._sections.push_back({ 1300, 1500 });
	EXPECT_EQ(si._sections.size(), 2u);
	EXPECT_EQ(si._sections[0]._from, 930u);
	EXPECT_EQ(si._sections[1]._end, 1500u);
}

TEST_F(R18StateInfoTest, IsInSectionsUsesClosedOpenInterval)
{
	StateInfo si;
	// 期望值由本用例自己喂的两个区间算出：[930,1130) 与 [1300,1500)
	si._sections.push_back({ 930, 1130 });
	si._sections.push_back({ 1300, 1500 });

	EXPECT_FALSE(si.isInSections(929));	// 第一个区间之前
	EXPECT_TRUE(si.isInSections(930));	// sec._from <= curTime 命中下界
	EXPECT_TRUE(si.isInSections(1129));	// 区间内
	EXPECT_FALSE(si.isInSections(1130));	// curTime < sec._end 不成立（右开）
	EXPECT_FALSE(si.isInSections(1200));	// 午休：两轮循环都不命中 -> L80 return false
	EXPECT_TRUE(si.isInSections(1300));	// 第二个区间的下界
	EXPECT_TRUE(si.isInSections(1499));
	EXPECT_FALSE(si.isInSections(1500));	// 第二个区间的上界
	EXPECT_FALSE(si.isInSections(2000));	// 全部区间之后
}

TEST_F(R18StateInfoTest, IsInSectionsOnEmptyAndDegenerateSections)
{
	StateInfo emptySec;
	// 空区间集合：L73 循环 0 次直接 false（StateMonitor.h:73/80）
	EXPECT_FALSE(emptySec.isInSections(930));
	EXPECT_FALSE(emptySec.isInSections(0));

	StateInfo degenerate;
	degenerate._sections.push_back({ 900, 900 });
	// from <= 900 成立、900 < end 不成立 -> 零宽区间永不命中
	EXPECT_FALSE(degenerate.isInSections(900));
	EXPECT_FALSE(degenerate.isInSections(899));

	StateInfo reversed;
	reversed._sections.push_back({ 1500, 930 });
	// 倒挂区间（from > end）：L77 两个子条件不可能同时成立 -> 恒 false
	EXPECT_FALSE(reversed.isInSections(1000));
	EXPECT_FALSE(reversed.isInSections(1500));
}

TEST_F(R18StateInfoTest, FreshMonitorQueriesOnEmptyStateTable)
{
	StateMonitor mon;
	// _map 为空：isAnyInState 0 次循环 -> L181 false（StateMonitor.h:172-181）
	EXPECT_FALSE(mon.isAnyInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isAnyInState(SS_CLOSED));
	// isAllInState 空表恒 true（没有「非节假日且 != ss」的元素）
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
	EXPECT_TRUE(mon.isAllInState(SS_CLOSED));
	// isInState 未命中 -> L216-217 false
	EXPECT_FALSE(mon.isInState("SSE", SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("", SS_ORIGINAL));
}

TEST_F(R18StateInfoTest, DataManagerInitFailsOnMissingModuleFileBothWays)
{
	// 把模块目录指向空目录：两条命名分支都必然找不到 .dylib
	const fs::path binDir = _dir / "bin";
	fs::create_directories(binDir);
	WtHelper::set_module_dir((binDir.string() + "/").c_str());

	// DLLHelper::wrap_module("WtDataStorage") -> "libWtDataStorage.dylib"
	// DLLHelper::wrap_module("r18_custom_store") -> "libr18_custom_store.dylib"
	const fs::path defaultName = binDir / "libWtDataStorage.dylib";
	const fs::path customName = binDir / "libr18_custom_store.dylib";
	ASSERT_FALSE(fs::exists(defaultName));
	ASSERT_FALSE(fs::exists(customName));

	// 1) config 里不写 module -> DataManager.cpp:73-74 走默认 writer 名
	write_file(_dir / "a.json", R"JSON({"filepath": "r18_state_default"})JSON");
	WTSVariant* paramsA = WTSCfgLoader::load_from_file((_dir / "a.json").string().c_str());
	ASSERT_NE(paramsA, nullptr);
	DataManager dmA;
	EXPECT_FALSE(dmA.init(paramsA, nullptr, nullptr));
	// 状态机为 NULL -> 全天候模式（:241-242）
	EXPECT_TRUE(dmA.canSessionReceive("SSE"));
	EXPECT_EQ(dmA.getBDMgr(), nullptr);
	paramsA->release();

	// 2) config 写了 module -> :75-76 用给定名字拼路径
	write_file(_dir / "b.json", R"JSON({"module": "r18_custom_store"})JSON");
	WTSVariant* paramsB = WTSCfgLoader::load_from_file((_dir / "b.json").string().c_str());
	ASSERT_NE(paramsB, nullptr);
	DataManager dmB;
	EXPECT_FALSE(dmB.init(paramsB, nullptr, nullptr));
	paramsB->release();

	// 两次失败都不该往模块目录落任何文件
	std::size_t entries = 0;
	for (fs::directory_iterator it(binDir), end; it != end; ++it)
		entries++;
	EXPECT_EQ(entries, 0u);

	// 未 init 成功的实例，全部 writer 守卫仍然生效
	EXPECT_FALSE(dmA.writeTick(nullptr, 0));
	EXPECT_EQ(dmA.getCurTick("600000", "SSE"), nullptr);
	dmA.release();
	dmB.release();
}

/*
 * 触发条件（禁用原因）：DataManager::init（DataManager.cpp:67-72）对 params 没有判空，
 * params==NULL 时 `params->getCString("module")` 直接解引用空指针 -> SIGSEGV。
 * 缺陷登记：docs/迭代计划/迭代01/bugs/R18_wtdtcore.md BUG-R18-006。
 */
TEST_F(R18StateInfoTest, DISABLED_InitCrashesWhenParamsIsNull)
{
	const fs::path binDir = _dir / "bin2";
	fs::create_directories(binDir);
	WtHelper::set_module_dir((binDir.string() + "/").c_str());

	DataManager dm;
	dm.init(nullptr, nullptr, nullptr);	// 崩在 :72
}

/*
 * 触发条件（禁用原因）：模块加载成功但找不到 createWriter / deleteWriter 时，
 * :84 与 :90 只打日志、:93 的 && 不成立 -> _writer 保持 NULL，
 * 而 :106 无条件执行 `_writer->init(params, this)` -> 空指针解引用 SIGSEGV。
 * 复现材料：把任意一个真实 Mach-O dylib 复制成模块目录里的 libWtDataStorage.dylib
 * （本机没有可用第三方 dylib 时该用例自动跳过）。
 * 同样崩溃的还有 createWriter 存在但返回 NULL 的情况。
 * 缺陷登记：docs/迭代计划/迭代01/bugs/R18_wtdtcore.md BUG-R18-007。
 */
TEST_F(R18StateInfoTest, DISABLED_InitCrashesWhenLoadedModuleHasNoWriterSymbols)
{
	const fs::path binDir = _dir / "bin3";
	fs::create_directories(binDir);
	WtHelper::set_module_dir((binDir.string() + "/").c_str());

	const fs::path foreign = find_foreign_dylib();
	if (foreign.empty())
	{
		GTEST_SKIP() << "本机没有可用作负样本的第三方 dylib";
	}

	std::error_code ec;
	// 默认路径 :74 拼出来的就是这个名字
	fs::copy_file(foreign, binDir / "libWtDataStorage.dylib",
		fs::copy_options::overwrite_existing, ec);
	ASSERT_FALSE(ec) << ec.message();

	write_file(_dir / "c.json", R"JSON({"filepath": "r18_state_nosym"})JSON");
	WTSVariant* params = WTSCfgLoader::load_from_file((_dir / "c.json").string().c_str());
	ASSERT_NE(params, nullptr);
	DataManager dm;
	// 走到 :106 即崩
	dm.init(params, nullptr, nullptr);
	params->release();
}

}  // namespace
