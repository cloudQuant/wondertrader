/*!
 * \file r44_datamgr_caster_edges_test.cpp
 * \brief WtDtCore/DataManager 与 IDataCaster 扇出边界补测（迭代01 第十波 R44）
 *
 * 与既有 q05_dtcore_datamgr_test.cpp / r18_state_info_datamgr_test.cpp 的分工：
 * 那两份覆盖 module 缺失/默认命名、_writer==NULL 的全部写入守卫与 tick 扇出。
 * 本文件补：
 *  - 模块目录里「同名文件存在但不是 Mach-O」与「同名目录」两条 dlopen 失败路径
 *    （DataManager.cpp:78-104）
 *  - add_caster 重复登记同一指针时的重复扇出（DataManager.h:78-84）
 *  - 四个 broadcast 重载各自到达 caster（DataManager.cpp:252-289）
 *  - release 在 _writer==NULL 时的幂等性（L127-134）
 *  - getSessionComms 返回品种 fullPid 集合（L297-300），以及 getTradingDate 委托
 * 禁用占位说明 bdMgr 是必需依赖；全天候装配只允许 StateMonitor 为空。
 * 仅 SessionComms 成功路径装载本地 AD writer；模块副本与 writer cache 均限制在临时目录。
 * 其余模块失败路径仍只使用必然 dlopen 失败的临时文件/目录。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
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

#include "r102_dtcore_util.h"

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

class R44CountingCaster : public IDataCaster
{
public:
	virtual void broadcast(WTSTickData* /*curTick*/) override { _tickCnt++; }
	int _tickCnt { 0 };
};

class R44FullCaster : public IDataCaster
{
public:
	virtual void broadcast(WTSTickData* /*curTick*/) override { tickCnt++; }
	virtual void broadcast(WTSOrdQueData* /*curOrdQue*/) override { queCnt++; }
	virtual void broadcast(WTSOrdDtlData* /*curOrdDtl*/) override { dtlCnt++; }
	virtual void broadcast(WTSTransData* /*curTrans*/) override { transCnt++; }

	int tickCnt { 0 };
	int queCnt { 0 };
	int dtlCnt { 0 };
	int transCnt { 0 };
};

class R44ModuleDirGuard
{
public:
	void capture()
	{
		if (_captured)
			return;
		_previous = WtHelper::get_module_dir();
		_captured = true;
	}

	void restore()
	{
		if (!_captured)
			return;
		WtHelper::set_module_dir(_previous.c_str());
		_captured = false;
	}

	~R44ModuleDirGuard() { restore(); }

private:
	std::string _previous;
	bool _captured { false };
};

class R44DataManagerEdgesTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_moduleDirGuard.capture();
		_dir = r44_make_tmp_dir("r44_dm");
		_bin = _dir / "bin";
		fs::create_directories(_bin);
		WtHelper::set_module_dir((_bin.string() + "/").c_str());

		r44_write_file(_dir / "sessions.json",
			R"JSON({
				"DAY0": {"name": "日盘", "offset": 0,
					"sections": [{"from": 930, "to": 1500}]}
			})JSON");
		r44_write_file(_dir / "commodities.json",
			R"JSON({
				"SSE": {"STK": {"name": "股票", "session": "DAY0", "holiday": "CHINA",
					"pricetick": 0.01, "volscale": 1, "covermode": 0,
					"pricemode": 0, "trademode": 1, "lotstick": 1, "minlots": 1}}
			})JSON");
		r44_write_file(_dir / "state.json",
			R"JSON({"DAY0": {"inittime": 900, "closetime": 1510, "proctime": 1530}})JSON");
		// I03-NEW-043（卡 1=A+C）：StateMonitor 初始化对有关联品种的会话使用 checked
		// 日期映射，品种引用的节假日模板（CHINA）必须已加载，否则 MissingCalendar、
		// 初始化失败。原夹具未加载节假日，SetUp 因此失败；此处补空节假日模板。
		r44_write_file(_dir / "holidays.json", R"JSON({"CHINA": []})JSON");

		ASSERT_TRUE(_bdMgr.loadSessions((_dir / "sessions.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadCommodities((_dir / "commodities.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadHolidays((_dir / "holidays.json").string().c_str()));
		ASSERT_TRUE(_mon.initialize((_dir / "state.json").string().c_str(), &_bdMgr, nullptr));

		r44_write_file(_dir / "dm.json", R"JSON({})JSON");
	}

	void TearDown() override
	{
		_moduleDirGuard.restore();
		std::error_code ec;
		if (!_dir.empty())
			fs::remove_all(_dir, ec);
	}

	WTSVariant* loadDmCfg()
	{
		return WTSCfgLoader::load_from_file((_dir / "dm.json").string().c_str());
	}

	fs::path _dir;
	fs::path _bin;
	WTSBaseDataMgr _bdMgr;
	StateMonitor _mon;
	// This fixture changes WtHelper global state; restore it before removing the temp module directory.
	R44ModuleDirGuard _moduleDirGuard;
};

