/**
 * r130_engine_fee_test.cpp — lane R130 / wave21
 *
 * 目标：src/WtCore/WtEngine.cpp 的手续费计算面。
 * 本文件用测试子类注入模板，逐分支验证 `calc_fee` 的算术与四舍五入；另通过
 * init 配置加载及 `load_fees` 重载验证商品费率和引擎手续费计算契约
 *（BUG-R130-001 / BUG-R41-001）。加载路径只断言公开结果，不约束内部模板存储方式。
 *
 * 覆盖行为：`calc_fee` 两种模式和各 offset；`load_fees` 的 init 加载、有效重载、
 * 错误重载原子性、空文件/空对象 no-op、缺省字段和 JSON/YAML 兼容行为。
 *
 * 调用 init 但不 run、不起线程；临时文件只写 temp_directory_path()/q01sel_r130fee_<pid>/。
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "q01_sel_env.h"
#include "r130_env.h"
#include "../p01_wtcore_stractx/p01_cta_env.h"

#include <Includes/WTSVariant.hpp>
#include <WtCore/WtEngine.h>
#include <WTSUtils/WTSCfgLoader.h>

using namespace wtp;

namespace {

namespace sfs = std::filesystem;

/** 把 WtEngine 的 protected 手续费模板与 load_fees 暴露给用例 */
class R130FeeEngine : public P01CtaEngine
{
public:
    void seedFee(const char* fullPid, double open, double close,
                 double closeToday, bool byVolume)
    {
        FeeItem item;
        item._open = open;
        item._close = close;
        item._close_today = closeToday;
        item._by_volume = byVolume;
        _fee_map[fullPid] = item;
    }

    void reloadFees(const char* filename) { load_fees(filename); }
};

const char* const kAg = "SHFE.ag.2512";

void prepareEnv(q01sel::BaseDataMgr& bd, q01sel::HotMgr& /*hot*/)
{
    bd.regisSessionWithTime("TRADING", 0, 2359);
    WTSSessionInfo* sess = bd.session("TRADING");
    WTSCommodityInfo* ag = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
    ag->setSessionInfo(sess);
    bd.regis(ag, q01sel::makeCt("SHFE", "ag", "ag2512"));
}
/**
 * 裸引擎必须走一次 init：WtCtaEngine 的 `_cfg` 由 init 赋值，
 * 而 `~WtCtaEngine` 会无条件 `_cfg->release()`。原夹具只 attach 不 init，
 * 析构时 `_cfg` 是未初始化指针，任何用例结束都会随机崩溃（本波 crash 的根因）。
 */
void initEngine(R130FeeEngine& engine, q01sel::BaseDataMgr& bd, q01sel::HotMgr& hot,
                const char* feeTemplate = "")
{
    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"product\":{\"session\":\"TRADING\"},\"filters\":\"\"}");
    ASSERT_NE(nullptr, cfg);
    ASSERT_TRUE(cfg->append("fees", feeTemplate));
    engine.init(cfg, &bd, NULL, &hot, NULL);
    cfg->release();
}

std::string writeFeeFile(const std::string& content, const std::string& filename = "fees.json")
{
    sfs::path dir = std::filesystem::temp_directory_path()
        / ("q01sel_r130fee_" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code ec;
    sfs::create_directories(dir, ec);
    sfs::path p = dir / filename;
    std::ofstream ofs(p.string().c_str(), std::ios::out | std::ios::trunc);
    ofs << content;
    ofs.close();
    return p.string();
}

void overwriteFeeFile(const std::string& path, const std::string& content)
{
    std::ofstream ofs(path.c_str(), std::ios::out | std::ios::trunc);
    ofs << content;
}

}  // namespace

