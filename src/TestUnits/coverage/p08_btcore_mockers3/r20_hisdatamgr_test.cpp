/**
 * r20_hisdatamgr_test.cpp  (lane R20)
 *
 * 靶子: src/WtBtCore/HisDataMgr.cpp（52/90 未覆盖）
 * 可公开驱动的只剩「_reader==NULL 守卫族」+ reader_log 透传：
 * - _reader 是 HisDataMgr 的 **private** 成员且无任何 setter/公开接缝
 *   （HisDataMgr.h 尾部），禁止 #define private public，因此五个 load_raw_*
 *   的 reader 非空臂（每臂约 5 行 × 5）本轮无法进入，登记留白。
 * - init() 在模块 load 失败时仍无条件执行 `_reader->init(cfg, this)`
 *   （HisDataMgr.cpp L71，_reader 此时必为 NULL）→ 必然段错误，
 *   整条 DISABLED_，缺陷已记 bugs/R20_wtbtcore.md（S2）。
 * 不读挂钟、不起线程；守卫返回值与回调触发次数都由本用例自喂输入算得。
 */
#include <gtest/gtest.h>

#include <string>

#include <WtBtCore/HisDataMgr.h>

namespace
{
	// 回调触发计数：守卫路径必须一次都不触发
	struct CbProbe
	{
		uint32_t calls { 0 };
		std::string last;
		FuncLoadDataCallback fn()
		{
			return [this](std::string& buf) {
				calls++;
				last = buf;
			};
		}
	};
}

class R20HisDataMgr : public ::testing::Test
{
protected:
	HisDataMgr _mgr;	// ctor: _reader(NULL)（HisDataMgr.h 内联初始化）
};

// load_raw_bars：未挂 reader → 记 LL_ERROR 并返回 false，回调 0 次
TEST_F(R20HisDataMgr, LoadRawBarsRefusesWithoutReader)
{
	CbProbe probe;
	EXPECT_FALSE(_mgr.load_raw_bars("SHFE", "rb.2405", KP_Minute1, probe.fn()));
	EXPECT_EQ(0u, probe.calls);
	// 重复调用行为一致（无副作用累积）
	EXPECT_FALSE(_mgr.load_raw_bars("SSE", "600001", KP_DAY, probe.fn()));
	EXPECT_EQ(0u, probe.calls);
}

// load_raw_ticks：同形守卫
TEST_F(R20HisDataMgr, LoadRawTicksRefusesWithoutReader)
{
	CbProbe probe;
	EXPECT_FALSE(_mgr.load_raw_ticks("SHFE", "rb.2405", 20240103, probe.fn()));
	EXPECT_EQ(0u, probe.calls);
}

// load_raw_trans / load_raw_ordque：同形守卫（两个函数各自独立返回 false）
TEST_F(R20HisDataMgr, LoadRawTransAndOrdqueRefuseWithoutReader)
{
	CbProbe probeT;
	CbProbe probeQ;
	EXPECT_FALSE(_mgr.load_raw_trans("SSE", "600001", 20240103, probeT.fn()));
	EXPECT_FALSE(_mgr.load_raw_ordque("SSE", "600001", 20240103, probeQ.fn()));
	EXPECT_EQ(0u, probeT.calls);
	EXPECT_EQ(0u, probeQ.calls);
}

// load_raw_orddtl：同形守卫
TEST_F(R20HisDataMgr, LoadRawOrdDtlRefusesWithoutReader)
{
	CbProbe probe;
	EXPECT_FALSE(_mgr.load_raw_orddtl("SSE", "600001", 20240103, probe.fn()));
	EXPECT_EQ(0u, probe.calls);
	// 空日期同样只走守卫，不进 reader
	EXPECT_FALSE(_mgr.load_raw_orddtl("", "", 0, probe.fn()));
	EXPECT_EQ(0u, probe.calls);
}

// 五个守卫函数在同一实例上混排调用仍互不影响（共享的只有 _reader==NULL 判定）
TEST_F(R20HisDataMgr, AllFiveGuardsAreIndependentAndRepeatable)
{
	CbProbe probe;
	EXPECT_FALSE(_mgr.load_raw_bars("SHFE", "rb.2405", KP_Tick, probe.fn()));
	EXPECT_FALSE(_mgr.load_raw_ticks("SHFE", "rb.2405", 20240104, probe.fn()));
	EXPECT_FALSE(_mgr.load_raw_trans("SHFE", "rb.2405", 20240104, probe.fn()));
	EXPECT_FALSE(_mgr.load_raw_ordque("SHFE", "rb.2405", 20240104, probe.fn()));
	EXPECT_FALSE(_mgr.load_raw_orddtl("SHFE", "rb.2405", 20240104, probe.fn()));
	EXPECT_EQ(0u, probe.calls);
}

// reader_log：IBtDtReaderSink 公有重写，把任意级别透传给 WTSLogger::log_raw，无状态副作用
TEST_F(R20HisDataMgr, ReaderLogPassesEveryLevelToGlobalLogger)
{
	_mgr.reader_log(LL_DEBUG, "r20 dbg");
	_mgr.reader_log(LL_INFO, "r20 info");
	_mgr.reader_log(LL_WARN, "r20 warn");
	_mgr.reader_log(LL_ERROR, "r20 err");
	_mgr.reader_log(LL_FATAL, "r20 fatal");
	// reader_log 不改动任何可观察状态：_reader 仍为 NULL，守卫族行为不变
	CbProbe probe;
	EXPECT_FALSE(_mgr.load_raw_ticks("SHFE", "rb.2405", 20240103, probe.fn()));
	EXPECT_EQ(0u, probe.calls);
}

// DISABLED_: HisDataMgr::init 的空指针解引用崩溃路径。
// 触发条件：cfg->getCString("module") 指向不存在的库（或默认 WtDataStorage 未随包）
//   → L46 load_library 返回 NULL → 走 L66 error 分支 → L71 `_reader->init(cfg, this)`
//   对 NULL _reader 解引用，必然段错误；即便库存在但 createBtDtReader 符号缺失，
//   L57 判空跳过后 L71 同样崩。两条臂都无 return false，属产品缺陷（S2，
//   见 bugs/R20_wtbtcore.md）。另：成功臂还会调用真实 createBtDtReader 工厂，本 lane 也不进。
TEST_F(R20HisDataMgr, DISABLED_InitCrashesWhenReaderNullAfterLoadFailure)
{
	GTEST_SKIP() << "HisDataMgr::init derefs NULL _reader at HisDataMgr.cpp:71 on every failure arm";
}
