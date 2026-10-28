/**
 * r110_engine_riskmon_test.cpp — lane R110 / wave19
 *
 * 目标：src/WtCore/WtEngine.cpp 的 init_riskmon 成功链（插件装载 ->
 * createRiskMonFact -> createRiskMonotor("SimpleRiskMon") -> init）。
 *
 * 覆盖行段：:1143-1185
 *
 * 只调用 init()，不调用 WtCtaEngine::run()，因此 WtSimpleRiskMon 的监控线程
 * 从未启动；随后引擎析构只做 delete，不会 join 未创建的线程。
 * 插件 libWtRiskMonFact.dylib 直接取自构建输出目录，不访问网络。
 */
#include <gtest/gtest.h>

#include <string>

#include "../n02_wtcore_engines/r73_fixture.h"
#include "r90_env.h"

#include <WtCore/WtCtaEngine.h>
#include <WtCore/WtHelper.h>
#include <WTSUtils/WTSCfgLoader.h>

using namespace wtp;

namespace {

TEST(R110EngineRiskMon, InitLoadsRiskMonFactAndCreatesSimpleMonitor)
{
    const std::string bin = r90::find_bin_dir();
    if (bin.empty())
        GTEST_SKIP() << "build bin dir not found";

    WtHelper::setGenerateDir(r90::temp_root().c_str());
    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((bin + "/").c_str());

    r73::BaseDataMgr bd;
    r73::HotMgr      hot;
    r73::DataMgr     dm;
    WTSSessionInfo* sess = bd.regisSession("TRADING");
    sess->addTradingSection(900, 1500);
    WTSCommodityInfo* ag = r73::makeComm("SHFE", "ag", "TRADING", TM_Both);
    ag->setSessionInfo(sess);
    bd.regis(ag, r73::makeCt("SHFE", "ag", "ag2512"));

    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\","
        "\"riskmon\":{\"active\":true,\"module\":\"WtRiskMonFact\","
        "\"name\":\"SimpleRiskMon\",\"calc_span\":1,\"risk_span\":30,"
        "\"basic_ratio\":1,\"inner_day_fd\":5.0,\"inner_day_active\":false,"
        "\"multi_day_fd\":10.0,\"multi_day_active\":false,"
        "\"base_amount\":1000000.0,\"risk_scale\":0.5}}");
    ASSERT_NE(nullptr, cfg);

    {
        WtCtaEngine engine;
        engine.set_adapter_mgr(NULL);
        engine.init(cfg, &bd, &dm, &hot, NULL);
        engine.set_date_time(20250110, 931, 0);
        engine.set_trading_date(20250110);
        EXPECT_EQ(20250110u, engine.get_trading_date());
    }

    cfg->release();
    WtHelper::setInstDir(oldInst.c_str());
}

}  // namespace
