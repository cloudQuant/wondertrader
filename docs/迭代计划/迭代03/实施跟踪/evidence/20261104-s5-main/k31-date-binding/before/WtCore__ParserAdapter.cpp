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
#include "WtEngine.h"
#include "WtCtaTicker.h"
#include "WtHelper.h"

#include "../Share/CodeHelper.hpp"
#include "../Share/TimeUtils.hpp"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/IHotMgr.h"

#include "../WTSTools/WTSLogger.h"

#include <cctype>
#include <cstddef>
#include <cstring>
#include <exception>
#include <new>

USING_NS_WTP;

namespace
{
	struct LifecycleOperation
	{
		bool& active;
		explicit LifecycleOperation(bool& value) : active(value) { active = true; }
		~LifecycleOperation() { active = false; }
	};

	bool isDigits(const char* begin, const char* end)
	{
		if (begin == end)
			return false;

		for (const char* p = begin; p != end; ++p)
		{
			if (!std::isdigit(static_cast<unsigned char>(*p)))
				return false;
		}
		return true;
	}

	bool isLetters(const char* begin, const char* end)
	{
		if (begin == end)
			return false;

		for (const char* p = begin; p != end; ++p)
		{
			if (!std::isalpha(static_cast<unsigned char>(*p)))
				return false;
		}
		return true;
	}

	bool isHyphenatedFuturesOptionCode(const char* code)
	{
		if (code == NULL || *code == '\0')
			return false;

		const char* first = std::strchr(code, '-');
		if (first == NULL)
			return false;

		const char* second = std::strchr(first + 1, '-');
		if (second == NULL || std::strchr(second + 1, '-') != NULL)
			return false;

		const char* month = first;
		while (month != code && std::isdigit(static_cast<unsigned char>(month[-1])))
			--month;

		return (first - month == 4)
			&& isLetters(code, month)
			&& second == first + 2
			&& (first[1] == 'C' || first[1] == 'P')
			&& isDigits(second + 1, code + std::strlen(code));
	}

	bool isCompactFuturesOptionCode(const char* code)
	{
		if (code == NULL || *code == '\0')
			return false;

		const char* end = code + std::strlen(code);
		const char* strike = end;
		while (strike != code && std::isdigit(static_cast<unsigned char>(strike[-1])))
			--strike;
		if (strike == code)
			return false;

		const char* optionSide = strike - 1;
		if (*optionSide != 'C' && *optionSide != 'P')
			return false;

		const char* month = optionSide;
		while (month != code && std::isdigit(static_cast<unsigned char>(month[-1])))
			--month;

		const std::size_t monthDigits = static_cast<std::size_t>(optionSide - month);
		return (monthDigits == 3 || monthDigits == 4)
			&& isLetters(code, month)
			&& isDigits(strike, end);
	}

	bool isFuturesOptionCode(const char* code, const char* exchange)
	{
		if (exchange == NULL)
			return false;
		if (std::strcmp(exchange, "CFFEX") == 0 || std::strcmp(exchange, "DCE") == 0)
			return isHyphenatedFuturesOptionCode(code);
		if (std::strcmp(exchange, "SHFE") == 0 || std::strcmp(exchange, "INE") == 0
			|| std::strcmp(exchange, "ZCE") == 0 || std::strcmp(exchange, "CZCE") == 0)
			return isCompactFuturesOptionCode(code);

		return isHyphenatedFuturesOptionCode(code) || isCompactFuturesOptionCode(code);
	}
}

//////////////////////////////////////////////////////////////////////////
//ParserAdapter
ParserAdapter::ParserAdapter()
	: _parser_api(NULL)
	, _contended_external_apis(NULL)
	, _remover(NULL)
	, _module_handle(NULL)
	, _lifecycle(NULL)
	, _operation_active(false)
	, _stop_accepted(false)
	, _external_stop_notified(false)
	, _stopped(false)
	, _initialized(false)
	, _api_creation_uncertain(false)
	, _release_uncertain(false)
	, _remover_pending(false)
	, _check_time(false)
	, _bd_mgr(NULL)
	, _stub(NULL)
	, _cfg(NULL)
{
}


ParserAdapter::~ParserAdapter()
{
}

