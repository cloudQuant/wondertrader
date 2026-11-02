#define LIB_YD_API_EXPORT
#include "r93_yd_api_stub.h"

namespace
{
	R93YdApiStub g_api;
	int g_create_count = 0;
}

extern "C" YDApi* makeYDApi(const char*)
{
	++g_create_count;
	return &g_api;
}

extern "C" YD_API_EXPORT int r60_yd_fixture_create_count()
{
	return g_create_count;
}

extern "C" YD_API_EXPORT int r60_yd_fixture_start_count()
{
	return g_api.n_start_;
}

extern "C" YD_API_EXPORT int r60_yd_fixture_disconnect_count()
{
	return g_api.n_disconnect_;
}

extern "C" YD_API_EXPORT int r60_yd_fixture_destroy_count()
{
	return g_api.n_destroy_;
}
