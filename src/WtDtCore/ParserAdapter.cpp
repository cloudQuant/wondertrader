/*!
 * \file ParserAdapter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "ParserAdapter.h"
#include "DataManager.h"
#include "StateMonitor.h"
#include "WtHelper.h"
#include "IndexWorkerMgr.h"

#include <algorithm>
#include <exception>

#include "../Share/StrUtil.hpp"
#include "../Share/DLLHelper.hpp"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../WTSTools/WTSBaseDataMgr.h"
#include "../WTSTools/WTSLogger.h"


//////////////////////////////////////////////////////////////////////////
//ParserAdapter
ParserAdapter::ParserAdapter(WTSBaseDataMgr * bgMgr, DataManager* dtMgr, IndexWorkerMgr *idxFactory)
	: _parser_api(NULL)
	, _extra_parser_apis()
	, _remover(NULL)
	, _module_handle(NULL)
	, _bd_mgr(bgMgr)
	, _dt_mgr(dtMgr)
	, _idx_fact(idxFactory)
	, _stopped(false)
	, _release_uncertain(false)
	, _release_in_progress(false)
	, _cfg(NULL)
{
}


ParserAdapter::~ParserAdapter()
{
	// Before an API is created, init() may already have retained the config.
	// In that state no plugin can still be using it, so return the reference even
	// when the caller drops an adapter whose initialization failed early.
	if (!_release_uncertain && !hasApi())
	{
		if (_cfg != NULL)
		{
			_cfg->release();
			_cfg = NULL;
		}
		if (_module_handle != NULL)
		{
			DLLHelper::free_library((DllHandle)_module_handle);
			_module_handle = NULL;
		}
		_remover = NULL;
	}
}

bool ParserAdapter::initExt(const char* id, IParserApi* api)
{
	if (api == NULL)
		return false;

	if (_parser_api == api)
		return !_stopped;
	if (std::find(_extra_parser_apis.begin(), _extra_parser_apis.end(), api) != _extra_parser_apis.end())
		return false;

	if (_parser_api != NULL || _cfg != NULL || _module_handle != NULL || _stopped)
	{
		try
		{
			_extra_parser_apis.push_back(api);
		}
		catch (...)
		{
			// Ownership has transferred; losing the candidate would make cleanup unsafe.
			std::terminate();
		}
		_stopped = true;
		return false;
	}

	// Ownership transfers at the call boundary. Keep even rejected or partially
	// initialized candidates on this adapter so the Runner can quarantine the
	// adapter together with its borrowed managers and SPI context.
	_parser_api = api;

	if (_stopped || _cfg != NULL || id == NULL || id[0] == '\0' || _bd_mgr == NULL)
	{
		_stopped = true;
		return false;
	}

	_id = id;
	try
	{
		api->registerSpi(this);
		if (!api->init(NULL))
		{
			_stopped = true;
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed: api initializing failed...", _id.c_str());
			return false;
		}

		ContractSet contractSet;
		WTSArray* ayContract = _bd_mgr->getContracts();
		if (ayContract == NULL)
		{
			_stopped = true;
			return false;
		}
		try
		{
			WTSArray::Iterator it = ayContract->begin();
			for (; it != ayContract->end(); it++)
			{
				WTSContractInfo* contract = STATIC_CONVERT(*it, WTSContractInfo*);
				contractSet.insert(contract->getFullCode());
			}
		}
		catch (...)
		{
			ayContract->release();
			throw;
		}

		ayContract->release();
		api->subscribe(contractSet);
		contractSet.clear();
	}
	catch (...)
	{
		// Provider release is not a drain guarantee. Keep the API and SPI owner
		// intact for Runner quarantine; do not detach, release or delete here.
		_stopped = true;
		WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initialization threw an exception", _id.c_str());
		return false;
	}

	_stopped = false;
	return true;
}


bool ParserAdapter::init(const char* id, WTSVariant* cfg)
{
	if (cfg == NULL || id == NULL)
		return false;

	if (hasApi() || _cfg != NULL || _module_handle != NULL || _stopped)
		return false;

	_cfg = cfg;
	_cfg->retain();
	_stopped = true;

	try
	{
		_id = id;
		if (cfg->getString("module").empty())
			return false;

		std::string module = DLLHelper::wrap_module(cfg->getCString("module"), "lib");;
		if (!StdFile::exists(module.c_str()))
		{
			module = WtHelper::get_module_dir();
			module += "parsers/";
			module += DLLHelper::wrap_module(cfg->getCString("module"), "lib");
		}

		_module_handle = DLLHelper::load_library(module.c_str());
		if (_module_handle == NULL)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser module {} loading failed", _id.c_str(), module.c_str());
			return false;
		}
		WTSLogger::log_dyn("parser", _id.c_str(), LL_INFO, "[{}] Parser module {} loaded", _id.c_str(), module.c_str());

		DllHandle hInst = (DllHandle)_module_handle;
		FuncCreateParser pFuncCreateParser = (FuncCreateParser)DLLHelper::get_symbol(hInst, "createParser");
		if (pFuncCreateParser == NULL)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Entrance function createParser not found", _id.c_str());
			return false;
		}

		_remover = (FuncDeleteParser)DLLHelper::get_symbol(hInst, "deleteParser");
		_parser_api = pFuncCreateParser();
		if (_parser_api == NULL)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Creating parser api failed", _id.c_str());
			return false;
		}

		const std::string& strFilter = cfg->getString("filter");
		if (!strFilter.empty())
		{
			const StringVector &ayFilter = StrUtil::split(strFilter, ",");
			auto it = ayFilter.begin();
			for (; it != ayFilter.end(); it++)
				_filters.insert(*it);
		}

		_parser_api->registerSpi(this);
		if (!_parser_api->init(cfg))
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed: api initializing failed...", _id.c_str());
			return false;
		}

		if (_bd_mgr == NULL)
			return false;

		ContractSet contractSet;
		WTSArray* allContracts = _bd_mgr->getContracts();
		if (allContracts == NULL)
			return false;
		try
		{
			for (auto it = allContracts->begin(); it != allContracts->end(); it++)
			{
				WTSContractInfo* cInfo = STATIC_CONVERT(*it, WTSContractInfo*);
				if (_filters.empty())
				{
					contractSet.insert(cInfo->getFullAltCode());
					continue;
				}
				auto it1 = _filters.find(cInfo->getExchg());
				auto it2 = _filters.find(cInfo->getCode());
				auto it3 = _filters.find(cInfo->getFullCode());
				auto it4 = _filters.find(cInfo->getFullPid());
				auto it5 = _filters.find(cInfo->getAltCode());
				auto it6 = _filters.find(cInfo->getFullAltCode());

				if (it1 != _filters.end() || it2 != _filters.end()
					|| it3 != _filters.end() || it4 != _filters.end()
					|| it5 != _filters.end() || it6 != _filters.end())
					contractSet.insert(cInfo->getFullAltCode());
			}
		}
		catch (...)
		{
			allContracts->release();
			throw;
		}
		allContracts->release();

		_parser_api->subscribe(contractSet);
		contractSet.clear();
	}
	catch (...)
	{
		_stopped = true;
		WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initialization threw an exception", _id.c_str());
		return false;
	}

	_stopped = false;
	return true;
}

void ParserAdapter::release()
{
	_stopped = true;
	if (_release_uncertain || _release_in_progress)
		return;
	_release_in_progress = true;

	auto retainAfterReleaseException = [this]() {
		// A throwing provider release/deleter does not prove callbacks have drained,
		// and retrying destroy can double-release partially torn-down state.
		_release_uncertain = true;
		_release_in_progress = false;
		WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser release threw; owner retained for quarantine", _id.c_str());
	};

	if (_parser_api != NULL)
	{
		try
		{
			_parser_api->release();
		}
		catch (...)
		{
			retainAfterReleaseException();
			return;
		}

		try
		{
			IParserApi* parserApi = _parser_api;
			if (_remover != NULL)
				_remover(parserApi);
			else
			{
				delete parserApi;
				parserApi = NULL;
			}
			if (parserApi != NULL)
			{
				retainAfterReleaseException();
				return;
			}
			_parser_api = NULL;
			_remover = NULL;
		}
		catch (...)
		{
			retainAfterReleaseException();
			return;
		}
	}

	while (!_extra_parser_apis.empty())
	{
		IParserApi* extraParserApi = _extra_parser_apis.front();
		if (extraParserApi == NULL)
		{
			_extra_parser_apis.erase(_extra_parser_apis.begin());
			continue;
		}
		try
		{
			extraParserApi->release();
			delete extraParserApi;
			_extra_parser_apis.erase(_extra_parser_apis.begin());
		}
		catch (...)
		{
			retainAfterReleaseException();
			return;
		}
	}

	try
	{
		if (_cfg != NULL)
		{
			_cfg->release();
			_cfg = NULL;
		}
	}
	catch (...)
	{
		retainAfterReleaseException();
		return;
	}

	if (_module_handle != NULL)
	{
		DLLHelper::free_library((DllHandle)_module_handle);
		_module_handle = NULL;
	}
	_filters.clear();
	_release_in_progress = false;
}

bool ParserAdapter::run()
{
	if (_parser_api == NULL || _stopped)
		return false;

	_parser_api->connect();
	return true;
}

void ParserAdapter::handleSymbolList( const WTSArray* /*aySymbols*/ )
{
	
}