bool ParserAdapter::hasApiUnlocked() const noexcept
{
	return _parser_api != NULL || !_rejected_external_apis.empty() ||
		_contended_external_apis.load() != NULL || _api_creation_uncertain || _release_uncertain;
}

bool ParserAdapter::hasApi() const noexcept
{
	try {
		std::unique_lock<std::recursive_mutex> lock(_lifecycle_mutex, std::try_to_lock);
		return !lock.owns_lock() || _operation_active || hasApiUnlocked();
	} catch (...) { return true; }
}

void ParserAdapter::retainContendedExternalApi(IParserApi* api) noexcept
{
	RetainedExternalNode* node = new (std::nothrow) RetainedExternalNode{api, NULL};
	if (!node) std::terminate(); // Ownership transferred at entry.
	RetainedExternalNode* head = _contended_external_apis.load();
	do { node->next = head; }
	while (!_contended_external_apis.compare_exchange_weak(head, node));
}

void ParserAdapter::retainRejectedExternalApi(IParserApi* api)
{
	if (api == NULL || api == _parser_api)
		return;

	for (IParserApi* retained : _rejected_external_apis)
	{
		if (retained == api)
			return;
	}

	try
	{
		_rejected_external_apis.push_back(api);
	}
	catch (...)
	{
		// Ownership has already transferred at the call boundary. Returning
		// would lose the only record of this owner, so fail-stop on OOM.
		std::terminate();
	}
}

bool ParserAdapter::initExt(const char* id, IParserApi* api, IParserStub* stub, IBaseDataMgr* bgMgr, IHotMgr* hotMgr/* = NULL*/)
{
	if (api == NULL)
		return false;
	std::unique_lock<std::recursive_mutex> lifecycleLock(_lifecycle_mutex, std::try_to_lock);
	if (!lifecycleLock.owns_lock() || _operation_active)
	{
		retainContendedExternalApi(api);
		return false;
	}
	LifecycleOperation operation(_operation_active);

	if (hasApiUnlocked() || _cfg != NULL || _module_handle != NULL)
	{
		// A non-null initExt argument transfers ownership even on this busy
		// path. Keep a distinct candidate without replacing or stopping the
		// adapter's currently registered API.
		retainRejectedExternalApi(api);
		return false;
	}

	_parser_api = api;
	_lifecycle = NULL;
	_stop_accepted = false;
	_external_stop_notified = false;
	_stub = stub;
	_bd_mgr = bgMgr;
	_hot_mgr = hotMgr;
	// External APIs do not provide parser configuration. Keep the documented
	// default deterministic, even if this adapter previously failed config init.
	_check_time = false;
	_stopped = false;
	_initialized = false;

	try
	{
		_id = id == NULL ? "" : id;
		WTSLogger::log_dyn("parser", _id.c_str(), LL_WARN, "[{}] External parser has no V2 drain proof; retaining its owner", _id.c_str());
		if (_bd_mgr == NULL)
		{
			// Ownership transferred at the initExt call boundary, but without a
			// base-data manager this adapter cannot safely service provider
			// callbacks. Keep the candidate quarantined and do not register or
			// initialize it.
			_stopped = true;
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initialization failed: base-data manager is required", _id.c_str());
			return false;
		}

		_parser_api->registerSpi(this);
		if (!_parser_api->init(NULL))
		{
			_stopped = true;
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed: api initializing failed...", _id.c_str());
			return false;
		}

		ContractSet contractSet;
		if (_bd_mgr != NULL)
		{
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
		}
		if (_stopped.load()) return false;
		_parser_api->subscribe(contractSet);
		_initialized = true;
		contractSet.clear();
	}
	catch (...)
	{
		// Keep the failed API owner on this adapter for the runner to quarantine.
		// IParserApi::release() is not a callback-drain guarantee.
		_stopped = true;
		WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initialization threw an exception", _id.c_str());
		return false;
	}

	return isInitialized();
}

