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
#include "WtUftEngine.h"
#include "WtHelper.h"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IBaseDataMgr.h"

#include "../Share/StrUtil.hpp"

#include "../WTSTools/WTSLogger.h"

#include <exception>
#include <cstdlib>
#include <cstddef>

USING_NS_WTP;

namespace
{
	struct ParserCallbackFrame
	{
		const ParserAdapter* adapter;
		ParserCallbackFrame* previous;
	};

	thread_local ParserCallbackFrame* g_parser_callback_frame = NULL;

	class ScopedParserCallback
	{
	public:
		explicit ScopedParserCallback(const ParserAdapter* adapter)
			: _frame{ adapter, g_parser_callback_frame }
		{
			g_parser_callback_frame = &_frame;
		}

		~ScopedParserCallback()
		{
			g_parser_callback_frame = _frame.previous;
		}

	private:
		ParserCallbackFrame _frame;
	};

	struct WTSArrayReleaser
	{
		void operator()(WTSArray* array) const
		{
			if (array != NULL)
				array->release();
		}
	};
}

//////////////////////////////////////////////////////////////////////////
//ParserAdapter
ParserAdapter::ParserAdapter()
	: _parser_api(NULL)
	, _remover(NULL)
	, _module_inst(NULL)
	, _stopped(false)
	, _initialized(false)
	, _api_creation_uncertain(false)
	, _teardown_in_progress(false)
	, _teardown_uncertain(false)
	, _teardown_pending(false)
	, _parser_release_completed(false)
	, _api_tombstone(NULL)
	, _api_tombstone_valid(false)
	, _api_replacement_tombstone(NULL)
	, _api_replacement_tombstone_valid(false)
	, _bd_mgr(NULL)
	, _stub(NULL)
	, _cfg(NULL)
{
}


ParserAdapter::~ParserAdapter()
{
}

bool ParserAdapter::init(const char* id, WTSVariant* cfg, IParserStub* stub, IBaseDataMgr* bgMgr)
{
	if (cfg == NULL || bgMgr == NULL || hasApi() || _cfg != NULL || _module_inst != NULL)
		return false;

	_stub = stub;
	_bd_mgr = bgMgr;
	_id = id == NULL ? "" : id;
	_stopped = false;
	_initialized = false;
	_filters.clear();

	_cfg = cfg;
	_cfg->retain();

	try
	{
		//加载模块
		if (cfg->getString("module").empty())
		{
			release();
			return false;
		}

		std::string module = DLLHelper::wrap_module(cfg->getCString("module"), "lib");;

		//先看工作目录下是否有交易模块
		std::string dllpath = WtHelper::getModulePath(module.c_str(), "parsers", true);
		//如果没有,则再看模块目录,即dll同目录下
		if (!StdFile::exists(dllpath.c_str()))
			dllpath = WtHelper::getModulePath(module.c_str(), "parsers", false);

		DllHandle hInst = DLLHelper::load_library(dllpath.c_str());
		if (hInst == NULL)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser module {} loading failed", _id.c_str(), dllpath.c_str());
			release();
			return false;
		}
		_module_inst = hInst;
		WTSLogger::log_dyn("parser", _id.c_str(), LL_INFO, "[{}] Parser module {} loaded", _id.c_str(), dllpath.c_str());

		FuncCreateParser pFuncCreateParser = (FuncCreateParser)DLLHelper::get_symbol(hInst, "createParser");
		if (NULL == pFuncCreateParser)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Entrance function createParser not found", _id.c_str());
			release();
			return false;
		}

		// A factory can start provider work before it throws or returns NULL.
		// Once entered, preserve the module/config/dependency graph if creation
		// does not produce a visible API object.
		_api_creation_uncertain = true;
		try
		{
			_parser_api = pFuncCreateParser();
		}
		catch (...)
		{
			_stopped = true;
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Creating parser api threw an exception; candidate retained", _id.c_str());
			return false;
		}
		if (NULL == _parser_api)
		{
			_stopped = true;
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Creating parser api failed; candidate retained", _id.c_str());
			return false;
		}
		_api_creation_uncertain = false;

		_remover = (FuncDeleteParser)DLLHelper::get_symbol(hInst, "deleteParser");
	}
	catch (...)
	{
		_stopped = true;
		_initialized = false;
		// Before entering the factory, there is no provider-owned work to drain.
		// Dispose of the retained config/module on exceptional preflight exits.
		// If factory creation started or produced an API, keep the entire candidate
		// graph quarantined; an exception is not evidence that it is safe to tear down.
		if (!_api_creation_uncertain && _parser_api == NULL)
		{
			try
			{
				release();
			}
			catch (...)
			{
				_teardown_uncertain = true;
			}
		}
		return false;
	}

	try
	{
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
			_stopped = true;
			_initialized = false;
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed: api initializing failed; candidate retained", _id.c_str());
			return false;
		}

		ContractSet contractSet;
		if (!_filters.empty())
		{
			std::unique_ptr<WTSArray, WTSArrayReleaser> allContracts(_bd_mgr->getContracts());
			if (!allContracts)
			{
				_stopped = true;
				return false;
			}
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
		}
		else
		{
			std::unique_ptr<WTSArray, WTSArrayReleaser> ayContract(_bd_mgr->getContracts());
			if (!ayContract)
			{
				_stopped = true;
				return false;
			}
			WTSArray::Iterator it = ayContract->begin();
			for (; it != ayContract->end(); it++)
			{
				WTSContractInfo* contract = STATIC_CONVERT(*it, WTSContractInfo*);
				contractSet.insert(contract->getFullAltCode());
			}
		}

		_parser_api->subscribe(contractSet);
		_initialized = true;
		return true;
	}
	catch (const std::exception& e)
	{
		_stopped = true;
		_initialized = false;
		WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initialization failed: {}; candidate retained", _id.c_str(), e.what());
		return false;
	}
	catch (...)
	{
		_stopped = true;
		_initialized = false;
		WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initialization failed with an unknown exception; candidate retained", _id.c_str());
		return false;
	}
}

