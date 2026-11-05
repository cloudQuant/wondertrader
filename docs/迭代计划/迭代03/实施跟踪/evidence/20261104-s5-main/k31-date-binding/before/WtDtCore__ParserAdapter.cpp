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


namespace
{
	// Bound for synchronous drains (failed-init rollback, release(), destructor).
	// A provider still PENDING after it keeps its owner for a later retry.
	const uint32_t kInitRollbackWaitMs = 2000;

	struct LifecycleOperation
	{
		bool& active;
		explicit LifecycleOperation(bool& value) : active(value) { active = true; }
		~LifecycleOperation() { active = false; }
	};
}

//////////////////////////////////////////////////////////////////////////
//ParserAdapter
ParserAdapter::ParserAdapter(WTSBaseDataMgr * bgMgr, DataManager* dtMgr, IndexWorkerMgr *idxFactory)
	: _parser_api(NULL)
	, _api_external(false)
	, _extra_parser_apis()
	, _module_handle(NULL)
	, _lifecycle(NULL)
	, _stop_accepted(false)
	, _bd_mgr(bgMgr)
	, _dt_mgr(dtMgr)
	, _idx_fact(idxFactory)
	, _operation_active(false)
	, _initialized(false)
	, _release_uncertain(false)
	, _cfg(NULL)
{
}


ParserAdapter::~ParserAdapter()
{
	_gate.close();
	_initialized = false;
	if (_release_uncertain.load() || _operation_active)
		return;

	// Only a V2 API can be proven drained here. External APIs need the caller's
	// barrier, so a destructor never releases them.
	if (_parser_api != NULL && !_api_external && _lifecycle != NULL &&
		!_gate.inCallbackOnThisThread() && _gate.waitDrained(kInitRollbackWaitMs))
	{
		if (wt_dt_parser_v2::request_stop(_lifecycle, _parser_api, _stop_accepted) == wt_dt_parser_v2::StepResult::Done)
			(void)wt_dt_parser_v2::drain_and_destroy(_lifecycle, _parser_api, _stop_accepted, kInitRollbackWaitMs);
	}

	if (hasApiUnlocked())
	{
		// Keep API, module and config mapped: code may still be executing.
		WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser adapter destroyed while its API is not proven drained; API, module and config are leaked", _id.c_str());
		return;
	}
	freeModuleAndConfig();
}

