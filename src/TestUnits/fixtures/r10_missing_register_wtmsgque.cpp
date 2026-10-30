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

	struct ModuleUnloadMarker
	{
		~ModuleUnloadMarker()
		{
			write_marker("WT_R10_MQ_UNLOAD_MARKER", "unloaded");
		}
	};

	ModuleUnloadMarker g_moduleUnloadMarker;
}

extern "C"
{
	// Deliberately omit regiter_callbacks while providing the other current MQ ABI exports.
	EXPORT_FLAG WtUInt32 create_server(const char* /*url*/, bool /*confirm*/)
	{
		write_marker("WT_R10_MQ_CREATE_MARKER", "created");
		return 0xF10u;
	}

	EXPORT_FLAG void destroy_server(WtUInt32 /*id*/)
	{
		write_marker("WT_R10_MQ_DESTROY_MARKER", "destroyed");
	}

	EXPORT_FLAG void publish_message(WtUInt32 /*id*/, const char* /*topic*/,
		const char* /*data*/, WtUInt32 /*dataLen*/)
	{
		write_marker("WT_R10_MQ_PUBLISH_MARKER", "published");
	}
}
