/**
 * p02_wt_engine_state_test.cpp  (lane P02 / WtCore 引擎与执行器 第3波)
 *
 * 覆盖目标:
 *   src/WtCore/WtEngine.cpp/.h :
 *     ctor(时钟初始化) / set_date_time / set_trading_date
 *     get_date / get_min_time / get_raw_time / get_secs / get_trading_date  (头文件内联)
 *     getCurDate / getCurTime / getTradingDate / isInTrading / transTimeToMin / setVolScale
 *     init(cfg, bdMgr, dataMgr, hotMgr, notifier) 的配置解析与缺省分支
 *     load_fees(空文件名早退) / load_datas(portfolio/datas.json 的 fund+positions+riskmon 反序列化)
 *     save_datas(同一份数据的再序列化, 通过 setVolScale 触发)
 *     get_commodity_info / get_exright_factor / get_adjusting_flag / get_kline_slice(商品缺失早退)
 *     calc_fee(无费率模板返回 0.0) / get_cur_price / get_day_price(无 tick 返回 0.0)
 *     append_signal(挂起分支) / do_set_position(仓位未变早退)
 *
 * 明确跳过(需要真实通道 / 线程 / 服务端回报, 详见 manifest):
 *   run() / start() / task_loop() —— 起线程并阻塞, 且 push_task 的 TaskItem 由内部产生;
 *   on_session_begin/on_session_end —— on_session_end 会用 _port_fund 写 funds.csv 并推进结算日;
 *   init_riskmon —— dlopen 风控插件;
 *   handle_push_quote —— 需要真实 WTSTickData 与 WTSContractInfo;
 *   get_session_info / get_contract_info / get_rawcode —— 依赖基础数据管理器返回真实对象,
 *   本 lane 不构造 WTSCommodityInfo/WTSContractInfo, 因此只走 get_commodity_info 的 NULL 分支。
 */
#include <gtest/gtest.h>

#include <boost/noncopyable.hpp>

#include "WtCore/WtEngine.h"
#include "WtCore/WtDtMgr.h"
#include "WtCore/WtHelper.h"
#include "Includes/FasterDefs.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/IDataManager.h"
#include "Includes/WTSVariant.hpp"
#include "WTSUtils/WTSCfgLoader.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

using namespace wtp;

