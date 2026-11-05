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
#include "../Includes/DateMappingBinding.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../WTSTools/WTSBaseDataMgr.h"
#include "../WTSTools/WTSLogger.h"

#include <algorithm>
#include <exception>


namespace
{
	// Bound for synchronous drains (failed-init rollback, release(), destructor).
	const uint32_t kBoundedDrainWaitMs = 2000;

	struct LifecycleOperation
	{
		bool& active;
		explicit LifecycleOperation(bool& value) : active(value) { active = true; }
		~LifecycleOperation() { active = false; }
	};
}

//////////////////////////////////////////////////////////////////////////
//ParserAdapter
ParserAdapter::ParserAdapter(WTSBaseDataMgr * bgMgr, WtDtRunner* runner)
	: _parser_api(NULL)
	, _api_external(false)
	, _release_uncertain(false)
	, _module_handle(NULL)
	, _lifecycle(NULL)
	, _stop_accepted(false)
	, _bd_mgr(bgMgr)
	, _dt_runner(runner)
	, _operation_active(false)
	, _initialized(false)
	, _cfg(NULL)
{
}

ParserAdapter::~ParserAdapter()
{
	_gate.close();
	_initialized = false;
	if (_release_uncertain.load() || _operation_active)
		return;

	if (_parser_api != NULL && !_api_external && _lifecycle != NULL &&
		!_gate.inCallbackOnThisThread() && _gate.waitDrained(kBoundedDrainWaitMs))
	{
		if (wt_dt_parser_v2::request_stop(_lifecycle, _parser_api, _stop_accepted) == wt_dt_parser_v2::StepResult::Done)
			(void)wt_dt_parser_v2::drain_and_destroy(_lifecycle, _parser_api, _stop_accepted, kBoundedDrainWaitMs);
	}

	if (hasApiUnlocked())
	{
		WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser adapter destroyed while its API is not proven drained; API, module and config are leaked", _id.c_str());
		return;
	}
	freeModuleAndConfig();
}

bool ParserAdapter::hasApiUnlocked() const noexcept
{
	return _parser_api != NULL || !_additional_parser_apis.empty() || _release_uncertain.load();
}

bool ParserAdapter::hasApi() const noexcept
{
	try
	{
		std::unique_lock<std::recursive_mutex> lock(_lifecycle_mutex, std::try_to_lock);
		return !lock.owns_lock() || _operation_active || hasApiUnlocked();
	}
	catch (...)
	{
		return true;
	}
}

