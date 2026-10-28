/*!
 * \file r183_dtcore_remaining_test.cpp
 * \brief R183 · WtDtCore 剩余公开面（IndexWorkerMgr 工厂跳过、DataManager 扇出、
 *        ShmCaster 值类型、StateMonitor.h 查询）
 *
 * 覆盖的生产行段：
 *  - WtDtCore/IndexWorkerMgr.cpp:87-91   dlopen 成功但无 create_index_factory 时 free+continue
 *  - WtDtCore/IndexWorkerMgr.h:26-35/62-63  IdxWorkerWrapper NULL 路径与 accessors
 *  - WtDtCore/DataManager.h:79-84          add_caster 的 NULL 守卫与 emplace_back
 *  - WtDtCore/ShmCaster.h:57/83            DataItem/_DataQueue 默认构造
 *  - WtDtCore/StateMonitor.h:70-81/169-222 isInSections/isAnyInState/isAllInState/isInState
 *
 * 边界：不建 socket/共享内存/监控线程；StateMonitor 只 initialize 不 run；
 *      IndexWorkerMgr poolsize=0 且无 indice 数组；临时文件只落系统临时目录并清理。
 */
#include <gtest/gtest.h>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/IDataCaster.h>
#include <WtDtCore/IndexWorkerMgr.h>
#include <WtDtCore/ShmCaster.h>
#include <WtDtCore/StateMonitor.h>
#include <WTSTools/WTSBaseDataMgr.h>

#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>

#include <mach-o/dyld.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

fs::path make_tmp_dir(const char* tag)
{
	fs::path p = fs::temp_directory_path()
		/ (std::string("r183_dt_") + tag + "_" + std::to_string((long)::getpid()));
	std::error_code ec;
	fs::remove_all(p, ec);
	fs::create_directories(p, ec);
	return p;
}

void write_file(const fs::path& p, const std::string& body)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << body;
}

fs::path share_helper_lib()
{
	char exe[4096] = { 0 };
	uint32_t size = sizeof(exe);
	if (_NSGetExecutablePath(exe, &size) != 0)
		return fs::path();

	// <build>/bin/TestUnits/CoverageTests_dtcore -> <build>/bin/libWtShareHelper.dylib
	const fs::path exePath = fs::canonical(exe);
	return exePath.parent_path().parent_path() / "libWtShareHelper.dylib";
}

class CountingCaster : public IDataCaster
{
public:
	void broadcast(WTSTickData* curTick) override
	{
		_count++;
		_last = curTick;
	}

	int _count { 0 };
	WTSTickData* _last { nullptr };
};

const char* kSessions = R"JSON({
	"Q1": {
		"name": "R183 day session", "offset": 0,
		"sections": [{"from": 930, "to": 1130}, {"from": 1300, "to": 1500}]
	}
})JSON";

const char* kStateCfg = R"JSON({
	"Q1": {"inittime": 900, "closetime": 1510, "proctime": 1530}
})JSON";

}	// namespace

/*
 * 生产: IndexWorkerMgr.cpp:87-91；IndexWorkerMgr.h:26-35/62-63
 * 成员指针取 accessors（先 init 注入 bdMgr，poolsize=0 且无 indice 返回 false）；
 * 空 wrapper 的 self() 为 NULL 且析构不调用 delete_worker；
 * 目录里放一个能 dlopen 但不导出 create_index_factory 的 dylib，loadFactories 跳过并返回 true。
 */
TEST(R183DtIndexWorkerMgr, MissingFactorySymbolIsSkippedAndAccessorsRoundTrip)
{
	const fs::path dir = make_tmp_dir("idx");
	const fs::path cfgFile = dir / "idx.json";
	write_file(cfgFile, R"JSON({"poolsize": 0})JSON");

	WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgFile.string().c_str());
	ASSERT_NE(nullptr, cfg);

	WTSBaseDataMgr bdMgr;
	IndexWorkerMgr mgr;
	EXPECT_FALSE(mgr.init(cfg, nullptr, &bdMgr, nullptr));
	cfg->release();

	IHotMgr* (IndexWorkerMgr::*getHot)() = &IndexWorkerMgr::get_hot_mgr;
	IBaseDataMgr* (IndexWorkerMgr::*getBd)() = &IndexWorkerMgr::get_bd_mgr;
	EXPECT_EQ(nullptr, (mgr.*getHot)());
	EXPECT_EQ(static_cast<IBaseDataMgr*>(&bdMgr), (mgr.*getBd)());

	{
		IdxWorkerWrapper wrapper(nullptr, nullptr);
		IIndexWorker* (IdxWorkerWrapper::*self)() = &IdxWorkerWrapper::self;
		EXPECT_EQ(nullptr, (wrapper.*self)());
	}

	const fs::path lib = share_helper_lib();
	ASSERT_FALSE(lib.empty());
	ASSERT_TRUE(fs::exists(lib)) << lib;

	std::error_code ec;
	fs::create_symlink(lib, dir / "libnofactory.dylib", ec);
	ASSERT_FALSE(ec) << ec.message();
	EXPECT_TRUE(mgr.loadFactories(dir.string().c_str()));

	fs::remove_all(dir, ec);
}

