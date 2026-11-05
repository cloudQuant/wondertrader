#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <time.h>
#include <sys/time.h>
#include <sys/timeb.h>

namespace {
std::atomic<int> failure{0};
std::atomic<int> calls{0};
time_t instant = 1704067200;
bool slowClock = false;

time_t testTime(time_t* out)
{
    ++calls;
    if (slowClock) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    int expected = 1;
    if (failure.compare_exchange_strong(expected, 0)) return static_cast<time_t>(-1);
    if (out) *out = instant;
    return instant;
}
struct tm* testGmtime(const time_t* t, struct tm* out)
{
    int expected = 2;
    if (failure.compare_exchange_strong(expected, 0)) return nullptr;
    return ::gmtime_r(t, out);
}
struct tm* testLocaltime(const time_t* t, struct tm* out)
{
    int expected = 3;
    if (failure.compare_exchange_strong(expected, 0)) return nullptr;
    return ::localtime_r(t, out);
}
time_t testMktime(struct tm* t)
{
    int expected = 4;
    if (failure.compare_exchange_strong(expected, 0)) return static_cast<time_t>(-1);
    return ::mktime(t);
}

int expectedOffset()
{
    struct tm utc{};
    if (!::gmtime_r(&instant, &utc)) std::abort();
    const time_t gt = ::mktime(&utc);
    struct tm local{};
    if (gt == static_cast<time_t>(-1) || !::localtime_r(&gt, &local)) std::abort();
    int32_t result = static_cast<uint32_t>(((instant - gt) + (local.tm_isdst ? 3600 : 0)) / 60);
    return result / 60;
}
}

// Test-only wrappers permit deterministic failures without changing the product API.
#define time testTime
#define gmtime_r testGmtime
#define localtime_r testLocaltime
#define mktime testMktime
#include "TimeUtils.hpp"
#undef mktime
#undef localtime_r
#undef gmtime_r
#undef time

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    const std::string mode(argv[1]);
    if (argc > 2) instant = static_cast<time_t>(std::strtoll(argv[2], nullptr, 10));
    ::tzset();
    const int want = expectedOffset();
    if (mode == "race") {
        slowClock = true;
        constexpr int n = 24;
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::atomic<int> errors{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < n; ++i) threads.emplace_back([&] {
            ++ready;
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            if (TimeUtils::getTZOffset() != want) ++errors;
        });
        while (ready.load() != n) std::this_thread::yield();
        go.store(true, std::memory_order_release);
        for (auto& t : threads) t.join();
        std::printf("expected=%d errors=%d clock_calls=%d\n", want, errors.load(), calls.load());
        return errors.load() ? 1 : 0;
    }
    if (mode == "normal") {
        const int actual = TimeUtils::getTZOffset();
        const int again = TimeUtils::getTZOffset();
        struct tm got{}, expected{};
        const time_t shifted = instant + static_cast<time_t>(actual) * 3600;
        if (TimeUtils::fasttime(instant, &got, actual) != 0 || !::gmtime_r(&shifted, &expected)) return 3;
        const bool dateOk = got.tm_year == expected.tm_year && got.tm_mon == expected.tm_mon &&
            got.tm_mday == expected.tm_mday && got.tm_hour == expected.tm_hour &&
            got.tm_min == expected.tm_min && got.tm_sec == expected.tm_sec;
        std::printf("expected=%d actual=%d again=%d clock_calls=%d\n", want, actual, again, calls.load());
        return actual == want && again == want && calls.load() == 1 && dateOk ? 0 : 1;
    }
    if (mode == "cache") {
        const int first = TimeUtils::getTZOffset();
        instant += 180 * 86400;
        ::setenv("TZ", "Pacific/Honolulu", 1);
        ::tzset();
        const int second = TimeUtils::getTZOffset();
        std::printf("first=%d second=%d clock_calls=%d\n", first, second, calls.load());
        return first == want && second == first && calls.load() == 1 ? 0 : 1;
    }
    const int stage = std::atoi(argv[1]);
    if (stage < 1 || stage > 4) return 2;
    failure = stage;
    const int failed = TimeUtils::getTZOffset();
    const int retry = TimeUtils::getTZOffset();
    const int cached = TimeUtils::getTZOffset();
    std::printf("stage=%d failed=%d retry=%d cached=%d expected=%d clock_calls=%d\n",
                stage, failed, retry, cached, want, calls.load());
    return failed == 99 && retry == want && cached == want && calls.load() == 2 ? 0 : 1;
}
