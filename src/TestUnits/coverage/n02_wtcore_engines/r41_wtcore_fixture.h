/**
 * r41_wtcore_engine_fixture.h  (lane R41 / wave10, WtCore engine 补测)
 *
 * 本文件只提供 R41 lane 自用的替身与夹具，不进入任何生产代码路径。
 * 依赖的生产类型全部是公开头文件里的接口/工厂：IBaseDataMgr、IHotMgr、
 * WTSCommodityInfo::create、WTSContractInfo::create、WTSSessionInfo::create、
 * WtDtMgr（默认构造）以及 WTSVariant/WTSCfgLoader。
 *
 * 目录/生命周期约定：
 *  - 每个用例在系统临时目录下建一个唯一子目录，generate 目录在全局环境里
 *    指向该 lane 自己的 `.../generated`，用例结束即整棵删除；
 *  - 引擎写入的 datas.json/trades.csv/closes.csv/funds.csv 固定在
 *    `WtHelper::getPortifolioDir()` 下，由夹具在 SetUp/TearDown 双端清理；
 *  - 所有替身都晚于引擎析构（引擎是 test body 内的局部对象），
 *    因此引擎析构时不会留下悬空引用。
 */
#pragma once

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "Includes/IBaseDataMgr.h"
#include "Includes/IHotMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSRiskDef.hpp"
#include "WTSUtils/WTSCfgLoader.h"
#include "WtCore/WtDtMgr.h"
#include "WtCore/WtEngine.h"
#include "WtCore/WtHelper.h"
#include "WtCore/TraderAdapter.h"

USING_NS_WTP;

