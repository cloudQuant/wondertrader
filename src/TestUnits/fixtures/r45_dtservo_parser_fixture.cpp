#include <Includes/IParserApi.h>
#include <Includes/IParserLifecycleV2.h>
#include <Includes/WTSVariant.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#define R45_DTSERVO_PARSER_EXPORT __declspec(dllexport)
#else
#define R45_DTSERVO_PARSER_EXPORT __attribute__((visibility("default")))
#endif

namespace
{
uint32_t g_create_count = 0;
uint32_t g_init_count = 0;
uint32_t g_register_count = 0;
uint32_t g_subscribe_count = 0;
uint32_t g_connect_count = 0;
uint32_t g_release_count = 0;
uint32_t g_delete_count = 0;
uint32_t g_destroy_count = 0;
uint32_t g_unsafe_delete_count = 0;
bool g_provider_quiescent = false;
void (*g_on_connect)() = nullptr;
// Card 9 (iteration 03): the host requires lifecycle V2. 0 = destroy deletes,
// 1 = destroy reports OK but leaves the handle (nonconforming), 2 = ERROR.
int g_destroy_mode = 0;
uint32_t g_stop_count = 0;
uint32_t g_wait_count = 0;
uint32_t g_destroy_api_count = 0;

class R45DtServoParser final : public wtp::IParserApi
{
public:
	bool init(wtp::WTSVariant* cfg) override
	{
		++g_init_count;
		const char* mode = cfg == nullptr ? nullptr : cfg->getCString("r45_init_mode");
		const char* connectMode = cfg == nullptr ? nullptr : cfg->getCString("r45_connect_mode");
		_connectOk = connectMode == nullptr || std::string(connectMode) != "false";
		if (mode != nullptr && std::string(mode) == "throw")
			throw std::runtime_error("R45 controlled parser init exception");
		return mode == nullptr || std::string(mode) != "false";
	}

	bool connect() override
	{
		++g_connect_count;
		if (g_on_connect != nullptr)
			g_on_connect();
		return _connectOk;
	}

	void subscribe(const wtp::CodeSet&) override { ++g_subscribe_count; }
	void registerSpi(wtp::IParserSpi*) override { ++g_register_count; }
	void release() override { ++g_release_count; }
	~R45DtServoParser() override { ++g_destroy_count; }
private:
	bool _connectOk = true;
};
}

extern "C" R45_DTSERVO_PARSER_EXPORT wtp::IParserApi* createParser()
{
	++g_create_count;
	return new R45DtServoParser();
}

namespace
{
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL r45_preflight() { return WT_PARSER_LIFECYCLE_V2_OK; }
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL r45_request_stop(void*) { ++g_stop_count; return WT_PARSER_LIFECYCLE_V2_OK; }
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL r45_can_wait(void*) { return WT_PARSER_LIFECYCLE_V2_OK; }
// This fake has no worker; the independent test barrier stands for drain.
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL r45_wait_drained(void*, uint32_t)
{
	++g_wait_count;
	return g_provider_quiescent ? WT_PARSER_LIFECYCLE_V2_DRAINED : WT_PARSER_LIFECYCLE_V2_PENDING;
}
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL r45_destroy_api(void** api)
{
	++g_destroy_api_count;
	if (g_destroy_mode == 2)
		return WT_PARSER_LIFECYCLE_V2_ERROR;
	if (g_destroy_mode == 1)
		return WT_PARSER_LIFECYCLE_V2_OK;
	++g_delete_count;
	delete static_cast<wtp::IParserApi*>(*api);
	*api = nullptr;
	return WT_PARSER_LIFECYCLE_V2_OK;
}
}

extern "C" R45_DTSERVO_PARSER_EXPORT const WtParserLifecycleApiV2* WT_PARSER_LIFECYCLE_V2_CALL getParserLifecycleApiV2(uint32_t major)
{
	static const WtParserLifecycleApiV2 table = {
		sizeof(WtParserLifecycleApiV2), WT_PARSER_LIFECYCLE_V2_ABI_VERSION,
		WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES, {0, 0},
		r45_preflight, r45_request_stop, r45_can_wait, r45_wait_drained, r45_destroy_api };
	return major == WT_PARSER_LIFECYCLE_V2_ABI_MAJOR ? &table : nullptr;
}

extern "C" R45_DTSERVO_PARSER_EXPORT void r45_dtservo_parser_set_destroy_mode(int mode) { g_destroy_mode = mode; }
extern "C" R45_DTSERVO_PARSER_EXPORT uint32_t r45_dtservo_parser_stop_count() { return g_stop_count; }
extern "C" R45_DTSERVO_PARSER_EXPORT uint32_t r45_dtservo_parser_wait_count() { return g_wait_count; }
extern "C" R45_DTSERVO_PARSER_EXPORT uint32_t r45_dtservo_parser_destroy_api_count() { return g_destroy_api_count; }

extern "C" R45_DTSERVO_PARSER_EXPORT void deleteParser(wtp::IParserApi*& parser)
{
	if (!g_provider_quiescent)
	{
		++g_unsafe_delete_count;
		return;
	}

	++g_delete_count;
	delete parser;
	parser = nullptr;
}

extern "C" R45_DTSERVO_PARSER_EXPORT void r45_dtservo_parser_reset_counts()
{
	g_create_count = 0;
	g_init_count = 0;
	g_register_count = 0;
	g_subscribe_count = 0;
	g_connect_count = 0;
	g_release_count = 0;
	g_delete_count = 0;
	g_destroy_count = 0;
	g_unsafe_delete_count = 0;
	g_provider_quiescent = false;
	g_on_connect = nullptr;
	g_destroy_mode = 0;
	g_stop_count = 0;
	g_wait_count = 0;
	g_destroy_api_count = 0;
}

extern "C" R45_DTSERVO_PARSER_EXPORT void r45_dtservo_parser_set_on_connect(void (*callback)())
{
	g_on_connect = callback;
}

// Tests can only make deletion legal after setting this independent barrier.
extern "C" R45_DTSERVO_PARSER_EXPORT void r45_dtservo_parser_mark_quiescent()
{
	g_provider_quiescent = true;
}

extern "C" R45_DTSERVO_PARSER_EXPORT uint32_t r45_dtservo_parser_create_count() { return g_create_count; }
extern "C" R45_DTSERVO_PARSER_EXPORT uint32_t r45_dtservo_parser_init_count() { return g_init_count; }
extern "C" R45_DTSERVO_PARSER_EXPORT uint32_t r45_dtservo_parser_register_count() { return g_register_count; }
extern "C" R45_DTSERVO_PARSER_EXPORT uint32_t r45_dtservo_parser_subscribe_count() { return g_subscribe_count; }
extern "C" R45_DTSERVO_PARSER_EXPORT uint32_t r45_dtservo_parser_connect_count() { return g_connect_count; }
extern "C" R45_DTSERVO_PARSER_EXPORT uint32_t r45_dtservo_parser_release_count() { return g_release_count; }
extern "C" R45_DTSERVO_PARSER_EXPORT uint32_t r45_dtservo_parser_delete_count() { return g_delete_count; }
extern "C" R45_DTSERVO_PARSER_EXPORT uint32_t r45_dtservo_parser_destroy_count() { return g_destroy_count; }
extern "C" R45_DTSERVO_PARSER_EXPORT uint32_t r45_dtservo_parser_unsafe_delete_count() { return g_unsafe_delete_count; }
