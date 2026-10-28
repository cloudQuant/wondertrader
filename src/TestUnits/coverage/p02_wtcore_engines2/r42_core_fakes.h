/**
 * r42_core_fakes.h —— lane R42 自用替身（p02_wtcore_engines2 目录内共享）
 *
 * 提供：
 *   - R42GenDir       ：把 WtHelper 的生成目录重定向到系统临时目录下的 r42_core_<pid>/
 *   - R42BaseDataMgr  ：IBaseDataMgr 替身（品种/合约/时段注册表，getContracts 返回契约数组）
 *   - R42HotMgr       ：IHotMgr 替身（不做任何主力映射）
 *   - R42HftEngine    ：WtHftEngine 派生，仅接上基础数据/主力与时钟
 *   - R42SelEngine    ：WtSelEngine 派生，同上
 *
 * 只用于测试；不触碰任何生产代码。
 */
#pragma once

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <unistd.h>

#include <Includes/IBaseDataMgr.h>
#include <Includes/IHotMgr.h>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSSessionInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSMarcos.h>
#include <Includes/WTSRiskDef.hpp>

#include <WtCore/WtHelper.h>
#include <WtCore/WtHftEngine.h>
#include <WtCore/WtSelEngine.h>

namespace r42fakes {

inline std::string gen_dir()
{
    static std::string dir = []() {
        std::filesystem::path p = std::filesystem::temp_directory_path()
            / ("r42_core_" + std::to_string(static_cast<long long>(::getpid())));
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        std::filesystem::create_directories(p, ec);
        return p.string() + "/";
    }();
    return dir;
}

struct R42GenDir
{
    R42GenDir() { WtHelper::setGenerateDir(gen_dir().c_str()); }
};

class R42BaseDataMgr : public wtp::IBaseDataMgr
{
public:
    virtual ~R42BaseDataMgr() {}

    wtp::WTSSessionInfo* session(const char* sid)
    {
        auto it = _sessions.find(sid);
        return it == _sessions.end() ? NULL : it->second;
    }

    void regisSession(const char* sid)
    {
        wtp::WTSSessionInfo* s = wtp::WTSSessionInfo::create(sid, sid, 0);
        _sessions[sid] = s;
        if (_first == NULL)
            _first = s;
    }

    void regis(wtp::WTSCommodityInfo* comm, wtp::WTSContractInfo* ct)
    {
        ct->setCommInfo(comm);
        _comm_infos.push_back(comm);
        _contracts.push_back(ct);
        _key_to_ct[std::string(ct->getCode()) + "|" + ct->getExchg()] = ct;
        _key_to_comm[std::string(comm->getProduct()) + "|" + comm->getExchg()] = comm;
    }

    virtual wtp::WTSCommodityInfo* getCommodity(const char* exchgpid) override
    {
        for (auto* c : _comm_infos)
        {
            if (strcmp(c->getFullPid(), exchgpid) == 0)
                return c;
        }
        return NULL;
    }

    virtual wtp::WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
    {
        auto it = _key_to_comm.find(std::string(pid) + "|" + exchg);
        return it == _key_to_comm.end() ? NULL : it->second;
    }

    virtual wtp::WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override
    {
        (void)uDate;
        auto it = _key_to_ct.find(std::string(code) + "|" + exchg);
        return it == _key_to_ct.end() ? NULL : it->second;
    }

    virtual wtp::WTSArray* getContracts(const char* exchg = "", uint32_t uDate = 0) override
    {
        (void)exchg;
        (void)uDate;
        wtp::WTSArray* ay = wtp::WTSArray::create();
        for (auto* ct : _contracts)
            ay->append(ct);
        return ay;
    }

    virtual wtp::WTSSessionInfo* getSession(const char* sid) override
    {
        wtp::WTSSessionInfo* ret = session(sid);
        return ret == NULL ? _first : ret;
    }

    virtual wtp::WTSSessionInfo* getSessionByCode(const char* code, const char* exchg = "") override
    {
        wtp::WTSContractInfo* ct = getContract(code, exchg);
        if (ct == NULL || ct->getCommInfo() == NULL)
            return NULL;
        return getSession(ct->getCommInfo()->getSession());
    }

    virtual wtp::WTSArray* getAllSessions() override { return NULL; }

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
    wtp::wt_hashmap<std::string, wtp::WTSContractInfo*>   _key_to_ct;
    wtp::wt_hashmap<std::string, wtp::WTSCommodityInfo*>  _key_to_comm;
    wtp::wt_hashmap<std::string, wtp::WTSSessionInfo*>    _sessions;
    std::vector<wtp::WTSCommodityInfo*>                   _comm_infos;
    std::vector<wtp::WTSContractInfo*>                    _contracts;
    wtp::WTSSessionInfo*                                  _first = NULL;
};

class R42HotMgr : public wtp::IHotMgr
{
public:
    virtual ~R42HotMgr() {}

