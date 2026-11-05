#include "../WtDtCore/ParserAdapter.h"
#include "../WtDtCore/DataManager.h"
#include "../WtDtCore/StateMonitor.h"
#include "../WtDtCore/UDPCaster.h"
#include "../WtDtCore/ShmCaster.h"
#include "../WtDtCore/WtHelper.h"
#include "../WtDtCore/IndexWorkerMgr.h"

#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../WTSTools/WTSHotMgr.h"
#include "../WTSTools/WTSBaseDataMgr.h"
#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../Share/StrUtil.hpp"
#include <Share/cppcli.hpp>

#include <cstdlib>
#include <vector>

#include "../WTSUtils/SignalHook.hpp"

WTSBaseDataMgr	g_baseDataMgr;
WTSHotMgr		g_hotMgr;
StateMonitor	g_stateMon;
UDPCaster		g_udpCaster;
ShmCaster		g_shmCaster;
DataManager		g_dataMgr;
ParserAdapterMgr g_parsers;
IndexWorkerMgr	g_idxFactory;
// Active parsers whose init failed but which still own a provider API (rollback
// pending or uncertain). They are never registered or started; they stay here
// until a later shutdown drains them, or until process exit (card 9a).
std::vector<ParserAdapterPtr> g_failedParsers;

namespace
{
// Constructed after the borrowed dependencies so its destructor joins the
// monitor before global DataManager/base-data destruction starts.
struct StateMonitorShutdown
{
	~StateMonitorShutdown() { g_stateMon.stop(); }
} g_stateMonitorShutdown;
}

bool shutdownQuoteFactory();

#ifdef _MSC_VER
#include "../Common/mdump.h"
DWORD g_dwMainThreadId = 0;
BOOL WINAPI ConsoleCtrlhandler(DWORD dwCtrlType)
{
	switch (dwCtrlType)
	{
	case CTRL_CLOSE_EVENT:
	{
		// Parsers and the monitor can call the writer; stop them before releasing it.
		(void)shutdownQuoteFactory();

		PostThreadMessage(g_dwMainThreadId, WM_QUIT, 0, 0);
	}
	break;
	}

	return TRUE;
}
#endif

const char* getBinDir()
{
	static std::string basePath;
	if (basePath.empty())
	{
		basePath = boost::filesystem::initial_path<boost::filesystem::path>().string();

		basePath = StrUtil::standardisePath(basePath);
	}

	return basePath.c_str();
}


void initDataMgr(WTSVariant* config, bool bAlldayMode = false)
{
	//如果是全天模式，则不传递状态机给DataManager
	g_dataMgr.init(config, &g_baseDataMgr, bAlldayMode ? NULL : &g_stateMon);
}

void initParsers(WTSVariant* cfg)
{
	uint32_t loaded = 0;
	uint32_t failed = 0;
	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		// By Wesley @ 2021.12.14
		// 如果id为空，则生成自动id
		std::string realid = id;
		if (realid.empty())
		{
			static uint32_t auto_parserid = 1000;
			realid = fmtutil::format<64>("auto_parser_{}", auto_parserid++);
		}

		ParserAdapterPtr adapter(new ParserAdapter(&g_baseDataMgr, &g_dataMgr, &g_idxFactory));
		if (!adapter->init(realid.c_str(), cfgItem))
		{
			// BUG-R104-002: a failed active parser is neither registered, counted
			// as loaded, nor connected. Keep it only while it still owns an API.
			++failed;
			WTSLogger::error("Active parser {} failed to initialize; it is not registered or started", realid.c_str());
			if (adapter->hasApi())
				g_failedParsers.push_back(adapter);
			continue;
		}
		if (!g_parsers.addAdapter(realid.c_str(), adapter))
		{
			++failed;
			// Initialized but not registrable (duplicate id): stop it again.
			adapter->release();
			if (adapter->hasApi())
				g_failedParsers.push_back(adapter);
			continue;
		}
		++loaded;
	}

	WTSLogger::info("{} market data parsers loaded in total, {} active parsers failed", loaded, failed);
}

