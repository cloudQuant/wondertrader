/*!
 * \file q05_idxworker_mgr_test.cpp
 * \brief WtDtCore/IndexWorkerMgr 覆盖用例（迭代01 第五波 q05_dtsrv）
 *
 * 覆盖目标：
 *  - loadFactories 的目录不存在 / 空目录 / 非 dylib 与子目录跳过（不加载真实 DLL）
 *  - init 的无 indice 数组返回 false、active=false、工厂缺失、名称无点号等跳过分支
 *  - IIndexContext 实现：get_hot_mgr/get_bd_mgr/sub_ticks/get_tick/push_tick/output_log
 *  - handle_quote 的 NULL tick、未订阅、已订阅（_workers 空同步遍历）三条路径
 *  - IdxWorkerWrapper 的 self() 与析构回调 delete_worker
 *  poolsize 固定为 0：_pool 保持空，handle_quote 走同步分支，不起任何线程。
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
#include <WtDtCore/IndexWorkerMgr.h>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSTools/WTSBaseDataMgr.h>
#include <WTSTools/WTSHotMgr.h>
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

class Q05IdxWorkerMgrTest : public ::testing::Test
{
protected:
	void SetUp() override { _dir = make_tmp_dir("q05_idx"); }

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	WTSVariant* loadCfg(const char* name, const std::string& body)
	{
		write_file(_dir / name, body);
		WTSVariant* v = WTSCfgLoader::load_from_file((_dir / name).string().c_str());
		return v;
	}

	fs::path _dir;
};

TEST_F(Q05IdxWorkerMgrTest, LoadFactoriesMissingDir)
{
	IndexWorkerMgr mgr;
	EXPECT_FALSE(mgr.loadFactories((_dir / "no_such_dir").string().c_str()));
}

TEST_F(Q05IdxWorkerMgrTest, LoadFactoriesSkipsDirsAndNonDylib)
{
	fs::path factDir = _dir / "factories";
	fs::create_directories(factDir);
	write_file(factDir / "readme.txt", "not a module");
	fs::create_directories(factDir / "subdir");

	IndexWorkerMgr mgr;
	// 目录存在：子目录与非 .dylib 文件被跳过，count=0，仍返回 true
	EXPECT_TRUE(mgr.loadFactories(factDir.string().c_str()));
}

TEST_F(Q05IdxWorkerMgrTest, InitWithoutIndiceArrayReturnsFalse)
{
	WTSVariant* cfg = loadCfg("idx1.json", R"JSON({"poolsize": 0})JSON");
	ASSERT_NE(cfg, nullptr);

	IndexWorkerMgr mgr;
	DataManager dm;  // 未 init：_writer==NULL，所有取数委托返回 NULL/false
	EXPECT_FALSE(mgr.init(cfg, nullptr, nullptr, &dm));
	cfg->release();
}

TEST_F(Q05IdxWorkerMgrTest, InitSkipsInactiveAndUnknownWorkers)
{
	WTSVariant* cfg = loadCfg("idx2.json", R"JSON({
		"poolsize": 0,
		"indice": [
			{"active": false, "id": "off1",  "name": "ma.MA"},
			{"active": true,  "id": "nodot", "name": "nodotname"},
			{"active": true,  "id": "ma1",   "name": "nofact.MA"}
		]
	})JSON");
	ASSERT_NE(cfg, nullptr);

	IndexWorkerMgr mgr;
	DataManager dm;
	// active=false 跳过；无点号名字与未知工厂均 createWorker 失败后 continue
	ASSERT_TRUE(mgr.init(cfg, nullptr, nullptr, &dm));

	// 没有任何工厂被加载时 init 依旧返回 true（逐项 continue）
	WTSVariant* empty = loadCfg("idx3.json", R"JSON({"poolsize": 0, "indice": []})JSON");
	ASSERT_NE(empty, nullptr);
	IndexWorkerMgr mgr2;
	EXPECT_TRUE(mgr2.init(empty, nullptr, nullptr, &dm));

	cfg->release();
	empty->release();
}

TEST_F(Q05IdxWorkerMgrTest, ContextDelegatesWithoutWriter)
{
	WTSVariant* cfg = loadCfg("idx4.json", R"JSON({"poolsize": 0, "indice": []})JSON");
	ASSERT_NE(cfg, nullptr);

	IndexWorkerMgr mgr;
	DataManager dm;
	ASSERT_TRUE(mgr.init(cfg, nullptr, nullptr, &dm));

	EXPECT_EQ(mgr.get_hot_mgr(), nullptr);
	EXPECT_EQ(mgr.get_bd_mgr(), nullptr);

	// 未 init 的 DataManager：getCurTick/writeTick 全走 _writer==NULL 守卫
	EXPECT_EQ(mgr.get_tick("600000", "SSE"), nullptr);
	EXPECT_EQ(mgr.sub_ticks("SSE.600000"), nullptr);
	mgr.push_tick(nullptr);  // -> dm.writeTick(nullptr,1) -> false，不崩
	mgr.output_log(LL_WARN, "q05 index worker mgr raw log");

	cfg->release();
}

TEST_F(Q05IdxWorkerMgrTest, HandleQuoteNullUnsubscribedAndSubscribed)
{
	WTSVariant* cfg = loadCfg("idx5.json", R"JSON({"poolsize": 0, "indice": []})JSON");
	ASSERT_NE(cfg, nullptr);

	IndexWorkerMgr mgr;
	DataManager dm;
	ASSERT_TRUE(mgr.init(cfg, nullptr, nullptr, &dm));

	WTSTickData* tick = WTSTickData::create("600000");
	ASSERT_NE(tick, nullptr);
	WTSContractInfo* cInfo = WTSContractInfo::create("600000", "测试合约", "SSE", "a60");
	ASSERT_NE(cInfo, nullptr);
	tick->setContractInfo(cInfo);

	// 1) NULL 入参直接返回
	mgr.handle_quote(nullptr);

	// 2) 未订阅：_subbed.find(fullCode) 命中 end() 分支
	const char* fullCode = cInfo->getFullCode();
	ASSERT_NE(fullCode, nullptr);
	mgr.handle_quote(tick);

	// 3) 已订阅但 _workers 为空：retain -> 空循环 -> （_pool 为空不 release）
	//    注意：无 _pool 的同步分支不 release，引用计数由调用方管理
	EXPECT_EQ(mgr.sub_ticks(fullCode), nullptr);
	mgr.handle_quote(tick);

	tick->release();
	cfg->release();
}

class FakeIdxWorker : public wtp::IIndexWorker
{
public:
	FakeIdxWorker() : IIndexWorker("q05w") {}

	virtual const char* get_fact_name() override { return "q05fact"; }
	virtual bool init(wtp::WTSVariant* config) override { (void)config; return true; }
	virtual void handle_quote(wtp::WTSTickData* newTick) override { (void)newTick; _quoteCnt++; }

	int _quoteCnt { 0 };
};

class FakeIdxFact : public wtp::IIndexWorkerFact
{
public:
	virtual const char* get_name() override { return "q05fact"; }

	virtual wtp::IIndexWorker* create_worker(const char* name, const char* id) override
	{
		(void)id;
		if (strcmp(name, "Known") != 0)
			return nullptr;
		return &_worker;
	}

	virtual bool delete_worker(wtp::IIndexWorker* worker) override
	{
		_delCnt++;
		return worker == &_worker;
	}

	FakeIdxWorker _worker;
	int _delCnt { 0 };
};

TEST_F(Q05IdxWorkerMgrTest, WrapperOwnsAndReturnsDeleteToFactory)
{
	WTSVariant* cfg = loadCfg("idx_wrapper.json", R"JSON({"poolsize": 0, "indice": []})JSON");
	ASSERT_NE(cfg, nullptr);
	WTSHotMgr hot;
	WTSBaseDataMgr bd;
	DataManager dm;
	IndexWorkerMgr mgr;
	ASSERT_TRUE(mgr.init(cfg, &hot, &bd, &dm));
	cfg->release();

	FakeIdxFact fact;
	{
		wtp::IIndexWorker* w = fact.create_worker("Known", "q05id");
		ASSERT_EQ(w, &fact._worker);
		IdxWorkerWrapper wrapper(w, &fact);
		EXPECT_EQ(wrapper.self(), w);
		EXPECT_EQ(fact._delCnt, 0);
	}
	// 析构把 worker 归还给工厂
	EXPECT_EQ(fact._delCnt, 1);

	// mgr 已通过空 indice 配置完成 init，getter 返回本次注入的有效 manager 地址。
	FakeIdxWorker worker;
	worker.set_factory(&mgr);
	EXPECT_EQ(mgr.get_hot_mgr(), &hot);
	EXPECT_EQ(mgr.get_bd_mgr(), &bd);
}

}  // namespace
