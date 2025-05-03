/*!
 * \file ParserAdapter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 行情解析模块适配器实现
 * 
 * \details 本文件实现了行情解析器的适配器类，用于封装不同的行情解析器并提供统一的接口。
 * 包括行情解析器的初始化、运行、数据处理等功能的实现。
 * 同时实现了适配器管理器，用于管理多个行情解析器适配器实例。
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


//////////////////////////////////////////////////////////////////////////
//ParserAdapter
/**
 * @brief 行情解析器适配器类构造函数
 * @param bgMgr 基础数据管理器指针，用于管理合约、交易所等基础数据
 * @param runner 数据服务运行器指针，用于处理接收到的行情数据
 * 
 * @details 初始化适配器的成员变量，包括解析器API、删除函数、停止标志、基础数据管理器和数据服务运行器
 */
ParserAdapter::ParserAdapter(WTSBaseDataMgr * bgMgr, WtDtRunner* runner)
	: _parser_api(NULL)
	, _remover(NULL)
	, _stopped(false)
	, _bd_mgr(bgMgr)
	, _dt_runner(runner)
	, _cfg(NULL)
{
}

/**
 * @brief 行情解析器适配器类析构函数
 * 
 * @details 析构函数中不需要手动释放资源，因为资源的释放在release方法中实现
 */
ParserAdapter::~ParserAdapter()
{
}

/**
 * @brief 使用外部提供的解析器API初始化适配器
 * @param id 适配器标识符
 * @param api 外部提供的解析器API指针
 * @return bool 初始化是否成功
 * 
 * @details 该方法用于使用外部提供的解析器API初始化适配器，而不是通过配置加载动态库创建。
 * 初始化过程包括注册回调接口、初始化API和订阅所有合约的行情数据。
 */
bool ParserAdapter::initExt(const char* id, IParserApi* api)
{
	if (api == NULL)
		return false;

	_parser_api = api;
	_id = id;

	if (_parser_api)
	{
		_parser_api->registerSpi(this);

		if (_parser_api->init(NULL))
		{
			ContractSet contractSet;
			WTSArray* ayContract = _bd_mgr->getContracts();
			WTSArray::Iterator it = ayContract->begin();
			for (; it != ayContract->end(); it++)
			{
				WTSContractInfo* contract = STATIC_CONVERT(*it, WTSContractInfo*);
				contractSet.insert(contract->getFullCode());
			}

			ayContract->release();

			_parser_api->subscribe(contractSet);
			contractSet.clear();
		}
		else
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed: api initializing failed...", _id.c_str());
		}
	}

	return true;
}


/**
 * @brief 根据配置初始化解析器适配器
 * @param id 适配器标识符
 * @param cfg 配置项
 * @return bool 初始化是否成功
 * 
 * @details 该方法根据配置项加载解析器动态库并创建解析器API实例，
 * 然后设置交易所和合约过滤器，并初始化解析器API。
 * 最后根据过滤器设置订阅的合约列表。
 */
