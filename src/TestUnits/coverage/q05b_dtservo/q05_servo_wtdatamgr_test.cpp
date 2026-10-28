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
 * 崩溃类输入（initStore 加载失败后 _reader 空指针解引用、subscribe_bar 无 reader
 * 直接取数）整类收进同一个 DISABLED_ 用例，启用用例零残留（见 BUG-Q05-001）。
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
#include <WtDtServo/WtDataManager.h>
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

class Q05ServoWtDataManagerTest : public ::testing::Test
{
protected:
	void SetUp() override { _dir = make_tmp_dir("q05_srvodm"); }

	void TearDown() override
	{
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

/*!
 * 崩溃类输入整类收口（默认禁用，禁止在 CI 启用）：
 *  1) initStore 传入含 store 段的配置但模块加载失败时，_reader 保持 NULL，
 *     WtDtServo/WtDataManager.cpp:87 无条件 _reader->init(cfg, this) -> 段错误。
 *  2) subscribe_bar 在未初始化 _reader 时直接 _reader->readKlineSliceByCount，
 *     同属空指针解引用一类。
 * 任何一条都会终止进程，因此二者只保留第一条作为代表（第二条不可达）。
 */
TEST_F(Q05ServoWtDataManagerTest, DISABLED_InitStoreLoadFailureDerefsNullReader)
{
	WtDataManager dm;
	WTSVariant* cfg = loadCfg("badstore.json",
		R"JSON({"align_by_section": false,
			"store": {"module": "q05_no_such_storage_module", "storedir": ""}})JSON");
	ASSERT_NE(cfg, nullptr);
	// 期望以段错误终止 —— 该用例的存在本身就是缺陷证明，见 BUG-Q05-001
	dm.init(cfg, nullptr);
	cfg->release();
}

}  // namespace
