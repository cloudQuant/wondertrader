/**
 * r81_env.h — lane R81（WtCore CTA/HFT 上下文的收尾补测）
 *
 * 只提供本 lane 两条测试 .cpp 所需的最小替身：
 *   R81TempRoot     系统临时目录 r81_wtcore_<pid>/，进程退出整棵删除
 *   R81GenEnv       把 WtHelper 生成目录钉到该临时目录
 *   R81BaseDataMgr  IBaseDataMgr 替身（时段/品种/合约）
 *   R81HotMgr       IHotMgr 替身，仅对 HOT 后缀返回自定义规则 tag
 *   R81CtaEngine    WtCtaEngine 派生：只接基础数据/主力，不 init/run/起线程
 *   R81HftEngine    WtHftEngine 派生：同上
 *   R81FakeTraderApi 进程内 ITraderApi 替身：init/connect 全空操作，不起线程
 *   R81HftCtx       HftStraBaseCtx 派生：暴露 protected 的 get_inner_code
 *
 * 不触碰任何生产代码；不依赖其他 lane 的测试文件。
 */
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include <Includes/IBaseDataMgr.h>
#include <Includes/IHotMgr.h>
#include <Includes/ITraderApi.h>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSMarcos.h>
#include <Includes/WTSRiskDef.hpp>
#include <Includes/WTSSessionInfo.hpp>
#include <Includes/WTSTradeDef.hpp>
#include <Includes/WTSVariant.hpp>

#include <WtCore/CtaStraBaseCtx.h>
#include <WtCore/HftStraBaseCtx.h>
#include <WtCore/WtCtaEngine.h>
#include <WtCore/WtHelper.h>
#include <WtCore/WtHftEngine.h>

namespace r81 {

namespace fs = std::filesystem;

inline std::string& temp_root_ref()
{
    static std::string root;
    return root;
}

inline void cleanup_temp_root()
{
    std::error_code ec;
    fs::remove_all(fs::path(temp_root_ref()), ec);
}

inline const std::string& temp_root()
{
    static const std::string root = []() {
        fs::path p = fs::temp_directory_path()
            / ("r81_wtcore_" + std::to_string(static_cast<long long>(::getpid())));
        std::error_code ec;
        fs::remove_all(p, ec);
        fs::create_directories(p, ec);
        temp_root_ref() = p.string();
        std::atexit(&cleanup_temp_root);
        return p.string() + "/";
    }();
    return root;
}

struct R81GenEnv
{
    R81GenEnv() { WtHelper::setGenerateDir(temp_root().c_str()); }
};

class R81BaseDataMgr : public wtp::IBaseDataMgr
{
public:
    ~R81BaseDataMgr()
    {
        for (auto* p : _sess_list)
            p->release();
        for (auto* p : _comm_infos)
            p->release();
        for (auto* p : _contracts)
            p->release();
    }

    wtp::WTSSessionInfo* session(const char* sid)
    {
        auto it = _sessions.find(sid);
        return it == _sessions.end() ? NULL : it->second;
    }

    void regisSession(const char* sid)
    {
        wtp::WTSSessionInfo* s = wtp::WTSSessionInfo::create(sid, sid, 0);
        _sessions[sid] = s;
        _sess_list.push_back(s);
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

    virtual wtp::WTSContractInfo* getContract(const char* code, const char* exchg = "",
        uint32_t uDate = 0) override
    {
        (void)uDate;
        auto it = _key_to_ct.find(std::string(code) + "|" + exchg);
        return it == _key_to_ct.end() ? NULL : it->second;
    }

    virtual wtp::WTSArray* getContracts(const char* exchg = "", uint32_t uDate = 0) override
    {
        (void)exchg;
        (void)uDate;
        return NULL;
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
        (void)pid;
        (void)uDate;
        (void)isTpl;
        return false;
    }

    virtual uint32_t calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime,
        bool isSession = false) override
    {
        (void)stdPID;
        (void)uTime;
        (void)isSession;
        return uDate;
    }

    virtual uint64_t getBoundaryTime(const char* stdPID, uint32_t tDate, bool isSession = false,
        bool isStart = true) override
    {
        (void)stdPID;
        (void)isSession;
        (void)isStart;
        return tDate;
    }

private:
    wtp::wt_hashmap<std::string, wtp::WTSContractInfo*> _key_to_ct;
    wtp::wt_hashmap<std::string, wtp::WTSCommodityInfo*> _key_to_comm;
    wtp::wt_hashmap<std::string, wtp::WTSSessionInfo*> _sessions;
    std::vector<wtp::WTSCommodityInfo*> _comm_infos;
    std::vector<wtp::WTSContractInfo*> _contracts;
    std::vector<wtp::WTSSessionInfo*> _sess_list;
    wtp::WTSSessionInfo* _first = NULL;
};

class R81HotMgr : public wtp::IHotMgr
{
public:
    std::string raw_ = "ag2512";

