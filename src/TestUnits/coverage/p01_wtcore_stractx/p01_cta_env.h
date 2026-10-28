/**
 * p01_cta_env.h —— lane p01（WtCore 策略上下文）共用替身
 *
 * 提供：
 *   - P01TempEnv       ：把 WtHelper 的生成目录重定向到 temp_directory_path()/p01_xxx_<pid>/
 *   - FakeBaseDataMgr  ：IBaseDataMgr 替身（品种/合约/时段注册表）
 *   - FakeHotMgr       ：IHotMgr 替身（一律返回空规则，即不做主力换月）
 *   - P01CtaEngine     ：WtCtaEngine 派生类，把 protected 的 _base_data_mgr/_hot_mgr 接上替身
 *   - P01CtaCtx        ：CtaStraBaseCtx 的最小具体子类（只补齐 4 个策略回调）
 * 注意：_data_mgr(WtDtMgr*) 故意保持 NULL，因此任何走到历史数据切片的接口都不能调用，
 *       详见 manifest 的跳过清单。
 */
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/stat.h>

#include <boost/core/noncopyable.hpp>

#include <Includes/IBaseDataMgr.h>
#include <Includes/IHotMgr.h>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSSessionInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSMarcos.h>
#include <Includes/WTSRiskDef.hpp>

#include <WtCore/WtCtaEngine.h>
#include <WtCore/CtaStraBaseCtx.h>
#include <WtCore/WtHelper.h>

USING_NS_WTP;

/**
 * 落地目录：temp_directory_path()/p01_<tag>_<pid>/
 * 同一进程内只生成一次（WtHelper 的目录 getter 内部是 static 记忆化的，
 * 所以必须在任何目录 getter 之前 setGenerateDir）。
 */
class P01TempEnv
{
public:
    explicit P01TempEnv(const char* tag)
    {
        static std::string cached;
        if (cached.empty())
        {
            std::filesystem::path dir = std::filesystem::temp_directory_path()
                / ("p01_" + std::string(tag) + "_" + std::to_string(::getpid()));
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir, ec);
            cached = dir.string() + "/";
        }
        WtHelper::setGenerateDir(cached.c_str());
        _dir = cached;
    }

    ~P01TempEnv()
    {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(_dir), ec);
    }

    const std::string& dir() const { return _dir; }

private:
    std::string _dir;
};

/** 基础数据替身：exchg|pid -> 品种，code|exchg -> 合约，sid -> 时段 */
class FakeBaseDataMgr : public IBaseDataMgr
{
public:
    virtual ~FakeBaseDataMgr() {}

    WTSSessionInfo* session(const char* sid = "TRADING")
    {
        auto it = _sessions.find(sid);
        return it == _sessions.end() ? NULL : it->second;
    }

    void regisSession(const char* sid)
    {
        WTSSessionInfo* s = WTSSessionInfo::create(sid, sid, 0);
        _sessions[sid] = s;
        _sess_list.push_back(s);
    }

    void regis(WTSCommodityInfo* comm, WTSContractInfo* ct)
    {
        ct->setCommInfo(comm);
        _comm_infos.push_back(comm);
        _contracts.push_back(ct);
        _key_to_ct[std::string(ct->getCode()) + "|" + ct->getExchg()] = ct;
        _key_to_comm[std::string(comm->getProduct()) + "|" + comm->getExchg()] = comm;
    }

    virtual WTSCommodityInfo* getCommodity(const char* exchgpid) override
    {
        for (auto* c : _comm_infos)
        {
            if (strcmp(c->getFullPid(), exchgpid) == 0)
                return c;
        }
        return NULL;
    }

    virtual WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
    {
        auto it = _key_to_comm.find(std::string(pid) + "|" + exchg);
        return it == _key_to_comm.end() ? NULL : it->second;
    }

    virtual WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override
    {
        (void)uDate;
        auto it = _key_to_ct.find(std::string(code) + "|" + exchg);
        if (it != _key_to_ct.end())
            return it->second;

        for (auto& kv : _key_to_ct)
        {
            if (kv.first.compare(0, strlen(code), code) == 0 && kv.first[strlen(code)] == '|')
                return kv.second;
        }
        return NULL;
    }

    virtual WTSArray* getContracts(const char* exchg = "", uint32_t uDate = 0) override
    {
        (void)exchg; (void)uDate;
        return NULL;
    }

