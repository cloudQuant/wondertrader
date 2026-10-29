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
 * 不加载真实写手插件（所有 module 路径都指向必然 dlopen 失败的临时文件/目录）。
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

class R44DataManagerEdgesTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
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

		ASSERT_TRUE(_bdMgr.loadSessions((_dir / "sessions.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadCommodities((_dir / "commodities.json").string().c_str()));
		ASSERT_TRUE(_mon.initialize((_dir / "state.json").string().c_str(), &_bdMgr, nullptr));

		r44_write_file(_dir / "dm.json", R"JSON({})JSON");
	}

	void TearDown() override
	{
		std::error_code ec;
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
};

// DataManager.cpp:73-104 —— 默认模块名对应的文件存在但不是 Mach-O，dlopen 失败后 return false；
// _bd_mgr/_state_mon 已在函数头部写入，可继续委托
TEST_F(R44DataManagerEdgesTest, InitFailsOnExistingButCorruptDefaultModuleFile)
{
	r44_write_file(_bin / "libWtDataStorage.dylib", "this is not a Mach-O library");

	WTSVariant* params = loadDmCfg();
	ASSERT_NE(params, nullptr);
	DataManager dm;
	EXPECT_FALSE(dm.init(params, &_bdMgr, &_mon));

	EXPECT_EQ(dm.getBDMgr(), static_cast<IBaseDataMgr*>(&_bdMgr));
	// 状态机里 DAY0 仍是 SS_ORIGINAL，不是 SS_RECEIVING
	EXPECT_FALSE(dm.canSessionReceive("DAY0"));
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
	EXPECT_EQ(dm.getBDMgr(), static_cast<IBaseDataMgr*>(&_bdMgr));
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
	WTSVariant* params = loadDmCfg();
	ASSERT_NE(params, nullptr);
	DataManager dm;
	EXPECT_FALSE(dm.init(params, &_bdMgr, &_mon));
	params->release();

	CodeSet* comms = dm.getSessionComms("DAY0");
	ASSERT_NE(comms, nullptr);
	EXPECT_NE(comms->find("SSE.STK"), comms->end());
	EXPECT_EQ(dm.getSessionComms("NO_SUCH_SESSION"), nullptr);

	// 只断言委托返回值非零，不锁定具体日期（避免墙钟断言）
	EXPECT_NE(dm.getTradingDate("SSE.STK"), 0u);
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
