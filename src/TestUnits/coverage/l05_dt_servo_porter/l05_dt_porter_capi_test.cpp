/*
 * L05 - WtDtPorter 导出的 C API 与扩展 Dumper 委托路径
 *
 * 覆盖 src/WtDtPorter/WtDtPorter.cpp（write_log 两个分支、parser_push_quote、
 * create_ext_dumper、register_extended_dumper、register_extended_hftdata_dumper）、
 * src/WtDtPorter/WtDtRunner.cpp（on_ext_parser_quote 未找到适配器分支、
 * dumpHisBars/Ticks/OrdQue/OrdDtl/Trans 的“未注册回调”与“已注册回调”分支）、
 * src/WtDtPorter/ExpDumper.h（5 个 dumpHis* 委托）。
 *
 * 只读生产代码；缺陷记录见 docs/迭代计划/迭代01/bugs/l05_dt_servo_porter.md。
 */
#include <gtest/gtest.h>

#include "WtDtPorter/WtDtPorter.h"
#include "WtDtPorter/ExpDumper.h"
#include "Includes/WTSDataDef.hpp"

#include <cstring>

USING_NS_WTP;

namespace {

	uint32_t& dumper_calls()
	{
		static uint32_t s_calls = 0;
		return s_calls;
	}

	bool PORTER_FLAG dump_bars_cb(const char* id, const char* stdCode, const char* period, WTSBarStruct* bars, WtUInt32 count)
	{
		dumper_calls() += 1;
		return id != NULL && stdCode != NULL && period != NULL && bars != NULL && count > 0;
	}

	bool PORTER_FLAG dump_ticks_cb(const char* id, const char* stdCode, WtUInt32 uDate, WTSTickStruct* ticks, WtUInt32 count)
	{
		dumper_calls() += 1;
		return id != NULL && stdCode != NULL && ticks != NULL && count > 0 && uDate > 0;
	}

	bool PORTER_FLAG dump_ordque_cb(const char* id, const char* stdCode, WtUInt32 uDate, WTSOrdQueStruct* items, WtUInt32 count)
	{
		dumper_calls() += 1;
		return id != NULL && stdCode != NULL && items != NULL && count > 0 && uDate > 0;
	}

	bool PORTER_FLAG dump_orddtl_cb(const char* id, const char* stdCode, WtUInt32 uDate, WTSOrdDtlStruct* items, WtUInt32 count)
	{
		dumper_calls() += 1;
		return id != NULL && stdCode != NULL && items != NULL && count > 0 && uDate > 0;
	}

	bool PORTER_FLAG dump_trans_cb(const char* id, const char* stdCode, WtUInt32 uDate, WTSTransStruct* items, WtUInt32 count)
	{
		dumper_calls() += 1;
		return id != NULL && stdCode != NULL && items != NULL && count > 0 && uDate > 0;
	}

	WTSBarStruct make_bar(uint32_t date, double price)
	{
		WTSBarStruct bar;
		memset(&bar, 0, sizeof(WTSBarStruct));
		bar.date = date;
		bar.open = price;
		bar.high = price;
		bar.low = price;
		bar.close = price;
		return bar;
	}

} // namespace

class L05PorterCapiTest : public ::testing::Test
{
protected:
	void SetUp() override { dumper_calls() = 0; }
};

TEST_F(L05PorterCapiTest, WriteLogWithAndWithoutCategory)
{
	// 分类日志与默认日志两条分支；NULL 分类会走 strlen(NULL) —— 见缺陷 L05-005，不在测试中触发
	write_log(1, "l05 message with category", "parser");
	write_log(2, "l05 message without category", "");
	SUCCEED();
}

TEST_F(L05PorterCapiTest, PushQuoteForUnknownParserIsIgnored)
{
	WTSTickStruct tick;
	memset(&tick, 0, sizeof(WTSTickStruct));
	strncpy(tick.exchg, "SHFE", sizeof(tick.exchg) - 1);
	strncpy(tick.code, "rb2010", sizeof(tick.code) - 1);
	tick.price = 3000;
	// 适配器不存在：只输出 warn 日志，不崩溃
	parser_push_quote("l05_not_created_parser", &tick, 0);
	SUCCEED();
}

