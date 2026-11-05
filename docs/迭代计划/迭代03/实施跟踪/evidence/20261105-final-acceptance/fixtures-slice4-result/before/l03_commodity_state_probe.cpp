#include "Includes/WTSContractInfo.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <new>
#include <type_traits>

using namespace wtp;

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

    // Compare object representations through unsigned char, never evaluate an
    // uninitialized getter. Each setter writes a value with bytes unlike 0xa5.
    auto check = [&](const char* name, auto set, std::size_t expected) {
        std::array<unsigned char, sizeof(Probe)> before{};
        std::memcpy(before.data(), storage, sizeof(storage));
        set();
        std::size_t changed = 0;
        bool allPoison = true;
        for (std::size_t i=0; i<sizeof(storage); ++i) {
            if (before[i] != storage[i]) {
                ++changed;
                allPoison = allPoison && baseline[i] == 0xa5;
            }
        }
        const bool ok = changed == expected && allPoison;
        std::printf("%s: changed=%zu expected=%zu prior_poison=%s %s\n", name,
                    changed, expected, allPoison ? "yes" : "no", ok ? "OBSERVED" : "DIFFERENT");
        if (!ok) ++failures;
    };
    check("volume scale", [&]{p->setVolScale(0x11223344u);}, sizeof(uint32_t));
    check("price tick", [&]{p->setPriceTick(1.25);}, sizeof(double));
    check("lot tick", [&]{p->setLotsTick(1.25);}, sizeof(double));
    check("minimum lots", [&]{p->setMinLots(1.25);}, sizeof(double));
    check("category", [&]{p->setCategory(CC_Future);}, sizeof(ContractCategory));
    check("cover mode", [&]{p->setCoverMode(CM_CoverToday);}, sizeof(CoverMode));
    check("price mode", [&]{p->setPriceMode(PM_Limit);}, sizeof(PriceMode));
    check("trading mode", [&]{p->setTradingMode(TM_Long);}, sizeof(TradingMode));
    if (p->getVolScale()!=0x11223344u || p->getPriceTick()!=1.25 ||
        p->getLotsTick()!=1.25 || p->getMinLots()!=1.25 ||
        p->getCategoty()!=CC_Future || p->getCoverMode()!=CM_CoverToday ||
        p->getPriceMode()!=PM_Limit || p->getTradingMode()!=TM_Long || p->canShort()) ++failures;
    p->~Probe();

    // Public factory, with all numeric and enum fields configured before use.
    WTSCommodityInfo* c = WTSCommodityInfo::create("rb", "rebar", "SHFE", "day", "CHINA");
    if (!c) return 2;
    c->setVolScale(10); c->setPriceTick(1.0); c->setLotsTick(1.0); c->setMinLots(1.0);
    c->setCategory(CC_Future); c->setCoverMode(CM_CoverToday);
    c->setPriceMode(PM_Limit); c->setTradingMode(TM_Both);
    if (std::strcmp(c->getFullPid(), "SHFE.rb") || c->getVolScale()!=10 ||
        c->getPriceTick()!=1.0 || !c->isFuture() || !c->canShort()) ++failures;
    c->release();
    std::printf("configured control: %s\n", failures ? "FAILED" : "PASS");
    // 0 confirms observed missing initialization; it is not a product pass.
    return failures ? 1 : 0;
}
