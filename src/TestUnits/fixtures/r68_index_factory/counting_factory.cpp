// Dynamic fixture for BUG-R68-003. Records load, factory, worker and callback
// events to a test-owned ordinary file; no production plugin ABI is changed.
#include "Includes/IIndexDefs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
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
    explicit Worker(const char* id) : IIndexWorker(id) { event("worker-created"); }
    const char* get_fact_name() override { return "R68Counter"; }
    bool init(wtp::WTSVariant*) override { event("worker-initialized"); return true; }
    void handle_quote(wtp::WTSTickData*) override { event("worker-quote"); }
};
class Factory final : public wtp::IIndexWorkerFact {
public:
    const char* get_name() override { return "R68Counter"; }
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
extern "C" __attribute__((visibility("default"))) wtp::IIndexWorkerFact* create_index_factory()
{
    event("factory-created");
    return new Factory();
}
