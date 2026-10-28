/*!
 * \file n06_sel_uft_hft_test.cpp
 * \brief lane N06: WtSelStraFact / WtUftStraFact / WtHftStraFact 工厂查找 +
 *        WtStraDtSel / WtUftStraDemo / WtHftStraDemo 的 init 参数边界
 *
 * 依据源码（本轮已读）：
 * - WtSelStraFact.cpp L69: createStrategy 匹配 "DualThrustSelection" → new WtStraDtSel
 * - WtUftStraFact.cpp L100: 匹配 "SimpleUft" → new WtUftStraDemo
 * - WtHftStraFact.cpp L91: 匹配 "SimpleHft" → new WtHftStraDemo
 * - WtStraDtSel::init (WtStraDtSel.cpp L70)：NULL 检查后读取 days/k1/k2/period/count/stock/codes，
 *   codes 用 StrUtil::split(",") 分入 _codes 集合
 * - WtUftStraDemo::init (L78) / WtHftStraDemo::init (L77)：不判 cfg==NULL 直接取参（缺陷类，
 *   归入本文件末尾的 DISABLED_ 用例）
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "Includes/WTSVariant.hpp"
#include "Includes/CtaStrategyDefs.h"
#include "Includes/SelStrategyDefs.h"
#include "Includes/UftStrategyDefs.h"
#include "Includes/HftStrategyDefs.h"
#include "WtSelStraFact/WtSelStraFact.h"
#include "WtSelStraFact/WtStraDtSel.h"
#include "WtUftStraFact/WtUftStraFact.h"
#include "WtUftStraFact/WtUftStraDemo.h"
#include "WtHftStraFact/WtHftStraFact.h"
#include "WtHftStraFact/WtHftStraDemo.h"

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

struct EnumLog
{
	std::string fact;
	std::string stra;
	bool isLast;
};
std::vector<EnumLog>& g_enum_logs()
{
	static std::vector<EnumLog> logs;
	return logs;
}
void sel_enum_cb(const char* f, const char* s, bool last)
{
	g_enum_logs().push_back(EnumLog{ f == NULL ? "" : f, s == NULL ? "" : s, last });
}

std::vector<std::string>*& g_uft_enum_out()
{
	static std::vector<std::string>* out = NULL;
	return out;
}
void uft_enum_cb(const char* f, const char* s, bool last)
{
	if (g_uft_enum_out() != NULL)
		g_uft_enum_out()->push_back(std::string(f == NULL ? "" : f) + "/" +
			(s == NULL ? "" : s) + (last ? ":last" : ""));
}

WTSVariant* makeSelCfg(const char* codes)
{
	WTSVariant* root = WTSVariant::createObject();
	root->append("days", (uint32_t)15);
	root->append("k1", 0.25);
	root->append("k2", 0.25);
	root->append("period", "d1");
	root->append("count", (uint32_t)30);
	root->append("stock", false);
	root->append("codes", codes);
	return root;
}

} // namespace

// ---------------------------------------------------------------- 选股工厂

TEST(n06_sel_fact, create_enum_delete_round_trip)
{
	WtSelStraFact fact;
	g_enum_logs().clear();
	fact.enumStrategy(sel_enum_cb);
	// WtSelStraFact 的 enumStrategy 恰好回调一个策略名（DualThrustSelection）
	ASSERT_EQ(1u, g_enum_logs().size());
	EXPECT_TRUE(g_enum_logs()[0].isLast);

	SelStrategy* stra = fact.createStrategy(g_enum_logs()[0].stra.c_str(), "sel1");
	ASSERT_NE(nullptr, stra);
	EXPECT_STREQ("DualThrustSelection", stra->getName());   // WtStraDtSel.cpp L49-51

	EXPECT_EQ(nullptr, fact.createStrategy("NoSuchSel", "x"));
	// deleteStrategy(NULL) → true（WtSelStraFact.cpp 同款实现）
	EXPECT_TRUE(fact.deleteStrategy(NULL));
	EXPECT_TRUE(fact.deleteStrategy(stra));
}

TEST(n06_sel_fact, get_name_is_factory_name)
{
	WtSelStraFact fact;
	// FACT_NAME 存在跨插件 TU 的 ODR 竞争（见 bugs），只断言非空且与策略一致
	EXPECT_NE(nullptr, fact.getName());
	EXPECT_GT(strlen(fact.getName()), 0u);
	WtStraDtSel sel("s0");
	EXPECT_STREQ(fact.getName(), sel.getFactName());
}

TEST(n06_sel_stra, init_null_and_cfg_bounds)
{
	WtStraDtSel sel("s1");
	// init 有 NULL 检查（WtStraDtSel.cpp L70-74）
	EXPECT_FALSE(sel.init(NULL));

	VarHolder empty(WTSVariant::createObject());
	// 空配置：codes 缺省为 ""，StrUtil::split 得到空/单项集合，不抛异常
	EXPECT_TRUE(sel.init(empty.get()));

	VarHolder cfg(makeSelCfg("SHFE.au.2005,SSE.600000,SSE.600000"));
	EXPECT_TRUE(sel.init(cfg.get()));

	VarHolder cfg2(makeSelCfg("NOSEP_CODE"));
	EXPECT_TRUE(sel.init(cfg2.get()));

	VarHolder cfg3(makeSelCfg(",,,,"));
	// 纯分隔符输入也不崩溃（split 得到空串集合）
	EXPECT_TRUE(sel.init(cfg3.get()));
}

// ---------------------------------------------------------------- UFT 工厂

TEST(n06_uft_fact, create_delete_and_enum)
{
	WtUftStraFact fact;
	UftStrategy* stra = fact.createStrategy("SimpleUft", "uft1");
	ASSERT_NE(nullptr, stra);
	EXPECT_NE(nullptr, stra->getName());
	EXPECT_GT(strlen(stra->getName()), 0u);

	// 未知名称返回 NULL（WtUftStraFact.cpp L100-110）
	EXPECT_EQ(nullptr, fact.createStrategy("Nope", "x"));
	EXPECT_TRUE(fact.deleteStrategy(NULL));
	EXPECT_TRUE(fact.deleteStrategy(stra));

	std::vector<std::string> seen;
	g_uft_enum_out() = &seen;
	fact.enumStrategy(uft_enum_cb);
	g_uft_enum_out() = NULL;
	// SimpleUft 必须在枚举结果里
	ASSERT_GE(seen.size(), 1u);
	bool found = false;
	for (const auto& s : seen)
		if (s.find("SimpleUft") != std::string::npos)
			found = true;
	EXPECT_TRUE(found);
}

TEST(n06_uft_stra, init_reads_params_and_on_entrust_unknown_id)
{
	WtUftStraDemo demo("u1");
	VarHolder cfg(WTSVariant::createObject());
	cfg->append("code", "SSE.600000");
	cfg->append("second", (uint32_t)3);
	cfg->append("freq", (uint32_t)200);
	cfg->append("offset", (uint32_t)2);
	cfg->append("lots", 1.0);
	// init 无 NULL 校验（缺陷类收入 DISABLED_），正常配置恒返回 true
	EXPECT_TRUE(demo.init(cfg.get()));
	EXPECT_NE(nullptr, demo.getName());
	EXPECT_GT(strlen(demo.getName()), 0u);

	// on_entrust(false) 对不存在的 localid 是无操作，不应崩溃（WtUftStraDemo.cpp L100-113）
	demo.on_entrust(12345, false, "mock fail");
	demo.on_entrust(12345, true, "mock ok");
}

// ---------------------------------------------------------------- HFT 工厂

TEST(n06_hft_fact, create_delete_and_enum)
{
	WtHftStraFact fact;
	HftStrategy* stra = fact.createStrategy("SimpleHft", "hft1");
	ASSERT_NE(nullptr, stra);
	EXPECT_NE(nullptr, stra->getName());
	EXPECT_GT(strlen(stra->getName()), 0u);
	EXPECT_EQ(nullptr, fact.createStrategy("Nope", "x"));
	EXPECT_TRUE(fact.deleteStrategy(NULL));
	EXPECT_TRUE(fact.deleteStrategy(stra));
}

TEST(n06_hft_stra, init_reads_params_and_empty_entrust)
{
	WtHftStraDemo demo("h1");
	VarHolder cfg(WTSVariant::createObject());
	cfg->append("code", "SSE.600000");
	cfg->append("second", (uint32_t)3);
	cfg->append("freq", (uint32_t)200);
	cfg->append("offset", (uint32_t)2);
	cfg->append("reserve", 1000.0);
	cfg->append("stock", true);
	EXPECT_TRUE(demo.init(cfg.get()));
	// stock=true 时 _unit=100（WtHftStraDemo.cpp L84-88）；无外部可见 getter，只验证不崩溃
	demo.on_entrust(9, false, "msg", "tag");
}

// ------------------------------------------------------- 崩溃缺陷类（默认禁用）
// 五个工厂的 createStrategy 均直接 strcmp(name,...) 不判空；
// WtUftStraDemo/WtHftStraDemo::init 不判 cfg==NULL。
// 所有同类输入统一收进本用例；确认修复前保持 DISABLED。
TEST(n06_sel_uft_hft, DISABLED_NULL_NAME_AND_NULL_CFG_CRASH_CLASS)
{
	WtSelStraFact sf;
	WtUftStraFact uf;
	WtHftStraFact hf;
	(void)sf.createStrategy(NULL, "c1");   // strcmp(NULL) UB
	(void)uf.createStrategy(NULL, "c2");
	(void)hf.createStrategy(NULL, "c3");

	WtUftStraDemo ud("c4");
	(void)ud.init(NULL);                   // cfg->getCString(NULL 解引用) UB
	WtHftStraDemo hd("c5");
	(void)hd.init(NULL);
}