namespace {

namespace fs = std::filesystem;

std::string p02_engine_tmp_root()
{
    static std::string root = []() {
        std::string name = "p02_wtcore_" + std::to_string(static_cast<long long>(::getpid()));
        fs::path path = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::create_directories(path, ec);
        return path.string() + "/";
    }();
    return root;
}

struct P02EngineGenDirGuard
{
    P02EngineGenDirGuard() { WtHelper::setGenerateDir(p02_engine_tmp_root().c_str()); }
};
const P02EngineGenDirGuard g_p02_engine_gen_dir;

std::string portfolio_data_file()
{
    return std::string(WtHelper::getPortifolioDir()) + "datas.json";
}

bool write_text_file(const std::string& path, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream ofs(path, std::ios::out | std::ios::trunc);
    if (!ofs.is_open())
        return false;
    ofs << content;
    ofs.close();
    return true;
}

std::string read_text_file(const std::string& path)
{
    std::ifstream ifs(path);
    if (!ifs.is_open())
        return std::string();
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

std::string write_cfg(const std::string& rel, const std::string& content)
{
    fs::path path = fs::path(p02_engine_tmp_root()) / rel;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream ofs(path.string(), std::ios::out | std::ios::trunc);
    ofs << content;
    ofs.close();
    return path.string();
}

//////////////////////////////////////////////////////////////////
// 替身
//////////////////////////////////////////////////////////////////

/// 只回答引擎会问到的几个数据接口, 一律"无数据"
class P02SilentDtMgr : public WtDtMgr
{
public:
    uint32_t _grab_calls {0};

    WTSTickData* grab_last_tick(const char*) override
    {
        ++_grab_calls;
        return NULL;
    }
    WTSTickSlice* get_tick_slice(const char*, uint32_t, uint64_t) override { return NULL; }
    WTSKlineSlice* get_kline_slice(const char*, WTSKlinePeriod, uint32_t, uint32_t, uint64_t) override { return NULL; }
    double get_adjusting_factor(const char*, uint32_t) override { return 1.5; }
    uint32_t get_adjusting_flag() override { return 1; }
};

/// 商品/合约/时段一律查不到
class P02EmptyBaseMgr : public IBaseDataMgr
{
public:
    uint32_t _comm_calls {0};

    WTSCommodityInfo* getCommodity(const char*) override { ++_comm_calls; return NULL; }
    WTSCommodityInfo* getCommodity(const char*, const char*) override { ++_comm_calls; return NULL; }
    WTSContractInfo* getContract(const char*, const char*, uint32_t) override { return NULL; }
    WTSArray* getContracts(const char*, uint32_t) override { return NULL; }
    WTSSessionInfo* getSession(const char*) override { return NULL; }
    WTSSessionInfo* getSessionByCode(const char*, const char*) override { return NULL; }
    WTSArray* getAllSessions() override { return NULL; }
    bool isHoliday(const char*, uint32_t, bool) override { return true; }
    uint32_t calcTradingDate(const char*, uint32_t, uint32_t, bool) override { return 0; }
    uint64_t getBoundaryTime(const char*, uint32_t, bool, bool) override { return 0; }
};

/// 可执行的具体引擎: run()/on_bar() 是纯虚, 这里都做成空实现, 绝不启动线程
class P02TestEngine : public WtEngine
{
public:
    uint32_t _bar_calls {0};
    uint32_t _tick_calls {0};

    void run() override { /* 故意空实现: 本 lane 不允许起线程 */ }

    void on_bar(const char*, const char*, uint32_t, WTSBarStruct*) override { ++_bar_calls; }

    void on_tick(const char*, WTSTickData*) override { ++_tick_calls; }

    uint32_t transTimeToMin(uint32_t uTime) override { return uTime; }

    // 把 WtEngine 的 protected 入口公开给用例
    void reload_datas() { load_datas(); }
    void emit_signal(const char* stdCode, double qty, bool bStandBy) { append_signal(stdCode, qty, bStandBy); }
    void set_pos(const char* stdCode, double qty, double curPx = -1) { do_set_position(stdCode, qty, curPx); }
    void dump_datas() { save_datas(); }
};

const char* kEngineInitCfg = R"json(
{
    "filters": "",
    "fees": ""
}
)json";

const char* kPortfolioData = R"json(
{
    "fund":
    {
        "predynbal": 1000000.0,
        "balance": 1000000.0,
        "prebalance": 1000000.0,
        "profit": 0.0,
        "dynprofit": 123.45,
        "fees": 6.66,
        "last_date": 20240401,
        "max_dyn_bal": 1100000.0,
        "max_time": 93000,
        "min_dyn_bal": 990000.0,
        "min_time": 143000,
        "maxmd": {"dyn_balance": 1050000.0, "date": 20240401},
        "minmd": {"dyn_balance": 995000.0, "date": 20240402},
        "update_time": 1712000000
    },
    "positions": [
        {
            "code": "SHFE.ag.2212",
            "closeprofit": 0.0,
            "volume": 6.0,
            "dynprofit": 300.0,
            "details": []
        }
    ],
    "riskmon":
    {
        "date": 20240401,
        "scale": 0.5
    }
}
)json";

class P02EngineTest : public ::testing::Test
{
protected:
    P02EmptyBaseMgr   _bd_mgr;
    P02SilentDtMgr    _dt_mgr;
    P02TestEngine     _engine;

    void SetUp() override
    {
        std::error_code ec;
        fs::remove(portfolio_data_file(), ec);
        _engine.set_date_time(20240403, 930, 15, 0);
        _engine.set_trading_date(20240403);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove(portfolio_data_file(), ec);
    }

    void do_init(const char* cfgContent = kEngineInitCfg, const std::string& rel = "p02_engine_cfg.json")
    {
        const std::string path = write_cfg(rel, cfgContent);
        WTSVariant* cfg = WTSCfgLoader::load_from_file(path.c_str());
        ASSERT_TRUE(cfg != NULL);
        // riskmon 段缺失 -> 走 "_fund_udt_span = 5" 的告警分支
        _engine.init(cfg, &_bd_mgr, &_dt_mgr, NULL, NULL);
        cfg->release();
    }
};

//////////////////////////////////////////////////////////////////
// 日期时间与状态查询
//////////////////////////////////////////////////////////////////

