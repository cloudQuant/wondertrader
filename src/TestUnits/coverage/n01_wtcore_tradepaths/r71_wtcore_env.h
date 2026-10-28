/**
 * r71_wtcore_env.h —— lane R71 自用替身（n01_wtcore_tradepaths 目录内共享）
 *
 * 提供：
 *   R71TempRoot     系统临时目录下 r71_wtcore_<pid>/，进程退出时整棵删除
 *   R71GenEnv       把 WtHelper 生成目录钉到上述临时目录
 *   R71BinDir       从测试可执行文件位置推出 build_x64/Debug/bin
 *   R71PluginDir    只含一个 libWtExeFact.dylib 符号链接的临时目录
 *                    （WtExecuterFactory::loadFactories 会 dlopen 目录里每个 dylib）
 *   R71BaseDataMgr  IBaseDataMgr 替身（时段/品种/合约/主力）
 *   R71HotMgr       IHotMgr 替身，只为 HOT 后缀返回自定义规则 tag
 *   R71DataMgr      WtDtMgr 派生，返回确定性的 K 线/Tick/L2 切片
 *   R71CtaEngine    WtCtaEngine 派生，只接基础数据/主力/数据管理器
 *   R71HftEngine    WtHftEngine 派生，同上；on_bar 只计数
 *   R71ExecStub     IExecuterStub 替身
 *   R71FakeTraderApi ITraderApi 进程内替身（不连柜台、不起线程）
 *   R71TraderEnv    ActionPolicyMgr + TraderAdapter 装配（复用公开 API）
 *
 * 只用于测试；不触碰任何生产代码。
 */
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <boost/core/noncopyable.hpp>

#include <Includes/IBaseDataMgr.h>
#include <Includes/IHotMgr.h>
#include <Includes/ITraderApi.h>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSMarcos.h>
#include <Includes/WTSRiskDef.hpp>
#include <Includes/WTSTradeDef.hpp>
#include <Includes/WTSSessionInfo.hpp>
#include <Includes/WTSVariant.hpp>
#include <Share/DLLHelper.hpp>

#include <WtCore/ActionPolicyMgr.h>
#include <WtCore/CtaStraBaseCtx.h>
#include <WtCore/HftStraBaseCtx.h>
#include <WtCore/IExecCommand.h>
#include <WtCore/TraderAdapter.h>
#include <WtCore/WtCtaEngine.h>
#include <WtCore/WtDtMgr.h>
#include <WtCore/WtExecuterFactory.h>
#include <WtCore/WtHelper.h>
#include <WtCore/WtHftEngine.h>

namespace r71 {

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
            / ("r71_wtcore_" + std::to_string(static_cast<long long>(::getpid())));
        std::error_code ec;
        fs::remove_all(p, ec);
        fs::create_directories(p, ec);
        temp_root_ref() = p.string();
        std::atexit(&cleanup_temp_root);
        return p.string() + "/";
    }();
    return root;
}

struct R71GenEnv
{
    R71GenEnv() { WtHelper::setGenerateDir(temp_root().c_str()); }
};

inline std::string exe_dir()
{
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, 0);
    _NSGetExecutablePath(buf.data(), &size);
    std::error_code ec;
    fs::path exe = fs::weakly_canonical(fs::path(buf.data()), ec);
    if (ec)
        exe = fs::path(buf.data());
    return exe.parent_path().string();
#else
    char buf[4096] = { 0 };
    ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0)
        return std::string();
    std::error_code ec;
    fs::path exe = fs::weakly_canonical(fs::path(std::string(buf, (std::size_t)len)), ec);
    if (ec)
        exe = fs::path(std::string(buf, (std::size_t)len));
    return exe.parent_path().string();
#endif
}

inline std::string bin_dir()
{
    fs::path dir(exe_dir());
    if (dir.filename() == "TestUnits")
        return dir.parent_path().string();
    return dir.string();
}

