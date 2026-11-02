#include "../../../Includes/HftStrategyDefs.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <cstdio>

namespace {
std::atomic<int> creations{0}, names{0}, deletes{0}, factory_deletes{0}, entered{0}, proceed{0};
void (*callback)() = nullptr;
void (*created_callback)() = nullptr;
const char* mode() { const char* p = std::getenv("WT_HFT_FACTORY_MODE"); return p ? p : "good"; }
void marker(const char* text) { const char* p = std::getenv("WT_HFT_MARKER"); if (p) { if (FILE* f = std::fopen(p, "a")) { std::fputs(text, f); std::fclose(f); } } }
class Strategy : public HftStrategy {
public: explicit Strategy(const char* id) : HftStrategy(id) {} const char* getName() override { return "unit"; } const char* getFactName() override { return "stable"; } void on_init(IHftStraCtx*) override {}
};
class Factory : public IHftStrategyFact {
public:
 Factory() : name(std::getenv("WT_HFT_FACTORY_NAME") ? std::getenv("WT_HFT_FACTORY_NAME") : "stable") {}
 ~Factory() override { ++factory_deletes; marker("factory-delete\n"); }
 const char* getName() override {
  int n = ++names;
  if (std::strcmp(mode(), "name-throw") == 0) throw std::runtime_error("name failure");
  if (std::strcmp(mode(), "empty") == 0) return "";
  if (n > 1 && std::strcmp(mode(), "single-name") == 0) throw std::runtime_error("second name call");
  return name.c_str();
 }
 void enumStrategy(FuncEnumHftStrategyCallback) override {}
 HftStrategy* createStrategy(const char* name, const char* id) override {
  HftStrategy* result = std::strcmp(name, "unit") == 0 ? new Strategy(id) : nullptr;
  if (created_callback) created_callback();
  return result;
 }
 bool deleteStrategy(HftStrategy* s) override { ++deletes; marker("strategy-delete\n"); delete s; return true; }
private:
 std::string name;
};
__attribute__((destructor)) void on_unload() { marker("unload\n"); }
}
extern "C" IHftStrategyFact* createStrategyFact() {
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
#ifndef OMIT_REMOVER
extern "C" void deleteStrategyFact(IHftStrategyFact* f) { delete f; }
#endif
extern "C" int fixture_creations() { return creations.load(); }
extern "C" int fixture_names() { return names.load(); }
extern "C" int fixture_deletes() { return deletes.load(); }
extern "C" int fixture_factory_deletes() { return factory_deletes.load(); }
extern "C" int fixture_entered() { return entered.load(std::memory_order_acquire); }
extern "C" void fixture_proceed() { proceed.store(1, std::memory_order_release); }
extern "C" void fixture_set_callback(void (*fn)()) { callback = fn; }
extern "C" void fixture_set_created_callback(void (*fn)()) { created_callback = fn; }
