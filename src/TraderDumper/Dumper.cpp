#include "Dumper.h"
#include "WtHelper.h"
#include "TraderAdapter.h"

#include "../WTSTools/WTSLogger.h"
#include "../WTSTools/WTSBaseDataMgr.h"
#include "../WTSUtils/WTSCfgLoader.h"

#include "../Includes/WTSVariant.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
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

void Dumper::register_callbacks(FuncOnAccount cbAccount, FuncOnOrder cbOrder, FuncOnTrade cbTrade, FuncOnPosition cbPosition)
{
	std::lock_guard<std::mutex> releaseGuard(_release_mutex);
	_cb_account = cbAccount;
	_cb_order = cbOrder;
	_cb_position = cbPosition;
	_cb_trade = cbTrade;
	const uint32_t state = _state.load(std::memory_order_acquire);
	if (state == DS_RELEASED && _callbacks.reopen())
	{
		{
			std::lock_guard<std::mutex> workerGuard(_worker_mutex);
			_stopped = false;
		}
		_state.store(DS_IDLE, std::memory_order_release);
	}
	else if (state == DS_STOP_DEFERRED || state == DS_DRAIN_UNCERTAIN)
	{
		WTSLogger::warn("交易数据落地模块停止尚未完成，新回调已登记但不会被调用");
	}
}

Dumper::~Dumper()
{
	if (wt_vendor_td::Lifecycle::reentered(this))
	{
		// Destroying the dumper from its own worker or callback would leave that
		// frame running on freed memory (card 9d).
		std::fprintf(stderr, "[TraderDumper] Destroying the dumper from its own worker or callback is unsupported\n");
		std::abort();
	}
	release();
	// A local dumper never leaves a joinable worker behind (std::terminate);
	// the exported singleton is never destroyed (getDumper keeps it alive).
	if (_worker && _worker->joinable())
		_worker->join();
	while (_callbacks.active() != 0 || _run_once_active.load() != 0)
		std::this_thread::sleep_for(std::chrono::microseconds(200));
}