void ParserAdapter::handleTransaction(WTSTransData* transData)
{
	if (_stopped)
		return;

	if (transData->actiondate() == 0 || transData->tradingdate() == 0)
		return;

	WTSContractInfo* contract = _bd_mgr->getContract(transData->code(), transData->exchg());
	if (contract == NULL)
		return;

	transData->setContractInfo(contract);

	_dt_mgr->writeTransaction(transData);
}

void ParserAdapter::handleOrderDetail(WTSOrdDtlData* ordDetailData)
{
	if (_stopped)
		return;

	if (ordDetailData->actiondate() == 0 || ordDetailData->tradingdate() == 0)
		return;

	WTSContractInfo* contract = _bd_mgr->getContract(ordDetailData->code(), ordDetailData->exchg());
	if (contract == NULL)
		return;

	ordDetailData->setContractInfo(contract);

	_dt_mgr->writeOrderDetail(ordDetailData);
}

void ParserAdapter::handleOrderQueue(WTSOrdQueData* ordQueData)
{
	if (_stopped)
		return;

	if (ordQueData->actiondate() == 0 || ordQueData->tradingdate() == 0)
		return;

	WTSContractInfo* contract = _bd_mgr->getContract(ordQueData->code(), ordQueData->exchg());
	if (contract == NULL)
		return;

	ordQueData->setContractInfo(contract);
		
	_dt_mgr->writeOrderQueue(ordQueData);
}

