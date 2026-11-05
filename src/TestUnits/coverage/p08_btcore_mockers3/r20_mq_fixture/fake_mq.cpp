// Compile with OMIT_CREATE, OMIT_DESTROY, OMIT_REGISTER, OMIT_PUBLISH or
// FAIL_CREATE to exercise each ABI edge without starting a real MQ server.
// WT_R20_MQ_SLOW_PUBLISH_US (environment) makes publish_message sleep so a
// concurrent release can be observed while publications are in flight; the
// fake records protocol violations (publish into or destroy while a
// publication is still in flight / after destroy) as separate log lines.
#include "WtMsgQue/WtMsgQue.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>

namespace {
std::mutex g_log_mutex;
std::atomic<int> g_inflight{0};
std::atomic<bool> g_destroyed{false};

void record(const char* event, WtUInt32 id = 0, WtUInt32 len = 0)
{
    const char* path = std::getenv("WT_R20_MQ_LOG");
    if (!path) return;
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::ofstream(path, std::ios::app) << event << ' ' << id << ' ' << len << '\n';
}
}
#ifndef OMIT_REGISTER
extern "C" void regiter_callbacks(FuncLogCallback cb)
{
    record("register");
    if (cb) cb(0xf1234567u, "fake", true);
}
#endif
#ifndef OMIT_CREATE
extern "C" WtUInt32 create_server(const char*, bool)
{
    record("create");
#ifdef FAIL_CREATE
    return 0;
#else
    g_destroyed.store(false);
    return 0xf1234567u;
#endif
}
#endif
#ifndef OMIT_DESTROY
extern "C" void destroy_server(WtUInt32 id)
{
    if (g_inflight.load() != 0) record("destroy-while-publishing", id);
    g_destroyed.store(true);
    record("destroy", id);
}
#endif
#ifndef OMIT_PUBLISH
extern "C" void publish_message(WtUInt32 id, const char*, const char*, WtUInt32 len)
{
    g_inflight.fetch_add(1);
    if (g_destroyed.load()) record("publish-after-destroy", id, len);
    const char* slow = std::getenv("WT_R20_MQ_SLOW_PUBLISH_US");
    if (slow && *slow)
        std::this_thread::sleep_for(std::chrono::microseconds(std::atoi(slow)));
    if (g_destroyed.load()) record("publish-ended-after-destroy", id, len);
    g_inflight.fetch_sub(1);
    // Concurrent publishers are summarised by the checker; only the
    // single-threaded modes need one line per publication.
    if (!std::getenv("WT_R20_MQ_QUIET_PUBLISH")) record("publish", id, len);
}
#endif