bool ParserAdapter::init(const char* id, WTSVariant* cfg, IParserStub* stub, IBaseDataMgr* bgMgr, IHotMgr* hotMgr/* = NULL*/)
{
	std::unique_lock<std::recursive_mutex> lifecycleLock(_lifecycle_mutex, std::try_to_lock);
	if (!lifecycleLock.owns_lock() || _operation_active || id == NULL || cfg == NULL || bgMgr == NULL || hasApiUnlocked() || _cfg != NULL || _module_handle != NULL)
		return false;
	LifecycleOperation operation(_operation_active);

	_stub = stub;
	_bd_mgr = bgMgr;
	_hot_mgr = hotMgr;
	_id = id;

	if (_cfg != NULL)
		return false;

	_cfg = cfg;
	_cfg->retain();
	_stopped = false;
	_initialized = false;
	_stop_accepted = false;

	auto failWithoutApi = [this]() {
		if (_module_handle != NULL)
		{
			DLLHelper::free_library(_module_handle);
			_module_handle = NULL;
		}
		_remover = NULL;
		_lifecycle = NULL;
		if (_cfg != NULL)
		{
			_cfg->release();
			_cfg = NULL;
		}
		_filters.clear();
		return false;
	};

	_check_time = cfg->getBoolean("check_time");

	{
		//加载模块
		if (cfg->getString("module").empty())
			return failWithoutApi();

		std::string module = DLLHelper::wrap_module(cfg->getCString("module"), "lib");;

		//先看工作目录下是否有交易模块
		std::string dllpath = WtHelper::getModulePath(module.c_str(), "parsers", true);
		//如果没有,则再看模块目录,即dll同目录下
		if (!StdFile::exists(dllpath.c_str()))
			dllpath = WtHelper::getModulePath(module.c_str(), "parsers", false);

		_module_handle = DLLHelper::load_library(dllpath.c_str());
		if (_module_handle == NULL)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser module {} loading failed", _id.c_str(), dllpath.c_str());
			return failWithoutApi();
		}
		else
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_INFO, "[{}] Parser module {} loaded", _id.c_str(), dllpath.c_str());
		}

		WtParserLifecycleV2GetterFn getter = reinterpret_cast<WtParserLifecycleV2GetterFn>(
			DLLHelper::get_symbol(_module_handle, WT_PARSER_LIFECYCLE_V2_GETTER_SYMBOL));
		if (!getter)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser lacks lifecycle V2", _id.c_str());
			return failWithoutApi();
		}
		try
		{
			_lifecycle = getter(WT_PARSER_LIFECYCLE_V2_ABI_MAJOR);
			if (!_lifecycle || _lifecycle->struct_size < WT_PARSER_LIFECYCLE_V2_REQUIRED_SIZE ||
				(_lifecycle->abi_version >> 16) != WT_PARSER_LIFECYCLE_V2_ABI_MAJOR ||
				(_lifecycle->capabilities & WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES) != WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES ||
				_lifecycle->reserved[0] != 0 || _lifecycle->reserved[1] != 0 ||
				!_lifecycle->preflight_factory || !_lifecycle->request_stop || !_lifecycle->can_wait ||
				!_lifecycle->wait_drained || !_lifecycle->destroy_api)
			{
				WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Invalid parser lifecycle V2", _id.c_str());
				return failWithoutApi();
			}
			const WtParserLifecycleV2Status preflight = _lifecycle->preflight_factory();
			if (preflight != WT_PARSER_LIFECYCLE_V2_OK)
			{
				WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser lifecycle preflight rejected", _id.c_str());
				if (preflight != WT_PARSER_LIFECYCLE_V2_PENDING && preflight != WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK &&
					preflight != WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT && preflight != WT_PARSER_LIFECYCLE_V2_UNSUPPORTED)
				{
					_api_creation_uncertain = true;
					_stopped = true;
					return false;
				}
				return failWithoutApi();
			}
		}
		catch (...)
		{
			_api_creation_uncertain = true;
			_stopped = true;
			return false;
		}

		FuncCreateParser pFuncCreateParser = (FuncCreateParser)DLLHelper::get_symbol(_module_handle, "createParser");
		if (NULL == pFuncCreateParser)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Entrance function createParser not found", _id.c_str());
			return failWithoutApi();
		}
		if (_stopped.load())
			return failWithoutApi();

		try
		{
			_parser_api = pFuncCreateParser();
		}
		catch (...)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Creating parser api threw an exception", _id.c_str());
			_api_creation_uncertain = true;
			_stopped = true;
			return false;
		}
		if (NULL == _parser_api)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Creating parser api failed", _id.c_str());
			_api_creation_uncertain = true;
			_stopped = true;
			return false;
		}
		if (_stopped.load())
			return false;

	}

	try
	{
		const std::string& strFilter = cfg->getString("filter");
		if (!strFilter.empty())
		{
			const StringVector& ayFilter = StrUtil::split(strFilter, ",");
			for (const auto& filter : ayFilter)
				_filters.insert(filter);
		}

		_parser_api->registerSpi(this);
		if (!_parser_api->init(cfg))
		{
			_stopped = true;
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed: api initializing failed...", _id.c_str());
			return false;
		}

		ContractSet contractSet;
		if (_bd_mgr != NULL)
		{
			WTSArray* contracts = _bd_mgr->getContracts();
			if (contracts != NULL)
			{
				for (auto it = contracts->begin(); it != contracts->end(); ++it)
				{
					WTSContractInfo* contract = STATIC_CONVERT(*it, WTSContractInfo*);
					if (_filters.empty())
					{
						contractSet.insert(contract->getFullAltCode());
						continue;
					}

					auto it1 = _filters.find(contract->getExchg());
					auto it2 = _filters.find(contract->getCode());
					auto it3 = _filters.find(contract->getFullCode());
					auto it4 = _filters.find(contract->getFullPid());
					auto it5 = _filters.find(contract->getAltCode());
					auto it6 = _filters.find(contract->getFullAltCode());
					if (it1 != _filters.end() || it2 != _filters.end()
						|| it3 != _filters.end() || it4 != _filters.end()
						|| it5 != _filters.end() || it6 != _filters.end())
						contractSet.insert(contract->getFullAltCode());
				}
				contracts->release();
			}
		}

		if (_stopped.load()) return false;
		_parser_api->subscribe(contractSet);
		_initialized = true;
	}
	catch (...)
	{
		// Do not destroy a failed candidate here: provider release is not
		// documented as a callback-drain barrier.
		_stopped = true;
		WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initialization threw an exception", _id.c_str());
		return false;
	}

	WTSLogger::log_dyn("parser", _id.c_str(), LL_INFO, "[{}] Parser initialzied, check_time: {}", _id.c_str(), _check_time);

	return isInitialized();
}

