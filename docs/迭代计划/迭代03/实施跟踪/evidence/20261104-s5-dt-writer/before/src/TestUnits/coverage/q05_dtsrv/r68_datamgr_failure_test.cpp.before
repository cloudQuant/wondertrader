/*!
 * \file r68_datamgr_failure_test.cpp
 * \brief WtDtCore/DataManager 的写入器加载失败与空查询补测（迭代01 第十三波 R68）
 *
 * 与既有用例的分工（均只读，不修改）：
 *  - q05_dtcore_datamgr_test.cpp / r18_state_info_datamgr_test.cpp /
 *    r44_datamgr_caster_edges_test.cpp 已覆盖默认模块名缺失、同名文件损坏、
 *    同名目录、显式模块名缺失、_writer==NULL 的全部写入守卫与广播扇出；
 *  - 本文件只补「显式 module 名解析到目录 / 垃圾文件」两条 dlopen 失败，
 *    以及带真实数据对象的空查询（守卫必须先于任何数据访问）与外部转储器不回调。
 *
 * 不加载真实 WtDataStorage 写手（其 init 会起 10s 级检查线程并落盘），
 * 所有 module 都指向必然 dlopen 失败的临时路径。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/WtHelper.h>
#include <Includes/IDataWriter.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WTSTools/WTSBaseDataMgr.h>

#include "r68_test_util.h"

namespace fs = std::filesystem;

namespace {

class R68CountingDumper : public IHisDataDumper
{
public:
	virtual bool dumpHisBars(const char* /*stdCode*/, const char* /*period*/,
		WTSBarStruct* /*bars*/, uint32_t /*count*/) override
	{
		_calls++;
		return true;
	}

	virtual bool dumpHisTicks(const char* /*stdCode*/, uint32_t /*uDate*/,
		WTSTickStruct* /*ticks*/, uint32_t /*count*/) override
	{
		_calls++;
		return true;
	}

	int _calls { 0 };
};

class R68DataManagerFailureTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r68::make_tmp_dir("dm");
		_bin = _dir / "bin";
		fs::create_directories(_bin);
		WtHelper::set_module_dir((_bin.string() + "/").c_str());
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	WTSVariant* loadCfg(const char* name, const std::string& body)
	{
		fs::path p = _dir / name;
		r68::write_file(p, body);
		return WTSCfgLoader::load_from_file(p.string().c_str());
	}

	fs::path _dir;
	fs::path _bin;
};

// DataManager.cpp:73-78、100-104 —— 显式 module 名拼出的路径是一个目录，
// dlopen 失败 -> error 日志 + return false；_bd_mgr 已在函数头写入，委托仍可用。
TEST_F(R68DataManagerFailureTest, InitFailsWhenCustomModuleNameIsDirectory)
{
	fs::create_directories(_bin / "libr68_store.dylib");

	WTSVariant* params = loadCfg("dir_module.json", R"JSON({"module": "r68_store"})JSON");
	ASSERT_NE(params, nullptr);

	WTSBaseDataMgr bdMgr;
	DataManager dm;
	EXPECT_FALSE(dm.init(params, &bdMgr, nullptr));
	params->release();

	EXPECT_EQ(dm.getBDMgr(), static_cast<IBaseDataMgr*>(&bdMgr));
	// 状态机为 NULL -> 全天候模式；写手为 NULL -> 写入/查询守卫全部生效
	EXPECT_TRUE(dm.canSessionReceive("SSE"));
	EXPECT_FALSE(dm.writeTick(nullptr, 0));
	EXPECT_EQ(dm.getCurTick("600000", "SSE"), nullptr);
}

