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
