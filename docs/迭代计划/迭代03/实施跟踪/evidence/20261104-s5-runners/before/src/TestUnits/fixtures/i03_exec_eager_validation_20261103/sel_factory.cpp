// Fake SEL strategy factory for BUG-R134-003 / BUG-WSN06-001. Loaded only by
// dlopen from <cwd>/sel/. Strategies: Good (init true), FailInit (init false),
// ThrowInit (init throws). Counters are read by the test through the same
// module handle. No market data, provider or order path is involved.
#include "../../../Includes/SelStrategyDefs.h"

#include <atomic>
#include <cstring>
#include <stdexcept>

namespace {
std::atomic<int> g_created{0}, g_deleted{0}, g_init{0}, g_on_init{0};

class Strategy : public SelStrategy
{
public:
	Strategy(const char* id, const char* unit) : SelStrategy(id), _unit(unit) {}
	const char* getName() override { return _unit; }
	const char* getFactName() override { return "I03SelFact"; }
	bool init(WTSVariant*) override
	{
		++g_init;
		if (std::strcmp(_unit, "FailInit") == 0)
			return false;
		if (std::strcmp(_unit, "ThrowInit") == 0)
			throw std::runtime_error("i03 init failure");
		return true;
	}
	void on_init(ISelStraCtx*) override { ++g_on_init; }
private:
	const char* _unit;
};

class Factory : public ISelStrategyFact
{
public:
	const char* getName() override { return "I03SelFact"; }
	void enumStrategy(FuncEnumSelStrategyCallback) override {}
	SelStrategy* createStrategy(const char* name, const char* id) override
	{
		static const char* const kUnits[] = { "Good", "FailInit", "ThrowInit" };
		for (const char* unit : kUnits)
		{
			if (std::strcmp(name, unit) == 0)
			{
				++g_created;
				return new Strategy(id, unit);
			}
		}
		return nullptr;
	}
	bool deleteStrategy(SelStrategy* s) override
	{
		++g_deleted;
		delete s;
		return true;
	}
};
} // namespace

extern "C" __attribute__((visibility("default"))) ISelStrategyFact* createSelStrategyFact() { return new Factory; }
extern "C" __attribute__((visibility("default"))) void deleteSelStrategyFact(ISelStrategyFact* f) { delete f; }
extern "C" __attribute__((visibility("default"))) void i03_sel_reset()
{
	g_created = 0; g_deleted = 0; g_init = 0; g_on_init = 0;
}
// 0 created, 1 deleted, 2 init calls, 3 on_init calls
extern "C" __attribute__((visibility("default"))) int i03_sel_count(int which)
{
	switch (which)
	{
	case 0: return g_created.load();
	case 1: return g_deleted.load();
	case 2: return g_init.load();
	case 3: return g_on_init.load();
	default: return -1;
	}
}
