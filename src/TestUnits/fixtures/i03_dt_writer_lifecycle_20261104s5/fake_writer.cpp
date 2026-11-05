// Fake data-writer modules for the dt-writer lifecycle fixture (iteration 03, card 9).
//
// Variants (compile definitions):
//   S5_FAKE_V1     export writer lifecycle V1 (getDataWriterLifecycleApiV1)
//   S5_FAKE_ASYNC  own a worker thread that keeps calling the sink until stopped
//
// Every event is appended to the file named by S5_FAKE_TRACE. writeTick deliberately
// touches the writer object after its sink callback returns ("write-exit"), so a host
// that destroys the writer during the callback is visible as destroy-before-write-exit
// (and as a sanitizer report). The destructor poisons a magic value that write-exit checks.
#include <Includes/IDataWriter.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

namespace {

std::mutex g_trace_mtx;

void trace(const char* event)
{
	const char* path = std::getenv("S5_FAKE_TRACE");
	if (path == nullptr || *path == '\0')
		return;
	std::lock_guard<std::mutex> lock(g_trace_mtx);
	std::ofstream out(path, std::ios::out | std::ios::app);
	out << event << '\n';
}

const uint64_t kAlive = 0xA11CEA11CEull;

class FakeWriter final : public wtp::IDataWriter
{
public:
	~FakeWriter() override
	{
		trace("destroy");
		stopWorker();
		_magic = 0;
	}

	bool init(wtp::WTSVariant* params, wtp::IDataWriterSink* sink) override
	{
		wtp::IDataWriter::init(params, sink);
		trace("init");
#if defined(S5_FAKE_ASYNC)
		_worker = std::thread([this]() {
			_worker_id.store(std::this_thread::get_id());
			while (!_stop.load())
			{
				_sink->broadcastTick(nullptr);	// sink callback from a writer-owned thread
				_callbacks.fetch_add(1);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			trace("worker-exit");
		});
#endif
		return true;
	}

	void release() override
	{
		trace("release");
		_stop.store(true);
	}

	bool writeTick(wtp::WTSTickData* tick, uint32_t) override
	{
		trace("write-enter");
		_sink->broadcastTick(tick);
		// Touch the object after the callback: must still be alive.
		_writes.fetch_add(1);
		trace(_magic == kAlive ? "write-exit" : "write-exit-dead");
		return true;
	}

	wtp::WTSTickData* getCurTick(const char*, const char*) override { return nullptr; }

	void requestStop()
	{
		trace("request-stop");
		_stop.store(true);
	}

	bool onWorker() const { return _worker_id.load() == std::this_thread::get_id(); }

	bool join()
	{
		if (onWorker())
			return false;
		stopWorker();
		return true;
	}

private:
	void stopWorker()
	{
		_stop.store(true);
		if (_worker.joinable())
			_worker.join();
	}

	std::atomic<bool> _stop{ false };
	std::atomic<uint32_t> _writes{ 0 };
	std::atomic<uint32_t> _callbacks{ 0 };
	std::atomic<std::thread::id> _worker_id{ std::thread::id() };
	std::thread _worker;
	volatile uint64_t _magic = kAlive;
};

std::mutex g_live_mtx;
std::set<wtp::IDataWriter*> g_live;

#if defined(S5_FAKE_V1)
FakeWriter* live(void* handle)
{
	std::lock_guard<std::mutex> lock(g_live_mtx);
	auto it = g_live.find(static_cast<wtp::IDataWriter*>(handle));
	return it == g_live.end() ? nullptr : static_cast<FakeWriter*>(*it);
}
#endif

} // namespace

extern "C" EXPORT_FLAG wtp::IDataWriter* createWriter()
{
	trace("factory");
	FakeWriter* writer = new FakeWriter();
	std::lock_guard<std::mutex> lock(g_live_mtx);
	g_live.insert(writer);
	return writer;
}

extern "C" EXPORT_FLAG void deleteWriter(wtp::IDataWriter*& writer)
{
	trace("delete");
	{
		std::lock_guard<std::mutex> lock(g_live_mtx);
		g_live.erase(writer);
	}
	delete writer;
	writer = nullptr;
}

#if defined(S5_FAKE_V1)
namespace {
WtDataWriterLifecycleV1Status fakeRequestStop(void* handle) noexcept
{
	FakeWriter* writer = live(handle);
	if (writer == nullptr)
		return WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT;
	writer->requestStop();
	return WT_DATA_WRITER_LIFECYCLE_V1_OK;
}

WtDataWriterLifecycleV1Status fakeCanWait(void* handle) noexcept
{
	FakeWriter* writer = live(handle);
	if (writer == nullptr)
		return WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT;
	return writer->onWorker() ? WT_DATA_WRITER_LIFECYCLE_V1_WOULD_DEADLOCK : WT_DATA_WRITER_LIFECYCLE_V1_OK;
}

WtDataWriterLifecycleV1Status fakeWaitDrained(void* handle, uint32_t) noexcept
{
	FakeWriter* writer = live(handle);
	if (writer == nullptr)
		return WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT;
	if (!writer->join())
		return WT_DATA_WRITER_LIFECYCLE_V1_WOULD_DEADLOCK;
	trace("drained");
	return WT_DATA_WRITER_LIFECYCLE_V1_DRAINED;
}

const WtDataWriterLifecycleApiV1 fakeLifecycle = {
	(uint32_t)sizeof(WtDataWriterLifecycleApiV1), WT_DATA_WRITER_LIFECYCLE_V1_ABI_VERSION,
	WT_DATA_WRITER_LIFECYCLE_V1_CAP_DRAIN, fakeRequestStop, fakeCanWait, fakeWaitDrained,
	NULL, NULL	// K20 minor 1 not offered: no CAP_DUMPERS (host uses the legacy registration)
};
} // namespace

extern "C" EXPORT_FLAG const WtDataWriterLifecycleApiV1* getDataWriterLifecycleApiV1(uint32_t major)
{
	return major == WT_DATA_WRITER_LIFECYCLE_V1_ABI_MAJOR ? &fakeLifecycle : nullptr;
}
#endif
