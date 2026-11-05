#include <Includes/IParserApi.h>
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
