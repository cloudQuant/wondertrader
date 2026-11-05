#include <Includes/IParserApi.h>
#include <Includes/IParserLifecycleV2.h>
#include <Includes/WTSVariant.hpp>

#include <cstdint>

#if defined(_WIN32)
#define R130_PARSER_EXPORT __declspec(dllexport)
#else
#define R130_PARSER_EXPORT __attribute__((visibility("default")))
#endif

namespace
{
uint32_t g_create_count = 0;
uint32_t g_init_count = 0;
uint32_t g_register_count = 0;
uint32_t g_subscribe_count = 0;
uint32_t g_last_subscribe_size = 0;
uint32_t g_connect_count = 0;
uint32_t g_release_count = 0;
uint32_t g_delete_count = 0;
uint32_t g_unsafe_release_count = 0;
bool g_quiescent = false;

class R130LifecycleParser final : public wtp::IParserApi
{
public:
	bool init(wtp::WTSVariant* cfg) override
	{
		++g_init_count;
		return cfg == nullptr || cfg->getBoolean("r130_init_ok", true);
	}
	bool connect() override
	{
		++g_connect_count;
		return true;
	}
	void subscribe(const wtp::CodeSet& set) override
	{
		++g_subscribe_count;
		g_last_subscribe_size = static_cast<uint32_t>(set.size());
	}
	void registerSpi(wtp::IParserSpi*) override { ++g_register_count; }
	void release() override
	{
		++g_release_count;
		if (!g_quiescent)
			++g_unsafe_release_count;
	}
};
}

extern "C" R130_PARSER_EXPORT wtp::IParserApi* createParser()
{
	++g_create_count;
	return new R130LifecycleParser();
}

extern "C" R130_PARSER_EXPORT void deleteParser(wtp::IParserApi*& parser)
{
	++g_delete_count;
	delete parser;
	parser = nullptr;
}

// I03-NEW-061 (card 9a): hosts require a parser lifecycle V2 table before
// createParser. The fake has no asynchronous producer; the test's explicit
// r130_parser_prove_quiescent() signal is what makes wait_drained report
// DRAINED. destroy_api performs the final synchronous IParserApi::release and
// the deletion inside this module, so the release/delete counters keep their
// original meaning (one each, and only after the quiescence signal).
namespace
{
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL r130_preflight() { return WT_PARSER_LIFECYCLE_V2_OK; }
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL r130_request_stop(void*) { return WT_PARSER_LIFECYCLE_V2_OK; }
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL r130_can_wait(void*) { return WT_PARSER_LIFECYCLE_V2_OK; }
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL r130_wait_drained(void*, uint32_t)
{
	return g_quiescent ? WT_PARSER_LIFECYCLE_V2_DRAINED : WT_PARSER_LIFECYCLE_V2_PENDING;
}
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL r130_destroy_api(void** api)
{
	wtp::IParserApi* parser = static_cast<wtp::IParserApi*>(*api);
	parser->release();
	++g_delete_count;
	delete parser;
	*api = nullptr;
	return WT_PARSER_LIFECYCLE_V2_OK;
}
}

extern "C" R130_PARSER_EXPORT const WtParserLifecycleApiV2* WT_PARSER_LIFECYCLE_V2_CALL getParserLifecycleApiV2(uint32_t major)
{
	static const WtParserLifecycleApiV2 table = {
		sizeof(WtParserLifecycleApiV2), WT_PARSER_LIFECYCLE_V2_ABI_VERSION,
		WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES, {0, 0},
		r130_preflight, r130_request_stop, r130_can_wait, r130_wait_drained, r130_destroy_api };
	return major == WT_PARSER_LIFECYCLE_V2_ABI_MAJOR ? &table : nullptr;
}

extern "C" R130_PARSER_EXPORT void r130_parser_reset_counts()
{
	g_create_count = 0;
	g_init_count = 0;
	g_register_count = 0;
	g_subscribe_count = 0;
	g_last_subscribe_size = 0;
	g_connect_count = 0;
	g_release_count = 0;
	g_delete_count = 0;
	g_unsafe_release_count = 0;
	g_quiescent = false;
}

extern "C" R130_PARSER_EXPORT void r130_parser_prove_quiescent()
{
	g_quiescent = true;
}

extern "C" R130_PARSER_EXPORT uint32_t r130_parser_create_count()
{
	return g_create_count;
}

extern "C" R130_PARSER_EXPORT uint32_t r130_parser_release_count()
{
	return g_release_count;
}

extern "C" R130_PARSER_EXPORT uint32_t r130_parser_init_count()
{
	return g_init_count;
}

extern "C" R130_PARSER_EXPORT uint32_t r130_parser_register_count()
{
	return g_register_count;
}

extern "C" R130_PARSER_EXPORT uint32_t r130_parser_subscribe_count()
{
	return g_subscribe_count;
}

extern "C" R130_PARSER_EXPORT uint32_t r130_parser_last_subscribe_size()
{
	return g_last_subscribe_size;
}

extern "C" R130_PARSER_EXPORT uint32_t r130_parser_connect_count()
{
	return g_connect_count;
}

extern "C" R130_PARSER_EXPORT uint32_t r130_parser_delete_count()
{
	return g_delete_count;
}

extern "C" R130_PARSER_EXPORT uint32_t r130_parser_unsafe_release_count()
{
	return g_unsafe_release_count;
}
