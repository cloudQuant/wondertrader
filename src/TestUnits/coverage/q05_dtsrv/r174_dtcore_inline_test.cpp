/*!
 * \file r174_dtcore_inline_test.cpp
 * \brief DataManager.h / IndexWorkerMgr.h 内联守卫的 out-of-line 覆盖
 *        （迭代01 第二十五波 R174）
 *
 * 背景：add_caster 与 get_hot_mgr/get_bd_mgr 在类内定义，既有用例的调用点被
 * 前端内联，profdata 的独立函数记录仍为 0（DataManager.h 5/5、
 * IndexWorkerMgr.h 10/10 未覆盖）。本文件用成员函数指针发起间接调用：
 *   DataManager.h:79-84      add_caster 的 NULL 守卫与 emplace_back
 *   IndexWorkerMgr.h:65-66   get_hot_mgr/get_bd_mgr 回传 init 注入的指针
 *   IndexWorkerMgr.h:30-38   IdxWorkerWrapper 构造/self/析构的 NULL worker 守卫
 * 不建 socket/shm/线程：IndexWorkerMgr 的 poolsize 固定 0，且没有 indice 数组。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/IDataCaster.h>
#include <WtDtCore/IndexWorkerMgr.h>
#include <Includes/WTSDataDef.hpp>
#include <WTSTools/WTSBaseDataMgr.h>

#include "r174_test_util.h"

namespace {

// 只记录 tick 扇出；其余三个重载走 IDataCaster 的默认空实现。
class R174CountingCaster : public IDataCaster
{
public:
	void broadcast(WTSTickData* curTick) override
	{
		_tickCount++;
		_lastTick = curTick;
	}

	int _tickCount { 0 };
	WTSTickData* _lastTick { nullptr };
};

class R174DtCoreInlineTest : public ::testing::Test
{
protected:
	void SetUp() override { _dir = r174::make_tmp_dir("inline"); }

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

	std::filesystem::path _dir;
};

/*
 * DataManager.h:79-84
 * 成员指针调用 add_caster：NULL 走早退，真实 caster 走 emplace_back。
 * broadcastTick 证明第二个 caster 确实进入 _casters 并被扇出。
 */
TEST_F(R174DtCoreInlineTest, AddCasterRejectsNullAndStoresRealCaster)
{
	DataManager dm;
	void (DataManager::*addCaster)(IDataCaster*) = &DataManager::add_caster;

	(dm.*addCaster)(nullptr);

	R174CountingCaster caster;
	(dm.*addCaster)(&caster);

	WTSTickData* tick = WTSTickData::create("600000");
	ASSERT_NE(tick, nullptr);
	dm.broadcastTick(tick);
	EXPECT_EQ(caster._tickCount, 1);
	EXPECT_EQ(caster._lastTick, tick);
	tick->release();
}

/*
 * IndexWorkerMgr.h:65-66
 * init 在检查 indice 数组之前先保存 hotMgr/bdMgr；配置没有 indice 时返回 false，
 * 但两个 accessor 已可回传注入值。成员指针调用避免内联。
 */
TEST_F(R174DtCoreInlineTest, AccessorsReturnInjectedManagersAfterInit)
{
	WTSVariant* cfg = r174::load_cfg(_dir, "idx.json", R"JSON({"poolsize": 0})JSON");
	ASSERT_NE(cfg, nullptr);

	WTSBaseDataMgr bdMgr;
	IndexWorkerMgr mgr;
	EXPECT_FALSE(mgr.init(cfg, nullptr, &bdMgr, nullptr));
	cfg->release();

	IHotMgr* (IndexWorkerMgr::*getHot)() = &IndexWorkerMgr::get_hot_mgr;
	IBaseDataMgr* (IndexWorkerMgr::*getBd)() = &IndexWorkerMgr::get_bd_mgr;
	EXPECT_EQ((mgr.*getHot)(), nullptr);
	EXPECT_EQ((mgr.*getBd)(), static_cast<IBaseDataMgr*>(&bdMgr));
}

/*
 * IndexWorkerMgr.h:30-38
 * 构造一个没有 worker 的 wrapper：self() 返回 NULL，析构时不调用 delete_worker。
 * self 用成员指针调用，构造/析构仍是直接调用，但同样执行这两段代码。
 */
TEST_F(R174DtCoreInlineTest, WrapperSelfAndDestructorHandleNullWorker)
{
	IdxWorkerWrapper wrapper(nullptr, nullptr);
	IIndexWorker* (IdxWorkerWrapper::*self)() = &IdxWorkerWrapper::self;
	EXPECT_EQ((wrapper.*self)(), nullptr);
}

}	// namespace
