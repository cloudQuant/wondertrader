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
#include "../WtDtCore/ParserLifecycleHostV2.hpp"

NS_WTP_BEGIN
class WTSVariant;
NS_WTP_END

USING_NS_WTP;
class WTSBaseDataMgr;
class WtDtRunner;

class ParserAdapter : public IParserSpi, private boost::noncopyable
{
public:
	ParserAdapter(WTSBaseDataMgr * bgMgr, WtDtRunner* runner);
	// Destruction is not a drain barrier: a V2 owner gets one bounded drain
	// attempt; anything not proven drained is leaked (never unloaded). Owners
	// must keep adapters that still own an API, and the runner they borrow,
	// alive until process exit.
	virtual ~ParserAdapter();

public:
	// Card 9 (9a=A): dynamic modules must export a valid parser lifecycle V2
	// table and pass preflight before createParser. A failed provider init is
	// rolled back through request_stop/can_wait/wait_drained/destroy_api; only
	// DRAINED destroys the API and returns the module/config references. A
	// second init on an owning adapter is rejected (9c).
	bool	init(const char* id, WTSVariant* cfg);

	// Transfers ownership of every unique non-null api on entry, even when
	// initialization is rejected or fails. Re-passing an already-owned pointer
	// does not create a second owner; distinct APIs passed while busy are retained.
	// External APIs have no V2 drain proof: on failure the callback gate is
	// closed and the API retained; release() is a caller-asserted teardown.
	bool	initExt(const char* id, IParserApi* api);

	// Nonblocking two-phase teardown (9d); see WtDtCore ParserAdapter.
	bool	requestStopLifecycle() noexcept;
	bool	finishReleaseLifecycle(uint32_t waitMs = 0) noexcept;
	// requestStopLifecycle() followed by a bounded finishReleaseLifecycle().
	void	release();

	bool	run();

	bool	hasApi() const noexcept;
	bool	isQuarantined() const noexcept { return _release_uncertain.load(); }
	bool	isInitialized() const noexcept { return _initialized.load() && !_gate.closed(); }

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
	friend struct ParserAdapterTestAccess;
	friend class ParserAdapterMgr;

	IParserApi*			_parser_api;
	bool				_api_external;
	std::atomic<bool>	_release_uncertain;
	void*				_module_handle;
	const WtParserLifecycleApiV2* _lifecycle;
	bool				_stop_accepted;
	WTSBaseDataMgr*		_bd_mgr;
	WtDtRunner*			_dt_runner;

	mutable std::recursive_mutex _lifecycle_mutex;
	bool				_operation_active;
	wt_dt_parser_v2::CallbackGate _gate;
	std::atomic<bool>	_initialized;
	// initExt transfers every unique non-null API passed to this adapter, even
	// when this adapter is already busy. Rejected APIs are held until release().
	std::vector<IParserApi*> _additional_parser_apis;

	wt_hashset<std::string>			_filters;
	WTSVariant*			_cfg;
	std::string			_id;

	bool ownsParserApi(IParserApi* api) const;
	void retainAdditionalParserApi(IParserApi* api);
	bool isReleased() const;
	bool hasApiUnlocked() const noexcept;
	bool rollbackFailedInit(uint32_t waitMs) noexcept;
	void freeModuleAndConfig() noexcept;
	bool releaseExternalApis() noexcept;
};

typedef std::shared_ptr<ParserAdapter>	ParserAdapterPtr;
typedef wt_hashmap<std::string, ParserAdapterPtr>	ParserAdapterMap;

class ParserAdapterMgr : private boost::noncopyable
{
public:
	// Stops every adapter before polling any, then a bounded drain. Adapters
	// that still own an API stay in the map (quarantined owners).
	void	release();
	bool	requestStopLifecycleAll() noexcept;
	bool	finishReleaseLifecycleAll(uint32_t waitMs = 0) noexcept;

	void	run();
	// Reports command submission only; false can follow partial submission.
	bool	runWithResult();

	ParserAdapterPtr getAdapter(const char* id);
	bool hasRetainedAdapters() const { return !_adapters.empty(); }

	bool	addAdapter(const char* id, ParserAdapterPtr& adapter);

	uint32_t size() const { return (uint32_t)_adapters.size(); }

public:
	ParserAdapterMap _adapters;
};


