/*!
 * \file r145_dtcore_edges_test.cpp
 * \brief WtDtCore 公开面的两个剩余边界（迭代01 第二十二波 R145）
 *
 *  - DataManager 扇出到「只重写 tick 的 IDataCaster」：证明 IDataCaster.h:40/46/52 的三个
 *    默认空实现确实被虚分派到（R44 的 R44FullCaster 把四个重载都重写了，默认实现从未执行）；
 *  - ShmCaster::init 在 active=true 且 path 为空串时的失败形态：BoostFile 打不开空路径、
 *    BoostMappingFile::map("") 抛出，init 没有捕获也不返回 false，异常直接穿透（与 R68 的
 *    “path 是目录” 同一缺陷的另一输入），且失败后 _queue 仍为 NULL，四个 broadcast 继续走守卫。
 *
 * 不建共享内存映射（空路径在 map 之前就失败）、不起线程、不 sleep、不读挂钟。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <exception>
#include <filesystem>
#include <string>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/IDataCaster.h>
#include <WtDtCore/ShmCaster.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>

#include "r145_test_util.h"

namespace fs = std::filesystem;

namespace {

// 只实现纯虚的 tick 重载；OrdQue/OrdDtl/Trans 三个重载故意留给 IDataCaster 的默认空实现。
class R145TickOnlyCaster : public IDataCaster
{
public:
	virtual void broadcast(WTSTickData* /*curTick*/) override { _tickCnt++; }

	int _tickCnt { 0 };
};

class R145DtCoreEdgesTest : public r145::DtcoreFixture
{
};

/*
 * DataManager.cpp:252-289 + IDataCaster.h:40/46/52
 * 四个 broadcast 各自到达 caster：tick 命中子类重写，另外三个命中基类默认空实现。
 * 空实现若被误删成纯虚，本用例无法编译；若默认实现误 retain/release，引用计数断言会变。
 */
TEST_F(R145DtCoreEdgesTest, DataManagerBaseCasterDefaultOverloadsAreNoops)
{
	DataManager dm;
	R145TickOnlyCaster caster;
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

	EXPECT_EQ(caster._tickCnt, 1);
	// 基类三个默认实现只做空操作，不接管、不释放载荷
	EXPECT_TRUE(tick->isSingleRefs());
	EXPECT_TRUE(que->isSingleRefs());
	EXPECT_TRUE(dtl->isSingleRefs());
	EXPECT_TRUE(trans->isSingleRefs());

	tick->release();
	que->release();
	dtl->release();
	trans->release();
}

/*
 * ShmCaster.cpp:35-51 —— active=true 且 path 为空串：
 *   L40 create_or_open_file("") 失败（或直接抛），L41 truncate_file 无有效句柄，
 *   L47 BoostMappingFile::map("") 抛出；init 没有 try/catch，也没有把失败转成 false。
 * 该路径不会创建/截断任何文件，也不会建立映射，因此不违反「不建 shm 映射」约束。
 * 与 R68 的 “path 是目录” 同一缺陷，不重复登记（见 bugs/R68.md BUG-R68-002）。
 */
TEST_F(R145DtCoreEdgesTest, ShmCasterEmptyPathThrowsAndKeepsUninitializedGuards)
{
	const fs::path cfgFile = _dir / "shm_empty_path.json";
	r145::write_file(cfgFile, R"JSON({"active": true, "path": ""})JSON");

	WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgFile.string().c_str());
	ASSERT_NE(cfg, nullptr);

	ShmCaster caster;
	EXPECT_THROW(caster.init(cfg), std::exception);
	cfg->release();

	// 失败后 _queue==NULL、_inited==false：四个 broadcast 都必须在解引用队列前返回
	WTSTickData* tick = WTSTickData::create("600000");
	WTSOrdQueData* que = WTSOrdQueData::create("600000");
	WTSOrdDtlData* dtl = WTSOrdDtlData::create("600000");
	WTSTransData* trans = WTSTransData::create("600000");
	ASSERT_NE(tick, nullptr);
	ASSERT_NE(que, nullptr);
	ASSERT_NE(dtl, nullptr);
	ASSERT_NE(trans, nullptr);

	caster.broadcast(tick);
	caster.broadcast(que);
	caster.broadcast(dtl);
	caster.broadcast(trans);

	EXPECT_EQ(tick->retainCount(), 1u);
	EXPECT_EQ(que->retainCount(), 1u);
	EXPECT_EQ(dtl->retainCount(), 1u);
	EXPECT_EQ(trans->retainCount(), 1u);

	tick->release();
	que->release();
	dtl->release();
	trans->release();
}

}	// namespace