bool ParserAdapter::requestStopLifecycle() noexcept
{
    _stopped.store(true);
    _initialized.store(false);
    try {
        std::unique_lock<std::recursive_mutex> lock(_lifecycle_mutex, std::try_to_lock);
        if (!lock.owns_lock() || _operation_active || _release_uncertain || _api_creation_uncertain) return false;
        LifecycleOperation operation(_operation_active);
        if (!_parser_api)
            return _rejected_external_apis.empty() && _contended_external_apis.load() == NULL;
        if (!_lifecycle)
        {
            // Decision K15=A (2026-11-04): an initExt API belongs to its caller and has no V2
            // drain proof. An explicit release notifies it to stop exactly once, which is not
            // treated as quiescence: the host keeps the API (never destroys it). A release
            // re-entered from that callback, or issued by another thread meanwhile, returns
            // immediately through the operation guard / try-lock above.
            if (!_external_stop_notified)
            {
                _external_stop_notified = true;
                try { _parser_api->release(); }
                catch (...) { _release_uncertain = true; }
            }
            return false;
        }
        if (_stop_accepted) return true;
        WtParserLifecycleV2Status status;
        try { status = _lifecycle->request_stop(static_cast<void*>(_parser_api)); }
        catch (...) { _release_uncertain = true; return false; }
        if (status == WT_PARSER_LIFECYCLE_V2_OK) { _stop_accepted = true; return true; }
        if (status != WT_PARSER_LIFECYCLE_V2_PENDING && status != WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK)
            _release_uncertain = true;
        return false;
    } catch (...) { return false; }
}

