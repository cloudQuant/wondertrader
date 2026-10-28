/*!
 * \file r8_wtdatamgr_sinks_test.cpp
 * \brief WtDtServo/WtDataManager 加深用例（迭代01 第七波 R8）
 *
 * 与同目录 q05_servo_wtdatamgr_test.cpp（第四/五波遗留）互补，本文件只打：
 *  - IRdmDtReaderSink 的两个访问器（WtDataManager.h:95 / :100）在 init 前后的差异，
 *    以此证明 WtDataManager.cpp:93-98 的 runner 绑定确实发生
 *  - init 的 align_by_section true/false 两条（:100-102）与 store 段缺失的 false 返回（:104）
 *  - get_exright_factor 的非股票（期货）分支：:495 false -> :498-503 -> :504
 *  - reader_log（:107-110）与 init 失败后的 clear_cache 取消分支（:609-612）
 * 会解引用 _reader 的入口（get_*slice / subscribe_bar）在本二进制里必然段错误：
 * WtDataStorage 插件的 WtRdmDtReader::init 在起线程前没有可注入的空实现
 * （src/WtDataStorage/WtRdmDtReader.cpp:87 无条件 StdThread），
 * 而测试禁止起线程，故只留 DISABLED_ 用例并在 bugs 里登记。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

#include <filesystem>

#include <Includes/IHotMgr.h>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WTSTools/WTSBaseDataMgr.h>
#include <WTSTools/WTSHotMgr.h>
#include <WtDtServo/WtDataManager.h>
#include <WtDtServo/WtDtRunner.h>
#include <WtDtServo/WtHelper.h>

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

class R8WtDataManagerSinksTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = make_tmp_dir("r8_dm");
		// 空模块目录：initStore 无论用默认 "WtDataStorage" 还是自定义名字都必然加载失败
		WtHelper::set_module_dir((_dir / "bin/").string().c_str());

		write_file(_dir / "sessions.json",
			R"JSON({"SD0930":{"name":"股指日盘","offset":0,"auction":{"from":929,"to":930},
				"sections":[{"from":930,"to":1130}]}})JSON");
		// category=1 => CC_Future（WTSTypes.h:20-23），故 isStock() 为假
		// (WTSContractInfo.hpp:80-83)，复权因子走热点规则分支而不是 _reader
		write_file(_dir / "commodities.json",
			R"JSON({"CFFEX":{"IC":{"covermode":0,"pricemode":0,"category":1,"precision":1,
				"pricetick":0.2,"volscale":200,"name":"中证500","exchg":"CFFEX",
				"session":"SD0930","holiday":"CHINA"}}})JSON");

		ASSERT_TRUE(_bdMgr.loadSessions((_dir / "sessions.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadCommodities((_dir / "commodities.json").string().c_str()));
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	WTSVariant* loadCfg(const char* name, const std::string& body)
	{
		fs::path p = _dir / name;
		write_file(p, body);
		WTSVariant* ret = WTSCfgLoader::load_from_file(p.string().c_str());
		_cfgs.push_back(ret);
		return ret;
	}

	fs::path _dir;
	WTSBaseDataMgr _bdMgr;
	std::vector<WTSVariant*> _cfgs;
};

TEST_F(R8WtDataManagerSinksTest, FreshInstanceExposesNullSinks)
{
	WtDataManager dm;
	// 构造只置空成员（WtDataManager.cpp:29-36），两个 sink 访问器必须还是 NULL
	EXPECT_EQ(dm.get_basedata_mgr(), nullptr);
	EXPECT_EQ(dm.get_hot_mgr(), nullptr);
	// commInfo==NULL -> :492-493 基准因子 1.0，不碰 _reader
	EXPECT_DOUBLE_EQ(dm.get_exright_factor("CFFEX.IC.2204"), 1.0);
}

TEST_F(R8WtDataManagerSinksTest, InitBindsRunnerSinksEvenWhenStoreIsMissing)
{
	WtDtRunner runner;
	WtDataManager dm;
	// 无 store 段 -> initStore(NULL) 在 :51-52 返回 false，但 :93-98 已先绑定
	WTSVariant* cfg = loadCfg("nostore.json", R"JSON({"align_by_section": false})JSON");
	ASSERT_NE(cfg, nullptr);

	ASSERT_FALSE(dm.init(cfg, &runner));
	EXPECT_EQ(dm.get_basedata_mgr(), static_cast<IBaseDataMgr*>(&runner.getBaseDataMgr()));
	EXPECT_EQ(dm.get_hot_mgr(), static_cast<IHotMgr*>(&runner.getHotMgr()));

	// _reader 仍为 NULL -> 只能走取消分支（:609-612），不得解引用
	dm.clear_cache();
	dm.reader_log(LL_WARN, "r8 datamgr sink raw log");
}

TEST_F(R8WtDataManagerSinksTest, InitWithAlignFlagTrueTakesSecondBranch)
{
	WtDtRunner runner;
	WtDataManager dm;
	WTSVariant* cfg = loadCfg("aligntrue.json", R"JSON({"align_by_section": true, "poolsize": 2})JSON");
	ASSERT_NE(cfg, nullptr);
	// :100-102 读到的布尔值无外部可见副作用，用 sink 绑定 + 返回值 false 作为可证伪面
	EXPECT_FALSE(dm.init(cfg, &runner));
	EXPECT_NE(dm.get_basedata_mgr(), nullptr);
	EXPECT_NE(dm.get_hot_mgr(), nullptr);
}

TEST_F(R8WtDataManagerSinksTest, InitWithoutRunnerLeavesSinksNull)
{
	WtDataManager dm;
	WTSVariant* cfg = loadCfg("nostore2.json", R"JSON({"align_by_section": true})JSON");
	ASSERT_NE(cfg, nullptr);
	// runner==NULL -> :94 的分支不进，两个 sink 保持 NULL
	EXPECT_FALSE(dm.init(cfg, nullptr));
	EXPECT_EQ(dm.get_basedata_mgr(), nullptr);
	EXPECT_EQ(dm.get_hot_mgr(), nullptr);
}

TEST_F(R8WtDataManagerSinksTest, NonStockCommodityTakesHotRuleFactorBranch)
{
	WtDtRunner runner;
	// 把品种表挂到 runner 自带的基础数据管理器上，使 WtDataManager 的 sink 指向同一份
	WTSBaseDataMgr& bdm = runner.getBaseDataMgr();
	ASSERT_TRUE(bdm.loadSessions((_dir / "sessions.json").string().c_str()));
	ASSERT_TRUE(bdm.loadCommodities((_dir / "commodities.json").string().c_str()));
	WTSCommodityInfo* commInfo = bdm.getCommodity("CFFEX", "IC");
	ASSERT_NE(commInfo, nullptr);
	ASSERT_FALSE(commInfo->isStock());

	WtDataManager dm;
	WTSVariant* cfg = loadCfg("nostore3.json", R"JSON({"align_by_section": false})JSON");
	ASSERT_NE(cfg, nullptr);
	dm.init(cfg, &runner);

	// _hot_mgr 未加载任何切换规则 -> WTSHotMgr.cpp:36-37 getRuleTag 返回 ""，
	// strlen(ruleTag)==0 使 :500 条件为假，最终落到 :504 的 1.0
	EXPECT_DOUBLE_EQ(dm.get_exright_factor("CFFEX.IC.2204", commInfo), 1.0);
	// 复权后缀会被 getRuleTag 去掉（:39-41），结果仍为 1.0
	EXPECT_DOUBLE_EQ(dm.get_exright_factor("CFFEX.IC.2204+", commInfo), 1.0);
}

TEST_F(R8WtDataManagerSinksTest, InitStoreWithEmptySectionIsNotEvenAttempted)
{
	WtDtRunner runner;
	WtDataManager dm;
	// data 段存在但内容为空对象：cfg->get("store") 返回 NULL -> 走 :51-52，
	// 与 "store":{...} 那条会加载失败的形态（见下面两个 DISABLED）互斥
	WTSVariant* cfg = loadCfg("emptydata.json", R"JSON({})JSON");
	ASSERT_NE(cfg, nullptr);
	EXPECT_FALSE(dm.init(cfg, &runner));
	EXPECT_EQ(dm.get_basedata_mgr(), static_cast<IBaseDataMgr*>(&runner.getBaseDataMgr()));
}

/*!
 * 触发条件（启用即段错误，禁止在 CI 启用）：
 * data 段里有 "store":{} 空对象 -> initStore 的 cfg 非 NULL，module 为空时
 * 取默认名 "WtDataStorage"（WtDataManager.cpp:54-56），模块目录被钉在空目录，
 * load_library 失败（:81-85）后 _reader 仍为 NULL，:87 无条件 _reader->init 崩溃。
 * 与 q05_servo_wtdatamgr_test.cpp 的 DISABLED 用例同根因，本条补充的是
 * 「默认模块名」这一形态（见 bugs/R8_wtdtservo.md BUG-R8-003）。
 */
