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
#include <algorithm>
#include <cstdlib>
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

	// 在独立子目录里复制一个 libWtIndexFactory.dylib，
	// 让 loadFactories 只扫描这一个模块，不碰 bin 目录里的其他插件。
	fs::path makePluginDir(const fs::path& plugin)
	{
		static std::atomic<int> seq { 0 };
		fs::path d = _dir / ("fact_" + std::to_string(seq++));
		fs::create_directories(d);
		fs::path stagedPlugin = d / "libWtIndexFactory.dylib";
		std::error_code ec;
		fs::copy_file(plugin, stagedPlugin, fs::copy_options::overwrite_existing, ec);
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
	// 工厂产出的 WtBlkIdxWorker 对空权重表直接返回；同步处理结束后临时引用归还。
	WTSTickData* tick = makeTick("600000", "SSE");
	ASSERT_NE(tick, nullptr);
	ASSERT_EQ(tick->retainCount(), 1u);
	mgr.handle_quote(tick);
	EXPECT_EQ(tick->retainCount(), 1u);
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
	// 同步分发结束后应归还本次临时 retain。
	EXPECT_EQ(tick->retainCount(), 1u);
	tick->release();
}

TEST_F(R68IndexWorkerFactoryTest, RepeatedSameModuleDoesNotRecreateFactoryOrInvalidateWorker)
{
	const char* fixture = std::getenv("WT_R68_COUNTING_PLUGIN");
	if (fixture == nullptr || fixture[0] == '\0')
	{
#ifdef WT_R68_COUNTING_PLUGIN_PATH
		fixture = WT_R68_COUNTING_PLUGIN_PATH;
#else
		FAIL() << "counting fixture unavailable: build its target or set WT_R68_COUNTING_PLUGIN";
#endif
	}
	std::error_code ec;
	ASSERT_TRUE(fs::is_regular_file(fs::symlink_status(fixture, ec)));
	ASSERT_FALSE(ec);
	const fs::path factDir = _dir / "counted";
	ASSERT_TRUE(fs::create_directories(factDir));
#ifdef __APPLE__
	const fs::path plugin = factDir / "libR68Counter.dylib";
#else
	const fs::path plugin = factDir / "libR68Counter.so";
#endif
	ASSERT_TRUE(fs::copy_file(fixture, plugin, fs::copy_options::none, ec));
	ASSERT_FALSE(ec);
	const std::string log = (_dir / "factory-events.txt").string();
	const char* oldEnv = std::getenv("WT_R68_FACTORY_EVENTS");
	const bool hadEnv = oldEnv != nullptr;
	const std::string previous = hadEnv ? oldEnv : "";
	struct EnvRestore
	{
		bool hadValue;
		std::string value;
		~EnvRestore() { if (hadValue) setenv("WT_R68_FACTORY_EVENTS", value.c_str(), 1); else unsetenv("WT_R68_FACTORY_EVENTS"); }
	} envRestore{hadEnv, previous};
	ASSERT_EQ(0, setenv("WT_R68_FACTORY_EVENTS", log.c_str(), 1));

	WTSVariant* cfg = loadCfg("counted.json", R"JSON({
		"poolsize":0,"indice":[{"active":true,"id":"counted","name":"R68Counter.Counter"}]
	})JSON");
	ASSERT_NE(cfg, nullptr);
	WTSBaseDataMgr bdMgr;
	DataManager dm;
	IndexWorkerMgr mgr;
	ASSERT_TRUE(mgr.loadFactories(factDir.string().c_str()));
	ASSERT_TRUE(mgr.init(cfg, nullptr, &bdMgr, &dm));
	cfg->release();
	EXPECT_EQ(nullptr, mgr.sub_ticks("SHFE.rb2410"));
	WTSTickData* tick = makeTick("rb2410", "SHFE");
	ASSERT_NE(tick, nullptr);
	mgr.handle_quote(tick);
	ASSERT_TRUE(mgr.loadFactories(factDir.string().c_str()));
	mgr.handle_quote(tick);
	EXPECT_EQ(1u, tick->retainCount());
	tick->release();

	std::ifstream input(log);
	std::vector<std::string> events;
	for (std::string line; std::getline(input, line);)
		events.push_back(line);
	EXPECT_EQ(1, std::count(events.begin(), events.end(), "module-loaded"));
	EXPECT_EQ(1, std::count(events.begin(), events.end(), "factory-created"));
	EXPECT_EQ(1, std::count(events.begin(), events.end(), "worker-created"));
	EXPECT_EQ(1, std::count(events.begin(), events.end(), "worker-initialized"));
	EXPECT_EQ(2, std::count(events.begin(), events.end(), "worker-quote"));
	EXPECT_EQ(0, std::count(events.begin(), events.end(), "module-unloaded"));
}