/// 目录里只放一个插件链接，避免 loadFactories 顺带 dlopen 其它插件
class R71PluginDir
{
public:
    explicit R71PluginDir(const char* module = "WtExeFact")
    {
        std::error_code ec;
        const std::string wrapped = DLLHelper::wrap_module(module);
        _dir = fs::path(temp_root()) / ("plugin_" + std::string(module));
        fs::remove_all(_dir, ec);
        fs::create_directories(_dir, ec);

        fs::path src = fs::path(bin_dir()) / wrapped;
        fs::path dst = _dir / wrapped;
        fs::create_symlink(src, dst, ec);
        _ok = !ec && fs::exists(dst);
    }

    ~R71PluginDir()
    {
        std::error_code ec;
        fs::remove_all(_dir, ec);
    }

    bool ok() const { return _ok; }
    const std::string dir() const { return _dir.string(); }

private:
    fs::path _dir;
    bool _ok = false;
};

class R71BaseDataMgr : public wtp::IBaseDataMgr
{
public:
    ~R71BaseDataMgr()
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

    /// 真实基础库按原始代码建键；这里额外登记标准代码别名，用于驱动 diff 的过期目标分支
    void regisAlias(const char* stdCode, wtp::WTSContractInfo* ct)
    {
        _key_to_ct[std::string(stdCode) + "|" + ct->getExchg()] = ct;
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

    virtual uint32_t calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession = false) override
    {
        (void)stdPID;
        (void)uTime;
        (void)isSession;
        return uDate;
    }

    virtual uint64_t getBoundaryTime(const char* stdPID, uint32_t tDate, bool isSession = false, bool isStart = true) override
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

class R71HotMgr : public wtp::IHotMgr
{
public:
    std::string raw_ = "ag2512";
    std::string prev_ = "";
    mutable uint32_t n_prev_ = 0;

    ~R71HotMgr() {}

    virtual const char* getRawCode(const char*, const char*, uint32_t) override { return ""; }
    virtual const char* getPrevRawCode(const char*, const char*, uint32_t) override
    {
        ++n_prev_;
        return prev_.c_str();
    }
    virtual bool isHot(const char*, const char*, uint32_t) override { return false; }
    virtual bool splitHotSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override
    {
        return false;
    }
    virtual const char* getSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
    virtual const char* getPrevSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
    virtual bool isSecond(const char*, const char*, uint32_t) override { return false; }
    virtual bool splitSecondSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override
    {
        return false;
    }
    virtual const char* getCustomRawCode(const char* tag, const char*, uint32_t = 0) override
    {
        return (tag != NULL && strlen(tag) > 0) ? raw_.c_str() : "";
    }
    virtual const char* getPrevCustomRawCode(const char*, const char*, uint32_t = 0) override { return ""; }
    virtual bool isCustomHot(const char*, const char*, uint32_t = 0) override { return false; }
    virtual bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections&) override
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
    uint32_t tdate, uint32_t atime, double price, double bid = 0.0, double ask = 0.0,
    double open = 0.0, double high = 0.0, double low = 0.0)
{
    wtp::WTSTickStruct st;
    wt_strcpy(st.exchg, exchg);
    wt_strcpy(st.code, code);
    st.action_date = adate;
    st.trading_date = tdate;
    st.action_time = atime;
    st.price = price;
    st.open = open;
    st.high = high;
    st.low = low;
    st.volume = 1;
    st.total_volume = 100;
    st.bid_prices[0] = bid;
    st.ask_prices[0] = ask;
    st.bid_qty[0] = 10;
    st.ask_qty[0] = 10;
    return wtp::WTSTickData::create(st);
}

/// WtDtMgr 派生：覆盖 5 个数据入口，返回测试内预置的确定性切片
class R71DataMgr : public wtp::WtDtMgr
{
public:
    R71DataMgr()
    {
        for (int i = 0; i < 3; i++)
        {
            _bars[i].date = 20250110;
            _bars[i].time = 202501100931ull + (uint64_t)i;
            _bars[i].open = 3000.0 + i;
            _bars[i].high = 3010.0 + i;
            _bars[i].low = 2990.0 + i;
            _bars[i].close = 3005.0 + i;

            _ticks[i].action_date = 20250110;
            _ticks[i].trading_date = 20250110;
            _ticks[i].action_time = 93100000 + i;
            _ticks[i].price = 3500.0 + i;
            _ticks[i].open = 3480.0;
            _ticks[i].high = 3520.0;
            _ticks[i].low = 3470.0;
            wt_strcpy(_ticks[i].exchg, "SHFE");
            wt_strcpy(_ticks[i].code, "SHFE.ag.2512");
        }
        _last_tick = wtp::WTSTickData::create(_ticks[2]);
        _last_tick->retain();
    }

