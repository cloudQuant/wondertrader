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
#include <boost/core/noncopyable.hpp>
#include "../Includes/IParserApi.h"

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
	virtual ~ParserAdapter();

public:
	bool	init(const char* id, WTSVariant* cfg);

	// A non-null external API transfers to this adapter on entry, including when
	// initialization fails. Keep this adapter quarantined with its borrowed
	// managers until the provider is known to have stopped using the SPI; keep
	// the API's module mapped through release() as well.
	bool	initExt(const char* id, IParserApi* api);
	bool	hasApi() const noexcept { return _parser_api != NULL || !_extra_parser_apis.empty(); }
	// Reports whether all owned API/config/module handles have been released. A
	// true result is not evidence of provider callback quiescence.
	bool	releaseComplete() const noexcept
	{
		return !_release_uncertain && !_release_in_progress && !hasApi() && _cfg == NULL && _module_handle == NULL;
	}

	// Calls provider release and destroys owned APIs; release() is not a callback
	// drain barrier. The caller must establish provider-specific quiescence first.
	// If release or the plugin deleter throws, ownership stays here and a second
	// call will not retry partially completed provider teardown.
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
	IParserApi*			_parser_api;
	std::vector<IParserApi*> _extra_parser_apis;
	FuncDeleteParser	_remover;
	void*				_module_handle;
	WTSBaseDataMgr*		_bd_mgr;
	DataManager*		_dt_mgr;
	IndexWorkerMgr*		_idx_fact;

	bool				_stopped;
	bool				_release_uncertain;
	bool				_release_in_progress;

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
	void	release();

	void	run();
	// False means the map is retaining adapter owners after incomplete teardown.
	bool	releaseComplete() const;

	ParserAdapterPtr getAdapter(const char* id);

	bool	addAdapter(const char* id, ParserAdapterPtr& adapter);

	uint32_t size() const { return (uint32_t)_adapters.size(); }

public:
	ParserAdapterMap _adapters;
};


