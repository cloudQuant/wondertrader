/*!
 * \file r102_idxworker_pool_test.cpp
 * \brief WtDtCore/IndexWorkerMgr 线程池分支与异步分发（迭代01 第十八波 R102）
 *
 * 与既有用例的分工（只读，不修改）：
 *  - q05/r18/r44/r68 已覆盖目录缺失/空目录/非 dylib/dlopen 失败/creator 缺失、
 *    indice 缺失/非数组、active=false、名称无点号、工厂缺失、poolsize=0 的同步分发、
 *    真实 WtIndexFactory 的装载成功段；
 *  - 本文件只补 IndexWorkerMgr.cpp:135-139（poolsize>0 建 boost::threadpool）
 *    与 :182-194（handle_quote 的 _pool->schedule 异步分支）。
 *
 * 线程池在本用例内创建并在析构时 join；没有 sleep/挂钟断言，任务完成用 tick 引用计数
 * 轮询等待（有上限），不依赖调度顺序。无 worker 时 lambda 只做一次 release。
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/IndexWorkerMgr.h>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSTools/WTSBaseDataMgr.h>
#include <WTSUtils/WTSCfgLoader.h>

#include "r102_dtcore_util.h"

namespace fs = std::filesystem;

namespace {

class R102IndexWorkerPoolTest : public ::testing::Test
{
protected:
	void SetUp() override { _dir = r102dt::make_tmp_dir("idx"); }

	void TearDown() override
	{
		if (_ct != nullptr)
		{
			_ct->release();
			_ct = nullptr;
		}
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	WTSVariant* load(const char* name, const std::string& body)
	{
		r102dt::write_file(_dir / name, body);
		return WTSCfgLoader::load_from_file((_dir / name).string().c_str());
	}

	WTSTickData* makeTick()
	{
		WTSTickStruct s;
		memset(&s, 0, sizeof(s));
		strncpy(s.code, "600000", sizeof(s.code) - 1);
		strncpy(s.exchg, "SSE", sizeof(s.exchg) - 1);
		s.action_date = 20240102;
		s.trading_date = 20240102;
		s.action_time = 93000000;
		s.price = 12.5;

		WTSTickData* tick = WTSTickData::create(s);
		if (tick != nullptr)
		{
			_ct = WTSContractInfo::create("600000", "SPDB", "SSE", "STK");
			tick->setContractInfo(_ct);
		}
		return tick;
	}

	fs::path			_dir;
	WTSContractInfo*	_ct = nullptr;
};

TEST_F(R102IndexWorkerPoolTest, PoolsizeOneBuildsPoolAndAsyncDispatchReleasesTick)
{
	WTSVariant* cfg = load("pool1.json", R"({"poolsize":1,"indice":[]})");
	ASSERT_NE(cfg, nullptr);

	WTSBaseDataMgr bdMgr;
	DataManager dataMgr;	// 未 init：getCurTick / writeTick 都走 _writer==NULL 守卫
	IndexWorkerMgr mgr;

	// IndexWorkerMgr.cpp:135-139：poolsize>0 -> new boost::threadpool::pool(1)
	ASSERT_TRUE(mgr.init(cfg, nullptr, &bdMgr, &dataMgr));
	cfg->release();

	// 登记订阅（无 writer -> 快照为 NULL）；fullCode 为 "SSE.600000"
	EXPECT_EQ(mgr.sub_ticks("SSE.600000"), nullptr);

	WTSTickData* tick = makeTick();
	ASSERT_NE(tick, nullptr);
	ASSERT_EQ(tick->retainCount(), 1u);

	// :175-194：命中订阅 -> retain；_pool 非空 -> schedule(lambda)
	// lambda 在 worker 线程里遍历空 _workers 后 release，引用数回到 1
	mgr.handle_quote(tick);
	EXPECT_EQ(tick->retainCount(), 2u);

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (tick->retainCount() > 1u && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));

	EXPECT_EQ(tick->retainCount(), 1u);
	tick->release();
}

}	// namespace