bool ParserAdapter::initExt(const char* id, IParserApi* api, IParserStub* stub, IBaseDataMgr* bgMgr)
{
	return initExt(id, api, stub, bgMgr, NULL);
}

bool ParserAdapter::initExt(const char* id, IParserApi* api, IParserStub* stub, IBaseDataMgr* bgMgr, FuncDeleteParser remover)
{
	if (api == NULL)
		return false;

	// A repeated pointer is already owned by this adapter. Do not create a
	// second owner for it, even when the adapter is otherwise busy.
	if (_parser_api == api
		|| (_api_tombstone_valid && _api_tombstone == api)
		|| (_api_replacement_tombstone_valid && _api_replacement_tombstone == api))
		return false;
	for (const ExternalApiOwner& owner : _additional_api_owners)
	{
		if (owner.api == api || owner.tombstone == api || owner.replacementTombstone == api)
			return false;
	}

	if (_teardown_in_progress || hasApi() || _cfg != NULL || _module_inst != NULL)
	{
		// The adapter cannot replace an existing candidate. Retain this distinct
		// non-null API too, without registering an SPI or initializing it.
		try
		{
			_additional_api_owners.push_back({ api, remover, NULL, NULL, stub, bgMgr, false });
		}
		catch (...)
		{
			// Returning would lose the transferred raw pointer. Fail-stop on the
			// only path where ownership cannot be recorded.
			std::terminate();
		}
		return false;
	}

	_parser_api = api;
	_remover = remover;
	_parser_release_completed = false;
	_stub = stub;
	_bd_mgr = bgMgr;
	_id = id == NULL ? "" : id;
	_stopped = true;
	_initialized = false;
	_filters.clear();

	// Ownership transfers even when the borrowed data manager is invalid. Keep
	// the API and other borrowed context attached for the caller's quarantine.
	if (bgMgr == NULL)
		return false;

	_stopped = false;

	try
	{
		_parser_api->registerSpi(this);
		if (!_parser_api->init(NULL))
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed: api initializing failed...", _id.c_str());
			_stopped = true;
			return false;
		}

		ContractSet contractSet;
		std::unique_ptr<WTSArray, WTSArrayReleaser> contracts(_bd_mgr->getContracts());
		if (!contracts)
		{
			_stopped = true;
			return false;
		}
		for (auto it = contracts->begin(); it != contracts->end(); ++it)
		{
			WTSContractInfo* contract = STATIC_CONVERT(*it, WTSContractInfo*);
			if (contract == NULL)
				continue;
			if (_filters.empty() || _filters.find(contract->getExchg()) != _filters.end()
				|| _filters.find(contract->getCode()) != _filters.end()
				|| _filters.find(contract->getFullCode()) != _filters.end()
				|| _filters.find(contract->getFullPid()) != _filters.end()
				|| _filters.find(contract->getAltCode()) != _filters.end()
				|| _filters.find(contract->getFullAltCode()) != _filters.end())
				contractSet.insert(contract->getFullAltCode());
		}
		_parser_api->subscribe(contractSet);
		_initialized = true;
	}
	catch (...)
	{
		_stopped = true;
		_initialized = false;
		return false;
	}
	return true;
}

