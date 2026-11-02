// Compile with OMIT_CREATE, OMIT_DESTROY, OMIT_REGISTER, OMIT_PUBLISH or
// FAIL_CREATE to exercise each ABI edge without starting a real MQ server.
#include "WtMsgQue/WtMsgQue.h"
#include <cstdlib>
#include <fstream>

namespace {
void record(const char* event, WtUInt32 id = 0, WtUInt32 len = 0)
{
    const char* path = std::getenv("WT_R20_MQ_LOG");
    if (path) std::ofstream(path, std::ios::app) << event << ' ' << id << ' ' << len << '\n';
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
    return 0xf1234567u;
#endif
}
#endif
#ifndef OMIT_DESTROY
extern "C" void destroy_server(WtUInt32 id) { record("destroy", id); }
#endif
#ifndef OMIT_PUBLISH
extern "C" void publish_message(WtUInt32 id, const char*, const char*, WtUInt32 len)
{
    record("publish", id, len);
}
#endif
