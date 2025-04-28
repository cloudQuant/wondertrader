/*!
 * \file ParserAdapter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 行情解析器适配器实现文件
 * \details 该文件实现了行情解析器适配器，用于连接不同的行情源和内部数据处理模块，
 *          将各种格式的行情数据转换为统一的内部格式，并分发给相应的处理模块
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

USING_NS_WTP;

//////////////////////////////////////////////////////////////////////////
//ParserAdapter
/**
 * @brief 解析器适配器构造函数
 * @details 初始化所有指针成员为NULL，停止标志设置为false
 */
ParserAdapter::ParserAdapter()
	: _parser_api(NULL)
	, _remover(NULL)
	, _stopped(false)
	, _bd_mgr(NULL)
	, _stub(NULL)
	, _cfg(NULL)
{
}


/**
 * @brief 解析器适配器析构函数
 * @details 析构函数为空，资源释放在release方法中进行
 * @note 这可能导致内存泄漏，如果用户忘记调用release方法
 */
ParserAdapter::~ParserAdapter()
{
}

/**
 * @brief 使用外部API初始化解析器适配器
 * @param id 解析器标识
 * @param api 已创建的解析器API实例
 * @param stub 数据回调对象
 * @param bgMgr 基础数据管理器
 * @param hotMgr 主力合约管理器
 * @return 初始化是否成功
 * @details 使用外部提供的API实例初始化适配器，无需加载模块
 */