bool ParserAdapter::init(const char* id, WTSVariant* cfg)
{
	if (cfg == NULL)
		return false;

	_id = id;

	if (_cfg != NULL)
		return false;

	_cfg = cfg;
	_cfg->retain();

	{
		//加载模块
		if (cfg->getString("module").empty())
			return false;

		std::string module = DLLHelper::wrap_module(cfg->getCString("module"), "lib");;

		if (!StdFile::exists(module.c_str()))
		{
			module = WtHelper::get_module_dir();
			module += "parsers/";
			module += DLLHelper::wrap_module(cfg->getCString("module"), "lib");
		}

		DllHandle hInst = DLLHelper::load_library(module.c_str());
		if (hInst == NULL)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser module {} loading failed", _id.c_str(), module.c_str());
			return false;
		}
		else
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_INFO, "[{}] Parser module {} loaded", _id.c_str(), module.c_str());
		}

		FuncCreateParser pFuncCreateParser = (FuncCreateParser)DLLHelper::get_symbol(hInst, "createParser");
		if (NULL == pFuncCreateParser)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Entrance function createParser not found", _id.c_str());
			return false;
		}

		_parser_api = pFuncCreateParser();
		if (NULL == _parser_api)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_FATAL, "[{}] Creating parser api failed", _id.c_str());
			return false;
		}

		_remover = (FuncDeleteParser)DLLHelper::get_symbol(hInst, "deleteParser");
	}


	const std::string& strFilter = cfg->getString("filter");
	if (!strFilter.empty())
	{
		const StringVector &ayFilter = StrUtil::split(strFilter, ",");
		auto it = ayFilter.begin();
		for (; it != ayFilter.end(); it++)
		{
			_exchg_filter.insert(*it);
		}
	}

	std::string strCodes = cfg->getString("code");
	if (!strCodes.empty())
	{
		const StringVector &ayCodes = StrUtil::split(strCodes, ",");
		auto it = ayCodes.begin();
		for (; it != ayCodes.end(); it++)
		{
			_code_filter.insert(*it);
		}
	}

	if (_parser_api)
	{
		_parser_api->registerSpi(this);

		if (_parser_api->init(cfg))
		{
			ContractSet contractSet;
			if (!_code_filter.empty())//优先判断合约过滤器
			{
				ExchgFilter::iterator it = _code_filter.begin();
				for (; it != _code_filter.end(); it++)
				{
					//全代码,形式如SSE.600000,期货代码为CFFEX.IF2005
					std::string code, exchg;
					auto ay = StrUtil::split((*it).c_str(), ".");
					if (ay.size() == 1)
						code = ay[0];
					else if (ay.size() == 2)
					{
						exchg = ay[0];
						code = ay[1];
					}
					else if (ay.size() == 3)
					{
						exchg = ay[0];
						code = ay[2];
					}
					WTSContractInfo* contract = _bd_mgr->getContract(code.c_str(), exchg.c_str());
					if (contract)
						contractSet.insert(contract->getFullCode());
					else
					{
						//如果是品种ID，则将该品种下全部合约都加到订阅列表
						WTSCommodityInfo* commInfo = _bd_mgr->getCommodity(exchg.c_str(), code.c_str());
						if (commInfo)
						{
							const auto& codes = commInfo->getCodes();
							for (const auto& c : codes)
							{
								contractSet.insert(fmt::format("{}.{}", exchg, c.c_str()));
							}
						}
					}
				}
			}
			else if (!_exchg_filter.empty())
			{
				ExchgFilter::iterator it = _exchg_filter.begin();
				for (; it != _exchg_filter.end(); it++)
				{
					const char* exchg = (*it).c_str();
					WTSArray* ayContract = _bd_mgr->getContracts(exchg);
					auto cnt = ayContract->size();
					WTSArray::Iterator it = ayContract->begin();
					for (; it != ayContract->end(); it++)
					{
						WTSContractInfo* contract = STATIC_CONVERT(*it, WTSContractInfo*);
						contractSet.insert(contract->getFullCode());
					}

					ayContract->release();

					WTSLogger::log_dyn("parser", _id.c_str(), LL_INFO, "[{}] {} contracts of {} added to sublist...", _id.c_str(), cnt, exchg);
				}
			}
			else
			{
				WTSArray* ayContract = _bd_mgr->getContracts();
				WTSArray::Iterator it = ayContract->begin();
				for (; it != ayContract->end(); it++)
				{
					WTSContractInfo* contract = STATIC_CONVERT(*it, WTSContractInfo*);
					contractSet.insert(contract->getFullCode());
				}

				ayContract->release();
			}

			_parser_api->subscribe(contractSet);
			contractSet.clear();
		}
		else
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed: api initializing failed...", _id.c_str());
		}
	}
	else
	{
		WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser initializing failed: creating api failed...", _id.c_str());
	}

	return true;
}

/**
 * @brief 释放适配器资源
 * 
 * @details 设置停止标志，释放解析器API资源，
 * 如果有删除函数则使用删除函数删除解析器API，否则直接删除。
 */
void ParserAdapter::release()
{
	_stopped = true;
	if (_parser_api)
	{
		_parser_api->release();
	}

	if (_remover)
		_remover(_parser_api);
	else
		delete _parser_api;
}

/**
 * @brief 启动解析器
 * @return bool 启动是否成功
 * 
 * @details 检查解析器API是否存在，如果存在则调用其connect方法连接行情源并开始接收行情数据。
 */
bool ParserAdapter::run()
{
	if (_parser_api == NULL)
		return false;

	_parser_api->connect();
	return true;
}

/**
 * @brief 处理合约列表
 * @param aySymbols 合约列表数组
 * 
 * @details 实现IParserSpi接口的合约列表处理方法，当前实现为空，可以根据需要扩展。
 */
void ParserAdapter::handleSymbolList( const WTSArray* aySymbols )
{
	
}

/**
 * @brief 处理成交数据
 * @param transData 成交数据指针
 * 
 * @details 实现IParserSpi接口的成交数据处理方法，接收并处理行情解析器返回的成交数据。
 * 首先检查是否已停止、数据是否有效，然后检查合约是否存在。
 * 当前实现只进行了基本的数据检查，可以根据需要扩展处理逻辑。
 */
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

