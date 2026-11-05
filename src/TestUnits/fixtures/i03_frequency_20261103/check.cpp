#include <Share/FrequencyWindow.hpp>
#include <atomic>
#include <cassert>
#include <climits>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

enum { CC_FutOption, CC_SpotOption, CC_Other, WDT_LONG, WDT_SHORT, WOT_OPEN };
struct WTSCommodityInfo { int getCategoty() const { return CC_Other; } };
struct WTSContractInfo {
    WTSCommodityInfo commodity;
    WTSCommodityInfo* getCommInfo() { return &commodity; }
    const char* getCode() const { return "au2212"; }
    const char* getAltCode() const { return "au2212"; }
    const char* getExchg() const { return "SHFE"; }
    const char* getProduct() const { return "au"; }
};
struct WTSEntrust {
    WTSContractInfo* contract;
    char id[64] = {}, tag[64] = {};
    std::string code = "SHFE.au.2212";
    char* getEntrustID() { return id; }
    char* getUserTag() { return tag; }
    const char* getCode() { return code.c_str(); }
    void setCode(const char* s) { code = s; }
    void setExchange(const char*) {}
    WTSContractInfo* getContractInfo() { return contract; }
};
struct WTSOrderInfo {
    WTSContractInfo* contract;
    bool isAlive() const { return true; }
    WTSContractInfo* getContractInfo() { return contract; }
    const char* getCode() const { return "au2212"; }
    const char* getEntrustID() const { return "E"; }
    const char* getOrderID() const { return "O"; }
    int getDirection() const { return WDT_LONG; }
    int getOffsetType() const { return WOT_OPEN; }
    double getVolLeft() const { return 1; }
    void release() {}
};
struct WTSEntrustAction {
    static WTSEntrustAction* create(const char*, const char*) { return new WTSEntrustAction; }
    void setEntrustID(const char*) {}
    void setOrderID(const char*) {}
    void release() { delete this; }
};
struct Api {
    std::atomic<int> inserts{0}, cancels{0};
    bool fail = false;
    std::function<void()> during_insert;
    void makeEntrustID(char*, int) {}
    int orderInsert(WTSEntrust*) { ++inserts; if (during_insert) during_insert(); return fail ? -1 : 0; }
    int orderAction(WTSEntrustAction*) { ++cancels; return fail ? -1 : 0; }
};
struct OrderMap : std::map<uint32_t, WTSOrderInfo*> {
    WTSOrderInfo* grab(uint32_t id) { auto it = find(id); return it == end() ? nullptr : it->second; }
};
struct SpinLock { explicit SpinLock(int&) {} };
struct decimal { static bool eq(double a, double b) { return a == b; } static bool ge(double a, double b) { return a >= b; } };
struct CodeHelper {
    struct CodeInfo { char _code[16] = {}; };
    static CodeInfo extractStdCode(const char*, void*) { CodeInfo c; std::strcpy(c._code,"au2212"); return c; }
    static bool isMonthlyCode(const char*) { return true; }
    static std::string rawFutOptCodeToStdCode(const char*, const char*) { return "SHFE.au.2212"; }
    static std::string rawMonthCodeToStdCode(const char*, const char*) { return "SHFE.au.2212"; }
    static std::string rawFlatCodeToStdCode(const char*, const char*, const char*) { return "SHFE.au.2212"; }
};
namespace fmtutil { template<class T> void format_to(char* p, const char*, T) { std::strcpy(p,"1"); } }
template<class... T> void wt_strcpy(char* dest, const char* src, T...) { std::strcpy(dest, src); }
struct WTSLogger { template<class... T> static void log_dyn(T...) {} };
enum { LL_ERROR };
using OrderIDs = std::vector<uint32_t>;
struct WTSTradeStateInfo {
    uint32_t orders=0, cancels=0;
    uint32_t total_orders() const { return orders; }
    uint32_t total_cancels() const { return cancels; }
};
struct TradeStats { WTSTradeStateInfo item; WTSTradeStateInfo* get(const char*) { return &item; } };
struct TraderAdapter {
    using CodeTimeCacheMap = std::map<std::string, std::vector<uint64_t>>;
    struct RiskParams { uint32_t _order_times_boundary=2, _order_stat_timespan=60, _cancel_times_boundary=2, _cancel_stat_timespan=60, _order_total_limits=0, _cancel_total_limits=0; } risk;
    FrequencyWindow<CodeTimeCacheMap> _order_frequency, _cancel_frequency;
    bool _risk_mon_enabled = true;
    bool isUnresolvedReportPaused() const { return false; }
    mutable std::mutex _risk_exclude_mutex;
    std::unordered_set<std::string> _exclude_codes;
    TradeStats* _stat_map = nullptr;
    std::string _id="mock", _order_pattern="otp.mock";
    OrderMap* _orders;
    Api* _trader_api;
    WTSContractInfo* contract;
    int _mtx_orders=0;
    std::atomic<uint32_t> next{0};
    const RiskParams* getRiskParams(const char*) { return &risk; }
    bool checkOrderLimits(const char*);
    bool checkCancelLimits(const char*);
    bool isTradeEnabled(const char*) const;
    WTSContractInfo* getContract(const char*) { return contract; }
    uint32_t makeLocalOrderID() { return ++next; }
    uint32_t doEntrust(WTSEntrust*);
    bool doCancel(WTSOrderInfo*);
    bool cancel(uint32_t);
    OrderIDs cancel(const char*, bool, double);
};
#include "extracted.inc"