bool ParserAdapter::initExt(const char* id, IParserApi* api, IParserStub* stub, IBaseDataMgr* bgMgr, IHotMgr* hotMgr/* = NULL*/)
{
	// 检查API是否为空
	if (api == NULL)
		return false;

	// 保存参数
	_parser_api = api;
	_stub = stub;
	_bd_mgr = bgMgr;
	_hot_mgr = hotMgr;
	_id = id;

	if (_parser_api)
	{
		// 注册回调接口
		_parser_api->registerSpi(this);

		if (_parser_api->init(NULL))
		{
			// 订阅所有合约
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
 * @brief 使用配置初始化解析器适配器
 * @param id 解析器标识
 * @param cfg 配置对象
 * @param stub 数据回调对象
 * @param bgMgr 基础数据管理器
 * @param hotMgr 主力合约管理器
 * @return 初始化是否成功
 * @details 根据配置加载解析器模块，创建API实例并初始化
 */
bool ParserAdapter::init(const char* id, WTSVariant* cfg, IParserStub* stub, IBaseDataMgr* bgMgr, IHotMgr* hotMgr/* = NULL*/)
{
	// 检查配置是否为空
	if (cfg == NULL)
		return false;

	// 保存参数
	_stub = stub;
	_bd_mgr = bgMgr;
	_hot_mgr = hotMgr;
	_id = id;

	// 检查是否已初始化
	if (_cfg != NULL)
		return false;

	// 保存配置并增加引用计数
	_cfg = cfg;
	_cfg->retain();

	// 获取时间检查标志
	_check_time = cfg->getBoolean("check_time");

	{
		//加载解析器模块
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
					if(contract)
						contractSet.insert(contract->getFullCode());
					else
					{
						//如果是品种ID，则将该品种下全部合约都加到订阅列表
						WTSCommodityInfo* commInfo = _bd_mgr->getCommodity(exchg.c_str(), code.c_str());
						if(commInfo)
						{
							const auto& codes = commInfo->getCodes();
							for(const auto& c : codes)
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
					WTSArray* ayContract =_bd_mgr->getContracts((*it).c_str());
					WTSArray::Iterator it = ayContract->begin();
					for (; it != ayContract->end(); it++)
					{
						WTSContractInfo* contract = STATIC_CONVERT(*it, WTSContractInfo*);
						contractSet.insert(contract->getFullCode());
					}

					ayContract->release();
				}
			}
			else
			{
				WTSArray* ayContract =_bd_mgr->getContracts();
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

	WTSLogger::log_dyn("parser", _id.c_str(), LL_INFO, "[{}] Parser initialzied, check_time: {}", _id.c_str(), _check_time);

	return true;
}

/**
 * @brief 释放解析器适配器资源
 * @details 设置停止标志并释放解析器API资源
 * @note 未释放_cfg配置对象，可能导致内存泄漏
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
 * @return 启动是否成功
 * @details 连接到行情源并开始接收数据
 */
bool ParserAdapter::run()
{
	if (_parser_api == NULL)
		return false;

	_parser_api->connect();
	return true;
}

/**
 * @brief 合理的毫秒时间差
 * @details 用于判断行情数据时间是否合理，设为1小时
 */
const int RESONABLE_MILLISECS = 60 * 60 * 1000;
/**
 * @brief 处理行情数据
 * @param quote 行情数据对象
 * @param procFlag 处理标志
 * @details 处理行情数据，进行过滤、时间检查和代码转换，然后转发给回调对象
 */
void ParserAdapter::handleQuote(WTSTickData *quote, uint32_t procFlag)
{
	// 检查数据有效性
	if (quote == NULL || _stopped || quote->actiondate() == 0 || quote->tradingdate() == 0)
		return;

	// 交易所过滤
	if (!_exchg_filter.empty() && (_exchg_filter.find(quote->exchg()) == _exchg_filter.end()))
		return;

	// 获取合约信息
	WTSContractInfo* cInfo = quote->getContractInfo();
	if (cInfo == NULL)
	{
		cInfo = _bd_mgr->getContract(quote->code(), quote->exchg());
		quote->setContractInfo(cInfo);
	}

	if (cInfo == NULL)
		return;

	// 获取品种信息
	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	WTSSessionInfo* sInfo = commInfo->getSessionInfo();

	// 时间检查
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

	// 代码转换
	std::string stdCode;
	if (commInfo->getCategoty() == CC_FutOption || commInfo->getCategoty() == CC_SpotOption)
	{
		// 期权代码转换
		stdCode = CodeHelper::rawFutOptCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
	}
	else if(CodeHelper::isMonthlyCode(quote->code()))
	{
		//分月合约代码转换
		stdCode = CodeHelper::rawMonthCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
	}
	else
	{
		// 普通代码转换
		stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), cInfo->getProduct());
	}
	quote->setCode(stdCode.c_str());

	// 转发行情数据
	_stub->handle_push_quote(quote);
}

/**
 * @brief 处理委托队列数据
 * @param ordQueData 委托队列数据对象
 * @details 处理委托队列数据，进行过滤和代码转换，然后转发给回调对象
 */
void ParserAdapter::handleOrderQueue(WTSOrdQueData* ordQueData)
{
	// 检查是否停止
	if (_stopped)
		return;

	// 交易所过滤
	if (!_exchg_filter.empty() && (_exchg_filter.find(ordQueData->exchg()) == _exchg_filter.end()))
		return;

	// 检查日期有效性
	if (ordQueData->actiondate() == 0 || ordQueData->tradingdate() == 0)
		return;

	// 获取合约信息
	WTSContractInfo* cInfo = _bd_mgr->getContract(ordQueData->code(), ordQueData->exchg());
	if (cInfo == NULL)
		return;

	// 代码转换
	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	std::string stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), commInfo->getProduct());
	ordQueData->setCode(stdCode.c_str());

	// 转发委托队列数据
	if (_stub)
		_stub->handle_push_order_queue(ordQueData);
}

/**
 * @brief 处理委托明细数据
 * @param ordDtlData 委托明细数据对象
 * @details 处理委托明细数据，进行过滤和代码转换，然后转发给回调对象
 */
void ParserAdapter::handleOrderDetail(WTSOrdDtlData* ordDtlData)
{
	// 检查是否停止
	if (_stopped)
		return;

	// 交易所过滤
	if (!_exchg_filter.empty() && (_exchg_filter.find(ordDtlData->exchg()) == _exchg_filter.end()))
		return;

	// 检查日期有效性
	if (ordDtlData->actiondate() == 0 || ordDtlData->tradingdate() == 0)
		return;

	// 获取合约信息
	WTSContractInfo* cInfo = _bd_mgr->getContract(ordDtlData->code(), ordDtlData->exchg());
	if (cInfo == NULL)
		return;

	// 代码转换
	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	std::string stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), commInfo->getProduct());
	ordDtlData->setCode(stdCode.c_str());

	// 转发委托明细数据
	if (_stub)
		_stub->handle_push_order_detail(ordDtlData);
}

