/*!
 * \file r44_idxworker_fresh_test.cpp
 * \brief WtDtCore/IndexWorkerMgr 未初始化与稀疏配置补测（迭代01 第十波 R44）
 *
 * 与既有 q05_idxworker_mgr_test.cpp / r18_idxworker_mgr_test.cpp 的分工：
 * 那两份都在 init() 之后驱动 handle_quote/sub_ticks/get_tick，或覆盖 loadFactories 的
 * 目录扫描。本文件补：
 *  - 从未 init() 的实例：handle_quote 在 _subbed 未命中时必须在触碰 _data_mgr 之前返回
 *    （IndexWorkerMgr.cpp:170-178）
 *  - 从未 init() 的 output_log（L223-226，不依赖任何成员指针）
 *  - init 的稀疏 indice 项：缺 name/id、空 name、只有 name 无 active，全部安全 continue
 *    （L147-165）
 *  - IdxWorkerWrapper(nullptr, fact)：析构不回调 delete_worker（IndexWorkerMgr.h:27-33）
 *  - sub_ticks 的多段/空段 fullCode 与 get_tick(NULL,NULL) 的 DataManager 守卫
 * 禁用用例记录三类未初始化指针缺陷（见 bugs/R44.md）。
 * 不加载真实指标工厂、不建线程池（poolsize 恒 0）、不 sleep。
 */
#include <gtest/gtest.h>

#include <atomic>
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
#include <WTSUtils/WTSCfgLoader.h>

namespace fs = std::filesystem;

namespace {

fs::path r44_make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

void r44_write_file(const fs::path& p, const std::string& content)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

class R44FakeIndexWorker : public wtp::IIndexWorker
{
public:
	R44FakeIndexWorker() : IIndexWorker("r44") {}
	virtual const char* get_fact_name() override { return "r44fact"; }
	virtual bool init(wtp::WTSVariant* /*config*/) override { return true; }
	virtual void handle_quote(wtp::WTSTickData* /*newTick*/) override {}
};

class R44FakeIndexFact : public wtp::IIndexWorkerFact
{
public:
	virtual const char* get_name() override { return "r44fact"; }
	virtual wtp::IIndexWorker* create_worker(const char* /*name*/, const char* /*id*/) override
	{
		return &_worker;
	}
	virtual bool delete_worker(wtp::IIndexWorker* worker) override
	{
		_delCnt++;
		return worker == &_worker;
	}

	R44FakeIndexWorker _worker;
	int _delCnt { 0 };
};

class R44IndexWorkerFreshTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r44_make_tmp_dir("r44_idx");
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	WTSVariant* loadCfg(const char* name, const std::string& body)
	{
		r44_write_file(_dir / name, body);
		return WTSCfgLoader::load_from_file((_dir / name).string().c_str());
	}

	static WTSTickData* makeTick(WTSContractInfo* cInfo)
	{
		WTSTickStruct s;
		memset(&s, 0, sizeof(s));
		strcpy(s.code, "600000");
		strcpy(s.exchg, "SSE");
		s.action_date = 20240102;
		s.trading_date = 20240102;
		s.price = 10.0;
		WTSTickData* tick = WTSTickData::create(s);
		if (cInfo != nullptr)
			tick->setContractInfo(cInfo);
		return tick;
	}

	fs::path _dir;
};

// IndexWorkerMgr.cpp:170-178 —— 未 init 的实例 _subbed 为空，handle_quote 必须先返回，
// 不能触碰未初始化的 _data_mgr
TEST_F(R44IndexWorkerFreshTest, FreshMgrHandleQuoteReturnsBeforeDataManager)
{
	IndexWorkerMgr mgr;
	WTSContractInfo* ct = WTSContractInfo::create("600000", "测试合约", "SSE", "STK");
	ASSERT_NE(ct, nullptr);
	WTSTickData* tick = makeTick(ct);
	ASSERT_NE(tick, nullptr);
	ASSERT_EQ(tick->retainCount(), 1u);

	mgr.handle_quote(tick);
	// 未订阅 -> 不动引用计数，也不碰 _data_mgr
	EXPECT_EQ(tick->retainCount(), 1u);

	tick->release();
	ct->release();
}

// IndexWorkerMgr.cpp:223-226 —— output_log 只转发到 WTSLogger，不依赖 _hot_mgr/_bd_mgr/_data_mgr
TEST_F(R44IndexWorkerFreshTest, FreshMgrOutputLogIsSafe)
{
	IndexWorkerMgr mgr;
	mgr.output_log(LL_INFO, "r44 fresh index worker mgr log");
	mgr.output_log(LL_FATAL, "r44 fresh index worker mgr fatal");
}

