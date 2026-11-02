#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../q01_wtcore3/r90_env.h"

#include <Share/DLLHelper.hpp>
#include <WtCore/WtHelper.h>
#include <WtRunner/WtRunner.h>

namespace
{
namespace n01fs = std::filesystem;

class ParserPluginProbe
{
public:
	using ResetFn = void (*)();
	using QuiesceFn = void (*)();
	using CountFn = uint32_t (*)();

	explicit ParserPluginProbe(const n01fs::path& path)
		: _module(DLLHelper::load_library(path.c_str()))
	{
		if (_module == nullptr)
			return;
		_reset = reinterpret_cast<ResetFn>(DLLHelper::get_symbol(_module, "r130_parser_reset_counts"));
		_proveQuiescent = reinterpret_cast<QuiesceFn>(DLLHelper::get_symbol(_module, "r130_parser_prove_quiescent"));
		_create = reinterpret_cast<CountFn>(DLLHelper::get_symbol(_module, "r130_parser_create_count"));
		_init = reinterpret_cast<CountFn>(DLLHelper::get_symbol(_module, "r130_parser_init_count"));
		_register = reinterpret_cast<CountFn>(DLLHelper::get_symbol(_module, "r130_parser_register_count"));
		_subscribe = reinterpret_cast<CountFn>(DLLHelper::get_symbol(_module, "r130_parser_subscribe_count"));
		_connect = reinterpret_cast<CountFn>(DLLHelper::get_symbol(_module, "r130_parser_connect_count"));
		_release = reinterpret_cast<CountFn>(DLLHelper::get_symbol(_module, "r130_parser_release_count"));
		_delete = reinterpret_cast<CountFn>(DLLHelper::get_symbol(_module, "r130_parser_delete_count"));
		_unsafeRelease = reinterpret_cast<CountFn>(DLLHelper::get_symbol(_module, "r130_parser_unsafe_release_count"));
	}

	~ParserPluginProbe()
	{
		if (_module != nullptr)
			DLLHelper::free_library(_module);
	}

	bool valid() const
	{
		return _module != nullptr && _reset != nullptr && _proveQuiescent != nullptr
			&& _create != nullptr && _init != nullptr && _register != nullptr
			&& _subscribe != nullptr && _connect != nullptr && _release != nullptr
			&& _delete != nullptr && _unsafeRelease != nullptr;
	}

	void reset() const { _reset(); }
	void proveQuiescent() const { _proveQuiescent(); }
	uint32_t createCount() const { return _create(); }
	uint32_t initCount() const { return _init(); }
	uint32_t registerCount() const { return _register(); }
	uint32_t subscribeCount() const { return _subscribe(); }
	uint32_t connectCount() const { return _connect(); }
	uint32_t releaseCount() const { return _release(); }
	uint32_t deleteCount() const { return _delete(); }
	uint32_t unsafeReleaseCount() const { return _unsafeRelease(); }

private:
	DllHandle _module = nullptr;
	ResetFn _reset = nullptr;
	QuiesceFn _proveQuiescent = nullptr;
	CountFn _create = nullptr;
	CountFn _init = nullptr;
	CountFn _register = nullptr;
	CountFn _subscribe = nullptr;
	CountFn _connect = nullptr;
	CountFn _release = nullptr;
	CountFn _delete = nullptr;
	CountFn _unsafeRelease = nullptr;
};

std::string pathWithSlash(const n01fs::path& path)
{
	std::string value = path.string();
	if (!value.empty() && value.back() != '/')
		value.push_back('/');
	return value;
}

// The runner intentionally retains this failed owner because the public API has
// no callback-drain operation. Keep it globally reachable so the test neither
// invokes the fail-closed destructor nor reports an unreachable leak.
WtRunner* g_failed_runner = nullptr;
}

