#include <cstdio>
#include <cstring>
#include <string>
#include <time.h>
#include "r61_ctpopt_api_stub.h"
#include "Share/TimeUtils.hpp"

static bool failClock = false;
static bool failTimezone = false;
static bool failUtc = false;
static bool failLocal = false;
static bool failMktime = false;
static time_t instant = 1704067200; // 2024-01-01 00:00:00 UTC
static time_t clockSeconds = instant;
static long clockNanos = 123000000;
static unsigned clockCalls = 0;
static bool advanceClock = false;
extern "C" int wt_i03_clock_gettime(clockid_t, struct timespec* ts)
{
    ++clockCalls;
    if (failClock) return -1;
    ts->tv_sec = clockSeconds;
    ts->tv_nsec = clockNanos;
    if (advanceClock) ++clockSeconds;
    return 0;
}
extern "C" time_t wt_i03_time(time_t* out)
{
    if (failTimezone) return static_cast<time_t>(-1);
    if (out) *out = instant;
    return instant;
}
extern "C" struct tm* wt_i03_gmtime_r(const time_t* value, struct tm* out)
{
    return failUtc ? nullptr : ::gmtime_r(value, out);
}
extern "C" struct tm* wt_i03_localtime_r(const time_t* value, struct tm* out)
{
    return failLocal ? nullptr : ::localtime_r(value, out);
}
extern "C" time_t wt_i03_mktime(struct tm* value)
{
    return failMktime ? static_cast<time_t>(-1) : ::mktime(value);
}

class CaptureApi : public R61CtpOptApiStub
{
public:
    unsigned confirmations = 0;
    CThostFtdcSettlementInfoConfirmField request{};
    int ReqSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* p, int) override
    {
        ++confirmations;
        request = *p;
        return stub_ret_;
    }
};
class Sink : public ITraderSpi
{
public:
    std::string log;
    unsigned loginResults = 0;
    IBaseDataMgr* getBaseDataMgr() override { return nullptr; }
    void handleEvent(WTSTraderEvent, int32_t) override {}
    void onLoginResult(bool, const char*, uint32_t) override { ++loginResults; }
    void handleTraderLog(WTSLogLevel, const char* text) override { if (text) log += text; }
};
class Probe : public TraderCTPOpt
{
public:
    void prepare(CaptureApi* api, Sink* sink)
    {
        m_pUserAPI = api;
        m_bscSink = sink;
        m_wrapperState = WS_CONFIRM_QRYED;
        m_lDate = 20240102;
        m_strBroker = "9999";
        m_strUser = "test";
    }
    void noApi() { m_pUserAPI = nullptr; }
    void notReady() { m_wrapperState = WS_NOTLOGIN; }
};
static int expect(bool condition, const char* text)
{
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", text);
    return condition ? 0 : 1;
}
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    ::setenv("TZ", "UTC", 1);
    ::tzset();
    const std::string mode(argv[1]);
#ifdef WT_TIME_SNAPSHOT_API
    if (mode.rfind("local-", 0) == 0) {
        const char* zone = nullptr;
        uint32_t expectedDate = 20240101, expectedTime = 0;
        const char* requestDate = "20240101";
        const char* requestTime = nullptr;
        if (mode == "local-kathmandu") {
            zone = "Asia/Kathmandu"; expectedTime = 54500123; requestTime = "05:45:00";
        } else if (mode == "local-adelaide") {
            zone = "Australia/Adelaide"; expectedTime = 103000123; requestTime = "10:30:00";
        } else if (mode == "local-year-boundary") {
            zone = "Asia/Kathmandu"; instant = clockSeconds = 1704046500;
            expectedTime = 123; requestTime = "00:00:00";
        } else if (mode == "local-dst-transition") {
            zone = "America/New_York"; instant = clockSeconds = 1710053999;
            expectedDate = 20240310; expectedTime = 15959123;
            requestDate = "20240310"; requestTime = "03:00:00";
        } else if (mode == "local-utc") {
            zone = "UTC"; expectedTime = 123; requestTime = "00:00:00";
        } else if (mode == "local-legacy-independent") {
            zone = "UTC"; expectedTime = 123; requestTime = "00:00:00";
            failTimezone = failUtc = failMktime = true;
        } else return 2;
        ::setenv("TZ", zone, 1); ::tzset();
        uint32_t date = 777, time = 888;
        int errors = expect(TimeUtils::tryGetDateTime(date, time), "local snapshot succeeds");
        errors += expect(date == expectedDate && time == expectedTime, "snapshot matches local civil time");
        if (mode == "local-dst-transition") clockSeconds = 1710054000;
        CaptureApi api; Sink sink; Probe probe; probe.prepare(&api, &sink);
        errors += expect(probe.confirm() == 0 && api.confirmations == 1, "confirmation submitted once");
        errors += expect(std::strcmp(api.request.ConfirmDate, requestDate) == 0, "confirmation local date");
        errors += expect(std::strcmp(api.request.ConfirmTime, requestTime) == 0, "confirmation local time");
        errors += expect(clockCalls == 2, "each snapshot samples the clock once");
        std::printf("%s errors=%d date=%u time=%u request=%s %s\n", mode.c_str(), errors,
                    date, time, api.request.ConfirmDate, api.request.ConfirmTime);
        return errors ? 1 : 0;
    }