void ParserAdapter::freeModuleAndConfig() noexcept
{
	_lifecycle = NULL;
	_stop_accepted = false;
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

bool ParserAdapter::rollbackFailedInit(uint32_t waitMs) noexcept
{
	_gate.close();
	_initialized = false;
	if (_parser_api == NULL || _api_external)
		return false;

	using namespace wt_dt_parser_v2;
	StepResult step = request_stop(_lifecycle, _parser_api, _stop_accepted);
	if (step == StepResult::Done && !_gate.inCallbackOnThisThread() && _gate.waitDrained(waitMs))
		step = drain_and_destroy(_lifecycle, _parser_api, _stop_accepted, waitMs);
	else if (step == StepResult::Done)
		step = StepResult::Pending;

	if (step == StepResult::Uncertain)
	{
		_release_uncertain = true;
		WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser rollback is uncertain; API, module and config are quarantined", _id.c_str());
		return false;
	}
	if (step == StepResult::Pending)
	{
		WTSLogger::log_dyn("parser", _id.c_str(), LL_WARN, "[{}] Parser rollback is pending; owner retained for a later release", _id.c_str());
		return false;
	}

	freeModuleAndConfig();
	return true;
}

bool ParserAdapter::initExt(const char* id, IParserApi* api)
{
	if (api == NULL)
		return false;

	std::unique_lock<std::recursive_mutex> lock(_lifecycle_mutex, std::try_to_lock);
	if (!lock.owns_lock() || _operation_active)
	{
		retainAdditionalParserApi(api);
		return false;
	}
	LifecycleOperation operation(_operation_active);

	// A repeated handoff of an already-owned pointer does not create another
	// owner. A distinct API passed while busy is still transferred to this
	// adapter, but is not initialized or registered.
	if (ownsParserApi(api))
		return false;

	if (_parser_api != NULL || _cfg != NULL || _module_handle != NULL || _release_uncertain.load())
	{
		retainAdditionalParserApi(api);
		return false;
	}

	// Take ownership before validating other arguments: every non-null API is
	// transferred on entry, including calls rejected for an invalid id.
	_parser_api = api;
	_api_external = true;
	_lifecycle = NULL;
	_stop_accepted = false;
	_initialized = false;
	if (id == NULL)
		return false;

	try
	{
		_id = id;
		_gate.open();
		_parser_api->registerSpi(this);
		if (!_parser_api->init(NULL))
		{
			_gate.close();
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed: api initializing failed...", _id.c_str());
			return false;
		}

		if (_bd_mgr == NULL)
		{
			_gate.close();
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

		if (_gate.closed())
			return false;
		_parser_api->subscribe(contractSet);
	}
	catch (...)
	{
		_gate.close();
		WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed with exception", _id.c_str());
		return false;
	}

	if (_gate.closed())
		return false;
	_initialized = true;
	return true;
}

bool ParserAdapter::ownsParserApi(IParserApi* api) const
{
	if (api == _parser_api)
		return true;

	return std::find(_additional_parser_apis.begin(), _additional_parser_apis.end(), api)
		!= _additional_parser_apis.end();
}

bool ParserAdapter::isReleased() const
{
	return _parser_api == NULL
		&& !_release_uncertain.load()
		&& _additional_parser_apis.empty()
		&& _module_handle == NULL
		&& _cfg == NULL
		&& _filters.empty()
		&& _gate.drained();
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
	if (id == NULL || cfg == NULL)
		return false;

	std::unique_lock<std::recursive_mutex> lock(_lifecycle_mutex, std::try_to_lock);
	if (!lock.owns_lock() || _operation_active)
		return false;
	LifecycleOperation operation(_operation_active);

	if (hasApiUnlocked() || _cfg != NULL || _module_handle != NULL || _initialized.load())
		return false;

	_id = id;
	_initialized = false;
	_api_external = false;
	_lifecycle = NULL;
	_stop_accepted = false;
	_gate.close();

	_cfg = cfg;
	_cfg->retain();
	auto failWithoutApi = [this]() {
		freeModuleAndConfig();
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

		const WtParserLifecycleApiV2* table = NULL;
		switch (wt_dt_parser_v2::bind_lifecycle((DllHandle)_module_handle, table))
		{
		case wt_dt_parser_v2::BindResult::Ok:
			break;
		case wt_dt_parser_v2::BindResult::MissingGetter:
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser module lacks lifecycle V2; rejected before createParser", _id.c_str());
			return failWithoutApi();
		case wt_dt_parser_v2::BindResult::InvalidTable:
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Invalid parser lifecycle V2 table", _id.c_str());
			return failWithoutApi();
		case wt_dt_parser_v2::BindResult::PreflightRejected:
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser lifecycle preflight rejected", _id.c_str());
			return failWithoutApi();
		case wt_dt_parser_v2::BindResult::Uncertain:
		default:
			_release_uncertain = true;
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser lifecycle preflight state is uncertain; module quarantined", _id.c_str());
			return false;
		}
		_lifecycle = table;

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
			_release_uncertain = true;
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Creating parser api threw; module quarantined", _id.c_str());
			return false;
		}
		if (NULL == _parser_api)
		{
			// V2: a NULL factory result leaves no asynchronous work behind.
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Creating parser api failed", _id.c_str());
			return failWithoutApi();
		}

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

		if (_bd_mgr == NULL)
		{
			(void)rollbackFailedInit(kBoundedDrainWaitMs);
			return false;
		}

		_gate.open();
		// K31=A: inject the host DateMappingV1 table before init when the parser module exports the binder.
		if (_bd_mgr != NULL)
		{
			const int bound = wtp::bindDateMappingV1(DLLHelper::get_symbol(_module_handle, WT_BIND_PARSER_DATE_MAPPING_V1),
				_parser_api, _bd_mgr->getDateMappingV1());
			if (bound == 1)
				WTSLogger::log_dyn("parser", _id.c_str(), LL_INFO, "[{}] Parser bound to the host date mapping table", _id.c_str());
			else if (bound == 0)
				WTSLogger::log_dyn("parser", _id.c_str(), LL_WARN, "[{}] Parser rejected the host date mapping table; it keeps the legacy mapping path", _id.c_str());
		}

		_parser_api->registerSpi(this);
		if (!_parser_api->init(cfg))
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed: api initializing failed...", _id.c_str());
			(void)rollbackFailedInit(kBoundedDrainWaitMs);
			return false;
		}

		ContractSet contractSet;
		WTSArray* allContracts = _bd_mgr->getContracts();
		if (allContracts == NULL)
		{
			(void)rollbackFailedInit(kBoundedDrainWaitMs);
			return false;
		}
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
		allContracts->release();

		if (_gate.closed())
		{
			(void)rollbackFailedInit(kBoundedDrainWaitMs);
			return false;
		}
		_parser_api->subscribe(contractSet);
	}
	catch (...)
	{
		WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed with exception", _id.c_str());
		if (_parser_api == NULL)
		{
			if (!_release_uncertain.load())
				return failWithoutApi();
			return false;
		}
		(void)rollbackFailedInit(kBoundedDrainWaitMs);
		return false;
	}

	if (_gate.closed())
	{
		(void)rollbackFailedInit(kBoundedDrainWaitMs);
		return false;
	}
	_initialized = true;
	return true;
}

