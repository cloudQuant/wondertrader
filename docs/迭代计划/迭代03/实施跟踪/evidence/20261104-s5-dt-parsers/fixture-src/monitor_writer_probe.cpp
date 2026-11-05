// StateMonitor writer-callback lifecycle modes (BUG-R22-002 self-join /
// exception, BUG-R5-007) against the current DataManager writer host. The
// writer is the local fake module libI03DtFakeWriter (legacy, no V1 table).
// usage: probe <mode> <module_dir>; exit 0 = pass.
#include <atomic>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <string>
#include <thread>

#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSVariant.hpp"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WtDtCore/DataManager.h"
#include "WtDtCore/StateMonitor.h"
#include "WtDtCore/WtHelper.h"

struct R154StateMonitorTestAccess
{
	static void attach(StateMonitor& monitor, DataManager* manager, WTSSessionInfo* session, SimpleState state, const char* id)
	{
		monitor._dt_mgr = manager;
		StatePtr info(new StateInfo);
		std::strcpy(info->_session, id);
		info->_sInfo = session;
		info->_state = state;
		info->_proc_time = 0;
		info->_close_time = 2400;
		info->_sections.push_back(StateInfo::Section{0, 2400});
		monitor._map[id] = info;
	}
	static bool joinInProgress(StateMonitor& monitor)
	{
		if (!monitor._stopped.load()) return false;
		if (!monitor._thread_mutex.try_lock()) return true;
		monitor._thread_mutex.unlock();
		return false;
	}
};

namespace
{
struct Ctx
{
	StateMonitor* monitor = nullptr;
	std::string mode;
	std::atomic<bool> entered{false}, proceed{false}, called{false}, sawProcessing{false};
};

void hook(void* p)
{
	Ctx* c = static_cast<Ctx*>(p);
	c->sawProcessing = c->monitor->isInState("TEST", SS_PROCING);
	c->entered = true;
	if (c->mode == "exception")
		throw std::runtime_error("injected writer exception");
	if (c->mode == "self-external-join")
		while (!c->proceed.load()) std::this_thread::yield();
	c->monitor->stop();		// stop from the monitor's own thread: must not self-join
	c->called = true;
}

bool waitFor(const std::atomic<bool>& flag, int ms = 3000)
{
	for (int i = 0; i < ms && !flag.load(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	return flag.load();
}
}

int main(int argc, char** argv)
{
	if (argc != 3) return 2;
	const std::string mode(argv[1]);
	if (mode != "self" && mode != "self-external-join" && mode != "exception") return 2;
	WtHelper::set_module_dir((std::string(argv[2]) + "/").c_str());
	void* writerModule = dlopen((std::string(argv[2]) + "/libI03DtFakeWriter.dylib").c_str(), RTLD_NOW);
	if (writerModule == nullptr) return 3;
	auto setHook = reinterpret_cast<void (*)(void (*)(void*), void*)>(dlsym(writerModule, "fake_writer_set_hook"));
	auto transCalls = reinterpret_cast<int (*)()>(dlsym(writerModule, "fake_writer_trans_calls"));
	if (!setHook || !transCalls) return 3;

	WTSBaseDataMgr bdMgr;
	StateMonitor monitor;
	DataManager manager;
	WTSVariant* params = WTSVariant::createObject();
	params->append("module", "I03DtFakeWriter");
	if (!manager.init(params, &bdMgr, &monitor)) return 4;
	params->release();

	Ctx ctx;
	ctx.monitor = &monitor;
	ctx.mode = mode;
	setHook(&hook, &ctx);
	WTSSessionInfo* session = WTSSessionInfo::create("TEST", "test");
	session->addTradingSection(0, 2400);
	R154StateMonitorTestAccess::attach(monitor, &manager, session, SS_CLOSED, "TEST");
	monitor.run();

	int rc = 0;
	if (mode == "exception")
	{
		for (int i = 0; i < 3000 && !monitor.hasFailed(); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if (!monitor.hasFailed()) rc = 6;
		monitor.stop();
		SimpleState recorded = SS_ORIGINAL;
		if (rc == 0 && (!monitor.tryGetRecordedState("TEST", recorded) || recorded != SS_PROCING ||
			monitor.isInState("TEST", SS_PROCING) || monitor.isAnyInState(SS_PROCED))) rc = 7;
		monitor.run(); monitor.stop();
		if (rc == 0 && transCalls() != 1) rc = 8;
	}
	else
	{
		if (!waitFor(ctx.entered)) rc = 4;
		if (rc == 0 && mode == "self-external-join")
		{
			std::thread joiner([&] { monitor.stop(); });
			bool inJoin = false;
			for (int i = 0; i < 3000 && !(inJoin = R154StateMonitorTestAccess::joinInProgress(monitor)); ++i)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			ctx.proceed = true;
			joiner.join();
			if (!inJoin) rc = 9;
		}
		if (rc == 0 && (!waitFor(ctx.called) || !ctx.sawProcessing.load())) rc = 3;
		monitor.stop();
	}
	setHook(nullptr, nullptr);
	manager.release();
	session->release();
	return rc;
}