#endif
    if (mode == "boundary") {
        instant = clockSeconds = 1704067199;
        clockNanos = 999000000;
        advanceClock = true;
    }
#ifdef WT_TIME_SNAPSHOT_EPOCH_API
    if (mode.rfind("epoch-", 0) == 0) {
        uint32_t date = 777, time = 888;
        uint64_t epoch = 999;
        if (mode == "epoch-success") {
            advanceClock = true;
            int errors = expect(TimeUtils::tryGetDateTime(date, time, epoch), "epoch snapshot succeeds");
            errors += expect(date == 20240101 && time == 123 && epoch == 1704067200123ULL,
                             "epoch and local fields derive from the same sample");
            errors += expect(clockCalls == 1, "epoch overload reads clock once");
            return errors ? 1 : 0;
        }
        if (mode == "epoch-clock") failClock = true;
        else if (mode == "epoch-local") failLocal = true;
        else if (mode == "epoch-overflow") clockSeconds = static_cast<time_t>(INT64_MAX);
        else if (mode == "epoch-alias-date-time") {
            const bool ok = TimeUtils::tryGetDateTime(date, date, epoch);
            return expect(!ok && date == 777 && epoch == 999 && clockCalls == 0, "date/time alias rejected");
        } else if (mode == "epoch-alias-epoch") {
            // Construct overlapping references only to test the pre-access range
            // check; the rejected call must not read or write the aliased output.
            alignas(uint64_t) unsigned char storage[sizeof(uint64_t)]{};
            auto& aliasEpoch = *reinterpret_cast<uint64_t*>(storage);
            auto& aliasDate = *reinterpret_cast<uint32_t*>(storage);
            const bool ok = TimeUtils::tryGetDateTime(aliasDate, time, aliasEpoch);
            return expect(!ok && time == 888 && clockCalls == 0, "epoch/date overlap rejected before access");
        } else return 2;
        int errors = expect(!TimeUtils::tryGetDateTime(date, time, epoch) &&
                            date == 777 && time == 888 && epoch == 999,
                            "failed snapshot preserves all outputs");
        failClock = failLocal = false;
        clockSeconds = instant;
        errors += expect(TimeUtils::tryGetDateTime(date, time, epoch) && epoch == 1704067200123ULL,
                         "epoch snapshot recovers after failure");
        return errors ? 1 : 0;
    }
#endif
#ifdef WT_TIME_SNAPSHOT_API
    if (mode.rfind("snapshot-", 0) == 0) {
        uint32_t date = 777, time = 888;
        if (mode == "snapshot-clock") failClock = true;
        else if (mode == "snapshot-timezone") failTimezone = true;
        else if (mode == "snapshot-utc") failUtc = true;
        else if (mode == "snapshot-local") failLocal = true;
        else if (mode == "snapshot-mktime") failMktime = true;
        else if (mode == "snapshot-nanos") clockNanos = 1000000000;
        else if (mode == "snapshot-negative") clockSeconds = -1;
        else if (mode == "snapshot-range") clockSeconds = static_cast<time_t>(INT64_MAX);
        else if (mode == "snapshot-alias") {
            const bool ok = TimeUtils::tryGetDateTime(date, date);
            return expect(!ok && date == 777 && clockCalls == 0, "alias rejects without output or clock read");
        } else return 2;
        const bool failed = TimeUtils::tryGetDateTime(date, time);
        int errors = expect(!failed && date == 777 && time == 888, "failed snapshot preserves both outputs");
        failClock = failTimezone = failUtc = failLocal = failMktime = false;
        clockNanos = 123000000;
        clockSeconds = instant;
        const bool recovered = TimeUtils::tryGetDateTime(date, time);
        errors += expect(recovered && date == 20240101 && time == 123, "same-source snapshot recovers");
        std::printf("%s errors=%d date=%u time=%u\n", mode.c_str(), errors, date, time);
        return errors ? 1 : 0;
    }
