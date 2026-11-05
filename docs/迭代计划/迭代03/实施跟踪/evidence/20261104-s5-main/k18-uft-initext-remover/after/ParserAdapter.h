/*!
 * \file ParserAdapter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once
#define WT_UFT_PARSER_K18_RESIDENT 1	// decision K18: isResidentQuarantined() is available
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include <boost/core/noncopyable.hpp>

#include "../Includes/FasterDefs.h"
#include "../Includes/IParserApi.h"
#include "../Includes/IParserLifecycleV2.h"
#include "../Includes/DateMappingV1.h"
#include "../Share/DLLHelper.hpp"


// Feature macro: card-9 callback admission barrier, V2 drain and quarantine (BUG-R78-001).
#define WT_UFT_PARSER_LIFECYCLE_CARD9 1

NS_WTP_BEGIN
class WTSVariant;

class IParserStub
{
public:
	virtual void			handle_push_quote(WTSTickData* /*curTick*/){}

	virtual void			handle_push_order_detail(WTSOrdDtlData* /*curOrdDtl*/){}
	virtual void			handle_push_order_queue(WTSOrdQueData* /*curOrdQue*/) {}
	virtual void			handle_push_transaction(WTSTransData* /*curTrans*/) {}
};

class ParserAdapter : public IParserSpi,
					private boost::noncopyable
{
public:
	ParserAdapter();
	virtual ~ParserAdapter();

public:
	// BUG-R78-001 / decision card 9 (9a=A+C, 9b=A, 9c=A, 9d=A):
	// - Dynamic modules must export a valid parser lifecycle V2 table; a module
	//   without it is rejected before createParser. Its API is destroyed and the
	//   module unloaded only after request_stop, a successful can_wait probe on
	//   the releasing thread, wait_drained == DRAINED and destroy_api == OK.
	// - A non-null external API passed to initExt transfers to the adapter on
	//   entry. Without a remover there is no provider proof that callbacks have
	//   stopped: release() only closes this adapter's callback admission and the
	//   API is retained (quarantined) until process exit; hasApi() stays true.
	// - With a remover, the provider deleter is the destruction confirmation: it
	//   must not set the reference to NULL before every provider thread and
	//   callback of that API has finished. A non-null unchanged reference means
	//   pending (retryable); a replaced reference is uncertain and never touched
	//   again. The caller must keep the module containing remover mapped until
	//   this adapter has been fully destroyed.
	// - release() first closes callback admission. Called from inside one of
	//   this adapter's callbacks it only requests stop and returns (teardown is
	//   deferred to a later release() from a safe point); while host callbacks
	//   are still running it returns without teardown and is retryable.
	// Owners must retain the adapter, its module and borrowed objects while
	// hasApi() is true.
	// K31=A：登记宿主的 DateMappingV1 表（须在 init 前调用）；解析模块导出 bindParserDateMappingV1 时注入。
	void	setDateMapping(const DateMappingV1& table) { _date_mapping = table; _has_date_mapping = true; }
	bool	init(const char* id, WTSVariant* cfg, IParserStub* stub, IBaseDataMgr* bgMgr);
	bool	initExt(const char* id, IParserApi* api, IParserStub* stub, IBaseDataMgr* bgMgr);
	bool	initExt(const char* id, IParserApi* api, IParserStub* stub, IBaseDataMgr* bgMgr, FuncDeleteParser remover);
	// Out of line on purpose: an inline (weak) definition could be bound at run
	// time to the unrelated WtCore ParserAdapter exported by another image.
	bool	isInitialized() const;
	bool	hasApi() const noexcept;
	bool	isTeardownUncertain() const noexcept;
	// Decision K18 (2026-11-04): an external API (initExt, with or without a remover) is
	// retained, with callback admission closed, until process exit. The manager then
	// keeps the adapter resident and treats its own release as complete.
	bool	isResidentQuarantined() const noexcept { return _legacy_quarantined && !_teardown_uncertain && !_teardown_pending; }
	bool	isTeardownPending() const noexcept;
	// True when the adapter retains an API it has no proof it may destroy.
	bool	isQuarantined() const noexcept;

	void	release();

	bool	run();

	const char* id() const{ return _id.c_str(); }

public:
	virtual void handleSymbolList(const WTSArray* /*aySymbols*/) override;

	/*
	 *	处理实时行情
	 *	@quote		实时行情
	 *	@bNeedSlice	是否需要切片,如果是从外部接入的快照行情数据,则需要切片,如果是内部广播的就不需要切片
	 */
	virtual void handleQuote(WTSTickData *quote, uint32_t procFlag) override;

	/*
	 *	处理委托队列数据（股票level2）
	 *	@ordQueData	委托对垒数据
	 */
	virtual void handleOrderQueue(WTSOrdQueData* ordQueData) override;

	/*
	 *	处理逐笔委托数据（股票level2）
	 *	@ordDetailData	逐笔委托数据
	 */
	virtual void handleOrderDetail(WTSOrdDtlData* ordDetailData) override;

	/*
		*	处理逐笔成交数据
		*	@transData	逐笔成交数据
		*/
	virtual void handleTransaction(WTSTransData* transData) override;

	virtual void handleParserLog(WTSLogLevel ll, const char* message) override;
	bool isInParserCallbackOnCurrentThread() const noexcept;

	virtual IBaseDataMgr* getBaseDataMgr() override { return _bd_mgr; }


private:
	struct ExternalApiOwner
	{
		IParserApi* api;
		FuncDeleteParser remover;
		void* tombstone;
		void* replacementTombstone;
		IParserStub* stub;
		IBaseDataMgr* bdMgr;
		bool releaseCompleted;
	};

	class CallbackAdmission;
	bool	retainsApiUnlocked() const noexcept;
	bool	releaseLifecycleApi();

	IParserApi*			_parser_api;
	FuncDeleteParser	_remover;
	DllHandle			_module_inst;
	const WtParserLifecycleApiV2* _lifecycle;	///< validated V2 table of a dynamic module, else NULL
	bool				_lifecycle_stop_accepted;
	bool				_legacy_quarantined;	///< external API without remover: never destroyed

	std::atomic<bool>	_stopped;	///< callback admission closed
	std::atomic<uint32_t>	_callbacks_in_flight;	///< host callbacks currently admitted
	bool				_initialized;
	bool				_api_creation_uncertain;
	bool				_teardown_in_progress;
	bool				_teardown_uncertain;
	bool				_teardown_pending;
	bool				_parser_release_completed;
	void*			_api_tombstone;
	bool			_api_tombstone_valid;
	void*			_api_replacement_tombstone;
	bool			_api_replacement_tombstone_valid;
	std::vector<ExternalApiOwner> _additional_api_owners;

	typedef wt_hashset<std::string>	ExchgFilter;
	ExchgFilter			_filters;
	IBaseDataMgr*		_bd_mgr;
	DateMappingV1		_date_mapping{};
	bool				_has_date_mapping = false;
	IParserStub*		_stub;
	WTSVariant*			_cfg;
	std::string			_id;
};

typedef std::shared_ptr<ParserAdapter>	ParserAdapterPtr;
typedef wt_hashmap<std::string, ParserAdapterPtr>	ParserAdapterMap;

class ParserAdapterMgr : private boost::noncopyable
{
public:
	// Call only after each provider has a callback-quiescence barrier.
	void	release();
	bool	hasRetainedAdapters() const noexcept;

	void	run();

	ParserAdapterPtr getAdapter(const char* id);

	bool	addAdapter(const char* id, ParserAdapterPtr& adapter);


public:
	ParserAdapterMap _adapters;
};

NS_WTP_END
