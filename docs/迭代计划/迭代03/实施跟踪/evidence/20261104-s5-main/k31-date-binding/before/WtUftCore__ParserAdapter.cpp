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

#include <algorithm>
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
	, _lifecycle(NULL)
	, _lifecycle_stop_accepted(false)
	, _legacy_quarantined(false)
	, _stopped(false)
	, _callbacks_in_flight(0)
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

// Host-side callback admission (card 9). A callback is admitted only while
// admission is open; release() closes admission first and tears down only
// after every admitted callback has left, so _stub/_bd_mgr/_cfg are never
// cleared under a running callback. seq_cst on both sides orders the
// increment/check here against the close/check in release().
class ParserAdapter::CallbackAdmission
{
public:
	explicit CallbackAdmission(ParserAdapter* owner)
		: _owner(owner), _admitted(false)
	{
		_owner->_callbacks_in_flight.fetch_add(1, std::memory_order_seq_cst);
		if (_owner->_stopped.load(std::memory_order_seq_cst))
			_owner->_callbacks_in_flight.fetch_sub(1, std::memory_order_seq_cst);
		else
			_admitted = true;
	}
	~CallbackAdmission()
	{
		if (_admitted)
			_owner->_callbacks_in_flight.fetch_sub(1, std::memory_order_seq_cst);
	}
	explicit operator bool() const { return _admitted; }

private:
	ParserAdapter* _owner;
	bool _admitted;
};

bool ParserAdapter::isInitialized() const
{
	return _initialized;
}

bool ParserAdapter::retainsApiUnlocked() const noexcept
{
	return _parser_api != NULL || _api_tombstone_valid || _api_replacement_tombstone_valid
		|| _api_creation_uncertain || _teardown_uncertain || !_additional_api_owners.empty();
}

bool ParserAdapter::hasApi() const noexcept
{
	return retainsApiUnlocked();
}

bool ParserAdapter::isTeardownUncertain() const noexcept
{
	return _teardown_uncertain || _api_creation_uncertain;
}

bool ParserAdapter::isTeardownPending() const noexcept
{
	return _teardown_pending;
}

bool ParserAdapter::isQuarantined() const noexcept
{
	return _legacy_quarantined || isTeardownUncertain();
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
	_lifecycle = NULL;
	_lifecycle_stop_accepted = false;
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

		// Card 9a=A: a dynamic provider must prove callback quiescence through the
		// versioned lifecycle table before any API is created.
		WtParserLifecycleV2GetterFn getter = reinterpret_cast<WtParserLifecycleV2GetterFn>(
			DLLHelper::get_symbol(hInst, WT_PARSER_LIFECYCLE_V2_GETTER_SYMBOL));
		if (getter == NULL)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser module lacks lifecycle V2; rejected before createParser", _id.c_str());
			release();
			return false;
		}
		const WtParserLifecycleApiV2* lifecycle = NULL;
		WtParserLifecycleV2Status preflight = WT_PARSER_LIFECYCLE_V2_ERROR;
		try
		{
			lifecycle = getter(WT_PARSER_LIFECYCLE_V2_ABI_MAJOR);
			if (lifecycle == NULL || lifecycle->struct_size < WT_PARSER_LIFECYCLE_V2_REQUIRED_SIZE
				|| (lifecycle->abi_version >> 16) != WT_PARSER_LIFECYCLE_V2_ABI_MAJOR
				|| (lifecycle->capabilities & WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES) != WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES
				|| lifecycle->reserved[0] != 0 || lifecycle->reserved[1] != 0
				|| lifecycle->preflight_factory == NULL || lifecycle->request_stop == NULL || lifecycle->can_wait == NULL
				|| lifecycle->wait_drained == NULL || lifecycle->destroy_api == NULL)
			{
				WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Invalid parser lifecycle V2 table; rejected before createParser", _id.c_str());
				release();
				return false;
			}
			preflight = lifecycle->preflight_factory();
		}
		catch (...)
		{
			// The provider broke the no-throw C ABI; its state is unknown.
			_stopped = true;
			_api_creation_uncertain = true;
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Parser lifecycle V2 probe threw; candidate retained", _id.c_str());
			return false;
		}
		if (preflight != WT_PARSER_LIFECYCLE_V2_OK)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser lifecycle V2 preflight rejected: {}", _id.c_str(), preflight);
			if (preflight == WT_PARSER_LIFECYCLE_V2_PENDING || preflight == WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK
				|| preflight == WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT || preflight == WT_PARSER_LIFECYCLE_V2_UNSUPPORTED)
			{
				release();
				return false;
			}
			_stopped = true;
			_api_creation_uncertain = true;
			return false;
		}

		FuncCreateParser pFuncCreateParser = (FuncCreateParser)DLLHelper::get_symbol(hInst, "createParser");
		if (NULL == pFuncCreateParser)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Entrance function createParser not found", _id.c_str());
			release();
			return false;
		}
		_lifecycle = lifecycle;

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
		// Destruction goes through the V2 table only; the legacy deleteParser
		// export is not a callback-drain proof.
		_remover = NULL;
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
	_lifecycle = NULL;
	_lifecycle_stop_accepted = false;
	_legacy_quarantined = false;
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