// Card 9 / R68-003 teardown order: state monitor (borrows the data manager),
// every parser (stop all, then drain), index workers (parser callbacks feed
// them), and only then the data manager. Returns false when a parser owner is
// not proven drained; the caller must then keep every global alive.
bool shutdownQuoteFactory()
{
	g_stateMon.stop();

	bool stopped = g_parsers.requestStopLifecycleAll();
	for (const ParserAdapterPtr& parser : g_failedParsers)
	{
		if (parser && !parser->requestStopLifecycle())
			stopped = false;
	}

	bool released = g_parsers.finishReleaseLifecycleAll(2000);
	for (auto it = g_failedParsers.begin(); it != g_failedParsers.end();)
	{
		if (!*it || ((*it)->finishReleaseLifecycle(2000) && !(*it)->hasApi()))
			it = g_failedParsers.erase(it);
		else
		{
			released = false;
			++it;
		}
	}

	if (!stopped || !released || g_parsers.size() != 0 || !g_failedParsers.empty())
	{
		WTSLogger::error("QuoteFactory parser shutdown is incomplete; parser owners and their managers are retained until process exit");
		return false;
	}

	g_idxFactory.release();
	g_dataMgr.release();
	return true;
}

bool initialize(const std::string& filename)
{
	WtHelper::set_module_dir(getBinDir());

	WTSVariant* config = WTSCfgLoader::load_from_file(filename.c_str());
	if(config == NULL)
	{
		WTSLogger::error("Loading config file {} failed", filename);
		return false;
	}

	//加载市场信息
	WTSVariant* cfgBF = config->get("basefiles");
	if (cfgBF == NULL || cfgBF->type() != WTSVariant::VT_Object)
	{
		WTSLogger::error("QuoteFactory config requires basefiles to be an object");
		config->release();
		return false;
	}

	if (cfgBF->get("session"))
	{
		g_baseDataMgr.loadSessions(cfgBF->getCString("session"));
		WTSLogger::info("Trading sessions loaded");
	}

	WTSVariant* cfgItem = cfgBF->get("commodity");
	if (cfgItem)
	{
		if (cfgItem->type() == WTSVariant::VT_String)
		{
			g_baseDataMgr.loadCommodities(cfgItem->asCString());
		}
		else if (cfgItem->type() == WTSVariant::VT_Array)
		{
			for (uint32_t i = 0; i < cfgItem->size(); i++)
			{
				g_baseDataMgr.loadCommodities(cfgItem->get(i)->asCString());
			}
		}
	}

	cfgItem = cfgBF->get("contract");
	if (cfgItem)
	{
		if (cfgItem->type() == WTSVariant::VT_String)
		{
			g_baseDataMgr.loadContracts(cfgItem->asCString());
		}
		else if (cfgItem->type() == WTSVariant::VT_Array)
		{
			for (uint32_t i = 0; i < cfgItem->size(); i++)
			{
				g_baseDataMgr.loadContracts(cfgItem->get(i)->asCString());
			}
		}
	}

	if (cfgBF->get("holiday"))
	{
		g_baseDataMgr.loadHolidays(cfgBF->getCString("holiday"));
		WTSLogger::info("Holidays loaded");
	}
	if (cfgBF->get("hot"))
	{
		g_hotMgr.loadHots(cfgBF->getCString("hot"));
		WTSLogger::log_raw(LL_INFO, "Hot rules loaded");
	}

	if (cfgBF->get("second"))
	{
		g_hotMgr.loadSeconds(cfgBF->getCString("second"));
		WTSLogger::log_raw(LL_INFO, "Second rules loaded");
	}

	if (cfgBF->has("rules"))
	{
		auto cfgRules = cfgBF->get("rules");
		auto tags = cfgRules->memberNames();
		for (const std::string& ruleTag : tags)
		{
			g_hotMgr.loadCustomRules(ruleTag.c_str(), cfgRules->getCString(ruleTag.c_str()));
			WTSLogger::info("{} rules loaded from {}", ruleTag, cfgRules->getCString(ruleTag.c_str()));
		}
	}

	WTSVariant* shmCfg = config->get("shmcaster");
	if (shmCfg && shmCfg->getBoolean("active"))
	{
		if (!g_shmCaster.init(shmCfg))
		{
			WTSLogger::error("Active ShmCaster could not be initialized; QuoteFactory startup aborted");
			config->release();
			return false;
		}

		g_dataMgr.add_caster(&g_shmCaster);
	}
	else if (shmCfg)
	{
		WTSLogger::info("ShmCaster is inactive; skipping registration");
	}

	if (config->has("broadcaster"))
	{
		g_udpCaster.init(config->get("broadcaster"), &g_baseDataMgr, &g_dataMgr);
		g_dataMgr.add_caster(&g_udpCaster);
	}


	//By Wesley @ 2021.12.27
	//全天候模式，不需要再使用状态机
	bool bAlldayMode = config->getBoolean("allday");
	if (!bAlldayMode)
	{
		g_stateMon.initialize(config->getCString("statemonitor"), &g_baseDataMgr, &g_dataMgr);
	}
	else
	{
		WTSLogger::info("QuoteFactory will run in allday mode");
	}
	initDataMgr(config->get("writer"), bAlldayMode);

	if(config->has("index"))
	{
		std::string path = fmt::format("{}index/", WtHelper::get_cwd());
		g_idxFactory.loadFactories(path.c_str());

		//如果存在指数模块要，配置指数
		const char* filename = config->getCString("index");
		WTSLogger::info("Reading index config from {}...", filename);
		WTSVariant* var = WTSCfgLoader::load_from_file(filename);
		if (var)
		{
			g_idxFactory.init(var, &g_hotMgr, &g_baseDataMgr, &g_dataMgr);
			var->release();
		}
		else
		{
			WTSLogger::error("Loading index config {} failed", filename);
		}		
	}

	WTSVariant* cfgParser = config->get("parsers");
	if (cfgParser)
	{
		if (cfgParser->type() == WTSVariant::VT_String)
		{
			const char* filename = cfgParser->asCString();
			if (StdFile::exists(filename))
			{
				WTSLogger::info("Reading parser config from {}...", filename);
				WTSVariant* var = WTSCfgLoader::load_from_file(filename);
				if (var)
				{
					initParsers(var->get("parsers"));
					var->release();
				}
				else
				{
					WTSLogger::error("Loading parser config {} failed", filename);
				}
			}
			else
			{
				WTSLogger::error("Parser configuration {} not exists", filename);
			}
		}
		else if (cfgParser->type() == WTSVariant::VT_Array)
		{
			initParsers(cfgParser);
		}
	}

	config->release();

	// I03-NEW-003: report command submission instead of discarding it. A false
	// result means at least one registered parser did not submit its connect
	// command; nothing here proves any provider connected.
	if (!g_parsers.runWithResult())
		WTSLogger::error("Not every registered parser submitted its connect command; QuoteFactory continues with partial parser startup");

	//全天候模式，不启动状态机
	if(!bAlldayMode)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		g_stateMon.run();
	}

	return true;
}