TEST(WtRunnerParserInitFailure, PublicConfigFailsAndQuarantinesUninitializedParser)
{
	const n01fs::path runnerBin = r90::find_bin_dir();
	ASSERT_FALSE(runnerBin.empty()) << "unable to locate the local product module directory";
	const n01fs::path buildRoot = runnerBin.parent_path().parent_path().parent_path();
	const n01fs::path fixture = buildRoot / "TestUnits" / "coverage" / "r130_parser_plugins"
		/ DLLHelper::wrap_module("R130LifecycleParser");
	const n01fs::path storage = runnerBin / DLLHelper::wrap_module("WtDataStorage");
	ASSERT_TRUE(n01fs::exists(fixture)) << fixture.string();
	ASSERT_TRUE(n01fs::exists(storage)) << storage.string();

	static uint32_t sequence = 0;
	const n01fs::path root = n01fs::temp_directory_path()
		/ ("n01_runner_parser_fail_" + std::to_string(++sequence));
	const n01fs::path parserDir = root / "parsers";
	const n01fs::path dataDir = root / "data";
	std::error_code ec;
	n01fs::remove_all(root, ec);
	n01fs::create_directories(parserDir, ec);
	ASSERT_FALSE(ec) << ec.message();
	n01fs::create_directories(dataDir, ec);
	ASSERT_FALSE(ec) << ec.message();
	const n01fs::path parserCopy = parserDir / fixture.filename();
	n01fs::copy_file(fixture, parserCopy, n01fs::copy_options::overwrite_existing, ec);
	ASSERT_FALSE(ec) << ec.message();
	n01fs::copy_file(storage, root / storage.filename(), n01fs::copy_options::overwrite_existing, ec);
	ASSERT_FALSE(ec) << ec.message();

	const n01fs::path policy = root / "bspolicy.json";
	{
		std::ofstream out(policy);
		ASSERT_TRUE(out.is_open());
		out << R"({"default":{}})";
	}
	const n01fs::path config = root / "runner.json";
	{
		std::ofstream out(config);
		ASSERT_TRUE(out.is_open());
		out << "{"
			<< "\"basefiles\":{},"
			<< "\"env\":{\"name\":\"hft\",\"product\":{\"session\":\"TRADING\"}},"
			<< "\"data\":{\"store\":{\"module\":\"WtDataStorage\",\"path\":\""
			<< pathWithSlash(dataDir) << "\"}},"
			<< "\"bspolicy\":\"" << policy.string() << "\","
			<< "\"parsers\":[{\"active\":true,\"id\":\"n01_failed_parser\","
			<< "\"module\":\"R130LifecycleParser\",\"r130_init_ok\":false}],"
			<< "\"traders\":[]"
			<< "}";
	}

	const std::string oldInst = WtHelper::getInstDir();
	WtHelper::setInstDir(pathWithSlash(root).c_str());
	WtHelper::setGenerateDir(pathWithSlash(root).c_str());
	ParserPluginProbe probe(parserCopy);
	ASSERT_TRUE(probe.valid());
	probe.reset();

	g_failed_runner = new WtRunner();
	ASSERT_FALSE(g_failed_runner->config(config.string()));
	EXPECT_EQ(1u, probe.createCount());
	EXPECT_EQ(1u, probe.registerCount());
	EXPECT_EQ(1u, probe.initCount());
	EXPECT_EQ(0u, probe.subscribeCount());
	EXPECT_EQ(0u, probe.connectCount());
	EXPECT_EQ(0u, probe.releaseCount());
	EXPECT_EQ(0u, probe.deleteCount());
	EXPECT_EQ(0u, probe.unsafeReleaseCount());

	// With no provider-specific quiescence proof, Runner shutdown must fail closed
	// and leave the parser owner intact instead of releasing/unloading it.
	EXPECT_FALSE(g_failed_runner->shutdown());
	EXPECT_EQ(0u, probe.releaseCount());
	EXPECT_EQ(0u, probe.deleteCount());
	EXPECT_EQ(0u, probe.unsafeReleaseCount());

	// Restore process-global paths. The intentionally retained runner and its
	// mapped fake remain rooted until process exit, as required by the contract.
	WtHelper::setInstDir(oldInst.c_str());
}