/**
 * @brief 处理委托明细数据
 * @param ordDetailData 委托明细数据指针
 * 
 * @details 实现IParserSpi接口的委托明细数据处理方法，接收并处理行情解析器返回的委托明细数据。
 * 首先检查是否已停止、数据是否有效，然后检查合约是否存在。
 * 当前实现只进行了基本的数据检查，可以根据需要扩展处理逻辑。
 */
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

/**
 * @brief 处理委托队列数据
 * @param ordQueData 委托队列数据指针
 * 
 * @details 实现IParserSpi接口的委托队列数据处理方法，接收并处理行情解析器返回的委托队列数据。
 * 首先检查是否已停止、数据是否有效，然后检查合约是否存在。
 * 当前实现只进行了基本的数据检查，可以根据需要扩展处理逻辑。
 */
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

/**
 * @brief 处理Tick行情数据
 * @param quote Tick数据指针
 * @param procFlag 处理标志
 * 
 * @details 实现IParserSpi接口的Tick数据处理方法，接收并处理行情解析器返回的Tick数据。
 * 首先检查数据指针是否为空、是否已停止、数据是否有效。
 * 然后将数据转发给数据服务运行器进行处理。
 */
void ParserAdapter::handleQuote( WTSTickData *quote, uint32_t procFlag )
{
	if (quote == NULL || _stopped || quote->actiondate() == 0 || quote->tradingdate() == 0)
		return;

	if (_dt_runner)
		_dt_runner->proc_tick(quote);
}

/**
 * @brief 处理解析器日志
 * @param ll 日志级别
 * @param message 日志消息
 * 
 * @details 实现IParserSpi接口的日志处理方法，接收并处理行情解析器返回的日志信息。
 * 首先检查是否已停止，然后将日志转发给日志系统进行记录。
 */
void ParserAdapter::handleParserLog( WTSLogLevel ll, const char* message)
{
	if (_stopped)
		return;

	WTSLogger::log_raw_by_cat("parser", ll, message);
}

/**
 * @brief 获取基础数据管理器
 * @return IBaseDataMgr* 基础数据管理器指针
 * 
 * @details 实现IParserSpi接口的获取基础数据管理器方法，返回内部保存的基础数据管理器指针。
 */
IBaseDataMgr* ParserAdapter::getBaseDataMgr()
{
	return _bd_mgr;
}


//////////////////////////////////////////////////////////////////////////
//ParserAdapterMgr
/**
 * @brief 释放所有适配器资源
 * 
 * @details 遍历并释放所有已注册的行情解析器适配器资源，然后清空适配器容器。
 */
void ParserAdapterMgr::release()
{
	for (auto it = _adapters.begin(); it != _adapters.end(); it++)
	{
		it->second->release();
	}

	_adapters.clear();
}

/**
 * @brief 添加适配器到管理器
 * @param id 适配器标识符
 * @param adapter 适配器指针
 * @return bool 添加是否成功
 * 
 * @details 将指定的行情解析器适配器添加到管理器中，以标识符为键。
 * 首先检查适配器指针是否为空、标识符是否有效，然后检查是否已存在相同标识符的适配器。
 * 如果没有重名，则添加到容器中并返回true。
 */
bool ParserAdapterMgr::addAdapter(const char* id, ParserAdapterPtr& adapter)
{
	if (adapter == NULL || strlen(id) == 0)
		return false;

	auto it = _adapters.find(id);
	if (it != _adapters.end())
	{
		WTSLogger::error(" Same name of parsers: %s", id);
		return false;
	}

	_adapters[id] = adapter;

	return true;
}

/**
 * @brief 获取指定标识符的适配器
 * @param id 适配器标识符
 * @return ParserAdapterPtr 适配器指针，如果不存在则返回空指针
 * 
 * @details 根据标识符从管理器中查找并返回相应的行情解析器适配器。
 * 如果找到则返回对应的适配器指针，否则返回空指针。
 */
ParserAdapterPtr ParserAdapterMgr::getAdapter(const char* id)
{
	auto it = _adapters.find(id);
	if (it != _adapters.end())
	{
		return it->second;
	}

	return ParserAdapterPtr();
}

/**
 * @brief 启动所有适配器
 * 
 * @details 遍历并启动所有已注册的行情解析器适配器，然后输出启动的适配器数量日志。
 */
void ParserAdapterMgr::run()
{
	for (auto it = _adapters.begin(); it != _adapters.end(); it++)
	{
		it->second->run();
	}

	WTSLogger::info("{} parsers started", _adapters.size());
}