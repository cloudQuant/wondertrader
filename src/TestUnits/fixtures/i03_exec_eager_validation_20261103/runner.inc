// Appended to m_WtRunner/r104_runner_executers_test.cpp by prepare.py; reuses its
// fixture()/basefiles_block()/tail_block()/prepare_base_fixtures()/make_runner().
// BUG-R10-001 / BUG-P02-002 (runner executer entry, card 8b=A) and
// BUG-R134-003 / BUG-WSN06-001 (SEL routing, create/init failure rollback,
// context on_init lifecycle). Only public/runner-private entry points that
// exist in the baseline are used, so the same file is the red control.
#include "WTSUtils/WTSCfgLoader.h"

namespace {

std::size_t i03ExecCount(WtExecuterMgr& manager)
{
	std::size_t n = 0;
	manager.enum_executer([&n](ExecCmdPtr) { ++n; });
	return n;
}

struct I03ExecList
{
	explicit I03ExecList(const std::string& json) : root(WTSCfgLoader::load_from_content(json, false)) {}
	~I03ExecList() { if (root) root->release(); }
	WTSVariant* list() const { return root ? root->get("executers") : nullptr; }
	WTSVariant* root;
};

const char* kI03Policy = R"("scale":1,"policy":{"default":{"name":"WtExeFact.WtMinImpactExeUnit"}})";

std::string i03Local(const char* id) { return std::string(R"({"active":true,"id":")") + id + R"(","name":"local",)" + kI03Policy + "}"; }
std::string i03Diff(const char* id) { return std::string(R"({"active":true,"id":")") + id + R"(","name":"diff",)" + kI03Policy + "}"; }
std::string i03List(std::initializer_list<std::string> items)
{
	std::string out = R"({"executers":[)";
	bool first = true;
	for (const std::string& item : items) { out += (first ? "" : ",") + item; first = false; }
	return out + "]}";
}

struct I03RunnerOwner
{
	I03RunnerOwner() : runner(make_runner()) { prepare_base_fixtures(); runner->init(fixture("logcfg.yaml")); }
	~I03RunnerOwner() { if (runner) EXPECT_TRUE(runner->shutdown()); }
	std::unique_ptr<WtRunner> runner;
	std::size_t cta() { return i03ExecCount(runner->_cta_engine.getExecuterMgr()); }
	std::size_t sel() { return i03ExecCount(runner->_sel_engine.getExecuterMgr()); }
};

void i03ExpectRejectedWithoutRegistration(const std::string& json)
{
	I03RunnerOwner owner;
	I03ExecList cfg(json);
	ASSERT_NE(cfg.list(), nullptr) << json;
	EXPECT_FALSE(owner.runner->initExecuters(cfg.list())) << json;
	EXPECT_EQ(owner.cta(), 0u) << json;
	EXPECT_EQ(owner.sel(), 0u) << json;
}

class I03SelProbe
{
public:
	using Reset = void (*)();
	using Count = int (*)(int);
	I03SelProbe()
	{
		const std::string path = WtHelper::getCWD() + "sel/" + DLLHelper::wrap_module("I03SelFact", "lib");
		module = DLLHelper::load_library(path.c_str());
		if (module)
		{
			reset = reinterpret_cast<Reset>(DLLHelper::get_symbol(module, "i03_sel_reset"));
			count = reinterpret_cast<Count>(DLLHelper::get_symbol(module, "i03_sel_count"));
		}
		if (valid())
			reset();
	}
	bool valid() const { return module && reset && count; }
	int created() const { return count(0); }
	int deleted() const { return count(1); }
	int inits() const { return count(2); }
	int onInits() const { return count(3); }
	// Intentionally not unloaded: the SEL manager keeps factories resident.
	DllHandle module = nullptr;
	Reset reset = nullptr;
	Count count = nullptr;
};

std::string i03SelEnvBlock()
{
	return std::string("env:\n")
		+ "    name: sel\n"
		+ "    poolsize: 0\n"; // no fees path: the fee gate keeps the legacy unconfigured branch
}

// K27=A (2026-11-04): d/w/m/y fire at the HHMM time; for the min period `time` is the
// interval in minutes and must be 1..1440, so callers pass it explicitly for "min".
std::string i03SelEntry(const char* id, const char* name, const char* period = "d", const char* time = "1500")
{
	return std::string("    -   active: true\n        id: ") + id + "\n        name: " + name
		+ "\n        period: " + period + "\n        date: 1\n        time: " + time + "\n        params: {}\n";
}

// strategies.cta carries an active entry with a factory that does not exist: if
// the SEL route still read the CTA list, config() would fail creating it.
std::string i03SelConfig(const std::string& selEntries)
{
	return basefiles_block() + i03SelEnvBlock() + tail_block()
		+ "strategies:\n"
		+ "    cta:\n"
		+ "    -   active: true\n        id: i03_cta_must_not_load\n        name: I03NoSuchCtaFact.Nope\n"
		+ "    sel:\n" + selEntries;
}

} // namespace

// ---------------------------------------------------------------- executers

TEST(I03RunnerExecEager, InvalidLocalInMiddleRegistersNothing)
{
	i03ExpectRejectedWithoutRegistration(i03List({ i03Local("i03_a"),
		R"({"active":true,"id":"i03_b","name":"local","scale":1,"policy":{"SHFE.ag":{"name":"WtExeFact.WtMinImpactExeUnit"}}})",
		i03Diff("i03_c") }));
}

TEST(I03RunnerExecEager, InvalidDiffInMiddleRegistersNothing)
{
	i03ExpectRejectedWithoutRegistration(i03List({ i03Diff("i03_a"),
		R"({"active":true,"id":"i03_b","name":"diff","scale":1})", i03Local("i03_c") }));
}

TEST(I03RunnerExecEager, InvalidPolicyNameAfterValidRegistersNothing)
{
	i03ExpectRejectedWithoutRegistration(i03List({ i03Local("i03_a"), i03Diff("i03_b"),
		R"({"active":true,"id":"i03_c","name":"local","scale":1,"policy":{"default":{"name":"NoSeparator"}}})" }));
}

TEST(I03RunnerExecEager, UnsupportedKindAfterValidRegistersNothing)
{
	// WtRunner has no "arbi" executer kind; Arbi is validated at the executer level.
	i03ExpectRejectedWithoutRegistration(i03List({ i03Local("i03_a"),
		std::string(R"({"active":true,"id":"i03_b","name":"arbi",)") + kI03Policy + "}" }));
}

TEST(I03RunnerExecEager, DuplicateIdAfterValidRegistersNothing)
{
	i03ExpectRejectedWithoutRegistration(i03List({ i03Local("i03_dup"), i03Diff("i03_dup") }));
}

TEST(I03RunnerExecEager, NonObjectEntryAfterValidRegistersNothing)
{
	i03ExpectRejectedWithoutRegistration(i03List({ i03Local("i03_a"), R"("not-an-object")" }));
}

TEST(I03RunnerExecEager, ValidListRegistersEveryKindOnCtaEngine)
{
	I03RunnerOwner owner;
	I03ExecList cfg(i03List({ i03Local("i03_ok_local"), i03Diff("i03_ok_diff"),
		R"({"active":true,"id":"i03_ok_dist","name":"dist","scale":1})",
		R"({"active":false,"id":"i03_inactive","name":"local"})" }));
	ASSERT_NE(cfg.list(), nullptr);
	EXPECT_TRUE(owner.runner->initExecuters(cfg.list()));
	EXPECT_EQ(owner.cta(), 3u);
	EXPECT_EQ(owner.sel(), 0u);
}

TEST(I03RunnerExecEager, SelModeRegistersExecutersOnSelEngine)
{
	I03RunnerOwner owner;
	owner.runner->_is_sel = true;
	I03ExecList cfg(i03List({ i03Local("i03_sel_local"), i03Diff("i03_sel_diff") }));
	ASSERT_NE(cfg.list(), nullptr);
	EXPECT_TRUE(owner.runner->initExecuters(cfg.list()));
	EXPECT_EQ(owner.sel(), 2u);
	EXPECT_EQ(owner.cta(), 0u);
}

TEST(I03RunnerExecEager, ConfigRejectsInvalidExecuterBeforeAnyMutation)
{
	I03RunnerOwner owner;
	const std::string cfg = basefiles_block() + env_block() + tail_block()
		+ "executers:\n"
		+ "-   id: i03_cfg_ok\n    active: true\n    name: local\n    scale: 1\n"
		+ "    policy:\n        default:\n            name: WtExeFact.WtMinImpactExeUnit\n"
		+ "-   id: i03_cfg_bad\n    active: true\n    name: diff\n    scale: 1\n"
		+ "-   id: i03_cfg_after\n    active: true\n    name: dist\n";
	write_fixture("i03_bad_exec.yaml", cfg);
	EXPECT_FALSE(owner.runner->config(fixture("i03_bad_exec.yaml")));
	// Rejected in the pure preflight: the runner did not adopt the new config,
	// did not create an engine and registered nothing.
	EXPECT_EQ(owner.runner->_config, nullptr);
	EXPECT_EQ(owner.runner->_engine, nullptr);
	EXPECT_EQ(owner.cta(), 0u);
	EXPECT_EQ(owner.sel(), 0u);
}

// ---------------------------------------------------------------- SEL strategies

TEST(I03RunnerSelStrategies, ValidSelConfigCreatesInitializesAndRunsOnInit)
{
	I03SelProbe probe;
	ASSERT_TRUE(probe.valid());
	{
		I03RunnerOwner owner;
		write_fixture("i03_sel_ok.yaml", i03SelConfig(i03SelEntry("i03_sel_a", "I03SelFact.Good")
			+ i03SelEntry("i03_sel_b", "I03SelFact.Good", "min", "5")));
		ASSERT_TRUE(owner.runner->config(fixture("i03_sel_ok.yaml")));
		EXPECT_EQ(owner.runner->_engine, static_cast<WtEngine*>(&owner.runner->_sel_engine));
		EXPECT_EQ(probe.created(), 2);
		EXPECT_EQ(probe.inits(), 2);
		EXPECT_EQ(probe.onInits(), 0); // contexts do not run on_init before the engine starts
		owner.runner->_sel_engine.on_init();
		EXPECT_EQ(probe.onInits(), 2);
	}
	EXPECT_EQ(probe.deleted(), probe.created());
}

namespace {
void i03ExpectSelBatchRejected(const std::string& tag, const std::string& entries,
	int expectCreated, int expectInits, bool preflight)
{
	I03SelProbe probe;
	ASSERT_TRUE(probe.valid());
	{
		I03RunnerOwner owner;
		write_fixture(tag + ".yaml", i03SelConfig(entries));
		EXPECT_FALSE(owner.runner->config(fixture(tag + ".yaml"))) << tag;
		if (preflight)
			EXPECT_EQ(owner.runner->_config, nullptr) << tag;
		EXPECT_EQ(probe.created(), expectCreated) << tag;
		EXPECT_EQ(probe.inits(), expectInits) << tag;
		// No context was registered: starting the engine reaches no strategy.
		owner.runner->_sel_engine.on_init();
		EXPECT_EQ(probe.onInits(), 0) << tag;
	}
	EXPECT_EQ(probe.deleted(), probe.created()) << tag;
}
} // namespace

TEST(I03RunnerSelStrategies, InitFalseInBatchRegistersNothing)
{
	i03ExpectSelBatchRejected("i03_sel_initfalse", i03SelEntry("i03_s1", "I03SelFact.Good")
		+ i03SelEntry("i03_s2", "I03SelFact.FailInit") + i03SelEntry("i03_s3", "I03SelFact.Good"), 3, 2, false);
}

TEST(I03RunnerSelStrategies, InitThrowInBatchRegistersNothing)
{
	i03ExpectSelBatchRejected("i03_sel_throw", i03SelEntry("i03_t1", "I03SelFact.Good")
		+ i03SelEntry("i03_t2", "I03SelFact.ThrowInit"), 2, 2, false);
}

TEST(I03RunnerSelStrategies, UnknownStrategyFailsBeforeAnyInit)
{
	i03ExpectSelBatchRejected("i03_sel_unknown", i03SelEntry("i03_u1", "I03SelFact.Good")
		+ i03SelEntry("i03_u2", "I03SelFact.Nope"), 1, 0, false);
}

TEST(I03RunnerSelStrategies, InvalidPeriodIsRejectedInPreflight)
{
	i03ExpectSelBatchRejected("i03_sel_period", i03SelEntry("i03_p1", "I03SelFact.Good")
		+ i03SelEntry("i03_p2", "I03SelFact.Good", "q"), 0, 0, true);
}

TEST(I03RunnerSelStrategies, MissingNameIsRejectedInPreflight)
{
	i03ExpectSelBatchRejected("i03_sel_noname", i03SelEntry("i03_n1", "I03SelFact.Good")
		+ "    -   active: true\n        id: i03_n2\n        period: d\n", 0, 0, true);
}
