#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
void marker(const char* text) {
 const char* p=std::getenv("WT_SEL_MARKER");
 if (p) if (FILE* f=std::fopen(p,"a")) { std::fputs(text,f); std::fclose(f); }
}
struct Activity {
 std::atomic<bool> stop{false};
 std::thread worker;
 Activity() : worker([this] {
  while (!stop.load(std::memory_order_relaxed)) {
   marker("tick\n"); std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
 }) { marker("load\n"); }
 ~Activity() { stop.store(true,std::memory_order_relaxed); worker.join(); marker("unload\n"); }
};
Activity activity;
}
// Deliberately no createSelStrategyFact export. The static initializer starts a
// worker so premature dlclose is observable without a real plugin or provider.