    virtual WTSSessionInfo* getSession(const char* sid) override
    {
        if (_sess_list.empty())
            return NULL;
        WTSSessionInfo* ret = session(sid);
        return ret == NULL ? _sess_list.front() : ret;
    }

    virtual WTSSessionInfo* getSessionByCode(const char* code, const char* exchg = "") override
    {
        WTSContractInfo* ct = getContract(code, exchg);
        if (ct == NULL || ct->getCommInfo() == NULL)
            return NULL;
        return getSession(ct->getCommInfo()->getSession());
    }

    virtual WTSArray* getAllSessions() override { return NULL; }

    virtual bool isHoliday(const char* pid, uint32_t uDate, bool isTpl = false) override
    {
        (void)pid; (void)uDate; (void)isTpl;
        return false;
    }

    virtual uint32_t calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession = false) override
    {
        (void)stdPID; (void)uTime; (void)isSession;
        return uDate;
    }

    virtual uint64_t getBoundaryTime(const char* stdPID, uint32_t tDate, bool isSession = false, bool isStart = true) override
    {
        (void)stdPID; (void)isSession; (void)isStart;
        return tDate;
    }

private:
    wt_hashmap<std::string, WTSContractInfo*>   _key_to_ct;
    wt_hashmap<std::string, WTSCommodityInfo*>  _key_to_comm;
    wt_hashmap<std::string, WTSSessionInfo*>    _sessions;
    std::vector<WTSCommodityInfo*>              _comm_infos;
    std::vector<WTSContractInfo*>               _contracts;
    std::vector<WTSSessionInfo*>                _sess_list;
};

/** 主力规则替身：不做任何主力/次主力/自定义映射 */
class FakeHotMgr : public IHotMgr
{
public:
    virtual ~FakeHotMgr() {}

    virtual const char* getRawCode(const char* exchg, const char* pid, uint32_t dt) override
    { (void)exchg; (void)pid; (void)dt; return ""; }
    virtual const char* getPrevRawCode(const char* exchg, const char* pid, uint32_t dt) override
    { (void)exchg; (void)pid; (void)dt; return ""; }
    virtual bool isHot(const char* exchg, const char* rawCode, uint32_t dt) override
    { (void)exchg; (void)rawCode; (void)dt; return false; }
    virtual bool splitHotSecions(const char* exchg, const char* hotCode, uint32_t sDt, uint32_t eDt, HotSections& sections) override
    { (void)exchg; (void)hotCode; (void)sDt; (void)eDt; (void)sections; return false; }
    virtual const char* getSecondRawCode(const char* exchg, const char* pid, uint32_t dt) override
    { (void)exchg; (void)pid; (void)dt; return ""; }
    virtual const char* getPrevSecondRawCode(const char* exchg, const char* pid, uint32_t dt) override
    { (void)exchg; (void)pid; (void)dt; return ""; }
    virtual bool isSecond(const char* exchg, const char* rawCode, uint32_t dt) override
    { (void)exchg; (void)rawCode; (void)dt; return false; }
    virtual bool splitSecondSecions(const char* exchg, const char* hotCode, uint32_t sDt, uint32_t eDt, HotSections& sections) override
    { (void)exchg; (void)hotCode; (void)sDt; (void)eDt; (void)sections; return false; }
    virtual const char* getCustomRawCode(const char* tag, const char* fullPid, uint32_t dt = 0) override
    { (void)tag; (void)fullPid; (void)dt; return ""; }
    virtual const char* getPrevCustomRawCode(const char* tag, const char* fullPid, uint32_t dt = 0) override
    { (void)tag; (void)fullPid; (void)dt; return ""; }
    virtual bool isCustomHot(const char* tag, const char* fullCode, uint32_t d = 0) override
    { (void)tag; (void)fullCode; (void)d; return false; }
    virtual bool splitCustomSections(const char* tag, const char* hotCode, uint32_t sDt, uint32_t eDt, HotSections& sections) override
    { (void)tag; (void)hotCode; (void)sDt; (void)eDt; (void)sections; return false; }
    virtual const char* getRuleTag(const char* stdCode) override
    { (void)stdCode; return ""; }
    virtual double getRuleFactor(const char* ruleTag, const char* fullPid, uint32_t uDate = 0) override
    { (void)ruleTag; (void)fullPid; (void)uDate; return 0; }
};