// IndexWorkerMgr.cpp:147-165 —— indice 项缺 name/id、name 为空、无 active 时都走
// createWorker 的 split 长度不足/工厂缺失分支，整体仍返回 true
TEST_F(R44IndexWorkerFreshTest, InitWithSparseIndiceEntriesSkipsEachItem)
{
	WTSVariant* cfg = loadCfg("sparse.json", R"JSON({
		"poolsize": 0,
		"indice": [
			{"active": true},
			{"active": true, "name": "", "id": ""},
			{"active": true, "name": "a.b", "id": "x"},
			{"name": "c.d", "id": "y"}
		]
	})JSON");
	ASSERT_NE(cfg, nullptr);

	DataManager dm;	// 未 init，_writer==NULL
	IndexWorkerMgr mgr;
	EXPECT_TRUE(mgr.init(cfg, nullptr, nullptr, &dm));
	cfg->release();
}

// IndexWorkerMgr.h:27-33 —— _worker==NULL 时 self() 返回空、析构不触发 delete_worker
TEST_F(R44IndexWorkerFreshTest, WrapperWithNullWorkerSkipsFactoryDelete)
{
	R44FakeIndexFact fact;
	{
		IdxWorkerWrapper wrapper(nullptr, &fact);
		EXPECT_EQ(wrapper.self(), nullptr);
		EXPECT_EQ(fact._delCnt, 0);
	}
	EXPECT_EQ(fact._delCnt, 0);
}

// IndexWorkerMgr.cpp:210-215 —— fullCode 至少含一个点号时不会越界；多段/空段都原样拆前两段
TEST_F(R44IndexWorkerFreshTest, SubTicksHandlesMultiSegmentAndEmptySegment)
{
	WTSVariant* cfg = loadCfg("sub.json", R"JSON({"poolsize": 0, "indice": []})JSON");
	ASSERT_NE(cfg, nullptr);
	DataManager dm;	// writer 为空，getCurTick 走守卫
	IndexWorkerMgr mgr;
	ASSERT_TRUE(mgr.init(cfg, nullptr, nullptr, &dm));
	cfg->release();

	EXPECT_EQ(mgr.sub_ticks("A.B.C"), nullptr);
	EXPECT_EQ(mgr.sub_ticks(".B"), nullptr);
	EXPECT_EQ(mgr.sub_ticks("A."), nullptr);
}

// IndexWorkerMgr.cpp:218-221 —— get_tick 原样转发；DataManager::getCurTick 在 _writer==NULL 时
// 先返回，所以 NULL code/exchg 不会触发 strlen/strcmp
TEST_F(R44IndexWorkerFreshTest, GetTickWithNullCodeIsStoppedByWriterGuard)
{
	WTSVariant* cfg = loadCfg("get.json", R"JSON({"poolsize": 0, "indice": []})JSON");
	ASSERT_NE(cfg, nullptr);
	DataManager dm;
	IndexWorkerMgr mgr;
	ASSERT_TRUE(mgr.init(cfg, nullptr, nullptr, &dm));
	cfg->release();

	EXPECT_EQ(mgr.get_tick(nullptr, nullptr), nullptr);
}

/*
 * 待确认的输入合同：loadFactories 返回 bool，不存在路径返回 false；公开接口没有
 * 说明普通文件应抛异常还是返回 false。普通文件不应被报告为成功，因此暂保留正确
 * 的 false 期望但禁用，待确定异常/返回值合同后再启用。当前 directory_iterator
 * 路径可能抛 filesystem_error，不能把异常本身当作通过。
 * 缺陷登记：bugs/R44.md BUG-R44-005。
 */
TEST_F(R44IndexWorkerFreshTest, DISABLED_LoadFactoriesOnRegularFileReturnsFalse_ContractPending)
{
	fs::path file = _dir / "not_a_dir.txt";
	r44_write_file(file, "plain file");
	IndexWorkerMgr mgr;
	EXPECT_FALSE(mgr.loadFactories(file.string().c_str()));
}

/*
 * 触发条件（禁用原因）：IndexWorkerMgr::init（L135）直接 config->getUInt32，
 * config==NULL 时空指针解引用。缺陷登记：bugs/R44.md BUG-R44-006。
 */
TEST_F(R44IndexWorkerFreshTest, DISABLED_InitCrashesWithNullConfig)
{
	DataManager dm;
	IndexWorkerMgr mgr;
	mgr.init(nullptr, nullptr, nullptr, &dm);	// 崩在 L135
}

/*
 * 触发条件（禁用原因）：构造函数（IndexWorkerMgr.cpp:26-28）没有初始化
 * _hot_mgr/_bd_mgr/_data_mgr，push_tick（L205-208）在 init 之前调用就是对野指针解引用。
 * 同类问题还有 sub_ticks/get_tick。缺陷登记：bugs/R44.md BUG-R44-007。
 */
TEST_F(R44IndexWorkerFreshTest, DISABLED_FreshPushTickCrashesOnUninitializedDataManager)
{
	IndexWorkerMgr mgr;
	WTSTickData* tick = makeTick(nullptr);
	ASSERT_NE(tick, nullptr);
	mgr.push_tick(tick);	// _data_mgr 未初始化
	tick->release();
}

}  // namespace
