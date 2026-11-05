#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

// The three TraderAdapter methods are taken verbatim from the current source
// into extracted.inc by run.py. These substitutes isolate their control flow;
// they are not the real provider, order map, or synchronization implementation.
enum { CC_FutOption, CC_SpotOption, CC_Other };
enum { WDT_LONG, WDT_SHORT, WOT_OPEN, WOT_CLOSE };
struct Commodity { int getCategoty() const { return CC_Other; } };
struct Contract {
    Commodity commodity;
    Commodity* getCommInfo() { return &commodity; }
    const char* getCode() const { return "au2212"; }
    const char* getExchg() const { return "SHFE"; }
    const char* getProduct() const { return "au"; }
};
using WTSCommodityInfo = Commodity;
using WTSContractInfo = Contract;
struct WTSOrderInfo {
    Contract contract;
    bool isAlive() const { return true; }
    Contract* getContractInfo() { return &contract; }
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
    bool succeed = true;
    int calls = 0;
    int orderAction(WTSEntrustAction*) { ++calls; return succeed ? 0 : -1; }
};
struct OrderMap : std::map<uint32_t, WTSOrderInfo*> {
    WTSOrderInfo* grab(uint32_t id) { auto it = find(id); return it == end() ? nullptr : it->second; }
};
struct SpinLock { explicit SpinLock(int&) {} };
struct TimeUtils { static uint64_t getLocalTimeNow() { return 100; } };
struct decimal {
    static bool eq(double a, double b) { return a == b; }
    static bool ge(double a, double b) { return a >= b; }
};
struct CodeHelper {
    struct CodeInfo { char _code[16] = {}; };
    static CodeInfo extractStdCode(const char*, void*) { return {}; }
    static bool isMonthlyCode(const char*) { return true; }
    static std::string rawFutOptCodeToStdCode(const char*, const char*) { return "SHFE.au.2212"; }
    static std::string rawMonthCodeToStdCode(const char*, const char*) { return "SHFE.au.2212"; }
    static std::string rawFlatCodeToStdCode(const char*, const char*, const char*) { return "SHFE.au.2212"; }
};
using OrderIDs = std::vector<uint32_t>;
struct TraderAdapter {
    OrderMap* _orders;
    Api* _trader_api;
    int _mtx_orders = 0;
    std::map<std::string, std::vector<uint64_t>> _cancel_time_cache;
    bool checkCancelLimits(const char*) { return true; }
    bool doCancel(WTSOrderInfo*);
    bool cancel(uint32_t);
    OrderIDs cancel(const char*, bool, double);
};

#include "extracted.inc"

int main(int argc, char** argv)
{
    if (argc != 2) return 10;
    WTSOrderInfo first, second;
    OrderMap orders;
    orders.emplace(1, &first);
    orders.emplace(2, &second);
    Api api;
    TraderAdapter adapter{&orders, &api, 0, {}};
    const std::string mode = argv[1];
    if (mode == "single") {
        if (!adapter.cancel(1)) return 11;
    } else if (mode == "failure") {
        api.succeed = false;
        if (adapter.cancel(1)) return 12;
    } else if (mode == "batch") {
        if (adapter.cancel("", true, 0).size() != 2) return 13;
    } else return 14;
    const auto it = adapter._cancel_time_cache.find("SHFE.au.2212");
    const auto count = it == adapter._cancel_time_cache.end() ? 0 : it->second.size();
    std::cout << mode << " calls=" << api.calls << " cache=" << count << '\n';
    const int expectedCalls = mode == "batch" ? 2 : 1;
    const size_t expectedCache = mode == "batch" ? 2 : (mode == "single" ? 1 : 0);
    return api.calls == expectedCalls && count == expectedCache ? 0 : 1;
}