/** 只接上基础数据/主力规则的裸引擎：不 init、不 run、不起线程 */
class P01CtaEngine : public WtCtaEngine
{
public:
    P01CtaEngine()
    {
        // WtEngine::init 会创建组合资金对象（WtEngine.cpp:475），裸引擎没有走 init，
        // 但引擎级 do_set_position 需要 _port_fund（WtEngine.cpp:985/1078）。
        _port_fund = WTSPortFundInfo::create();
    }

    virtual ~P01CtaEngine()
    {
        if (_port_fund != NULL)
        {
            _port_fund->release();
            _port_fund = NULL;
        }
    }

    void attach(IBaseDataMgr* bd, IHotMgr* hot)
    {
        _base_data_mgr = bd;
        _hot_mgr = hot;
        set_date_time(20250110, 931, 0);
        set_trading_date(20250110);
    }

    void setNow(uint32_t date, uint32_t mins, uint32_t tdate)
    {
        set_date_time(date, mins, 0);
        set_trading_date(tdate);
    }

    /**
     * 只在测试侧预置引擎的实时价格缓存。
     * CtaStraBaseCtx::do_set_position 会把变化量回提交给 WtCtaEngine::handle_pos_change，
     * 后者在 WtEngine::append_signal 里调用 get_cur_price；该函数在 _price_map 未命中时会
     * 落到 _data_mgr->grab_last_tick。本夹具的 _data_mgr 故意为 NULL（见文件头），
     * 因此测试必须先喂价格，否则会在空数据管理器上解引用（生产侧空指针，不属于本 lane 的修复范围）。
     */
    void prime_price(const char* code, double price)
    {
        _price_map[code] = price;
    }
};

/** CtaStraBaseCtx 的具体子类：只补齐策略回调，其余全部走被测基类实现 */
class P01CtaCtx : public CtaStraBaseCtx
{
public:
    P01CtaCtx(WtCtaEngine* engine, const char* name, int32_t slippage = 0)
        : CtaStraBaseCtx(engine, name, slippage) {}

    virtual ~P01CtaCtx() {}

    virtual void on_tick_updated(const char* stdCode, WTSTickData* newTick) override
    {
        ++tick_updates;
        if (newTick != NULL)
            _last_upd_code = stdCode;
    }

    virtual void on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) override
    {
        ++bar_closes;
        _last_close_code = stdCode;
        _last_close_period = period;
        _last_close_bar = newBar;
    }

    virtual void on_calculate(uint32_t curDate, uint32_t curTime) override
    {
        ++calc_times;
        _last_calc_date = curDate;
        _last_calc_time = curTime;
    }

    virtual void on_condition_triggered(const char* stdCode, double target, double price, const char* usertag) override
    {
        ++cond_fires;
        _cond_code = stdCode;
        _cond_target = target;
        _cond_price = price;
        _cond_tag = usertag;
    }

    uint32_t tick_updates = 0;
    uint32_t bar_closes = 0;
    uint32_t calc_times = 0;
    uint32_t cond_fires = 0;
    std::string _last_upd_code;
    std::string _last_close_code;
    std::string _last_close_period;
    WTSBarStruct* _last_close_bar = NULL;
    uint32_t _last_calc_date = 0;
    uint32_t _last_calc_time = 0;
    std::string _cond_code;
    double _cond_target = 0;
    double _cond_price = 0;
    std::string _cond_tag;
};

inline WTSCommodityInfo* p01MakeComm(const char* exchg, const char* pid, const char* session,
    TradingMode tm, ContractCategory cat = CC_Future)
{
    WTSCommodityInfo* comm = WTSCommodityInfo::create(pid, pid, exchg, session, "CHINA");
    comm->setCategory(cat);
    comm->setPriceTick(1.0);
    comm->setLotsTick(1.0);
    comm->setMinLots(1.0);
    comm->setVolScale(10);
    comm->setTradingMode(tm);
    return comm;
}

inline WTSContractInfo* p01MakeCt(const char* exchg, const char* pid, const char* code)
{
    return WTSContractInfo::create(code, code, exchg, pid);
}

inline WTSTickData* p01MakeTick(const char* exchg, const char* code, uint32_t adate,
    uint32_t tdate, uint32_t atime, double price)
{
    WTSTickStruct st;
    memset(&st, 0, sizeof(st));
    wt_strcpy(st.exchg, exchg);
    wt_strcpy(st.code, code);
    st.action_date = adate;
    st.trading_date = tdate;
    st.action_time = atime;
    st.price = price;
    st.total_volume = 100;
    return WTSTickData::create(st);
}
