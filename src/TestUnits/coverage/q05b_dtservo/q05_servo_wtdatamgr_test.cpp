/*!
 * \file q05_servo_wtdatamgr_test.cpp
 * \brief WtDtServo/WtDataManager 覆盖用例（迭代01 第五波 q05_dtsrv）
 *
 * 覆盖目标：
 *  - 构造/析构的空成员初值（_reader/_rt_bars/_bd_mgr/_hot_mgr 均为 NULL）
 *  - initStore(NULL cfg) 的早退 false 分支
 *  - init(cfg, nullptr) 在无 runner、cfg 无 store 段时安全返回 false
 *  - get_exright_factor 的 commInfo==NULL 恒 1.0 分支
 *  - update_bars 的 _rt_bars==NULL 早退、clear_subbed_bars 的空表清空、
 *    reader_log / clear_cache（_reader==NULL 取消分支）
 * initStore 覆盖缺模块、已加载模块缺符号、工厂返回 NULL 三种失败路径，并验证失败后查询安全；
 * WtDataStorage 成功路径由同目标的 R102WtDataManagerTest 正控覆盖。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <Includes/WTSVariant.hpp>
#include <Share/DLLHelper.hpp>
#include <WtDtServo/WtDataManager.h>
#include <WtDtServo/WtHelper.h>
#include <WTSUtils/WTSCfgLoader.h>

namespace fs = std::filesystem;

namespace {

void expectUninitializedReaderQueriesAreSafe(WtDataManager& dm)
{
	EXPECT_EQ(dm.get_tick_slices_by_range("SSE.STK.600100", 20200101090000), nullptr);
	EXPECT_EQ(dm.get_tick_slice_by_date("SSE.STK.600100", 20200101), nullptr);
	EXPECT_EQ(dm.get_tick_slice_by_count("SSE.STK.600100", 10), nullptr);
	EXPECT_EQ(dm.get_order_queue_slice("SSE.STK.600100", 20200101090000), nullptr);
	EXPECT_EQ(dm.get_order_detail_slice("SSE.STK.600100", 20200101090000), nullptr);
	EXPECT_EQ(dm.get_transaction_slice("SSE.STK.600100", 20200101090000), nullptr);
	EXPECT_EQ(dm.get_skline_slice_by_date("SSE.STK.600100", 30, 20200101), nullptr);
	EXPECT_EQ(dm.get_kline_slice_by_date("SSE.STK.600100", KP_DAY, 1, 20200101), nullptr);
	EXPECT_EQ(dm.get_kline_slice_by_range("SSE.STK.600100", KP_DAY, 1, 20200101090000), nullptr);
	EXPECT_EQ(dm.get_kline_slice_by_range("SSE.STK.600100", KP_DAY, 5, 20200101090000), nullptr);
	EXPECT_EQ(dm.get_kline_slice_by_count("SSE.STK.600100", KP_DAY, 1, 10), nullptr);
	EXPECT_EQ(dm.get_kline_slice_by_count("SSE.STK.600100", KP_DAY, 5, 10), nullptr);
	EXPECT_DOUBLE_EQ(dm.get_exright_factor("SSE.STK.600100"), 1.0);
	EXPECT_NO_THROW(dm.subscribe_bar("SSE.STK.600100", KP_DAY, 1));
	EXPECT_NO_THROW(dm.clear_cache());
}

fs::path make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

class Q05ServoWtDataManagerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = make_tmp_dir("q05_srvodm");
		_originalModuleDir = WtHelper::get_module_dir();
		WtHelper::set_module_dir(WT_Q05_READER_MODULE_DIR);
	}

	void TearDown() override
	{
		WtHelper::set_module_dir(_originalModuleDir.c_str());
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	WTSVariant* loadCfg(const char* name, const std::string& body)
	{
		fs::path p = _dir / name;
		std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
		ofs << body;
		ofs.close();
		return WTSCfgLoader::load_from_file(p.string().c_str());
	}

	fs::path _dir;
	std::string _originalModuleDir;
};

TEST_F(Q05ServoWtDataManagerTest, FreshInstanceGuardsReturnDefaults)
{
	WtDataManager dm;

	// commInfo==NULL：不触碰 _reader/_hot_mgr，直接返回基准复权因子 1.0
	EXPECT_DOUBLE_EQ(dm.get_exright_factor("a60"), 1.0);

	// _rt_bars==NULL：update_bars 与 clear_subbed_bars 都应立即返回而不解引用
	dm.update_bars("a60", nullptr);
	dm.clear_subbed_bars();

	// _reader==NULL：clear_cache 走 "clearing canceled" 警告分支
	dm.clear_cache();

	dm.reader_log(LL_INFO, "q05 servo datamgr raw log");
}

TEST_F(Q05ServoWtDataManagerTest, InitWithoutRunnerAndStoreSectionIsSafe)
{
	WtDataManager dm;
	WTSVariant* cfg = loadCfg("nomgr.json",
		R"JSON({"align_by_section": true})JSON");
	ASSERT_NE(cfg, nullptr);

	// runner==NULL -> 不取 bd/hot mgr；cfg->get("store")==NULL -> initStore 早退 false
	// （initStore 为私有，简报标注的 L77 实为 private，经 init 间接覆盖其 NULL 分支）
	EXPECT_FALSE(dm.init(cfg, nullptr));
	cfg->release();
}

TEST_F(Q05ServoWtDataManagerTest, InitWithFalseAlignFlagStillSkipsStore)
{
	WtDataManager dm;
	WTSVariant* cfg = loadCfg("alignfalse.json",
		R"JSON({"align_by_section": false, "poolsize": 1})JSON");
	ASSERT_NE(cfg, nullptr);
	// store 段整体缺失：cfg->get("store")==NULL -> initStore 早退 false，不应崩溃
	EXPECT_FALSE(dm.init(cfg, nullptr));
	cfg->release();
}

TEST_F(Q05ServoWtDataManagerTest, InitStoreLoadFailureReturnsFalseWithoutDereferencingReader)
{
	WtDataManager dm;
	WTSVariant* cfg = loadCfg("badstore.json",
		R"JSON({"align_by_section": false,
			"store": {"module": "q05_no_such_storage_module", "storedir": ""}})JSON");
	ASSERT_NE(cfg, nullptr);
	EXPECT_FALSE(dm.init(cfg, nullptr));
	expectUninitializedReaderQueriesAreSafe(dm);
	cfg->release();
}

TEST_F(Q05ServoWtDataManagerTest, InitStoreLoadedModuleMissingReaderSymbolsReturnsFalse)
{
	const std::string modulePath = std::string(WT_Q05_READER_MODULE_DIR)
		+ DLLHelper::wrap_module("WtIndexFactory");
	DllHandle handle = DLLHelper::load_library(modulePath.c_str());
	ASSERT_NE(handle, nullptr) << "expected real module to load: " << modulePath;
	EXPECT_EQ(DLLHelper::get_symbol(handle, "createRdmDtReader"), nullptr);
	EXPECT_EQ(DLLHelper::get_symbol(handle, "deleteRdmDtReader"), nullptr);
	DLLHelper::free_library(handle);

	WtDataManager dm;
	WTSVariant* cfg = loadCfg("missing_reader_symbols.json",
		R"JSON({"align_by_section": false,
			"store": {"module": "WtIndexFactory", "storedir": ""}})JSON");
	ASSERT_NE(cfg, nullptr);
	EXPECT_FALSE(dm.init(cfg, nullptr));
	expectUninitializedReaderQueriesAreSafe(dm);
	EXPECT_NO_THROW(dm.reader_log(LL_INFO, "q05 missing reader symbols"));
	cfg->release();
}

TEST_F(Q05ServoWtDataManagerTest, InitStoreFactoryReturningNullReturnsFalse)
{
	const std::string modulePath = std::string(WT_Q05_READER_MODULE_DIR)
		+ DLLHelper::wrap_module(WT_Q05_NULL_READER_MODULE);
	DllHandle handle = DLLHelper::load_library(modulePath.c_str());
	ASSERT_NE(handle, nullptr) << "expected null-reader fixture to load: " << modulePath;
	EXPECT_NE(DLLHelper::get_symbol(handle, "createRdmDtReader"), nullptr);
	EXPECT_NE(DLLHelper::get_symbol(handle, "deleteRdmDtReader"), nullptr);
	DLLHelper::free_library(handle);

	WtDataManager dm;
	WTSVariant* cfg = loadCfg("null_reader.json",
		std::string(R"JSON({"align_by_section": false,
			"store": {"module": ")JSON") + WT_Q05_NULL_READER_MODULE
			+ R"JSON(", "storedir": ""}})JSON");
	ASSERT_NE(cfg, nullptr);
	EXPECT_FALSE(dm.init(cfg, nullptr));
	expectUninitializedReaderQueriesAreSafe(dm);
	EXPECT_NO_THROW(dm.reader_log(LL_INFO, "q05 null reader factory"));
	cfg->release();
}

}  // namespace
