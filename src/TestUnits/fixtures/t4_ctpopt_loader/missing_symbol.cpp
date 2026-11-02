#include <fstream>

#ifndef WT_T4_CTP_OPT_UNLOAD_MARKER
#define WT_T4_CTP_OPT_UNLOAD_MARKER "ctpopt_missing_symbol_unloaded"
#endif

#if defined(_WIN32)
#define T4_CTP_OPT_EXPORT __declspec(dllexport)
#else
#define T4_CTP_OPT_EXPORT __attribute__((visibility("default")))
#endif

namespace
{
struct UnloadMarker
{
	~UnloadMarker()
	{
		std::ofstream marker(WT_T4_CTP_OPT_UNLOAD_MARKER);
		marker << "unloaded\n";
	}
};

UnloadMarker g_unloadMarker;
}

extern "C" T4_CTP_OPT_EXPORT void wt_t4_ctpopt_missing_symbol_marker()
{
}
