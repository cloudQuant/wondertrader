#include "Includes/ExecuteDefs.h"

#include <cstdio>
#include <cstdlib>

using namespace wtp;

#if defined(_WIN32)
#define COV_EXPORT __declspec(dllexport)
#else
#define COV_EXPORT __attribute__((visibility("default")))
#endif

namespace
{
void record(const char* event)
{
	const char* path = std::getenv("WT_COV_EXEC_V2_LOG");
	if (path == nullptr || path[0] == '\0')
		return;
	FILE* file = std::fopen(path, "ab");
	if (file == nullptr)
		return;
	std::fprintf(file, "%s\n", event);
	std::fclose(file);
}

struct LoadMarker
{
	LoadMarker() { record("module_loaded"); }
};

LoadMarker g_load_marker;
}

extern "C"
{
COV_EXPORT IExecuterFact* createExecFact()
{
	record("legacy_create_factory");
	return nullptr;
}

COV_EXPORT void deleteExecFact(IExecuterFact*& fact)
{
	record("legacy_delete_factory");
	fact = nullptr;
}
}