// DataManager.cpp:78、100-104 —— 显式 module 文件存在但不是 Mach-O，
// dlopen 失败同样在 return false 收口，不会走到 :106 的 _writer->init。
TEST_F(R68DataManagerFailureTest, InitFailsWhenCustomModuleFileIsCorrupt)
{
	r68::write_file(_bin / "libr68_corrupt.dylib", "not a Mach-O image");

	WTSVariant* params = loadCfg("bad_module.json", R"JSON({"module": "r68_corrupt"})JSON");
	ASSERT_NE(params, nullptr);

	DataManager dm;
	R68CountingDumper dumper;
	EXPECT_FALSE(dm.init(params, nullptr, nullptr));
	params->release();

	// _writer==NULL 时 add_ext_dumper 必须直接返回，不能回调/持有转储器
	dm.add_ext_dumper("r68", &dumper);
	EXPECT_EQ(dumper._calls, 0);
	EXPECT_FALSE(dm.writeOrderQueue(nullptr));
	EXPECT_FALSE(dm.writeOrderDetail(nullptr));
	EXPECT_FALSE(dm.writeTransaction(nullptr));
	EXPECT_FALSE(dm.isSessionProceeded("SSE"));
	dm.transHisData("SSE");	// _writer==NULL -> 静默 no-op
	dm.release();
	dm.release();	// 幂等
}

// DataManager.cpp:67-70、100-104、237-245 —— 显式坏 module + 全 NULL 装配：
// init 失败但 _state_mon 为 NULL，canSessionReceive 按全天候模式返回 true；
// getBDMgr 返回 NULL；所有取数入口在 _writer 守卫处返回空值。
TEST_F(R68DataManagerFailureTest, FailedInitWithNullTargetsStaysAlldayMode)
{
	WTSVariant* params = loadCfg("null_targets.json", R"JSON({"module": "r68_missing"})JSON");
	ASSERT_NE(params, nullptr);

	DataManager dm;
	EXPECT_FALSE(dm.init(params, nullptr, nullptr));
	params->release();

	EXPECT_EQ(dm.getBDMgr(), nullptr);
	EXPECT_TRUE(dm.canSessionReceive("SSE"));
	EXPECT_TRUE(dm.canSessionReceive("NO_SUCH_SESSION"));
	EXPECT_FALSE(dm.writeTick(nullptr, 1));
	EXPECT_EQ(dm.getCurTick(nullptr, nullptr), nullptr);
}

// DataManager.cpp:143-206 —— 传入真实数据对象而不是 NULL：_writer==NULL 的守卫
// 必须先于任何 tick/逐笔结构访问（漏守卫会解引用空写手）。
TEST_F(R68DataManagerFailureTest, WriteAndQueryGuardsPrecedeDataAccess)
{
	DataManager dm;	// 从未 init

	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	strcpy(ts.code, "600000");
	strcpy(ts.exchg, "SSE");
	WTSTickData* tick = WTSTickData::create(ts);
	WTSOrdQueData* oq = WTSOrdQueData::create("600000");
	WTSOrdDtlData* od = WTSOrdDtlData::create("600000");
	WTSTransData* tr = WTSTransData::create("600000");
	ASSERT_NE(tick, nullptr);
	ASSERT_NE(oq, nullptr);
	ASSERT_NE(od, nullptr);
	ASSERT_NE(tr, nullptr);

	EXPECT_FALSE(dm.writeTick(tick, 0));
	EXPECT_FALSE(dm.writeOrderQueue(oq));
	EXPECT_FALSE(dm.writeOrderDetail(od));
	EXPECT_FALSE(dm.writeTransaction(tr));
	EXPECT_EQ(dm.getCurTick("600000", "SSE"), nullptr);
	EXPECT_EQ(dm.getCurTick(""), nullptr);

	// 对象没有被写手接管，引用计数保持自持状态
	EXPECT_TRUE(tick->isSingleRefs());
	EXPECT_TRUE(oq->isSingleRefs());
	EXPECT_TRUE(od->isSingleRefs());
	EXPECT_TRUE(tr->isSingleRefs());

	tick->release();
	oq->release();
	od->release();
	tr->release();
}

}	// namespace