TEST_F(R68IndexWorkerFactoryTest, DifferentModulesWithSameFactoryNameSealWithoutReplacingOwner)
{
#if !defined(WT_R68_COUNTING_PLUGIN_PATH) || !defined(WT_R68_CONFLICT_PLUGIN_PATH)
	FAIL() << "Both counting fixture modules must be supplied by the dedicated CMake fixture";
#else
	const fs::path first = WT_R68_COUNTING_PLUGIN_PATH;
	const fs::path second = WT_R68_CONFLICT_PLUGIN_PATH;
	std::error_code ec;
	ASSERT_TRUE(fs::is_regular_file(first, ec));
	ASSERT_FALSE(ec);
	ASSERT_TRUE(fs::is_regular_file(second, ec));
	ASSERT_FALSE(ec);
	const fs::path firstDir = _dir / "first";
	const fs::path secondDir = _dir / "second";
	const fs::path thirdDir = _dir / "third";
	ASSERT_TRUE(fs::create_directories(firstDir));
	ASSERT_TRUE(fs::create_directories(secondDir));
	ASSERT_TRUE(fs::create_directories(thirdDir));
	ASSERT_TRUE(fs::copy_file(first, firstDir / first.filename(), ec));
	ASSERT_FALSE(ec);
	ASSERT_TRUE(fs::copy_file(second, secondDir / second.filename(), ec));
	ASSERT_FALSE(ec);
	ASSERT_TRUE(fs::copy_file(first, thirdDir / first.filename(), ec));
	ASSERT_FALSE(ec);
	const std::string log = (_dir / "duplicate-events.txt").string();
	const char* oldEnv = std::getenv("WT_R68_FACTORY_EVENTS");
	const bool hadEnv = oldEnv != nullptr;
	const std::string previous = hadEnv ? oldEnv : "";
	struct EnvRestore
	{
		bool hadValue;
		std::string value;
		~EnvRestore() { if (hadValue) setenv("WT_R68_FACTORY_EVENTS", value.c_str(), 1); else unsetenv("WT_R68_FACTORY_EVENTS"); }
	} envRestore{hadEnv, previous};
	ASSERT_EQ(0, setenv("WT_R68_FACTORY_EVENTS", log.c_str(), 1));

	WTSVariant* firstCfg = loadCfg("first.json", R"JSON({
		"poolsize":0,"indice":[{"active":true,"id":"before","name":"R68Counter.Counter"}]
	})JSON");
	WTSVariant* secondCfg = loadCfg("second.json", R"JSON({
		"poolsize":0,"indice":[{"active":true,"id":"after","name":"R68Counter.Counter"}]
	})JSON");
	ASSERT_NE(nullptr, firstCfg);
	ASSERT_NE(nullptr, secondCfg);
	WTSBaseDataMgr bdMgr;
	DataManager dm;
	IndexWorkerMgr mgr;
	ASSERT_TRUE(mgr.loadFactories(firstDir.string().c_str()));
	ASSERT_TRUE(mgr.init(firstCfg, nullptr, &bdMgr, &dm));
	firstCfg->release();
	EXPECT_EQ(nullptr, mgr.sub_ticks("SHFE.rb2410"));
	WTSTickData* tick = makeTick("rb2410", "SHFE");
	ASSERT_NE(nullptr, tick);
	mgr.handle_quote(tick);
	EXPECT_FALSE(mgr.loadFactories(secondDir.string().c_str()));
	EXPECT_FALSE(mgr.loadFactories(secondDir.string().c_str()));
	EXPECT_FALSE(mgr.loadFactories(thirdDir.string().c_str()));
	ASSERT_TRUE(mgr.init(secondCfg, nullptr, &bdMgr, &dm));
	secondCfg->release();
	mgr.handle_quote(tick);
	EXPECT_EQ(1u, tick->retainCount());
	tick->release();

	std::ifstream input(log);
	std::vector<std::string> events;
	for (std::string line; std::getline(input, line);)
		events.push_back(line);
	EXPECT_EQ(2, std::count(events.begin(), events.end(), "module-loaded"));
	EXPECT_EQ(1, std::count(events.begin(), events.end(), "factory-from-first"));
	EXPECT_EQ(1, std::count(events.begin(), events.end(), "factory-from-second"));
	EXPECT_EQ(2, std::count(events.begin(), events.end(), "worker-from-first"));
	EXPECT_EQ(0, std::count(events.begin(), events.end(), "worker-from-second"));
	EXPECT_EQ(3, std::count(events.begin(), events.end(), "worker-quote"));
	EXPECT_EQ(0, std::count(events.begin(), events.end(), "module-unloaded"));
