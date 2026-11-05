// Test-side access to fake_provider.cpp through dlopen/dlsym only.
#pragma once

#include <dlfcn.h>
#include <string>

namespace dtfake
{
enum Counter
{
	C_CREATE, C_REGISTER, C_UNREGISTER, C_INIT, C_SUBSCRIBE, C_CONNECT, C_RELEASE,
	C_DELETE_OK, C_DELETE_UNSAFE, C_REQUEST_STOP, C_CAN_WAIT, C_WAIT, C_DESTROY,
	C_CALLBACKS, C_CALLBACKS_AFTER_RELEASE, C_PREFLIGHT, C_LIVE, C_COUNT
};
enum Key { K_INIT_OK = 0, K_START_THREAD = 1, K_WAIT_MODE = 2, K_DESTROY_MODE = 3, K_INIT_THROWS = 4, K_INTERVAL_US = 5, K_QUOTE_MODE = 6 };

struct Probe
{
	void* handle = nullptr;
	void (*reset)() = nullptr;
	int (*count)(int) = nullptr;
	void (*set)(int, int) = nullptr;
	void (*set_hook)(void (*)(void*), void*) = nullptr;

	bool open(const std::string& path)
	{
		// RTLD_NOLOAD would fail before the adapter loads it; keep our own
		// reference so the counters survive adapter unloads in one test.
		handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (handle == nullptr)
			return false;
		reset = reinterpret_cast<void (*)()>(dlsym(handle, "fake_reset"));
		count = reinterpret_cast<int (*)(int)>(dlsym(handle, "fake_count"));
		set = reinterpret_cast<void (*)(int, int)>(dlsym(handle, "fake_set"));
		set_hook = reinterpret_cast<void (*)(void (*)(void*), void*)>(dlsym(handle, "fake_set_hook"));
		return reset && count && set && set_hook;
	}
	int operator[](int i) const { return count(i); }
};
}