bool ParserAdapter::releaseExternalApis() noexcept
{
	// Caller-asserted teardown for APIs without V2 proof. A throwing provider
	// release leaves its state unknowable; keep every owner and context.
	if (_parser_api != NULL && _api_external)
	{
		try
		{
			_parser_api->release();
		}
		catch (...)
		{
			_release_uncertain = true;
			return false;
		}
		delete _parser_api;
		_parser_api = NULL;
		_api_external = false;
	}

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
			return false;
		}
		delete api;
		api = NULL;
	}
	_additional_parser_apis.clear();
	return true;
}

bool ParserAdapter::requestStopLifecycle() noexcept
{
	_gate.close();
	_initialized = false;
	try
	{
		std::unique_lock<std::recursive_mutex> lock(_lifecycle_mutex, std::try_to_lock);
		if (!lock.owns_lock() || _operation_active || _release_uncertain.load())
			return false;
		LifecycleOperation operation(_operation_active);
		if (_parser_api == NULL || _api_external)
			return true;

		using namespace wt_dt_parser_v2;
		const StepResult step = request_stop(_lifecycle, _parser_api, _stop_accepted);
		if (step == StepResult::Uncertain)
		{
			_release_uncertain = true;
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser stop request failed; owner quarantined", _id.c_str());
		}
		return step == StepResult::Done;
	}
	catch (...)
	{
		return false;
	}
}

bool ParserAdapter::finishReleaseLifecycle(uint32_t waitMs /* = 0 */) noexcept
{
	try
	{
		std::unique_lock<std::recursive_mutex> lock(_lifecycle_mutex, std::try_to_lock);
		if (!lock.owns_lock() || _operation_active || _release_uncertain.load())
			return false;
		LifecycleOperation operation(_operation_active);

		// A callback on this thread cannot wait for itself (9d): defer to a safe point.
		// Otherwise wait (bounded) for admitted host callbacks to leave before any
		// provider teardown or release of borrowed managers (R123-003).
		if (!_gate.closed() || _gate.inCallbackOnThisThread() || !_gate.waitDrained(waitMs))
			return false;

		if (_parser_api != NULL && !_api_external)
		{
			using namespace wt_dt_parser_v2;
			const StepResult step = drain_and_destroy(_lifecycle, _parser_api, _stop_accepted, waitMs);
			if (step == StepResult::Uncertain)
			{
				_release_uncertain = true;
				WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser drain state is uncertain; API, module and config are quarantined", _id.c_str());
				return false;
			}
			if (step != StepResult::Done)
				return false;
		}

		if (!releaseExternalApis())
			return false;

		freeModuleAndConfig();
		return true;
	}
	catch (...)
	{
		return false;
	}
}

