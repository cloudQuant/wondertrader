/*!
 * \file ParserAdapter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 行情解析器适配器实现
 * 
 * 本文件实现了行情解析器的适配器类，将各种行情源的数据转换为统一格式，
 * 并通过定义的回调接口将数据传递给交易系统的其他模块。
 * 实现了对快照、委托队列、逐笔委托和逐笔成交等数据的处理。
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

USING_NS_WTP;

//////////////////////////////////////////////////////////////////////////
//ParserAdapter
/**
 * @brief 行情解析器适配器构造函数
 * 
 * 初始化行情解析器适配器对象，设置初始属性值
 */
ParserAdapter::ParserAdapter()
	: _parser_api(NULL)    // 初始化行情解析器API指针
	, _remover(NULL)      // 初始化删除器函数指针
	, _stopped(false)     // 初始化停止标志
	, _bd_mgr(NULL)       // 初始化基础数据管理器指针
	, _stub(NULL)         // 初始化回调接口指针
	, _cfg(NULL)          // 初始化配置对象指针
{
	// 构造函数体为空，所有初始化工作已通过初始化列表完成
}


/**
 * @brief 行情解析器适配器析构函数
 * 
 * 清理并释放适配器资源
 */
ParserAdapter::~ParserAdapter()
{
	// 析构函数体为空，实际的资源释放在release方法中实现
}

/**
 * @brief 初始化行情解析器适配器
 * 
 * 根据配置加载行情解析器模块，并进行初始化、过滤器设置和订阅合约
 * 
 * @param id 适配器标识符
 * @param cfg 适配器配置
 * @param stub 行情回调接口
 * @param bgMgr 基础数据管理器
 * @return bool 初始化是否成功
 */
