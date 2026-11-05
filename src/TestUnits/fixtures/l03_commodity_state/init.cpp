#include "Includes/WTSContractInfo.hpp"

#include <cstdio>
#include <cstring>
#include <new>

using namespace wtp;

struct Probe final : WTSCommodityInfo {
    ~Probe() override = default;
};

int main() {
    alignas(Probe) unsigned char storage[sizeof(Probe)];
    std::memset(storage, 0xa5, sizeof(storage));
    Probe* p = new (storage) Probe;
    // Observe object representation first; never read an uninitialized field
    // on the current unfixed implementation. A zero-setting setter should not
    // change any bytes if the constructor wrote the same value already.
    //
    // 2026-11-04 (BUG-L03-002, decision card 4a=A): an unconfigured commodity
    // has the effective trading mode TM_None and is not ready. setTradingMode()
    // now also records "trading mode configured" in the state byte, so it is
    // expected to change the object representation and is checked separately
    // below instead of in the byte-identity list; the old probe treated that
    // byte change, and the raw TM_Both zero, as a failure.
    bool ok = !p->isTradingModeConfigured() && p->getTradingMode() == TM_None &&
        !p->canShort() && !p->isT1() && !p->isReady();
    if (!ok) std::puts("FAIL: unconfigured commodity is not TM_None/not ready");
    auto check = [&](auto set) {
        unsigned char before[sizeof(storage)];
        std::memcpy(before, storage, sizeof(storage));
        set();
        ok = (std::memcmp(before, storage, sizeof(storage)) == 0) && ok;
    };
    check([&]{ p->setVolScale(0); });
    check([&]{ p->setPriceTick(0); });
    check([&]{ p->setLotsTick(0); });
    check([&]{ p->setMinLots(0); });
    check([&]{ p->setCategory(static_cast<ContractCategory>(0)); });
    check([&]{ p->setCoverMode(static_cast<CoverMode>(0)); });
    check([&]{ p->setPriceMode(static_cast<PriceMode>(0)); });
    ok = ok && p->getVolScale() == 0 && p->getPriceTick() == 0.0 &&
        p->getLotsTick() == 0.0 && p->getMinLots() == 0.0 &&
        static_cast<unsigned>(p->getCategoty()) == 0 &&
        static_cast<unsigned>(p->getCoverMode()) == 0 &&
        static_cast<unsigned>(p->getPriceMode()) == 0 &&
        p->getTradingMode() == TM_None &&
        !*p->getName() && !*p->getExchg() && !*p->getProduct() &&
        !*p->getCurrency() && !*p->getSession() && !*p->getTradingTpl() &&
        !*p->getFullPid() && p->getSessionInfo() == nullptr && !p->hasValidConfiguration();
    // An explicit TM_Both is a configured value (not the unconfigured default).
    p->setTradingMode(TM_Both);
    ok = ok && p->isTradingModeConfigured() && p->getTradingMode() == TM_Both &&
        p->canShort() && !p->isReady();
    p->~Probe();
    if (!ok) { std::puts("FAIL: default initialization"); return 1; }

    auto* c = WTSCommodityInfo::create("rb", "rebar", "SHFE", "day", "CHINA");
    if (!c) return 2;
    c->setVolScale(10); c->setPriceTick(0.2); c->setLotsTick(1.0); c->setMinLots(1.0);
    c->setCategory(CC_Future); c->setCoverMode(CM_CoverToday);
    c->setPriceMode(PM_Limit); c->setTradingMode(TM_Long);
    ok = !std::strcmp(c->getFullPid(), "SHFE.rb") && c->getVolScale() == 10 &&
        c->getPriceTick() == 0.2 && c->getLotsTick() == 1.0 && c->getMinLots() == 1.0 &&
        c->hasValidConfiguration() && c->isFuture() && c->getCoverMode() == CM_CoverToday &&
        c->getPriceMode() == PM_Limit && c->getTradingMode() == TM_Long && !c->canShort();
    c->release();
    std::printf("initialized and explicitly configured controls: %s; sizeof=%zu\n", ok ? "PASS" : "FAIL", sizeof(Probe));
    return ok ? 0 : 3;
}
