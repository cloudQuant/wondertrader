/*!
 * \file ParserAdapter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 行情解析模块适配器实现
 * \details 实现了ParserAdapter和ParserAdapterMgr类，用于封装和管理第三方行情解析模块
 */
#include "ParserAdapter.h"
#include "DataManager.h"
#include "StateMonitor.h"
#include "WtHelper.h"
#include "IndexFactory.h"

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
/**
 * @brief 构造函数
 * @details 初始化ParserAdapter对象
 * @param bgMgr 基础数据管理器指针，用于获取合约信息
 * @param dtMgr 数据管理器指针，用于存储行情数据
 * @param idxFactory 指数工厂指针，用于处理行情数据生成指数
 */
ParserAdapter::ParserAdapter(WTSBaseDataMgr * bgMgr, DataManager* dtMgr, IndexFactory *idxFactory)
	: _parser_api(NULL)
	, _remover(NULL)
	, _stopped(false)
	, _bd_mgr(bgMgr)
	, _dt_mgr(dtMgr)
	, _idx_fact(idxFactory)
	, _cfg(NULL)
{
}


/**
 * @brief 析构函数
 * @details 释放适配器相关资源
 */
ParserAdapter::~ParserAdapter()
{
}

/**
 * @brief 使用外部提供的API初始化适配器
 * @details 外部创建的行情解析API对象的初始化方式，负责注册回调接口并订阅合约
 * @param id 适配器唯一标识
 * @param api 外部提供的行情解析API对象
 * @return bool 初始化是否成功
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
 * @brief 根据配置初始化适配器
 * @details 加载解析模块，创建API对象，并订阅合约
 *          支持通过交易所过滤器和合约过滤器进行筛选
 * @param id 适配器唯一标识
 * @param cfg 适配器配置对象
 * @return bool 初始化是否成功
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
 * @details 标记适配器为停止状态，释放解析API资源
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
 * @brief 启动适配器
 * @details 调用解析API的connect方法连接行情源
 * @return bool 启动是否成功
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
 * @details 处理行情解析API返回的合约列表
 * @param aySymbols 合约列表数组
 */
void ParserAdapter::handleSymbolList( const WTSArray* aySymbols )
{
	
}

/**
 * @brief 处理逆向成交数据
 * @details 接收行情解析API返回的成交数据，并存储到数据管理器中
 * @param transData 逆向成交数据指针
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

	transData->setContractInfo(contract);

	_dt_mgr->writeTransaction(transData);
}

/**
 * @brief 处理逆向逆向委托明细数据
 * @details 接收行情解析API返回的委托明细数据，并存储到数据管理器中
 * @param ordDetailData 委托明细数据指针
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

	ordDetailData->setContractInfo(contract);

	_dt_mgr->writeOrderDetail(ordDetailData);
}

/**
 * @brief 处理委托队列数据
 * @details 接收行情解析API返回的委托队列数据，并存储到数据管理器中
 * @param ordQueData 委托队列数据指针
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

	ordQueData->setContractInfo(contract);
		
	_dt_mgr->writeOrderQueue(ordQueData);
}

/**
 * @brief 处理行情数据
 * @details 接收行情解析API返回的Tick数据，存储到数据管理器中，并传递给指数工厂处理
 * @param quote Tick数据指针
 * @param procFlag 处理标记，用于指示如何处理数据
 */
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

/**
 * @brief 处理解析器日志
 * @details 接收行情解析API输出的日志信息，并转发到日志系统
 * @param ll 日志级别
 * @param message 日志消息内容
 */
void ParserAdapter::handleParserLog( WTSLogLevel ll, const char* message)
{
	if (_stopped)
		return;

	WTSLogger::log_raw_by_cat("parser", ll, message);
}

/**
 * @brief 获取基础数据管理器
 * @details 实现IParserSpi接口中的方法，返回适配器的基础数据管理器
 * @return IBaseDataMgr* 基础数据管理器指针
 */
IBaseDataMgr* ParserAdapter::getBaseDataMgr()
{
	return _bd_mgr;
}


//////////////////////////////////////////////////////////////////////////
//ParserAdapterMgr
/**
 * @brief 释放所有适配器资源
 * @details 遍历并释放管理器中的所有解析适配器实例
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
 * @brief 添加解析适配器
 * @details 将解析适配器实例添加到管理器中，不允许重复的ID
 * @param id 适配器唯一标识
 * @param adapter 适配器智能指针引用
 * @return bool 添加是否成功
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
 * @brief 获取指定的解析适配器
 * @details 根据适配器ID从管理器中获取相应的适配器实例
 * @param id 适配器唯一标识
 * @return ParserAdapterPtr 适配器智能指针，如果不存在则返回空指针
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
 * @brief 运行所有解析适配器
 * @details 遍历并启动所有适配器实例，并输出日志信息
 */
void ParserAdapterMgr::run()
{
	for (auto it = _adapters.begin(); it != _adapters.end(); it++)
	{
		it->second->run();
	}

	WTSLogger::info("{} parsers started", _adapters.size());
}