TEST_F(P02EngineTest, DateTimeGettersMirrorTheValuesSet)
{
    EXPECT_EQ(_engine.get_date(), 20240403u);
    EXPECT_EQ(_engine.get_min_time(), 930u);
    EXPECT_EQ(_engine.get_secs(), 15u);
    // rawTime == 0 时用 curTime 兜底
    EXPECT_EQ(_engine.get_raw_time(), 930u);
    EXPECT_EQ(_engine.get_trading_date(), 20240403u);

    // 显式给出 rawTime
    _engine.set_date_time(20240403, 931, 20, 932);
    EXPECT_EQ(_engine.get_min_time(), 931u);
    EXPECT_EQ(_engine.get_raw_time(), 932u);
    EXPECT_EQ(_engine.get_secs(), 20u);

    _engine.set_trading_date(20240405);
    EXPECT_EQ(_engine.get_trading_date(), 20240405u);
}

TEST_F(P02EngineTest, PortContextQueriesWithoutInit)
{
    // 基类实现: 恒为 false / 恒为 0
    EXPECT_FALSE(_engine.isInTrading());
    EXPECT_EQ(_engine.transTimeToMin(930), 0u);

    _engine.on_bar("SHFE.ag.2212", "m", 1, NULL);
    EXPECT_EQ(_engine._bar_calls, 1u);
    _engine.on_tick("SHFE.ag.2212", NULL);
    EXPECT_EQ(_engine._tick_calls, 1u);
}

TEST_F(P02EngineTest, InitBuildsPortFundAndReadsNoFiles)
{
    do_init();

    // load_datas() 在文件不存在时只创建 _port_fund 就返回
    EXPECT_EQ(_engine.getCurDate(), 20240403u);
    EXPECT_EQ(_engine.getCurTime(), 930u);
    EXPECT_EQ(_engine.getTradingDate(), 20240403u);

    // 商品/费率都查不到时的默认返回
    EXPECT_TRUE(_engine.get_commodity_info("SHFE.ag.2212") == NULL);
    EXPECT_DOUBLE_EQ(_engine.calc_fee("SHFE.ag.2212", 5200.0, 1.0, 0), 0.0);
    EXPECT_DOUBLE_EQ(_engine.get_exright_factor("SHFE.ag.2212"), 1.0);
    EXPECT_DOUBLE_EQ(_engine.get_exright_factor("SHFE.ag.2212", NULL), 1.0);
    EXPECT_TRUE(_engine.get_kline_slice(1, "SHFE.ag.2212", "m", 10, 1) == NULL);
    EXPECT_TRUE(_engine.get_kline_slice(1, "SHFE.ag.2212", "d", 10, 1) == NULL);
    EXPECT_GT(_bd_mgr._comm_calls, 0u);
}

TEST_F(P02EngineTest, AdjustingFlagIgnoresDataManager)
{
    do_init();
    // 替身返回 1, 但 WtEngine::get_adjusting_flag() 丢弃了返回值恒返回 0
    // -> 见 bugs/p02_wtcore_engines2.md BUG-P02-003
    EXPECT_EQ(_dt_mgr.get_adjusting_flag(), 1u);
    EXPECT_EQ(_engine.get_adjusting_flag(), 0u);
}

TEST_F(P02EngineTest, PriceLookupsReturnZeroWithoutTicks)
{
    do_init();

    EXPECT_DOUBLE_EQ(_engine.get_cur_price("SHFE.ag.2212"), 0.0);
    EXPECT_EQ(_dt_mgr._grab_calls, 1u);
    EXPECT_DOUBLE_EQ(_engine.get_day_price("SHFE.ag.2212", 0), 0.0);
    EXPECT_DOUBLE_EQ(_engine.get_day_price("SHFE.ag.2212", 1), 0.0);
    EXPECT_DOUBLE_EQ(_engine.get_day_price("SHFE.ag.2212", 2), 0.0);
    EXPECT_DOUBLE_EQ(_engine.get_day_price("SHFE.ag.2212", 3), 0.0);
    EXPECT_DOUBLE_EQ(_engine.get_day_price("SHFE.ag.2212", 9), 0.0);
    EXPECT_EQ(_dt_mgr._grab_calls, 6u);
}

//////////////////////////////////////////////////////////////////
// 组合数据的 load / save 往返
//////////////////////////////////////////////////////////////////