void Dumper::run(bool bOnce /* = true */)
{
	if (!_configured)
	{
		WTSLogger::error("交易数据落地模块配置未成功，拒绝启动");
		return;
	}

	{
		// T3-012 / 卡9c：已在运行（worker 存在或 run(true) 循环中）时拒绝重复 run，
		// 绝不替换仍可 join 的线程；已请求停止后也拒绝。
		std::lock_guard<std::mutex> workerGuard(_worker_mutex);
		if (_stopped || _callbacks.closed())
		{
			WTSLogger::error("交易数据落地模块已请求停止，拒绝启动");
			return;
		}
		if (_worker || _run_once_active.load() != 0)
		{
			WTSLogger::error("交易数据落地模块已在运行，拒绝重复启动");
			return;
		}
		if (bOnce)
			_run_once_active.fetch_add(1);
		_state.store(DS_RUNNING, std::memory_order_release);
	}

	g_adapterMgr.run();

	if(bOnce)
	{
		// release() 请求停止后立即返回，不再等通道全部结束。
		std::unique_lock<std::mutex> workerLock(_worker_mutex);
		while (!_stopped && g_adapterMgr.isAnyAlive())
		{
			_worker_cv.wait_for(workerLock, std::chrono::seconds(1), [this]() { return _stopped; });
		}
		_run_once_active.fetch_sub(1);
		workerLock.unlock();
		_worker_cv.notify_all();
	}
	else
	{
		std::lock_guard<std::mutex> workerGuard(_worker_mutex);
		const uint32_t refreshSpan = _refresh_span;
		_worker_running.store(true, std::memory_order_release);
		_worker.reset(new StdThread([this, refreshSpan]() {
			wt_vendor_td::WorkerMark mark(this);
			std::unique_lock<std::mutex> workerLock(_worker_mutex);
			while (!_stopped)
			{
#ifdef WT_DUMPER_TEST_HOOKS
				if (g_dumperUseLegacySleepForTest.load())
				{
					workerLock.unlock();
					notify_dumper_worker_wait_for_test();
					std::this_thread::sleep_for(std::chrono::seconds(refreshSpan));
					workerLock.lock();
				}
				else
#endif
				{
#ifdef WT_DUMPER_TEST_HOOKS
					// The hook runs on the worker without the mutex (it may call back
					// into the dumper). release() cannot set the stop predicate between
					// re-locking and wait_for atomically entering the wait.
					workerLock.unlock();
					notify_dumper_worker_wait_for_test();
					workerLock.lock();
					if (_stopped)
						break;
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
			_worker_running.store(false, std::memory_order_release);
		}));
	}
}

bool Dumper::complete_release_locked()
{
	// Caller holds _release_mutex and is not on the worker or inside a callback.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(_stop_bound_ms);
	if (_worker)
	{
		if (_worker_running.load(std::memory_order_acquire) &&
			!wt_vendor_td::Lifecycle::waitUntil(deadline, [this]() { return !_worker_running.load(std::memory_order_acquire); }))
			return false;
		if (_worker->joinable())
			_worker->join();
		std::lock_guard<std::mutex> workerGuard(_worker_mutex);
		_worker.reset();
	}
	// run(true) on another thread observes the stop request and returns.
	if (!wt_vendor_td::Lifecycle::waitUntil(deadline, [this]() { return _run_once_active.load() == 0; }))
		return false;
	// After this point no user callback is running and none can start.
	return wt_vendor_td::Lifecycle::waitUntil(deadline, [this]() { return _callbacks.active() == 0; });
}

void Dumper::release()
{
	// T3-012 / 卡9：关闭用户回调准入并请求停止；重复调用幂等。
	_configured = false;
	_callbacks.close();
	{
		std::lock_guard<std::mutex> workerGuard(_worker_mutex);
		_stopped = true;
	}
	_worker_cv.notify_all();

	if (wt_vendor_td::Lifecycle::reentered(this))
	{
		// 卡9d：在 worker 或用户回调内请求释放时不能自 join/自等待；延后到下一次安全调用。
		uint32_t expected = _state.load(std::memory_order_acquire);
		if (expected != DS_RELEASED)
			_state.store(DS_STOP_DEFERRED, std::memory_order_release);
		WTSLogger::warn("交易数据落地模块在 worker/回调内请求释放：已关闭准入，join 延后到下一次安全调用");
		return;
	}

	std::lock_guard<std::mutex> releaseGuard(_release_mutex);
	if (_state.load(std::memory_order_acquire) == DS_RELEASED)
		return;
	if (!complete_release_locked())
	{
		_state.store(DS_DRAIN_UNCERTAIN, std::memory_order_release);
		WTSLogger::error("交易数据落地模块停止未能在 {} ms 内排空（worker 或用户回调仍在运行），对象保留，未视为已释放", _stop_bound_ms);
		return;
	}
	_state.store(DS_RELEASED, std::memory_order_release);
}

void Dumper::on_account(const char* channelid, uint32_t curTDate, const char* currency, double prebalance, 
		double balance, double dynbalance, double closeprofit, double dynprofit, double fee, 
		double margin, double deposit, double withdraw, double available, bool isLast)
{
	wt_vendor_td::Scope scope(_callbacks, wt_vendor_td::callbackFrames(), this);
	FuncOnAccount cb = _cb_account.load();
	if (scope && cb)
		cb(channelid, curTDate, currency, prebalance, balance, dynbalance, closeprofit, dynprofit, fee, margin, deposit, withdraw, available, isLast);
}

void Dumper::on_position(const char* channelid, const char* exchg, const char* code, uint32_t curTDate, uint32_t direct,
		double volume, double newvol, double cost, double margin, double avgpx, double dynprofit, uint32_t volscale, bool isLast)
{
	wt_vendor_td::Scope scope(_callbacks, wt_vendor_td::callbackFrames(), this);
	FuncOnPosition cb = _cb_position.load();
	if (scope && cb)
		cb(channelid, exchg, code, curTDate, direct, volume, newvol, cost, margin, avgpx, dynprofit, volscale, isLast);
}

void Dumper::on_trade(const char* channelid, const char* exchg, const char* code, uint32_t curTDate, const char* tradeid, const char* orderid, 
		uint32_t direct, uint32_t offset, double volume, double price, double amount, uint32_t ordertype, uint32_t tradetype, WtUInt64 tradetime, bool isLast)
{
	wt_vendor_td::Scope scope(_callbacks, wt_vendor_td::callbackFrames(), this);
	FuncOnTrade cb = _cb_trade.load();
	if (scope && cb)
		cb(channelid, exchg, code, curTDate, tradeid, orderid, direct, offset, volume, price, amount, ordertype, tradetype, tradetime, isLast);
}

void Dumper::on_order(const char* channelid, const char* exchg, const char* code, uint32_t curTDate, const char* orderid, uint32_t direct, uint32_t offset, double volume, double leftover, double traded, double price, uint32_t ordertype, uint32_t pricetype, WtUInt64 ordertime, uint32_t state, const char* statemsg, bool isLast)
{
	wt_vendor_td::Scope scope(_callbacks, wt_vendor_td::callbackFrames(), this);
	FuncOnOrder cb = _cb_order.load();
	if (scope && cb)
		cb(channelid, exchg, code, curTDate, orderid, direct, offset, volume, leftover, traded, price, ordertype, pricetype, ordertime, state, statemsg, isLast);
}