using Window = FrequencyWindow<std::map<std::string,std::vector<uint64_t>>>;
void test_window() {
    Window w;
    { Window::Reservation r(w,"a",2,1,1000); assert(r); r.commit(); }
    { Window::Reservation r(w,"a",2,1,1100); assert(r); r.commit(); }
    { Window::Reservation r(w,"a",2,1,1999); assert(!r); }
    assert(!w.available("a",2,1,1999));
    assert(w.available("a",2,1,2000));
    assert(w.available("a",2,1,9000));
    assert(w.cache.empty());
    { Window::Reservation r(w,"a",1,1,10000); assert(r); r.beginSubmission(10000); r.rollback(); }
    assert(w.available("a",1,1,10000));
    { Window::Reservation r(w,"a",0,1,10000); assert(r); r.commit(); }
    { Window::Reservation r(w,"a",1,0,10000); assert(r); r.commit(); }
    assert(w.cache.empty());
    { Window::Reservation r(w,"a",1,1,10000); assert(r); assert(!w.available("a",1,1,20000)); }
    assert(w.available("a",1,1,20000));
    { Window::Reservation r(w,"one",1,1,30000); assert(r); r.commit(); }
    { Window::Reservation r(w,"one",1,1,30999); assert(!r); }
    { Window::Reservation r(w,"one",1,1,31000); assert(r); r.commit(); }
    { Window::Reservation r(w,"other",1,1,31000); assert(r); r.commit(); }
    assert(w.available("unused",1,1,32000));
    assert(w.cache.empty());
}
void test_requests() {
    WTSContractInfo c; WTSOrderInfo a{&c}, b{&c}, d{&c}; OrderMap orders; orders.emplace(1,&a); orders.emplace(2,&b); orders.emplace(3,&d); Api api;
    TraderAdapter t; t._orders=&orders; t._trader_api=&api; t.contract=&c;
    TraderAdapter failed; failed._orders=&orders; failed._trader_api=&api; failed.contract=&c; failed.risk._order_times_boundary=1; failed.risk._cancel_times_boundary=1;
    api.fail=true; WTSEntrust first{&c}; assert(failed.doEntrust(&first)==UINT_MAX); assert(!failed.cancel(1));
    api.fail=false; WTSEntrust afterFail{&c}; assert(failed.doEntrust(&afterFail)==UINT_MAX); assert(!failed.cancel(1));
    WTSEntrust second{&c}, third{&c}, fourth{&c};
    assert(t.doEntrust(&second)!=UINT_MAX); assert(t.doEntrust(&third)!=UINT_MAX); assert(t.doEntrust(&fourth)==UINT_MAX); assert(api.inserts==3);
    assert(t.cancel("",true,0).size()==2); assert(!t.cancel(3)); assert(api.cancels==3);
    TraderAdapter re; re._orders=&orders; re._trader_api=&api; re.contract=&c; re.risk._order_times_boundary=1;
    bool nestedDenied=false; api.during_insert=[&] { WTSEntrust nested{&c}; nestedDenied=re.doEntrust(&nested)==UINT_MAX; };
    WTSEntrust outer{&c}; assert(re.doEntrust(&outer)!=UINT_MAX); assert(nestedDenied); api.during_insert={};
    TraderAdapter uncertain; uncertain._orders=&orders; uncertain._trader_api=&api; uncertain.contract=&c; uncertain.risk._order_times_boundary=1;
    api.during_insert=[] { throw std::runtime_error("unknown provider outcome"); };
    try { WTSEntrust x{&c}; uncertain.doEntrust(&x); assert(false); } catch (const std::runtime_error&) {}
    api.during_insert={}; WTSEntrust after{&c}; assert(uncertain.doEntrust(&after)==UINT_MAX);
    assert(t.isTradeEnabled("SHFE.au.2212"));
    TradeStats stats; TraderAdapter total; total._stat_map=&stats; total.risk._order_total_limits=1; stats.item.orders=1;
    assert(!total.checkOrderLimits("SHFE.au.2212")); assert(!total.isTradeEnabled("SHFE.au.2212"));
    TraderAdapter single; single._orders=&orders; single._trader_api=&api; single.contract=&c; single.risk._cancel_times_boundary=1;
    assert(single.cancel(1)); assert(!single.cancel(2));
    TraderAdapter disabled; disabled._orders=&orders; disabled._trader_api=&api; disabled.contract=&c; disabled.risk._cancel_times_boundary=0;
    assert(disabled.cancel(1)); assert(disabled.cancel(2)); assert(disabled.cancel(3));
    TraderAdapter concurrent; concurrent._orders=&orders; concurrent._trader_api=&api; concurrent.contract=&c; concurrent.risk._order_times_boundary=2;
    std::atomic<int> success{0}; std::vector<std::thread> threads;
    for(int i=0;i<20;i++) threads.emplace_back([&] { WTSEntrust x{&c}; if(concurrent.doEntrust(&x)!=UINT_MAX) ++success; });
    for(auto& thread:threads) thread.join(); assert(success==2);
}
int main() { test_window(); test_requests(); std::cout << "frequency fixture passed\n"; }
