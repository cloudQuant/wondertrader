#include <gtest/gtest.h>

#include "WtCore/WtExecuterFactory.h"
#include "Includes/IExecPluginV2.h"
#include "Includes/ExecuteDefs.h"
#include "Share/DLLHelper.hpp"

#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern "C"
{
wtp::IExecuterFact* createExecFact();
void deleteExecFact(wtp::IExecuterFact* fact);
}

static_assert(std::is_same<decltype(&createExecFact), wtp::FuncCreateExeFact>::value,
	"the legacy create export declaration must match FuncCreateExeFact");
static_assert(std::is_same<decltype(&deleteExecFact), wtp::FuncDeleteExeFact>::value,
	"the legacy delete export declaration must match FuncDeleteExeFact");

#ifndef WT_COV_EXEC_V2_LEGACY_PATH
#error WT_COV_EXEC_V2_LEGACY_PATH must name the legacy plugin fixture
#endif
#ifndef WT_COV_EXEC_V2_BAD_VERSION_PATH
#error WT_COV_EXEC_V2_BAD_VERSION_PATH must name the bad-version plugin fixture
#endif
#ifndef WT_COV_EXEC_V2_BAD_SIZE_PATH
#error WT_COV_EXEC_V2_BAD_SIZE_PATH must name the small-table plugin fixture
#endif
#ifndef WT_COV_EXEC_V2_BAD_CALLBACK_PATH
#error WT_COV_EXEC_V2_BAD_CALLBACK_PATH must name the missing-callback plugin fixture
#endif
#ifndef WT_COV_EXEC_V2_INIT_FAIL_PATH
#error WT_COV_EXEC_V2_INIT_FAIL_PATH must name the failing-init plugin fixture
#endif
#ifndef WT_COV_EXEC_V2_VALID_PATH
#error WT_COV_EXEC_V2_VALID_PATH must name the valid plugin fixture
#endif
#ifndef WT_COV_EXEC_V2_NULL_FACTORY_PATH
#error WT_COV_EXEC_V2_NULL_FACTORY_PATH must name the null-factory plugin fixture
#endif
#ifndef WT_COV_EXEC_V2_ABI20_PATH
#error WT_COV_EXEC_V2_ABI20_PATH must name the ABI 2.0 plugin fixture
#endif
#ifndef WT_COV_EXEC_V2_NO_QUIESCE_CAPABILITY_PATH
#error WT_COV_EXEC_V2_NO_QUIESCE_CAPABILITY_PATH must name the missing-capability plugin fixture
#endif
#ifndef WT_COV_EXEC_V2_NO_WAIT_QUIESCE_PATH
#error WT_COV_EXEC_V2_NO_WAIT_QUIESCE_PATH must name the missing-wait plugin fixture
#endif
#ifndef WT_COV_EXEC_V2_NO_CAN_WAIT_QUIESCE_PATH
#error WT_COV_EXEC_V2_NO_CAN_WAIT_QUIESCE_PATH must name the missing-can-wait plugin fixture
#endif
#ifndef WT_COV_EXEC_V2_SHORT_QUIESCE_PATH
#error WT_COV_EXEC_V2_SHORT_QUIESCE_PATH must name the short-quiesce-table plugin fixture
#endif
#ifndef WT_COV_EXEC_V2_NO_REQUEST_QUIESCE_PATH
#error WT_COV_EXEC_V2_NO_REQUEST_QUIESCE_PATH must name the missing-request plugin fixture
#endif
#ifndef WT_COV_EXEC_V2_ABI21_PATH
#error WT_COV_EXEC_V2_ABI21_PATH must name the ABI 2.1 plugin fixture
#endif
#ifndef WT_COV_EXEC_V2_SHORT_FACTORY_PREFLIGHT_PATH
#error WT_COV_EXEC_V2_SHORT_FACTORY_PREFLIGHT_PATH must name the short ABI 2.2 fixture
#endif
#ifndef WT_COV_EXEC_V2_NO_FACTORY_PREFLIGHT_CAPABILITY_PATH
#error WT_COV_EXEC_V2_NO_FACTORY_PREFLIGHT_CAPABILITY_PATH must name the missing factory capability fixture
#endif
#ifndef WT_COV_EXEC_V2_NO_FACTORY_PREFLIGHT_PATH
#error WT_COV_EXEC_V2_NO_FACTORY_PREFLIGHT_PATH must name the missing factory preflight fixture
#endif

