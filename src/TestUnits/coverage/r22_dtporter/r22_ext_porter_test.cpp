/*
 * R22 - src/WtDtPorter 扩展 Parser / Dumper 转发层
 *
 * 覆盖（只走 WtDtPorter 这一族的符号缝，不起线程、不 sleep、不碰 socket/shm）：
 *  - WtDtRunner.cpp  registerParserPorter / parser_init|connect|disconnect|release /
 *    parser_subscribe|unsubscribe / on_ext_parser_quote / createExtParser /
 *    createExtDumper / registerExtDumper / registerExtHftDataDumper / dumpHis{Bars,Ticks,OrdQue,OrdDtl,Trans}
 *  - ExpParser.cpp   init/release/connect/disconnect/subscribe/unsubscribe/registerSpi
 *  - ExpParser.h     构造/析构/isConnected
 *  - ExpDumper.cpp   dumpHis{Bars,Ticks,OrdQue,OrdDtl,Trans} 五个转发
 *  - ExpDumper.h     构造/析构
 *  - WtDtPorter.cpp  create_ext_parser / parser_push_quote / register_parser_callbacks /
 *    create_ext_dumper / register_extended_dumper / register_extended_hftdata_dumper
 *
 * 观测口径全部来自本用例自己注册的回调计数器，不读挂钟、不读日志。
 * 生产代码一行未改；缺陷见 docs/迭代计划/迭代01/bugs/R22_dtporter.md。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "Includes/FasterDefs.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/IParserApi.h"
#include "Includes/WTSTypes.h"
#include "WTSTools/WTSBaseDataMgr.h"

#include "WtDtPorter/PorterDefs.h"
#include "WtDtPorter/ExpParser.h"
#include "WtDtPorter/ExpDumper.h"
#include "WtDtPorter/WtDtRunner.h"
#include "WtDtPorter/WtDtPorter.h"

USING_NS_WTP;

namespace {

//////////////////////////////////////////////////////////////////////////
// 记录器：每个用例 SetUp 清零，只统计自己注册的回调
struct EvtHit
{
	uint32_t	_evt;
	std::string	_id;
};

struct SubHit
{
	std::string	_id;
	std::string	_code;
	bool		_sub;
};

struct DumpHit
{
	std::string		_id;
	std::string		_code;
	std::string		_period;	// 仅 bars 用
	uint32_t		_date;		// 仅 hft/ticks 用
	const void*		_items;
	uint32_t		_count;
};

std::vector<EvtHit>& evt_hits()		{ static std::vector<EvtHit> v; return v; }
std::vector<SubHit>& sub_hits()		{ static std::vector<SubHit> v; return v; }
std::vector<DumpHit>& bar_hits()	{ static std::vector<DumpHit> v; return v; }
std::vector<DumpHit>& tick_hits()	{ static std::vector<DumpHit> v; return v; }
std::vector<DumpHit>& que_hits()	{ static std::vector<DumpHit> v; return v; }
std::vector<DumpHit>& dtl_hits()	{ static std::vector<DumpHit> v; return v; }
std::vector<DumpHit>& trx_hits()	{ static std::vector<DumpHit> v; return v; }

// 回调自身返回值可翻转，用来验证 WtDtRunner 是否原样透出返回值
bool g_cb_ret = true;

void PORTER_FLAG rec_evt(WtUInt32 evtId, const char* id)
{
	evt_hits().push_back(EvtHit { evtId, id == NULL ? std::string() : std::string(id) });
}

void PORTER_FLAG rec_sub(const char* id, const char* fullCode, bool isForSub)
{
	sub_hits().push_back(SubHit {
		id == NULL ? std::string() : std::string(id),
		fullCode == NULL ? std::string() : std::string(fullCode),
		isForSub });
}

bool PORTER_FLAG rec_bar(const char* id, const char* stdCode, const char* period,
	WTSBarStruct* bars, WtUInt32 count)
{
	bar_hits().push_back(DumpHit { id ? id : "", stdCode ? stdCode : "", period ? period : "",
		0, bars, count });
	return g_cb_ret;
}

bool PORTER_FLAG rec_tick(const char* id, const char* stdCode, WtUInt32 uDate,
	WTSTickStruct* ticks, WtUInt32 count)
{
	tick_hits().push_back(DumpHit { id ? id : "", stdCode ? stdCode : "", "", uDate, ticks, count });
	return g_cb_ret;
}

bool PORTER_FLAG rec_que(const char* id, const char* stdCode, WtUInt32 uDate,
	WTSOrdQueStruct* items, WtUInt32 count)
{
	que_hits().push_back(DumpHit { id ? id : "", stdCode ? stdCode : "", "", uDate, items, count });
	return g_cb_ret;
}

bool PORTER_FLAG rec_dtl(const char* id, const char* stdCode, WtUInt32 uDate,
	WTSOrdDtlStruct* items, WtUInt32 count)
{
	dtl_hits().push_back(DumpHit { id ? id : "", stdCode ? stdCode : "", "", uDate, items, count });
	return g_cb_ret;
}

bool PORTER_FLAG rec_trx(const char* id, const char* stdCode, WtUInt32 uDate,
	WTSTransStruct* items, WtUInt32 count)
{
	trx_hits().push_back(DumpHit { id ? id : "", stdCode ? stdCode : "", "", uDate, items, count });
	return g_cb_ret;
}

// IParserSpi 替身：只为让 ExpParser::registerSpi 有一个可计数的 sink
class R22ParserSpi : public IParserSpi
{
public:
	virtual void handleSymbolList(const WTSArray*) override { }
	virtual void handleQuote(WTSTickData*, uint32_t) override { }
	virtual void handleParserLog(WTSLogLevel, const char*) override { }

	virtual IBaseDataMgr* getBaseDataMgr() override
	{
		bdm_calls_++;
		return &bdm_;
	}

	uint32_t		bdm_calls_ { 0 };
	WTSBaseDataMgr	bdm_;
};

void clear_all_hits()
{
	evt_hits().clear();
	sub_hits().clear();
	bar_hits().clear();
	tick_hits().clear();
	que_hits().clear();
	dtl_hits().clear();
	trx_hits().clear();
	g_cb_ret = true;
}

WTSTickStruct make_tick(const char* exchg, const char* code, uint32_t act_date, uint32_t trd_date)
{
	WTSTickStruct tick;
	memset(&tick, 0, sizeof(WTSTickStruct));
	strncpy(tick.exchg, exchg, sizeof(tick.exchg) - 1);
	strncpy(tick.code, code, sizeof(tick.code) - 1);
	tick.action_date = act_date;
	tick.action_time = 93000000;
	tick.trading_date = trd_date;
	tick.price = 3000;
	return tick;
}

} // namespace

class R22ExtPorterTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		clear_all_hits();
		// 全局单例上的槽位每次用例都清空，避免跨用例串扰
		register_parser_callbacks(NULL, NULL);
		register_extended_dumper(NULL, NULL);
		register_extended_hftdata_dumper(NULL, NULL, NULL);
	}

	void TearDown() override
	{
		register_parser_callbacks(NULL, NULL);
		register_extended_dumper(NULL, NULL);
		register_extended_hftdata_dumper(NULL, NULL, NULL);
	}
};

//////////////////////////////////////////////////////////////////////////
// 一、dumper 槽位的「注册前静默 / 注册后透出 / 置 NULL 撤销」
TEST_F(R22ExtPorterTest, FiveDumpersReturnFalseBeforeAnyRegistration)
{
	WtDtRunner runner;
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	WTSTickStruct tick = make_tick("SHFE", "rb2401", 20240110, 20240110);
	WTSOrdQueStruct que;
	memset(&que, 0, sizeof(que));
	WTSOrdDtlStruct dtl;
	memset(&dtl, 0, sizeof(dtl));
	WTSTransStruct trx;
	memset(&trx, 0, sizeof(trx));

	// WtDtRunner.cpp:385-438 的五个空槽分支
	EXPECT_FALSE(runner.dumpHisTicks("d1", "SHFE.rb2401", 20240110, &tick, 1));
	EXPECT_FALSE(runner.dumpHisBars("d1", "SHFE.rb2401", "m1", &bar, 1));
	EXPECT_FALSE(runner.dumpHisOrdDtl("d1", "SHFE.rb2401", 20240110, &dtl, 1));
	EXPECT_FALSE(runner.dumpHisOrdQue("d1", "SHFE.rb2401", 20240110, &que, 1));
	EXPECT_FALSE(runner.dumpHisTrans("d1", "SHFE.rb2401", 20240110, &trx, 1));

	EXPECT_TRUE(bar_hits().empty());
	EXPECT_TRUE(tick_hits().empty());
	EXPECT_TRUE(que_hits().empty());
	EXPECT_TRUE(dtl_hits().empty());
	EXPECT_TRUE(trx_hits().empty());
}

TEST_F(R22ExtPorterTest, RegisterExtDumperEnablesBarsAndTicksOnly)
{
	WtDtRunner runner;
	runner.registerExtDumper(rec_bar, rec_tick);
	// 高频三槽此刻仍是空的（WtDtRunner.cpp:378-383 未被调用）
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	WTSTickStruct tick = make_tick("SHFE", "rb2401", 20240110, 20240110);
	WTSOrdQueStruct que;
	memset(&que, 0, sizeof(que));
	WTSOrdDtlStruct dtl;
	memset(&dtl, 0, sizeof(dtl));
	WTSTransStruct trx;
	memset(&trx, 0, sizeof(trx));

	EXPECT_TRUE(runner.dumpHisBars("d2", "SHFE.rb2401", "m5", &bar, 3));
	EXPECT_TRUE(runner.dumpHisTicks("d2", "SHFE.rb2401", 20240111, &tick, 2));
	EXPECT_FALSE(runner.dumpHisOrdQue("d2", "SHFE.rb2401", 20240111, &que, 2));
	EXPECT_FALSE(runner.dumpHisOrdDtl("d2", "SHFE.rb2401", 20240111, &dtl, 2));
	EXPECT_FALSE(runner.dumpHisTrans("d2", "SHFE.rb2401", 20240111, &trx, 2));

	ASSERT_EQ(1U, bar_hits().size());
	ASSERT_EQ(1U, tick_hits().size());
	EXPECT_EQ("d2", bar_hits()[0]._id);
	EXPECT_EQ("m5", bar_hits()[0]._period);
	EXPECT_EQ(3U, bar_hits()[0]._count);
	EXPECT_EQ(2U, tick_hits()[0]._count);
	EXPECT_EQ(20240111U, tick_hits()[0]._date);
}

TEST_F(R22ExtPorterTest, RegisterHftDumperEnablesThreeHftSlotsOnly)
{
	WtDtRunner runner;
	runner.registerExtHftDataDumper(rec_que, rec_dtl, rec_trx);

	WTSOrdQueStruct que;
	memset(&que, 0, sizeof(que));
	WTSOrdDtlStruct dtl;
	memset(&dtl, 0, sizeof(dtl));
	WTSTransStruct trx;
	memset(&trx, 0, sizeof(trx));
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));

	EXPECT_TRUE(runner.dumpHisOrdQue("d3", "SSE.600000", 20240108, &que, 7));
	EXPECT_TRUE(runner.dumpHisOrdDtl("d3", "SSE.600000", 20240108, &dtl, 8));
	EXPECT_TRUE(runner.dumpHisTrans("d3", "SSE.600000", 20240108, &trx, 9));
	EXPECT_FALSE(runner.dumpHisBars("d3", "SSE.600000", "d1", &bar, 1));
	EXPECT_TRUE(bar_hits().empty());

	ASSERT_EQ(3U, que_hits().size() + dtl_hits().size() + trx_hits().size());
	EXPECT_EQ(7U, que_hits()[0]._count);
	EXPECT_EQ(8U, dtl_hits()[0]._count);
	EXPECT_EQ(9U, trx_hits()[0]._count);
	EXPECT_EQ(20240108U, que_hits()[0]._date);
}

TEST_F(R22ExtPorterTest, DumperForwardsPointerIdentityAndCallbackReturnValue)
{
	WtDtRunner runner;
	runner.registerExtDumper(rec_bar, rec_tick);
	runner.registerExtHftDataDumper(rec_que, rec_dtl, rec_trx);

	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	WTSTickStruct tick = make_tick("SHFE", "rb2401", 20240110, 20240110);
	WTSOrdQueStruct que;
	memset(&que, 0, sizeof(que));
	WTSOrdDtlStruct dtl;
	memset(&dtl, 0, sizeof(dtl));
	WTSTransStruct trx;
	memset(&trx, 0, sizeof(trx));

	g_cb_ret = false;
	EXPECT_FALSE(runner.dumpHisBars("d4", "SHFE.rb2401", "m1", &bar, 1));
	EXPECT_FALSE(runner.dumpHisTicks("d4", "SHFE.rb2401", 20240110, &tick, 1));
	EXPECT_FALSE(runner.dumpHisOrdQue("d4", "SHFE.rb2401", 20240110, &que, 1));
	EXPECT_FALSE(runner.dumpHisOrdDtl("d4", "SHFE.rb2401", 20240110, &dtl, 1));
	EXPECT_FALSE(runner.dumpHisTrans("d4", "SHFE.rb2401", 20240110, &trx, 1));
	g_cb_ret = true;

	// 指针必须原样透出，且 count=0 / 空 stdCode 不做任何合法性过滤
	EXPECT_EQ(&bar, bar_hits()[0]._items);
	EXPECT_EQ(&tick, tick_hits()[0]._items);
	EXPECT_EQ(&que, que_hits()[0]._items);
	EXPECT_EQ(&dtl, dtl_hits()[0]._items);
	EXPECT_EQ(&trx, trx_hits()[0]._items);

	EXPECT_TRUE(runner.dumpHisBars("d4", "", "m1", &bar, 0));
	EXPECT_EQ("d4", bar_hits()[1]._id);
	EXPECT_TRUE(bar_hits()[1]._code.empty());
	EXPECT_EQ(0U, bar_hits()[1]._count);
}

TEST_F(R22ExtPorterTest, ReregisteringDumpersWithNullsClearsEverySlot)
{
	WtDtRunner runner;
	runner.registerExtDumper(rec_bar, rec_tick);
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	ASSERT_TRUE(runner.dumpHisBars("d5", "SHFE.rb2401", "m1", &bar, 1));
	ASSERT_EQ(1U, bar_hits().size());

	runner.registerExtDumper(NULL, NULL);
	runner.registerExtHftDataDumper(NULL, NULL, NULL);
	EXPECT_FALSE(runner.dumpHisBars("d5", "SHFE.rb2401", "m1", &bar, 1));
	EXPECT_FALSE(runner.dumpHisTicks("d5", "SHFE.rb2401", 20240110, NULL, 0));
	EXPECT_EQ(1U, bar_hits().size());
}

//////////////////////////////////////////////////////////////////////////
// 二、parser 事件/订阅槽位
TEST_F(R22ExtPorterTest, SixParserEntriesAreSilentBeforeRegistration)
{
	WtDtRunner runner;
	// 未注册任何回调：六个入口必须安静返回（WtDtRunner.cpp:298-332 的 if 假支）
	runner.parser_init("p0");
	runner.parser_connect("p0");
	runner.parser_disconnect("p0");
	runner.parser_release("p0");
	runner.parser_subscribe("p0", "SHFE.rb2401");
	runner.parser_unsubscribe("p0", "SHFE.rb2401");
	EXPECT_TRUE(evt_hits().empty());
	EXPECT_TRUE(sub_hits().empty());
}

TEST_F(R22ExtPorterTest, FourParserEventsMapToPorterDefsConstants)
{
	WtDtRunner runner;
	runner.registerParserPorter(rec_evt, rec_sub);

	runner.parser_init("pA");
	runner.parser_connect("pA");
	runner.parser_disconnect("pA");
	runner.parser_release("pA");

	// 常量真值来自 PorterDefs.h:67-85
	ASSERT_EQ(4U, evt_hits().size());
	EXPECT_EQ(EVENT_PARSER_INIT, evt_hits()[0]._evt);
	EXPECT_EQ(EVENT_PARSER_CONNECT, evt_hits()[1]._evt);
	EXPECT_EQ(EVENT_PARSER_DISCONNECT, evt_hits()[2]._evt);
	EXPECT_EQ(EVENT_PARSER_RELEASE, evt_hits()[3]._evt);
	EXPECT_EQ(1U, EVENT_PARSER_INIT);
	EXPECT_EQ(4U, EVENT_PARSER_RELEASE);
	for (size_t i = 0; i < evt_hits().size(); i++)
		EXPECT_EQ("pA", evt_hits()[i]._id);

	// 注册语义是覆盖，不是追加
	runner.registerParserPorter(NULL, NULL);
	runner.parser_init("pA");
	EXPECT_EQ(4U, evt_hits().size());
}

TEST_F(R22ExtPorterTest, SubscribeAndUnsubscribeCarryOneCallbackPerCode)
{
	WtDtRunner runner;
	runner.registerParserPorter(rec_evt, rec_sub);

	CodeSet set;
	set.insert("SHFE.rb2401");
	set.insert("SSE.600000");
	runner.parser_subscribe("pB", "SHFE.rb2401");
	runner.parser_unsubscribe("pB", "SSE.600000");

	ASSERT_EQ(2U, sub_hits().size());
	EXPECT_TRUE(sub_hits()[0]._sub);
	EXPECT_FALSE(sub_hits()[1]._sub);
	EXPECT_EQ("pB", sub_hits()[0]._id);
	EXPECT_EQ("SHFE.rb2401", sub_hits()[0]._code);
	EXPECT_EQ("SSE.600000", sub_hits()[1]._code);
	EXPECT_TRUE(evt_hits().empty());
}

TEST_F(R22ExtPorterTest, SlotsOfTwoRunnersDoNotLeakIntoEachOther)
{
	WtDtRunner a, b;
	a.registerParserPorter(rec_evt, rec_sub);

	b.parser_init("pC");
	EXPECT_TRUE(evt_hits().empty());

	a.parser_connect("pC");
	ASSERT_EQ(1U, evt_hits().size());
	EXPECT_EQ(EVENT_PARSER_CONNECT, evt_hits()[0]._evt);
}

//////////////////////////////////////////////////////////////////////////
// 三、ExpParser（头文件在 WtDtPorter 目录内，构造函数/析构/isConnected 是内联）
TEST_F(R22ExtPorterTest, ExpParserLifecycleForwardsFourEventsAndReturnsTrue)
{
	WtDtRunner owner;
	owner.registerParserPorter(rec_evt, rec_sub);
	// ExpParser 硬绑 getRunner() 单例（ExpParser.cpp:21），所以这里的观测者
	// 必须是单例上的槽位，而不是 owner 的槽位 —— 见 BUG-R22-001
	register_parser_callbacks(rec_evt, rec_sub);

	ExpParser parser("xp1");
	EXPECT_TRUE(parser.init(NULL));
	EXPECT_TRUE(parser.connect());
	EXPECT_TRUE(parser.disconnect());
	parser.release();
	EXPECT_TRUE(parser.isConnected());

	ASSERT_EQ(4U, evt_hits().size());
	EXPECT_EQ(EVENT_PARSER_INIT, evt_hits()[0]._evt);
	EXPECT_EQ(EVENT_PARSER_CONNECT, evt_hits()[1]._evt);
	EXPECT_EQ(EVENT_PARSER_DISCONNECT, evt_hits()[2]._evt);
	EXPECT_EQ(EVENT_PARSER_RELEASE, evt_hits()[3]._evt);
	for (size_t i = 0; i < evt_hits().size(); i++)
		EXPECT_EQ("xp1", evt_hits()[i]._id);
}

TEST_F(R22ExtPorterTest, ExpParserSubscribeWalksEveryCodeInSet)
{
	register_parser_callbacks(rec_evt, rec_sub);

	ExpParser parser("xp2");
	CodeSet set;
	set.insert("SHFE.rb2401");
	set.insert("SHFE.rb2405");
	set.insert("SSE.600000");
	parser.subscribe(set);
	parser.unsubscribe(set);

	// 三个代码 × 两次调用；hashset 无序，故按集合成员校验而不是按下标
	ASSERT_EQ(6U, sub_hits().size());
	uint32_t nSub = 0, nUnsub = 0;
	for (size_t i = 0; i < sub_hits().size(); i++)
	{
		EXPECT_EQ("xp2", sub_hits()[i]._id);
		EXPECT_TRUE(sub_hits()[i]._code == "SHFE.rb2401"
			|| sub_hits()[i]._code == "SHFE.rb2405"
			|| sub_hits()[i]._code == "SSE.600000");
		if (sub_hits()[i]._sub)
			nSub++;
		else
			nUnsub++;
	}
	EXPECT_EQ(3U, nSub);
	EXPECT_EQ(3U, nUnsub);
	EXPECT_TRUE(evt_hits().empty());

	ExpParser empty("xp2b");
	CodeSet none;
	empty.subscribe(none);
	empty.unsubscribe(none);
	EXPECT_EQ(6U, sub_hits().size());	// 空集合不产生任何回调
}

TEST_F(R22ExtPorterTest, ExpParserRegisterSpiPullsBaseDataMgrOncePerValidSink)
{
	ExpParser parser("xp3");
	R22ParserSpi spi;

	parser.registerSpi(NULL);
	EXPECT_EQ(0U, spi.bdm_calls_);

	parser.registerSpi(&spi);
	EXPECT_EQ(1U, spi.bdm_calls_);

	parser.registerSpi(&spi);
	EXPECT_EQ(2U, spi.bdm_calls_);
}

TEST_F(R22ExtPorterTest, CreateExtParserFiresInitThroughExpParserOnSingleton)
{
	WtDtRunner runner;
	register_parser_callbacks(rec_evt, rec_sub);

	EXPECT_TRUE(runner.createExtParser("r22_ext_a"));

	// WtDtRunner.cpp:349-357 -> ParserAdapter::initExt -> ExpParser::init -> 单例槽位
	ASSERT_EQ(1U, evt_hits().size());
	EXPECT_EQ(EVENT_PARSER_INIT, evt_hits()[0]._evt);
	EXPECT_EQ("r22_ext_a", evt_hits()[0]._id);
	// 该 runner 自己没有 contracts（未 initialize），故 getContracts() 为空集，订阅扇出为 0
	EXPECT_TRUE(sub_hits().empty());
	EXPECT_EQ(1U, evt_hits().size());
}

//////////////////////////////////////////////////////////////////////////
// 四、ExpDumper 与 createExtDumper
TEST_F(R22ExtPorterTest, CreateExtDumperRegistersByIdOnSingleton)
{
	register_parser_callbacks(NULL, NULL);
	EXPECT_TRUE(create_ext_dumper("r22_dump_a"));

	// ExpDumper 由 runner 持有，这里用同 id 直接构造一个等价实例来驱动五个转发
	ExpDumper dumper("r22_dump_a");
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	WTSTickStruct tick = make_tick("SHFE", "rb2401", 20240110, 20240110);
	WTSOrdQueStruct que;
	memset(&que, 0, sizeof(que));
	WTSOrdDtlStruct dtl;
	memset(&dtl, 0, sizeof(dtl));
	WTSTransStruct trx;
	memset(&trx, 0, sizeof(trx));

	// 此刻全局槽位仍是空的（SetUp 里清过）
	EXPECT_FALSE(dumper.dumpHisBars("SHFE.rb2401", "m1", &bar, 1));
	EXPECT_FALSE(dumper.dumpHisTicks("SHFE.rb2401", 20240110, &tick, 1));
	EXPECT_FALSE(dumper.dumpHisOrdQue("SHFE.rb2401", 20240110, &que, 1));
	EXPECT_FALSE(dumper.dumpHisOrdDtl("SHFE.rb2401", 20240110, &dtl, 1));
	EXPECT_FALSE(dumper.dumpHisTrans("SHFE.rb2401", 20240110, &trx, 1));
	EXPECT_TRUE(bar_hits().empty());

	register_extended_dumper(rec_bar, rec_tick);
	register_extended_hftdata_dumper(rec_que, rec_dtl, rec_trx);
	EXPECT_TRUE(dumper.dumpHisBars("SHFE.rb2401", "m1", &bar, 4));
	EXPECT_TRUE(dumper.dumpHisTicks("SHFE.rb2401", 20240115, &tick, 5));
	EXPECT_TRUE(dumper.dumpHisOrdQue("SHFE.rb2401", 20240115, &que, 6));
	EXPECT_TRUE(dumper.dumpHisOrdDtl("SHFE.rb2401", 20240115, &dtl, 7));
	EXPECT_TRUE(dumper.dumpHisTrans("SHFE.rb2401", 20240115, &trx, 8));

	ASSERT_EQ(1U, bar_hits().size());
	// ExpDumper 把自己构造时的 _id 作为第一个实参透出（ExpDumper.cpp:76-80）
	EXPECT_EQ("r22_dump_a", bar_hits()[0]._id);
	EXPECT_EQ("SHFE.rb2401", bar_hits()[0]._code);
	EXPECT_EQ(4U, bar_hits()[0]._count);
	ASSERT_EQ(1U, tick_hits().size());
	EXPECT_EQ(20240115U, tick_hits()[0]._date);
	EXPECT_EQ(5U, tick_hits()[0]._count);
	EXPECT_EQ(6U, que_hits()[0]._count);
	EXPECT_EQ(7U, dtl_hits()[0]._count);
	EXPECT_EQ(8U, trx_hits()[0]._count);
}

TEST_F(R22ExtPorterTest, ExtDumperIdIsIndependentOfCreateExtDumperId)
{
	ExpDumper dumper("r22_dump_b");
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	register_extended_dumper(rec_bar, rec_tick);
	EXPECT_TRUE(dumper.dumpHisBars("SSE.600000", "d1", &bar, 1));
	ASSERT_EQ(1U, bar_hits().size());
	EXPECT_EQ("r22_dump_b", bar_hits()[0]._id);
	EXPECT_EQ("d1", bar_hits()[0]._period);
	// 未经 create_ext_dumper 登记 id 也能导出：槽位是全局的，与 id 无关
}

//////////////////////////////////////////////////////////////////////////
// 五、on_ext_parser_quote / parser_push_quote
TEST_F(R22ExtPorterTest, PushQuoteForUnknownParserLeavesTickUntouched)
{
	WtDtRunner runner;
	register_parser_callbacks(rec_evt, rec_sub);

	WTSTickStruct tick = make_tick("SHFE", "rb2401", 20240110, 20240110);
	WTSTickStruct origin = tick;

	runner.on_ext_parser_quote("r22_nope", &tick, 1);

	EXPECT_EQ(0, memcmp(&origin, &tick, sizeof(WTSTickStruct)));
	EXPECT_TRUE(evt_hits().empty());
	EXPECT_TRUE(sub_hits().empty());
}

TEST_F(R22ExtPorterTest, PushQuoteForKnownExtParserStopsAtZeroDates)
{
	WtDtRunner runner;
	register_parser_callbacks(rec_evt, rec_sub);
	ASSERT_TRUE(runner.createExtParser("r22_ext_q"));
	evt_hits().clear();

	WTSTickStruct zero = make_tick("SHFE", "rb2401", 0, 0);
	WTSTickStruct origin = zero;
	runner.on_ext_parser_quote("r22_ext_q", &zero, 1);
	// ParserAdapter::handleQuote 在 actiondate/tradingdate 为 0 时原样返回，不回写调用方缓冲
	EXPECT_EQ(0, memcmp(&origin, &zero, sizeof(WTSTickStruct)));
	EXPECT_TRUE(evt_hits().empty());
}

TEST_F(R22ExtPorterTest, PushQuoteAfterInitResolvesContractAndWrites)
{
	// 见 r22_runner_init_test.cpp 的同类用例；此处仅验证空 base data 下
	// 未知合约在 writeTick 之前返回（WtDtCore/ParserAdapter.cpp handleQuote 的 contract==NULL 支）
	WtDtRunner runner;
	register_parser_callbacks(rec_evt, rec_sub);
	ASSERT_TRUE(runner.createExtParser("r22_ext_c"));

	WTSTickStruct tick = make_tick("SHFE", "rb9999", 20240110, 20240110);
	runner.on_ext_parser_quote("r22_ext_c", &tick, 0);
	EXPECT_EQ(20240110U, tick.trading_date);
	EXPECT_EQ(0, strcmp(tick.code, "rb9999"));
}

TEST_F(R22ExtPorterTest, CApiCreateExtParserAndPushQuoteReachTheRegisteredSlots)
{
	register_parser_callbacks(rec_evt, rec_sub);

	ASSERT_TRUE(create_ext_parser("r22_capi_p"));
	ASSERT_EQ(1U, evt_hits().size());
	EXPECT_EQ(EVENT_PARSER_INIT, evt_hits()[0]._evt);
	EXPECT_EQ("r22_capi_p", evt_hits()[0]._id);

	WTSTickStruct tick = make_tick("SHFE", "rb2401", 0, 0);
	parser_push_quote("r22_capi_p", &tick, 0);
	EXPECT_EQ(0, tick.action_date);
	parser_push_quote("r22_capi_missing", &tick, 0);
	EXPECT_EQ(1U, evt_hits().size());
}
