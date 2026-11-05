/*!
 * \file ParserAdapter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 行情解析模块适配类定义
 */
#pragma once
#include <set>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <boost/core/noncopyable.hpp>
#include "../Includes/IParserApi.h"
#include "ParserLifecycleHostV2.hpp"

NS_WTP_BEGIN
class WTSVariant;
NS_WTP_END

USING_NS_WTP;
class wxMainFrame;
class WTSBaseDataMgr;
class DataManager;
class IndexWorkerMgr;

class ParserAdapter : public IParserSpi, private boost::noncopyable
{
public:
	ParserAdapter(WTSBaseDataMgr * bgMgr, DataManager* dtMgr, IndexWorkerMgr *idxFactory);
	// Destruction is not a drain barrier. An adapter that still owns an API is
	// stopped and given one nonblocking V2 drain attempt; if the API remains, the
	// API, module and config are deliberately leaked (never unloaded). Owners must
	// keep such adapters (and every borrowed manager) alive until process exit.
	virtual ~ParserAdapter();

public:
	// Card 9 (9a=A): a dynamic module must export a valid parser lifecycle V2
	// table and pass its preflight before createParser. When the provider's init
	// fails (false/throw) or subscription throws, the adapter closes its callback
	// gate and rolls the API back through request_stop/can_wait/wait_drained/
	// destroy_api. Only DRAINED destroys the API and returns module/config
	// references; otherwise the owner is retained (PENDING retry or quarantine).
	// A second init on an adapter that owns anything is rejected (9c).
	bool	init(const char* id, WTSVariant* cfg);

	// A non-null external API transfers to this adapter on entry, including when
	// initialization fails. External APIs carry no V2 drain proof: on failure the
	// adapter closes its callback gate (no further dispatch to borrowed managers)
	// and keeps the API. Its release() is a caller-asserted teardown: the caller
	// must have established that the provider no longer calls this SPI.
	bool	initExt(const char* id, IParserApi* api);
	bool	hasApi() const noexcept;
	bool	isInitialized() const noexcept { return _initialized.load() && !_gate.closed(); }
	// True when provider teardown became uncertain; the owner is quarantined
	// permanently and release() never retries it.
	bool	isQuarantined() const noexcept { return _release_uncertain.load(); }
	// Reports whether all owned API/config/module handles have been released.
	bool	releaseComplete() const noexcept;
	// True when every owned API came from initExt (no V2 table, no module).
	bool	ownsOnlyExternalApis() const noexcept;
	// True once the callback gate is closed and no admitted callback is running:
	// this adapter no longer dispatches into its borrowed managers.
	bool	callbacksQuiesced(uint32_t waitMs = 0) const noexcept
	{
		return _gate.closed() && !_gate.inCallbackOnThisThread() && _gate.waitDrained(waitMs);
	}

	// Nonblocking, retryable two-phase teardown (9d). requestStopLifecycle closes
	// the callback gate and sends V2 request_stop. finishReleaseLifecycle destroys
	// a V2 API only after can_wait OK and DRAINED, then unloads the module and
	// returns the config reference. Both return false while anything remains.
	bool	requestStopLifecycle() noexcept;
	bool	finishReleaseLifecycle(uint32_t waitMs = 0) noexcept;

	// requestStopLifecycle() followed by a bounded finishReleaseLifecycle().
	void	release();

	bool	run();

	const char* id() const { return _id.c_str(); }

public:
	virtual void handleSymbolList(const WTSArray* aySymbols) override;

	virtual void handleQuote(WTSTickData *quote, uint32_t procFlag) override;

	virtual void handleOrderQueue(WTSOrdQueData* ordQueData) override;

	virtual void handleTransaction(WTSTransData* transData) override;

	virtual void handleOrderDetail(WTSOrdDtlData* ordDetailData) override;

	virtual void handleParserLog(WTSLogLevel ll, const char* message) override;

	virtual IBaseDataMgr* getBaseDataMgr() override;

private:
	bool	hasApiUnlocked() const noexcept;
	bool	rollbackFailedInit(uint32_t waitMs) noexcept;
	void	freeModuleAndConfig() noexcept;
	bool	releaseExternalApis() noexcept;

	IParserApi*			_parser_api;
	bool				_api_external;		// _parser_api came from initExt (no V2 proof)
	std::vector<IParserApi*> _extra_parser_apis;
	void*				_module_handle;
	const WtParserLifecycleApiV2* _lifecycle;
	bool				_stop_accepted;
	WTSBaseDataMgr*		_bd_mgr;
	DataManager*		_dt_mgr;
	IndexWorkerMgr*		_idx_fact;

	mutable std::recursive_mutex _lifecycle_mutex;
	bool				_operation_active;
	wt_dt_parser_v2::CallbackGate _gate;
	std::atomic<bool>	_initialized;
	std::atomic<bool>	_release_uncertain;

	typedef wt_hashset<std::string>	ExchgFilter;
	ExchgFilter			_filters;
	WTSVariant*			_cfg;
	std::string			_id;
};

typedef std::shared_ptr<ParserAdapter>	ParserAdapterPtr;
typedef wt_hashmap<std::string, ParserAdapterPtr>	ParserAdapterMap;

class ParserAdapterMgr : private boost::noncopyable
{
public:
	// requestStopLifecycleAll() then a bounded finishReleaseLifecycleAll(). Adapters that
	// still own an API stay in the map (quarantined owners).
	void	release();
	// Sends stop to every adapter before any adapter is polled.
	bool	requestStopLifecycleAll() noexcept;
	// Erases adapters whose teardown completed; false while any owner remains.
	bool	finishReleaseLifecycleAll(uint32_t waitMs = 0) noexcept;

	void	run();
	// True only if every connect command was submitted.
	// failedIds (optional) receives the ids whose connect command was not submitted (K14).
	bool	runWithResult(std::vector<std::string>* failedIds = NULL);
	// False means the map is retaining adapter owners after incomplete teardown.
	bool	releaseComplete() const;

	ParserAdapterPtr getAdapter(const char* id);

	bool	addAdapter(const char* id, ParserAdapterPtr& adapter);

	uint32_t size() const { return (uint32_t)_adapters.size(); }

public:
	ParserAdapterMap _adapters;
};

