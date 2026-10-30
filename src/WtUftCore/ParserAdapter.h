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
#include <memory>
#include <vector>
#include <boost/core/noncopyable.hpp>

#include "../Includes/FasterDefs.h"
#include "../Includes/IParserApi.h"
#include "../Share/DLLHelper.hpp"


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
	// A non-null external API transfers to the adapter on entry. If init fails,
	// the adapter remains its owner and callers must retain this adapter and its
	// borrowed dependencies until the provider is known to be quiescent.
	// IParserApi::release() is not a callback-drain guarantee; release() may only
	// be called after a provider-specific callback barrier.
	bool	init(const char* id, WTSVariant* cfg, IParserStub* stub, IBaseDataMgr* bgMgr);
	bool	initExt(const char* id, IParserApi* api, IParserStub* stub, IBaseDataMgr* bgMgr);
	// Use this overload when the external API requires a provider deleter. A
	// non-null unchanged reference after a deleter call means pending; replacing it is
	// treated as uncertain and the adapter will not touch either pointer again.
	// The caller must keep the module containing remover mapped until this adapter
	// has been fully destroyed; this overload does not take ownership of a module handle.
	bool	initExt(const char* id, IParserApi* api, IParserStub* stub, IBaseDataMgr* bgMgr, FuncDeleteParser remover);
	bool	isInitialized() const { return _initialized; }
	bool	hasApi() const noexcept
	{
		return _parser_api != NULL || _api_tombstone_valid || _api_replacement_tombstone_valid
			|| _api_creation_uncertain || _teardown_uncertain || !_additional_api_owners.empty();
	}
	bool	isTeardownUncertain() const noexcept { return _teardown_uncertain || _api_creation_uncertain; }
	bool	isTeardownPending() const noexcept { return _teardown_pending; }

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

	IParserApi*			_parser_api;
	FuncDeleteParser	_remover;
	DllHandle			_module_inst;

	bool				_stopped;
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