    virtual const char* getRawCode(const char*, const char*, uint32_t) override { return ""; }
    virtual const char* getPrevRawCode(const char*, const char*, uint32_t) override { return ""; }
    virtual bool isHot(const char*, const char*, uint32_t) override { return false; }
    virtual bool splitHotSecions(const char*, const char*, uint32_t, uint32_t,
        HotSections&) override
    {
        return false;
    }
    virtual const char* getSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
    virtual const char* getPrevSecondRawCode(const char*, const char*, uint32_t) override
    {
        return "";
    }
    virtual bool isSecond(const char*, const char*, uint32_t) override { return false; }
    virtual bool splitSecondSecions(const char*, const char*, uint32_t, uint32_t,
        HotSections&) override
    {
        return false;
    }
    virtual const char* getCustomRawCode(const char* tag, const char*, uint32_t = 0) override
    {
        return (tag != NULL && strlen(tag) > 0) ? raw_.c_str() : "";
    }
    virtual const char* getPrevCustomRawCode(const char*, const char*, uint32_t = 0) override
    {
        return "";
    }
    virtual bool isCustomHot(const char*, const char*, uint32_t = 0) override { return false; }
    virtual bool splitCustomSections(const char*, const char*, uint32_t, uint32_t,
        HotSections&) override
    {
        return false;
    }
    virtual const char* getRuleTag(const char* code) override
    {
        return (code != NULL && strcmp(code, "HOT") == 0) ? "HOT" : "";
    }
    virtual double getRuleFactor(const char*, const char*, uint32_t = 0) override { return 1.0; }
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
    wtp::WTSContractInfo* ct = wtp::WTSContractInfo::create(code, code, exchg, pid);
    ct->setAltCode(code);
    ct->setVolumeLimits(0, 0);
    return ct;
}

inline wtp::WTSTickData* makeTick(const char* exchg, const char* code, uint32_t adate,
    uint32_t tdate, uint32_t atime, double price, double bid = 0.0, double ask = 0.0)
{
    wtp::WTSTickStruct st;
    memset(&st, 0, sizeof(st));
    wt_strcpy(st.exchg, exchg);
    wt_strcpy(st.code, code);
    st.action_date = adate;
    st.trading_date = tdate;
    st.action_time = atime;
    st.price = price;
    st.volume = 1;
    st.total_volume = 100;
    st.bid_prices[0] = bid;
    st.ask_prices[0] = ask;
    st.bid_qty[0] = 10;
    st.ask_qty[0] = 10;
    return wtp::WTSTickData::create(st);
}

class R81CtaEngine : public wtp::WtCtaEngine
{
public:
    R81CtaEngine() { _port_fund = wtp::WTSPortFundInfo::create(); }

    ~R81CtaEngine() override
    {
        if (_port_fund != NULL)
        {
            _port_fund->release();
            _port_fund = NULL;
        }
    }

    void attach(wtp::IBaseDataMgr* bd, wtp::IHotMgr* hot, uint32_t date = 20250110,
        uint32_t mins = 931)
    {
        _base_data_mgr = bd;
        _hot_mgr = hot;
        set_date_time(date, mins, 0);
        set_trading_date(date);
    }

    void prime_price(const char* code, double price) { _price_map[code] = price; }
};

class R81HftEngine : public wtp::WtHftEngine
{
public:
    R81HftEngine() { _port_fund = wtp::WTSPortFundInfo::create(); }

    ~R81HftEngine() override
    {
        if (_port_fund != NULL)
        {
            _port_fund->release();
            _port_fund = NULL;
        }
    }

    void attach(wtp::IBaseDataMgr* bd, wtp::IHotMgr* hot, uint32_t date = 20250110,
        uint32_t mins = 931)
    {
        _base_data_mgr = bd;
        _hot_mgr = hot;
        set_date_time(date, mins, 0);
        set_trading_date(date);
    }
};

class R81FakeTraderApi : public wtp::ITraderApi
{
public:
    uint32_t n_init = 0;
    uint32_t n_insert = 0;

    ~R81FakeTraderApi() override {}

    virtual bool init(wtp::WTSVariant*) override
    {
        ++n_init;
        return true;
    }

    virtual bool makeEntrustID(char* buffer, int length) override
    {
        snprintf(buffer, (size_t)length, "R81E%03u", ++_seq);
        return true;
    }

    virtual int orderInsert(wtp::WTSEntrust*) override
    {
        ++n_insert;
        return 0;
    }

    virtual int orderAction(wtp::WTSEntrustAction*) override { return 0; }
    virtual int queryAccount() override { return 0; }
    virtual int queryPositions() override { return 0; }
    virtual int queryOrders() override { return 0; }
    virtual int queryTrades() override { return 0; }

private:
    uint32_t _seq = 0;
};

class R81HftCtx : public wtp::HftStraBaseCtx
{
public:
    R81HftCtx(wtp::WtHftEngine* engine, const char* name, bool agent, int32_t slippage)
        : HftStraBaseCtx(engine, name, agent, slippage)
    {
    }

    ~R81HftCtx() override {}

    using HftStraBaseCtx::get_inner_code;
};

inline std::string stra_data_file(const char* straName)
{
    std::string dir = WtHelper::getStraDataDir();
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    return dir + straName + ".json";
}

inline std::string user_data_file(const char* straName)
{
    std::string dir = WtHelper::getStraUsrDatDir();
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    return dir + "ud_" + straName + ".json";
}

inline bool write_file(const std::string& path, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc);
    if (!out.is_open())
        return false;
    out << content;
    out.close();
    return true;
}

inline std::string read_file(const std::string& path)
{
    std::ifstream in(path.c_str());
    if (!in.is_open())
        return std::string();
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace r81