#endif
}

TEST_F(R68IndexWorkerFactoryTest, BadFactoryNamesAndThrowingCreatorAreQuarantined)
{
#if !defined(WT_R68_COUNTING_PLUGIN_PATH) || !defined(WT_R68_CONFLICT_PLUGIN_PATH)
	FAIL() << "Both counting fixture modules must be supplied by the dedicated CMake fixture";
#else
	const char* oldMode = std::getenv("WT_R68_FACTORY_MODE");
	const bool hadMode = oldMode != nullptr;
	const std::string previousMode = hadMode ? oldMode : "";
	const char* oldLog = std::getenv("WT_R68_FACTORY_EVENTS");
	const bool hadLog = oldLog != nullptr;
	const std::string previousLog = hadLog ? oldLog : "";
	struct EnvRestore
	{
		bool hadMode, hadLog;
		std::string mode, log;
		~EnvRestore()
		{
			if (hadMode) setenv("WT_R68_FACTORY_MODE", mode.c_str(), 1); else unsetenv("WT_R68_FACTORY_MODE");
			if (hadLog) setenv("WT_R68_FACTORY_EVENTS", log.c_str(), 1); else unsetenv("WT_R68_FACTORY_EVENTS");
		}
	} restore{hadMode, hadLog, previousMode, previousLog};

	const fs::path first = WT_R68_COUNTING_PLUGIN_PATH;
	const fs::path second = WT_R68_CONFLICT_PLUGIN_PATH;
	for (const char* mode : {"null-name", "empty-name", "throw-name", "throw-create"})
	{
		SCOPED_TRACE(mode);
		const fs::path root = _dir / mode;
		const fs::path firstDir = root / "first", badDir = root / "bad", nextDir = root / "next";
		ASSERT_TRUE(fs::create_directories(firstDir));
		ASSERT_TRUE(fs::create_directories(badDir));
		ASSERT_TRUE(fs::create_directories(nextDir));
		std::error_code ec;
		ASSERT_TRUE(fs::copy_file(first, firstDir / first.filename(), ec)); ASSERT_FALSE(ec);
		ASSERT_TRUE(fs::copy_file(second, badDir / second.filename(), ec)); ASSERT_FALSE(ec);
		ASSERT_TRUE(fs::copy_file(first, nextDir / first.filename(), ec)); ASSERT_FALSE(ec);
		const std::string log = (root / "events.txt").string();
		ASSERT_EQ(0, setenv("WT_R68_FACTORY_EVENTS", log.c_str(), 1));
		ASSERT_EQ(0, unsetenv("WT_R68_FACTORY_MODE"));
		IndexWorkerMgr mgr;
		ASSERT_TRUE(mgr.loadFactories(firstDir.string().c_str()));
		ASSERT_EQ(0, setenv("WT_R68_FACTORY_MODE", mode, 1));
		EXPECT_FALSE(mgr.loadFactories(badDir.string().c_str()));
		ASSERT_EQ(0, unsetenv("WT_R68_FACTORY_MODE"));
		EXPECT_FALSE(mgr.loadFactories(nextDir.string().c_str()));

		const std::string cfgName = std::string(mode) + ".json";
		WTSVariant* cfg = loadCfg(cfgName.c_str(), R"JSON({
			"poolsize":0,"indice":[{"active":true,"id":"survivor","name":"R68Counter.Counter"}]
		})JSON");
		ASSERT_NE(nullptr, cfg);
		WTSBaseDataMgr bdMgr;
		DataManager dm;
		EXPECT_TRUE(mgr.init(cfg, nullptr, &bdMgr, &dm));
		cfg->release();
		std::ifstream input(log);
		std::vector<std::string> events;
		for (std::string line; std::getline(input, line);) events.push_back(line);
		EXPECT_EQ(2, std::count(events.begin(), events.end(), "module-loaded"));
		EXPECT_EQ(1, std::count(events.begin(), events.end(), "factory-from-first"));
		EXPECT_EQ(1, std::count(events.begin(), events.end(), "factory-from-second"));
		EXPECT_EQ(1, std::count(events.begin(), events.end(), "worker-from-first"));
		EXPECT_EQ(0, std::count(events.begin(), events.end(), "module-unloaded"));
	}
