#include "Includes/WTSVariant.hpp"
#include "Includes/CtaStrategyDefs.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

// Inspect the stored configuration to verify a failed reinitialization is atomic.
#define private public
#include "WtCtaStraFact/WtStraDualThrust.h"
#undef private

USING_NS_WTP;
const char* FACT_NAME = "fixture";

struct Config {
    WTSVariant* value = WTSVariant::createObject();
    ~Config() { value->release(); }
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
    Config() = default;
};

static void common(Config& c) {
    c.value->append("period", "d1");
    c.value->append("count", uint32_t(10));
    c.value->append("code", "SHFE.au.2005");
    c.value->append("k1", 0.5);
    c.value->append("k2", 0.7);
    c.value->append("stock", true);
}

static int failures = 0;
static void check(bool condition, const char* label) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}

int main() {
    WtStraDualThrust dt("test");
    Config good; common(good); good.value->append("days", uint32_t(5));
    check(dt.init(good.value), "valid config");
    check(dt._days == 5 && dt._count == 10 && dt._k1 == 0.5 && dt._k2 == 0.7 &&
          dt._period == "d1" && dt._code == "SHFE.au.2005" && dt._isstk,
          "valid values committed");
    Config missing; common(missing);
    check(!dt.init(missing.value) && dt._days == 5, "missing days rejected without publishing");
    Config one; common(one); one.value->append("days", "1");
    check(dt.init(one.value) && dt._days == 1, "days one accepted");
    Config exponent; common(exponent); exponent.value->append("days", "1.20e1");
    check(dt.init(exponent.value) && dt._days == 12, "integral scientific days accepted");
    Config max; common(max); max.value->append("days", uint32_t(4294967295u));
    check(dt.init(max.value) && dt._days == 4294967295u, "uint32 maximum accepted");
    Config defaultCoefficients; defaultCoefficients.value->append("days", uint32_t(2));
    defaultCoefficients.value->append("period", "d1"); defaultCoefficients.value->append("code", "SHFE.au.2005");
    defaultCoefficients.value->append("count", uint32_t(10));
    check(dt.init(defaultCoefficients.value) && dt._k1 == 0 && dt._k2 == 0, "omitted coefficients retain zero defaults");
    check(dt.init(good.value), "reset good config");

    auto rejected = [&](Config& cfg, const char* label) {
        check(!dt.init(cfg.value), label);
        check(dt._days == 5 && dt._count == 10 && dt._k1 == 0.5 && dt._k2 == 0.7 &&
              dt._period == "d1" && dt._code == "SHFE.au.2005" && dt._isstk,
              "failed config preserves all previous values");
    };
    Config zero; common(zero); zero.value->append("days", uint32_t(0)); rejected(zero, "explicit zero");
    Config negative; common(negative); negative.value->append("days", int32_t(-1)); rejected(negative, "negative days");
    Config bad; common(bad); bad.value->append("days", "5garbage"); rejected(bad, "nonnumeric days");
    Config overflow; common(overflow); overflow.value->append("days", uint64_t(4294967296ULL)); rejected(overflow, "overflow days");
    Config nearOverflow; common(nearOverflow); nearOverflow.value->append("days", "4294967295.0000001"); rejected(nearOverflow, "fractional overflow days");
    Config fraction; common(fraction); fraction.value->append("days", 1.5); rejected(fraction, "fractional days");
    Config nan; common(nan); nan.value->append("days", std::numeric_limits<double>::quiet_NaN()); rejected(nan, "NaN days");
    Config boolean; common(boolean); boolean.value->append("days", true); rejected(boolean, "boolean days");
    Config array; common(array); WTSVariant* a = WTSVariant::createArray(); array.value->append("days", a, false); rejected(array, "array days");
    Config badcount; badcount.value->append("days", uint32_t(2)); badcount.value->append("period", "d1"); badcount.value->append("code", "SHFE.au.2005"); badcount.value->append("count", "-1"); rejected(badcount, "negative count");
    Config missingcount; missingcount.value->append("days", uint32_t(2)); missingcount.value->append("period", "d1"); missingcount.value->append("code", "SHFE.au.2005"); rejected(missingcount, "missing count");
    Config missingcode; missingcode.value->append("days", uint32_t(2)); missingcode.value->append("period", "d1"); missingcode.value->append("count", uint32_t(10)); rejected(missingcode, "missing code");
    Config emptyperiod; emptyperiod.value->append("days", uint32_t(2)); emptyperiod.value->append("period", ""); emptyperiod.value->append("code", "SHFE.au.2005"); emptyperiod.value->append("count", uint32_t(10)); rejected(emptyperiod, "empty period");
    Config badk; badk.value->append("days", uint32_t(2)); badk.value->append("period", "d1"); badk.value->append("code", "SHFE.au.2005"); badk.value->append("count", uint32_t(10)); badk.value->append("k1", std::numeric_limits<double>::infinity()); rejected(badk, "infinite coefficient");
    Config badk2; badk2.value->append("days", uint32_t(2)); badk2.value->append("period", "d1"); badk2.value->append("code", "SHFE.au.2005"); badk2.value->append("count", uint32_t(10)); badk2.value->append("k2", "not-a-number"); rejected(badk2, "invalid coefficient");
    check(!dt.init(nullptr), "null config");
    std::cout << (failures == 0 ? "PASS" : "FAIL") << " i03_dualthrust_config, failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