// 迭代03 卡9（dt-writer 包，2026-11-04）：init 失败（模块/工厂未就绪）回滚为未初始化态，
// 借用的基础数据/状态机不在失败态对外发布（9c 拒绝/保留原实例的对偶：失败不留部分状态，可重试；
// 同 WP2“失败对象不得进入运行态”、13a 先准备后发布）。旧断言“函数头部已赋值”来自迭代01覆盖计划
// 的旧实现，与该合同冲突，按 BUG-I02-NEW-056 归因更新；原文件见证据 before/*.before。
// 默认模块名对应的文件存在但不是 Mach-O，dlopen 失败后 return false，且不登记借用依赖。
TEST_F(R44DataManagerEdgesTest, InitFailsOnExistingButCorruptDefaultModuleFile)
{
	r44_write_file(_bin / "libWtDataStorage.dylib", "this is not a Mach-O library");

	WTSVariant* params = loadDmCfg();
	ASSERT_NE(params, nullptr);
	DataManager dm;
	EXPECT_FALSE(dm.init(params, &_bdMgr, &_mon));

	EXPECT_EQ(dm.writerLifecycleState(), DataManager::WLS_None);	// 回滚，可重试
	EXPECT_EQ(dm.getBDMgr(), nullptr);
	// 未登记状态机 -> 按未初始化（全天候）语义；DAY0 的 SS_ORIGINAL 不再被失败实例引用
	EXPECT_TRUE(dm.canSessionReceive("DAY0"));
	EXPECT_FALSE(dm.writeTick(nullptr, 0));
	params->release();
}

// DataManager.cpp:78-103 —— 默认模块名对应的是目录而不是文件，dlopen 同样失败
TEST_F(R44DataManagerEdgesTest, InitFailsWhenDefaultModuleNameIsDirectory)
{
	fs::create_directories(_bin / "libWtDataStorage.dylib");

	WTSVariant* params = loadDmCfg();
	ASSERT_NE(params, nullptr);
	DataManager dm;
	EXPECT_FALSE(dm.init(params, &_bdMgr, &_mon));
	// 卡9：失败态不发布借用依赖（见上方说明）
	EXPECT_EQ(dm.writerLifecycleState(), DataManager::WLS_None);
	EXPECT_EQ(dm.getBDMgr(), nullptr);
	params->release();
}

// DataManager.h:78-84 + DataManager.cpp:252-256 —— 同一 caster 登记两次会被扇出两次
TEST_F(R44DataManagerEdgesTest, DuplicateCasterIsFannedOutTwice)
{
	DataManager dm;
	R44CountingCaster caster;
	dm.add_caster(&caster);
	dm.add_caster(&caster);
	dm.add_caster(nullptr);	// NULL 被过滤

	WTSTickData* tick = WTSTickData::create("600000");
	ASSERT_NE(tick, nullptr);
	dm.broadcastTick(tick);
	EXPECT_EQ(caster._tickCnt, 2);
	tick->release();
}

// DataManager.cpp:252-289 —— 四个广播重载分别到达 caster 的对应虚函数
TEST_F(R44DataManagerEdgesTest, FourBroadcastOverloadsReachCaster)
{
	DataManager dm;
	R44FullCaster caster;
	dm.add_caster(&caster);

	WTSTickData* tick = WTSTickData::create("600000");
	WTSOrdQueData* que = WTSOrdQueData::create("600000");
	WTSOrdDtlData* dtl = WTSOrdDtlData::create("600000");
	WTSTransData* trans = WTSTransData::create("600000");
	ASSERT_NE(tick, nullptr);
	ASSERT_NE(que, nullptr);
	ASSERT_NE(dtl, nullptr);
	ASSERT_NE(trans, nullptr);

	dm.broadcastTick(tick);
	dm.broadcastOrdQue(que);
	dm.broadcastOrdDtl(dtl);
	dm.broadcastTrans(trans);

	EXPECT_EQ(caster.tickCnt, 1);
	EXPECT_EQ(caster.queCnt, 1);
	EXPECT_EQ(caster.dtlCnt, 1);
	EXPECT_EQ(caster.transCnt, 1);

	tick->release();
	que->release();
	dtl->release();
	trans->release();
}