void ParserAdapter::handleQuote( WTSTickData *quote, uint32_t procFlag )
{
	if (_stopped)
		return;

	if (quote->actiondate() == 0 || quote->tradingdate() == 0)
		return;

	WTSContractInfo* contract = quote->getContractInfo();
	if (contract == NULL)
	{
		contract = _bd_mgr->getContract(quote->code(), quote->exchg());
		quote->setContractInfo(contract);
	}

	if (contract == NULL)
		return;

	if (!_dt_mgr->writeTick(quote, procFlag))
		return;

	if (_idx_fact)
		_idx_fact->handle_quote(quote);
}

void ParserAdapter::handleParserLog( WTSLogLevel ll, const char* message)
{
	if (_stopped)
		return;

	WTSLogger::log_raw_by_cat("parser", ll, message);
}

IBaseDataMgr* ParserAdapter::getBaseDataMgr()
{
	return _bd_mgr;
}


//////////////////////////////////////////////////////////////////////////
//ParserAdapterMgr
void ParserAdapterMgr::release()
{
	bool allReleased = true;
	for (auto it = _adapters.begin(); it != _adapters.end(); it++)
	{
		if (it->second == NULL)
		{
			allReleased = false;
			continue;
		}
		try
		{
			it->second->release();
		}
		catch (...)
		{
			allReleased = false;
		}
		if (!it->second->releaseComplete())
			allReleased = false;
	}

	// Keep shared adapter owners available to the Runner when provider teardown
	// throws or remains incomplete. Runner must also keep the borrowed managers.
	if (allReleased)
		_adapters.clear();
}

bool ParserAdapterMgr::addAdapter(const char* id, ParserAdapterPtr& adapter)
{
	if (adapter == NULL || strlen(id) == 0)
		return false;

	auto it = _adapters.find(id);
	if (it != _adapters.end())
	{
		WTSLogger::error(" Same name of parsers: {}", id);
		return false;
	}

	_adapters[id] = adapter;

	return true;
}

ParserAdapterPtr ParserAdapterMgr::getAdapter(const char* id)
{
	auto it = _adapters.find(id);
	if (it != _adapters.end())
	{
		return it->second;
	}

	return ParserAdapterPtr();
}

void ParserAdapterMgr::run()
{
	for (auto it = _adapters.begin(); it != _adapters.end(); it++)
	{
		it->second->run();
	}

	WTSLogger::info("{} parsers started", _adapters.size());
}

bool ParserAdapterMgr::releaseComplete() const
{
	for (auto it = _adapters.begin(); it != _adapters.end(); it++)
	{
		if (it->second == NULL || !it->second->releaseComplete())
			return false;
	}
	return true;
}
