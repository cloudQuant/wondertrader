/**
 * r66_fixture.h  (lane R66 / wave13)
 *
 * 本 lane 自用夹具，只依赖公开头文件里的工厂/接口：
 *   - BaseDataMgrFake：ag2512(带 SHFE.ag.HOT 热合约) + au2212(无热合约) + 900-1500 日盘；
 *   - HotMgrFake     ：无主力/自定义规则，规则因子固定 2.0；
 *   - FakeReader     ：IDataReader 替身，只记录 onMinuteEnd；
 *   - EvtListenerFake：记录引擎 initialize/schedule/session 事件；
 *   - R66HftEngine   ：只把 WtEngine 的 protected 订阅表暴露成测试入口；
 *   - R66HftCtx      ：HftStraBaseCtx 具体子类，记录引擎回调次数；
 *   - R66Fixture     ：临时目录 + 基础数据/数据管理器 + 引擎配置。
 *
 * 每个用例在系统临时目录下建唯一目录，用例结束整棵删除。
 * 覆盖率套件按“一条用例一个进程”运行，因此这里不依赖跨用例静态状态。
 */
#pragma once

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include <boost/core/noncopyable.hpp>

#include "Includes/IBaseDataMgr.h"
#include "Includes/IDataReader.h"
#include "Includes/IHotMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSVariant.hpp"
#include "WTSUtils/WTSCfgLoader.h"

#include "WtCore/HftStraBaseCtx.h"
#include "WtCore/TraderAdapter.h"
#include "WtCore/WtDtMgr.h"
#include "WtCore/WtHelper.h"
#include "WtCore/WtHftEngine.h"

USING_NS_WTP;

namespace r66 {

namespace stdfs = std::filesystem;

inline std::string g_generate_dir;

/** 系统临时目录下的唯一目录，析构时整棵删除。 */
class TempDir
{
public:
    explicit TempDir(const char* tag)
    {
        static uint32_t seq = 0;
        std::string name = std::string("wt_r66_") + tag + "_" +
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
        return (rel.empty() ? _path : (_path / rel)).string();
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
// 基础数据替身
//////////////////////////////////////////////////////////////////
class BaseDataMgrFake final : public IBaseDataMgr
{
public:
    BaseDataMgrFake()
    {
        _session = WTSSessionInfo::create("TRADING", "day-session");
        _session->addTradingSection(900, 1500);

        _ag = WTSCommodityInfo::create("ag", "silver", "SHFE", "TRADING", "CHINA");
        _ag->setVolScale(10);
        _ag->setPriceTick(1.0);
        _ag->setCategory(CC_Future);
        _ag->setTradingMode(TM_Both);
        _ag->setSessionInfo(_session);

        _agContract = WTSContractInfo::create("ag2512", "ag2512", "SHFE", "ag");
        _agContract->setVolScale(10);
        _agContract->setCommInfo(_ag);
        // 热合约映射：让三个 ticker 的 trigger_price 热合约循环可达
        _agContract->addHotCode("SHFE.ag.HOT");

        _au = WTSCommodityInfo::create("au", "gold", "SHFE", "TRADING", "CHINA");
        _au->setVolScale(10);
        _au->setPriceTick(1.0);
        _au->setCategory(CC_Future);
        _au->setTradingMode(TM_Both);
        _au->setSessionInfo(_session);

        // 无热合约映射：覆盖 isFlat()==true 的短路分支
        _auContract = WTSContractInfo::create("au2212", "au2212", "SHFE", "au");
        _auContract->setVolScale(10);
        _auContract->setCommInfo(_au);
    }

    ~BaseDataMgrFake()
    {
        _auContract->release();
        _au->release();
        _agContract->release();
        _ag->release();
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
        if (exchg == NULL || pid == NULL)
            return NULL;
        if (std::string(exchg) == "SHFE" && std::string(pid) == "ag")
            return _ag;
        if (std::string(exchg) == "SHFE" && std::string(pid) == "au")
            return _au;
        return NULL;
    }

    WTSContractInfo* getContract(const char* code, const char* exchg, uint32_t) override
    {
        if (code == NULL || exchg == NULL || std::string(exchg) != "SHFE")
            return NULL;
        if (std::string(code) == "ag2512")
            return _agContract;
        if (std::string(code) == "au2212")
            return _auContract;
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

    uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool) override { return uDate; }

    uint64_t getBoundaryTime(const char*, uint32_t, bool, bool) override { return 0; }

    WTSSessionInfo* session() { return _session; }
    WTSContractInfo* agContract() { return _agContract; }
    WTSContractInfo* auContract() { return _auContract; }

private:
    WTSSessionInfo* _session = nullptr;
    WTSCommodityInfo* _ag = nullptr;
    WTSCommodityInfo* _au = nullptr;
    WTSContractInfo* _agContract = nullptr;
    WTSContractInfo* _auContract = nullptr;
};

/** 主力/规则替身：无规则标签；规则因子固定 2.0，便于复权路径断言。 */
class HotMgrFake final : public IHotMgr
{
public:
    const char* getRawCode(const char*, const char*, uint32_t) override { return ""; }
    const char* getPrevRawCode(const char*, const char*, uint32_t) override { return ""; }
    bool isHot(const char*, const char*, uint32_t) override { return false; }
    bool splitHotSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }

    const char* getSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
    const char* getPrevSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
    bool isSecond(const char*, const char*, uint32_t) override { return false; }
    bool splitSecondSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }

    const char* getCustomRawCode(const char*, const char*, uint32_t) override { return ""; }
    const char* getPrevCustomRawCode(const char*, const char*, uint32_t) override { return ""; }
    bool isCustomHot(const char*, const char*, uint32_t) override { return false; }
    bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }

    const char* getRuleTag(const char*) override { return ""; }
    double getRuleFactor(const char*, const char*, uint32_t) override { return 2.0; }
};

//////////////////////////////////////////////////////////////////
// 数据读取器替身：只记录 onMinuteEnd
//////////////////////////////////////////////////////////////////
class FakeReader final : public IDataReader
{
public:
    void onMinuteEnd(uint32_t uDate, uint32_t uTime, uint32_t endTDate = 0) override
    {
        ++count;
        last_date = uDate;
        last_time = uTime;
        last_end_tdate = endTDate;
    }

    WTSTickSlice* readTickSlice(const char*, uint32_t, uint64_t = 0) override { return NULL; }
    WTSKlineSlice* readKlineSlice(const char*, WTSKlinePeriod, uint32_t, uint64_t = 0) override { return NULL; }

    uint32_t count = 0;
    uint32_t last_date = 0;
    uint32_t last_time = 0;
    uint32_t last_end_tdate = 0;
};

//////////////////////////////////////////////////////////////////
// 引擎事件监听替身
//////////////////////////////////////////////////////////////////
class EvtListenerFake final : public IEngineEvtListener
{
public:
    void on_initialize_event() override { ++init; }

    void on_schedule_event(uint32_t uDate, uint32_t uTime) override
    {
        ++sched;
        last_sched_date = uDate;
        last_sched_time = uTime;
    }

    void on_session_event(uint32_t uDate, bool isBegin) override
    {
        ++session;
        last_session_date = uDate;
        last_session_begin = isBegin;
    }

    uint32_t init = 0;
    uint32_t sched = 0;
    uint32_t session = 0;
    uint32_t last_sched_date = 0;
    uint32_t last_sched_time = 0;
    uint32_t last_session_date = 0;
    bool last_session_begin = false;
};

//////////////////////////////////////////////////////////////////
// 把 protected 订阅表开放给用例；不是类布局镜像，只继承公开类
//////////////////////////////////////////////////////////////////
class R66HftEngine : public WtHftEngine
{
public:
    void subTickOpt(uint32_t sid, const char* code, uint32_t opt)
    {
        _tick_sub_map[code][sid] = SubOpt(sid, opt);
    }

    void subBar(uint32_t sid, const char* code, const char* period, uint32_t times)
    {
        std::string key = std::string(code) + "-" + period + "-" + std::to_string(times);
        _bar_sub_map[key][sid] = SubOpt(sid, 0);
    }
};

class R66HftCtx : public HftStraBaseCtx
{
public:
    R66HftCtx(WtHftEngine* engine, const char* name)
        : HftStraBaseCtx(engine, name, false, 0)
    {
    }

    void on_tick(const char* stdCode, WTSTickData*) override
    {
        ++ticks;
        last_tick_code = stdString(stdCode);
    }

