#ifdef TEST_HFT
#include "../../../Includes/HftStrategyDefs.h"
using StrategyBase = HftStrategy; using FactoryBase = IHftStrategyFact; using EnumCallback = FuncEnumHftStrategyCallback;
#else
#include "../../../Includes/SelStrategyDefs.h"
using StrategyBase = SelStrategy; using FactoryBase = ISelStrategyFact; using EnumCallback = FuncEnumSelStrategyCallback;
#endif
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <thread>
static std::atomic<int> calls{0}, deletes{0}, mode{0}, entered{0}, proceed{0};
static void (*callback)() = nullptr;
class Strategy : public StrategyBase {
public:
 explicit Strategy(const char* id) : StrategyBase(id) {}
 const char* getName() override { return "unit"; }
 const char* getFactName() override { return "stable"; }
#ifdef TEST_HFT
 void on_init(IHftStraCtx*) override {}
#endif
};
class Factory : public FactoryBase {
public:
 const char* getName() override { return "stable"; }
 void enumStrategy(EnumCallback) override {}
 StrategyBase* createStrategy(const char*, const char* id) override {
  ++calls;
  int current = mode.exchange(0);
  if (current == 1) return nullptr;
  if (current == 2) throw std::runtime_error("creator failure");
  if (current == 3 && callback) callback();
  if (current == 4) { entered.store(1); while (!proceed.load()) std::this_thread::yield(); }
  return new Strategy(id);
 }
 bool deleteStrategy(StrategyBase* s) override { ++deletes; delete s; return true; }
};
#ifdef TEST_HFT
extern "C" FactoryBase* createStrategyFact() { return new Factory; }
extern "C" void deleteStrategyFact(FactoryBase* f) { delete f; }
#else
extern "C" FactoryBase* createSelStrategyFact() { return new Factory; }
extern "C" void deleteSelStrategyFact(FactoryBase* f) { delete f; }
#endif
extern "C" int fixture_calls() { return calls.load(); }
extern "C" int fixture_deletes() { return deletes.load(); }
extern "C" void fixture_mode(int n) { mode.store(n); }
extern "C" int fixture_entered() { return entered.load(); }
extern "C" void fixture_proceed() { proceed.store(1); }
extern "C" void fixture_callback(void (*f)()) { callback = f; }

// Card 4b=A (iteration 03 s5): current hosts only use the versioned table.
#include "../../../Includes/IStrategyPluginV2.h"
namespace {
void* WT_STRA_PLUGIN_V2_CALL fx_create_v2() { return static_cast<void*>(static_cast<FactoryBase*>(new Factory)); }
void WT_STRA_PLUGIN_V2_CALL fx_destroy_v2(void* f) { delete static_cast<FactoryBase*>(f); }
const WtStrategyPluginApiV2 fx_api = {
 sizeof(WtStrategyPluginApiV2), WT_STRA_PLUGIN_V2_ABI_VERSION,
#ifdef TEST_HFT
 WT_STRA_PLUGIN_V2_FAMILY_HFT,
#else
 WT_STRA_PLUGIN_V2_FAMILY_SEL,
#endif
 WT_STRA_PLUGIN_V2_OBJECT_REVISION, WT_STRA_PLUGIN_V2_CAP_SYNC_TEARDOWN, {0, 0, 0}, fx_create_v2, fx_destroy_v2 };
}
#ifdef TEST_HFT
extern "C" __attribute__((visibility("default"))) const WtStrategyPluginApiV2* getHftStrategyPluginApiV2(uint32_t major)
#else
extern "C" __attribute__((visibility("default"))) const WtStrategyPluginApiV2* getSelStrategyPluginApiV2(uint32_t major)
#endif
{ return major == WT_STRA_PLUGIN_V2_ABI_MAJOR ? &fx_api : nullptr; }
