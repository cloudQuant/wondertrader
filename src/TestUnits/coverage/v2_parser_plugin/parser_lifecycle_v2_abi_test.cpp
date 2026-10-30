#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Includes/IParserLifecycleV2.h"

/*
 * This is a test-local table validator and fake create sequence. It checks the
 * header contract only; it does not exercise a production loader or decide
 * whether legacy parser providers are accepted.
 */
namespace
{
struct WtParserLifecycleV2HeaderPrefixForTest
{
	uint32_t struct_size;
	uint32_t abi_version;
	WtParserLifecycleV2Capabilities capabilities;
};

struct WtParserLifecycleV2FactoryPrefixForTest
{
	uint32_t struct_size;
	uint32_t abi_version;
	WtParserLifecycleV2Capabilities capabilities;
	uint32_t reserved[2];
	WtParserLifecycleV2PreflightFactoryFn preflight_factory;
};

struct WtParserLifecycleV2DrainPrefixForTest
{
	uint32_t struct_size;
	uint32_t abi_version;
	WtParserLifecycleV2Capabilities capabilities;
	uint32_t reserved[2];
	WtParserLifecycleV2PreflightFactoryFn preflight_factory;
	WtParserLifecycleV2RequestStopFn request_stop;
	WtParserLifecycleV2CanWaitFn can_wait;
	WtParserLifecycleV2WaitDrainedFn wait_drained;
	WtParserLifecycleV2DestroyApiFn destroy_api;
};

static_assert(sizeof(WtParserLifecycleV2Status) == sizeof(int32_t),
	"lifecycle result must keep a fixed-width ABI type");
static_assert(sizeof(((WtParserLifecycleApiV2*)0)->struct_size) == sizeof(uint32_t),
	"struct_size must be fixed-width");
static_assert(sizeof(((WtParserLifecycleApiV2*)0)->abi_version) == sizeof(uint32_t),
	"abi_version must be fixed-width");
static_assert(sizeof(((WtParserLifecycleApiV2*)0)->capabilities) == sizeof(uint64_t),
	"capabilities must be fixed-width");
static_assert(std::is_standard_layout<WtParserLifecycleApiV2>::value,
	"the ABI table must have standard layout");
#define WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(field, prefix_type) \
	static_assert(offsetof(WtParserLifecycleApiV2, field) == offsetof(prefix_type, field), \
		"Parser lifecycle ABI prefix offset mismatch: " #field)
WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(struct_size, WtParserLifecycleV2HeaderPrefixForTest);
WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(abi_version, WtParserLifecycleV2HeaderPrefixForTest);
WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(capabilities, WtParserLifecycleV2HeaderPrefixForTest);
WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(struct_size, WtParserLifecycleV2FactoryPrefixForTest);
WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(abi_version, WtParserLifecycleV2FactoryPrefixForTest);
WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(capabilities, WtParserLifecycleV2FactoryPrefixForTest);
WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(reserved, WtParserLifecycleV2FactoryPrefixForTest);
WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(preflight_factory, WtParserLifecycleV2FactoryPrefixForTest);
WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(struct_size, WtParserLifecycleV2DrainPrefixForTest);
WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(abi_version, WtParserLifecycleV2DrainPrefixForTest);
WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(capabilities, WtParserLifecycleV2DrainPrefixForTest);
WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(reserved, WtParserLifecycleV2DrainPrefixForTest);
WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(preflight_factory, WtParserLifecycleV2DrainPrefixForTest);
WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(request_stop, WtParserLifecycleV2DrainPrefixForTest);
WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(can_wait, WtParserLifecycleV2DrainPrefixForTest);
WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(wait_drained, WtParserLifecycleV2DrainPrefixForTest);
WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH(destroy_api, WtParserLifecycleV2DrainPrefixForTest);
#undef WT_COV_PARSER_V2_ASSERT_OFFSET_MATCH
static_assert(sizeof(WtParserLifecycleV2HeaderPrefixForTest) ==
	WT_PARSER_LIFECYCLE_V2_HEADER_SIZE,
	"the table header size must include capabilities");
static_assert(offsetof(WtParserLifecycleApiV2, preflight_factory) ==
	offsetof(WtParserLifecycleV2FactoryPrefixForTest, preflight_factory),
	"factory preflight must follow the fixed table header");
static_assert(sizeof(WtParserLifecycleV2FactoryPrefixForTest) ==
	WT_PARSER_LIFECYCLE_V2_FACTORY_PREFLIGHT_SIZE,
	"factory preflight size must include the callback");
static_assert(sizeof(WtParserLifecycleV2DrainPrefixForTest) ==
	WT_PARSER_LIFECYCLE_V2_REQUIRED_SIZE,
	"the required table size must include destroy_api");
static_assert(WT_PARSER_LIFECYCLE_V2_ABI_MAJOR == 2u &&
	WT_PARSER_LIFECYCLE_V2_ABI_MINOR == 0u,
	"the initial Parser lifecycle ABI version must remain stable");
static_assert(WT_PARSER_LIFECYCLE_V2_ABI_VERSION == (2u << 16),
	"the version packs major in the high 16 bits and minor in the low 16 bits");
static_assert(WT_PARSER_LIFECYCLE_V2_CAP_FACTORY_PREFLIGHT == UINT64_C(1),
	"factory preflight capability bit is stable");
static_assert(WT_PARSER_LIFECYCLE_V2_CAP_API_DRAIN == (UINT64_C(1) << 1),
	"API drain capability bit is stable");
static_assert(WT_PARSER_LIFECYCLE_V2_DRAINED == 1 &&
	WT_PARSER_LIFECYCLE_V2_PENDING == 2 &&
	WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK == 3 &&
	WT_PARSER_LIFECYCLE_V2_ERROR == -3,
	"wait result values are stable");

WtParserLifecycleV2Status g_preflight_result = WT_PARSER_LIFECYCLE_V2_OK;
unsigned g_preflight_calls = 0;
unsigned g_create_calls = 0;

WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL fixturePreflight()
{
	++g_preflight_calls;
	return g_preflight_result;
}

WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL fixtureRequestStop(void*)
{
	return WT_PARSER_LIFECYCLE_V2_OK;
}

WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL fixtureCanWait(void*)
{
	return WT_PARSER_LIFECYCLE_V2_OK;
}

WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL fixtureWaitDrained(void*, uint32_t)
{
	return WT_PARSER_LIFECYCLE_V2_DRAINED;
}

WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL fixtureDestroyApi(void** api)
{
	if (api == nullptr || *api == nullptr)
		return WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT;
	*api = nullptr;
	return WT_PARSER_LIFECYCLE_V2_OK;
}

WtParserLifecycleApiV2 makeFixtureTable()
{
	WtParserLifecycleApiV2 table = {};
	table.struct_size = WT_PARSER_LIFECYCLE_V2_REQUIRED_SIZE;
	table.abi_version = WT_PARSER_LIFECYCLE_V2_ABI_VERSION;
	table.capabilities = WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES;
	table.preflight_factory = &fixturePreflight;
	table.request_stop = &fixtureRequestStop;
	table.can_wait = &fixtureCanWait;
	table.wait_drained = &fixtureWaitDrained;
	table.destroy_api = &fixtureDestroyApi;
	return table;
}

bool hasField(const WtParserLifecycleApiV2& table, size_t end_offset)
{
	return table.struct_size >= end_offset;
}

bool validateForCreate(const WtParserLifecycleApiV2* table)
{
	if (table == nullptr || !hasField(*table, WT_PARSER_LIFECYCLE_V2_HEADER_SIZE))
		return false;

	const uint32_t major = table->abi_version >> 16;
	if (major != WT_PARSER_LIFECYCLE_V2_ABI_MAJOR ||
		!hasField(*table, WT_PARSER_LIFECYCLE_V2_REQUIRED_SIZE))
		return false;

	if ((table->capabilities & WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES) !=
		WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES)
		return false;
	if (table->reserved[0] != 0 || table->reserved[1] != 0)
		return false;
	return table->preflight_factory != nullptr && table->request_stop != nullptr &&
		table->can_wait != nullptr && table->wait_drained != nullptr &&
		table->destroy_api != nullptr;
}

bool fixtureCreateParserOnlyAfterPreflight(const WtParserLifecycleApiV2* table)
{
	if (!validateForCreate(table))
		return false;
	if (table->preflight_factory() != WT_PARSER_LIFECYCLE_V2_OK)
		return false;
	++g_create_calls;
	return true;
}
}

TEST(ParserLifecycleV2Abi, StructSizesAndCapabilitiesAreExplicit)
{
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_HEADER_SIZE,
		static_cast<uint32_t>(offsetof(WtParserLifecycleApiV2, capabilities) +
			sizeof(((WtParserLifecycleApiV2*)0)->capabilities)));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_FACTORY_PREFLIGHT_SIZE,
		static_cast<uint32_t>(offsetof(WtParserLifecycleApiV2, preflight_factory) +
			sizeof(((WtParserLifecycleApiV2*)0)->preflight_factory)));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_REQUIRED_SIZE,
		static_cast<uint32_t>(offsetof(WtParserLifecycleApiV2, destroy_api) +
			sizeof(((WtParserLifecycleApiV2*)0)->destroy_api)));
}

