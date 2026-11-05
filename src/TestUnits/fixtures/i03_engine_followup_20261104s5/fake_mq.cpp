// Local fake WtMsgQue for I03-NEW-065. FAKE_CREATE_ID is the value returned by
// create_server (0 = the MQ module could not create the server). No socket,
// network or provider is involved.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#define FAKE_EXPORT extern "C" __attribute__((visibility("default")))
typedef uint32_t WtUInt32;
typedef void (*FuncLogCallback)(WtUInt32, const char*, bool);

#ifndef FAKE_CREATE_ID
#define FAKE_CREATE_ID 0u
#endif

namespace {
std::atomic<int> g_create{0}, g_destroy{0}, g_publish{0}, g_register{0};
}

FAKE_EXPORT void regiter_callbacks(FuncLogCallback) { ++g_register; }
FAKE_EXPORT WtUInt32 create_server(const char*, bool) { ++g_create; return FAKE_CREATE_ID; }
FAKE_EXPORT void destroy_server(WtUInt32) { ++g_destroy; }
#ifdef FAKE_BLOCK_PUBLISH
// K24=E1: publish blocks until the test releases it (bounded stop must not wait for it).
namespace { std::atomic<bool> g_release{false}; std::atomic<int> g_in_publish{0}; }
FAKE_EXPORT void publish_message(WtUInt32, const char*, const char*, WtUInt32)
{
	++g_in_publish;
	while (!g_release.load())
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	++g_publish;
	--g_in_publish;
}
FAKE_EXPORT int fake_release_publish() { g_release.store(true); return 0; }
FAKE_EXPORT int fake_in_publish() { return g_in_publish.load(); }
#else
FAKE_EXPORT void publish_message(WtUInt32, const char*, const char*, WtUInt32) { ++g_publish; }
#endif
FAKE_EXPORT int fake_create_count() { return g_create.load(); }
FAKE_EXPORT int fake_destroy_count() { return g_destroy.load(); }
FAKE_EXPORT int fake_publish_count() { return g_publish.load(); }
FAKE_EXPORT int fake_register_count() { return g_register.load(); }