#endif
}

TEST_F(R68IndexWorkerFactoryTest, CreatorReentryCannotStartAnotherScan)
{
#if !defined(WT_R68_COUNTING_PLUGIN_PATH)
	FAIL() << "Counting fixture module must be supplied by the dedicated CMake fixture";
#else
	const fs::path fixture = WT_R68_COUNTING_PLUGIN_PATH;
	const fs::path dir = _dir / "reentry";
	ASSERT_TRUE(fs::create_directories(dir));
	const fs::path plugin = dir / fixture.filename();
	std::error_code ec;
	ASSERT_TRUE(fs::copy_file(fixture, plugin, ec)); ASSERT_FALSE(ec);
	DllHandle handle = DLLHelper::load_library(plugin.string().c_str());
	ASSERT_NE(nullptr, handle);
	struct HandleGuard { DllHandle value; ~HandleGuard() { DLLHelper::free_library(value); } } handleGuard{handle};
	using SetHook = void (*)(void (*)(void*), void*);
	auto setHook = reinterpret_cast<SetHook>(DLLHelper::get_symbol(handle, "r68_set_reentry"));
	ASSERT_NE(nullptr, setHook);
	const char* old = std::getenv("WT_R68_FACTORY_MODE");
	const bool hadMode = old != nullptr;
	const std::string previous = hadMode ? old : "";
	struct EnvRestore { bool had; std::string value; ~EnvRestore() {
		if (had) setenv("WT_R68_FACTORY_MODE", value.c_str(), 1); else unsetenv("WT_R68_FACTORY_MODE");
	} } restore{hadMode, previous};
	IndexWorkerMgr mgr;
	struct Context { IndexWorkerMgr* manager; std::string path; bool called = false; bool result = true; } ctx{&mgr, dir.string()};
	setHook([](void* p) {
		auto* c = static_cast<Context*>(p);
		c->called = true;
		c->result = c->manager->loadFactories(c->path.c_str());
	}, &ctx);
	ASSERT_EQ(0, setenv("WT_R68_FACTORY_MODE", "reenter", 1));
	EXPECT_TRUE(mgr.loadFactories(dir.string().c_str()));
	EXPECT_TRUE(ctx.called);
	EXPECT_FALSE(ctx.result);
	setHook(nullptr, nullptr);
#endif
}

}	// namespace

