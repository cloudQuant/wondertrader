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
class WTSBaseDataMgr;
class WtDtRunner;

class ParserAdapter : public IParserSpi, private boost::noncopyable
{
public:
	ParserAdapter(WTSBaseDataMgr * bgMgr, WtDtRunner* runner);
	virtual ~ParserAdapter();

public:
	bool	init(const char* id, WTSVariant* cfg);

	// Transfers ownership of every unique non-null api on entry, even when
	// initialization is rejected or fails. Re-passing an already-owned pointer
	// does not create a second owner; distinct APIs passed while busy are retained.
	bool	initExt(const char* id, IParserApi* api);

	// IParserApi does not promise that release() drains callbacks. The caller must
	// stop producers and establish callback quiescence before releasing the API.
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
	friend struct ParserAdapterTestAccess;
	friend class ParserAdapterMgr;

	IParserApi*			_parser_api;
	IParserApi*			_uncertain_original_api;
	bool				_release_uncertain;
	FuncDeleteParser	_remover;
	void*				_module_handle;
	WTSBaseDataMgr*		_bd_mgr;
	WtDtRunner*			_dt_runner;

	bool				_stopped;
	bool				_initialized;
	// initExt transfers every unique non-null API passed to this adapter, even
	// when this adapter is already busy. Rejected APIs are held until release().
	std::vector<IParserApi*> _additional_parser_apis;

	wt_hashset<std::string>			_filters;
	WTSVariant*			_cfg;
	std::string			_id;

	bool ownsParserApi(IParserApi* api) const;
	void retainAdditionalParserApi(IParserApi* api);
	bool isReleased() const;
};

typedef std::shared_ptr<ParserAdapter>	ParserAdapterPtr;
typedef wt_hashmap<std::string, ParserAdapterPtr>	ParserAdapterMap;

class ParserAdapterMgr : private boost::noncopyable
{
public:
	void	release();

	void	run();

	ParserAdapterPtr getAdapter(const char* id);
	bool hasRetainedAdapters() const { return !_adapters.empty(); }

	bool	addAdapter(const char* id, ParserAdapterPtr& adapter);

	uint32_t size() const { return (uint32_t)_adapters.size(); }

public:
	ParserAdapterMap _adapters;
};