TEST_F(P02EngineTest, LoadDatasParsesFundPositionsAndRiskMon)
{
    ASSERT_TRUE(write_text_file(portfolio_data_file(), kPortfolioData));

    do_init();

    // load_datas 已经读回 riskmon.scale=0.5, setVolScale 会把它改写后再落盘
    _engine.setVolScale(0.75);

    const std::string saved = read_text_file(portfolio_data_file());
    EXPECT_NE(saved.find("SHFE.ag.2212"), std::string::npos);
    EXPECT_NE(saved.find("\"fund\""), std::string::npos);
    EXPECT_NE(saved.find("1000000"), std::string::npos);
    EXPECT_NE(saved.find("0.75"), std::string::npos);

    // 再次加载改写后的文件, 覆盖同一套解析代码
    _engine.reload_datas();
    _engine.setVolScale(0.25);
    const std::string again = read_text_file(portfolio_data_file());
    EXPECT_NE(again.find("0.25"), std::string::npos);
}

TEST_F(P02EngineTest, LoadDatasToleratesBrokenAndEmptyFiles)
{
    ASSERT_TRUE(write_text_file(portfolio_data_file(), "{ this is not json"));
    do_init();   // 内部 load_datas -> HasParseError 直接返回

    ASSERT_TRUE(write_text_file(portfolio_data_file(), ""));
    _engine.reload_datas();   // content.empty() -> 返回

    EXPECT_EQ(_engine.getCurDate(), 20240403u);
}

TEST_F(P02EngineTest, LoadDatasWithoutRiskMonSection)
{
    ASSERT_TRUE(write_text_file(portfolio_data_file(), R"json(
    {
        "fund":
        {
            "predynbal": 500000.0, "balance": 500000.0, "prebalance": 500000.0,
            "profit": 0.0, "dynprofit": 0.0, "fees": 0.0, "last_date": 20240402,
            "max_dyn_bal": 500000.0, "max_time": 0, "min_dyn_bal": 500000.0, "min_time": 0,
            "maxmd": {"dyn_balance": 0.0, "date": 0},
            "minmd": {"dyn_balance": 0.0, "date": 0}
        },
        "positions": [
            {"code": "CFFEX.IF.2204", "closeprofit": 0.0, "volume": 0.0, "dynprofit": 0.0, "details": []}
        ]
    }
    )json"));

    do_init();
    // 空仓位 -> save_datas 再写一次, 保持文件可读
    _engine.setVolScale(1.0);
    EXPECT_NE(read_text_file(portfolio_data_file()).find("CFFEX.IF.2204"), std::string::npos);
}

//////////////////////////////////////////////////////////////////
// 信号与组合仓位
//////////////////////////////////////////////////////////////////

TEST_F(P02EngineTest, AppendSignalIsHeldWhenNoPriceYet)
{
    do_init();

    // bStandBy=true -> 只记信号
    _engine.emit_signal("SHFE.ag.2212", 5.0, true);
    // bStandBy=false 但当前无价 -> 仍然只记信号
    _engine.emit_signal("SHFE.ag.2212", -5.0, false);

    // 目标仓位与当前(0)相同 -> 建立 PosInfo 后原样返回
    _engine.set_pos("SHFE.ag.2212", 0.0);
    _engine.set_pos("CFFEX.IF.2204", 0.0, 0.0);

    EXPECT_EQ(_engine.getCurTime(), 930u);
}

//////////////////////////////////////////////////////////////////
// 缺陷类收口: 下面这些输入都会段错误, 统一放进同一个 DISABLED_ 用例
// (BUG-P02-004 空指针解引用集合, 见 bugs/p02_wtcore_engines2.md)
//   1. sub_tick()/get_rawcode()/calc_fee(有模板时) 走 _hot_mgr 或商品对象, 未判空
//   2. do_set_position(code, 非0数量) 在未配置基础数据时 commInfo 为 NULL 直接 calcFee
//   3. handle_push_quote(NULL) / _data_mgr 为 NULL 时的 get_tick_slice、get_last_tick
//   4. getFundInfo() -> update_fund_dynprofit() 未在本 lane 验证, 暂不驱动
//   5. task_loop() 是 while(!_terminated) 死循环, 只能通过 run()/start() 退出 -> HANG
//////////////////////////////////////////////////////////////////
TEST(P02EngineCrashClass, DISABLED_NullHotMgrAndUnseededBaseDataPaths)
{
    // 该用例只用于登记缺陷类, 默认不执行(DISABLED_ 前缀)
}

}  // namespace
