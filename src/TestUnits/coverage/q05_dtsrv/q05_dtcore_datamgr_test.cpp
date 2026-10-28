/*!
 * \file q05_dtcore_datamgr_test.cpp
 * \brief WtDtCore/DataManager 覆盖用例（迭代01 第五波 q05_dtsrv）
 *
 * 覆盖目标：
 *  - init 的模块加载失败路径（load_library 失败 -> return false，
 *    但 _bd_mgr/_state_mon 已先行赋值，可驱动 IDataWriterSink 委托方法）
 *  - _writer==NULL 时所有写入/查询/转储入口的守卫分支
 *  - add_caster 的 NULL 过滤 + broadcastTick/OrdQue/OrdDtl/Trans 广播扇出
 *  - canSessionReceive 的全天候（_state_mon==NULL 恒 true）分支与状态机分支
 *  - getBDMgr / getSessionComms / getTradingDate / outputLog 委托
 *  不加载任何插件 DLL（module 指向不存在的路径），不触碰网络/shm。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/IDataCaster.h>
#include <WtDtCore/StateMonitor.h>
#include <WtDtCore/WtHelper.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WTSTools/WTSBaseDataMgr.h>

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

// 记录四种广播各自被调了几次的替身；只覆写 tick，其余走 IDataCaster 默认空实现
class CountingCaster : public IDataCaster
{
public:
	virtual void broadcast(WTSTickData* curTick) override
	{
		_tickCnt++;
		_lastTick = curTick;
	}

	int _tickCnt { 0 };
	int _ordQueCnt { 0 };
	WTSTickData* _lastTick { nullptr };
};

class Q05DataManagerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = make_tmp_dir("q05_dm");
		// 让 get_module_dir() 落在一个必然没有 WtDataStorage 插件的目录，
		// 从而稳定走 load_library 失败分支
		WtHelper::set_module_dir((_dir / "bin/").string().c_str());

		write_file(_dir / "sessions.json",
			R"JSON({
				"SSE": {"name": "上交所", "offset": 0,
					"auction": {"from": 915, "to": 925},
					"sections": [{"from": 930, "to": 1500}]}
			})JSON");
		ASSERT_TRUE(_bdMgr.loadSessions((_dir / "sessions.json").string().c_str()));

		write_file(_dir / "state.json",
			R"JSON({"SSE": {"inittime": 900, "closetime": 1510, "proctime": 1530}})JSON");
		ASSERT_TRUE(_mon.initialize((_dir / "state.json").string().c_str(), &_bdMgr, nullptr));

		write_file(_dir / "writer.json",
			R"JSON({"module": "q05_no_such_writer_module"})JSON");
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	fs::path _dir;
	WTSBaseDataMgr _bdMgr;
	StateMonitor _mon;
};

TEST_F(Q05DataManagerTest, InitFailsOnMissingModuleButKeepsSinkTargets)
{
	WTSVariant* params = WTSCfgLoader::load_from_file((_dir / "writer.json").string().c_str());
	ASSERT_NE(params, nullptr);

	DataManager dm;
	// libWriter 加载失败 -> return false；_bd_mgr/_state_mon 已在函数头部赋值
	ASSERT_FALSE(dm.init(params, &_bdMgr, &_mon));

	// getBDMgr 返回的是 WTSBaseDataMgr 向上转的 IBaseDataMgr*
	EXPECT_EQ(dm.getBDMgr(), static_cast<IBaseDataMgr*>(&_bdMgr));

	// 状态机存在且 SSE 处于 SS_ORIGINAL -> 不可收数
	EXPECT_FALSE(dm.canSessionReceive("SSE"));
	EXPECT_FALSE(dm.canSessionReceive("UNKNOWN"));

	// 会话存在但从未 loadContracts，_session_to_pids 为空 -> NULL
	EXPECT_EQ(dm.getSessionComms("SSE"), nullptr);

	// 委托性质：不比较具体值（墙钟相关），只断言与 bd_mgr 返回值一致
	EXPECT_EQ(dm.getTradingDate("a60"), _bdMgr.getTradingDate("a60"));

	dm.outputLog(LL_INFO, "q05 delegate outputLog");
	dm.release();  // _writer==NULL，release 应为空操作且不崩

	params->release();
}

TEST_F(Q05DataManagerTest, NullStateMonitorMeansAlldayMode)
{
	WTSVariant* params = WTSCfgLoader::load_from_file((_dir / "writer.json").string().c_str());
	ASSERT_NE(params, nullptr);

	DataManager dm;
	// stMonitor==NULL：_state_mon 置空（全天候模式），init 仍因加载失败返回 false
	ASSERT_FALSE(dm.init(params, nullptr, nullptr));
	EXPECT_EQ(dm.getBDMgr(), nullptr);
	EXPECT_TRUE(dm.canSessionReceive("SSE"));
	EXPECT_TRUE(dm.canSessionReceive("anything"));

	params->release();
}

TEST_F(Q05DataManagerTest, WriterGuardsReturnDefaults)
{
	DataManager dm;  // 从未 init，_writer==NULL
	EXPECT_FALSE(dm.writeTick(nullptr, 0));
	EXPECT_FALSE(dm.writeOrderQueue(nullptr));
	EXPECT_FALSE(dm.writeOrderDetail(nullptr));
	EXPECT_FALSE(dm.writeTransaction(nullptr));
	EXPECT_EQ(dm.getCurTick("600000", "SSE"), nullptr);
	EXPECT_EQ(dm.getCurTick("600000"), nullptr);
	EXPECT_FALSE(dm.isSessionProceeded("SSE"));

	// 两者在无 writer 时都应静默返回而不是崩溃
	dm.add_ext_dumper("q05", nullptr);
	dm.transHisData("SSE");
	dm.release();
}

TEST_F(Q05DataManagerTest, BroadcastFanoutSkipsNullCaster)
{
	DataManager dm;
	CountingCaster c1;
	CountingCaster c2;

	dm.add_caster(nullptr);  // NULL 被过滤
	dm.add_caster(&c1);
	dm.add_caster(&c2);

	WTSTickData* tick = WTSTickData::create("600000");
	ASSERT_NE(tick, nullptr);
	dm.broadcastTick(tick);
	EXPECT_EQ(c1._tickCnt, 1);
	EXPECT_EQ(c2._tickCnt, 1);
	EXPECT_EQ(c1._lastTick, tick);

	// 未覆写的三个重载走 IDataCaster 默认空实现，仅验证扇出可达且不改计数
	dm.broadcastOrdQue(nullptr);
	dm.broadcastOrdDtl(nullptr);
	dm.broadcastTrans(nullptr);
	EXPECT_EQ(c1._tickCnt, 1);

	tick->release();
}

}  // namespace
