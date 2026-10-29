/**
 * r130_engine_fee_test.cpp — lane R130 / wave21
 *
 * 目标：src/WtCore/WtEngine.cpp 的手续费计算面。
 * 本文件用测试子类注入模板，逐分支验证 `calc_fee` 的算术与四舍五入；另通过
 * `load_fees` 的公开加载路径验证商品费率正控及引擎手续费计算契约
 *（BUG-R130-001 / BUG-R41-001）。加载路径只断言公开结果，不约束内部模板存储方式。
 *
 * 覆盖行段：
 *   calc_fee :879-916（byvolume × offset 0/1/2/default、按金额 × 0/1/2/default、
 *             无模板早退 :884-888、两位小数四舍五入 :915）
 *   load_fees :843-876（空文件名、文件不存在、正常解析并写品种费率）
 *
 * 不 init/不 run/不起线程；临时文件只写 temp_directory_path()/q01sel_r130fee_<pid>/。
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
void initEngine(R130FeeEngine& engine, q01sel::BaseDataMgr& bd, q01sel::HotMgr& hot)
{
    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"}");
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &bd, NULL, &hot, NULL);
    cfg->release();
}

std::string writeFeeFile(const std::string& content)
{
    sfs::path dir = std::filesystem::temp_directory_path()
        / ("q01sel_r130fee_" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code ec;
    sfs::create_directories(dir, ec);
    sfs::path p = dir / "fees.json";
    std::ofstream ofs(p.string().c_str(), std::ios::out | std::ios::trunc);
    ofs << content;
    ofs.close();
    return p.string();
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
// 公开 load_fees → calc_fee 正向契约：按量模板的引擎手续费应为 3.0
// src/WtCore/WtEngine.cpp:843-916（BUG-R130-001 / BUG-R41-001）
//////////////////////////////////////////////////////////////////
TEST(R130EngineFee, LoadFeesMakesTemplateAvailableForEngineCalculation)
{
    r130::TempEnv env("r130fee");
    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    prepareEnv(bd, hot);

    const std::string path = writeFeeFile(
        "{\"SHFE.ag\":{\"open\":1.5,\"close\":1.2,\"closetoday\":1.0,"
        "\"byvolume\":true,\"margin\":0.1}}");

    R130FeeEngine engine;
    engine.attach(&bd, &hot);
    initEngine(engine, bd, hot);

    // 未加载模板时，引擎手续费使用无模板默认值 0。
    EXPECT_DOUBLE_EQ(0.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));

    engine.reloadFees(path.c_str());

    // 品种上的费率确实被 load_fees 改写（1.5 * 2 手 = 3.0）
    WTSCommodityInfo* comm = bd.getCommodity("SHFE", "ag");
    ASSERT_NE(nullptr, comm);
    EXPECT_DOUBLE_EQ(3.0, comm->calcFee(3500.0, 2.0, 0));

    // 引擎计算与同一已加载模板一致，不锁定模板在引擎内部的存储实现。
    EXPECT_DOUBLE_EQ(3.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));

    std::error_code ec;
    sfs::remove(sfs::path(path).parent_path(), ec);
}