bool ParserAdapter::finishReleaseLifecycle() noexcept
{
    try {
        std::unique_lock<std::recursive_mutex> lock(_lifecycle_mutex, std::try_to_lock);
        if (!lock.owns_lock() || _operation_active || _release_uncertain || _api_creation_uncertain) return false;
        LifecycleOperation operation(_operation_active);
        if (_parser_api) {
            if (!_lifecycle || !_stop_accepted) return false;
            WtParserLifecycleV2Status status;
            try { status = _lifecycle->can_wait(static_cast<void*>(_parser_api)); }
            catch (...) { _release_uncertain = true; return false; }
            if (status != WT_PARSER_LIFECYCLE_V2_OK) {
                if (status != WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK && status != WT_PARSER_LIFECYCLE_V2_PENDING)
                    _release_uncertain = true;
                return false;
            }
            try { status = _lifecycle->wait_drained(static_cast<void*>(_parser_api), 0); }
            catch (...) { _release_uncertain = true; return false; }
            if (status != WT_PARSER_LIFECYCLE_V2_DRAINED) {
                if (status != WT_PARSER_LIFECYCLE_V2_PENDING && status != WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK)
                    _release_uncertain = true;
                return false;
            }
            IParserApi* original = _parser_api;
            void* opaque = static_cast<void*>(original);
            try { status = _lifecycle->destroy_api(&opaque); }
            catch (...) { _release_uncertain = true; return false; }
            if (status != WT_PARSER_LIFECYCLE_V2_OK || opaque != NULL) {
                _release_uncertain = true; // Preserve the original handle, even after an ambiguous callback.
                return false;
            }
            _parser_api = NULL;
        }
        if (!_rejected_external_apis.empty() || _contended_external_apis.load() != NULL) return false;
        _lifecycle = NULL;
        _remover = NULL;
        _remover_pending = false;
        _stop_accepted = false;
        if (_module_handle) { DLLHelper::free_library(_module_handle); _module_handle = NULL; }
        if (_cfg) { _cfg->release(); _cfg = NULL; }
        _filters.clear();
        return true;
    } catch (...) { return false; }
}

void ParserAdapter::release()
{
    if (requestStopLifecycle()) (void)finishReleaseLifecycle();
}

bool ParserAdapter::run()
{
	std::unique_lock<std::recursive_mutex> lock(_lifecycle_mutex, std::try_to_lock);
	if (!lock.owns_lock() || _operation_active) return false;
	LifecycleOperation operation(_operation_active);
	if (_parser_api == NULL || !_initialized || _stopped)
		return false;

	return _parser_api->connect();
}

//合理毫秒数时间差
const int RESONABLE_MILLISECS = 60 * 60 * 1000;
void ParserAdapter::handleQuote(WTSTickData *quote, uint32_t /*procFlag*/)
{
	if (quote == NULL || _stopped || quote->actiondate() == 0 || quote->tradingdate() == 0)
		return;

	if (!_filters.empty() && (_filters.find(quote->exchg()) == _filters.end()))
		return;

	WTSContractInfo* cInfo = quote->getContractInfo();
	if (cInfo == NULL)
	{
		cInfo = _bd_mgr->getContract(quote->code(), quote->exchg());
		quote->setContractInfo(cInfo);
	}

	if (cInfo == NULL)
		return;

	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	[[maybe_unused]] WTSSessionInfo* sInfo = commInfo->getSessionInfo();

	if (_check_time)
	{
		int64_t tick_time = TimeUtils::makeTime(quote->actiondate(), quote->actiontime());
		int64_t local_time = TimeUtils::getLocalTimeNow();

		/*
		 *	By Wesley @ 2022.04.20
		 *	如果最新的tick时间，和本地时间相差太大
		 *	则认为tick的时间戳是错误的
		 *	这里要求本地时间是要时常进行校准的
		 */
		if (tick_time - local_time > RESONABLE_MILLISECS)
		{
			WTSLogger::warn("Tick of {} with wrong timestamp {}.{} received, skipped", cInfo->getFullCode(), quote->actiondate(), quote->actiontime());
			return;
		}
	}

	std::string stdCode;
	const ContractCategory category = commInfo->getCategoty();
	const char* exchange = cInfo->getExchg();
	const bool cffexSpotOption = category == CC_SpotOption
		&& exchange != NULL && std::strcmp(exchange, "CFFEX") == 0;
	if (category == CC_FutOption || cffexSpotOption)
	{
		// rawFutOptCodeToStdCode assumes valid C/P fields and can underflow on
		// malformed input. Validate the supported formats before calling it.
		if (!isFuturesOptionCode(cInfo->getCode(), exchange)
			|| (cffexSpotOption && !isHyphenatedFuturesOptionCode(cInfo->getCode())))
			return;

		stdCode = CodeHelper::rawFutOptCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
	}
	else if(CodeHelper::isMonthlyCode(quote->code()))
	{
		//如果是分月合约，则进行主力和次主力的判断
		stdCode = CodeHelper::rawMonthCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
		if (stdCode.empty())
			return;
	}
	else
	{
		stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), cInfo->getProduct());
	}
	quote->setCode(stdCode.c_str());

	_stub->handle_push_quote(quote);
}