// DataManager.cpp:127-134 —— _writer==NULL 时 release 是空操作，连续调用无副作用
TEST_F(R44DataManagerEdgesTest, ReleaseWithoutWriterIsIdempotent)
{
	DataManager dm;
	dm.release();
	dm.release();
	EXPECT_FALSE(dm.writeTick(nullptr, 0));
	EXPECT_FALSE(dm.writeOrderQueue(nullptr));
	EXPECT_FALSE(dm.writeOrderDetail(nullptr));
	EXPECT_FALSE(dm.writeTransaction(nullptr));
	EXPECT_EQ(dm.getCurTick("600000", "SSE"), nullptr);
}

// DataManager.cpp:297-311 —— getSessionComms 返回品种 fullPid 集合（不是合约 fullCode），
// getTradingDate 原样委托给 WTSBaseDataMgr
TEST_F(R44DataManagerEdgesTest, SessionCommsExposeCommodityFullPids)
{
	const std::string buildBin = r102dt::bin_dir();
	const fs::path moduleSrc = fs::path(buildBin) / "libWtDataStorageAD.dylib";
	std::error_code statusError;
	const fs::file_status moduleStatus = fs::symlink_status(moduleSrc, statusError);
	if (statusError || fs::is_symlink(moduleStatus) || !fs::is_regular_file(moduleStatus))
		GTEST_SKIP() << "本地 libWtDataStorageAD.dylib 缺失或不是非符号链接普通文件";
	if (!r102dt::ad_writer_ready(buildBin))
		GTEST_SKIP() << "libWtDataStorageAD.dylib / createWriter 不可用";

	// 用本构建的 AD writer 做有效本地模块；模块副本和 writer cache 都留在 fixture 临时目录。
	const fs::path moduleCopy = _bin / "libWtDataStorageAD.dylib";
	std::error_code copyError;
	fs::copy_file(moduleSrc, moduleCopy, fs::copy_options::overwrite_existing, copyError);
	ASSERT_FALSE(copyError) << "无法复制本地 AD storage module: " << copyError.message();

	const std::string storePath = (_dir / "writer_store").string() + "/";
	r44_write_file(_dir / "dm.json",
		"{\"module\":\"WtDataStorageAD\",\"path\":\"" + storePath +
		"\",\"groupsize\":10,\"disabletick\":true,"
		"\"disablemin1\":true,\"disablemin5\":true,"
		"\"disableday\":true,\"async\":false}");
	WTSVariant* params = loadDmCfg();
	ASSERT_NE(params, nullptr);
	DataManager dm;
	const bool initialized = dm.init(params, &_bdMgr, &_mon);
	params->release();
	ASSERT_TRUE(initialized) << "AD storage writer must initialize before sink callbacks are queried";

	CodeSet* comms = dm.getSessionComms("DAY0");
	ASSERT_NE(comms, nullptr);
	EXPECT_NE(comms->find("SSE.STK"), comms->end());
	EXPECT_EQ(dm.getSessionComms("NO_SUCH_SESSION"), nullptr);

	// 只断言委托返回值非零，不锁定具体日期（避免墙钟断言）
	EXPECT_NE(dm.getTradingDate("SSE.STK"), 0u);
	dm.release();
}

/*
 * 无效依赖探针：QuoteFactory 的全天候装配仍传入 bdMgr，只把 StateMonitor 设为空。
 * 本用例原先把 bdMgr=null 和 init 失败后的查询当成正常用法；该前置条件不受支持，
 * 因而不作为 BUG-R44-008 的产品复现或验收。跳过以防显式启用 disabled 用例时崩溃。
 */
TEST_F(R44DataManagerEdgesTest, DISABLED_NullBaseDataManagerIsAnInvalidDependencyProbe)
{
	GTEST_SKIP() << "DataManager requires a base data manager; only StateMonitor is optional "
				 << "for the QuoteFactory all-day mode.";
}

}  // namespace