namespace
{
struct WtExecPluginApiV2Abi20PrefixForTest
{
	uint32_t struct_size;
	uint32_t abi_version;
	uint32_t reserved[2];
	WtExecPluginV2CreateFactoryFn create_factory;
	WtExecPluginV2DestroyFactoryFn destroy_factory;
	WtExecPluginV2GetFactoryNameFn get_factory_name;
	WtExecPluginV2CreateUnitFn create_unit;
	WtExecPluginV2DestroyUnitFn destroy_unit;
	WtExecPluginV2InitializeUnitFn initialize_unit;
};

struct WtExecPluginApiV2Abi21PrefixForTest
{
	uint32_t struct_size;
	uint32_t abi_version;
	uint32_t reserved[2];
	WtExecPluginV2CreateFactoryFn create_factory;
	WtExecPluginV2DestroyFactoryFn destroy_factory;
	WtExecPluginV2GetFactoryNameFn get_factory_name;
	WtExecPluginV2CreateUnitFn create_unit;
	WtExecPluginV2DestroyUnitFn destroy_unit;
	WtExecPluginV2InitializeUnitFn initialize_unit;
	WtExecPluginV2Capabilities capabilities;
	WtExecPluginV2RequestQuiesceFn request_quiesce;
	WtExecPluginV2CanWaitQuiesceFn can_wait_quiesce;
	WtExecPluginV2WaitQuiesceFn wait_quiesce;
};

#define WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(field, prefix_type) \
	static_assert(offsetof(WtExecPluginApiV2, field) == offsetof(prefix_type, field), \
		"ABI prefix offset mismatch: " #field)
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(struct_size, WtExecPluginApiV2Abi20PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(abi_version, WtExecPluginApiV2Abi20PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(reserved, WtExecPluginApiV2Abi20PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(create_factory, WtExecPluginApiV2Abi20PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(destroy_factory, WtExecPluginApiV2Abi20PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(get_factory_name, WtExecPluginApiV2Abi20PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(create_unit, WtExecPluginApiV2Abi20PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(destroy_unit, WtExecPluginApiV2Abi20PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(initialize_unit, WtExecPluginApiV2Abi20PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(struct_size, WtExecPluginApiV2Abi21PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(abi_version, WtExecPluginApiV2Abi21PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(reserved, WtExecPluginApiV2Abi21PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(create_factory, WtExecPluginApiV2Abi21PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(destroy_factory, WtExecPluginApiV2Abi21PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(get_factory_name, WtExecPluginApiV2Abi21PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(create_unit, WtExecPluginApiV2Abi21PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(destroy_unit, WtExecPluginApiV2Abi21PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(initialize_unit, WtExecPluginApiV2Abi21PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(capabilities, WtExecPluginApiV2Abi21PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(request_quiesce, WtExecPluginApiV2Abi21PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(can_wait_quiesce, WtExecPluginApiV2Abi21PrefixForTest);
WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH(wait_quiesce, WtExecPluginApiV2Abi21PrefixForTest);
#undef WT_COV_EXEC_V2_ASSERT_OFFSET_MATCH

static_assert(sizeof(WtExecPluginApiV2Abi20PrefixForTest) == WT_EXEC_PLUGIN_V2_REQUIRED_SIZE,
	"ABI 2.1 and 2.2 must retain the exact ABI 2.0 prefix size");
static_assert(offsetof(WtExecPluginApiV2, initialize_unit) ==
	offsetof(WtExecPluginApiV2Abi20PrefixForTest, initialize_unit),
	"ABI 2.1 and 2.2 must preserve ABI 2.0 fields");
static_assert(offsetof(WtExecPluginApiV2, capabilities) == WT_EXEC_PLUGIN_V2_REQUIRED_SIZE,
	"the ABI 2.1 tail must begin immediately after the ABI 2.0 prefix");
static_assert(offsetof(WtExecPluginApiV2, request_quiesce) >=
	offsetof(WtExecPluginApiV2, capabilities) + sizeof(WtExecPluginV2Capabilities),
	"request_quiesce must follow capabilities");
static_assert(offsetof(WtExecPluginApiV2, can_wait_quiesce) >
	offsetof(WtExecPluginApiV2, request_quiesce), "can_wait_quiesce must follow request_quiesce");
static_assert(offsetof(WtExecPluginApiV2, wait_quiesce) >
	offsetof(WtExecPluginApiV2, can_wait_quiesce), "wait_quiesce must follow can_wait_quiesce");
static_assert(WT_EXEC_PLUGIN_V2_QUIESCE_SIZE == offsetof(WtExecPluginApiV2, wait_quiesce) +
	sizeof(((WtExecPluginApiV2*)0)->wait_quiesce), "quiesce tail size must include wait_quiesce");
static_assert(sizeof(WtExecPluginApiV2Abi21PrefixForTest) == WT_EXEC_PLUGIN_V2_QUIESCE_SIZE,
	"ABI 2.2 must retain the exact ABI 2.1 prefix size");
static_assert(offsetof(WtExecPluginApiV2, wait_quiesce) ==
	offsetof(WtExecPluginApiV2Abi21PrefixForTest, wait_quiesce),
	"ABI 2.2 must preserve every ABI 2.1 field offset");
static_assert(offsetof(WtExecPluginApiV2, can_wait_factory_quiesce) == WT_EXEC_PLUGIN_V2_QUIESCE_SIZE,
	"the ABI 2.2 field must begin immediately after the ABI 2.1 prefix");
static_assert(WT_EXEC_PLUGIN_V2_FACTORY_PREFLIGHT_SIZE ==
	offsetof(WtExecPluginApiV2, can_wait_factory_quiesce) +
	sizeof(((WtExecPluginApiV2*)0)->can_wait_factory_quiesce),
	"factory preflight size must cover the appended callback");
static_assert(WT_EXEC_PLUGIN_V2_ABI_MAJOR == 2u && WT_EXEC_PLUGIN_V2_ABI_MINOR == 2u,
	"factory preflight must be introduced as ABI 2.2 without changing the major version");
static_assert(WT_EXEC_PLUGIN_V2_CAP_QUIESCE == 1u, "quiesce capability bit is stable");
static_assert(WT_EXEC_PLUGIN_V2_CAP_FACTORY_QUIESCE_PREFLIGHT == (1u << 1),
	"factory preflight capability bit is stable");
}

namespace
{
namespace fs = std::filesystem;
using namespace wtp;

class TempPluginDir
{
public:
	TempPluginDir()
	{
		_path = fs::temp_directory_path() /
			("wt_exec_v2_" + std::to_string(static_cast<long long>(::getpid())) +
				"_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
		std::error_code ec;
		fs::remove_all(_path, ec);
		fs::create_directories(_path, ec);
	}

	~TempPluginDir()
	{
		std::error_code ec;
		fs::remove_all(_path, ec);
	}

	std::string path() const { return _path.string(); }
	std::string logPath() const { return (_path / "callbacks.log").string(); }

	void install(const char* source) const
	{
		const fs::path input(source);
		std::error_code ec;
		fs::copy_file(input, _path / input.filename(), fs::copy_options::overwrite_existing, ec);
		ASSERT_FALSE(ec) << "copying plugin fixture failed: " << ec.message();
	}

private:
	fs::path _path;
};

class ScopedCallbackLog
{
public:
	explicit ScopedCallbackLog(const std::string& path)
	{
		const char* prior = std::getenv("WT_COV_EXEC_V2_LOG");
		if (prior != nullptr)
			_prior = prior;
#if defined(_WIN32)
		_putenv_s("WT_COV_EXEC_V2_LOG", path.c_str());
#else
		setenv("WT_COV_EXEC_V2_LOG", path.c_str(), 1);
#endif
	}

	~ScopedCallbackLog()
	{
#if defined(_WIN32)
		_putenv_s("WT_COV_EXEC_V2_LOG", _prior.c_str());
#else
		if (_prior.empty())
			unsetenv("WT_COV_EXEC_V2_LOG");
		else
			setenv("WT_COV_EXEC_V2_LOG", _prior.c_str(), 1);
#endif
	}

private:
	std::string _prior;
};

std::string readLog(const TempPluginDir& root)
{
	std::ifstream input(root.logPath().c_str(), std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

size_t countEvents(const std::string& log, const std::string& event)
{
	const std::string line = event + "\n";
	size_t count = 0;
	for (size_t pos = 0; (pos = log.find(line, pos)) != std::string::npos; pos += line.size())
		++count;
	return count;
}

std::string currentExecutablePath()
{
#ifdef __APPLE__
	uint32_t capacity = 0;
	if (_NSGetExecutablePath(NULL, &capacity) != -1 || capacity == 0)
		return std::string();
	std::vector<char> path(static_cast<size_t>(capacity) + 1, '\0');
	if (_NSGetExecutablePath(path.data(), &capacity) != 0)
		return std::string();
	std::error_code ec;
	const fs::path canonical = fs::weakly_canonical(fs::path(path.data()), ec);
	return ec ? fs::path(path.data()).string() : canonical.string();
#else
	char path[4096] = { 0 };
	const ssize_t length = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (length <= 0)
		return std::string();
	std::error_code ec;
	const fs::path raw(std::string(path, static_cast<size_t>(length)));
	const fs::path canonical = fs::weakly_canonical(raw, ec);
	return ec ? raw.string() : canonical.string();
#endif
}

class ScopedKnownV1Plugin
{
public:
	~ScopedKnownV1Plugin()
	{
		if (_unit != NULL && _factory != NULL && !deleteUnit())
			return;
		if (_factory != NULL && _delete_factory != NULL && !deleteFactory())
			return;
		if (_module != NULL)
			unload();
	}

	bool open(const std::string& path)
	{
		_module = DLLHelper::load_library(path.c_str());
		if (_module == NULL)
			return false;
		_create_factory = reinterpret_cast<wtp::FuncCreateExeFact>(
			DLLHelper::get_symbol(_module, "createExecFact"));
		_delete_factory = reinterpret_cast<wtp::FuncDeleteExeFact>(
			DLLHelper::get_symbol(_module, "deleteExecFact"));
		return _create_factory != NULL && _delete_factory != NULL;
	}

	wtp::IExecuterFact* createFactory()
	{
		_factory = _create_factory ? _create_factory() : NULL;
		return _factory;
	}

	wtp::ExecuteUnit* createUnit(const char* name)
	{
		_unit = _factory ? _factory->createExeUnit(name) : NULL;
		return _unit;
	}

	bool deleteUnit()
	{
		if (_unit == NULL || _factory == NULL)
			return false;
		const bool deleted = _factory->deleteExeUnit(_unit);
		++_unit_delete_calls;
		if (!deleted)
			return false;
		_unit = NULL;
		return true;
	}

	bool deleteFactory()
	{
		if (_factory == NULL || _delete_factory == NULL)
			return false;
		_delete_factory(_factory);
		_factory = NULL;
		++_factory_delete_calls;
		return true;
	}

	bool unload()
	{
		if (_unit != NULL || _factory != NULL || _module == NULL)
			return false;
#ifdef _MSC_VER
		DLLHelper::free_library(_module);
#else
		if (::dlclose(_module) != 0)
			return false;
#endif
		_module = NULL;
		++_module_close_calls;
		return true;
	}

	unsigned unitDeleteCalls() const { return _unit_delete_calls; }
	unsigned factoryDeleteCalls() const { return _factory_delete_calls; }
	unsigned moduleCloseCalls() const { return _module_close_calls; }

private:
	DllHandle _module = NULL;
	wtp::FuncCreateExeFact _create_factory = NULL;
	wtp::FuncDeleteExeFact _delete_factory = NULL;
	wtp::IExecuterFact* _factory = NULL;
	wtp::ExecuteUnit* _unit = NULL;
	unsigned _unit_delete_calls = 0;
	unsigned _factory_delete_calls = 0;
	unsigned _module_close_calls = 0;
};

void expectRejectedBeforeFactoryCall(const char* pluginPath, const char* expectedLog)
{
	TempPluginDir root;
	root.install(pluginPath);
	ScopedCallbackLog callbackLog(root.logPath());
	WtExecuterFactory factory;
	ASSERT_TRUE(factory.loadFactories(root.path().c_str()));
	EXPECT_EQ(nullptr, factory.createExeUnit("FakeV2Factory.Unit").get());
	EXPECT_EQ(expectedLog, readLog(root))
		<< "the plugin must be scanned and rejected before factory creation";
}

TEST(V2ExecuterFactoryTest, RejectsLegacyPluginBeforeCallingUnversionedFactory)
{
	expectRejectedBeforeFactoryCall(WT_COV_EXEC_V2_LEGACY_PATH, "module_loaded\n");
}

TEST(V2ExecuterFactoryTest, KnownBuiltInLegacyExportsUseExactTypesAndReleaseHandleAfterDeletion)
{
	const std::string executable = currentExecutablePath();
	ASSERT_FALSE(executable.empty());
	const fs::path pluginPath = fs::path(executable).parent_path().parent_path() /
		DLLHelper::wrap_module("WtExeFact");
	ASSERT_TRUE(fs::is_regular_file(pluginPath)) << pluginPath.string();

	ScopedKnownV1Plugin plugin;
	ASSERT_TRUE(plugin.open(pluginPath.string())) << pluginPath.string();
	wtp::IExecuterFact* factory = plugin.createFactory();
	ASSERT_NE(nullptr, factory);
	ASSERT_STREQ("WtExeFact", factory->getName());

	wtp::ExecuteUnit* unit = plugin.createUnit("WtMinImpactExeUnit");
	ASSERT_NE(nullptr, unit);
	EXPECT_STREQ("WtMinImpactExeUnit", unit->getName());
	EXPECT_STREQ("WtExeFact", unit->getFactName());
	ASSERT_TRUE(plugin.deleteUnit()) << "the unit must be destroyed through its owning factory";
	ASSERT_TRUE(plugin.deleteFactory()) << "the v1 factory deleter must be called once";
	ASSERT_TRUE(plugin.unload()) << "the module handle may close only after both objects are destroyed";
	EXPECT_EQ(1u, plugin.unitDeleteCalls());
	EXPECT_EQ(1u, plugin.factoryDeleteCalls());
	EXPECT_EQ(1u, plugin.moduleCloseCalls());

	// macOS documents that dylibs with thread-local variables may remain mapped
	// after their last dlclose; WtExeFact's linked image contains TLS. This checks
	// exactly-once object destruction and successful handle close, not physical unmap.
}

TEST(V2ExecuterFactoryTest, RejectsMismatchedMajorBeforeCreatingFactory)
{
	expectRejectedBeforeFactoryCall(WT_COV_EXEC_V2_BAD_VERSION_PATH, "get_api\n");
}

TEST(V2ExecuterFactoryTest, RejectsTruncatedApiTableBeforeCreatingFactory)
{
	expectRejectedBeforeFactoryCall(WT_COV_EXEC_V2_BAD_SIZE_PATH, "get_api\n");
}

TEST(V2ExecuterFactoryTest, RejectsOldAbi20BinaryBeforeCreatingFactory)
{
	expectRejectedBeforeFactoryCall(WT_COV_EXEC_V2_ABI20_PATH, "get_api\n");
}

TEST(V2ExecuterFactoryTest, RejectsOldAbi21BinaryBeforeCreatingFactory)
{
	expectRejectedBeforeFactoryCall(WT_COV_EXEC_V2_ABI21_PATH, "get_api\n");
}

TEST(V2ExecuterFactoryTest, RejectsMissingQuiesceCapabilityBeforeCreatingFactory)
{
	expectRejectedBeforeFactoryCall(WT_COV_EXEC_V2_NO_QUIESCE_CAPABILITY_PATH, "get_api\n");
}

TEST(V2ExecuterFactoryTest, RejectsMissingWaitQuiesceCallbackBeforeCreatingFactory)
{
	expectRejectedBeforeFactoryCall(WT_COV_EXEC_V2_NO_WAIT_QUIESCE_PATH, "get_api\n");
}

TEST(V2ExecuterFactoryTest, RejectsMissingCanWaitQuiesceCallbackBeforeCreatingFactory)
{
	expectRejectedBeforeFactoryCall(WT_COV_EXEC_V2_NO_CAN_WAIT_QUIESCE_PATH, "get_api\n");
}

TEST(V2ExecuterFactoryTest, RejectsTableShortOfQuiesceFieldsBeforeCreatingFactory)
{
	expectRejectedBeforeFactoryCall(WT_COV_EXEC_V2_SHORT_QUIESCE_PATH, "get_api\n");
}

TEST(V2ExecuterFactoryTest, RejectsMissingRequestQuiesceCallbackBeforeCreatingFactory)
{
	expectRejectedBeforeFactoryCall(WT_COV_EXEC_V2_NO_REQUEST_QUIESCE_PATH, "get_api\n");
}

TEST(V2ExecuterFactoryTest, RejectsTableShortOfFactoryPreflightBeforeCreatingFactory)
{
	expectRejectedBeforeFactoryCall(WT_COV_EXEC_V2_SHORT_FACTORY_PREFLIGHT_PATH, "get_api\n");
}

TEST(V2ExecuterFactoryTest, RejectsMissingFactoryPreflightCapabilityBeforeCreatingFactory)
{
	expectRejectedBeforeFactoryCall(WT_COV_EXEC_V2_NO_FACTORY_PREFLIGHT_CAPABILITY_PATH, "get_api\n");
}

TEST(V2ExecuterFactoryTest, RejectsMissingFactoryPreflightCallbackBeforeCreatingFactory)
{
	expectRejectedBeforeFactoryCall(WT_COV_EXEC_V2_NO_FACTORY_PREFLIGHT_PATH, "get_api\n");
}

TEST(V2ExecuterFactoryTest, RejectsMissingRequiredCallbackBeforeCreatingFactory)
{
	expectRejectedBeforeFactoryCall(WT_COV_EXEC_V2_BAD_CALLBACK_PATH, "get_api\n");
}

TEST(V2ExecuterFactoryTest, FailedInitializationReturnsErrorAndDestroysCandidate)
{
	TempPluginDir root;
	root.install(WT_COV_EXEC_V2_INIT_FAIL_PATH);
	ScopedCallbackLog callbackLog(root.logPath());
	{
		WtExecuterFactory factory;
		ASSERT_TRUE(factory.loadFactories(root.path().c_str()));
		ExecuteUnitPtr unit = factory.createExeUnit("FakeV2Factory.Unit");
		ASSERT_NE(nullptr, unit.get());
		ASSERT_NE(nullptr, unit->self());
		std::string error;
		EXPECT_FALSE(unit->initialize(nullptr, "TEST", nullptr, error));
		EXPECT_EQ("controlled initialization failure", error);
		EXPECT_EQ(nullptr, unit->self());
		unit.reset();
	}
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\ndestroy_unit\ndestroy_factory\n", readLog(root));
}

TEST(V2ExecuterFactoryTest, NullFactoryIsUnloadedWithoutBeingPublished)
{
	TempPluginDir root;
	root.install(WT_COV_EXEC_V2_NULL_FACTORY_PATH);
	ScopedCallbackLog callbackLog(root.logPath());

	WtExecuterFactory factory;
	ASSERT_TRUE(factory.loadFactories(root.path().c_str()));
	EXPECT_EQ(nullptr, factory.createExeUnit("NullFactory.Unit").get());

	const std::string log = readLog(root);
	EXPECT_EQ("module_loaded\nget_api\ncreate_factory\nmodule_unloaded\n", log);
}

TEST(V2ExecuterFactoryTest, NullFactoryDoesNotPreventValidPluginLoad)
{
	TempPluginDir root;
	root.install(WT_COV_EXEC_V2_NULL_FACTORY_PATH);
	root.install(WT_COV_EXEC_V2_VALID_PATH);
	WtExecuterFactory factory;
	ASSERT_TRUE(factory.loadFactories(root.path().c_str()));

	EXPECT_EQ(nullptr, factory.createExeUnit("NullFactory.Unit").get());
	ExecuteUnitPtr valid = factory.createExeUnit("FakeV2Factory.Unit");
	ASSERT_NE(nullptr, valid.get());
	ASSERT_NE(nullptr, valid->self());
}

TEST(V2ExecuterFactoryTest, RepeatedLoadSkipsNullAndDuplicateFactoriesSafely)
{
	TempPluginDir root;
	root.install(WT_COV_EXEC_V2_NULL_FACTORY_PATH);
	root.install(WT_COV_EXEC_V2_VALID_PATH);
	ScopedCallbackLog callbackLog(root.logPath());

	{
		WtExecuterFactory factory;
		ASSERT_TRUE(factory.loadFactories(root.path().c_str()));
		ASSERT_TRUE(factory.loadFactories(root.path().c_str()));

		EXPECT_EQ(nullptr, factory.createExeUnit("NullFactory.Unit").get());
		ExecuteUnitPtr valid = factory.createExeUnit("FakeV2Factory.Unit");
		ASSERT_NE(nullptr, valid.get());
		ASSERT_NE(nullptr, valid->self());
	}

	const std::string log = readLog(root);
	EXPECT_EQ(2u, countEvents(log, "module_loaded")) << log;
	EXPECT_EQ(2u, countEvents(log, "module_unloaded")) << log;
	EXPECT_EQ(4u, countEvents(log, "get_api")) << log;
	EXPECT_EQ(4u, countEvents(log, "create_factory")) << log;
	EXPECT_EQ(2u, countEvents(log, "destroy_factory")) << log;
	EXPECT_EQ(1u, countEvents(log, "create_unit")) << log;
	EXPECT_EQ(1u, countEvents(log, "destroy_unit")) << log;
}

TEST(V2ExecuterFactoryTest, WrapperKeepsModuleLoadedAndDestroysUnitBeforeFactory)
{
	TempPluginDir root;
	root.install(WT_COV_EXEC_V2_VALID_PATH);
	ScopedCallbackLog callbackLog(root.logPath());
	ExecuteUnitPtr unit;
	{
		WtExecuterFactory factory;
		ASSERT_TRUE(factory.loadFactories(root.path().c_str()));
		unit = factory.createExeUnit("FakeV2Factory.Unit");
		ASSERT_NE(nullptr, unit.get());
	}

	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\n", readLog(root));
	std::string error;
	EXPECT_TRUE(unit->initialize(nullptr, "TEST", nullptr, error)) << error;
	EXPECT_TRUE(error.empty());
	EXPECT_NE(nullptr, unit->self());
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\n", readLog(root));
	unit->requestQuiesce();
	EXPECT_TRUE(unit->canWaitQuiescedOnCurrentThread());
	EXPECT_TRUE(unit->waitQuiesced());
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\n"
		"request_quiesce\ncan_wait_quiesce\ncan_wait_quiesce\nwait_quiesce\n",
		readLog(root));
	unit.reset();
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\n"
		"request_quiesce\ncan_wait_quiesce\ncan_wait_quiesce\nwait_quiesce\n"
		"destroy_unit\ndestroy_factory\n", readLog(root));
}
}

