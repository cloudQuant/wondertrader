#include "TraderDumper/Dumper.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <thread>
#include <string>

void setDumperWorkerWaitHookForTest(void (*)());
static std::atomic<bool> entered{false};
static void mark() { entered.store(true, std::memory_order_release); }
int main(int argc, char** argv) {
    if (argc != 2) return 10;
    std::set_terminate([] { std::cerr << "std::terminate\n"; std::_Exit(88); });
    Dumper dumper;
    if (!dumper.config("{\"config\":{\"refresh_span\":60},\"basefiles\":{},\"traders\":[]}", false, "/tmp/")) return 11;
    setDumperWorkerWaitHookForTest(mark);
    dumper.run(false);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    if (!entered.load(std::memory_order_acquire)) return 12;
    std::cerr << "worker entered wait\n";
    try {
        if (std::string(argv[1]) == "duplicate_run") {
            dumper.run(false);
            std::cerr << "second run returned\n";
        }
        dumper.release();
        std::cerr << "first release returned\n";
        if (std::string(argv[1]) == "repeat_release") {
            dumper.release();
            std::cerr << "second release returned\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "caught exception: " << e.what() << '\n';
        return 7;
    }
    setDumperWorkerWaitHookForTest(nullptr);
    return 0;
}
