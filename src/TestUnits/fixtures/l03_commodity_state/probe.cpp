#include "Includes/WTSContractInfo.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <new>
#include <type_traits>

using namespace wtp;

// 2026-11-05 (final acceptance): BUG-L03-002 is decided (card 4a=A) and implemented:
// the constructor zero-fills every numeric/enum field, the object starts not ready, and
// the effective trading mode is TM_None until setTradingMode(). The 2026-11-02 version of
// this probe observed the opposite (setter bytes still held the 0xa5 pre-fill, i.e. the
// fields were left uninitialized) and exited 0 on that defect; its output is kept in
// evidence-20261102/. The probe now asserts the decided contract through the same object
// representation technique: every byte a setter writes was already initialized to zero by
// the constructor (none still holds the 0xa5 pre-fill), and getters are read only on the
// constructed (now initialized) object. Exit 0 means the contract holds.

struct Probe final : WTSCommodityInfo {
    Probe() : WTSCommodityInfo() {}
    ~Probe() override = default;
};

static_assert(!std::is_copy_constructible<WTSCommodityInfo>::value,
              "WTSCommodityInfo should not be assumed copyable");
static_assert(!std::is_copy_constructible<Probe>::value,
              "WTSObject's atomic reference count prevents implicit copying");

int main() {
    alignas(Probe) unsigned char storage[sizeof(Probe)];
    std::memset(storage, 0xa5, sizeof(storage));
    Probe* p = new (storage) Probe;
    unsigned char baseline[sizeof(Probe)];
    std::memcpy(baseline, storage, sizeof(storage));
    unsigned failures = 0;

    // Unconfigured state (card 4a=A): not ready, effective TM_None, cannot short.
    const bool unconfigured = !p->isReady() && !p->hasValidConfiguration() &&
        !p->isTradingModeConfigured() && p->getTradingMode() == TM_None && !p->canShort() && !p->isT1();
    std::printf("unconfigured state (not ready, TM_None, no short): %s\n", unconfigured ? "OK" : "VIOLATED");
    if (!unconfigured) ++failures;

    // Each setter writes a value with bytes unlike 0x00 where possible; every byte it
    // changes must have been zero after construction (initialized, not the pre-fill).
    // maxChanged: the field size; setTradingMode also records its configured bit in the
    // state byte, so it may change one more byte.
    auto check = [&](const char* name, auto set, std::size_t maxChanged) {
        std::array<unsigned char, sizeof(Probe)> before{};
        std::memcpy(before.data(), storage, sizeof(storage));
        set();
        std::size_t changed = 0;
        bool priorZero = true;
        for (std::size_t i=0; i<sizeof(storage); ++i) {
            if (before[i] != storage[i]) {
                ++changed;
                priorZero = priorZero && baseline[i] == 0x00;
            }
        }
        const bool ok = changed >= 1 && changed <= maxChanged && priorZero;
        std::printf("%s: changed=%zu max=%zu prior_zero=%s %s\n", name,
                    changed, maxChanged, priorZero ? "yes" : "no", ok ? "INITIALIZED" : "DIFFERENT");
        if (!ok) ++failures;
    };
    check("volume scale", [&]{p->setVolScale(0x11223344u);}, sizeof(uint32_t));
    check("price tick", [&]{p->setPriceTick(1.25);}, sizeof(double));
    check("lot tick", [&]{p->setLotsTick(1.25);}, sizeof(double));
    check("minimum lots", [&]{p->setMinLots(1.25);}, sizeof(double));
    check("category", [&]{p->setCategory(CC_Future);}, sizeof(ContractCategory));
    check("cover mode", [&]{p->setCoverMode(CM_CoverToday);}, sizeof(CoverMode));
    check("price mode", [&]{p->setPriceMode(PM_Limit);}, sizeof(PriceMode));
    check("trading mode", [&]{p->setTradingMode(TM_Long);}, sizeof(TradingMode) + 1);
    if (p->getVolScale()!=0x11223344u || p->getPriceTick()!=1.25 ||
        p->getLotsTick()!=1.25 || p->getMinLots()!=1.25 ||
        p->getCategoty()!=CC_Future || p->getCoverMode()!=CM_CoverToday ||
        p->getPriceMode()!=PM_Limit || p->getTradingMode()!=TM_Long || p->canShort() ||
        !p->isTradingModeConfigured() || p->isReady()) ++failures;
    p->~Probe();

    // Public factory, with all numeric and enum fields configured before use.
    WTSCommodityInfo* c = WTSCommodityInfo::create("rb", "rebar", "SHFE", "day", "CHINA");
    if (!c) return 2;
    c->setVolScale(10); c->setPriceTick(1.0); c->setLotsTick(1.0); c->setMinLots(1.0);
    c->setCategory(CC_Future); c->setCoverMode(CM_CoverToday);
    c->setPriceMode(PM_Limit); c->setTradingMode(TM_Both);
    if (std::strcmp(c->getFullPid(), "SHFE.rb") || c->getVolScale()!=10 ||
        c->getPriceTick()!=1.0 || !c->isFuture() || !c->canShort() ||
        !c->hasValidConfiguration() || c->isReady()) ++failures;	// no session bound: not ready
    c->release();
    std::printf("card 4a=A initialization contract: %s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
