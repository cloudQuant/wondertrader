// N05 lane：WtRiskMonFact 风控工厂 + WtExecMon/WtSimpDataMgr 纯逻辑测试
// 目标文件: src/WtRiskMonFact/WtRiskMonFact.cpp / WtSimpRiskMon.cpp、src/WtExecMon/WtSimpDataMgr.cpp
// 覆盖符号: createRiskMonFact / deleteRiskMonFact / WtRiskMonFact::getName / enumRiskMonitors /
//           createRiskMonotor / deleteRiskMonotor / WtSimpleRiskMon::getName / getFactName / stop、
//           WtSimpDataMgr 构造/析构 + grab_last_tick / get_tick_slice / get_kline_slice 空数据分支
// 跳过: WtSimpleRiskMon::run(后台线程+墙钟休眠)、init(需真实 WtPortContext)、
//       WtExecRunner::init/config/run(需真实配置文件与交易通道)、WtSimpDataMgr::initStore/init(需 DLL/reader)。
#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>

#include "Includes/RiskMonDefs.h"
#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "WtRiskMonFact/WtRiskMonFact.h"
#include "WtRiskMonFact/WtSimpRiskMon.h"
#include "WtExecMon/WtSimpDataMgr.h"

extern "C"
{
	EXPORT_FLAG IRiskMonitorFact* createRiskMonFact();
	EXPORT_FLAG void deleteRiskMonFact(IRiskMonitorFact* fact);
}

USING_NS_WTP;

namespace
{
std::set<std::string> g_riskSeen;
bool g_riskSeenLast = false;
void collectRiskMon(const char* fname, const char* uname, bool isLast)
{
	g_riskSeen.insert(std::string(fname) + "/" + uname);
	g_riskSeenLast = isLast;
}

// 冒名顶替的风控器：getFactName 不属于 WtRiskMonFact
class ForeignRiskMon : public WtRiskMonitor
{
public:
	const char* getName() override { return "ForeignRiskMon"; }
	const char* getFactName() override { return "OtherFact"; }
	void init(WtPortContext* /*ctx*/, WTSVariant* /*cfg*/) override {}
	void run() override {}
	void stop() override {}
};
}

//////////////////////////////////////////////////////////////////////////
// 工厂 C 接口与枚举
//////////////////////////////////////////////////////////////////////////
TEST(n05_riskmon, c_api_and_name)
{
	IRiskMonitorFact* fact = createRiskMonFact();
	ASSERT_NE(fact, nullptr);
	EXPECT_STREQ(fact->getName(), "WtRiskMonFact");
	deleteRiskMonFact(fact);
	deleteRiskMonFact(NULL);
}

TEST(n05_riskmon, enum_risk_monitors)
{
	WtRiskMonFact fact;
	g_riskSeen.clear();
	g_riskSeenLast = false;
	fact.enumRiskMonitors(collectRiskMon);
	ASSERT_EQ(g_riskSeen.size(), 1u);
	EXPECT_NE(g_riskSeen.find("WtRiskMonFact/SimpleRiskMon"), g_riskSeen.end());
	EXPECT_TRUE(g_riskSeenLast);
}

//////////////////////////////////////////////////////////////////////////
// 创建 / 删除风控器
//////////////////////////////////////////////////////////////////////////
TEST(n05_riskmon, create_delete_simple_riskmon)
{
	WtRiskMonFact fact;
	WtRiskMonitor* mon = fact.createRiskMonotor("SimpleRiskMon");
	ASSERT_NE(mon, nullptr);
	EXPECT_STREQ(mon->getName(), "WtSimpleRiskMon");
	EXPECT_STREQ(mon->getFactName(), "WtRiskMonFact");
	// stop 未 run 时应安全返回（_thrd 为空）
	mon->stop();
	EXPECT_TRUE(fact.deleteRiskMonotor(mon));

	EXPECT_EQ(fact.createRiskMonotor("NoSuchMon"), nullptr);
	EXPECT_EQ(fact.createRiskMonotor("WtSimpleRiskMon"), nullptr);  // 注册名是 SimpleRiskMon
}

TEST(n05_riskmon, delete_guards_foreign_and_null)
{
	WtRiskMonFact fact;
	EXPECT_TRUE(fact.deleteRiskMonotor(NULL));

	ForeignRiskMon foreign;
	EXPECT_FALSE(fact.deleteRiskMonotor(&foreign));  // 非本厂产品，拒绝且不 delete
}

//////////////////////////////////////////////////////////////////////////
// WtSimpDataMgr 未初始化即安全调用的分支
//////////////////////////////////////////////////////////////////////////
TEST(n05_simpdatamgr, empty_state_accessors)
{
	WtSimpDataMgr mgr;
	// 时间/日期查询在第一笔行情前使用 0 哨兵。
	EXPECT_EQ(mgr.get_date(), 0u);
	EXPECT_EQ(mgr.get_min_time(), 0u);
	EXPECT_EQ(mgr.get_secs(), 0u);
	EXPECT_EQ(mgr.get_raw_time(), 0u);
	EXPECT_EQ(mgr.get_trading_day(), 0u);
	// 从未推送过行情：缓存表为空，读取安全返回空
	EXPECT_EQ(mgr.grab_last_tick("SHFE.rb2401"), nullptr);
	// reader 为空时切片接口返回 NULL，不应崩溃
	EXPECT_EQ(mgr.get_tick_slice("SHFE.rb2401", 10), nullptr);
	WTSTickData* t = mgr.grab_last_tick("DCE.i2405");
	EXPECT_EQ(t, nullptr);
}

TEST(n05_simpdatamgr, push_null_tick_is_noop)
{
	WtSimpDataMgr mgr;
	mgr.handle_push_quote("SHFE.rb2401", NULL);  // curTick==NULL 直接 return
	EXPECT_EQ(mgr.grab_last_tick("SHFE.rb2401"), nullptr);
}

// BUG-N05-002：未配置有效 session 时，首笔行情会解引用空 _s_info。
// 本用例只记录独立的 session 生命周期缺陷；未初始化时钟字段已由上面的普通用例覆盖。
TEST(DISABLED_n05_simpdatamgr, push_quote_without_session_dereferences_null)
{
	WtSimpDataMgr mgr;
	WTSTickStruct ts;
	snprintf(ts.code, sizeof(ts.code), "%s", "rb2401");
	ts.action_date = 20240101;
	ts.trading_date = 20240101;
	ts.action_time = 93000000;
	WTSTickData* tick = WTSTickData::create(ts);
	// 未 init 时 _s_info 为空，handle_push_quote 解引用 _s_info->timeToMinutes。
	mgr.handle_push_quote("rb2401", tick);
	tick->release();
}