// BUG-R68-003 (iteration 03): release() drains scheduled pool tasks before it
// deletes workers through their factory, then stops admitting quotes. The
// factory and its module stay loaded: the exported delete_index_factory and
// FuncDeleteIndexFact disagree on the parameter type, so no remover is called.
TEST_F(R68IndexWorkerFactoryTest, ReleaseDrainsPoolThenDeletesWorkersAndPinsFactoryModule)
{
#ifndef WT_R68_COUNTING_PLUGIN_PATH
	FAIL() << "counting fixture unavailable: build its target";
#else
	const char* fixture = WT_R68_COUNTING_PLUGIN_PATH;
	std::error_code ec;
	const fs::path factDir = _dir / "release_order";
	ASSERT_TRUE(fs::create_directories(factDir));
#ifdef __APPLE__
	const fs::path plugin = factDir / "libR68Counter.dylib";
#else
	const fs::path plugin = factDir / "libR68Counter.so";
#endif
	ASSERT_TRUE(fs::copy_file(fixture, plugin, fs::copy_options::none, ec));
	ASSERT_FALSE(ec);
	const std::string log = (_dir / "release-events.txt").string();
	const char* oldEnv = std::getenv("WT_R68_FACTORY_EVENTS");
	const bool hadEnv = oldEnv != nullptr;
	const std::string previous = hadEnv ? oldEnv : "";
	struct EnvRestore
	{
		bool hadValue;
		std::string value;
		~EnvRestore() { if (hadValue) setenv("WT_R68_FACTORY_EVENTS", value.c_str(), 1); else unsetenv("WT_R68_FACTORY_EVENTS"); }
	} envRestore{hadEnv, previous};
	ASSERT_EQ(0, setenv("WT_R68_FACTORY_EVENTS", log.c_str(), 1));

	WTSVariant* cfg = loadCfg("release_order.json", R"JSON({
		"poolsize":1,"indice":[{"active":true,"id":"released","name":"R68Counter.Counter"}]
	})JSON");
	ASSERT_NE(cfg, nullptr);
	WTSBaseDataMgr bdMgr;
	DataManager dm;
	IndexWorkerMgr mgr;
	ASSERT_TRUE(mgr.loadFactories(factDir.string().c_str()));
	ASSERT_TRUE(mgr.init(cfg, nullptr, &bdMgr, &dm));
	cfg->release();
	EXPECT_EQ(nullptr, mgr.sub_ticks("SHFE.rb2410"));
	WTSTickData* tick = makeTick("rb2410", "SHFE");
	ASSERT_NE(tick, nullptr);
	mgr.handle_quote(tick);		// scheduled on the pool
	mgr.release();
	EXPECT_TRUE(mgr.isReleased());
	mgr.handle_quote(tick);		// no longer admitted
	mgr.release();				// idempotent
	EXPECT_EQ(1u, tick->retainCount());
	tick->release();

	std::ifstream input(log);
	std::vector<std::string> events;
	for (std::string line; std::getline(input, line);)
		events.push_back(line);
	const auto quote = std::find(events.begin(), events.end(), "worker-quote");
	const auto deleted = std::find(events.begin(), events.end(), "worker-deleted");
	ASSERT_NE(quote, events.end());
	ASSERT_NE(deleted, events.end());
	EXPECT_LT(quote - events.begin(), deleted - events.begin());	// drained before delete
	EXPECT_EQ(1, std::count(events.begin(), events.end(), "worker-quote"));
	EXPECT_EQ(1, std::count(events.begin(), events.end(), "worker-deleted"));
	EXPECT_EQ(0, std::count(events.begin(), events.end(), "module-unloaded"));
#endif
}