bool ParserAdapter::init(const char* id, WTSVariant* cfg, IParserStub* stub, IBaseDataMgr* bgMgr)
{
	if (cfg == NULL)
		return false;

	_stub = stub;
	_bd_mgr = bgMgr;
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

		//先看工作目录下是否有交易模块
		std::string dllpath = WtHelper::getModulePath(module.c_str(), "parsers", true);
		//如果没有,则再看模块目录,即dll同目录下
		if (!StdFile::exists(dllpath.c_str()))
			dllpath = WtHelper::getModulePath(module.c_str(), "parsers", false);

		DllHandle hInst = DLLHelper::load_library(dllpath.c_str());
		if (hInst == NULL)
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_ERROR, "[{}] Parser module {} loading failed", _id.c_str(), dllpath.c_str());
			return false;
		}
		else
		{
			WTSLogger::log_dyn("parser", _id.c_str(), LL_INFO, "[{}] Parser module {} loaded", _id.c_str(), dllpath.c_str());
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
			WTSArray* ay = _bd_mgr->getContracts();
			for(auto it = ay->begin(); it != ay->end(); it++)
			{
				WTSContractInfo* cInfo = STATIC_CONVERT(*it, WTSContractInfo*);

				//先检查合约和品种是否符合条件
				if(!_code_filter.empty())
				{
					auto cit = _code_filter.find(cInfo->getFullCode());
					auto pit = _code_filter.find(cInfo->getFullPid());
					if (cit != _code_filter.end() || pit != _code_filter.end())
					{
						contractSet.insert(cInfo->getFullCode());
						continue;
					}
				}
				
				//再检查交易所是否符合条件
				if (!_exchg_filter.empty())
				{
					auto eit = _exchg_filter.find(cInfo->getExchg());
					if (eit != _exchg_filter.end())
					{
						contractSet.insert(cInfo->getFullCode());
						continue;
					}
					else
					{
						continue;
					}
				}

				if(_code_filter.empty() && _exchg_filter.empty())
					contractSet.insert(cInfo->getFullCode());

			}
			ay->release();

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
 * @brief 使用外部API初始化行情解析器适配器
 * 
 * 使用外部提供的行情解析器API初始化适配器，设置合约过滤条件并订阅合约
 * 
 * @param id 适配器标识符
 * @param api 外部行情解析器API对象
 * @param stub 行情回调接口
 * @param bgMgr 基础数据管理器
 * @return bool 初始化是否成功
 */
bool ParserAdapter::initExt(const char* id, IParserApi* api, IParserStub* stub, IBaseDataMgr* bgMgr)
{
	if (api == NULL)
		return false;

	_parser_api = api;
	_stub = stub;
	_bd_mgr = bgMgr;
	_id = id;

	if (_parser_api)
	{
		_parser_api->registerSpi(this);

		if (_parser_api->init(NULL))
		{
			ContractSet contractSet;
			WTSArray* ay = _bd_mgr->getContracts();
			for (auto it = ay->begin(); it != ay->end(); it++)
			{
				WTSContractInfo* cInfo = STATIC_CONVERT(*it, WTSContractInfo*);

				//先检查合约和品种是否符合条件
				if (!_code_filter.empty())
				{
					auto cit = _code_filter.find(cInfo->getFullCode());
					auto pit = _code_filter.find(cInfo->getFullPid());
					if (cit != _code_filter.end() || pit != _code_filter.end())
					{
						contractSet.insert(cInfo->getFullCode());
						continue;
					}
				}

				//再检查交易所是否符合条件
				if (!_exchg_filter.empty())
				{
					auto eit = _exchg_filter.find(cInfo->getExchg());
					if (eit != _code_filter.end())
					{
						contractSet.insert(cInfo->getFullCode());
						continue;
					}
					else
					{
						continue;
					}
				}

				if (_code_filter.empty() && _exchg_filter.empty())
					contractSet.insert(cInfo->getFullCode());

			}
			ay->release();

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
 * @brief 释放适配器资源
 * 
 * 清理并释放适配器使用的资源，包括停止行情解析器和释放API对象
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
 * @brief 运行行情适配器
 * 
 * 连接行情源并开始接收行情数据
 * 
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
 * @brief 处理实时行情数据
 * 
 * 接收并处理行情源提供的实时快照数据，进行必要的检查和数据格式转换后转发给回调接口
 * 
 * @param quote 实时行情数据
 * @param procFlag 处理标志
 */
void ParserAdapter::handleQuote(WTSTickData *quote, uint32_t procFlag)
{
	if (quote == NULL || _stopped || quote->actiondate() == 0)
		return;

	WTSContractInfo* cInfo = quote->getContractInfo();
	if (cInfo == NULL) cInfo = _bd_mgr->getContract(quote->code(), quote->exchg());
	if (cInfo == NULL)
		return;

	quote->setCode(cInfo->getFullCode());

	_stub->handle_push_quote(quote);
}

/**
 * @brief 处理委托队列数据
 * 
 * 接收并处理行情源提供的委托队列数据，进行交易所过滤和数据完整性检查后转发给回调接口
 * 
 * @param ordQueData 委托队列数据
 */
void ParserAdapter::handleOrderQueue(WTSOrdQueData* ordQueData)
{
	if (_stopped)
		return;

	if (!_exchg_filter.empty() && (_exchg_filter.find(ordQueData->exchg()) == _exchg_filter.end()))
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

/**
 * @brief 处理逐笔委托数据
 * 
 * 接收并处理行情源提供的逐笔委托数据，进行交易所过滤和数据完整性检查后转发给回调接口
 * 
 * @param ordDtlData 逐笔委托数据
 */
void ParserAdapter::handleOrderDetail(WTSOrdDtlData* ordDtlData)
{
	if (_stopped)
		return;

	if (!_exchg_filter.empty() && (_exchg_filter.find(ordDtlData->exchg()) == _exchg_filter.end()))
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

/**
 * @brief 处理逐笔成交数据
 * 
 * 接收并处理行情源提供的逐笔成交数据，进行交易所过滤和数据完整性检查后转发给回调接口
 * 
 * @param transData 逐笔成交数据
 */
void ParserAdapter::handleTransaction(WTSTransData* transData)
{
	if (_stopped)
		return;

	if (!_exchg_filter.empty() && (_exchg_filter.find(transData->exchg()) == _exchg_filter.end()))
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


/**
 * @brief 处理解析器日志
 * 
 * 接收并处理行情解析器输出的日志信息，转发到系统日志模块
 * 
 * @param ll 日志级别
 * @param message 日志消息
 */
void ParserAdapter::handleParserLog(WTSLogLevel ll, const char* message)
{
	if (_stopped)
		return;

	WTSLogger::log_dyn_raw("parser", _id.c_str(), ll, message);
}


//////////////////////////////////////////////////////////////////////////
//ParserAdapterMgr
/**
 * @brief 释放所有适配器资源
 * 
 * 遍历并释放所有管理的行情适配器资源
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
 * @brief 添加新的适配器
 * 
 * 将行情适配器添加到适配器映射表中
 * 
 * @param id 适配器标识符
 * @param adapter 行情适配器指针
 * @return bool 添加是否成功
 */
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


/**
 * @brief 获取指定标识符的适配器
 * 
 * 根据标识符从适配器映射表中查找并返回相应的适配器
 * 
 * @param id 适配器标识符
 * @return ParserAdapterPtr 行情适配器指针，如果不存在则返回空指针
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
 * 遍历并启动所有管理的行情适配器
 */
void ParserAdapterMgr::run()
{
	for (auto it = _adapters.begin(); it != _adapters.end(); it++)
	{
		it->second->run();
	}

	WTSLogger::info("{} parsers started", _adapters.size());
}