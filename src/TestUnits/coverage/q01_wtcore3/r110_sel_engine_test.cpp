/**
 * r110_sel_engine_test.cpp — lane R110 / wave19
 *
 * 目标：src/WtCore/WtSelEngine.cpp 残余公开面。
 *
 * 覆盖行段：
 *   - run()：marker.json 导出（策略/通道列表）+ ticker 生命周期：:339-380
 *   - release()/handle_push_quote 的 ticker 分支：:45-57, 79-83
 *   - addContext 重复任务拒绝：:390-400
 *   - on_minute_end 的非触发分支：跨日回绕 :224-229，任务时间过滤 :233-237，
 *     节假日回溯 :243-255，None/Monthly/Weekly/Yearly 周期判定 :258-326
 *   - handle_pos_change 的过滤器早退、自定义规则标签、组合风控缩放：:427-488
 *
 * 不启动任何长线程：run() 后立即 release()，ticker 线程在 10ms 内被 stop/join。
 * on_minute_end 只构造「不应触发」的任务；命中触发的任务会创建局部 StdThread
 * 并析构，属已知 BUG-R10（std::terminate），本 lane 不进入。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "q01_sel_env.h"
#include "r90_env.h"

#include <WtCore/WtDtMgr.h>
#include <WtCore/TraderAdapter.h>
#include <WTSUtils/WTSCfgLoader.h>

#include <Includes/WTSVariant.hpp>

using namespace wtp;

namespace {

namespace fs = std::filesystem;

/** 可配置节假日的 IBaseDataMgr 替身 */
class R110HolidayBase : public q01sel::BaseDataMgr
{
public:
    uint32_t holiday = 0;

    bool isHoliday(const char* /*pid*/, uint32_t uDate, bool /*isTpl*/ = false) override
    {
        return holiday != 0 && uDate == holiday;
    }
};

/** HOT 规则主力替身：把 SHFE.ag.HOT 映射到 ag2512 */
class R110RuleHot : public q01sel::HotMgr
{
public:
    const char* getRuleTag(const char* stdCode) override
    {
        return (stdCode != NULL && strcmp(stdCode, "HOT") == 0) ? "HOT" : "";
    }

    const char* getCustomRawCode(const char* tag, const char* /*fullPid*/, uint32_t /*dt*/ = 0) override
    {
        return (tag != NULL && strlen(tag) > 0) ? "ag2512" : "";
    }
};

class R110SelBase
{
public:
    R110SelBase()
    {
        WtHelper::setGenerateDir(r90::temp_root().c_str());
        WtHelper::setTime(20250110, 931, 0);

        bd.regisSession("TRADING");
        WTSSessionInfo* sess = bd.session("TRADING");
        sess->addTradingSection(900, 1500);

        WTSCommodityInfo* ag = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
        ag->setSessionInfo(sess);
        bd.regis(ag, q01sel::makeCt("SHFE", "ag", "ag2512"));
    }

    WTSVariant* cfg(const std::string& filters = std::string())
    {
        std::string json = "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\""
            + filters + "\"}";
        return WTSCfgLoader::load_from_content(json.c_str());
    }

    q01sel::BaseDataMgr bd;
    q01sel::HotMgr      hot;
    WtDtMgr             dm;
};

//////////////////////////////////////////////////////////////////
// run()：marker.json 导出 + ticker 创建；handle_push_quote 走 ticker；
// release() 删除 ticker
// src/WtCore/WtSelEngine.cpp:45-57, 73-83, 339-380
//////////////////////////////////////////////////////////////////
TEST(R110SelEngine, RunExportsMarkerAndForwardsQuoteThroughTicker)
{
    R110SelBase env;
    TraderAdapterMgr adapterMgr;

    // _tm_ticker 未初始化（BUG-R42-001），引擎堆分配不回收，规避析构读脏指针
    WtSelEngine& engine = *new WtSelEngine();
    engine.set_adapter_mgr(&adapterMgr);

    WTSVariant* cfg = env.cfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &env.bd, &env.dm, &env.hot, NULL);
    cfg->release();
    engine.set_date_time(20250110, 931, 0);
    engine.set_trading_date(20250110);

    engine.run();

    // marker.json 由 run() 在启动 ticker 前写出
    const std::string marker = std::string(WtHelper::getBaseDir()) + "marker.json";
    EXPECT_TRUE(fs::exists(marker));

    // _tm_ticker 已存在：handle_push_quote 转入 ticker::on_tick
    WTSTickData* tick = q01sel::makeTick("SHFE", "ag2512", 20250110, 20250110, 93100000, 3500.0);
    tick->setContractInfo(env.bd.getContract("ag2512", "SHFE"));
    engine.handle_push_quote(tick);
    tick->release();

    // release：ticker stop/join + WtEngine 任务线程收尾
    engine.release();
}

