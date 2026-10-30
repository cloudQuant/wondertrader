#include <gtest/gtest.h>

#include "WtCore/WtExecuterFactory.h"
#include "Includes/IExecPluginV2.h"

#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include <unistd.h>

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

