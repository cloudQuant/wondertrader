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

USING_NS_WTP;

namespace
{
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
	, _remover(NULL)
	, _module_handle(NULL)
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

	if (hasApi() || _cfg != NULL || _module_handle != NULL)
	{
		// A non-null initExt argument transfers ownership even on this busy
		// path. Keep a distinct candidate without replacing or stopping the
		// adapter's currently registered API.
		retainRejectedExternalApi(api);
		return false;
	}

	_parser_api = api;
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

	return _initialized;
}

bool ParserAdapter::init(const char* id, WTSVariant* cfg, IParserStub* stub, IBaseDataMgr* bgMgr, IHotMgr* hotMgr/* = NULL*/)
{
	if (cfg == NULL || bgMgr == NULL || hasApi() || _cfg != NULL || _module_handle != NULL)
		return false;

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

	auto failWithoutApi = [this]() {
		if (_module_handle != NULL)
		{
			DLLHelper::free_library(_module_handle);
			_module_handle = NULL;
		}
		_remover = NULL;
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

		_remover = (FuncDeleteParser)DLLHelper::get_symbol(_module_handle, "deleteParser");
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

	return _initialized;
}

void ParserAdapter::release()
{
	_stopped = true;
	_initialized = false;
	if (_release_uncertain)
		return;
	if (_api_creation_uncertain)
	{
		// No handle exists to release, and the factory may have left hidden
		// work or a callback registered before returning/throwing. Preserve the
		// module, config and adapter's borrowed dependency graph for quarantine.
		return;
	}
	if (_parser_api)
	{
		IParserApi* api_being_released = _parser_api;
		if (_remover_pending && _remover == NULL)
		{
			// A pending provider remover cannot safely be replaced with delete.
			_release_uncertain = true;
			return;
		}
		try
		{
			// IParserApi::release() may not be idempotent. Once a remover leaves
			// the API pointer non-null, retry only the remover on later calls.
			if (!_remover_pending)
				_parser_api->release();
			if (_remover)
				_remover(_parser_api);
			else
			{
				delete _parser_api;
				_parser_api = NULL;
			}
		}
		catch (...)
		{
			// release() may have partially changed provider state; a remover may
			// also have partially destroyed the object. Preserve all remaining
			// ownership context and never retry this teardown implicitly. Keep
			// the original pointer value even if a throwing remover nulled it;
			// the sticky flag prevents any future dereference or deletion.
			_parser_api = api_being_released;
			_release_uncertain = true;
			return;
		}
		if (_remover != NULL && _parser_api != NULL)
		{
			if (_parser_api == api_being_released)
			{
				// The remover's by-reference pointer is its completion signal. A
				// non-null result means it deferred destruction; retain the API,
				// remover, module, config and callback dependencies for a later
				// retry after the caller establishes a safe provider barrier.
				_remover_pending = true;
				return;
			}

			// A remover is specified to null the pointer on success or preserve
			// the same pointer when it defers. A replacement pointer is ambiguous.
			_release_uncertain = true;
			return;
		}
		_parser_api = NULL;
		_remover_pending = false;
	}
	for (std::vector<IParserApi*>::iterator it = _rejected_external_apis.begin();
		it != _rejected_external_apis.end();)
	{
		IParserApi* api = *it;
		try
		{
			api->release();
		}
		catch (...)
		{
			// Preserve this owner and its remaining candidates if a provider
			// cannot complete its teardown. Never retry a partial release.
			_release_uncertain = true;
			return;
		}
		delete api;
		it = _rejected_external_apis.erase(it);
	}
	_remover = NULL;
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
	for (ParserAdapterMap::iterator it = _adapters.begin(); it != _adapters.end();)
	{
		ParserAdapterPtr adapter = it->second;
		if (adapter)
			adapter->release();

		if (adapter && adapter->hasApi())
			++it;
		else
			it = _adapters.erase(it);
	}
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
	return true; // Simulate the historical discarded manager result.
}