//////////////////////////////////////////////////////////////////
// 按手数计费：offset 0/1/2/default + 两位小数四舍五入
// src/WtCore/WtEngine.cpp:890-902, 915
//////////////////////////////////////////////////////////////////
TEST(R130EngineFee, ByVolumeOffsetsAndRounding)
{
    r130::TempEnv env("r130fee");
    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    prepareEnv(bd, hot);

    R130FeeEngine engine;
    engine.attach(&bd, &hot);
    initEngine(engine, bd, hot);
    engine.seedFee("SHFE.ag", 3.0, 2.0, 1.0, true);

    EXPECT_DOUBLE_EQ(6.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(4.0, engine.calc_fee(kAg, 3500.0, 2.0, 1));
    EXPECT_DOUBLE_EQ(2.0, engine.calc_fee(kAg, 3500.0, 2.0, 2));
    EXPECT_DOUBLE_EQ(0.0, engine.calc_fee(kAg, 3500.0, 2.0, 9));

    // 0.333 手 -> 0.33（(int32_t)(33.3+0.5)/100）
    engine.seedFee("SHFE.ag", 0.333, 0.0, 0.0, true);
    EXPECT_DOUBLE_EQ(0.33, engine.calc_fee(kAg, 3500.0, 1.0, 0));

    // 费率模板键是品种（交易所.品种），不含月份
    EXPECT_DOUBLE_EQ(0.0, engine.calc_fee("SHFE.au.2512", 3500.0, 2.0, 0));
}

//////////////////////////////////////////////////////////////////
// 按金额计费：amount = price*qty*volScale(10)
// src/WtCore/WtEngine.cpp:903-913, 915
//////////////////////////////////////////////////////////////////
TEST(R130EngineFee, ByAmountOffsetsUseVolumeScale)
{
    r130::TempEnv env("r130fee");
    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    prepareEnv(bd, hot);

    R130FeeEngine engine;
    engine.attach(&bd, &hot);
    initEngine(engine, bd, hot);
    engine.seedFee("SHFE.ag", 0.0002, 0.0001, 0.00005, false);

    // 3500 * 2 * 10 = 70000
    EXPECT_DOUBLE_EQ(14.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(7.0, engine.calc_fee(kAg, 3500.0, 2.0, 1));
    EXPECT_DOUBLE_EQ(3.5, engine.calc_fee(kAg, 3500.0, 2.0, 2));
    EXPECT_DOUBLE_EQ(0.0, engine.calc_fee(kAg, 3500.0, 2.0, 3));
}

//////////////////////////////////////////////////////////////////
// init → load_fees → calc_fee 正向契约（BUG-R130-001 / BUG-R41-001）
//////////////////////////////////////////////////////////////////
TEST(R130EngineFee, LoadFeesMakesTemplateAvailableForEngineCalculation)
{
    r130::TempEnv env("r130fee");
    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    prepareEnv(bd, hot);

    WTSCommodityInfo* dceA = q01sel::makeComm("DCE", "a", "TRADING", TM_Both);
    dceA->setSessionInfo(bd.session("TRADING"));
    bd.regis(dceA, q01sel::makeCt("DCE", "a", "a2512"));

    const std::string path = writeFeeFile(
        "{\"SHFE.ag\":{\"open\":1.5,\"close\":1.2,\"closetoday\":1.0,"
        "\"byvolume\":true,\"margin\":0.1,\"minfee\":5.0},"
        "\"DCE.a\":{\"open\":2.0,\"close\":3.0,\"closetoday\":4.0,"
        "\"byvolume\":true,\"margin\":0.3}}");

    R130FeeEngine engine;
    engine.attach(&bd, &hot);
    initEngine(engine, bd, hot, path.c_str());

    // init 的 fees 配置加载后，品种与引擎读取同一组费率。
    WTSCommodityInfo* comm = bd.getCommodity("SHFE", "ag");
    ASSERT_NE(nullptr, comm);
    WTSCommodityInfo* dceComm = bd.getCommodity("DCE", "a");
    ASSERT_NE(nullptr, dceComm);
    EXPECT_DOUBLE_EQ(0.1, comm->getMarginRate());
    EXPECT_DOUBLE_EQ(3.0, comm->calcFee(3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(3.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(4.0, dceComm->calcFee(3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(4.0, engine.calc_fee("DCE.a.2512", 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(0.3, dceComm->getMarginRate());
    EXPECT_DOUBLE_EQ(comm->calcFee(3500.0, 2.0, 1),
        engine.calc_fee(kAg, 3500.0, 2.0, 1));
    EXPECT_DOUBLE_EQ(comm->calcFee(3500.0, 2.0, 2),
        engine.calc_fee(kAg, 3500.0, 2.0, 2));
    EXPECT_DOUBLE_EQ(4500000000.0, engine.calc_fee(kAg, 3500.0, 3000000000.0, 0));
    EXPECT_DOUBLE_EQ(comm->calcFee(3500.0, 3000000000.0, 0),
        engine.calc_fee(kAg, 3500.0, 3000000000.0, 0));
    EXPECT_DOUBLE_EQ(0.0, engine.calc_fee("SHFE.au.2512", 3500.0, 2.0, 0));

    auto expectOriginalSnapshot = [&]()
    {
        EXPECT_DOUBLE_EQ(3.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
        EXPECT_DOUBLE_EQ(2.4, engine.calc_fee(kAg, 3500.0, 2.0, 1));
        EXPECT_DOUBLE_EQ(2.0, engine.calc_fee(kAg, 3500.0, 2.0, 2));
        EXPECT_DOUBLE_EQ(3.0, comm->calcFee(3500.0, 2.0, 0));
        EXPECT_DOUBLE_EQ(0.1, comm->getMarginRate());
    };
    auto expectOriginalTwoProductSnapshot = [&]()
    {
        expectOriginalSnapshot();
        EXPECT_DOUBLE_EQ(4.0, engine.calc_fee("DCE.a.2512", 3500.0, 2.0, 0));
        EXPECT_DOUBLE_EQ(4.0, dceComm->calcFee(3500.0, 2.0, 0));
        EXPECT_DOUBLE_EQ(0.3, dceComm->getMarginRate());
    };

    // Missing fee fields retain the old getter defaults, but an unsafe value
    // in any known entry rejects the whole reload before changing either
    // engine fees or commodity metadata.
    overwriteFeeFile(path,
        "{\"SHFE.ag\":{\"open\":99.0,\"close\":99.0,\"closetoday\":99.0,"
        "\"byvolume\":true,\"margin\":0.9},"
        "\"DCE.a\":{\"open\":99.0,\"close\":{},\"closetoday\":99.0,"
        "\"byvolume\":true,\"margin\":0.9}}");
    engine.reloadFees(path.c_str());
    expectOriginalTwoProductSnapshot();

    overwriteFeeFile(path,
        "{\"SHFE.ag\":{\"open\":\"99.0partial\",\"close\":99.0,\"closetoday\":99.0,"
        "\"byvolume\":true,\"margin\":0.9}}");
    engine.reloadFees(path.c_str());
    expectOriginalTwoProductSnapshot();

    overwriteFeeFile(path,
        "{\"SHFE.ag\":{\"open\":99.0,\"close\":99.0,\"closetoday\":99.0,"
        "\"byvolume\":true,\"margin\":\"0.9partial\"}}");
    engine.reloadFees(path.c_str());
    expectOriginalTwoProductSnapshot();

    overwriteFeeFile(path,
        "{\"SHFE.ag\":{\"open\":99.0,\"close\":99.0,\"closetoday\":99.0,"
        "\"byvolume\":true,\"margin\":0.9,\"minfee\":\"5.0partial\"}}");
    engine.reloadFees(path.c_str());
    expectOriginalTwoProductSnapshot();

    // WTSVariant omits null-valued top-level entries, which remain a no-op.
    overwriteFeeFile(path, "{\"SHFE.ag\":null}");
    engine.reloadFees(path.c_str());
    expectOriginalTwoProductSnapshot();

    // Missing and null fee fields use getDouble/getBoolean's historical
    // defaults (0 / false), and an explicit entry updates only that product.
    overwriteFeeFile(path, "{\"SHFE.ag\":{}}");
    engine.reloadFees(path.c_str());
    EXPECT_DOUBLE_EQ(0.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(0.0, comm->calcFee(3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(0.0, comm->getMarginRate());
    EXPECT_DOUBLE_EQ(4.0, engine.calc_fee("DCE.a.2512", 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(0.3, dceComm->getMarginRate());

    overwriteFeeFile(path,
        "{\"SHFE.ag\":{\"open\":null,\"close\":null,\"closetoday\":null,"
        "\"byvolume\":null,\"margin\":null}}");
    engine.reloadFees(path.c_str());
    EXPECT_DOUBLE_EQ(0.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(0.0, comm->calcFee(3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(0.0, comm->getMarginRate());

    // Restore the initial SHFE item; the absent DCE entry remains in place.
    overwriteFeeFile(path,
        "{\"SHFE.ag\":{\"open\":1.5,\"close\":1.2,\"closetoday\":1.0,"
        "\"byvolume\":true,\"margin\":0.1}}");
    engine.reloadFees(path.c_str());
    expectOriginalTwoProductSnapshot();

    // JSON string numbers and legacy true/yes string booleans stay compatible.
    overwriteFeeFile(path,
        "{\"SHFE.ag\":{\"open\":\"1.5\",\"close\":\"1.2\",\"closetoday\":\"1.0\","
        "\"byvolume\":\"yes\",\"margin\":\"0.1\",\"minfee\":\"5.0\"}}");
    engine.reloadFees(path.c_str());
    expectOriginalTwoProductSnapshot();

    overwriteFeeFile(path,
        "{\"SHFE.ag\":{\"open\":\"1.5\",\"close\":\"1.2\",\"closetoday\":\"1.0\","
        "\"byvolume\":\"true\",\"margin\":\"0.1\"}}");
    engine.reloadFees(path.c_str());
    expectOriginalTwoProductSnapshot();

    overwriteFeeFile(path,
        "{\"SHFE.ag\":{\"open\":\"0.0002\",\"close\":\"0.0001\",\"closetoday\":\"0.00005\","
        "\"byvolume\":\"false\",\"margin\":\"0.2\"}}");
    engine.reloadFees(path.c_str());
    EXPECT_DOUBLE_EQ(14.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(7.0, engine.calc_fee(kAg, 3500.0, 2.0, 1));
    EXPECT_DOUBLE_EQ(3.5, engine.calc_fee(kAg, 3500.0, 2.0, 2));
    EXPECT_DOUBLE_EQ(0.2, comm->getMarginRate());
    EXPECT_DOUBLE_EQ(4.0, engine.calc_fee("DCE.a.2512", 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(0.3, dceComm->getMarginRate());

    // Legacy asBoolean maps every non-true/yes value to false.
    overwriteFeeFile(path,
        "{\"SHFE.ag\":{\"open\":\"0.0002\",\"close\":\"0.0001\","
        "\"closetoday\":\"0.00005\",\"byvolume\":\"maybe\",\"margin\":\"0.2\"}}");
    engine.reloadFees(path.c_str());
    EXPECT_DOUBLE_EQ(14.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(7.0, engine.calc_fee(kAg, 3500.0, 2.0, 1));
    EXPECT_DOUBLE_EQ(3.5, engine.calc_fee(kAg, 3500.0, 2.0, 2));

    // Unknown products retain the historical skip behavior; the known valid
    // entry in the same snapshot still applies even if the unknown item is null.
    overwriteFeeFile(path,
        "{\"SHFE.ag\":{\"open\":4.0,\"close\":5.0,\"closetoday\":6.0,"
        "\"byvolume\":true,\"margin\":0.9},\"SHFE.unknown\":null}");
    engine.reloadFees(path.c_str());
    EXPECT_DOUBLE_EQ(8.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(10.0, engine.calc_fee(kAg, 3500.0, 2.0, 1));
    EXPECT_DOUBLE_EQ(12.0, engine.calc_fee(kAg, 3500.0, 2.0, 2));
    EXPECT_DOUBLE_EQ(8.0, comm->calcFee(3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(0.9, comm->getMarginRate());
    EXPECT_DOUBLE_EQ(4.0, engine.calc_fee("DCE.a.2512", 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(0.3, dceComm->getMarginRate());

    // A valid reload updates products present in the new file and preserves
    // registered products omitted from this partial file.
    overwriteFeeFile(path,
        "{\"SHFE.ag\":{\"open\":0.0002,\"close\":0.0001,\"closetoday\":0.00005,"
        "\"byvolume\":false,\"margin\":0.2}}");
    engine.reloadFees(path.c_str());
    EXPECT_DOUBLE_EQ(14.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(7.0, engine.calc_fee(kAg, 3500.0, 2.0, 1));
    EXPECT_DOUBLE_EQ(3.5, engine.calc_fee(kAg, 3500.0, 2.0, 2));
    EXPECT_DOUBLE_EQ(0.2, comm->getMarginRate());
    EXPECT_DOUBLE_EQ(comm->calcFee(3500.0, 2.0, 0),
        engine.calc_fee(kAg, 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(4.0, engine.calc_fee("DCE.a.2512", 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(0.3, dceComm->getMarginRate());

    // Large fees remain accurate; do not narrow the cent value through int32_t.
    overwriteFeeFile(path,
        "{\"SHFE.ag\":{\"open\":1000000,\"close\":1000000,\"closetoday\":1000000,"
        "\"byvolume\":true}}");
    engine.reloadFees(path.c_str());
    EXPECT_DOUBLE_EQ(3000000000.0, engine.calc_fee(kAg, 3500.0, 3000.0, 0));
    EXPECT_DOUBLE_EQ(comm->calcFee(3500.0, 3000.0, 0),
        engine.calc_fee(kAg, 3500.0, 3000.0, 0));
    EXPECT_DOUBLE_EQ(0.0, comm->getMarginRate());
    EXPECT_DOUBLE_EQ(4.0, engine.calc_fee("DCE.a.2512", 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(0.3, dceComm->getMarginRate());

    // Empty content fails to load and an empty object contains no updates;
    // both retain the current fees, matching the legacy no-op behavior.
    overwriteFeeFile(path, "");
    engine.reloadFees(path.c_str());
    EXPECT_DOUBLE_EQ(3000000000.0, engine.calc_fee(kAg, 3500.0, 3000.0, 0));
    EXPECT_DOUBLE_EQ(0.0, comm->getMarginRate());

    overwriteFeeFile(path, "{}");
    engine.reloadFees(path.c_str());
    EXPECT_DOUBLE_EQ(2000000.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(2000000.0, comm->calcFee(3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(0.0, comm->getMarginRate());
    EXPECT_DOUBLE_EQ(4.0, engine.calc_fee("DCE.a.2512", 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(0.3, dceComm->getMarginRate());

    std::error_code ec;
    sfs::remove(sfs::path(path), ec);
    sfs::remove(sfs::path(path).parent_path(), ec);
}

TEST(R130EngineFee, ByAmountFeesLoadThroughInitAndMatchCommodityCalculation)
{
    r130::TempEnv env("r130fee_by_amount_init");
    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    prepareEnv(bd, hot);

    const std::string path = writeFeeFile(
        "{\"SHFE.ag\":{\"open\":0.0002,\"close\":0.0001,\"closetoday\":0.00005,"
        "\"byvolume\":false,\"margin\":0.07}}",
        "fees_by_amount.json");

    R130FeeEngine engine;
    engine.attach(&bd, &hot);
    initEngine(engine, bd, hot, path.c_str());

    WTSCommodityInfo* comm = bd.getCommodity("SHFE", "ag");
    ASSERT_NE(nullptr, comm);
    EXPECT_DOUBLE_EQ(0.07, comm->getMarginRate());

    // init -> load_fees -> calc_fee, with fractional rates and explicit
    // parity against the commodity calculation for open/close/close-today.
    EXPECT_DOUBLE_EQ(14.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(7.0, engine.calc_fee(kAg, 3500.0, 2.0, 1));
    EXPECT_DOUBLE_EQ(3.5, engine.calc_fee(kAg, 3500.0, 2.0, 2));
    for (uint32_t offset = 0; offset <= 2; ++offset)
        EXPECT_DOUBLE_EQ(comm->calcFee(3500.0, 2.0, offset),
            engine.calc_fee(kAg, 3500.0, 2.0, offset));

    // A large amount produces a fee above the int32 cent-conversion range.
    const double largeQty = 10000000.0;
    EXPECT_DOUBLE_EQ(70000000.0, engine.calc_fee(kAg, 3500.0, largeQty, 0));
    EXPECT_DOUBLE_EQ(35000000.0, engine.calc_fee(kAg, 3500.0, largeQty, 1));
    EXPECT_DOUBLE_EQ(17500000.0, engine.calc_fee(kAg, 3500.0, largeQty, 2));
    for (uint32_t offset = 0; offset <= 2; ++offset)
        EXPECT_DOUBLE_EQ(comm->calcFee(3500.0, largeQty, offset),
            engine.calc_fee(kAg, 3500.0, largeQty, offset));

    std::error_code ec;
    sfs::remove(sfs::path(path), ec);
    sfs::remove(sfs::path(path).parent_path(), ec);
}

TEST(R130EngineFee, YAMLFeeTemplatePreservesNumericAndBooleanScalarCompatibility)
{
    r130::TempEnv env("r130fee_yaml");
    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    prepareEnv(bd, hot);

    const std::string path = writeFeeFile(
        "SHFE.ag:\n"
        "  open: 0.0002\n"
        "  close: 0.0001\n"
        "  closetoday: 0.00005\n"
        "  byvolume: false\n"
        "  margin: 0.1\n",
        "fees.yaml");

    R130FeeEngine engine;
    engine.attach(&bd, &hot);
    initEngine(engine, bd, hot, path.c_str());

    WTSCommodityInfo* comm = bd.getCommodity("SHFE", "ag");
    ASSERT_NE(nullptr, comm);
    EXPECT_DOUBLE_EQ(14.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(14.0, comm->calcFee(3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(0.1, comm->getMarginRate());

    // YAML null/missing fields use the same legacy defaults as JSON; a
    // non-true/yes scalar byvolume value remains false.
    overwriteFeeFile(path,
        "SHFE.ag:\n"
        "  open: null\n"
        "  byvolume: maybe\n");
    engine.reloadFees(path.c_str());
    EXPECT_DOUBLE_EQ(0.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(0.0, comm->calcFee(3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(0.0, comm->getMarginRate());

    std::error_code ec;
    sfs::remove(sfs::path(path), ec);
    sfs::remove(sfs::path(path).parent_path(), ec);
}
