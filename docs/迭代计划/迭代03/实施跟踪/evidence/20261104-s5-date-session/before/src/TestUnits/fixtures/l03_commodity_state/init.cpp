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
    bool ok = true;
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
    check([&]{ p->setTradingMode(static_cast<TradingMode>(0)); });
    ok = ok && p->getVolScale() == 0 && p->getPriceTick() == 0.0 &&
        p->getLotsTick() == 0.0 && p->getMinLots() == 0.0 &&
        static_cast<unsigned>(p->getCategoty()) == 0 &&
        static_cast<unsigned>(p->getCoverMode()) == 0 &&
        static_cast<unsigned>(p->getPriceMode()) == 0 &&
        static_cast<unsigned>(p->getTradingMode()) == 0 &&
        !*p->getName() && !*p->getExchg() && !*p->getProduct() &&
        !*p->getCurrency() && !*p->getSession() && !*p->getTradingTpl() &&
        !*p->getFullPid() && p->getSessionInfo() == nullptr && !p->hasValidConfiguration();
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
