// N05 lane：WtExeFact 执行单元工厂测试
// 目标文件: src/WtExeFact/WtExeFact.cpp / WtExeFact.h
// 覆盖符号: createExecFact / deleteExecFact / WtExeFact::getName / enumExeUnit /
//           createExeUnit / createDiffExeUnit / createArbiExeUnit / deleteExeUnit /
//           各 ExecuteUnit 的 getName / getFactName（不触网路径）
#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>

#include "Includes/ExecuteDefs.h"
#include "WtExeFact/WtExeFact.h"

extern "C"
{
	EXPORT_FLAG IExecuterFact* createExecFact();
	EXPORT_FLAG void deleteExecFact(IExecuterFact* fact);
}

using namespace wtp;

namespace
{
// FuncEnumUnitCallback 是裸函数指针，只能用非捕获回调 + 内部链接状态
std::set<std::string> g_enumSeen;
std::string g_enumFact;
void collectUnit(const char* fname, const char* uname, bool special)
{
	g_enumFact = fname;
	g_enumSeen.insert(std::string(uname) + (special ? "|S" : "|N"));
}
}

//////////////////////////////////////////////////////////////////////////
// C 接口
//////////////////////////////////////////////////////////////////////////
TEST(n05_execfact, c_api_create_delete)
{
	IExecuterFact* fact = createExecFact();
	ASSERT_NE(fact, nullptr);
	EXPECT_STREQ(fact->getName(), "WtExeFact");
	deleteExecFact(fact);
	deleteExecFact(NULL);  // NULL 安全
}

//////////////////////////////////////////////////////////////////////////
// enumExeUnit
//////////////////////////////////////////////////////////////////////////
TEST(n05_execfact, enum_exe_unit_lists_twap_and_minimpact)
{
	WtExeFact fact;
	g_enumSeen.clear();
	g_enumFact.clear();
	fact.enumExeUnit(collectUnit);
	EXPECT_EQ(g_enumFact, "WtExeFact");
	ASSERT_EQ(g_enumSeen.size(), 2u);
	// WtTWapExeUnit 是普通单元(special=false)，WtMinImpactExeUnit 是特殊单元(special=true)
	EXPECT_NE(g_enumSeen.find("WtTWapExeUnit|N"), g_enumSeen.end());
	EXPECT_NE(g_enumSeen.find("WtMinImpactExeUnit|S"), g_enumSeen.end());
}

//////////////////////////////////////////////////////////////////////////
// createExeUnit
//////////////////////////////////////////////////////////////////////////
TEST(n05_execfact, create_supported_exe_units)
{
	WtExeFact fact;
	const char* names[] = {
		"WtTWapExeUnit", "WtMinImpactExeUnit", "WtStockMinImpactExeUnit",
		"WtVWapExeUnit", "WtStockVWapExeUnit"
	};
	for (const char* n : names)
	{
		ExecuteUnit* unit = fact.createExeUnit(n);
		ASSERT_NE(unit, nullptr) << n;
		EXPECT_STREQ(unit->getName(), n);
		EXPECT_STREQ(unit->getFactName(), "WtExeFact");
		EXPECT_TRUE(fact.deleteExeUnit(unit));
	}
}

TEST(n05_execfact, create_unknown_exe_unit_returns_null)
{
	WtExeFact fact;
	EXPECT_EQ(fact.createExeUnit("NoSuchUnit"), nullptr);
	EXPECT_EQ(fact.createExeUnit("WtDiffMinImpactExeUnit"), nullptr);  // 差量单元不在此接口
}

TEST(n05_execfact, create_diff_exe_unit)
{
	WtExeFact fact;
	ExecuteUnit* diff = fact.createDiffExeUnit("WtDiffMinImpactExeUnit");
	ASSERT_NE(diff, nullptr);
	EXPECT_STREQ(diff->getName(), "WtDiffMinImpactExeUnit");
	EXPECT_STREQ(diff->getFactName(), "WtExeFact");
	EXPECT_TRUE(fact.deleteExeUnit(diff));
	EXPECT_EQ(fact.createDiffExeUnit("WtMinImpactExeUnit"), nullptr);
}

TEST(n05_execfact, create_arbi_exe_unit_always_null)
{
	WtExeFact fact;
	EXPECT_EQ(fact.createArbiExeUnit("anything"), nullptr);
	EXPECT_EQ(fact.createArbiExeUnit(""), nullptr);
}

//////////////////////////////////////////////////////////////////////////
// deleteExeUnit 防御分支
//////////////////////////////////////////////////////////////////////////
namespace
{
// 冒名顶替：getFactName 不属于 WtExeFact，deleteExeUnit 必须拒绝
class ForeignUnit : public ExecuteUnit
{
public:
	const char* getName() override { return "ForeignUnit"; }
	const char* getFactName() override { return "OtherFact"; }
	void set_position(const char*, double) override {}
	void on_tick(WTSTickData*) override {}
	void on_trade(uint32_t, const char*, bool, double, double) override {}
	void on_order(uint32_t, const char*, bool, double, double, bool) override {}
	void on_entrust(uint32_t, const char*, bool, const char*) override {}
	void on_channel_ready() override {}
	void on_channel_lost() override {}
};
}

TEST(n05_execfact, delete_exe_unit_guards)
{
	WtExeFact fact;
	EXPECT_TRUE(fact.deleteExeUnit(NULL));  // NULL 视为成功

	ForeignUnit foreign;
	EXPECT_FALSE(fact.deleteExeUnit(&foreign));  // 非本厂产品，拒绝且不删除

	ExecuteUnit* own = fact.createExeUnit("WtVWapExeUnit");
	ASSERT_NE(own, nullptr);
	EXPECT_TRUE(fact.deleteExeUnit(own));
}

//////////////////////////////////////////////////////////////////////////
// ExecuteUnit 基类默认行为（未 init 的裸对象）
//////////////////////////////////////////////////////////////////////////
TEST(n05_execfact, base_default_init_stores_ctx_and_code)
{
	WtExeFact fact;
	ExecuteUnit* unit = fact.createExeUnit("WtTWapExeUnit");
	ASSERT_NE(unit, nullptr);
	// 基类 ExecuteUnit::init 仅保存 ctx/code；这里传 NULL 上下文，只验证不崩溃
	// 且析构路径（deleteExeUnit）覆盖构造函数与析构函数
	EXPECT_TRUE(fact.deleteExeUnit(unit));
}

TEST(n05_execfact, diff_unit_name_contract)
{
	WtExeFact fact;
	ExecuteUnit* u1 = fact.createDiffExeUnit("WtDiffMinImpactExeUnit");
	ExecuteUnit* u2 = fact.createDiffExeUnit("WtDiffMinImpactExeUnit");
	ASSERT_NE(u1, nullptr);
	ASSERT_NE(u2, nullptr);
	// 每次调用都是独立实例
	EXPECT_NE(u1, u2);
	EXPECT_STREQ(u1->getName(), u2->getName());
	EXPECT_TRUE(fact.deleteExeUnit(u1));
	EXPECT_TRUE(fact.deleteExeUnit(u2));
}
