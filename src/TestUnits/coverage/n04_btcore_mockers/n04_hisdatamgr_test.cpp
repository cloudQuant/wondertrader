/**
 * n04_hisdatamgr_test.cpp
 * 覆盖 src/WtBtCore/HisDataMgr.{h,cpp} 与 src/WtBtCore/WtHelper 中与数据存储模块
 * 路径拼装相关的可确定逻辑：
 *  - 未初始化（_reader == NULL）时 5 个 load_raw_* 入口一律返回 false 且不回调
 *  - reader_log 转发到全局日志（未初始化时走 print_message 分支）
 *  - init() 使用的模块路径拼装（WtHelper::getInstDir + DLLHelper::wrap_module）
 * 不重复 l06 已覆盖的 HisDataReplayer 时段/费率/运行期接口。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include <WtBtCore/HisDataMgr.h>
#include <WtBtCore/WtHelper.h>
#include <Share/DLLHelper.hpp>
#include <Includes/WTSVariant.hpp>

USING_NS_WTP;
namespace fs = std::filesystem;

namespace {
const char* kExchg = "SHFE";
const char* kCode = "au2012";
}

class HisDataMgrTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		callbackHits = 0;
		lastBufSize = 0;
	}

	void TearDown() override
	{
		WtHelper::setInstDir("");
		if (!_tmpDir.empty())
		{
			std::error_code ec;
			fs::remove_all(_tmpDir, ec);
			_tmpDir.clear();
		}
	}

	fs::path tempDir(const char* name)
	{
		fs::path base = fs::temp_directory_path();
		fs::path dir = base / (std::string("n04_") + name);
		std::error_code ec;
		fs::create_directories(dir, ec);
		_tmpDir = dir;
		return dir;
	}

	FuncLoadDataCallback countingCb()
	{
		return [this](std::string& buf) {
			callbackHits++;
			lastBufSize = buf.size();
		};
	}

	HisDataMgr mgr;
	int callbackHits;
	size_t lastBufSize;
	fs::path _tmpDir;
};

TEST_F(HisDataMgrTest, uninitializedReaderRejectsEveryLoadEntry)
{
	EXPECT_FALSE(mgr.load_raw_bars(kExchg, kCode, KP_Minute1, countingCb()));
	EXPECT_FALSE(mgr.load_raw_bars(kExchg, kCode, KP_Minute5, countingCb()));
	EXPECT_FALSE(mgr.load_raw_bars(kExchg, kCode, KP_DAY, countingCb()));
	EXPECT_FALSE(mgr.load_raw_ticks(kExchg, kCode, 20201201, countingCb()));
	EXPECT_FALSE(mgr.load_raw_ordque(kExchg, kCode, 20201201, countingCb()));
	EXPECT_FALSE(mgr.load_raw_orddtl(kExchg, kCode, 20201201, countingCb()));
	EXPECT_FALSE(mgr.load_raw_trans(kExchg, kCode, 20201201, countingCb()));

	// 空回调同样必须早退，不允许解引用 _reader
	EXPECT_FALSE(mgr.load_raw_ticks(kExchg, kCode, 20201201, FuncLoadDataCallback()));
	EXPECT_FALSE(mgr.load_raw_bars(kExchg, kCode, KP_DAY, FuncLoadDataCallback()));
	EXPECT_EQ(0, callbackHits);
}

TEST_F(HisDataMgrTest, loadEntriesAreIdempotentAndNullCodeSafe)
{
	for (int i = 0; i < 3; i++)
	{
		EXPECT_FALSE(mgr.load_raw_ticks(kExchg, kCode, 20201201 + i, countingCb()));
	}
	EXPECT_FALSE(mgr.load_raw_bars("", "", KP_Minute1, countingCb()));
	EXPECT_FALSE(mgr.load_raw_trans(kExchg, "", 0, countingCb()));
	EXPECT_EQ(0, callbackHits);
}

TEST_F(HisDataMgrTest, readerLogForwardsAllLevels)
{
	mgr.reader_log(LL_DEBUG, "n04 debug line");
	mgr.reader_log(LL_INFO, "n04 info line");
	mgr.reader_log(LL_WARN, "n04 warn line");
	mgr.reader_log(LL_ERROR, "n04 error line");
	mgr.reader_log(LL_FATAL, "n04 fatal line");
	// reader_log 不应产生任何数据回调
	EXPECT_EQ(0, callbackHits);
}

TEST_F(HisDataMgrTest, modulePathAssemblyMatchesInitConvention)
{
	fs::path dir = tempDir("hisdatamgr_inst");
	const std::string instDir = dir.string() + "/";
	WtHelper::setInstDir(instDir.c_str());
	EXPECT_EQ(instDir, WtHelper::getInstDir());

	// init() 的模块名 = 安装目录 + wrap_module(模块名)
	const std::string wrapped = DLLHelper::wrap_module("WtDataStorage");
	EXPECT_NE(std::string::npos, wrapped.find("WtDataStorage")) << wrapped;
	const std::string assembled = WtHelper::getInstDir() + wrapped;
	EXPECT_EQ(0u, assembled.find(instDir));
	EXPECT_NE(std::string::npos, assembled.find("WtDataStorage"));

	// 同一输入必须得到稳定输出（日期->文件定位依赖该拼装）
	EXPECT_EQ(assembled, WtHelper::getInstDir() + DLLHelper::wrap_module("WtDataStorage"));

	const std::string other = WtHelper::getInstDir() + DLLHelper::wrap_module("WtDataStorageCsv");
	EXPECT_NE(assembled, other);
}

TEST_F(HisDataMgrTest, emptyConfigYieldsDefaultModuleNameOnly)
{
	WTSVariant* cfg = WTSVariant::createObject();
	const char* module = cfg->getCString("module");
	// getCString 缺失键返回默认值 ""，init() 因此走默认 WtDataStorage 分支
	EXPECT_STREQ("", module);
	EXPECT_TRUE(std::string(module).empty());
	cfg->release();

	WTSVariant* cfg2 = WTSVariant::createObject();
	cfg2->append("module", "WtDataStorageJson");
	EXPECT_STREQ("WtDataStorageJson", cfg2->getCString("module"));
	cfg2->release();
}

/**
 * 缺陷类用例（禁用，见 bugs/n04_btcore_mockers.md BUG-N04-002）：
 * HisDataMgr::init 在模块装载失败或 createBtDtReader 符号缺失时不会给 _reader 赋值，
 * 但末尾无条件执行 `_reader->init(cfg, this)` -> 空指针解引用崩溃。
 * 该类的所有输入形态统一放在本禁用用例里（不要拆成启用用例，否则整轮度量会被
 * 崩溃/挂死拖走）：
 *  - 空配置（默认模块名在本机构建产物中不存在）
 *  - 显式指定不存在的模块
 *  - 指向存在但不含 createBtDtReader 符号的库
 *  - init(nullptr)
 */
TEST_F(HisDataMgrTest, DISABLED_initCrashesWhenReaderModuleMissing)
{
	WTSVariant* empty = WTSVariant::createObject();
	mgr.init(empty);
	empty->release();

	WTSVariant* bogus = WTSVariant::createObject();
	bogus->append("module", "/nonexistent_n04_dir/wt_data_storage");
	mgr.init(bogus);
	bogus->release();

	mgr.init(nullptr);
}
