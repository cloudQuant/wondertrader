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
#include "WtHelper.h"
#include "WtDtRunner.h"

#include "../Share/StrUtil.hpp"
#include "../Share/DLLHelper.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/CodeHelper.hpp"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../WTSTools/WTSBaseDataMgr.h"
#include "../WTSTools/WTSLogger.h"

#include <algorithm>
#include <exception>


//////////////////////////////////////////////////////////////////////////
//ParserAdapter
ParserAdapter::ParserAdapter(WTSBaseDataMgr * bgMgr, WtDtRunner* runner)
	: _parser_api(NULL)
	, _uncertain_original_api(NULL)
	, _release_uncertain(false)
	, _remover(NULL)
	, _module_handle(NULL)
	, _bd_mgr(bgMgr)
	, _dt_runner(runner)
	, _stopped(false)
	, _initialized(false)
	, _cfg(NULL)
{
}

ParserAdapter::~ParserAdapter()
{
}

bool ParserAdapter::initExt(const char* id, IParserApi* api)
{
	if (api == NULL)
		return false;

	// A repeated handoff of an already-owned pointer does not create another
	// owner. A distinct API passed while busy is still transferred to this
	// adapter, but is not initialized or registered.
	if (ownsParserApi(api))
		return false;

	if (_parser_api != NULL || _cfg != NULL || _module_handle != NULL)
	{
		retainAdditionalParserApi(api);
		return false;
	}

	// Take ownership before validating other arguments: every non-null API is
	// transferred on entry, including calls rejected for an invalid id.
	_parser_api = api;
	_stopped = true;
	_initialized = false;
	if (id == NULL)
		return false;

	try
	{
		_id = id;
		_parser_api->registerSpi(this);
		if (!_parser_api->init(NULL))
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed: api initializing failed...", _id.c_str());
			return false;
		}

		if (_bd_mgr == NULL)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed: base data manager is null", _id.c_str());
			return false;
		}

		ContractSet contractSet;
		WTSArray* ayContract = _bd_mgr->getContracts();
		if (ayContract != NULL)
		{
			WTSArray::Iterator it = ayContract->begin();
			for (; it != ayContract->end(); it++)
			{
				WTSContractInfo* contract = STATIC_CONVERT(*it, WTSContractInfo*);
				contractSet.insert(contract->getFullCode());
			}
			ayContract->release();
		}

		_parser_api->subscribe(contractSet);
		_initialized = true;
		_stopped = false;
	}
	catch (...)
	{
		_stopped = true;
		WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed with exception", _id.c_str());
		return false;
	}

	return _initialized;
}

bool ParserAdapter::ownsParserApi(IParserApi* api) const
{
	if (api == _parser_api || api == _uncertain_original_api)
		return true;

	return std::find(_additional_parser_apis.begin(), _additional_parser_apis.end(), api)
		!= _additional_parser_apis.end();
}

bool ParserAdapter::isReleased() const
{
	return _parser_api == NULL
		&& _uncertain_original_api == NULL
		&& !_release_uncertain
		&& _remover == NULL
		&& _additional_parser_apis.empty()
		&& _module_handle == NULL
		&& _cfg == NULL
		&& _filters.empty();
}

void ParserAdapter::retainAdditionalParserApi(IParserApi* api)
{
	if (api == NULL || ownsParserApi(api))
		return;

	try
	{
		_additional_parser_apis.push_back(api);
	}
	catch (...)
	{
		// The call has transferred ownership, and this API may retain a callback
		// into the adapter. If memory exhaustion prevents recording that owner,
		// continuing or returning would make safe cleanup impossible.
		std::terminate();
	}
}


bool ParserAdapter::init(const char* id, WTSVariant* cfg)
{
	if (id == NULL || cfg == NULL || _parser_api != NULL || _cfg != NULL || _module_handle != NULL)
		return false;

	_id = id;
	_stopped = true;
	_initialized = false;

	_cfg = cfg;
	_cfg->retain();
	auto failWithoutApi = [this]() {
		if (_module_handle != NULL)
		{
			DLLHelper::free_library(_module_handle);
			_module_handle = NULL;
		}
		_remover = NULL;
		_filters.clear();
		if (_cfg != NULL)
		{
			_cfg->release();
			_cfg = NULL;
		}
		return false;
	};

	try
	{
		//加载模块
		if (cfg->getString("module").empty())
			return failWithoutApi();

		std::string module = DLLHelper::wrap_module(cfg->getCString("module"), "lib");;

		if (!StdFile::exists(module.c_str()))
		{
			module = WtHelper::get_module_path(
				std::string("parsers/") + DLLHelper::wrap_module(cfg->getCString("module"), "lib"));
		}

		_module_handle = DLLHelper::load_library(module.c_str());
		if (_module_handle == NULL)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser module {} loading failed", _id.c_str(), module.c_str());
			return failWithoutApi();
		}
		else
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_INFO, "[{}] Parser module {} loaded", _id.c_str(), module.c_str());
		}

		FuncCreateParser pFuncCreateParser = (FuncCreateParser)DLLHelper::get_symbol(_module_handle, "createParser");
		if (NULL == pFuncCreateParser)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Entrance function createParser not found", _id.c_str());
			return failWithoutApi();
		}

		try
		{
			_parser_api = pFuncCreateParser();
		}
		catch (...)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Creating parser api threw an exception", _id.c_str());
			return failWithoutApi();
		}
		if (NULL == _parser_api)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Creating parser api failed", _id.c_str());
			return failWithoutApi();
		}

		_remover = (FuncDeleteParser)DLLHelper::get_symbol(_module_handle, "deleteParser");

		const std::string& strFilter = cfg->getString("filter");
		if (!strFilter.empty())
		{
			const StringVector &ayFilter = StrUtil::split(strFilter, ",");
			auto it = ayFilter.begin();
			for (; it != ayFilter.end(); it++)
			{
				_filters.insert(*it);
			}
		}

		_parser_api->registerSpi(this);
		if (!_parser_api->init(cfg))
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed: api initializing failed...", _id.c_str());
			return false;
		}

		ContractSet contractSet;
		if (!_filters.empty())
		{
			WTSArray* allContracts = _bd_mgr == NULL ? NULL : _bd_mgr->getContracts();
			if (allContracts == NULL)
				return false;
			for (auto it = allContracts->begin(); it != allContracts->end(); it++)
			{
				WTSContractInfo* cInfo = STATIC_CONVERT(*it, WTSContractInfo*);
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
			allContracts->release();
		}
		else
		{
			WTSArray* ayContract = _bd_mgr == NULL ? NULL : _bd_mgr->getContracts();
			if (ayContract == NULL)
				return false;
			WTSArray::Iterator it = ayContract->begin();
			for (; it != ayContract->end(); it++)
			{
				WTSContractInfo* contract = STATIC_CONVERT(*it, WTSContractInfo*);
				contractSet.insert(contract->getFullAltCode());
			}
			ayContract->release();
		}

		_parser_api->subscribe(contractSet);
		_initialized = true;
		_stopped = false;
	}
	catch (...)
	{
		// Keep the failed API owner, module, and configuration for runner quarantine.
		// IParserApi::release() is not documented as a callback-drain barrier.
		_stopped = true;
		WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed with exception", _id.c_str());
		if (_parser_api == NULL)
			return failWithoutApi();
		return false;
	}

	return _initialized;
}