namespace r41 {

namespace stdfs = std::filesystem;

inline std::string g_portfolio_dir;

/** 系统临时目录下的唯一目录，析构时整棵删除。 */
class TempDir
{
public:
    explicit TempDir(const std::string& tag)
    {
        static uint32_t seq = 0;
        std::string name = "wt_r41_" + tag + "_" +
                           std::to_string(static_cast<long long>(::getpid())) + "_" +
                           std::to_string(++seq);
        _path = stdfs::temp_directory_path() / name;
        std::error_code ec;
        stdfs::remove_all(_path, ec);
        stdfs::create_directories(_path, ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        stdfs::remove_all(_path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string path(const std::string& rel = "") const
    {
        return rel.empty() ? _path.string() : (_path / rel).string();
    }

    void write(const std::string& rel, const std::string& content) const
    {
        stdfs::path p = _path / rel;
        stdfs::create_directories(p.parent_path());
        std::ofstream ofs(p.string().c_str(), std::ios::binary | std::ios::trunc);
        ofs << content;
        ofs.flush();
    }

private:
    stdfs::path _path;
};

//////////////////////////////////////////////////////////////////
// 基础数据替身: 只放一个期货品种 rb / 合约 rb2310 / 日盘 session
//////////////////////////////////////////////////////////////////
class BaseDataMgrFake final : public IBaseDataMgr
{
public:
    BaseDataMgrFake()
    {
        _session = WTSSessionInfo::create("TRADING", "day-session");
        _session->addTradingSection(900, 1500);

        _rb = WTSCommodityInfo::create("rb", "rebar", "SHFE", "TRADING", "CHINA");
        _rb->setVolScale(10);
        _rb->setPriceTick(1.0);
        _rb->setCategory(CC_Future);
        _rb->setTradingMode(TM_Both);
        _rb->setSessionInfo(_session);

        _rbContract = WTSContractInfo::create("rb2310", "rb2310", "SHFE", "rb");
        _rbContract->setVolScale(10);
        _rbContract->setCommInfo(_rb);
        // 带一个主力映射, 让 trigger_price/handle_push_quote 的热合约循环可达
        _rbContract->addHotCode("SHFE.rb.HOT");
    }

    ~BaseDataMgrFake()
    {
        _rbContract->release();
        _rb->release();
        _session->release();
    }

    WTSCommodityInfo* getCommodity(const char* exchgpid) override
    {
        std::string s(exchgpid == NULL ? "" : exchgpid);
        std::string::size_type pos = s.find('.');
        if (pos == std::string::npos)
            return NULL;
        return getCommodity(s.substr(0, pos).c_str(), s.substr(pos + 1).c_str());
    }

    WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
    {
        if (exchg != NULL && pid != NULL &&
            std::string(exchg) == "SHFE" && std::string(pid) == "rb")
            return _rb;
        return NULL;
    }

    WTSContractInfo* getContract(const char* code, const char* exchg, uint32_t) override
    {
        if (code != NULL && exchg != NULL &&
            std::string(code) == "rb2310" && std::string(exchg) == "SHFE")
            return _rbContract;
        return NULL;
    }

    WTSArray* getContracts(const char*, uint32_t) override { return NULL; }

    WTSSessionInfo* getSession(const char* sid) override
    {
        if (sid != NULL && _session->id() != NULL && std::string(sid) == _session->id())
            return _session;
        return NULL;
    }

    WTSSessionInfo* getSessionByCode(const char*, const char*) override { return _session; }

    WTSArray* getAllSessions() override { return NULL; }

    bool isHoliday(const char*, uint32_t, bool) override { return false; }

    uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool) override
    {
        // 不使用真实日历: 原样返回, 保证用例确定性
        return uDate;
    }

    uint64_t getBoundaryTime(const char*, uint32_t, bool, bool) override { return 0; }

    WTSSessionInfo* session() { return _session; }
    WTSContractInfo* contract() { return _rbContract; }
    WTSCommodityInfo* commodity() { return _rb; }

private:
    WTSSessionInfo* _session = nullptr;
    WTSCommodityInfo* _rb = nullptr;
    WTSContractInfo* _rbContract = nullptr;
};

//////////////////////////////////////////////////////////////////
// 主力/规则替身: 普通代码无规则; 带 ".R41" 的代码返回规则标签
//////////////////////////////////////////////////////////////////
class HotMgrFake final : public IHotMgr
{
public:
    const char* getRawCode(const char*, const char*, uint32_t) override { return ""; }
    const char* getPrevRawCode(const char*, const char*, uint32_t) override { return ""; }
    bool isHot(const char*, const char*, uint32_t) override { return false; }
    bool splitHotSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override
    {
        return false;
    }

    const char* getSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
    const char* getPrevSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
    bool isSecond(const char*, const char*, uint32_t) override { return false; }
    bool splitSecondSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override
    {
        return false;
    }

    const char* getCustomRawCode(const char*, const char*, uint32_t) override { return "rb2310"; }
    const char* getPrevCustomRawCode(const char*, const char*, uint32_t) override { return "rb2310"; }
    bool isCustomHot(const char*, const char*, uint32_t) override { return false; }
    bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections&) override
    {
        return false;
    }

    const char* getRuleTag(const char* stdCode) override
    {
        // CodeHelper 传入的是最后一段(如 "R41"), 也可能传入完整标准代码
        if (stdCode != NULL && std::string(stdCode).find("R41") != std::string::npos)
            return "R41";
        return "";
    }

    double getRuleFactor(const char*, const char*, uint32_t) override { return 2.0; }
};

//////////////////////////////////////////////////////////////////
// 事件监听替身: 记录引擎回调次数, 用于断言事件顺序/次数
//////////////////////////////////////////////////////////////////
class EvtListenerFake : public IEngineEvtListener
{
public:
    void on_initialize_event() override { ++_init; }

    void on_schedule_event(uint32_t uDate, uint32_t uTime) override
    {
        ++_sched;
        _last_sched_date = uDate;
        _last_sched_time = uTime;
    }

    void on_session_event(uint32_t uDate, bool isBegin) override
    {
        ++_session;
        _last_session_date = uDate;
        _last_session_begin = isBegin;
    }

    uint32_t _init = 0;
    uint32_t _sched = 0;
    uint32_t _session = 0;
    uint32_t _last_sched_date = 0;
    uint32_t _last_sched_time = 0;
    uint32_t _last_session_date = 0;
    bool _last_session_begin = false;
};

//////////////////////////////////////////////////////////////////
// 夹具
//////////////////////////////////////////////////////////////////
class R41EngineFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // generate 根目录由 R41GenerateDirEnv 统一指向本 lane 的临时目录,
        // 这里只取一次 portfolio 子目录, 不在每个用例里重置静态缓存。
        g_portfolio_dir = WtHelper::getPortifolioDir();