void ParserAdapter::release()
{
	_stopped = true;
	_initialized = false;
	if (_teardown_uncertain || _teardown_in_progress)
		return;
	_teardown_pending = false;
	if (_api_creation_uncertain)
	{
		// The factory may have left an unobservable API or callback behind. No
		// release/delete/unload is safe because no provider drain contract exists.
		_teardown_uncertain = true;
		return;
	}
	_teardown_in_progress = true;
	const bool hadApiOwner = _parser_api != NULL || !_additional_api_owners.empty();
	if (_parser_api != NULL)
	{
		IParserApi* apiBeingReleased = _parser_api;
		if (!_parser_release_completed)
		{
			try
			{
				apiBeingReleased->release();
				_parser_release_completed = true;
			}
			catch (...)
			{
				// release() may have partially changed provider state. Keep the typed
				// pointer and all borrowed dependencies, and never retry this call.
				_teardown_uncertain = true;
				_teardown_in_progress = false;
				return;
			}
		}
		_api_tombstone = apiBeingReleased;
		_api_tombstone_valid = true;
		try
		{
			if (_remover != NULL)
			{
				_remover(_parser_api);
				if (_parser_api != NULL)
				{
					if (_parser_api != apiBeingReleased)
					{
						_api_replacement_tombstone = _parser_api;
						_api_replacement_tombstone_valid = true;
						_parser_api = NULL;
						_teardown_uncertain = true;
						_teardown_in_progress = false;
						return;
					}
					_api_tombstone = NULL;
					_api_tombstone_valid = false;
					_teardown_pending = true;
					_teardown_in_progress = false;
					return;
				}
			}
			else
				delete apiBeingReleased;
		}
		catch (...)
		{
			// A remover can destroy the object or replace the reference before
			// throwing. Keep those addresses opaque; never retry or dereference them.
			IParserApi* replacement = _parser_api;
			_parser_api = NULL;
			if (replacement != NULL && replacement != apiBeingReleased)
			{
				_api_replacement_tombstone = replacement;
				_api_replacement_tombstone_valid = true;
			}
			_teardown_uncertain = true;
			_teardown_in_progress = false;
			return;
		}
		_api_tombstone = NULL;
		_api_tombstone_valid = false;
		_parser_api = NULL;
		_remover = NULL;
		_parser_release_completed = false;
	}
	for (std::size_t ownerIndex = 0; ownerIndex < _additional_api_owners.size(); ++ownerIndex)
	{
		IParserApi* apiBeingReleased = _additional_api_owners[ownerIndex].api;
		if (apiBeingReleased == NULL)
			continue;
		if (!_additional_api_owners[ownerIndex].releaseCompleted)
		{
			try
			{
				apiBeingReleased->release();
				_additional_api_owners[ownerIndex].releaseCompleted = true;
			}
			catch (...)
			{
				_teardown_uncertain = true;
				_teardown_in_progress = false;
				return;
			}
		}
		const FuncDeleteParser ownerRemover = _additional_api_owners[ownerIndex].remover;
		// Temporarily keep an opaque address in the owner record while plugin
		// destruction runs. Reentrant initExt calls must not adopt the same API.
		_additional_api_owners[ownerIndex].api = NULL;
		_additional_api_owners[ownerIndex].tombstone = apiBeingReleased;
		IParserApi* apiForRemoval = apiBeingReleased;
		try
		{
			if (ownerRemover != NULL)
			{
				ownerRemover(apiForRemoval);
				if (apiForRemoval != NULL)
				{
					if (apiForRemoval != apiBeingReleased)
					{
						_additional_api_owners[ownerIndex].replacementTombstone = apiForRemoval;
						_teardown_uncertain = true;
						_teardown_in_progress = false;
						return;
					}
					_additional_api_owners[ownerIndex].api = apiForRemoval;
					_additional_api_owners[ownerIndex].tombstone = NULL;
					_teardown_pending = true;
					_teardown_in_progress = false;
					return;
				}
			}
			else
				delete apiBeingReleased;
		}
		catch (...)
		{
			if (apiForRemoval != NULL && apiForRemoval != apiBeingReleased)
				_additional_api_owners[ownerIndex].replacementTombstone = apiForRemoval;
			_teardown_uncertain = true;
			_teardown_in_progress = false;
			return;
		}
		_additional_api_owners[ownerIndex].tombstone = NULL;
	}
	_additional_api_owners.clear();

	if (_cfg != NULL)
	{
		try
		{
			_cfg->release();
		}
		catch (...)
		{
			_teardown_uncertain = true;
			_teardown_in_progress = false;
			return;
		}
		_cfg = NULL;
	}

	// IParserApi::release() is not a callback-drain guarantee. Once an API was
	// created, leave its plugin module mapped; caller-side quiescence is still
	// required before this adapter and its borrowed graph can be destroyed.
	if (_module_inst != NULL)
	{
		if (hadApiOwner)
			_module_inst = NULL; // deliberately leave the module mapped
		else
		{
			DLLHelper::free_library(_module_inst);
			_module_inst = NULL;
		}
	}
	_bd_mgr = NULL;
	_stub = NULL;
	_filters.clear();
	_teardown_in_progress = false;
}

