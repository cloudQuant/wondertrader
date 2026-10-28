/**
 * r120_sel_engine_run_test.cpp — lane R120 / wave20
 *
 * 目标：src/WtCore/WtSelEngine.cpp 中「非空策略/通道」与节假日早退分支。
 *
 * 覆盖行段：
 *   - run() 落盘 marker.json 的策略列表 :349-357 与通道列表 :359-366
 *   - on_minute_end 的节假日早退 :240-244（Trigger 前返回，不会创建 StdThread）
 *
 * 说明：run() 末尾会拉起 WtSelRtTicker 线程，用例读完 marker 立即 release()
 * 停线程（与 R110 同口径，不做任何墙钟断言）。on_minute_end 只构造一个
 * 时间命中但当日为节假日的任务，命中 :244 continue，因此不会走到
 * :329-336 的线程创建（该处在产品里会 std::terminate，见 bugs/R120.md）。
 */
#include <TestUnits/coverage/q01_wtcore3/q01_sel_env.h>
#include <TestUnits/coverage/n01_wtcore_tradepaths/r71_wtcore_env.h>

#include <WTSUtils/WTSCfgLoader.h>

#include <WtCore/TraderAdapter.h>
#include <WtCore/WtDtMgr.h>
#include <WtCore/WtSelEngine.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace wtp;

namespace {

namespace fs = std::filesystem;

/** 当日为节假日的交易日历替身 */
class R120HolidayBase : public q01sel::BaseDataMgr
{
public:
    bool isHoliday(const char*, uint32_t uDate, bool = false) override
    {
        return uDate == 20250110;
    }
};

/** 只补上组合资金与数据管理器注入的选股引擎 */
class R120SelEngine : public q01sel::SelEngine
{
public:
    // _port_fund 由 WtEngine::init -> load_datas 创建；这里只负责回收
    R120SelEngine() {}

    ~R120SelEngine()
    {
        if (_port_fund != NULL)
        {
            _port_fund->release();
            _port_fund = NULL;
        }
    }

    void setDataMgr(WtDtMgr* dm) { _data_mgr = dm; }
};

WTSVariant* makeCfg()
{
    return WTSCfgLoader::load_from_content(
        "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"}");
}

std::string marker_file() { return std::string(WtHelper::getBaseDir()) + "marker.json"; }

std::string read_all(const std::string& path)
{
    std::ifstream ifs(path.c_str(), std::ios::binary);
    if (!ifs.is_open())
        return std::string();
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

class R120SelFixture : public ::testing::Test
{
protected:
    q01sel::Q01TempEnv env{"r120sel"};
    q01sel::HotMgr hot;
    WtDtMgr dm;

    void TearDown() override
    {
        std::error_code ec;
        fs::remove(marker_file(), ec);
    }
};

//////////////////////////////////////////////////////////////////
// run()：marker.json 同时导出策略与通道
// src/WtCore/WtSelEngine.cpp:339-380（重点 :349-357, :359-366）
//////////////////////////////////////////////////////////////////
TEST_F(R120SelFixture, RunWritesStrategyAndChannelMarkers)
{
    q01sel::BaseDataMgr bd;
    bd.regisSessionWithTime("TRADING", 900, 1500);

    r71::R71TraderEnv traderEnv;
    TraderAdapterPtr adapter(new TraderAdapter());
    ASSERT_TRUE(traderEnv.build(*adapter, "r120ch"));
    ASSERT_STREQ("r120ch", adapter->id());

    TraderAdapterMgr adapterMgr;
    ASSERT_TRUE(adapterMgr.addAdapter("r120ch", adapter));

    // _tm_ticker 未在构造函数中初始化（BUG-R42-001），先 run 再 release 才安全析构
    R120SelEngine engine;
    engine.attach(&bd, &hot);
    engine.setDataMgr(&dm);
    engine.set_adapter_mgr(&adapterMgr);

    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &bd, &dm, &hot, NULL);
    cfg->release();

    SelContextPtr ctx(new q01sel::SelCtx(&engine, "r120sel", 0));
    engine.addContext(ctx, 20250110, 931, TPT_None, true, "CHINA", "TRADING");
    EXPECT_EQ(ctx, engine.getContext(ctx->id()));

    engine.run();

    const std::string content = read_all(marker_file());
    engine.release();

    ASSERT_FALSE(content.empty()) << marker_file();
    EXPECT_NE(std::string::npos, content.find("\"r120sel\""));
    EXPECT_NE(std::string::npos, content.find("\"r120ch\""));
    EXPECT_NE(std::string::npos, content.find("\"SEL\""));
}

//////////////////////////////////////////////////////////////////
// on_minute_end：时间命中但当日节假日 -> continue，不创建线程
// src/WtCore/WtSelEngine.cpp:224-244（重点 :243-244）
//////////////////////////////////////////////////////////////////
TEST_F(R120SelFixture, MinuteEndSkipsTaskOnHoliday)
{
    R120HolidayBase bd;
    bd.regisSessionWithTime("TRADING", 900, 1500);

    R120SelEngine engine;
    TraderAdapterMgr adapterMgr;
    engine.set_adapter_mgr(&adapterMgr);
    engine.attach(&bd, &hot);
    engine.setDataMgr(&dm);

    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &bd, &dm, &hot, NULL);
    cfg->release();

    SelContextPtr ctx(new q01sel::SelCtx(&engine, "r120sel_holiday", 0));
    engine.addContext(ctx, 20250110, 931, TPT_None, true, "CHINA", "TRADING");

    // 同样先 run 再 release，规避未初始化 _tm_ticker 的析构读脏
    engine.run();
    engine.release();

    // 930 -> nextTime 931，与任务 _time 命中；isHoliday 为真直接在 :244 早退
    engine.on_minute_end(20250110, 930);

    EXPECT_EQ(ctx, engine.getContext(ctx->id()));
}

}  // namespace

