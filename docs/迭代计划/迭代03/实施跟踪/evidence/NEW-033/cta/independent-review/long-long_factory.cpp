#include "/Users/yunjinqi/Documents/new_projects/wondertrader/src/Includes/CtaStrategyDefs.h"
#include <string>
#include <cstring>
class S : public CtaStrategy {
 public: explicit S(const char* id):CtaStrategy(id) {} const char* getName() override { return "unit"; } const char* getFactName() override { return "long"; }
};
class F : public ICtaStrategyFact {
 std::string n = std::string(4096, 'x');
 public: const char* getName() override { return n.c_str(); } void enumStrategy(FuncEnumStrategyCallback) override {}
 CtaStrategy* createStrategy(const char* name, const char* id) override { return std::strcmp(name,"unit") == 0 ? new S(id) : nullptr; }
 bool deleteStrategy(CtaStrategy* s) override { delete s; return true; }
};
extern "C" ICtaStrategyFact* createStrategyFact() { return new F; }
