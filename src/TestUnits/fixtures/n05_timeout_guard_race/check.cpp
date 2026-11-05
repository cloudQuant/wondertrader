#include "../../../WtExeFact/WtOrdMon.h"

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

// A controlled probe of the shared helper, not a full executer/provider test.
// Hold the callback immediately after the helper's unlocked check, then latch an
// expired order before allowing the callback to perform a simulated action.
int main(int argc, char** argv)
{
    const bool sequential = argc == 2 && std::strcmp(argv[1], "sequential") == 0;
    WtOrdMon monitor;
    auto id = monitor.push_order_with_identity(7, 0);
    if (!id.valid()) return 2;
    bool acted = false;
    if (sequential)
    {
        if (!monitor.mark_unverified_timeout(id, 1, 1000)) return 3;
        const bool ran = wtRunIfNoUnverifiedTimeout(monitor, [&] { acted = true; });
        std::printf("latched=%d callback_ran=%d action=%d\n", monitor.has_unverified_timeout(), ran, acted);
        return (!ran && !acted) ? 0 : 4;
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool resume = false;
    std::thread caller([&] {
        wtRunIfNoUnverifiedTimeout(monitor, [&] {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            condition.notify_one();
            condition.wait(lock, [&] { return resume; });
            acted = true;
        });
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return entered; });
        if (!monitor.mark_unverified_timeout(id, 1, 1000))
        {
            resume = true;
            condition.notify_one();
            lock.unlock();
            caller.join();
            return 3;
        }
        resume = true;
        condition.notify_one();
    }
    caller.join();
    std::printf("latched=%d action_after_latch=%d\n", monitor.has_unverified_timeout(), acted);
    return acted ? 1 : 0; // exit 1 is the known concurrency counterexample
}