    ~R71DataMgr()
    {
        if (_last_tick != NULL)
        {
            _last_tick->release();
            _last_tick = NULL;
        }
    }

    wtp::WTSKlineSlice* get_kline_slice(const char* stdCode, wtp::WTSKlinePeriod period,
        uint32_t times, uint32_t count, uint64_t etime = 0) override
    {
        (void)count;
        (void)etime;
        ++kline_calls;
        return wtp::WTSKlineSlice::create(stdCode, period, times, _bars, 3);
    }

    wtp::WTSTickSlice* get_tick_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override
    {
        (void)count;
        (void)etime;
        ++tick_calls;
        return wtp::WTSTickSlice::create(stdCode, _ticks, 3);
    }

    wtp::WTSTickData* grab_last_tick(const char* stdCode) override
    {
        (void)stdCode;
        ++last_tick_calls;
        if (_last_tick == NULL)
            return NULL;
        _last_tick->retain();
        return _last_tick;
    }

    wtp::WTSOrdDtlSlice* get_order_detail_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override
    {
        (void)count;
        (void)etime;
        ++dtl_calls;
        return wtp::WTSOrdDtlSlice::create(stdCode, &_ord_dtl, 1);
    }

    wtp::WTSOrdQueSlice* get_order_queue_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override
    {
        (void)count;
        (void)etime;
        ++que_calls;
        return wtp::WTSOrdQueSlice::create(stdCode, &_ord_que, 1);
    }

    wtp::WTSTransSlice* get_transaction_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override
    {
        (void)count;
        (void)etime;
        ++trans_calls;
        return wtp::WTSTransSlice::create(stdCode, &_trans, 1);
    }

    uint32_t kline_calls = 0;
    uint32_t tick_calls = 0;
    uint32_t last_tick_calls = 0;
    uint32_t dtl_calls = 0;
    uint32_t que_calls = 0;
    uint32_t trans_calls = 0;

private:
    wtp::WTSBarStruct _bars[3];
    wtp::WTSTickStruct _ticks[3];
    wtp::WTSOrdDtlStruct _ord_dtl {};
    wtp::WTSOrdQueStruct _ord_que {};
    wtp::WTSTransStruct _trans {};
    wtp::WTSTickData* _last_tick = NULL;
};

class R71CtaEngine : public wtp::WtCtaEngine
{
public:
    R71CtaEngine()
    {
        _port_fund = wtp::WTSPortFundInfo::create();
    }

    ~R71CtaEngine() override
    {
        if (_port_fund != NULL)
        {
            _port_fund->release();
            _port_fund = NULL;
        }
    }

    void attach(wtp::IBaseDataMgr* bd, wtp::IHotMgr* hot, wtp::WtDtMgr* dm,
        uint32_t date = 20250110, uint32_t mins = 931)
    {
        _base_data_mgr = bd;
        _hot_mgr = hot;
        _data_mgr = dm;
        set_date_time(date, mins, 0);
        set_trading_date(date);
    }

    void prime_price(const char* code, double price) { _price_map[code] = price; }
};

class R71HftEngine : public wtp::WtHftEngine
{
public:
    R71HftEngine()
    {
        _port_fund = wtp::WTSPortFundInfo::create();
    }

    ~R71HftEngine() override
    {
        if (_port_fund != NULL)
        {
            _port_fund->release();
            _port_fund = NULL;
        }
    }

    void attach(wtp::IBaseDataMgr* bd, wtp::IHotMgr* hot, wtp::WtDtMgr* dm,
        uint32_t date = 20250110, uint32_t mins = 931)
    {
        _base_data_mgr = bd;
        _hot_mgr = hot;
        _data_mgr = dm;
        set_date_time(date, mins, 0);
        set_trading_date(date);
    }

    void prime_price(const char* code, double price) { _price_map[code] = price; }

    void on_bar(const char* code, const char* period, uint32_t times, wtp::WTSBarStruct* newBar) override
    {
        (void)period;
        (void)newBar;
        ++bar_count;
        last_bar_code = code;
        last_bar_times = times;
    }