    virtual const char* getRawCode(const char*, const char*, uint32_t) override { return ""; }
    virtual const char* getPrevRawCode(const char*, const char*, uint32_t) override { return ""; }
    virtual bool isHot(const char*, const char*, uint32_t) override { return false; }
    virtual bool splitHotSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
    virtual const char* getSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
    virtual const char* getPrevSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
    virtual bool isSecond(const char*, const char*, uint32_t) override { return false; }
    virtual bool splitSecondSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
    virtual const char* getCustomRawCode(const char*, const char*, uint32_t = 0) override { return ""; }
    virtual const char* getPrevCustomRawCode(const char*, const char*, uint32_t = 0) override { return ""; }
    virtual bool isCustomHot(const char*, const char*, uint32_t = 0) override { return false; }
    virtual bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
    virtual const char* getRuleTag(const char*) override { return ""; }
    virtual double getRuleFactor(const char*, const char*, uint32_t = 0) override { return 0; }
};

class R42HftEngine : public wtp::WtHftEngine
{
public:
    R42HftEngine()
    {
        // 裸引擎未走 WtEngine::init，但引擎级 do_set_position 需要 _port_fund
        // （WtEngine.cpp:475 创建、985/1078 使用）。
        _port_fund = wtp::WTSPortFundInfo::create();
    }

    virtual ~R42HftEngine()
    {
        if (_port_fund != NULL)
        {
            _port_fund->release();
            _port_fund = NULL;
        }
    }

    void attach(wtp::IBaseDataMgr* bd, wtp::IHotMgr* hot, uint32_t date = 20250110, uint32_t mins = 931)
    {
        _base_data_mgr = bd;
        _hot_mgr = hot;
        set_date_time(date, mins, 0);
        set_trading_date(date);
    }

    // 预置引擎实时价格缓存。策略上下文的 do_set_position 会把变化量回提交给引擎，
    // 引擎 append_signal -> get_cur_price 在缓存未命中时会落到 _data_mgr；
    // 本替身没有数据管理器，所以测试在喂 tick 前必须先调用本方法。
    void prime_price(const char* code, double price) { _price_map[code] = price; }
};

class R42SelEngine : public wtp::WtSelEngine
{
public:
    R42SelEngine()
    {
        // 同 R42HftEngine：引擎级 do_set_position 需要 _port_fund。
        _port_fund = wtp::WTSPortFundInfo::create();
    }
    virtual ~R42SelEngine()
    {
        if (_port_fund != NULL)
        {
            _port_fund->release();
            _port_fund = NULL;
        }
    }

    void attach(wtp::IBaseDataMgr* bd, wtp::IHotMgr* hot, uint32_t date = 20250110, uint32_t mins = 931)
    {
        _base_data_mgr = bd;
        _hot_mgr = hot;
        set_date_time(date, mins, 0);
        set_trading_date(date);
    }

    // 同 R42HftEngine：喂 tick 前先填引擎实时价格，避免 get_cur_price 落到空的 _data_mgr。
    void prime_price(const char* code, double price) { _price_map[code] = price; }
};

inline wtp::WTSCommodityInfo* makeComm(const char* exchg, const char* pid, const char* session,
    wtp::TradingMode tm, wtp::ContractCategory cat = wtp::CC_Future)
{
    wtp::WTSCommodityInfo* comm = wtp::WTSCommodityInfo::create(pid, pid, exchg, session, "CHINA");
    comm->setCategory(cat);
    comm->setPriceTick(1.0);
    comm->setLotsTick(1.0);
    comm->setMinLots(1.0);
    comm->setVolScale(10);
    comm->setTradingMode(tm);
    return comm;
}

inline wtp::WTSContractInfo* makeCt(const char* exchg, const char* pid, const char* code)
{
    return wtp::WTSContractInfo::create(code, code, exchg, pid);
}

inline wtp::WTSTickData* makeTick(const char* exchg, const char* code, uint32_t adate,
    uint32_t tdate, uint32_t atime, double price)
{
    wtp::WTSTickStruct st;
    memset(&st, 0, sizeof(st));
    wt_strcpy(st.exchg, exchg);
    wt_strcpy(st.code, code);
    st.action_date = adate;
    st.trading_date = tdate;
    st.action_time = atime;
    st.price = price;
    st.total_volume = 100;
    return wtp::WTSTickData::create(st);
}

} // namespace r42fakes
