/*!
 * \file r68_idxworker_factory_test.cpp
 * \brief WtDtCore/IndexWorkerMgr 的工厂装载成功段 + 同步查询补测（迭代01 第十三波 R68）
 *
 * 与既有用例的分工（均只读，不修改）：
 *  - q05_idxworker_mgr_test.cpp / r18_idxworker_mgr_test.cpp / r44_idxworker_fresh_test.cpp
 *    已覆盖目录缺失/空目录/非 dylib/dlopen 失败/creator 缺失、indice 缺失/非数组、
 *    active=false、名称无点号、工厂缺失、poolsize=0 的同步分支与上下文委托；
 *  - 本文件补 R18 清单明确留白的「真实工厂插件」段：
 *      IndexWorkerMgr.cpp:73-91（工厂登记）、:116-126（createWorker 成功/worker 为 NULL）、
 *      :157-165（set_factory + init）、:182-202（_pool 为空时的同步分发）。
 *
 * 插件只用覆盖率构建产出的第一方 libWtIndexFactory.dylib（按测试二进制相对位置查找），
 * 通过公开的 loadFactories() 加载；找不到时 GTEST_SKIP，不做任何下载/编译。
 * 工厂与 WtBlkIdxWorker 的 init 在 trigger="time"、timeout=0 下不建线程、不连网、不写盘。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <filesystem>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/IndexWorkerMgr.h>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WTSTools/WTSBaseDataMgr.h>

#include "r68_test_util.h"

namespace fs = std::filesystem;

namespace {

class R68IndexWorkerFactoryTest : public ::testing::Test
{
protected:
	void SetUp() override { _dir = r68::make_tmp_dir("idx"); }

	void TearDown() override
	{
		// setContractInfo 只存裸指针（不 retain），必须等 tick 全部释放后再回收合约对象
		for (WTSContractInfo* ci : _cis)
			ci->release();
		_cis.clear();

		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	// 在独立子目录里放一个 libWtIndexFactory.dylib 的符号链接（失败时退化为复制），
	// 让 loadFactories 只扫描这一个模块，不碰 bin 目录里的其他插件。
	fs::path makePluginDir(const fs::path& plugin)
	{
		static std::atomic<int> seq { 0 };
		fs::path d = _dir / ("fact_" + std::to_string(seq++));
		fs::create_directories(d);
		fs::path link = d / "libWtIndexFactory.dylib";
		std::error_code ec;
		fs::create_symlink(plugin, link, ec);
		if (ec)
		{
			ec.clear();
			fs::copy_file(plugin, link, fs::copy_options::overwrite_existing, ec);
		}
		return ec ? fs::path() : d;
	}

	WTSVariant* loadCfg(const char* name, const std::string& body)
	{
		fs::path p = _dir / name;
		r68::write_file(p, body);
		return WTSCfgLoader::load_from_file(p.string().c_str());
	}

	WTSTickData* makeTick(const char* code, const char* exchg)
	{
		WTSTickStruct s;
		memset(&s, 0, sizeof(s));
		strcpy(s.code, code);
		strcpy(s.exchg, exchg);
		s.action_date = 20240102;
		s.trading_date = 20240102;
		s.price = 10.0;
		WTSTickData* tick = WTSTickData::create(s);
		if (tick != nullptr)
		{
			WTSContractInfo* ci = WTSContractInfo::create(code, "测试合约", exchg, "STK");
			if (ci != nullptr)
				_cis.push_back(ci);	// 生命周期由 fixture 持有，TearDown 统一释放
			tick->setContractInfo(ci);
		}
		return tick;
	}

	fs::path _dir;
	std::vector<WTSContractInfo*> _cis;
};

// IndexWorkerMgr.cpp:73-91 —— dlopen 成功且导出 create_index_factory，
// 工厂被登记进 _factories；随后 init 用未知单元名走 :116-122 的 create_worker==NULL 分支。
TEST_F(R68IndexWorkerFactoryTest, LoadFactoriesRegistersPluginAndUnknownWorkerSkips)
{
	const fs::path plugin = r68::find_index_factory_plugin();
	if (plugin.empty())
		GTEST_SKIP() << "没有覆盖率构建产出的 libWtIndexFactory.dylib";

	const fs::path factDir = makePluginDir(plugin);
	ASSERT_FALSE(factDir.empty());

	WTSVariant* cfg = loadCfg("unknown_worker.json", R"JSON({
		"poolsize": 0,
		"indice": [
			{"active": true, "id": "u1", "name": "WtIndexFactory.NoSuchWorker"},
			{"active": true, "id": "u2", "name": "WtIndexFactory."},
			{"active": true, "id": "u3", "name": "NoSuchFactory.WtBuiltinIndex"}
		]
	})JSON");
	ASSERT_NE(cfg, nullptr);

	WTSBaseDataMgr bdMgr;
	DataManager dm;
	IndexWorkerMgr mgr;
	ASSERT_TRUE(mgr.loadFactories(factDir.string().c_str()));
	// 三个条目都创建失败，但 init 仍整体返回 true（逐项 continue）
	EXPECT_TRUE(mgr.init(cfg, nullptr, &bdMgr, &dm));
	EXPECT_EQ(mgr.get_bd_mgr(), static_cast<IBaseDataMgr*>(&bdMgr));
	cfg->release();

	// 没有 worker 时同步分发不得崩
	WTSTickData* tick = makeTick("600000", "SSE");
	ASSERT_NE(tick, nullptr);
	mgr.handle_quote(tick);
	tick->release();
}

// IndexWorkerMgr.cpp:116-126、157-165 —— 工厂命中且 create_worker 返回对象：
// IdxWorkerWrapper 入 _workers、set_factory 注入上下文、worker->init 成功。
TEST_F(R68IndexWorkerFactoryTest, InitCreatesBuiltinWorkerAndInitializesIt)
{
	const fs::path plugin = r68::find_index_factory_plugin();
	if (plugin.empty())
		GTEST_SKIP() << "没有覆盖率构建产出的 libWtIndexFactory.dylib";

	const fs::path factDir = makePluginDir(plugin);
	ASSERT_FALSE(factDir.empty());

	WTSVariant* cfg = loadCfg("builtin.json", R"JSON({
		"poolsize": 0,
		"indice": [{
			"active": true,
			"id": "r68blk",
			"name": "WtIndexFactory.WtBuiltinIndex",
			"exchg": "SSE",
			"code": "600000",
			"trigger": "time",
			"timeout": 0,
			"stand_scale": 1.0,
			"weight_alg": 0
		}]
	})JSON");
	ASSERT_NE(cfg, nullptr);

	WTSBaseDataMgr bdMgr;
	DataManager dm;	// 未 init：getCurTick/writeTick 都走 _writer==NULL 守卫
	IndexWorkerMgr mgr;
	ASSERT_TRUE(mgr.loadFactories(factDir.string().c_str()));
	ASSERT_TRUE(mgr.init(cfg, nullptr, &bdMgr, &dm));
	cfg->release();

	// sub_ticks 只登记订阅并向 DataManager 取快照（writer 为空 -> NULL）
	EXPECT_EQ(mgr.sub_ticks("SSE.600000"), nullptr);

	// IndexWorkerMgr.cpp:180-202：已订阅 -> retain；_pool 为空 -> 同步遍历 _workers，
	// 工厂产出的 WtBlkIdxWorker 对空权重表直接返回。同步分支不配对 release（BUG-R18-007）。
	WTSTickData* tick = makeTick("600000", "SSE");
	ASSERT_NE(tick, nullptr);
	ASSERT_EQ(tick->retainCount(), 1u);
	mgr.handle_quote(tick);
	EXPECT_EQ(tick->retainCount(), 2u);
	tick->release();
	tick->release();
}

// IndexWorkerMgr.cpp:73-91 的重复装载 —— 同一插件再次 loadFactories 会覆写
// _factories["WtIndexFactory"]；旧模块句柄不释放（已登记为资源泄漏留白），但新条目可用。
TEST_F(R68IndexWorkerFactoryTest, RepeatedLoadKeepsFactoryUsable)
{
	const fs::path plugin = r68::find_index_factory_plugin();
	if (plugin.empty())
		GTEST_SKIP() << "没有覆盖率构建产出的 libWtIndexFactory.dylib";

	const fs::path factDir = makePluginDir(plugin);
	ASSERT_FALSE(factDir.empty());

	WTSVariant* cfg = loadCfg("multi_dot.json", R"JSON({
		"poolsize": 0,
		"indice": [{
			"active": true,
			"id": "r68blk2",
			"name": "WtIndexFactory.WtBuiltinIndex.extra",
			"exchg": "SHFE",
			"code": "rb2410",
			"trigger": "time",
			"timeout": 0
		}]
	})JSON");
	ASSERT_NE(cfg, nullptr);

	WTSBaseDataMgr bdMgr;
	DataManager dm;
	IndexWorkerMgr mgr;
	ASSERT_TRUE(mgr.loadFactories(factDir.string().c_str()));
	ASSERT_TRUE(mgr.loadFactories(factDir.string().c_str()));
	// split("a.b.c") 取前两段（工厂名 + 单元名），第三段被忽略
	EXPECT_TRUE(mgr.init(cfg, nullptr, &bdMgr, &dm));
	cfg->release();

	EXPECT_EQ(mgr.sub_ticks("SHFE.rb2410"), nullptr);
	WTSTickData* tick = makeTick("rb2410", "SHFE");
	ASSERT_NE(tick, nullptr);
	mgr.handle_quote(tick);
	// 同步分支的 retain 泄漏（BUG-R18-007）：自有 1 + 该分支 1
	EXPECT_EQ(tick->retainCount(), 2u);
	tick->release();
	tick->release();
}

}	// namespace