bool ParserAdapter::hasApiUnlocked() const noexcept
{
	return _parser_api != NULL || !_extra_parser_apis.empty() || _release_uncertain.load();
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

bool ParserAdapter::releaseComplete() const noexcept
{
	try
	{
		std::unique_lock<std::recursive_mutex> lock(_lifecycle_mutex, std::try_to_lock);
		if (!lock.owns_lock() || _operation_active)
			return false;
		return !hasApiUnlocked() && _cfg == NULL && _module_handle == NULL && _gate.drained();
	}
	catch (...)
	{
		return false;
	}
}

bool ParserAdapter::ownsOnlyExternalApis() const noexcept
{
	try
	{
		std::unique_lock<std::recursive_mutex> lock(_lifecycle_mutex, std::try_to_lock);
		if (!lock.owns_lock() || _operation_active)
			return false;
		if (_parser_api != NULL && !_api_external)
			return false;
		return _module_handle == NULL && _cfg == NULL && !_release_uncertain.load() &&
			(_parser_api != NULL || !_extra_parser_apis.empty());
	}
	catch (...)
	{
		return false;
	}
}

void ParserAdapter::freeModuleAndConfig() noexcept
{
	_lifecycle = NULL;
	_stop_accepted = false;
	if (_module_handle != NULL)
	{
		DLLHelper::free_library((DllHandle)_module_handle);
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
		// Busy/reentrant: ownership still transfers; keep the distinct candidate.
		if (api != _parser_api && std::find(_extra_parser_apis.begin(), _extra_parser_apis.end(), api) == _extra_parser_apis.end())
		{
			try { _extra_parser_apis.push_back(api); }
			catch (...) { std::terminate(); }
		}
		return false;
	}
	LifecycleOperation operation(_operation_active);

	if (_parser_api == api)
		return false;
	if (std::find(_extra_parser_apis.begin(), _extra_parser_apis.end(), api) != _extra_parser_apis.end())
		return false;

	if (_parser_api != NULL || _cfg != NULL || _module_handle != NULL || _release_uncertain.load() || _initialized.load())
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
		return false;
	}

	// Ownership transfers at the call boundary. Keep even rejected or partially
	// initialized candidates on this adapter so the Runner can quarantine the
	// adapter together with its borrowed managers and SPI context.
	_parser_api = api;
	_api_external = true;
	_lifecycle = NULL;
	_stop_accepted = false;
	_initialized = false;

	if (id == NULL || id[0] == '\0' || _bd_mgr == NULL)
		return false;

	_id = id;
	_gate.open();
	try
	{
		api->registerSpi(this);
		if (!api->init(NULL))
		{
			_gate.close();
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed: api initializing failed...", _id.c_str());
			return false;
		}

		ContractSet contractSet;
		WTSArray* ayContract = _bd_mgr->getContracts();
		if (ayContract == NULL)
		{
			_gate.close();
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
		if (_gate.closed())
			return false;
		api->subscribe(contractSet);
		contractSet.clear();
	}
	catch (...)
	{
		// Provider release is not a drain guarantee. Close the host gate and keep
		// the API and SPI owner intact for Runner quarantine.
		_gate.close();
		WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initialization threw an exception", _id.c_str());
		return false;
	}

	if (_gate.closed())
		return false;
	_initialized = true;
	return true;
}


bool ParserAdapter::init(const char* id, WTSVariant* cfg)
{
	if (cfg == NULL || id == NULL)
		return false;

	std::unique_lock<std::recursive_mutex> lock(_lifecycle_mutex, std::try_to_lock);
	if (!lock.owns_lock() || _operation_active)
		return false;
	LifecycleOperation operation(_operation_active);

	if (hasApiUnlocked() || _cfg != NULL || _module_handle != NULL || _initialized.load())
		return false;

	_cfg = cfg;
	_cfg->retain();
	_api_external = false;
	_lifecycle = NULL;
	_stop_accepted = false;
	_initialized = false;
	_gate.close();

	auto failWithoutApi = [this]() {
		freeModuleAndConfig();
		return false;
	};

	try
	{
		_id = id;
		if (cfg->getString("module").empty())
			return failWithoutApi();

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
			return failWithoutApi();
		}
		WTSLogger::log_dyn("parser", _id.c_str(), LL_INFO, "[{}] Parser module {} loaded", _id.c_str(), module.c_str());

		DllHandle hInst = (DllHandle)_module_handle;
		const WtParserLifecycleApiV2* table = NULL;
		switch (wt_dt_parser_v2::bind_lifecycle(hInst, table))
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
			// The table or preflight misbehaved; keep the module mapped.
			_release_uncertain = true;
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser lifecycle preflight state is uncertain; module quarantined", _id.c_str());
			return false;
		}
		_lifecycle = table;

		FuncCreateParser pFuncCreateParser = (FuncCreateParser)DLLHelper::get_symbol(hInst, "createParser");
		if (pFuncCreateParser == NULL)
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
			// An exception across the factory ABI violates the V2 contract.
			_release_uncertain = true;
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Creating parser api threw; module quarantined", _id.c_str());
			return false;
		}
		if (_parser_api == NULL)
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
				_filters.insert(*it);
		}

		if (_bd_mgr == NULL)
		{
			// Nothing was registered or started yet; destroy through V2.
			(void)rollbackFailedInit(kInitRollbackWaitMs);
			return false;
		}

		_gate.open();
		_parser_api->registerSpi(this);
		if (!_parser_api->init(cfg))
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed: api initializing failed...", _id.c_str());
			(void)rollbackFailedInit(kInitRollbackWaitMs);
			return false;
		}

		ContractSet contractSet;
		WTSArray* allContracts = _bd_mgr->getContracts();
		if (allContracts == NULL)
		{
			(void)rollbackFailedInit(kInitRollbackWaitMs);
			return false;
		}
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

		if (_gate.closed())
		{
			(void)rollbackFailedInit(kInitRollbackWaitMs);
			return false;
		}
		_parser_api->subscribe(contractSet);
		contractSet.clear();
	}
	catch (...)
	{
		WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initialization threw an exception", _id.c_str());
		if (_parser_api == NULL)
		{
			if (!_release_uncertain.load())
				return failWithoutApi();
			return false;
		}
		(void)rollbackFailedInit(kInitRollbackWaitMs);
		return false;
	}

	if (_gate.closed())
	{
		(void)rollbackFailedInit(kInitRollbackWaitMs);
		return false;
	}
	_initialized = true;
	return true;
}

