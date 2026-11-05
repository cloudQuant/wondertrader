#include <Includes/IDataWriter.h>

#include <cstdlib>
#include <fstream>

namespace {

void trace_event(const char* event)
{
	const char* path = std::getenv("WT_R18_WRITER_TRACE");
	if (path == nullptr || *path == '\0')
		return;

	std::ofstream trace(path, std::ios::out | std::ios::app);
	trace << event << '\n';
}

struct ModuleLoadTrace
{
	ModuleLoadTrace() { trace_event("module-load"); }
};

ModuleLoadTrace moduleLoadTrace;

class FakeWriter final : public wtp::IDataWriter
{
public:
	~FakeWriter() override { trace_event("destroy"); }

	bool init(wtp::WTSVariant* params, wtp::IDataWriterSink* sink) override
	{
		wtp::IDataWriter::init(params, sink);
#if defined(WT_R18_WRITER_INIT_FALSE)
		trace_event("init-false");
		return false;
#else
		trace_event("init-true");
		return true;
#endif
	}

	void release() override { trace_event("release"); }
	bool writeTick(wtp::WTSTickData*, uint32_t) override { return true; }
	wtp::WTSTickData* getCurTick(const char*, const char*) override { return nullptr; }
};

} // namespace

#if defined(WT_R18_WRITER_OMIT_CREATE)
extern "C" EXPORT_FLAG int r18_writer_missing_create_control()
{
	return 0;
}
#else
extern "C" EXPORT_FLAG wtp::IDataWriter* createWriter()
{
	trace_event("factory");
#if defined(WT_R18_WRITER_NULL_FACTORY)
	return nullptr;
#else
	return new FakeWriter();
#endif
}
#endif

#if !defined(WT_R18_WRITER_OMIT_DELETE)
extern "C" EXPORT_FLAG void deleteWriter(wtp::IDataWriter*& writer)
{
	trace_event("delete");
	delete writer;
	writer = nullptr;
}
#endif

#if !defined(WT_R18_WRITER_LEGACY)
// Iteration 03 card 9a: the fake owns no asynchronous work and never calls its sink
// outside a host call, so it can truthfully advertise writer lifecycle V1 and keep
// the original release/delete/destroy oracle. WT_R18_WRITER_LEGACY builds the
// legacy (quarantined) variant.
namespace {
WtDataWriterLifecycleV1Status fakeRequestStop(void* writer) noexcept
{
	return writer != nullptr ? WT_DATA_WRITER_LIFECYCLE_V1_OK : WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT;
}
WtDataWriterLifecycleV1Status fakeCanWait(void* writer) noexcept
{
	return writer != nullptr ? WT_DATA_WRITER_LIFECYCLE_V1_OK : WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT;
}
WtDataWriterLifecycleV1Status fakeWaitDrained(void* writer, uint32_t) noexcept
{
	return writer != nullptr ? WT_DATA_WRITER_LIFECYCLE_V1_DRAINED : WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT;
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
