#include "WtMsgQue/WtMsgQue.h"

#include <cstdio>
#include <cstdlib>

namespace
{
	void write_marker(const char* environmentName, const char* value)
	{
		const char* path = std::getenv(environmentName);
		if (path == nullptr || path[0] == '\0')
			return;

		FILE* file = std::fopen(path, "wb");
		if (file == nullptr)
			return;
		std::fputs(value, file);
		std::fclose(file);
	}

	struct ModuleMarkers
	{
		ModuleMarkers()
		{
			write_marker("WT_R10_CREATE_MQ_LOAD_MARKER", "loaded");
		}

		~ModuleMarkers()
		{
			write_marker("WT_R10_CREATE_MQ_UNLOAD_MARKER", "unloaded");
		}
	};

	ModuleMarkers g_moduleMarkers;
}

extern "C"
{
	// Deliberately omit create_server while retaining the other required export.
	// If the loader calls any plugin function before validating the full ABI,
	// this marker makes that ordering observable.
	EXPORT_FLAG void regiter_callbacks(FuncLogCallback /*callback*/)
	{
		write_marker("WT_R10_CREATE_MQ_CALLBACK_MARKER", "registered");
	}

	EXPORT_FLAG void destroy_server(WtUInt32 /*id*/)
	{
		write_marker("WT_R10_CREATE_MQ_DESTROY_MARKER", "destroyed");
	}

	EXPORT_FLAG void publish_message(WtUInt32 /*id*/, const char* /*topic*/,
		const char* /*data*/, WtUInt32 /*dataLen*/)
	{
		write_marker("WT_R10_CREATE_MQ_PUBLISH_MARKER", "published");
	}
}
