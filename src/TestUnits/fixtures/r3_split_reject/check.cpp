#include <algorithm>
#include <cfloat>
#include <climits>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>
using std::min;

// buy/sell are extracted verbatim from the current production TU. Every other
// component below is a deterministic substitute; in particular the seam models
// a prospective rate limiter rejecting just the third order before provider API.
enum { AT_Open, AT_CloseToday, AT_CloseYestoday, AT_Close };
enum { CM_CoverToday, CM_OpenCover };
enum { LL_ERROR, LL_WARN, LL_INFO };
struct decimal {
    static bool eq(double a, double b) { return a == b; }
    static bool gt(double a, double b) { return a > b; }
};
struct WTSLogger { template <class... T> static void log_dyn(T&&...) {} };
struct WtHelper { static int getTime() { return 930; } };
struct WTSSessionInfo { bool isInTradingTime(int, bool) { return true; } };
struct WTSCommodityInfo {
    WTSSessionInfo session;
    WTSSessionInfo* getSessionInfo() { return &session; }
    int getCoverMode() const { return CM_OpenCover; }
    const char* getFullPid() const { return "SHFE.rb"; }
};
struct WTSContractInfo {
    WTSCommodityInfo commodity;
    WTSCommodityInfo* getCommInfo() { return &commodity; }
    double getMaxMktVol() const { return 1; }
    double getMaxLmtVol() const { return 1; }
};
struct TradeStatInfo { double l_openvol = 0, s_openvol = 0; };
struct WTSTradeStateInfo {
    TradeStatInfo state;
    static WTSTradeStateInfo* create(const char*) { return new WTSTradeStateInfo; }
    TradeStatInfo& statInfo() { return state; }
};
struct Stats {
    WTSTradeStateInfo* value = nullptr;
    ~Stats() { delete value; }
    WTSTradeStateInfo* get(const char*) { return value; }
    void add(const char*, WTSTradeStateInfo* v, bool) { value = v; }
};
struct PosItem {
    double s_newavail = 0, s_preavail = 0, s_prevol = 0, s_newvol = 0;
    double l_newavail = 0, l_preavail = 0, l_prevol = 0, l_newvol = 0;
    double avail_pos(bool) const { return 0; }
};
struct ActionRule { int _atype = AT_Open; double _limit_l = 0, _limit_s = 0, _limit = 0; bool _pure = false; };
using ActionRuleGroup = std::vector<ActionRule>;
struct ActionPolicyMgr {
    ActionRuleGroup rules{{}};
    const ActionRuleGroup& getActionRules(const char*) { return rules; }
};
using OrderIDs = std::vector<uint32_t>;
struct TraderAdapter {
    std::string _id = "test";
    std::map<std::string, PosItem> _positions;
    Stats stats;
    Stats* _stat_map = &stats;
    ActionPolicyMgr policy;
    ActionPolicyMgr* _policy_mgr = &policy;
    WTSContractInfo contract;
    double undone = 0;
    unsigned attempts = 0, accepted = 0;
    bool rejectThird = false;
    bool isSelfMatched(const char*) { return false; }
    WTSContractInfo* getContract(const char*) { return &contract; }
    void updateUndone(const char*, double qty, bool) { undone += qty; }
    uint32_t submit() { ++attempts; if (rejectThird && attempts == 3) return UINT_MAX; return ++accepted; }
    uint32_t openLong(const char*, double, double, int, WTSContractInfo*) { return submit(); }
    uint32_t openShort(const char*, double, double, int, WTSContractInfo*) { return submit(); }
    uint32_t closeLong(const char*, double, double, bool, int, WTSContractInfo*) { return submit(); }
    uint32_t closeShort(const char*, double, double, bool, int, WTSContractInfo*) { return submit(); }
    OrderIDs buy(const char*, double, double, int, bool, WTSContractInfo*);
    OrderIDs sell(const char*, double, double, int, bool, WTSContractInfo*);
};

#include "extracted.inc"

int main(int argc, char** argv)
{
    if (argc != 2) return 10;
    const std::string mode(argv[1]);
    const bool buy = mode.rfind("buy_", 0) == 0;
    const bool sell = mode.rfind("sell_", 0) == 0;
    if (!buy && !sell) return 11;
    TraderAdapter adapter;
    adapter.rejectThird = mode.find("reject") != std::string::npos;
    auto ids = buy ? adapter.buy("SHFE.rb.2510", 1, 3, 0, false, nullptr)
                   : adapter.sell("SHFE.rb.2510", 1, 3, 0, false, nullptr);
    const auto invalid = std::count(ids.begin(), ids.end(), UINT_MAX);
    const double expectedUndone = (buy ? 1.0 : -1.0) * static_cast<double>(adapter.accepted);
    std::cout << mode << " attempted=" << adapter.attempts << " accepted=" << adapter.accepted
              << " returned=" << ids.size() << " invalid=" << invalid
              << " undone=" << adapter.undone << " expected=" << expectedUndone << '\n';
    return ids.size() == adapter.accepted && invalid == 0 && adapter.undone == expectedUndone ? 0 : 1;
}
