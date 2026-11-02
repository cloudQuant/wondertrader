#include "Dumper.h"
#include "WtHelper.h"
#include "TraderAdapter.h"

#include "../WTSTools/WTSLogger.h"
#include "../WTSTools/WTSBaseDataMgr.h"
#include "../WTSUtils/WTSCfgLoader.h"

#include "../Includes/WTSVariant.hpp"

#include <limits>
#ifdef WT_DUMPER_TEST_HOOKS
#include <atomic>
#endif

USING_NS_WTP;

namespace
{
#ifdef WT_DUMPER_TEST_HOOKS
using DumperWorkerWaitHook = void (*)();
std::atomic<DumperWorkerWaitHook> g_dumperWorkerWaitHookForTest{nullptr};
std::atomic<bool> g_dumperUseLegacySleepForTest{false};

void notify_dumper_worker_wait_for_test()
{
	DumperWorkerWaitHook hook = g_dumperWorkerWaitHookForTest.load();
	if (hook != nullptr)
		hook();
}
#endif

bool parse_refresh_span(WTSVariant* value, uint32_t& refreshSpan)
{
	if (value == NULL)
		return false;

	uint64_t parsed = 0;
	switch (value->type())
	{
	case WTSVariant::VT_Int32:
	case WTSVariant::VT_Int64:
	{
		const int64_t signedValue = value->asInt64();
		if (signedValue <= 0)
			return false;
		parsed = static_cast<uint64_t>(signedValue);
		break;
	}
	case WTSVariant::VT_Uint32:
	case WTSVariant::VT_Uint64:
		parsed = value->asUInt64();
		break;
	case WTSVariant::VT_String:
	{
		const std::string text = value->asString();
		if (text.empty())
			return false;
		for (char digit : text)
		{
			if (digit < '0' || digit > '9')
				return false;
			const uint64_t next = parsed * 10 + static_cast<uint64_t>(digit - '0');
			if (next > std::numeric_limits<uint32_t>::max())
				return false;
			parsed = next;
		}
		break;
	}
	default:
		return false;
	}

	if (parsed == 0 || parsed > std::numeric_limits<uint32_t>::max())
		return false;

	refreshSpan = static_cast<uint32_t>(parsed);
	return true;
}
}

#ifdef WT_DUMPER_TEST_HOOKS
void setDumperWorkerWaitHookForTest(DumperWorkerWaitHook hook)
{
	g_dumperWorkerWaitHookForTest.store(hook);
}

void setDumperUseLegacySleepForTest(bool useLegacySleep)
{
	g_dumperUseLegacySleepForTest.store(useLegacySleep);
}
#endif

WTSBaseDataMgr		g_bdMgr;
TraderAdapterMgr	g_adapterMgr;

void Dumper::init(const char* logProfile)
{
	WTSLogger::init(logProfile);
}