void ParserAdapter::release()
{
	// IParserApi::release() has no callback-drain contract. Runner/provider
	// shutdown must stop and drain callbacks before this adapter is released.
	_stopped = true;
	_initialized = false;
	if (_release_uncertain)
		return;

	if (_parser_api)
	{
		try
		{
			_parser_api->release();
		}
		catch (...)
		{
			// A throwing provider release leaves its state unknowable. Keep every
			// owner and context and require an external recovery decision.
			_release_uncertain = true;
			return;
		}

		if (_remover)
		{
			IParserApi* const apiBeforeDelete = _parser_api;
			try
			{
				_remover(_parser_api);
			}
			catch (...)
			{
				_uncertain_original_api = apiBeforeDelete;
				_release_uncertain = true;
				return;
			}
			if (_parser_api != NULL)
			{
				// A plugin deleter may refuse destruction while called from its
				// own callback thread. Keep the API, deleter, module, and config
				// together so the runner can retry after callbacks are quiescent.
				if (_parser_api != apiBeforeDelete)
				{
					// V1 provides no way to identify an unexpected replacement
					// pointer. Preserve both addresses and fail closed permanently.
					_uncertain_original_api = apiBeforeDelete;
					_release_uncertain = true;
				}
				return;
			}
		}
		else
		{
			delete _parser_api;
			_parser_api = NULL;
		}
	}
	_remover = NULL;

	// These APIs were transferred by rejected/busy initExt calls. They were not
	// registered with this adapter, but remain its unique owners and must be
	// released only after the caller has established callback quiescence.
	for (IParserApi*& api : _additional_parser_apis)
	{
		if (api == NULL)
			continue;

		try
		{
			api->release();
		}
		catch (...)
		{
			_release_uncertain = true;
			return;
		}
		delete api;
		api = NULL;
	}
	_additional_parser_apis.clear();

	if (_module_handle != NULL)
	{
		DLLHelper::free_library(_module_handle);
		_module_handle = NULL;
	}

	if (_cfg != NULL)
	{
		_cfg->release();
		_cfg = NULL;
	}
	_filters.clear();
}

bool ParserAdapter::run()
{
	if (_parser_api == NULL || !_initialized || _stopped)
		return false;

	return _parser_api->connect();
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
		
}

void ParserAdapter::handleQuote( WTSTickData *quote, uint32_t /*procFlag*/ )
{
	if (quote == NULL || _stopped || quote->actiondate() == 0 || quote->tradingdate() == 0)
		return;

	if (_dt_runner)
		_dt_runner->proc_tick(quote);
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
	for (auto it = _adapters.begin(); it != _adapters.end();)
	{
		if (it->second)
		{
			try
			{
				it->second->release();
			}
			catch (...)
			{
				// Keep an adapter whose provider threw during shutdown. Its owner
				// state must remain available for external recovery.
			}
		}

		if (!it->second || it->second->isReleased())
			it = _adapters.erase(it);
		else
			++it;
	}
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
	(void)runWithResult();
}

bool ParserAdapterMgr::runWithResult()
{
	bool allSubmitted = true;
	for (const auto& entry : _adapters)
	{
		bool submitted = false;
		try
		{
			if (entry.second)
				submitted = entry.second->run();
		}
		catch (...)
		{
			WTSLogger::error("Parser '{}' connect command threw", entry.first.c_str());
		}
		if (!submitted)
		{
			allSubmitted = false;
			WTSLogger::error("Parser '{}' connect command was not submitted", entry.first.c_str());
		}
	}
	if (allSubmitted)
		WTSLogger::info("Connect commands submitted for {} parsers", _adapters.size());
	return allSubmitted;
}
