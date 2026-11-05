#ifdef TEST_HFT
#include "../../../WtCore/HftStrategyMgr.h"
using Manager = HftStrategyMgr; using Ptr = HftStrategyPtr;
#else
#include "../../../WtCore/SelStrategyMgr.h"
using Manager = SelStrategyMgr; using Ptr = SelStrategyPtr;
#endif
#include <dlfcn.h>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <atomic>
static int failures = 0;
static void check(bool ok, const char* label) { if (!ok) { ++failures; std::fprintf(stderr,"FAIL %s\n", label); } }
static Manager* manager;
static bool inner_ok;
static void reenter() { inner_ok = !manager->createStrategy("stable.unit", "reentrant") && !manager->getStrategy("reentrant"); }
int main(int argc, char** argv) {
 if (argc != 3) return 90;
 void* lib = dlopen(argv[2], RTLD_NOW); if (!lib) return 91;
 auto calls = reinterpret_cast<int(*)()>(dlsym(lib,"fixture_calls"));
 auto deletes = reinterpret_cast<int(*)()>(dlsym(lib,"fixture_deletes"));
 auto mode = reinterpret_cast<void(*)(int)>(dlsym(lib,"fixture_mode"));
 auto entered = reinterpret_cast<int(*)()>(dlsym(lib,"fixture_entered"));
 auto proceed = reinterpret_cast<void(*)()>(dlsym(lib,"fixture_proceed"));
 auto callback = reinterpret_cast<void(*)(void(*)())>(dlsym(lib,"fixture_callback"));
 if (!calls || !deletes || !mode || !entered || !proceed || !callback) return 92;
 {
  Manager m; manager = &m;
  check(m.loadFactories(argv[1]), "load factory");
  Ptr original = m.createStrategy("stable.unit", "duplicate");
  int n = calls();
  check(original && original->self(), "initial creation");
  check(!m.createStrategy("stable", "unit", "duplicate") && calls() == n, "duplicate three args avoids creator");
  check(m.getStrategy("duplicate") == original && deletes() == 0, "original retained");
  Ptr different = m.createStrategy("stable", "unit", "different");
  n = calls();
  check(different && !m.createStrategy("stable.unit", "different") && calls() == n, "other ID and qualified duplicate");
  mode(1); check(!m.createStrategy("stable.unit", "retry-null"), "null failure");
  check(m.createStrategy("stable", "unit", "retry-null") != nullptr, "null retry");
  mode(2); bool threw = false; try { m.createStrategy("stable", "unit", "retry-throw"); } catch (const std::runtime_error&) { threw = true; }
  check(threw && m.createStrategy("stable.unit", "retry-throw") != nullptr, "throw retry");
  callback(reenter); mode(3); Ptr outer = m.createStrategy("stable", "unit", "reentrant"); callback(nullptr);
  check(inner_ok && outer && m.getStrategy("reentrant") == outer, "same-thread reentrant duplicate");
  mode(4); Ptr concurrent; std::thread worker([&] { concurrent = m.createStrategy("stable.unit", "parallel"); });
  while (!entered()) std::this_thread::yield();
  n = calls();
  check(!m.createStrategy("stable", "unit", "parallel") && calls() == n, "concurrent duplicate avoids creator");
  check(!m.getStrategy("parallel"), "pending not published");
  Ptr parallelOther = m.createStrategy("stable", "unit", "parallel-other");
  check(parallelOther != nullptr, "different ID progresses while creator blocked");
  proceed(); worker.join();
  check(concurrent && m.getStrategy("parallel") == concurrent, "concurrent original published");
 }
 std::printf("%s\n", failures ? "FAIL" : "PASS");
 return failures ? 1 : 0;
}