void ParserAdapter::handleOrderQueue(WTSOrdQueData* ordQueData)
{
	if (_stopped)
		return;

	if (!_filters.empty() && (_filters.find(ordQueData->exchg()) == _filters.end()))
		return;

	if (ordQueData->actiondate() == 0 || ordQueData->tradingdate() == 0)
		return;

	WTSContractInfo* cInfo = _bd_mgr->getContract(ordQueData->code(), ordQueData->exchg());
	if (cInfo == NULL)
		return;

	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	std::string stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), commInfo->getProduct());
	ordQueData->setCode(stdCode.c_str());

	if (_stub)
		_stub->handle_push_order_queue(ordQueData);
}

void ParserAdapter::handleOrderDetail(WTSOrdDtlData* ordDtlData)
{
	if (_stopped)
		return;

	if (!_filters.empty() && (_filters.find(ordDtlData->exchg()) == _filters.end()))
		return;

	if (ordDtlData->actiondate() == 0 || ordDtlData->tradingdate() == 0)
		return;

	WTSContractInfo* cInfo = _bd_mgr->getContract(ordDtlData->code(), ordDtlData->exchg());
	if (cInfo == NULL)
		return;

	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	std::string stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), commInfo->getProduct());
	ordDtlData->setCode(stdCode.c_str());

	if (_stub)
		_stub->handle_push_order_detail(ordDtlData);
}

void ParserAdapter::handleTransaction(WTSTransData* transData)
{
	if (_stopped)
		return;

	if (!_filters.empty() && (_filters.find(transData->exchg()) == _filters.end()))
		return;

	if (transData->actiondate() == 0 || transData->tradingdate() == 0)
		return;

	WTSContractInfo* cInfo = _bd_mgr->getContract(transData->code(), transData->exchg());
	if (cInfo == NULL)
		return;

	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	std::string stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), commInfo->getProduct());
	transData->setCode(stdCode.c_str());

	if (_stub)
		_stub->handle_push_transaction(transData);
}


void ParserAdapter::handleParserLog(WTSLogLevel ll, const char* message)
{
	if (_stopped)
		return;

	WTSLogger::log_dyn_raw("parser", _id.c_str(), ll, message);
}


//////////////////////////////////////////////////////////////////////////
//ParserAdapterMgr
void ParserAdapterMgr::release()
{
	(void)requestStopLifecycleAll();
	(void)finishReleaseLifecycleAll();
}

bool ParserAdapterMgr::requestStopLifecycleAll() noexcept
{
	bool allStopped = true;
	for (auto& entry : _adapters)
	{
		if (entry.second && !entry.second->requestStopLifecycle()) allStopped = false;
	}
	return allStopped;
}

bool ParserAdapterMgr::finishReleaseLifecycleAll() noexcept
{
	bool allReleased = true;
	for (ParserAdapterMap::iterator it = _adapters.begin(); it != _adapters.end();)
	{
		ParserAdapterPtr adapter = it->second;
		if (adapter && (!adapter->finishReleaseLifecycle() || adapter->hasApi()))
		{
			allReleased = false;
			++it;
		}
		else
			it = _adapters.erase(it);
	}
	return allReleased;
}

bool ParserAdapterMgr::hasQuarantinedAdapters() const noexcept
{
	for (ParserAdapterMap::const_iterator it = _adapters.begin(); it != _adapters.end(); ++it)
	{
		if (it->second && it->second->hasApi())
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
	(void)runWithResult();
}

bool ParserAdapterMgr::runWithResult()
{
	bool allSubmitted = true;
	for (auto it = _adapters.begin(); it != _adapters.end(); it++)
	{
		if (!it->second || !it->second->run())
		{
			allSubmitted = false;
			WTSLogger::error("Parser '{}' connect command was not submitted", it->first.c_str());
		}
	}

	if (allSubmitted)
		WTSLogger::info("Connect commands submitted for {} parsers", _adapters.size());
	return allSubmitted;
}
