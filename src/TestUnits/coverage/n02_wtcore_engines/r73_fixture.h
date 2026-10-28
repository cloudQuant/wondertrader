/**
 * r73_fixture.h — lane R73 自用替身（第十五波补测）
 *
 * 只使用公开头文件里的接口/工厂与 protected 成员装配测试替身，不修改生产代码。
 * 提供：
 *   - TempDir：系统临时目录下的唯一子目录，析构整棵删除
 *   - R73BaseDataMgr / R73HotMgr：基础数据/主力规则替身
 *   - R73DataMgr：WtDtMgr 派生，返回确定性的 K 线/Tick/L2 切片（可切换 NULL）
 *   - R73Stub：IExecuterStub 替身
 *   - R73CtaEngine / R73HftEngine：只接 bd/hot/data_mgr 与固定时钟的引擎替身
 *   - R73CtaCtx / R73HftCtx：策略上下文具体子类（记录回调/暴露 protected 入口）
 *   - R73PluginDir：只含一个插件符号链接的临时目录
 *
 * 所有替身都在用例内构造，晚于被测算例对象析构；不启动线程、不联网。
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
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSMarcos.h>
#include <Includes/WTSRiskDef.hpp>
#include <Includes/WTSSessionInfo.hpp>
#include <Includes/WTSVariant.hpp>
#include <Includes/WTSTradeDef.hpp>

#include <Share/DLLHelper.hpp>

#include <WtCore/CtaStraBaseCtx.h>
#include <WtCore/HftStraBaseCtx.h>
#include <WtCore/IExecCommand.h>
#include <WtCore/WtCtaEngine.h>
#include <WtCore/WtDtMgr.h>
#include <WtCore/WtHelper.h>
#include <WtCore/WtHftEngine.h>

namespace r73 {

namespace fs = std::filesystem;

class TempDir
{
public:
    explicit TempDir(const std::string& tag)
    {
        static uint32_t seq = 0;
        fs::path p = fs::temp_directory_path()
            / ("wt_r73_" + tag + "_" + std::to_string(static_cast<long long>(::getpid()))
                + "_" + std::to_string(++seq));
        std::error_code ec;
        fs::remove_all(p, ec);
        fs::create_directories(p, ec);
        _path = p;
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(_path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string path(const std::string& rel = "") const
    {
        return rel.empty() ? _path.string() : (_path / rel).string();
    }

    void write(const std::string& rel, const std::string& content) const
    {
        fs::path p = _path / rel;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream ofs(p.string().c_str(), std::ios::binary | std::ios::trunc);
        ofs << content;
        ofs.flush();
    }

private:
    fs::path _path;
};

inline bool write_file(const std::string& path, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream ofs(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!ofs.is_open())
        return false;
    ofs << content;
    ofs.close();
    return true;
}

inline std::string read_file(const std::string& path)
{
    std::ifstream ifs(path.c_str(), std::ios::binary);
    if (!ifs.is_open())
        return std::string();
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

//////////////////////////////////////////////////////////////////
// 基础数据替身：按 rawCode|exchg 建索引
//////////////////////////////////////////////////////////////////
class BaseDataMgr : public wtp::IBaseDataMgr
{
public:
    ~BaseDataMgr()
    {
        for (auto* p : _sess_list)
            p->release();
        for (auto* p : _comms)
            p->release();
        for (auto* p : _cts)
            p->release();
    }

    wtp::WTSSessionInfo* session(const char* sid = "TRADING")
    {
        auto it = _sessions.find(sid);
        return it == _sessions.end() ? NULL : it->second;
    }

    wtp::WTSSessionInfo* regisSession(const char* sid, uint32_t otime = 900, uint32_t ctime = 1500)
    {
        wtp::WTSSessionInfo* s = wtp::WTSSessionInfo::create(sid, sid, 0);
        s->addTradingSection(otime, ctime);
        _sessions[sid] = s;
        _sess_list.push_back(s);
        if (_first == NULL)
            _first = s;
        return s;
    }

    void regis(wtp::WTSCommodityInfo* comm, wtp::WTSContractInfo* ct)
    {
        ct->setCommInfo(comm);
        _comms.push_back(comm);
        _cts.push_back(ct);
        _key_to_ct[std::string(ct->getCode()) + "|" + ct->getExchg()] = ct;
        _key_to_comm[std::string(comm->getProduct()) + "|" + comm->getExchg()] = comm;
    }

    wtp::WTSCommodityInfo* getCommodity(const char* exchgpid) override
    {
        for (auto* c : _comms)
        {
            if (strcmp(c->getFullPid(), exchgpid) == 0)
                return c;
        }
        return NULL;
    }

    wtp::WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
    {
        auto it = _key_to_comm.find(std::string(pid) + "|" + exchg);
        return it == _key_to_comm.end() ? NULL : it->second;
    }

    wtp::WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t = 0) override
    {
        auto it = _key_to_ct.find(std::string(code) + "|" + exchg);
        return it == _key_to_ct.end() ? NULL : it->second;
    }

    wtp::WTSArray* getContracts(const char* = "", uint32_t = 0) override { return NULL; }

    wtp::WTSSessionInfo* getSession(const char* sid) override
    {
        wtp::WTSSessionInfo* ret = session(sid);
        return ret == NULL ? _first : ret;
    }

    wtp::WTSSessionInfo* getSessionByCode(const char* code, const char* exchg = "") override
    {
        wtp::WTSContractInfo* ct = getContract(code, exchg);
        if (ct == NULL || ct->getCommInfo() == NULL)
            return NULL;
        return getSession(ct->getCommInfo()->getSession());
    }

    wtp::WTSArray* getAllSessions() override { return NULL; }
    bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
    uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate; }
    uint64_t getBoundaryTime(const char*, uint32_t tDate, bool = false, bool = true) override { return tDate; }

private:
    wtp::wt_hashmap<std::string, wtp::WTSContractInfo*> _key_to_ct;
    wtp::wt_hashmap<std::string, wtp::WTSCommodityInfo*> _key_to_comm;
    wtp::wt_hashmap<std::string, wtp::WTSSessionInfo*> _sessions;
    std::vector<wtp::WTSCommodityInfo*> _comms;
    std::vector<wtp::WTSContractInfo*> _cts;
    std::vector<wtp::WTSSessionInfo*> _sess_list;
    wtp::WTSSessionInfo* _first = NULL;
};

//////////////////////////////////////////////////////////////////
// 主力规则替身：prev_code 可配置，用于 on_position 自动清理链
//////////////////////////////////////////////////////////////////
class HotMgr : public wtp::IHotMgr
{
public:
    std::string prev_code;
    std::string prev_custom = "rb2510";
    std::string rule_tag;
    std::string raw_code = "rb2510";

    const char* getRawCode(const char*, const char*, uint32_t) override { return raw_code.c_str(); }
    const char* getPrevRawCode(const char*, const char*, uint32_t) override { return prev_code.c_str(); }
    bool isHot(const char*, const char*, uint32_t) override { return false; }
    bool splitHotSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
    const char* getSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
    const char* getPrevSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
    bool isSecond(const char*, const char*, uint32_t) override { return false; }
    bool splitSecondSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
    const char* getCustomRawCode(const char* tag, const char*, uint32_t = 0) override
    {
        return (tag != NULL && strlen(tag) > 0) ? prev_custom.c_str() : "";
    }
    const char* getPrevCustomRawCode(const char*, const char*, uint32_t = 0) override { return ""; }
    bool isCustomHot(const char*, const char*, uint32_t = 0) override { return false; }
    bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
    const char* getRuleTag(const char* code) override
    {
        return (code != NULL && rule_tag.size() > 0 && strstr(code, rule_tag.c_str()) != NULL)
            ? rule_tag.c_str() : "";
    }
    double getRuleFactor(const char*, const char*, uint32_t = 0) override { return 1.0; }
};

//////////////////////////////////////////////////////////////////
// WtDtMgr 派生：确定性切片；null_* 打开时返回 NULL
//////////////////////////////////////////////////////////////////
class DataMgr : public wtp::WtDtMgr
{
public:
    DataMgr()
    {
        for (int i = 0; i < 3; i++)
        {
            memset(&_bars[i], 0, sizeof(_bars[i]));
            _bars[i].date = 20250110;
            _bars[i].time = 202501100931ull + (uint64_t)i;
            _bars[i].open = 3000.0 + i;
            _bars[i].high = 3010.0 + i;
            _bars[i].low = 2990.0 + i;
            _bars[i].close = 3005.0 + i;

            memset(&_ticks[i], 0, sizeof(_ticks[i]));
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

    ~DataMgr()
    {
        if (_last_tick != NULL)
        {
            _last_tick->release();
            _last_tick = NULL;
        }
    }

    wtp::WTSKlineSlice* get_kline_slice(const char* stdCode, wtp::WTSKlinePeriod period,
        uint32_t times, uint32_t, uint64_t = 0) override
    {
        ++kline_calls;
        if (null_kline)
            return NULL;
        return wtp::WTSKlineSlice::create(stdCode, period, times, _bars, 3);
    }

    wtp::WTSTickSlice* get_tick_slice(const char* stdCode, uint32_t, uint64_t = 0) override
    {
        ++tick_calls;
        if (null_ticks)
            return NULL;
        return wtp::WTSTickSlice::create(stdCode, _ticks, 3);
    }

    wtp::WTSTickData* grab_last_tick(const char*) override
    {
        ++last_tick_calls;
        if (null_last_tick || _last_tick == NULL)
            return NULL;
        _last_tick->retain();
        return _last_tick;
    }

    wtp::WTSOrdDtlSlice* get_order_detail_slice(const char* stdCode, uint32_t, uint64_t = 0) override
    {
        ++dtl_calls;
        if (null_l2)
            return NULL;
        return wtp::WTSOrdDtlSlice::create(stdCode, &_ord_dtl, 1);
    }

    wtp::WTSOrdQueSlice* get_order_queue_slice(const char* stdCode, uint32_t, uint64_t = 0) override
    {
        ++que_calls;
        if (null_l2)
            return NULL;
        return wtp::WTSOrdQueSlice::create(stdCode, &_ord_que, 1);
    }

    wtp::WTSTransSlice* get_transaction_slice(const char* stdCode, uint32_t, uint64_t = 0) override
    {
        ++trans_calls;
        if (null_l2)
            return NULL;
        return wtp::WTSTransSlice::create(stdCode, &_trans, 1);
    }

    bool null_kline = false;
    bool null_ticks = false;
    bool null_last_tick = false;
    bool null_l2 = false;
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

//////////////////////////////////////////////////////////////////
// 引擎替身：只装配数据面与固定时钟
//////////////////////////////////////////////////////////////////
class CtaEngine : public wtp::WtCtaEngine
{
public:
    CtaEngine() { _port_fund = wtp::WTSPortFundInfo::create(); }
    ~CtaEngine() override
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

class HftEngine : public wtp::WtHftEngine
{
public:
    HftEngine() { _port_fund = wtp::WTSPortFundInfo::create(); }
    ~HftEngine() override
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

    void on_bar(const char*, const char*, uint32_t, wtp::WTSBarStruct*) override
    {
        ++bar_count;
    }

    uint32_t bar_count = 0;
};

//////////////////////////////////////////////////////////////////
// 上下文具体子类
//////////////////////////////////////////////////////////////////
class CtaCtx : public wtp::CtaStraBaseCtx
{
public:
    CtaCtx(wtp::WtCtaEngine* engine, const char* name, int32_t slippage = 0)
        : CtaStraBaseCtx(engine, name, slippage) {}
    ~CtaCtx() override {}

    void on_tick_updated(const char*, wtp::WTSTickData*) override { ++tick_updates; }
    void on_bar_close(const char*, const char* period, wtp::WTSBarStruct*) override
    {
        ++bar_closes;
        last_period = period == NULL ? "" : period;
    }
    void on_calculate(uint32_t, uint32_t) override
    {
        ++calc_times;
        if (emit_condition)
            stra_enter_long(cond_code.c_str(), 1.0, "sched", 9999.0);
    }
    void on_condition_triggered(const char*, double target, double, const char*) override
    {
        ++cond_fires;
        last_cond_target = target;
    }

    size_t condition_count() const { return _condtions.size(); }
    uint32_t last_barno() const { return _last_barno; }

    uint32_t tick_updates = 0;
    uint32_t bar_closes = 0;
    uint32_t calc_times = 0;
    uint32_t cond_fires = 0;
    double last_cond_target = 0;
    std::string last_period;
    bool emit_condition = false;
    std::string cond_code;
};

class HftCtx : public wtp::HftStraBaseCtx
{
public:
    HftCtx(wtp::WtHftEngine* engine, const char* name, bool agent, int32_t slippage = 0)
        : HftStraBaseCtx(engine, name, agent, slippage) {}
    ~HftCtx() override {}

    using HftStraBaseCtx::do_set_position;
    using HftStraBaseCtx::update_dyn_profit;
};

//////////////////////////////////////////////////////////////////
// 执行器存根
//////////////////////////////////////////////////////////////////
class ExecStub : public wtp::IExecuterStub
{
public:
    wtp::WTSCommodityInfo* comm = NULL;
    wtp::WTSSessionInfo* sess = NULL;
    wtp::IHotMgr* hot = NULL;

    uint64_t get_real_time() override { return 20250110093100000ull; }
    wtp::WTSCommodityInfo* get_comm_info(const char*) override { return comm; }
    wtp::WTSSessionInfo* get_sess_info(const char*) override { return sess; }
    wtp::IHotMgr* get_hot_mon() override { return hot; }
    uint32_t get_trading_day() override { return 20250110; }
};

//////////////////////////////////////////////////////////////////
// 辅助工厂
//////////////////////////////////////////////////////////////////
inline wtp::WTSCommodityInfo* makeComm(const char* exchg, const char* pid,
    const char* session = "TRADING", wtp::TradingMode tm = wtp::TM_Both,
    wtp::ContractCategory cat = wtp::CC_Future)
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
    wtp::WTSContractInfo* ct = NULL)
{
    wtp::WTSTickStruct st;
    memset(&st, 0, sizeof(st));
    wt_strcpy(st.exchg, exchg);
    wt_strcpy(st.code, code);
    st.action_date = adate;
    st.trading_date = tdate;
    st.action_time = atime;
    st.price = price;
    st.open = price;
    st.high = price;
    st.low = price;
    st.volume = 1;
    st.total_volume = 100;
    st.bid_prices[0] = bid;
    st.ask_prices[0] = ask;
    st.bid_qty[0] = 10;
    st.ask_qty[0] = 10;
    wtp::WTSTickData* tick = wtp::WTSTickData::create(st);
    if (ct != NULL)
        tick->setContractInfo(ct);
    return tick;
}

//////////////////////////////////////////////////////////////////
// 插件目录：只放一个模块符号链接，避免 dlopen 其它插件
//////////////////////////////////////////////////////////////////
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

class PluginDir
{
public:
    explicit PluginDir(const char* module, const std::string& root)
    {
        std::error_code ec;
        const std::string wrapped = DLLHelper::wrap_module(module);
        _dir = fs::path(root) / ("plugin_" + std::string(module));
        fs::remove_all(_dir, ec);
        fs::create_directories(_dir, ec);

        fs::path src = fs::path(bin_dir()) / wrapped;
        fs::path dst = _dir / wrapped;
        fs::create_symlink(src, dst, ec);
        _ok = !ec && fs::exists(dst);
    }

    ~PluginDir()
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

}  // namespace r73