bool ParserAdapter::releaseLifecycleApi()
{
	// Card 9: only a V2 drain proof authorizes destroying the API. Every status
	// other than the expected one keeps the API, module and borrowed graph.
	void* handle = static_cast<void*>(_parser_api);
	WtParserLifecycleV2Status status = WT_PARSER_LIFECYCLE_V2_ERROR;
	try
	{
		if (!_lifecycle_stop_accepted)
		{
			status = _lifecycle->request_stop(handle);
			if (status != WT_PARSER_LIFECYCLE_V2_OK)
			{
				if (status == WT_PARSER_LIFECYCLE_V2_PENDING || status == WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK)
					_teardown_pending = true;
				else
					_teardown_uncertain = true;
				return false;
			}
			_lifecycle_stop_accepted = true;
		}

		status = _lifecycle->can_wait(handle);
		if (status != WT_PARSER_LIFECYCLE_V2_OK)
		{
			if (status == WT_PARSER_LIFECYCLE_V2_PENDING || status == WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK)
				_teardown_pending = true;
			else
				_teardown_uncertain = true;
			return false;
		}

		status = _lifecycle->wait_drained(handle, 0);
		if (status != WT_PARSER_LIFECYCLE_V2_DRAINED)
		{
			if (status == WT_PARSER_LIFECYCLE_V2_PENDING || status == WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK)
				_teardown_pending = true;
			else
				_teardown_uncertain = true;
			return false;
		}

		void* opaque = handle;
		status = _lifecycle->destroy_api(&opaque);
		if (status != WT_PARSER_LIFECYCLE_V2_OK || opaque != NULL)
		{
			// Keep the original handle even after an ambiguous destroy.
			_teardown_uncertain = true;
			return false;
		}
	}
	catch (...)
	{
		_teardown_uncertain = true;
		return false;
	}
	_parser_api = NULL;
	_lifecycle_stop_accepted = false;
	return true;
}

void ParserAdapter::release()
{
	// Close callback admission first: no new host callback can begin after this.
	_stopped.store(true, std::memory_order_seq_cst);
	_initialized = false;
	if (_teardown_uncertain || _teardown_in_progress)
		return;
	if (isInParserCallbackOnCurrentThread() || _callbacks_in_flight.load(std::memory_order_seq_cst) != 0)
	{
		// Card 9: a release from a callback (or while callbacks run) only
		// requests stop; teardown happens on a later release() at a safe point.
		_teardown_pending = true;
		return;
	}
	_teardown_pending = false;
	if (_api_creation_uncertain)
	{
		// The factory may have left an unobservable API or callback behind. No
		// release/delete/unload is safe because no provider drain contract exists.
		_teardown_uncertain = true;
		return;
	}
	if (_legacy_quarantined)
		return;	// retained until process exit; admission stays closed
	_teardown_in_progress = true;
	bool retainsUnprovenApi = false;
	if (_parser_api != NULL && _lifecycle != NULL)
	{
		if (!releaseLifecycleApi())
		{
			_teardown_in_progress = false;
			return;
		}
	}
	else if (_parser_api != NULL && _remover == NULL)
	{
		// An external API without a remover has no provider confirmation that
		// its callbacks have stopped (card 9a): quarantine it, never delete it.
		_legacy_quarantined = true;
		retainsUnprovenApi = true;
		WTSLogger::log_dyn("parser", _id.c_str(), LL_WARN,
			"[{}] External parser API has no callback-drain proof; retained until process exit", _id.c_str());
	}
	else if (_parser_api != NULL)
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
			// The remover is the provider's destruction confirmation: NULL means
			// the API and all of its callbacks are gone; unchanged means pending.
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
		const FuncDeleteParser ownerRemover = _additional_api_owners[ownerIndex].remover;
		if (ownerRemover == NULL)
		{
			// Same rule as the primary external API: no proof, no destruction.
			retainsUnprovenApi = true;
			continue;
		}
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
		// Temporarily keep an opaque address in the owner record while plugin
		// destruction runs. Reentrant initExt calls must not adopt the same API.
		_additional_api_owners[ownerIndex].api = NULL;
		_additional_api_owners[ownerIndex].tombstone = apiBeingReleased;
		IParserApi* apiForRemoval = apiBeingReleased;
		try
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
	if (retainsUnprovenApi)
	{
		// Keep every retained API with its borrowed graph and module. Owners
		// without proof stay in _additional_api_owners; finished ones are erased.
		_additional_api_owners.erase(std::remove_if(_additional_api_owners.begin(), _additional_api_owners.end(),
			[](const ExternalApiOwner& owner) { return owner.api == NULL; }), _additional_api_owners.end());
		_legacy_quarantined = true;
		_teardown_in_progress = false;
		return;
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

	// Every API this adapter owned has been destroyed with a drain proof (V2
	// DRAINED + destroy_api, or a provider remover confirmation), or none was
	// ever created; only then may the module be unloaded.
	if (_module_inst != NULL)
	{
		DLLHelper::free_library(_module_inst);
		_module_inst = NULL;
	}
	_lifecycle = NULL;
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
	CallbackAdmission admission(this);
}

void ParserAdapter::handleQuote(WTSTickData *quote, uint32_t /*procFlag*/)
{
	ScopedParserCallback callbackScope(this);
	CallbackAdmission admission(this);
	if (!admission || quote == NULL || quote->actiondate() == 0)
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
	CallbackAdmission admission(this);
	if (!admission)
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
	CallbackAdmission admission(this);
	if (!admission)
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
	CallbackAdmission admission(this);
	if (!admission)
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
	CallbackAdmission admission(this);
	if (!admission)
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