TEST(ParserLifecycleV2Abi, UndersizedOrIncompleteTableCannotReachCreate)
{
	WtParserLifecycleApiV2 table = makeFixtureTable();
	g_create_calls = 0;
	g_preflight_calls = 0;

	table.struct_size = WT_PARSER_LIFECYCLE_V2_HEADER_SIZE - 1u;
	EXPECT_FALSE(fixtureCreateParserOnlyAfterPreflight(&table));
	EXPECT_EQ(0u, g_create_calls);
	EXPECT_EQ(0u, g_preflight_calls);

	table = makeFixtureTable();
	table.struct_size = WT_PARSER_LIFECYCLE_V2_REQUIRED_SIZE - 1u;
	EXPECT_FALSE(fixtureCreateParserOnlyAfterPreflight(&table));
	EXPECT_EQ(0u, g_create_calls);
	EXPECT_EQ(0u, g_preflight_calls);

	table = makeFixtureTable();
	table.capabilities &= ~WT_PARSER_LIFECYCLE_V2_CAP_API_DRAIN;
	EXPECT_FALSE(fixtureCreateParserOnlyAfterPreflight(&table));
	EXPECT_EQ(0u, g_create_calls);
	EXPECT_EQ(0u, g_preflight_calls);

	table = makeFixtureTable();
	table.destroy_api = nullptr;
	EXPECT_FALSE(fixtureCreateParserOnlyAfterPreflight(&table));
	EXPECT_EQ(0u, g_create_calls);
	EXPECT_EQ(0u, g_preflight_calls);
}