TEST_F(R8WtDataManagerSinksTest, DISABLED_InitStoreWithDefaultModuleNameDerefsNullReader)
{
	WtDtRunner runner;
	WtDataManager dm;
	WTSVariant* cfg = loadCfg("defstore.json", R"JSON({"align_by_section": false, "store": {}})JSON");
	ASSERT_NE(cfg, nullptr);
	EXPECT_FALSE(dm.init(cfg, &runner));
	cfg->release();
}

/*!
 * 触发条件（启用即段错误，禁止在 CI 启用）：
 * 任何把请求交给 _reader 的公开入口，在 _reader==NULL 时都直接解引用：
 * get_tick_slice_by_date -> WtDataManager.cpp:121，
 * get_tick_slices_by_range -> :116，get_kline_slice_by_range(times==1) -> :203，
 * get_kline_slice_by_count(times==1) -> :341，subscribe_bar -> :516。
 * 这条用例证明「没有可注入的 IRdmDtReader 接缝」= 约 400 行不可达（见 manifest）。
 */
TEST_F(R8WtDataManagerSinksTest, DISABLED_TickSliceDelegationWithoutReaderSegfaults)
{
	WtDtRunner runner;
	WtDataManager dm;
	WTSVariant* cfg = loadCfg("nostore4.json", R"JSON({"align_by_section": false})JSON");
	ASSERT_NE(cfg, nullptr);
	dm.init(cfg, &runner);
	// 若存在 reader 接缝，这里应返回 NULL 切片（日期范围内无数据）
	EXPECT_EQ(dm.get_tick_slice_by_date("CFFEX.IC.2204", 20240102), nullptr);
}

}  // namespace