    void on_bar(const char* stdCode, const char* period, uint32_t, WTSBarStruct*) override
    {
        ++bars;
        last_bar_key = stdString(stdCode) + "-" + period;
    }

    void on_order_detail(const char* stdCode, WTSOrdDtlData*) override
    {
        ++orddtl;
        last_detail_code = stdString(stdCode);
    }

    void on_order_queue(const char* stdCode, WTSOrdQueData*) override
    {
        ++ordque;
        last_queue_code = stdString(stdCode);
    }

    void on_transaction(const char* stdCode, WTSTransData*) override
    {
        ++trans;
        last_trans_code = stdString(stdCode);
    }

    void on_session_begin(uint32_t) override { ++session_begin; }
    void on_session_end(uint32_t) override { ++session_end; }

    uint32_t ticks = 0;
    uint32_t bars = 0;
    uint32_t orddtl = 0;
    uint32_t ordque = 0;
    uint32_t trans = 0;
    uint32_t session_begin = 0;
    uint32_t session_end = 0;
    std::string last_tick_code;
    std::string last_bar_key;
    std::string last_detail_code;
    std::string last_queue_code;
    std::string last_trans_code;

private:
    static std::string stdString(const char* s) { return s == NULL ? std::string() : std::string(s); }
};

//////////////////////////////////////////////////////////////////
// 夹具
//////////////////////////////////////////////////////////////////
class R66Fixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        g_generate_dir = _tmp.path("generated");
        stdfs::create_directories(g_generate_dir);
        WtHelper::setGenerateDir(g_generate_dir.c_str());

        _bd.reset(new BaseDataMgrFake());
        _hot.reset(new HotMgrFake());
        _dt.reset(new WtDtMgr());
    }

    void TearDown() override
    {
        for (WTSVariant* cfg : _cfg_owned)
            cfg->release();
        _cfg_owned.clear();
        _dt.reset();
        _hot.reset();
        _bd.reset();
    }

    /** 生成引擎配置；feesPath 为空表示不加载手续费模板。 */
    WTSVariant* makeCfg(uint32_t poolsize = 0)
    {
        std::string content =
            "{\n"
            "  \"product\": { \"session\": \"TRADING\" },\n"
            "  \"poolsize\": " + std::to_string(poolsize) + ",\n"
            "  \"fees\": \"\"\n"
            "}\n";
        _tmp.write("engine_cfg.json", content);
        WTSVariant* cfg = WTSCfgLoader::load_from_file(_tmp.path("engine_cfg.json").c_str());
        if (cfg != NULL)
            _cfg_owned.push_back(cfg);
        return cfg;
    }

public:
    WTSTickData* makeTick(const char* code, double price, WTSContractInfo* cInfo,
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
        ts.volume = 1;
        ts.total_volume = 1;
        ts.turn_over = price;
        ts.total_turnover = price;
        ts.open_interest = 100;
        ts.trading_date = tdate;
        ts.action_date = actionDate;
        ts.action_time = actionTime;
        tick->setContractInfo(cInfo);
        return tick;
    }

    WTSTickData* makeAgTick(double price, uint32_t actionTime = 93000000,
                            uint32_t actionDate = 20231010) const
    {
        return makeTick("SHFE.ag.2512", price, _bd->agContract(), 20231010, actionTime, actionDate);
    }

    WTSTickData* makeAuTick(double price, uint32_t actionTime = 93000000,
                            uint32_t actionDate = 20231010) const
    {
        return makeTick("SHFE.au.2212", price, _bd->auContract(), 20231010, actionTime, actionDate);
    }

    static WTSBarStruct makeBar(uint32_t date, uint32_t time, double price)
    {
        WTSBarStruct bar;
        std::memset(&bar, 0, sizeof(bar));
        bar.date = date;
        bar.time = time;
        bar.open = price;
        bar.high = price + 1;
        bar.low = price - 1;
        bar.close = price;
        bar.vol = 1;
        return bar;
    }

protected:
    TempDir _tmp{"main"};
    std::unique_ptr<BaseDataMgrFake> _bd;
    std::unique_ptr<HotMgrFake> _hot;
    std::unique_ptr<WtDtMgr> _dt;
    std::vector<WTSVariant*> _cfg_owned;
    TraderAdapterMgr _adapter_mgr;
};

}  // namespace r66
