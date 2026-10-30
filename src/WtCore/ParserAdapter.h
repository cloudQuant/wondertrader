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
class IHotMgr;

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
	// A non-null API passed to initExt transfers to this adapter immediately,
	// including when this adapter is busy or initialization later fails. A
	// rejected API is retained until release(); repeated submissions of an API
	// already owned here do not create another owner. The caller must keep an
	// initExt API's module mapped through release(). For init(), this adapter
	// retains the config and loaded module while it owns the API.
	// IParserApi::release() is not a callback-drain guarantee. Call release(),
	// destroy the adapter, unload an initExt API's module, or release borrowed
	// dependencies only after a provider-specific barrier proves callbacks and
	// other API use have stopped. Without such a barrier, keep the adapter and
	// its borrowed dependency graph quarantined; do not infer safety from release().
	// If API release or its remover throws, hasApi() remains true and subsequent
	// release() calls are no-ops because teardown may have partially completed.
	bool	init(const char* id, WTSVariant* cfg, IParserStub* stub, IBaseDataMgr* bgMgr, IHotMgr* hotMgr = NULL);

	bool	initExt(const char* id, IParserApi* api, IParserStub* stub, IBaseDataMgr* bgMgr, IHotMgr* hotMgr = NULL);
	bool	isInitialized() const { return _initialized; }
	// True while an API owner is retained or creation/teardown left its exact
	// state uncertain. A teardown exception is sticky and cannot be retried.
	bool	hasApi() const noexcept { return _parser_api != NULL || !_rejected_external_apis.empty() || _api_creation_uncertain || _release_uncertain; }

	void	release();

	bool	run();

	const char* id() const{ return _id.c_str(); }

public:
	virtual void handleSymbolList(const WTSArray* /*aySymbols*/) override {}

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

	virtual IBaseDataMgr* getBaseDataMgr() override { return _bd_mgr; }


private:
	friend struct ParserAdapterTestAccess;

	void retainRejectedExternalApi(IParserApi* api);

	IParserApi*			_parser_api;
	std::vector<IParserApi*> _rejected_external_apis;
	FuncDeleteParser	_remover;
	DllHandle			_module_handle;

	bool				_stopped;
	bool				_initialized;
	bool				_api_creation_uncertain;
	bool				_release_uncertain;
	bool				_remover_pending;

	/*
	 *	检查时间设置项
	 *	如果为true，则在收到行情的时候进行时间检查
	 *	主要适用于直接从行情源接入
	 *	因为直接从行情源接入很可能会有错误时间戳的数据进来
	 *	该选项默认为false
	 */
	bool				_check_time;

	typedef wt_hashset<std::string>	ExchgFilter;
	ExchgFilter			_filters;
	IBaseDataMgr*		_bd_mgr;
	IHotMgr*			_hot_mgr;
	IParserStub*		_stub;
	WTSVariant*			_cfg;
	std::string			_id;
};

typedef std::shared_ptr<ParserAdapter>	ParserAdapterPtr;
typedef wt_hashmap<std::string, ParserAdapterPtr>	ParserAdapterMap;

class ParserAdapterMgr : private boost::noncopyable
{
public:
	// The caller must establish provider-specific callback quiescence before
	// invoking this teardown pass; Manager release() is not itself a drain.
	void	release();
	// After release(), true means at least one adapter still retains an API or
	// an uncertain provider owner and this manager must remain quarantined.
	bool	hasQuarantinedAdapters() const noexcept;

	void	run();

	ParserAdapterPtr getAdapter(const char* id);

	bool	addAdapter(const char* id, ParserAdapterPtr& adapter);


public:
	ParserAdapterMap _adapters;
};

NS_WTP_END
