/**
 * q01_sel_env.h —— lane q01（WtCore 选股策略上下文 SelStraBaseCtx）共用替身
 *
 * 提供（全部收在 namespace q01sel 里，避免与 p01/p11 的 global 同名替身 ODR 冲突）：
 *   - Q01TempEnv    ：把 WtHelper 生成目录重定向到 temp_directory_path()/q01sel_<tag>_<pid>/
 *   - BaseDataMgr   ：IBaseDataMgr 替身（品种/合约/时段注册表）
 *   - HotMgr        ：IHotMgr 替身（一律返回空规则，不做主力换月）
 *   - SelEngine     ：WtSelEngine 派生类，只把 protected 的 _base_data_mgr/_hot_mgr 接上替身
 *                     不 init / 不 run / 不起线程；_data_mgr 故意保持 NULL
 *   - SelCtx        ：SelStraBaseCtx 具体子类，只补齐纯虚 on_bar_close，
 *                     并记录 on_tick_updated/on_strategy_schedule 以便断言装配
 *
 * 说明：任何走到历史数据切片（get_day_price/get_kline_slice/get_tick_slice/get_last_tick）
 *       的接口都会解引用 _data_mgr（NULL），属崩溃类，收在 DISABLED 用例，本头不提供。
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
#include <Includes/WTSTypes.h>
#include <Includes/WTSMarcos.h>

#include <WtCore/WtSelEngine.h>
#include <WtCore/SelStraBaseCtx.h>
#include <WtCore/WtHelper.h>

namespace q01sel {

USING_NS_WTP;

/**
 * 落地目录：temp_directory_path()/q01sel_<tag>_<pid>/
 * 同一进程内只生成一次（WtHelper 目录 getter 内部 static 记忆化）。
 */
class Q01TempEnv
{
public:
    explicit Q01TempEnv(const char* tag)
    {
        static std::string cached;
        if (cached.empty())
        {
            std::filesystem::path dir = std::filesystem::temp_directory_path()
                / ("q01sel_" + std::string(tag) + "_" + std::to_string(::getpid()));
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir, ec);
            cached = dir.string() + "/";
        }
        WtHelper::setGenerateDir(cached.c_str());
        _dir = cached;
    }

    ~Q01TempEnv()
    {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(_dir), ec);
    }

    const std::string& dir() const { return _dir; }

private:
    std::string _dir;
};

/** 基础数据替身：code|exchg -> 合约，pid|exchg -> 品种，sid -> 时段 */
class BaseDataMgr : public IBaseDataMgr
{
public:
    virtual ~BaseDataMgr() {}

    WTSSessionInfo* session(const char* sid)
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

    // 带交易时段的会话（用于 isInTradingTime 判定；q01 启用用例故意不调用，避免走到 do_set_position）
    void regisSessionWithTime(const char* sid, uint32_t sTime, uint32_t eTime)
    {
        WTSSessionInfo* s = WTSSessionInfo::create(sid, sid, 0);
        s->addTradingSection(sTime, eTime);
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
class HotMgr : public IHotMgr
{
public:
    virtual ~HotMgr() {}

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

/** 只接上基础数据/主力规则的裸选股引擎：不 init、不 run、不起线程 */
class SelEngine : public WtSelEngine
{
public:
    SelEngine() {}

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
};

/** SelStraBaseCtx 具体子类：补齐纯虚 on_bar_close，记录回调 */
class SelCtx : public SelStraBaseCtx
{
public:
    SelCtx(WtSelEngine* engine, const char* name, int32_t slippage = 0)
        : SelStraBaseCtx(engine, name, slippage) {}

    virtual ~SelCtx() {}

    virtual void on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) override
    {
        ++bar_closes;
        _last_close_code = stdCode;
        _last_close_period = period;
        _last_close_bar = newBar;
    }

    virtual void on_tick_updated(const char* stdCode, WTSTickData* newTick) override
    {
        (void)newTick;
        ++tick_updates;
        _last_upd_code = stdCode;
    }

    virtual void on_strategy_schedule(uint32_t curDate, uint32_t curTime) override
    {
        ++schedule_fires;
        _sched_date = curDate;
        _sched_time = curTime;
        // 调度过程中读取自身时间应当命中 _schedule_date/_schedule_time（由被测基类维护）
        _date_in_sched = stra_get_date();
        _time_in_sched = stra_get_time();
    }

    uint32_t bar_closes = 0;
    uint32_t tick_updates = 0;
    uint32_t schedule_fires = 0;
    std::string _last_close_code;
    std::string _last_close_period;
    WTSBarStruct* _last_close_bar = NULL;
    std::string _last_upd_code;
    uint32_t _sched_date = 0;
    uint32_t _sched_time = 0;
    uint32_t _date_in_sched = 0;
    uint32_t _time_in_sched = 0;
};

inline WTSCommodityInfo* makeComm(const char* exchg, const char* pid, const char* session,
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

inline WTSContractInfo* makeCt(const char* exchg, const char* pid, const char* code)
{
    return WTSContractInfo::create(code, code, exchg, pid);
}

inline WTSTickData* makeTick(const char* exchg, const char* code, uint32_t adate,
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

} // namespace q01sel
