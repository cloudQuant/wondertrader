// Legacy (no lifecycle V1) fake data writer module for the StateMonitor
// writer-callback modes of BUG-R22-002 / R5-007. Local only.
#include "Includes/IDataWriter.h"
#include <atomic>

namespace
{
void (*g_trans_hook)(void*) = nullptr;
void* g_trans_ctx = nullptr;
std::atomic<int> g_trans_calls{0};
std::atomic<int> g_release_dumps{0};

class FakeWriter final : public wtp::IDataWriter
{
public:
	// A writer may flush through its registered external dumpers while it is
	// released (I03-NEW-047): call each one, so a freed dumper is a use-after-free.
	void release() override
	{
		for (auto& entry : _dumpers)
		{
			if (entry.second != nullptr)
			{
				entry.second->dumpHisBars("CFFEX.IC.2204", "m1", nullptr, 0);
				++g_release_dumps;
			}
		}
	}
	bool writeTick(wtp::WTSTickData*, uint32_t) override { return false; }
	wtp::WTSTickData* getCurTick(const char*, const char*) override { return nullptr; }
	bool isSessionProceeded(const char*) override { return false; }
	void transHisData(const char*) override
	{
		++g_trans_calls;
		if (g_trans_hook != nullptr)
			g_trans_hook(g_trans_ctx);
	}
};
}

extern "C" __attribute__((visibility("default"))) wtp::IDataWriter* createWriter() { return new FakeWriter(); }
extern "C" __attribute__((visibility("default"))) void deleteWriter(wtp::IDataWriter*& writer) { delete writer; writer = nullptr; }
extern "C" __attribute__((visibility("default"))) void fake_writer_set_hook(void (*hook)(void*), void* ctx) { g_trans_ctx = ctx; g_trans_hook = hook; }
extern "C" __attribute__((visibility("default"))) int fake_writer_trans_calls() { return g_trans_calls.load(); }
extern "C" __attribute__((visibility("default"))) int fake_writer_release_dumps() { return g_release_dumps.load(); }