bool ParserAdapter::run()
{
	if (_parser_api == NULL || !_initialized || _stopped)
		return false;

	_parser_api->connect();
	return true;
}

bool ParserAdapter::isInParserCallbackOnCurrentThread() const noexcept
{
	for (ParserCallbackFrame* frame = g_parser_callback_frame; frame != NULL; frame = frame->previous)
	{
		if (frame->adapter == this)
			return true;
	}
	return false;
}

void ParserAdapter::handleSymbolList(const WTSArray* /*aySymbols*/)
{
	ScopedParserCallback callbackScope(this);
}

void ParserAdapter::handleQuote(WTSTickData *quote, uint32_t /*procFlag*/)
{
	ScopedParserCallback callbackScope(this);
	if (quote == NULL || _stopped || quote->actiondate() == 0)
		return;

	WTSContractInfo* cInfo = quote->getContractInfo();
	if (cInfo == NULL && _bd_mgr != NULL)
	{
		cInfo = _bd_mgr->getContract(quote->code(), quote->exchg());
		if (cInfo != NULL)
			// getContract() returns a borrowed object owned by the base-data manager.
			// WTSTickData stores the pointer without changing its reference count.
			quote->setContractInfo(cInfo);
	}
	if (cInfo == NULL)
		return;

	quote->setCode(cInfo->getFullCode());

	_stub->handle_push_quote(quote);
}

void ParserAdapter::handleOrderQueue(WTSOrdQueData* ordQueData)
{
	ScopedParserCallback callbackScope(this);
	if (_stopped)
		return;

	if (!_filters.empty() && (_filters.find(ordQueData->exchg()) == _filters.end()))
		return;

	if (ordQueData->actiondate() == 0 || ordQueData->tradingdate() == 0)
		return;

	WTSContractInfo* cInfo = _bd_mgr->getContract(ordQueData->code(), ordQueData->exchg());
	if (cInfo == NULL)
		return;

	ordQueData->setCode(cInfo->getFullCode());

	if (_stub)
		_stub->handle_push_order_queue(ordQueData);
}

void ParserAdapter::handleOrderDetail(WTSOrdDtlData* ordDtlData)
{
	ScopedParserCallback callbackScope(this);
	if (_stopped)
		return;

	if (!_filters.empty() && (_filters.find(ordDtlData->exchg()) == _filters.end()))
		return;

	if (ordDtlData->actiondate() == 0 || ordDtlData->tradingdate() == 0)
		return;

	WTSContractInfo* cInfo = _bd_mgr->getContract(ordDtlData->code(), ordDtlData->exchg());
	if (cInfo == NULL)
		return;

	ordDtlData->setCode(cInfo->getFullCode());

	if (_stub)
		_stub->handle_push_order_detail(ordDtlData);
}

void ParserAdapter::handleTransaction(WTSTransData* transData)
{
	ScopedParserCallback callbackScope(this);
	if (_stopped)
		return;

	if (!_filters.empty() && (_filters.find(transData->exchg()) == _filters.end()))
		return;

	if (transData->actiondate() == 0 || transData->tradingdate() == 0)
		return;

	WTSContractInfo* cInfo = _bd_mgr->getContract(transData->code(), transData->exchg());
	if (cInfo == NULL)
		return;

	transData->setCode(cInfo->getFullCode());

	if (_stub)
		_stub->handle_push_transaction(transData);
}


void ParserAdapter::handleParserLog(WTSLogLevel ll, const char* message)
{
	ScopedParserCallback callbackScope(this);
	if (_stopped)
		return;

	WTSLogger::log_dyn_raw("parser", _id.c_str(), ll, message);
}


//////////////////////////////////////////////////////////////////////////
//ParserAdapterMgr
void ParserAdapterMgr::release()
{
	for (auto it = _adapters.begin(); it != _adapters.end();)
	{
		ParserAdapterPtr adapter = it->second;
		if (adapter != NULL)
			adapter->release();

		if (adapter != NULL && (adapter->hasApi() || adapter->isTeardownUncertain()))
			++it;
		else
			it = _adapters.erase(it);
	}
}

bool ParserAdapterMgr::hasRetainedAdapters() const noexcept
{
	for (auto it = _adapters.begin(); it != _adapters.end(); ++it)
	{
		const ParserAdapterPtr& adapter = it->second;
		if (adapter != NULL && (adapter->hasApi() || adapter->isTeardownUncertain()))
			return true;
	}
	return false;
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