bool Dumper::config(const char* cfgfile, bool isFile, const char* modDir)
{
	_configured = false;
	if (cfgfile == NULL || modDir == NULL)
	{
		WTSLogger::error("交易数据落地模块配置路径或模块目录为空");
		return false;
	}

	WtHelper::set_module_dir(modDir);

	WTSVariant* root = NULL;
	if (isFile)
		root = WTSCfgLoader::load_from_file(cfgfile);
	else
		root = WTSCfgLoader::load_from_content(cfgfile, false);

	if (root == NULL || root->type() != WTSVariant::VT_Object)
	{
		if (root)
			root->release();
		WTSLogger::error("交易数据落地模块配置解析失败或根节点不是对象");
		return false;
	}

	// basefiles and traders are required before applying configuration side effects.
	WTSVariant* cfgBF = root->get("basefiles");
	if (cfgBF == NULL || cfgBF->type() != WTSVariant::VT_Object)
	{
		WTSLogger::error("交易数据落地模块配置缺少对象 basefiles");
		root->release();
		return false;
	}

	WTSVariant* traders = root->get("traders");
	if (traders == NULL || traders->type() != WTSVariant::VT_Array)
	{
		WTSLogger::error("交易数据落地模块配置缺少数组 traders");
		root->release();
		return false;
	}

	WTSVariant* cfg = root->get("config");
	if (cfg && cfg->type() != WTSVariant::VT_Object)
	{
		WTSLogger::error("交易数据落地模块配置项必须是对象");
		root->release();
		return false;
	}

	uint32_t refreshSpan = _refresh_span;
	if (cfg && cfg->has("refresh_span") &&
		!parse_refresh_span(cfg->get("refresh_span"), refreshSpan))
	{
		WTSLogger::error("config.refresh_span 必须是 1 到 {} 之间的正整数", std::numeric_limits<uint32_t>::max());
		root->release();
		return false;
	}

	// Validate all deterministic trader inputs before loading files or plugins.
	for (uint32_t idx = 0; idx < traders->size(); idx++)
	{
		WTSVariant* cfgItem = traders->get(idx);
		if (cfgItem == NULL || cfgItem->type() != WTSVariant::VT_Object)
		{
			WTSLogger::error("traders[{}] 必须是对象", idx);
			root->release();
			return false;
		}

		if (!cfgItem->getBoolean("active"))
			continue;

		WTSVariant* channel = cfgItem->get("channelid");
		WTSVariant* module = cfgItem->get("module");
		const char* channelid = cfgItem->getCString("channelid");
		if (channel == NULL || channel->type() != WTSVariant::VT_String || channelid[0] == '\0' ||
			module == NULL || module->type() != WTSVariant::VT_String || module->asString().empty())
		{
			WTSLogger::error("traders[{}] 缺少有效的 channelid 或 module", idx);
			root->release();
			return false;
		}

		if (g_adapterMgr.getAdapter(channelid))
		{
			WTSLogger::error("traders[{}] 的 channelid 已经登记: {}", idx, channelid);
			root->release();
			return false;
		}

		for (uint32_t prior = 0; prior < idx; prior++)
		{
			WTSVariant* priorItem = traders->get(prior);
			if (priorItem->getBoolean("active") &&
				strcmp(priorItem->getCString("channelid"), channelid) == 0)
			{
				WTSLogger::error("traders[{}] 的 channelid 重复: {}", idx, channelid);
				root->release();
				return false;
			}
		}
	}

	//基础数据文件
	if (cfgBF->get("session"))
	{
		g_bdMgr.loadSessions(cfgBF->getCString("session"));
		WTSLogger::info("Trading sessions loaded");
	}

	WTSVariant* cfgItem = cfgBF->get("commodity");
	if (cfgItem)
	{
		if (cfgItem->type() == WTSVariant::VT_String)
		{
			g_bdMgr.loadCommodities(cfgItem->asCString());
		}
		else if (cfgItem->type() == WTSVariant::VT_Array)
		{
			for (uint32_t i = 0; i < cfgItem->size(); i++)
			{
				g_bdMgr.loadCommodities(cfgItem->get(i)->asCString());
			}
		}
	}

	cfgItem = cfgBF->get("contract");
	if (cfgItem)
	{
		if (cfgItem->type() == WTSVariant::VT_String)
		{
			g_bdMgr.loadContracts(cfgItem->asCString());
		}
		else if (cfgItem->type() == WTSVariant::VT_Array)
		{
			for (uint32_t i = 0; i < cfgItem->size(); i++)
			{
				g_bdMgr.loadContracts(cfgItem->get(i)->asCString());
			}
		}
	}

	for (uint32_t idx = 0; idx < traders->size(); idx++)
	{
		WTSVariant* cfgItem = traders->get(idx);
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* channelid = cfgItem->getCString("channelid");

		TraderAdapterPtr adapter(new TraderAdapter(&g_adapterMgr));
		if (!adapter->init(channelid, cfgItem, &g_bdMgr))
		{
			WTSLogger::error("[{}] 交易适配器初始化失败，不登记该通道", channelid);
			// ITraderApi has no general quiescence contract; earlier initialized
			// adapters cannot be safely rolled back. _configured stays false so
			// run() will not start this partial manager state.
			root->release();
			return false;
		}

		if (!g_adapterMgr.addAdapter(channelid, adapter))
		{
			WTSLogger::error("[{}] 交易适配器登记失败", channelid);
			root->release();
			return false;
		}
	}

	root->release();
	_refresh_span = refreshSpan;
	_configured = true;

	WTSLogger::info("交易数据落地模块初始化完成，主动刷新间隔:{}s", _refresh_span);

	return true;
}

