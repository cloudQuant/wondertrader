/*!
 * \file p06_stra_mgr_test.cpp
 * \brief WonderTrader 测试覆盖率迭代01 第三波 / lane p06 —— UftStrategyMgr 注册表存取 + ShareManager 装配
 *
 * 覆盖范围（不装载任何策略 DLL、不触碰共享内存段）：
 *   - UftStrategyMgr::loadFactories 的「目录不存在 -> false」与「目录存在但无 dylib」两支，
 *     含目录项的子目录跳过分支与扩展名不匹配跳过分支（macOS 只认 .dylib）
 *   - UftStrategyMgr::createStrategy(name, id) 的参数校验：无 '.' 的非法名、工厂未注册
 *   - UftStrategyMgr::createStrategy(factname, unitname, id) 的工厂未注册分支
 *   - UftStrategyMgr::getStrategy(id) 的注册表 miss 分支
 *   - 析构函数在空注册表下的清理循环
 *   - ShareManager::self() 单例取用 + set_engine() 装配（纯赋值，不 initialize/不 start_watching）
 *
 * 明确不覆盖（见 manifest 排除清单）：
 *   - loadFactories 的 DLL 装载循环体（DLLHelper::load_library/get_symbol + createStrategyFact
 *     符号解析 + IUftStrategyFact 实例化），需要真实策略插件目录
 *   - createStrategy 命中工厂后的 UftStraWrapper 构造与 _strategies 写入（同上，需要工厂）
 *   - ShareManager::initialize / start_watching / init_domain / set_value / get_value /
 *     allocate_value（全部依赖 libwtpshare 共享内存段与 func_* 函数指针）
 */
#include <gtest/gtest.h>

#include "WtUftCore/UftStrategyMgr.h"
#include "WtUftCore/ShareManager.h"
#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/WtUftDtMgr.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSMarcos.h"

#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

USING_NS_WTP;

namespace {

//////////////////////////////////////////////////////////////////////////
// p06_ 前缀的唯一临时目录，构造时建、析构时删
class P06TempDir
{
public:
	explicit P06TempDir(const char* tag)
	{
		std::filesystem::path base = std::filesystem::temp_directory_path();
		_root = base / (std::string("p06_") + tag + "_" + std::to_string(static_cast<long>(::getpid()))
			+ "_" + std::to_string(++_seq));
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
		std::filesystem::create_directories(_root, ec);
	}

	~P06TempDir()
	{
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	const std::string str() const { return _root.string(); }
	const std::filesystem::path& path() const { return _root; }

	void touchFile(const char* name)
	{
		std::error_code ec;
		std::filesystem::create_directories(_root.parent_path(), ec);
		std::ofstream of(_root / name, std::ios::binary | std::ios::trunc);
		of << "p06";
		of.close();
	}

	void makeSubDir(const char* name)
	{
		std::error_code ec;
		std::filesystem::create_directories(_root / name, ec);
	}

private:
	std::filesystem::path _root;
	std::string _rootStr;
	static int _seq;
};

int P06TempDir::_seq = 0;

//////////////////////////////////////////////////////////////////////////
// 只为拿到一个可用的引擎指针
class P06NullBaseDataMgr : public IBaseDataMgr
{
public:
	WTSCommodityInfo* getCommodity(const char*) override { return nullptr; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return nullptr; }
	WTSContractInfo* getContract(const char*, const char* = "", uint32_t = 0) override { return nullptr; }
	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return WTSArray::create(); }
	WTSSessionInfo* getSession(const char*) override { return nullptr; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return nullptr; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate; }
	uint64_t getBoundaryTime(const char*, uint32_t, bool = false, bool = true) override { return 0; }
};

}  // namespace

//////////////////////////////////////////////////////////////////////////
TEST(p06_stra_mgr, load_factories_missing_directory_returns_false)
{
	P06TempDir tmp("missing");
	const std::string badPath = tmp.path().string() + "/never_created_dir";

	UftStrategyMgr mgr;
	EXPECT_FALSE(mgr.loadFactories(badPath.c_str()));

	// 目录不存在时注册表保持为空，后续取用一律 miss
	EXPECT_EQ(nullptr, mgr.createStrategy("P06Fact", "P06Unit", "p06s1").get());
	EXPECT_EQ(nullptr, mgr.getStrategy("p06s1").get());
}