int main(int argc, char* argv[])
{
	cppcli::Option opt(argc, argv);

	auto cParam = opt("-c", "--config", "configure filepath, dtcfg.yaml as default", false);
	auto lParam = opt("-l", "--logcfg", "logging configure filepath, logcfgdt.yaml as default", false);

	auto hParam = opt("-h", "--help", "gain help doc", false)->asHelpParam();

	opt.parse();

	if (hParam->exists())
		return 0;

	std::string filename;
	if (lParam->exists())
		filename = lParam->get<std::string>();
	else
		filename = "./logcfgdt.yaml";
	WTSLogger::init(filename.c_str());

#ifdef _MSC_VER
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);

	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG);

	g_dwMainThreadId = GetCurrentThreadId();
	SetConsoleCtrlHandler(ConsoleCtrlhandler, TRUE);

	CMiniDumper::Enable("QuoteFactory.exe", true);
#endif

	if (cParam->exists())
		filename = cParam->get<std::string>();
	else
		filename = "./dtcfg.yaml";

	if(!StdFile::exists(filename.c_str()))
	{
		fmt::print("Configuration file {} does not exist\n", filename);
		return 1;
	}

	if (!initialize(filename))
		return 1;

	bool bExit = false;
	install_signal_hooks([&bExit](const char* message) {
		if (!bExit)
			WTSLogger::error(message);
	}, [&bExit](bool toExit) {
		if (bExit)
			return;

		bExit = toExit;
		WTSLogger::info("Exit flag is {}", bExit);
	});

	while (!bExit)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	if (!shutdownQuoteFactory())
	{
		// Retain every owner until the process ends: skip static destruction of
		// the globals that providers may still reach through the SPI.
		WTSLogger::stop();
		std::_Exit(0);
	}
	return 0;
}

