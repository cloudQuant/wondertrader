// Dynamic fixture for BUG-R68-003. Records load, factory, worker and callback
// events to a test-owned ordinary file; no production plugin ABI is changed.
#include "Includes/IIndexDefs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#ifndef R68_FACTORY_VARIANT
#define R68_FACTORY_VARIANT "first"
#endif

namespace {
void (*reenter)(void*) = nullptr;
void* reenterContext = nullptr;
void event(const char* name)
{
    const char* path = std::getenv("WT_R68_FACTORY_EVENTS");
    if (!path)
        return;
    if (FILE* f = std::fopen(path, "a")) {
        std::fprintf(f, "%s\n", name);
        std::fclose(f);
    }
}

__attribute__((constructor)) void module_loaded() { event("module-loaded"); }
__attribute__((destructor)) void module_unloaded() { event("module-unloaded"); }

class Worker final : public wtp::IIndexWorker {
public:
    explicit Worker(const char* id) : IIndexWorker(id) { event("worker-created"); event("worker-from-" R68_FACTORY_VARIANT); }
    const char* get_fact_name() override { return "R68Counter"; }
    bool init(wtp::WTSVariant*) override { event("worker-initialized"); return true; }
    void handle_quote(wtp::WTSTickData*) override { event("worker-quote"); }
};
class Factory final : public wtp::IIndexWorkerFact {
public:
    const char* get_name() override {
        const char* mode = std::getenv("WT_R68_FACTORY_MODE");
        if (mode && std::strcmp(mode, "null-name") == 0) return nullptr;
        if (mode && std::strcmp(mode, "empty-name") == 0) return "";
        if (mode && std::strcmp(mode, "throw-name") == 0) throw std::runtime_error("fixture name");
        return "R68Counter";
    }
    wtp::IIndexWorker* create_worker(const char* name, const char* id) override
    {
        return std::strcmp(name, "Counter") == 0 ? new Worker(id) : nullptr;
    }
    bool delete_worker(wtp::IIndexWorker* worker) override
    {
        event("worker-deleted");
        delete worker;
        return true;
    }
};
}
extern "C" __attribute__((visibility("default"))) void r68_set_reentry(void (*callback)(void*), void* context)
{
    reenter = callback;
    reenterContext = context;
}
extern "C" __attribute__((visibility("default"))) wtp::IIndexWorkerFact* create_index_factory()
{
    event("factory-created");
    event("factory-from-" R68_FACTORY_VARIANT);
    const char* mode = std::getenv("WT_R68_FACTORY_MODE");
    if (mode && std::strcmp(mode, "throw-create") == 0)
        throw std::runtime_error("fixture creator");
    if (mode && std::strcmp(mode, "reenter") == 0 && reenter)
        reenter(reenterContext);
    return new Factory();
}