void Dumper::run(bool bOnce /* = true */)
{
	if (!_configured)
	{
		WTSLogger::error("交易数据落地模块配置未成功，拒绝启动");
		return;
	}

	g_adapterMgr.run();

	if(bOnce)
	{
		for (;;)
		{
			if (g_adapterMgr.isAnyAlive())
				std::this_thread::sleep_for(std::chrono::seconds(1));
			else
				break;
		}
	}
	else
	{
		std::lock_guard<std::mutex> workerGuard(_worker_mutex);
		_stopped = false;
		const uint32_t refreshSpan = _refresh_span;
		_worker.reset(new StdThread([this, refreshSpan]() {
			std::unique_lock<std::mutex> workerLock(_worker_mutex);
			while (!_stopped)
			{
#ifdef WT_DUMPER_TEST_HOOKS
				if (g_dumperUseLegacySleepForTest.load())
				{
					notify_dumper_worker_wait_for_test();
					workerLock.unlock();
					std::this_thread::sleep_for(std::chrono::seconds(refreshSpan));
					workerLock.lock();
				}
				else
#endif
				{
#ifdef WT_DUMPER_TEST_HOOKS
					notify_dumper_worker_wait_for_test();
					// release() uses this mutex, so it cannot set the stop predicate
					// until wait_for atomically releases the mutex and enters the wait.
#endif
					if (_worker_cv.wait_for(workerLock, std::chrono::seconds(refreshSpan), [this]() {
						return _stopped;
					}))
						break;
				}
				workerLock.unlock();
				g_adapterMgr.refresh();
				workerLock.lock();
			}
		}));
	}
}

void Dumper::release()
{
	_configured = false;
	{
		std::lock_guard<std::mutex> workerGuard(_worker_mutex);
		_stopped = true;
	}
	_worker_cv.notify_all();
	if (_worker)
		_worker->join();
}

void Dumper::on_account(const char* channelid, uint32_t curTDate, const char* currency, double prebalance, 
		double balance, double dynbalance, double closeprofit, double dynprofit, double fee, 
		double margin, double deposit, double withdraw, double available, bool isLast)
{
	if (_cb_account)
		_cb_account(channelid, curTDate, currency, prebalance, balance, dynbalance, closeprofit, dynprofit, fee, margin, deposit, withdraw, available, isLast);
}

void Dumper::on_position(const char* channelid, const char* exchg, const char* code, uint32_t curTDate, uint32_t direct,
		double volume, double newvol, double cost, double margin, double avgpx, double dynprofit, uint32_t volscale, bool isLast)
{
	if (_cb_position)
		_cb_position(channelid, exchg, code, curTDate, direct, volume, newvol, cost, margin, avgpx, dynprofit, volscale, isLast);
}

void Dumper::on_trade(const char* channelid, const char* exchg, const char* code, uint32_t curTDate, const char* tradeid, const char* orderid, 
		uint32_t direct, uint32_t offset, double volume, double price, double amount, uint32_t ordertype, uint32_t tradetype, WtUInt64 tradetime, bool isLast)
{
	if (_cb_trade)
		_cb_trade(channelid, exchg, code, curTDate, tradeid, orderid, direct, offset, volume, price, amount, ordertype, tradetype, tradetime, isLast);
}

void Dumper::on_order(const char* channelid, const char* exchg, const char* code, uint32_t curTDate, const char* orderid, uint32_t direct, uint32_t offset, double volume, double leftover, double traded, double price, uint32_t ordertype, uint32_t pricetype, WtUInt64 ordertime, uint32_t state, const char* statemsg, bool isLast)
{
	if (_cb_order)
		_cb_order(channelid, exchg, code, curTDate, orderid, direct, offset, volume, leftover, traded, price, ordertype, pricetype, ordertime, state, statemsg, isLast);
}