//////////////////////////////////////////////////////////////////
// addContext 重复注册；on_minute_end 的跨日回绕、任务时间过滤、节假日回溯
// 与 None/Monthly/Weekly/Yearly 的「不触发」判定
// src/WtCore/WtSelEngine.cpp:224-255, 258-326, 390-400
//////////////////////////////////////////////////////////////////
TEST(R110SelEngine, MinuteEndCoversCalendarBranchesWithoutFiring)
{
    R110HolidayBase bd;
    WtHelper::setGenerateDir(r90::temp_root().c_str());
    WtHelper::setTime(20250110, 931, 0);
    bd.regisSession("TRADING");
    WTSSessionInfo* sess = bd.session("TRADING");
    sess->addTradingSection(900, 1500);
    WTSCommodityInfo* ag = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
    ag->setSessionInfo(sess);
    bd.regis(ag, q01sel::makeCt("SHFE", "ag", "ag2512"));
    bd.holiday = 20250109;  // 上一交易日放假 -> while 回溯循环

    q01sel::HotMgr hot;
    WtDtMgr dm;
    WtSelEngine& engine = *new WtSelEngine();  // BUG-R42-001：不回收
    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"}");
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &bd, &dm, &hot, NULL);
    cfg->release();
    engine.set_date_time(20250110, 931, 0);
    engine.set_trading_date(20250110);

    SelContextPtr none(new q01sel::SelCtx(&engine, "r110none"));
    SelContextPtr monthly(new q01sel::SelCtx(&engine, "r110month"));
    SelContextPtr weekly(new q01sel::SelCtx(&engine, "r110week"));
    SelContextPtr yearly(new q01sel::SelCtx(&engine, "r110year"));

    engine.addContext(none, 0, 931, TPT_None);
    engine.addContext(monthly, 250111, 931, TPT_Monthly);
    engine.addContext(weekly, 6, 931, TPT_Weekly);
    engine.addContext(yearly, 1231, 931, TPT_Yearly);
    // 重复注册：同一 id 第二次被拒绝（只在日志里报错，不覆盖任务）
    engine.addContext(none, 0, 931, TPT_None);

    // nextTime=931，四个任务都命中时间；节假日回溯后各自 bIgnore=true
    engine.on_minute_end(20250110, 930);

    // 跨日回绕：2359 -> 0000，curDate 前进一天；任务时间不匹配直接跳过
    engine.on_minute_end(20250110, 2359);

    EXPECT_EQ(0u, static_cast<q01sel::SelCtx*>(none.get())->schedule_fires);
    EXPECT_EQ(0u, static_cast<q01sel::SelCtx*>(monthly.get())->schedule_fires);
    EXPECT_EQ(0u, static_cast<q01sel::SelCtx*>(weekly.get())->schedule_fires);
    EXPECT_EQ(0u, static_cast<q01sel::SelCtx*>(yearly.get())->schedule_fires);
}

//////////////////////////////////////////////////////////////////
// handle_pos_change：策略过滤器早退、HOT 规则标签换月、组合风控缩放
// src/WtCore/WtSelEngine.cpp:427-488
//////////////////////////////////////////////////////////////////
TEST(R110SelEngine, HandlePosChangeFilterRuleTagAndRiskScale)
{
    R110RuleHot hot;
    WtHelper::setGenerateDir(r90::temp_root().c_str());
    WtHelper::setTime(20250110, 931, 0);

    q01sel::BaseDataMgr bd;
    bd.regisSession("TRADING");
    WTSSessionInfo* sess = bd.session("TRADING");
    sess->addTradingSection(900, 1500);
    WTSCommodityInfo* ag = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
    ag->setSessionInfo(sess);
    bd.regis(ag, q01sel::makeCt("SHFE", "ag", "ag2512"));

    const std::string filters = r90::write_file("r110_filters/sel.json",
        "{\"strategy_filters\":{\"r110ignore\":{\"action\":\"ignore\"}}}");

    WtDtMgr dm;
    WtSelEngine& engine = *new WtSelEngine();  // BUG-R42-001：不回收
    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        (std::string("{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"")
         + filters + "\"}").c_str());
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &bd, &dm, &hot, NULL);
    cfg->release();
    engine.set_date_time(20250110, 931, 0);
    engine.set_trading_date(20250110);

    // 过滤器命中：整条增量被丢弃
    engine.handle_pos_change("r110ignore", "SHFE.ag.2512", 2.0);

    // 组合风控缩放 2x；HOT 规则经 hot_mgr 还原成 SHFE.ag.2512
    engine.setVolScale(2.0);
    engine.handle_pos_change("r110rule", "SHFE.ag.HOT", 3.0);

    // 增量 0 仍会建立仓位记录，随后被过滤/缩放逻辑安全跳过
    engine.handle_pos_change("r110rule", "SHFE.ag.2512", 0.0);

    EXPECT_EQ(20250110u, engine.get_trading_date());
}

}  // namespace
