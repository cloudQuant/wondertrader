/**
 * r101_engine_gap_test.cpp — lane R101 / wave18
 *
 * 目标：src/WtCore/WtEngine.cpp 剩余的组合资金、费率与持仓明细分支。
 *
 * 覆盖行段：
 *   - load_datas 明细可选 opentdate：:563-564
 *   - on_session_end 的 funds.csv 追加写：:334-337
 *   - update_fund_dynprofit 当日已结算早退：:185-189
 *   - get_exright_factor 股票分支：:759-760
 *   - load_fees 坏 JSON / 品种缺失：:854-859, :866-868
 *   - do_set_position 零手明细跳过：:1016-1020
 */
#include <TestUnits/coverage/n02_wtcore_engines/r73_fixture.h>

#include <WTSUtils/WTSCfgLoader.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace wtp;

namespace {

const char* const kAg = "SHFE.ag.2512";
const char* const kStock = "SSE.600000.600000";

class R101Engine : public r73::CtaEngine
{
public:
    using WtEngine::append_signal;
    using WtEngine::update_fund_dynprofit;

    double volume(const char* code) const
    {
        auto it = _pos_map.find(code);
        return it == _pos_map.end() ? 0.0 : it->second->_volume;
    }
};

std::string portfolio_file()
{
    return std::string(WtHelper::getPortifolioDir()) + "datas.json";
}

void write_text(const std::string& path, const std::string& content)
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream ofs(path.c_str(), std::ios::binary | std::ios::trunc);
    ofs << content;
}

std::string datas_json()
{
    return R"JSON({
  "fund": {"predynbal":100000.0,"balance":100000.0,"prebalance":100000.0,
           "profit":0.0,"dynprofit":0.0,"fees":0.0,"last_date":20250109,
           "max_dyn_bal":100000.0,"max_time":0,"min_dyn_bal":100000.0,"min_time":0,
           "maxmd":{"dyn_balance":100000.0,"date":20250109},
           "minmd":{"dyn_balance":100000.0,"date":20250109}},
  "positions": [
    {"code":"SHFE.ag.2512","volume":5.0,"closeprofit":0.0,"dynprofit":0.0,
     "details":[
       {"long":true,"price":5000.0,"volume":0.0,"opentime":202501100931,
        "opentdate":20250110,"profit":0.0},
       {"long":true,"price":5000.0,"volume":5.0,"opentime":202501100931,
        "opentdate":20250110,"profit":0.0}]}
  ]
})JSON";
}

class R101EngineGap : public ::testing::Test
{
protected:
    r73::TempDir tmp{"r101_engine"};
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

        WTSCommodityInfo* stock = r73::makeComm("SSE", "600000", "TRADING", TM_Long, CC_Stock);
        stock->setSessionInfo(sess);
        bd.regis(stock, r73::makeCt("SSE", "600000", "600000"));
    }
};

//////////////////////////////////////////////////////////////////
// 资金结算两次 + 当日早退 + 股票复权因子 + 零手明细平仓
// src/WtCore/WtEngine.cpp:185-189, 334-337, 563-564, 759-760, 1016-1020
//////////////////////////////////////////////////////////////////
TEST_F(R101EngineGap, FundSettlementAndZeroDetailClose)
{
    write_text(portfolio_file(), datas_json());

    R101Engine engine;
    engine.attach(&bd, &hot, &dm);
    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"poolsize\":0,\"filters\":\"\",\"fees\":\"\"}");
    ASSERT_NE(cfg, nullptr);
    engine.init(cfg, &bd, &dm, &hot, NULL);

    // 首次结算写 funds.csv；推进交易日后第二次走追加分支
    engine.on_session_end();
    engine.set_date_time(20250111, 931, 0);
    engine.set_trading_date(20250111);
    engine.on_session_end();

    // 当日已结算 -> 直接返回
    engine.update_fund_dynprofit();

    // 股票复权因子委托给 WtDtMgr（无 reader 时为 1.0）
    EXPECT_DOUBLE_EQ(1.0, engine.get_exright_factor(kStock));

    // 5 手多仓 -> 0：第一条零手明细跳过，第二条平完并清理
    engine.append_signal(kAg, 0.0, false);
    EXPECT_DOUBLE_EQ(0.0, engine.volume(kAg));
}

//////////////////////////////////////////////////////////////////
// load_fees 坏 JSON 与品种缺失
// src/WtCore/WtEngine.cpp:854-859, 866-868
//////////////////////////////////////////////////////////////////
TEST_F(R101EngineGap, FeeFileErrorAndMissingCommodity)
{
    const std::string bad = tmp.path("bad_fees.json");
    write_text(bad, "{not-json");

    {
        R101Engine engine;
        engine.attach(&bd, &hot, &dm);
        const std::string json = "{\"fees\":\"" + bad + "\"}";
        WTSVariant* cfg = WTSCfgLoader::load_from_content(json.c_str());
        ASSERT_NE(cfg, nullptr);
        engine.init(cfg, &bd, &dm, &hot, NULL);
        EXPECT_DOUBLE_EQ(0.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    }

    const std::string unknown = tmp.path("unknown_fees.json");
    write_text(unknown,
        "{\"XX.yy\":{\"open\":1.0,\"close\":1.0,\"closetoday\":1.0,"
        "\"byvolume\":true,\"margin\":0.1}}");

    {
        R101Engine engine;
        engine.attach(&bd, &hot, &dm);
        const std::string json = "{\"fees\":\"" + unknown + "\"}";
        WTSVariant* cfg = WTSCfgLoader::load_from_content(json.c_str());
        ASSERT_NE(cfg, nullptr);
        engine.init(cfg, &bd, &dm, &hot, NULL);
        EXPECT_DOUBLE_EQ(0.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    }
}

}  // namespace
