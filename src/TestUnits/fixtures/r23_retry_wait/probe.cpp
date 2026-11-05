#include "../../coverage/p10_storage_writer/p10_writer_prefix.h"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace wtp;
namespace {
struct State {
    std::mutex mutex;
    std::condition_variable cv;
    int attempts = 0;
    int failures = 0;
    bool atWait = false;
    bool done = false;
} state;
class Sink : public IDataWriterSink {
public:
    IBaseDataMgr* getBDMgr() override { return nullptr; }
    bool canSessionReceive(const char*) override { return true; }
    void broadcastTick(WTSTickData*) override {}
    void broadcastOrdQue(WTSOrdQueData*) override {}
    void broadcastOrdDtl(WTSOrdDtlData*) override {}
    void broadcastTrans(WTSTransData*) override {}
    CodeSet* getSessionComms(const char*) override { return nullptr; }
    uint32_t getTradingDate(const char*) override { return 0; }
    void outputLog(WTSLogLevel, const char*) override {}
};
template<class Predicate> bool waitFor(Predicate p, int seconds) {
    std::unique_lock<std::mutex> lock(state.mutex);
    return state.cv.wait_for(lock, std::chrono::seconds(seconds), p);
}
}
extern "C" void r23_before_remove() {
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.attempts;
    state.cv.notify_all();
    if (state.attempts <= state.failures) throw std::runtime_error("injected remove failure");
}
extern "C" void r23_before_wait() {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.atWait = true;
    state.cv.notify_all();
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string mode = argv[1];
    const std::filesystem::path dir = argv[2];
    if (std::filesystem::exists(dir)) return 2;
    std::filesystem::create_directories(dir);
    Sink sink;
    WtDataWriter writer;
    writer._base_dir = dir.string() + "/";
    writer.IDataWriter::init(nullptr, &sink);
    writer._bd_mgr = nullptr;
    writer._async_proc = false;
    {
        StdUniqueLock lock(writer._accept_mtx);
        writer._lifecycle_state = WtDataWriter::LifecycleState::Ready;
        writer._accepting = true;
    }
    if (mode == "shutdown") state.failures = 5;
    else if (mode == "normal-retry") state.failures = 1;
    else if (mode != "normal") return 2;
    const auto start = std::chrono::steady_clock::now();
    writer.transHisData("CMD_CLEAR_CACHE");
    if (mode == "shutdown") {
        if (!waitFor([] { return state.atWait; }, 5)) std::_Exit(3);
        writer.transHisData("CMD_CLEAR_CACHE");
        {
            StdUniqueLock lock(writer._proc_mtx);
            if (writer._proc_que.size() != 1 || writer._proc_stopping) std::_Exit(4);
        }
        // A queue wake with no stopping predicate must not trigger an early retry.
        writer._proc_cond.notify_all();
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            if (state.cv.wait_for(lock, std::chrono::milliseconds(200), [] { return state.attempts > 1; }))
                std::_Exit(10);
        }
        std::thread closer([&] {
            writer.release();
            std::lock_guard<std::mutex> lock(state.mutex);
            state.done = true;
            state.cv.notify_all();
        });
        if (!waitFor([] { return state.done; }, 5)) std::_Exit(5);
        closer.join();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.attempts != 6) return 6;
    } else if (mode == "normal-retry") {
        if (!waitFor([] { return state.attempts >= 2; }, 315)) std::_Exit(7);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed < 300000) std::_Exit(8);
        writer.release();
        std::cout << "retry elapsed_ms=" << elapsed << '\n';
    } else {
        if (!waitFor([] { return state.attempts >= 1; }, 5)) std::_Exit(9);
        writer.release();
    }
    std::cout << mode << " PASS attempts=" << state.attempts << '\n';
    return 0;
}