#endif
    CaptureApi api;
    Sink sink;
    Probe probe;
    probe.prepare(&api, &sink);
    int errors = 0;
    if (mode == "no-api" || mode == "not-ready") {
        if (mode == "no-api") probe.noApi(); else probe.notReady();
        errors += expect(probe.confirm() < 0, "invalid state is rejected");
        errors += expect(api.confirmations == 0 && clockCalls == 0, "no SDK or clock access");
        return errors ? 1 : 0;
    }
    if (mode == "sdk-failure") {
        api.stub_ret_ = -5;
        errors += expect(probe.confirm() < 0 && api.confirmations == 1, "SDK failure propagates");
        errors += expect(sink.log.find("failed") != std::string::npos, "SDK failure logged");
        return errors ? 1 : 0;
    }
    if (mode == "clock" || mode == "timezone" || mode == "utc" || mode == "local" || mode == "mktime" ||
        mode == "callback-clock" || mode == "callback-timezone" || mode == "callback-utc" || mode == "callback-local" || mode == "callback-mktime") {
        failClock = mode == "clock" || mode == "callback-clock";
        failTimezone = mode == "timezone" || mode == "callback-timezone";
        failUtc = mode == "utc" || mode == "callback-utc";
        failLocal = mode == "local" || mode == "callback-local";
        failMktime = mode == "mktime" || mode == "callback-mktime";
        const bool callback = mode.rfind("callback-", 0) == 0;
        CThostFtdcSettlementInfoConfirmField stale{};
        std::snprintf(stale.ConfirmDate, sizeof(stale.ConfirmDate), "%s", "20230101");
        if (callback) probe.OnRspQrySettlementInfoConfirm(&stale, nullptr, 1, true);
        else errors += expect(probe.confirm() < 0, "time failure returns negative");
        errors += expect(api.confirmations == 0, "time failure makes no SDK request");
        errors += expect(sink.log.find("time") != std::string::npos, "time failure is logged");
        if (callback) {
            errors += expect(sink.loginResults == 0, "callback does not signal login success");
            errors += expect(!probe.isConnected(), "time failure does not report readiness");
        }
        failClock = failTimezone = failUtc = failLocal = failMktime = false;
        if (callback) {
            probe.OnRspQrySettlementInfoConfirm(&stale, nullptr, 2, true);
            errors += expect(api.confirmations == 1, "callback retry sends after recovery");
            errors += expect(sink.loginResults == 0, "request submission is not login readiness");
            errors += expect(!probe.isConnected(), "submission alone is not readiness");
            std::printf("%s errors=%d confirmations=%u\n", mode.c_str(), errors, api.confirmations);
            return errors ? 1 : 0;
        }
    } else if (mode != "success" && mode != "boundary") return 2;
    const int result = probe.confirm();
    errors += expect(result == 0, "success returns zero");
    errors += expect(api.confirmations == 1, "exactly one SDK request after recovery");
    errors += expect(std::strcmp(api.request.BrokerID, "9999") == 0, "broker preserved");
    errors += expect(std::strcmp(api.request.InvestorID, "test") == 0, "user preserved");
    errors += expect(std::strcmp(api.request.ConfirmDate, mode == "boundary" ? "20231231" : "20240101") == 0, "date from snapshot");
    errors += expect(std::strcmp(api.request.ConfirmTime, mode == "boundary" ? "23:59:59" : "00:00:00") == 0, "time from same snapshot");
    if (mode == "boundary") errors += expect(clockCalls == 1, "clock sampled once");
    std::printf("%s errors=%d confirmations=%u date=%s time=%s\n", mode.c_str(), errors,
        api.confirmations, api.request.ConfirmDate, api.request.ConfirmTime);
    return errors ? 1 : 0;
}