    void run() override {}

    uint32_t bar_count = 0;
    uint32_t last_bar_times = 0;
    std::string last_bar_code;
};

class R71ExecStub : public wtp::IExecuterStub
{
public:
    wtp::IHotMgr* hot_ = NULL;
    wtp::WTSCommodityInfo* comm_ = NULL;
    wtp::WTSSessionInfo* sess_ = NULL;

    virtual uint64_t get_real_time() override { return 20250110093100000ull; }
    virtual wtp::WTSCommodityInfo* get_comm_info(const char*) override { return comm_; }
    virtual wtp::WTSSessionInfo* get_sess_info(const char*) override { return sess_; }
    virtual wtp::IHotMgr* get_hot_mon() override { return hot_; }
    virtual uint32_t get_trading_day() override { return 20250110; }
};

class R71FakeTraderApi : public wtp::ITraderApi
{
public:
    uint32_t n_init = 0;
    uint32_t n_insert = 0;
    uint32_t n_action = 0;
    std::string last_code;
    double last_price = 0;
    double last_vol = 0;
    wtp::WTSDirectionType last_dir = wtp::WDT_LONG;
    wtp::WTSOffsetType last_off = wtp::WOT_OPEN;

    ~R71FakeTraderApi() override {}

    virtual bool init(wtp::WTSVariant*) override { ++n_init; return true; }
    virtual bool makeEntrustID(char* buffer, int length) override
    {
        snprintf(buffer, (size_t)length, "R71E%03u", ++_seq);
        return true;
    }
    virtual int orderInsert(wtp::WTSEntrust* entrust) override
    {
        ++n_insert;
        last_code = entrust->getCode();
        last_price = entrust->getPrice();
        last_vol = entrust->getVolume();
        last_dir = entrust->getDirection();
        last_off = entrust->getOffsetType();
        return 0;
    }
    virtual int orderAction(wtp::WTSEntrustAction* action) override
    {
        (void)action;
        ++n_action;
        return 0;
    }
    virtual int queryAccount() override { return 0; }
    virtual int queryPositions() override { return 0; }
    virtual int queryOrders() override { return 0; }
    virtual int queryTrades() override { return 0; }

private:
    uint32_t _seq = 0;
};

/// ActionPolicyMgr + TraderAdapter 的最小装配（真实 TraderAdapter，不接柜台）
class R71TraderEnv
{
public:
    R71TraderEnv()
    {
        std::string policyFile = temp_root() + "r71_policy.json";
        {
            std::ofstream out(policyFile.c_str(), std::ios::out | std::ios::trunc);
            out << R"json({"default":{"order":[{"action":"open"}]}})json";
        }
        _policy.reset(new wtp::ActionPolicyMgr);
        _policy_ok = _policy->init(policyFile.c_str());
        WtHelper::setTime(20250110, 1000, 0);
    }

    bool policy_ok() const { return _policy_ok; }
    wtp::ActionPolicyMgr* policy() { return _policy.get(); }

    bool build(wtp::TraderAdapter& trader, const char* id)
    {
        if (!_policy_ok)
            return false;
        if (!trader.initExt(id, &api, &bd, _policy.get()))
            return false;
        if (!trader.run())
            return false;
        trader.onLoginResult(true, "r71", 20250110);
        trader.onRspPosition(static_cast<const wtp::WTSArray*>(nullptr));
        trader.onRspOrders(static_cast<const wtp::WTSArray*>(nullptr));
        trader.onRspTrades(static_cast<const wtp::WTSArray*>(nullptr));
        trader.onRspAccount(static_cast<wtp::WTSArray*>(nullptr));
        return trader.state() == wtp::TraderAdapter::AS_ALLREADY;
    }

    R71BaseDataMgr bd;
    R71HotMgr hot;
    R71FakeTraderApi api;

private:
    std::unique_ptr<wtp::ActionPolicyMgr> _policy;
    bool _policy_ok = false;
};

inline std::string stra_data_file(const char* straName)
{
    std::string dir = WtHelper::getStraDataDir();
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    return dir + straName + ".json";
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

} // namespace r71