TEST(ParserLifecycleV2Abi, FactoryPreflightRunsBeforeCreateAndMustSucceed)
{
	WtParserLifecycleApiV2 table = makeFixtureTable();
	g_create_calls = 0;
	g_preflight_calls = 0;
	g_preflight_result = WT_PARSER_LIFECYCLE_V2_ERROR;

	EXPECT_FALSE(fixtureCreateParserOnlyAfterPreflight(&table));
	EXPECT_EQ(1u, g_preflight_calls);
	EXPECT_EQ(0u, g_create_calls);

	g_preflight_result = WT_PARSER_LIFECYCLE_V2_OK;
	EXPECT_TRUE(fixtureCreateParserOnlyAfterPreflight(&table));
	EXPECT_EQ(2u, g_preflight_calls);
	EXPECT_EQ(1u, g_create_calls);
}

TEST(ParserLifecycleV2Abi, SameMajorMinorExtensionAndUnknownCapabilitiesAreCompatible)
{
	WtParserLifecycleApiV2 table = makeFixtureTable();
	table.abi_version = (WT_PARSER_LIFECYCLE_V2_ABI_MAJOR << 16) | 7u;
	table.capabilities |= (UINT64_C(1) << 40);

	EXPECT_TRUE(validateForCreate(&table));

	table.abi_version = (3u << 16);
	EXPECT_FALSE(validateForCreate(&table));
}

TEST(ParserLifecycleV2Abi, ModuleDestroyClearsOpaqueHandle)
{
	WtParserLifecycleApiV2 table = makeFixtureTable();
	int parser_instance = 0;
	void* api_handle = &parser_instance;

	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK, table.destroy_api(&api_handle));
	EXPECT_EQ(nullptr, api_handle);
}
