/**
 * r101_cta_engine_hot_test.cpp — lane R101 / wave18
 *
 * 目标：src/WtCore/WtCtaEngine.cpp 中“持仓代码带主力规则标签”的映射分支。
 *
 * 覆盖行段：
 *   - on_init 枚举持仓时把 HOT 规则代码转成月度合约：:170-181
 *   - on_schedule 无线程池的同一映射：:336-347
 *   - on_schedule 线程池分支的同一映射：:295-306
 *
 * 用 r73 夹具的裸引擎 + 固定时钟；线程池只在 on_schedule 的 pool->wait() 内使用，
 * 不启动 ticker / 不 run 引擎。
 */
#include <TestUnits/coverage/n02_wtcore_engines/r73_fixture.h>

#include <WTSUtils/WTSCfgLoader.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace wtp;

namespace {

const char* const kHot = "SHFE.ag.HOT";

std::string data_file(const char* name)
{
    return std::string(WtHelper::getStraDataDir()) + name + ".json";
}

void write_text(const std::string& path, const std::string& content)
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream ofs(path.c_str(), std::ios::binary | std::ios::trunc);
    ofs << content;
}

std::string hot_position_json()
{
    return R"JSON({
  "fund": {"total_profit": 0.0, "total_dynprofit": 0.0, "total_fees": 0.0, "tdate": 20250110},
  "positions": [
    {"code":"SHFE.ag.HOT","volume":2.0,"closeprofit":0.0,"dynprofit":0.0,
     "lastentertime":202501100931,"lastexittime":0,"frozen":0.0,"frozendate":0,
     "details":[{"long":true,"price":5200.0,"volume":2.0,"opentime":202501100931,
       "opentdate":20250110,"maxprice":5200.0,"minprice":5200.0,
       "profit":0.0,"maxprofit":0.0,"maxloss":0.0,"opentag":"hot1"}]}
  ],
  "conditions": {"settime": 0, "items": {}},
  "signals": {},
  "utils": {"lastbarno": 0}
})JSON";
}

class R101CtaEngineHot : public ::testing::Test
{
protected:
    r73::TempDir tmp{"r101_cta_hot"};
    r73::BaseDataMgr bd;
    r73::HotMgr hot;
    r73::DataMgr dm;

    void SetUp() override
    {
        WtHelper::setGenerateDir((tmp.path("generated") + "/").c_str());
        WtHelper::setTime(20250110, 1000, 0);

        bd.regisSession("TRADING", 900, 1500);
        WTSSessionInfo* sess = bd.session("TRADING");
        ASSERT_NE(sess, nullptr);

        WTSCommodityInfo* ag = r73::makeComm("SHFE", "ag", "TRADING", TM_Both);
        ag->setSessionInfo(sess);
        bd.regis(ag, r73::makeCt("SHFE", "ag", "ag2512"));

        hot.rule_tag = "HOT";
        hot.prev_custom = "ag2512";
        hot.raw_code = "ag2512";
    }
};

//////////////////////////////////////////////////////////////////
// on_init / on_schedule（无池）主力规则代码映射
// src/WtCore/WtCtaEngine.cpp:170-181, 336-347
//////////////////////////////////////////////////////////////////
TEST_F(R101CtaEngineHot, OnInitAndScheduleMapHotCode)
{
    write_text(data_file("r101hot"), hot_position_json());

    r73::CtaEngine engine;
    WTSVariant* cfg = WTSCfgLoader::load_from_content("{\"poolsize\":0,\"filters\":\"\",\"fees\":\"\"}");
    ASSERT_NE(cfg, nullptr);
    engine.init(cfg, &bd, &dm, &hot, NULL);
    engine.attach(&bd, &hot, &dm);

    r73::CtaCtx* raw = new r73::CtaCtx(&engine, "r101hot", 0);
    CtaContextPtr ctx(raw);
    engine.addContext(ctx);

    engine.on_init();
    EXPECT_DOUBLE_EQ(2.0, raw->stra_get_position(kHot));

    engine.on_schedule(20250110, 1000);
    EXPECT_DOUBLE_EQ(2.0, raw->stra_get_position(kHot));
}

//////////////////////////////////////////////////////////////////
// on_schedule（有池）主力规则代码映射
// src/WtCore/WtCtaEngine.cpp:295-306
//////////////////////////////////////////////////////////////////
TEST_F(R101CtaEngineHot, PooledScheduleMapsHotCode)
{
    write_text(data_file("r101hotp"), hot_position_json());

    r73::CtaEngine engine;
    WTSVariant* cfg = WTSCfgLoader::load_from_content("{\"poolsize\":1,\"filters\":\"\",\"fees\":\"\"}");
    ASSERT_NE(cfg, nullptr);
    engine.init(cfg, &bd, &dm, &hot, NULL);
    engine.attach(&bd, &hot, &dm);

    r73::CtaCtx* raw = new r73::CtaCtx(&engine, "r101hotp", 0);
    CtaContextPtr ctx(raw);
    engine.addContext(ctx);

    engine.on_init();
    engine.on_schedule(20250110, 1001);
    EXPECT_DOUBLE_EQ(2.0, raw->stra_get_position(kHot));
}

}  // namespace