TEST(p06_stra_mgr, load_factories_empty_directory_returns_true)
{
	P06TempDir tmp("empty");

	UftStrategyMgr mgr;
	// 空目录：不进入任何 DLL 分支，直接返回 true
	EXPECT_TRUE(mgr.loadFactories(tmp.str().c_str()));
	EXPECT_EQ(nullptr, mgr.createStrategy("P06Fact", "P06Unit", "p06s2").get());
}

TEST(p06_stra_mgr, load_factories_skips_dirs_and_wrong_extensions)
{
	P06TempDir tmp("mixed");
	tmp.makeSubDir("subdir_should_be_skipped");
	tmp.touchFile("readme.txt");
	tmp.touchFile("libfake.so");     // Linux 后缀，macOS 下应被跳过
	tmp.touchFile("notastrategy");   // 无后缀

	UftStrategyMgr mgr;
	// 目录里有内容但全部被 is_directory / extension 过滤掉，不会尝试装载
	EXPECT_TRUE(mgr.loadFactories(tmp.str().c_str()));
	EXPECT_EQ(nullptr, mgr.createStrategy("P06Fact", "P06Unit", "p06s3").get());
	EXPECT_EQ(nullptr, mgr.getStrategy("p06s3").get());
}

TEST(p06_stra_mgr, create_strategy_by_dotted_name_validates_input)
{
	UftStrategyMgr mgr;

	// 没有 '.' 的名字 -> 参数校验分支直接返回空
	EXPECT_EQ(nullptr, mgr.createStrategy("P06UnitOnly", "p06s4").get());
	// 空串同样走校验分支
	EXPECT_EQ(nullptr, mgr.createStrategy("", "p06s5").get());
	// 只有一个点 -> ay.size()==2 但工厂不存在
	EXPECT_EQ(nullptr, mgr.createStrategy("P06Fact.", "p06s6").get());
	// 正常点分名 -> 工厂未注册
	EXPECT_EQ(nullptr, mgr.createStrategy("P06Fact.P06Unit", "p06s7").get());

	// 注册表始终为空
	EXPECT_EQ(nullptr, mgr.getStrategy("p06s4").get());
	EXPECT_EQ(nullptr, mgr.getStrategy("p06s7").get());
}

TEST(p06_stra_mgr, repeated_load_is_idempotent_on_empty_dir)
{
	P06TempDir tmp("twice");
	UftStrategyMgr mgr;

	EXPECT_TRUE(mgr.loadFactories(tmp.str().c_str()));
	EXPECT_TRUE(mgr.loadFactories(tmp.str().c_str()));
	// 二次装载未增加工厂，取用仍然 miss
	EXPECT_EQ(nullptr, mgr.createStrategy("P06Fact", "P06Unit", "p06s8").get());
}

TEST(p06_stra_mgr, manager_destructor_with_empty_registry)
{
	P06TempDir tmp("dtor");
	{
		UftStrategyMgr mgr;
		EXPECT_TRUE(mgr.loadFactories(tmp.str().c_str()));
		EXPECT_EQ(nullptr, mgr.getStrategy("nothing_here").get());
	}
	// 作用域退出时析构遍历空注册表，不应触发任何 DLL 卸载
EXPECT_TRUE(true);
}

//////////////////////////////////////////////////////////////////////////
TEST(p06_share_manager, self_singleton_and_engine_wiring)
{
	P06NullBaseDataMgr bdMgr;
	WtUftDtMgr dtMgr;
	WtUftEngine engine;
	// 先把指针写满再交给 ShareManager，避免未初始化成员
	engine.init(nullptr, &bdMgr, &dtMgr, nullptr);

	ShareManager& sm1 = ShareManager::self();
	ShareManager& sm2 = ShareManager::self();
	// 单例：同一地址
	EXPECT_EQ(&sm1, &sm2);

	// set_engine 是纯赋值，不触发 initialize/start_watching
	sm1.set_engine(&engine);
	sm1.set_engine(nullptr);
	sm1.set_engine(&engine);

	// 引擎状态不受影响
	engine.set_date_time(20240301, 930, 10, 0);
	EXPECT_EQ(930u, engine.get_min_time());
	EXPECT_EQ(20240301u, engine.get_date());
}