TEST_F(L05PorterCapiTest, RegisterParserCallbacksAcceptsNull)
{
	register_parser_callbacks(NULL, NULL);
	register_parser_callbacks((FuncParserEvtCallback)NULL, (FuncParserSubCallback)NULL);
	SUCCEED();
}

TEST_F(L05PorterCapiTest, DumpersWithoutRegistrationReturnFalse)
{
	register_extended_dumper(NULL, NULL);
	register_extended_hftdata_dumper(NULL, NULL, NULL);

	ExpDumper dumper("l05ext");
	WTSBarStruct bar = make_bar(20200116, 3000);
	EXPECT_FALSE(dumper.dumpHisBars("SHFE.rb2010", "m1", &bar, 1));

	WTSTickStruct tick;
	memset(&tick, 0, sizeof(WTSTickStruct));
	EXPECT_FALSE(dumper.dumpHisTicks("SHFE.rb2010", 20200116, &tick, 1));

	WTSOrdQueStruct que;
	memset(&que, 0, sizeof(WTSOrdQueStruct));
	EXPECT_FALSE(dumper.dumpHisOrdQue("SHFE.rb2010", 20200116, &que, 1));

	WTSOrdDtlStruct dtl;
	memset(&dtl, 0, sizeof(WTSOrdDtlStruct));
	EXPECT_FALSE(dumper.dumpHisOrdDtl("SHFE.rb2010", 20200116, &dtl, 1));

	WTSTransStruct trans;
	memset(&trans, 0, sizeof(WTSTransStruct));
	EXPECT_FALSE(dumper.dumpHisTrans("SHFE.rb2010", 20200116, &trans, 1));

	EXPECT_EQ(dumper_calls(), 0u);
}

TEST_F(L05PorterCapiTest, DumpersWithRegistrationDelegateToCallbacks)
{
	register_extended_dumper(dump_bars_cb, dump_ticks_cb);
	register_extended_hftdata_dumper(dump_ordque_cb, dump_orddtl_cb, dump_trans_cb);

	ExpDumper dumper("l05ext");
	WTSBarStruct bar = make_bar(20200116, 3000);
	EXPECT_TRUE(dumper.dumpHisBars("SHFE.rb2010", "m1", &bar, 1));

	WTSTickStruct tick;
	memset(&tick, 0, sizeof(WTSTickStruct));
	tick.price = 3000;
	EXPECT_TRUE(dumper.dumpHisTicks("SHFE.rb2010", 20200116, &tick, 1));

	WTSOrdQueStruct que;
	memset(&que, 0, sizeof(WTSOrdQueStruct));
	EXPECT_TRUE(dumper.dumpHisOrdQue("SHFE.rb2010", 20200116, &que, 2));

	WTSOrdDtlStruct dtl;
	memset(&dtl, 0, sizeof(WTSOrdDtlStruct));
	EXPECT_TRUE(dumper.dumpHisOrdDtl("SHFE.rb2010", 20200116, &dtl, 3));

	WTSTransStruct trans;
	memset(&trans, 0, sizeof(WTSTransStruct));
	EXPECT_TRUE(dumper.dumpHisTrans("SHFE.rb2010", 20200116, &trans, 4));

	EXPECT_EQ(dumper_calls(), 5u);

	// 复原为空注册，避免影响其它用例
	register_extended_dumper(NULL, NULL);
	register_extended_hftdata_dumper(NULL, NULL, NULL);
}

TEST_F(L05PorterCapiTest, CreateExtDumperRegistersById)
{
	EXPECT_TRUE(create_ext_dumper("l05dumper"));
	EXPECT_TRUE(create_ext_dumper("l05dumper"));
}
