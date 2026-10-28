/*!
 * \file n06_cta_stra_test.cpp
 * \brief lane N06: CTA 策略(WtStraTwoMa / WtStraDualThrust)与 CTA 工厂的参数边界与查找逻辑
 *
 * 说明：
 * - WtCtaStraFact 与 WtCtaTwoMaFact 两个插件 TU 都在 CoverageTests 里定义了同名类 WtStraFact、
 *   同名全局 FACT_NAME 与 extern "C" createStrategyFact（ODR/重复符号风险已在 manifest 上报）。
 *   因此本文件对“工厂名/策略名”只做不依赖链接选择的宽松断言（见 bugs/n06 待审核条目）。
 * - 符号名全部来自本轮读取的源码：
 *   WtCtaTwoMaFact.cpp L17 FACT_NAME/WtStraFact::createStrategy("TwoMa")、
 *   WtCtaStraFact.cpp L17/L90 ("DualThrust")、WtStraTwoMa::init (WtStraTwoMa.cpp L79)、
 *   WtStraDualThrust::init (WtStraDualThrust.cpp L79)。
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Includes/WTSVariant.hpp"
#include "Includes/CtaStrategyDefs.h"
#include "WtCtaTwoMaFact/WtCtaTwoMaFact.h"
#include "WtCtaTwoMaFact/WtStraTwoMa.h"
#include "WtCtaStraFact/WtStraDualThrust.h"

USING_NS_WTP;

namespace {

class VarHolder
{
public:
	explicit VarHolder(WTSVariant* v = NULL) : _v(v) {}
	~VarHolder() { reset(); }
	VarHolder(const VarHolder&) = delete;
	VarHolder& operator=(const VarHolder&) = delete;
	void reset() { if (_v != NULL) { _v->release(); _v = NULL; } }
	WTSVariant* get() const { return _v; }
	WTSVariant* operator->() const { return _v; }
private:
	WTSVariant* _v;
};

WTSVariant* makeFullCtaCfg()
{
	WTSVariant* root = WTSVariant::createObject();
	root->append("days", (uint32_t)5);
	root->append("k1", 0.5);
	root->append("k2", 0.5);
	root->append("period", std::string("d1").c_str());
	root->append("count", (uint32_t)20);
	root->append("code", "SHFE.au.2005");
	root->append("stock", false);
	return root;
}

std::vector<std::string>& enum_result()
{
	// enumStrategy 的回调是 C 函数指针，测试里用静态变量收集
	static std::vector<std::string> ret;
	return ret;
}
void collect_enum_cb(const char* factName, const char* straName, bool isLast)
{
	enum_result().push_back(std::string(factName == NULL ? "<null>" : factName) + "/" +
		(straName == NULL ? "<null>" : straName) + (isLast ? ":last" : ""));
}

} // namespace

// ---------------------------------------------------------------- 策略名与 init 边界

TEST(n06_cta_stra, strategy_names_are_stable)
{
	WtStraTwoMa two_ma("id1");
	WtStraDualThrust dt("id2");
	// 名称在各自 .cpp 里硬编码：WtStraTwoMa::getName()="TwoMa"、WtStraDualThrust="DualThrust"
	EXPECT_STREQ("TwoMa", two_ma.getName());
	EXPECT_STREQ("DualThrust", dt.getName());
	// getFactName() 返回外部全局 FACT_NAME（存在 ODR 竞争，见 bugs），只断言非空
	EXPECT_NE(nullptr, two_ma.getFactName());
	EXPECT_GT(strlen(two_ma.getFactName()), 0u);
	EXPECT_GT(strlen(dt.getFactName()), 0u);
}

TEST(n06_cta_stra, init_null_cfg_returns_false)
{
	WtStraTwoMa two_ma("id1");
	WtStraDualThrust dt("id2");
	// 两个 CTA 策略都显式做了 NULL 检查（WtStraTwoMa.cpp L81、WtStraDualThrust.cpp L82）
	EXPECT_FALSE(two_ma.init(NULL));
	EXPECT_FALSE(dt.init(NULL));
}

TEST(n06_cta_stra, init_full_cfg_round_trip)
{
	WtStraTwoMa two_ma("id1");
	WtStraDualThrust dt("id2");
	VarHolder cfg(makeFullCtaCfg());
	// 空字符串键取回 ""（getCString 默认值语义）
	EXPECT_TRUE(dt.init(cfg.get()));
	// days=5 等参数不会导致 init 失败（DualThrust 只读参数不做合法性校验）
	VarHolder cfg2(makeFullCtaCfg());
	cfg2->append("short_days", (uint32_t)3);
	cfg2->append("long_days", (uint32_t)8);
	EXPECT_TRUE(two_ma.init(cfg2.get()));
}

TEST(n06_cta_stra, init_missing_keys_fall_back_to_defaults)
{
	WtStraTwoMa two_ma("id1");
	VarHolder cfg(WTSVariant::createObject());
	// 空配置：getUInt32/getDouble/getCString/getBoolean 全部走默认值 0/0.0/""/false
	// TwoMa 的 init 不校验参数，直接返回 true（WtStraTwoMa.cpp L79-101）
	EXPECT_TRUE(two_ma.init(cfg.get()));
	EXPECT_TRUE(two_ma.init(NULL) == false);

	WtStraDualThrust dt("id3");
	VarHolder cfg2(WTSVariant::createObject());
	EXPECT_TRUE(dt.init(cfg2.get()));
}

// ---------------------------------------------------------------- 工厂查找与删除

TEST(n06_cta_fact, create_strategy_unknown_name_returns_null)
{
	WtStraFact fact;
	// 无论链接到哪个工厂实现，未知名称都必须返回 NULL
	EXPECT_EQ(nullptr, fact.createStrategy("NoSuchStrategy", "id1"));
	// "TwoMa" 与 "DualThrust" 至多命中其中一个（两个 TU 的 WtStraFact ODR 竞争，见 bugs）
	CtaStrategy* a = fact.createStrategy("TwoMa", "idA");
	CtaStrategy* b = fact.createStrategy("DualThrust", "idB");
	int hits = (a != NULL) + (b != NULL);
	EXPECT_EQ(1, hits) << "linker 只会让其中一个 createStrategy 实现生效";
	if (a != NULL) EXPECT_STREQ("TwoMa", a->getName());
	if (b != NULL) EXPECT_STREQ("DualThrust", b->getName());
	if (a != NULL) EXPECT_TRUE(fact.deleteStrategy(a));
	if (b != NULL) EXPECT_TRUE(fact.deleteStrategy(b));
}

TEST(n06_cta_fact, delete_null_strategy_returns_true)
{
	WtStraFact fact;
	// deleteStrategy(NULL) 直接返回 true（WtCtaTwoMaFact.cpp L105-118）
	EXPECT_TRUE(fact.deleteStrategy(NULL));
}

TEST(n06_cta_fact, factory_get_name_non_empty)
{
	WtStraFact fact;
	const char* name = fact.getName();
	EXPECT_NE(nullptr, name);
	// FACT_NAME 可能是 "WtCtaStraFact" 或 "WtCtaTwoMaFact"（ODR 竞争），不断言具体值
	// 但策略自身的 getName 必须与之一致：deleteStrategy 用 getFactName() 与 FACT_NAME 比较
	WtStraTwoMa two_ma("id1");
	EXPECT_STREQ(fact.getName(), two_ma.getFactName());
}

TEST(n06_cta_fact, enum_strategy_reports_one_name)
{
	WtStraFact fact;
	enum_result().clear();
	fact.enumStrategy(collect_enum_cb);
	// 两个实现都恰好回调一次（cb(FACT_NAME, "TwoMa"/"DualThrust", true)）
	ASSERT_EQ(1u, enum_result().size());
	EXPECT_NE(std::string::npos, enum_result()[0].find(":last"));
	// 回调里出现的策略名必须能被 createStrategy 找回
	auto pos = enum_result()[0].find('/');
	std::string straName = enum_result()[0].substr(pos + 1, enum_result()[0].size() - 5 - pos);
	CtaStrategy* stra = fact.createStrategy(straName.c_str(), "e1");
	ASSERT_NE(nullptr, stra);
	EXPECT_STREQ(straName.c_str(), stra->getName());
	EXPECT_TRUE(fact.deleteStrategy(stra));
}

// ------------------------------------------------------- 整类崩溃输入（缺陷类覆盖）
// createStrategy 五个工厂都直接 strcmp(name, ...) 而不判空：name==NULL 是未定义行为
// （段错误），WtUftStraDemo/WtHftStraDemo::init 也不判 cfg==NULL。
// 缺陷类的所有成员统一放进本 DISABLED_ 用例，默认不运行。
TEST(n06_cta_fact, DISABLED_NULL_NAME_AND_NULL_CFG_CRASH_CLASS)
{
	WtStraFact cta_fact;
	(void)cta_fact.createStrategy(NULL, "crash1");        // strcmp(NULL) UB
	(void)cta_fact.createStrategy(NULL, "crash2");
}