void ParserAdapter::release()
{
	if (requestStopLifecycle())
		(void)finishReleaseLifecycle(kBoundedDrainWaitMs);
}

bool ParserAdapter::run()
{
	std::unique_lock<std::recursive_mutex> lock(_lifecycle_mutex, std::try_to_lock);
	if (!lock.owns_lock() || _operation_active)
		return false;
	LifecycleOperation operation(_operation_active);
	if (_parser_api == NULL || !_initialized.load() || _gate.closed())
		return false;

	return _parser_api->connect();
}

void ParserAdapter::handleSymbolList( const WTSArray* /*aySymbols*/ )
{
	
}

void ParserAdapter::handleTransaction(WTSTransData* transData)
{
	wt_dt_parser_v2::CallbackGate::Scope scope(_gate);
	if (!scope.admitted() || transData == NULL)
		return;


	if (transData->actiondate() == 0 || transData->tradingdate() == 0)
		return;

	WTSContractInfo* contract = _bd_mgr->getContract(transData->code(), transData->exchg());
	if (contract == NULL)
		return;

}

void ParserAdapter::handleOrderDetail(WTSOrdDtlData* ordDetailData)
{
	wt_dt_parser_v2::CallbackGate::Scope scope(_gate);
	if (!scope.admitted() || ordDetailData == NULL)
		return;

	if (ordDetailData->actiondate() == 0 || ordDetailData->tradingdate() == 0)
		return;

	WTSContractInfo* contract = _bd_mgr->getContract(ordDetailData->code(), ordDetailData->exchg());
	if (contract == NULL)
		return;

}

void ParserAdapter::handleOrderQueue(WTSOrdQueData* ordQueData)
{
	wt_dt_parser_v2::CallbackGate::Scope scope(_gate);
	if (!scope.admitted() || ordQueData == NULL)
		return;

	if (ordQueData->actiondate() == 0 || ordQueData->tradingdate() == 0)
		return;

	WTSContractInfo* contract = _bd_mgr->getContract(ordQueData->code(), ordQueData->exchg());
	if (contract == NULL)
		return;
		
}

void ParserAdapter::handleQuote( WTSTickData *quote, uint32_t /*procFlag*/ )
{
	wt_dt_parser_v2::CallbackGate::Scope scope(_gate);
	if (!scope.admitted() || quote == NULL || quote->actiondate() == 0 || quote->tradingdate() == 0)
		return;

	if (_dt_runner)
		_dt_runner->proc_tick(quote);
}

void ParserAdapter::handleParserLog( WTSLogLevel ll, const char* message)
{
	wt_dt_parser_v2::CallbackGate::Scope scope(_gate);
	if (!scope.admitted())
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
	(void)requestStopLifecycleAll();
	(void)finishReleaseLifecycleAll(kBoundedDrainWaitMs);
}

bool ParserAdapterMgr::requestStopLifecycleAll() noexcept
{
	bool allStopped = true;
	for (auto& entry : _adapters)
	{
		if (!entry.second || !entry.second->requestStopLifecycle())
			allStopped = false;
	}
	return allStopped;
}

bool ParserAdapterMgr::finishReleaseLifecycleAll(uint32_t waitMs /* = 0 */) noexcept
{
	bool allReleased = true;
	for (auto it = _adapters.begin(); it != _adapters.end();)
	{
		ParserAdapterPtr adapter = it->second;
		bool done = false;
		if (adapter)
		{
			// Keep an adapter whose provider teardown is pending or uncertain.
			// Its owner state must remain available for retry or quarantine.
			done = adapter->finishReleaseLifecycle(waitMs) && adapter->isReleased();
		}
		if (done || !adapter)
			it = _adapters.erase(it);
		else
		{
			allReleased = false;
			++it;
		}
	}
	return allReleased;
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
