#include "../../../Includes/CtaStrategyDefs.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <cstdio>

namespace {
std::atomic<int> creations{0}, names{0}, deletes{0}, entered{0}, proceed{0};
void (*callback)() = nullptr;
const char* mode() { const char* p = std::getenv("WT_CTA_FACTORY_MODE"); return p ? p : "good"; }
void marker(const char* text) { const char* p = std::getenv("WT_CTA_MARKER"); if (p) { if (FILE* f = std::fopen(p, "a")) { std::fputs(text, f); std::fclose(f); } } }
class Strategy : public CtaStrategy {
public: explicit Strategy(const char* id) : CtaStrategy(id) {} const char* getName() override { return "unit"; } const char* getFactName() override { return "stable"; }
};
class Factory : public ICtaStrategyFact {
public:
 Factory() : name(std::getenv("WT_CTA_FACTORY_NAME") ? std::getenv("WT_CTA_FACTORY_NAME") : "stable") {}
 ~Factory() override { marker("factory-delete\n"); }
 const char* getName() override {
  int n = ++names;
  if (std::strcmp(mode(), "name-throw") == 0) throw std::runtime_error("name failure");
  if (std::strcmp(mode(), "empty") == 0) return "";
  if (n > 1 && std::strcmp(mode(), "single-name") == 0) throw std::runtime_error("second name call");
  return name.c_str();
 }
 void enumStrategy(FuncEnumStrategyCallback) override {}
 CtaStrategy* createStrategy(const char* name, const char* id) override { return std::strcmp(name, "unit") == 0 ? new Strategy(id) : nullptr; }
 bool deleteStrategy(CtaStrategy* s) override { ++deletes; delete s; return true; }
private:
 std::string name;
};
__attribute__((destructor)) void on_unload() { marker("unload\n"); }
}
extern "C" ICtaStrategyFact* createStrategyFact() {
 ++creations;
 if (std::strcmp(mode(), "creator-throw") == 0) throw std::runtime_error("creator failure");
 if (std::strcmp(mode(), "null") == 0) return nullptr;
 if (std::strcmp(mode(), "reentrant") == 0 && callback) callback();
 if (std::strcmp(mode(), "concurrent") == 0) {
  entered.store(1, std::memory_order_release);
  while (!proceed.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
 }
 return new Factory;
}
extern "C" void deleteStrategyFact(ICtaStrategyFact* f) { delete f; }
extern "C" int fixture_creations() { return creations.load(); }
extern "C" int fixture_names() { return names.load(); }
extern "C" int fixture_deletes() { return deletes.load(); }
extern "C" int fixture_entered() { return entered.load(std::memory_order_acquire); }
extern "C" void fixture_proceed() { proceed.store(1, std::memory_order_release); }
extern "C" void fixture_set_callback(void (*fn)()) { callback = fn; }

// Card 4b=A (iteration 03 s5): current hosts only use the versioned table.
// create_factory forwards to the legacy creator so every failure mode above
// (throw/null/reentrant/concurrent) still reaches the host unchanged.
#include "../../../Includes/IStrategyPluginV2.h"
namespace {
void* WT_STRA_PLUGIN_V2_CALL fx_create_v2() { return static_cast<void*>(createStrategyFact()); }
void WT_STRA_PLUGIN_V2_CALL fx_destroy_v2(void* f) { delete static_cast<ICtaStrategyFact*>(f); }
const WtStrategyPluginApiV2 fx_api = {
 sizeof(WtStrategyPluginApiV2), WT_STRA_PLUGIN_V2_ABI_VERSION, WT_STRA_PLUGIN_V2_FAMILY_CTA,
 WT_STRA_PLUGIN_V2_OBJECT_REVISION,
 WT_STRA_PLUGIN_V2_CAP_SYNC_TEARDOWN,
 {0, 0, 0}, fx_create_v2, fx_destroy_v2};
}
extern "C" __attribute__((visibility("default"))) const WtStrategyPluginApiV2* getCtaStrategyPluginApiV2(uint32_t major)
{ return major == WT_STRA_PLUGIN_V2_ABI_MAJOR ? &fx_api : nullptr; }