bool ParserAdapter::releaseExternalApis() noexcept
{
	// Caller-asserted teardown of APIs without V2 proof (initExt and rejected
	// candidates). A throwing provider release leaves the owner quarantined.
	if (_parser_api != NULL && _api_external)
	{
		try
		{
			_parser_api->release();
			delete _parser_api;
		}
		catch (...)
		{
			_release_uncertain = true;
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser release threw; owner retained for quarantine", _id.c_str());
			return false;
		}
		_parser_api = NULL;
		_api_external = false;
	}

	while (!_extra_parser_apis.empty())
	{
		IParserApi* extraParserApi = _extra_parser_apis.front();
		if (extraParserApi != NULL)
		{
			try
			{
				extraParserApi->release();
				delete extraParserApi;
			}
			catch (...)
			{
				_release_uncertain = true;
				WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser release threw; owner retained for quarantine", _id.c_str());
				return false;
			}
		}
		_extra_parser_apis.erase(_extra_parser_apis.begin());
	}
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
		(void)finishReleaseLifecycle(kInitRollbackWaitMs);
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

	transData->setContractInfo(contract);

	_dt_mgr->writeTransaction(transData);
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

	ordDetailData->setContractInfo(contract);

	_dt_mgr->writeOrderDetail(ordDetailData);
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

	ordQueData->setContractInfo(contract);
		
	_dt_mgr->writeOrderQueue(ordQueData);
}

void ParserAdapter::handleQuote( WTSTickData *quote, uint32_t procFlag )
{
	wt_dt_parser_v2::CallbackGate::Scope scope(_gate);
	if (!scope.admitted() || quote == NULL)
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
	(void)finishReleaseLifecycleAll(kInitRollbackWaitMs);
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
		if (!adapter)
		{
			allReleased = false;
			++it;
			continue;
		}
		if (!adapter->finishReleaseLifecycle(waitMs) || !adapter->releaseComplete())
		{
			// Keep shared adapter owners when provider teardown is pending or
			// uncertain. The Runner must also keep the borrowed managers.
			allReleased = false;
			++it;
		}
		else
			it = _adapters.erase(it);
	}
	return allReleased;
}

bool ParserAdapterMgr::addAdapter(const char* id, ParserAdapterPtr& adapter)
{
	if (adapter == NULL || id == NULL || strlen(id) == 0)
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
	for (auto it = _adapters.begin(); it != _adapters.end(); it++)
	{
		bool submitted = false;
		try
		{
			submitted = it->second && it->second->run();
		}
		catch (...)
		{
			submitted = false;
		}
		if (!submitted)
		{
			allSubmitted = false;
			WTSLogger::error("Parser '{}' connect command was not submitted", it->first.c_str());
		}
	}

	WTSLogger::info("{} parsers started", _adapters.size());
	return allSubmitted;
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