/*
 * 生产: DataManager.h:79-84；DataManager.cpp:252-256
 * 成员指针调用 add_caster：NULL 早退；真实 caster 入 _casters 后由 broadcastTick 扇出。
 * 只传 NULL tick，fake caster 不解引用，避免依赖行情对象。
 */
TEST(R183DtDataManager, AddCasterRejectsNullAndBroadcasts)
{
	DataManager dm;
	void (DataManager::*addCaster)(IDataCaster*) = &DataManager::add_caster;
	(dm.*addCaster)(nullptr);

	CountingCaster caster;
	(dm.*addCaster)(&caster);

	dm.broadcastTick(nullptr);
	EXPECT_EQ(1, caster._count);
	EXPECT_EQ(nullptr, caster._last);
}

/*
 * 生产: ShmCaster.h:57/83
 * 只构造值类型，不 init 共享内存：DataItem 全零，CastQueue 容量固定 8192、
 * 初始可读索引 UINT64_MAX、可写索引与 pid 为 0。
 */
TEST(R183DtShmCaster, ValueTypesDefaultState)
{
	ShmCaster::DataItem item;
	EXPECT_EQ(0U, item._type);

	std::unique_ptr<ShmCaster::CastQueue> queue(new ShmCaster::CastQueue());
	EXPECT_EQ(8192U, queue->_capacity);
	EXPECT_EQ(UINT64_MAX, queue->_readable);
	EXPECT_EQ(0U, queue->_writable);
	EXPECT_EQ(0U, queue->_pid);
}

/*
 * 生产: StateMonitor.h:70-81/169-222
 * 单会话 Q1 initialize 后保持 SS_ORIGINAL：
 *  - isInSections 的空区间/半开区间边界；
 *  - isInState 命中相等/命中不等/sid 不存在；
 *  - isAnyInState 命中/全表未命中；
 *  - isAllInState 全部相等/首个不等；
 *  - 未 initialize 的空表：isAny=false、isAll=true、isIn=false。
 * 全部经成员函数指针调用，避免小函数被前端内联。
 */
TEST(R183DtStateMonitor, QuerySurfaceCoversHitMissAndEmptyMap)
{
	const fs::path dir = make_tmp_dir("sm");
	const fs::path sessions = dir / "sessions.json";
	const fs::path stateCfg = dir / "state.json";
	write_file(sessions, kSessions);
	write_file(stateCfg, kStateCfg);

	StateInfo info;
	info._sections.push_back(StateInfo::Section{930, 1130});
	info._sections.push_back(StateInfo::Section{1300, 1500});
	bool (StateInfo::*inSections)(uint32_t) = &StateInfo::isInSections;
	EXPECT_FALSE((info.*inSections)(929));
	EXPECT_TRUE((info.*inSections)(930));
	EXPECT_TRUE((info.*inSections)(1129));
	EXPECT_FALSE((info.*inSections)(1130));
	EXPECT_TRUE((info.*inSections)(1300));
	EXPECT_FALSE((info.*inSections)(1500));

	StateInfo emptyInfo;
	EXPECT_FALSE((emptyInfo.*inSections)(930));

	WTSBaseDataMgr bdMgr;
	ASSERT_TRUE(bdMgr.loadSessions(sessions.string().c_str()));

	StateMonitor mon;
	ASSERT_TRUE(mon.initialize(stateCfg.string().c_str(), &bdMgr, nullptr));

	using InStateFn = bool (StateMonitor::*)(const char*, SimpleState) const;
	using AnyStateFn = bool (StateMonitor::*)(SimpleState) const;
	InStateFn inState = &StateMonitor::isInState;
	AnyStateFn anyState = &StateMonitor::isAnyInState;
	AnyStateFn allState = &StateMonitor::isAllInState;

	EXPECT_TRUE((mon.*inState)("Q1", SS_ORIGINAL));
	EXPECT_FALSE((mon.*inState)("Q1", SS_RECEIVING));
	EXPECT_FALSE((mon.*inState)("GHOST", SS_ORIGINAL));

	EXPECT_TRUE((mon.*anyState)(SS_ORIGINAL));
	EXPECT_FALSE((mon.*anyState)(SS_RECEIVING));

	EXPECT_TRUE((mon.*allState)(SS_ORIGINAL));
	EXPECT_FALSE((mon.*allState)(SS_RECEIVING));
	EXPECT_FALSE((mon.*allState)(SS_Holiday));

	StateMonitor emptyMon;
	EXPECT_FALSE((emptyMon.*anyState)(SS_ORIGINAL));
	EXPECT_TRUE((emptyMon.*allState)(SS_ORIGINAL));
	EXPECT_TRUE((emptyMon.*allState)(SS_Holiday));
	EXPECT_FALSE((emptyMon.*inState)("Q1", SS_ORIGINAL));

	std::error_code ec;
	fs::remove_all(dir, ec);
}