        _bd.reset(new BaseDataMgrFake());
        _hot.reset(new HotMgrFake());
        _dt.reset(new WtDtMgr());

        removePortfolioFiles();
    }

    void TearDown() override
    {
        for (WTSVariant* cfg : _cfg_owned)
            cfg->release();
        _cfg_owned.clear();
        _dt.reset();
        _hot.reset();
        _bd.reset();
        removePortfolioFiles();
    }

    static std::string portfolioFile(const std::string& name)
    {
        return g_portfolio_dir + name;
    }

    void removePortfolioFiles() const
    {
        static const char* const files[] = {"datas.json", "trades.csv", "closes.csv", "funds.csv"};
        std::error_code ec;
        for (const char* f : files)
            stdfs::remove(portfolioFile(f), ec);
    }

    void writePortfolio(const std::string& name, const std::string& content) const
    {
        stdfs::create_directories(g_portfolio_dir);
        std::ofstream ofs(portfolioFile(name).c_str(), std::ios::binary | std::ios::trunc);
        ofs << content;
        ofs.flush();
    }

    std::string readPortfolio(const std::string& name) const
    {
        std::ifstream ifs(portfolioFile(name).c_str(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }

    std::string writeFees()
    {
        std::string content =
            "{\n"
            "  \"SHFE.rb\": { \"open\": 2.0, \"close\": 2.0, \"closetoday\": 3.0,"
            " \"byvolume\": true, \"margin\": 0.12 }\n"
            "}\n";
        _tmp.write("fees.json", content);
        return _tmp.path("fees.json");
    }

    /** 生成引擎配置; feesPath 为空串表示不加载手续费模板。 */
    WTSVariant* makeCfg(const std::string& feesPath = "", uint32_t poolsize = 0,
                        const std::string& extra = "")
    {
        std::string content =
            "{\n"
            "  \"product\": { \"session\": \"TRADING\" },\n"
            "  \"poolsize\": " + std::to_string(poolsize) + ",\n"
            "  \"fees\": \"" + feesPath + "\"" + extra + "\n"
            "}\n";
        _tmp.write("engine_cfg.json", content);
        WTSVariant* cfg = WTSCfgLoader::load_from_file(_tmp.path("engine_cfg.json").c_str());
        if (cfg != NULL)
            _cfg_owned.push_back(cfg);
        return cfg;
    }

public:
    WTSTickData* makeTick(const char* code, double price, double volume = 1,
                          uint32_t tdate = 20231010, uint32_t actionTime = 93000000,
                          uint32_t actionDate = 20231010) const
    {
        WTSTickData* tick = WTSTickData::create(code);
        WTSTickStruct& ts = tick->getTickStruct();
        ts.price = price;
        ts.open = price - 1;
        ts.high = price + 1;
        ts.low = price - 2;
        ts.pre_close = price - 1;
        ts.pre_settle = price - 1;
        ts.settle_price = price;
        ts.volume = volume;
        ts.total_volume = volume;
        ts.turn_over = volume * price;
        ts.total_turnover = volume * price;
        ts.open_interest = 100;
        ts.trading_date = tdate;
        ts.action_date = actionDate;
        ts.action_time = actionTime;
        tick->setContractInfo(_bd->contract());
        return tick;
    }

protected:
    TempDir _tmp{"engine"};
    std::unique_ptr<BaseDataMgrFake> _bd;
    std::unique_ptr<HotMgrFake> _hot;
    std::unique_ptr<WtDtMgr> _dt;
    std::vector<WTSVariant*> _cfg_owned;
    TraderAdapterMgr _adapter_mgr;
};

}  // namespace r41

// 让本 lane 的所有引擎输出都落到系统临时目录。全局环境在任何用例之前 SetUp,
// 因此不受同进程其他 lane 的 WtHelper::get*Dir 静态缓存影响。
class R41GenerateDirEnv : public ::testing::Environment
{
public:
    void SetUp() override
    {
        WtHelper::setGenerateDir(_dir.path("generated").c_str());
    }

    void TearDown() override {}

private:
    r41::TempDir _dir{"generated"};
};