/**
 * @brief 处理成交数据
 * @param transData 成交数据对象
 * @details 处理成交数据，进行过滤和代码转换，然后转发给回调对象
 */
void ParserAdapter::handleTransaction(WTSTransData* transData)
{
	// 检查是否停止
	if (_stopped)
		return;

	// 交易所过滤
	if (!_exchg_filter.empty() && (_exchg_filter.find(transData->exchg()) == _exchg_filter.end()))
		return;

	// 检查日期有效性
	if (transData->actiondate() == 0 || transData->tradingdate() == 0)
		return;

	// 获取合约信息
	WTSContractInfo* cInfo = _bd_mgr->getContract(transData->code(), transData->exchg());
	if (cInfo == NULL)
		return;

	// 代码转换
	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	std::string stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), commInfo->getProduct());
	transData->setCode(stdCode.c_str());

	// 转发成交数据
	if (_stub)
		_stub->handle_push_transaction(transData);
}


/**
 * @brief 处理解析器日志
 * @param ll 日志级别
 * @param message 日志消息
 * @details 将解析器生成的日志转发到系统日志模块
 */
void ParserAdapter::handleParserLog(WTSLogLevel ll, const char* message)
{
	// 检查是否停止
	if (_stopped)
		return;

	// 转发日志消息
	WTSLogger::log_dyn_raw("parser", _id.c_str(), ll, message);
}


//////////////////////////////////////////////////////////////////////////
//ParserAdapterMgr
/**
 * @brief 解析器适配器管理器类
 * @details 管理多个解析器适配器实例，提供添加、获取、启动和释放等功能
 */
/**
 * @brief 释放所有解析器适配器资源
 * @details 遍历并释放所有解析器适配器，清空适配器列表
 */
void ParserAdapterMgr::release()
{
	// 遍历并释放所有适配器
	for (auto it = _adapters.begin(); it != _adapters.end(); it++)
	{
		it->second->release();
	}
	// 清空适配器列表
	_adapters.clear();
}

/**
 * @brief 添加解析器适配器
 * @param id 适配器标识
 * @param adapter 适配器指针
 * @return 添加是否成功
 * @details 将解析器适配器添加到管理器中，确保标识唯一
 */
bool ParserAdapterMgr::addAdapter(const char* id, ParserAdapterPtr& adapter)
{
	// 检查参数有效性
	if (adapter == NULL || strlen(id) == 0)
		return false;

	// 检查是否已存在相同标识的适配器
	auto it = _adapters.find(id);
	if (it != _adapters.end())
	{
		WTSLogger::error(" Same name of parsers: {}", id);
		return false;
	}

	// 添加适配器
	_adapters[id] = adapter;

	return true;
}


/**
 * @brief 获取解析器适配器
 * @param id 适配器标识
 * @return 解析器适配器指针，如果不存在则返回空指针
 * @details 根据标识查找并返回相应的解析器适配器
 */
ParserAdapterPtr ParserAdapterMgr::getAdapter(const char* id)
{
	// 查找指定标识的适配器
	auto it = _adapters.find(id);
	if (it != _adapters.end())
	{
		return it->second;
	}

	// 未找到则返回空指针
	return ParserAdapterPtr();
}

/**
 * @brief 启动所有解析器
 * @details 遍历并启动所有解析器适配器，记录启动日志
 */
void ParserAdapterMgr::run()
{
	// 遍历并启动所有适配器
	for (auto it = _adapters.begin(); it != _adapters.end(); it++)
	{
		it->second->run();
	}

	// 记录启动日志
	WTSLogger::info("{} parsers started", _adapters.size());
}