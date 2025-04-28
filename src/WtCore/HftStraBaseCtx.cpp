/*!
 * \file HftStraBaseCtx.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * \brief 高频交易策略基础上下文实现文件
 * 
 * 本文件实现了高频交易策略的基础上下文，包括订单管理、持仓管理、数据订阅、日志记录等功能
 * 高频交易策略上下文是策略和交易引擎之间的桥梁，提供了策略所需的各种接口和数据
 */
#include "HftStraBaseCtx.h"
#include "WtHftEngine.h"
#include "TraderAdapter.h"
#include "WtHelper.h"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/IBaseDataMgr.h"

#include "../Share/CodeHelper.hpp"
#include "../Share/decimal.h"

#include "../WTSTools/WTSLogger.h"
#include "../WTSTools/WTSHotMgr.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;

USING_NS_WTP;

/**
 * @brief 生成高频策略上下文ID
 * @return 新生成的上下文ID
 * @details 使用原子操作生成唯一的高频策略上下文ID，起始值为6000
 */
inline uint32_t makeHftCtxId()
{
	static std::atomic<uint32_t> _auto_context_id{ 6000 };
	return _auto_context_id.fetch_add(1);
}

/**
 * @brief 高频交易策略上下文构造函数
 * @param engine 高频交易引擎指针
 * @param name 策略名称
 * @param bAgent 是否作为数据代理，影响日志输出等功能
 * @param slippage 滑点设置，影响交易价格计算
 * @details 初始化高频交易策略上下文，设置引擎、名称、数据代理标志和滑点，并生成唯一的上下文ID
 */
HftStraBaseCtx::HftStraBaseCtx(WtHftEngine* engine, const char* name, bool bAgent, int32_t slippage)
	: IHftStraCtx(name)
	, _engine(engine)
	, _data_agent(bAgent)
	, _slippage(slippage)
{
	_context_id = makeHftCtxId();
}


/**
 * @brief 高频交易策略上下文析构函数
 * @details 清理高频交易策略上下文资源
 */
HftStraBaseCtx::~HftStraBaseCtx()
{
}

/**
 * @brief 获取策略上下文ID
 * @return 策略上下文唯一ID
 * @details 返回当前高频策略上下文的唯一标识符
 */
uint32_t HftStraBaseCtx::id()
{
	return _context_id;
}

/**
 * @brief 设置交易适配器
 * @param trader 交易适配器指针
 * @details 为策略上下文设置交易适配器，用于执行实际交易操作
 */
void HftStraBaseCtx::setTrader(TraderAdapter* trader)
{
	_trader = trader;
}

/**
 * @brief 初始化输出文件
 * @details 初始化策略的各种日志输出文件，包括交易日志、平仓日志、资金日志和信号日志
 * 当_data_agent为false时，不创建输出文件
 */
void HftStraBaseCtx::init_outputs()
{
	if (!_data_agent)
		return;

	std::string folder = WtHelper::getOutputDir();
	folder += _name;
	folder += "//";
	BoostFile::create_directories(folder.c_str());

	// 创建交易日志文件
	std::string filename = folder + "trades.csv";
	_trade_logs.reset(new BoostFile());
	{
		bool isNewFile = !BoostFile::exists(filename.c_str());
		_trade_logs->create_or_open_file(filename.c_str());
		if (isNewFile)
		{
			_trade_logs->write_file("code,time,direct,action,price,qty,tag,fee\n");
		}
		else
		{
			_trade_logs->seek_to_end();
		}
	}

	// 创建平仓日志文件
	filename = folder + "closes.csv";
	_close_logs.reset(new BoostFile());
	{
		bool isNewFile = !BoostFile::exists(filename.c_str());
		_close_logs->create_or_open_file(filename.c_str());
		if (isNewFile)
		{
			_close_logs->write_file("code,direct,opentime,openprice,closetime,closeprice,qty,profit,totalprofit,entertag,exittag\n");
		}
		else
		{
			_close_logs->seek_to_end();
		}
	}

	// 创建资金日志文件
	filename = folder + "funds.csv";
	_fund_logs.reset(new BoostFile());
	{
		bool isNewFile = !BoostFile::exists(filename.c_str());
		_fund_logs->create_or_open_file(filename.c_str());
		if (isNewFile)
		{
			_fund_logs->write_file("date,closeprofit,positionprofit,dynbalance,fee\n");
		}
		else
		{
			_fund_logs->seek_to_end();
		}
	}

	// 创建信号日志文件
	filename = folder + "signals.csv";
	_sig_logs.reset(new BoostFile());
	{
		bool isNewFile = !BoostFile::exists(filename.c_str());
		_sig_logs->create_or_open_file(filename.c_str());
		if (isNewFile)
		{
			_sig_logs->write_file("code,target,sigprice,gentime,usertag\n");
		}
		else
		{
			_sig_logs->seek_to_end();
		}
	}
}

/**
 * @brief 策略初始化回调
 * @details 在策略初始化时触发，初始化输出文件并加载用户数据
 * 该函数在策略启动时被调用，用于准备策略运行环境
 */
void HftStraBaseCtx::on_init()
{
	init_outputs();

	load_userdata();
}

/**
 * @brief Tick数据回调
 * @param stdCode 标准化合约代码
 * @param newTick 新的Tick数据
 * @details 当收到新的Tick数据时触发此回调
 * 如果用户数据被修改，则保存用户数据
 */
void HftStraBaseCtx::on_tick(const char* stdCode, WTSTickData* newTick)
{
	if (_ud_modified)
	{
		save_userdata();
		_ud_modified = false;
	}
}

/**
 * @brief 委托队列数据回调
 * @param stdCode 标准化合约代码
 * @param newOrdQue 新的委托队列数据
 * @details 当收到新的委托队列数据时触发此回调
 * 如果用户数据被修改，则保存用户数据
 */
void HftStraBaseCtx::on_order_queue(const char* stdCode, WTSOrdQueData* newOrdQue)
{
	if (_ud_modified)
	{
		save_userdata();
		_ud_modified = false;
	}
}

/**
 * @brief 委托明细数据回调
 * @param stdCode 标准化合约代码
 * @param newOrdDtl 新的委托明细数据
 * @details 当收到新的委托明细数据时触发此回调
 * 如果用户数据被修改，则保存用户数据
 */
void HftStraBaseCtx::on_order_detail(const char* stdCode, WTSOrdDtlData* newOrdDtl)
{
	if (_ud_modified)
	{
		save_userdata();
		_ud_modified = false;
	}
}

/**
 * @brief 成交明细数据回调
 * @param stdCode 标准化合约代码
 * @param newTrans 新的成交明细数据
 * @details 当收到新的成交明细数据时触发此回调
 * 如果用户数据被修改，则保存用户数据
 */
void HftStraBaseCtx::on_transaction(const char* stdCode, WTSTransData* newTrans)
{
	if (_ud_modified)
	{
		save_userdata();
		_ud_modified = false;
	}
}

/**
 * @brief K线数据回调
 * @param stdCode 标准化合约代码
 * @param period 周期标识，如"m1"/"m5"/"d1"等
 * @param times 周期倍数
 * @param newBar 新的K线数据
 * @details 当收到新的K线数据时触发此回调
 * 如果用户数据被修改，则保存用户数据
 */
void HftStraBaseCtx::on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	if (_ud_modified)
	{
		save_userdata();
		_ud_modified = false;
	}
}

/**
 * @brief 根据本地订单ID撤单
 * @param localid 本地订单ID
 * @return 撤单是否成功
 * @details 根据本地订单ID撤销指定订单
 */
bool HftStraBaseCtx::stra_cancel(uint32_t localid)
{
	return _trader->cancel(localid);
}

/**
 * @brief 根据合约和方向撤单
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买单
 * @param qty 撤单数量
 * @return 撤销的订单ID列表
 * @details 根据合约代码、交易方向和数量撤销订单
 * 在撤单前会进行撤单频率检查，防止过频撤单
 */
OrderIDs HftStraBaseCtx::stra_cancel(const char* stdCode, bool isBuy, double qty)
{
	//撤单频率检查
	if (!_trader->checkCancelLimits(stdCode))
		return OrderIDs();

	return _trader->cancel(stdCode, isBuy, qty);
}

const /**
 * @brief 获取内部代码
 * @param stdCode 标准化合约代码
 * @return 内部代码字符串
 * @details 将标准化合约代码转换为内部使用的代码格式
 * 例如，将 "SHFE.rb" 转换为 "rb.SHFE"
 * 使用线程本地存储缓存转换结果
 */
char* HftStraBaseCtx::get_inner_code(const char* stdCode)
{
	auto it = _code_map.find(stdCode);
	if (it == _code_map.end())
		return stdCode;

	return it->second.c_str();
}

/**
 * @brief 执行买入操作
 * @param stdCode 标准化合约代码
 * @param price 买入价格
 * @param qty 买入数量
 * @param userTag 用户自定义标签
 * @param flag 交易标志，默认为0
 * @param bForceClose 是否强制平仓，默认为false
 * @return 订单ID列表
 * @details 执行买入操作，支持自定义规则合约的映射处理
 * 在下单前会检查下单限制，如果合约被禁止交易则返回空列表
 */
OrderIDs HftStraBaseCtx::stra_buy(const char* stdCode, double price, double qty, const char* userTag, int flag /* = 0 */, bool bForceClose /* = false */)
{
	/*
	 *	By Wesley @ 2022.05.26
	 *	如果找到匹配自定义规则，则进行映射处理
	 */
	 //const char* ruleTag = _engine->get_hot_mgr()->getRuleTag(stdCode);
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _engine->get_hot_mgr());
	if (strlen(cInfo._ruletag) > 0)
	{
		// 处理自定义规则合约
		std::string code = _engine->get_hot_mgr()->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _engine->get_trading_date());
		std::string realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);

		WTSContractInfo* ct = _engine->get_basedata_mgr()->getContract(code.c_str(), cInfo._exchg);

		_code_map[realCode] = stdCode;

		if (_trader && !_trader->checkOrderLimits(realCode.c_str()))
		{
			log_info("{} is forbidden to trade", realCode.c_str());
			return OrderIDs();
		}

		auto ids = _trader->buy(realCode.c_str(), price, qty, flag, bForceClose, ct);
		for (auto localid : ids)
			setUserTag(localid, userTag);

		return ids;
	}
	else
	{
		// 处理普通合约
		WTSContractInfo* ct = _engine->get_basedata_mgr()->getContract(cInfo._code, cInfo._exchg);
		if (ct == NULL)
		{
			log_error("Cannot find corresponding contract info of {}", stdCode);
			return OrderIDs();
		}

		if (!_trader->checkOrderLimits(stdCode))
		{
			log_info("{} is forbidden to trade", stdCode);
			return OrderIDs();
		}

		auto ids = _trader->buy(stdCode, price, qty, flag, bForceClose, ct);
		for (auto localid : ids)
			setUserTag(localid, userTag);
		return ids;
	}
}

/**
 * @brief 执行卖出操作
 * @param stdCode 标准化合约代码
 * @param price 卖出价格
 * @param qty 卖出数量
 * @param userTag 用户自定义标签
 * @param flag 交易标志，默认为0
 * @param bForceClose 是否强制平仓，默认为false
 * @return 订单ID列表
 * @details 执行卖出操作，支持自定义规则合约的映射处理
 * 在下单前会检查下单限制，如果合约被禁止交易则返回空列表
 * 对于不能做空的商品，会检查可用持仓是否足够
 */
OrderIDs HftStraBaseCtx::stra_sell(const char* stdCode, double price, double qty, const char* userTag, int flag /* = 0 */, bool bForceClose /* = false */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _engine->get_hot_mgr());
	WTSCommodityInfo* commInfo = _engine->get_basedata_mgr()->getCommodity(cInfo._exchg, cInfo._product);

	//如果不能做空，则要看可用持仓
	if (!commInfo->canShort())
	{
		double curPos = stra_get_position(stdCode, true);//只读可用持仓
		if (decimal::gt(qty, curPos))
		{
			log_error("No enough position of {} to sell", stdCode);
			return OrderIDs();
		}
	}

	/*
	 *	By Wesley @ 2022.05.26
	 *	如果找到匹配自定义规则，则进行映射处理
	 */
	
	if (strlen(cInfo._ruletag) > 0)
	{
		// 处理自定义规则合约
		std::string code = _engine->get_hot_mgr()->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _engine->get_trading_date());
		std::string realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);

		WTSContractInfo* ct = _engine->get_basedata_mgr()->getContract(code.c_str(), cInfo._exchg);

		_code_map[realCode] = stdCode;

		if (_trader && !_trader->checkOrderLimits(realCode.c_str()))
		{
			log_info("{} is forbidden to trade", realCode.c_str());
			return OrderIDs();
		}

		auto ids = _trader->sell(realCode.c_str(), price, qty, flag, bForceClose, ct);
		for (auto localid : ids)
			setUserTag(localid, userTag);
		return ids;
	}
	else
	{
		// 处理普通合约
		WTSContractInfo* ct = _engine->get_basedata_mgr()->getContract(cInfo._code, cInfo._exchg);
		if (ct == NULL)
		{
			log_error("Cannot find corresponding contract info of {}", stdCode);
			return OrderIDs();
		}

		if (_trader && !_trader->checkOrderLimits(stdCode))
		{
			log_info("{} is forbidden to trade", stdCode);
			return OrderIDs();
		}

		auto ids = _trader->sell(stdCode, price, qty, flag, bForceClose, ct);
		for (auto localid : ids)
			setUserTag(localid, userTag);
		return ids;
	}
}

/**
 * @brief 开多仓操作
 * @param stdCode 标准化合约代码
 * @param price 交易价格
 * @param qty 交易数量
 * @param userTag 用户自定义标签
 * @param flag 交易标志，默认为0
 * @return 订单ID
 * @details 执行开多仓操作，支持自定义规则合约的映射处理
 * 如果合约代码是自定义规则合约，则先转换为实际合约再交易
 */
uint32_t HftStraBaseCtx::stra_enter_long(const char* stdCode, double price, double qty, const char* userTag, int flag/* = 0*/)
{
	std::string realCode = stdCode;
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _engine->get_hot_mgr());
	if (strlen(cInfo._ruletag) > 0)
	{
		// 处理自定义规则合约
		std::string code = _engine->get_hot_mgr()->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _engine->get_trading_date());
		realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);
		_code_map[realCode] = stdCode;
	}

	return _trader->openLong(realCode.c_str(), price, qty, flag);
}

/**
 * @brief 平多仓操作
 * @param stdCode 标准化合约代码
 * @param price 交易价格
 * @param qty 交易数量
 * @param userTag 用户自定义标签
 * @param isToday 是否平今仓，默认为false
 * @param flag 交易标志，默认为0
 * @return 订单ID
 * @details 执行平多仓操作，支持自定义规则合约的映射处理
 * 如果合约代码是自定义规则合约，则先转换为实际合约再交易
 */
uint32_t HftStraBaseCtx::stra_exit_long(const char* stdCode, double price, double qty, const char* userTag, bool isToday/* = false*/, int flag/* = 0*/)
{
	std::string realCode = stdCode;
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _engine->get_hot_mgr());
	if (strlen(cInfo._ruletag) > 0)
	{
		// 处理自定义规则合约
		std::string code = _engine->get_hot_mgr()->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _engine->get_trading_date());
		realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);

		_code_map[realCode] = stdCode;
	}

	return _trader->closeLong(realCode.c_str(), price, qty, isToday, flag);
}

/**
 * @brief 开空仓操作
 * @param stdCode 标准化合约代码
 * @param price 交易价格
 * @param qty 交易数量
 * @param userTag 用户自定义标签
 * @param flag 交易标志，默认为0
 * @return 订单ID
 * @details 执行开空仓操作，支持自定义规则合约的映射处理
 * 如果合约代码是自定义规则合约，则先转换为实际合约再交易
 */
uint32_t HftStraBaseCtx::stra_enter_short(const char* stdCode, double price, double qty, const char* userTag, int flag/* = 0*/)
{
	std::string realCode = stdCode;
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _engine->get_hot_mgr());
	if (strlen(cInfo._ruletag) > 0)
	{
		// 处理自定义规则合约
		std::string code = _engine->get_hot_mgr()->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _engine->get_trading_date());
		realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);

		_code_map[realCode] = stdCode;
	}
	//else if (CodeHelper::isStdFutHotCode(stdCode))
	//{
	//	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	//	std::string code = _engine->get_hot_mgr()->getRawCode(cInfo._exchg, cInfo._product, _engine->get_trading_date());
	//	realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);

	//	_code_map[realCode] = stdCode;
	//}
	//else if (CodeHelper::isStdFut2ndCode(stdCode))
	//{
	//	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	//	std::string code = _engine->get_hot_mgr()->getSecondRawCode(cInfo._exchg, cInfo._product, _engine->get_trading_date());
	//	realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);

	//	_code_map[realCode] = stdCode;
	//}

	return _trader->openShort(realCode.c_str(), price, qty, flag);
}

/**
 * @brief 平空仓操作
 * @param stdCode 标准化合约代码
 * @param price 交易价格
 * @param qty 交易数量
 * @param userTag 用户自定义标签
 * @param isToday 是否平今仓，默认为false
 * @param flag 交易标志，默认为0
 * @return 订单ID
 * @details 执行平空仓操作，支持自定义规则合约的映射处理
 * 如果合约代码是自定义规则合约，则先转换为实际合约再交易
 */
uint32_t HftStraBaseCtx::stra_exit_short(const char* stdCode, double price, double qty, const char* userTag, bool isToday/* = false*/, int flag/* = 0*/)
{
	std::string realCode = stdCode;
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _engine->get_hot_mgr());
	if (strlen(cInfo._ruletag) > 0)
	{
		// 处理自定义规则合约
		std::string code = _engine->get_hot_mgr()->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _engine->get_trading_date());
		realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);

		_code_map[realCode] = stdCode;
	}
	//else if (CodeHelper::isStdFutHotCode(stdCode))
	//{
	//	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	//	std::string code = _engine->get_hot_mgr()->getRawCode(cInfo._exchg, cInfo._product, _engine->get_trading_date());
	//	realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);

	//	_code_map[realCode] = stdCode;
	//}
	//else if (CodeHelper::isStdFut2ndCode(stdCode))
	//{
	//	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	//	std::string code = _engine->get_hot_mgr()->getSecondRawCode(cInfo._exchg, cInfo._product, _engine->get_trading_date());
	//	realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);

	//	_code_map[realCode] = stdCode;
	//}

	return _trader->closeShort(realCode.c_str(), price, qty, isToday, flag);
}

/**
 * @brief 获取商品信息
 * @param stdCode 标准化合约代码
 * @return 商品信息指针
 * @details 获取指定合约的商品信息，包括合约乘数、手续费率、交易时段等
 */
WTSCommodityInfo* HftStraBaseCtx::stra_get_comminfo(const char* stdCode)
{
	return _engine->get_commodity_info(stdCode);
}

/**
 * @brief 获取原始合约代码
 * @param stdCode 标准化合约代码
 * @return 原始合约代码
 * @details 将标准化合约代码转换为原始合约代码，如将SHFE.rb.HOT转换为实际的合约月份代码
 */
std::string HftStraBaseCtx::stra_get_rawcode(const char* stdCode)
{
	return _engine->get_rawcode(stdCode);
}

/**
 * @brief 获取K线数据
 * @param stdCode 标准化合约代码
 * @param period 周期标识，如"m1"/"m5"/"d1"等
 * @param count 请求的K线数量
 * @return K线数据切片指针
 * @details 获取指定合约的历史K线数据切片
 * 周期格式为单个字母加数字，如m1表示1分钟，m5表示5分钟
 * 获取数据后会自动订阅该合约的tick数据
 */
WTSKlineSlice* HftStraBaseCtx::stra_get_bars(const char* stdCode, const char* period, uint32_t count)
{
	// 使用线程本地存储来保存基础周期
	thread_local static char basePeriod[2] = { 0 };
	basePeriod[0] = period[0];
	uint32_t times = 1;
	if (strlen(period) > 1)
		times = strtoul(period + 1, NULL, 10);

	WTSKlineSlice* ret = _engine->get_kline_slice(_context_id, stdCode, basePeriod, count, times);

	// 如果成功获取切片，自动订阅tick数据
	if (ret)
		_engine->sub_tick(id(), stdCode);

	return ret;
}

/**
 * @brief 获取Tick数据
 * @param stdCode 标准化合约代码
 * @param count 请求的Tick数量
 * @return Tick数据切片指针
 * @details 获取指定合约的历史Tick数据切片
 * 获取数据后会自动订阅该合约的tick数据
 */
WTSTickSlice* HftStraBaseCtx::stra_get_ticks(const char* stdCode, uint32_t count)
{
	// 从引擎中获取tick切片
	WTSTickSlice* ticks = _engine->get_tick_slice(_context_id, stdCode, count);

	// 如果成功获取切片，自动订阅tick数据
	if (ticks)
		_engine->sub_tick(id(), stdCode);
	return ticks;
}

/**
 * @brief 获取委托明细数据
 * @param stdCode 标准化合约代码
 * @param count 请求的委托明细数量
 * @return 委托明细数据切片指针
 * @details 获取指定合约的历史委托明细数据切片
 * 获取数据后会自动订阅该合约的委托明细数据
 */
WTSOrdDtlSlice* HftStraBaseCtx::stra_get_order_detail(const char* stdCode, uint32_t count)
{
	// 从引擎中获取委托明细切片
	WTSOrdDtlSlice* ret = _engine->get_order_detail_slice(_context_id, stdCode, count);

	// 如果成功获取切片，自动订阅委托明细数据
	if (ret)
		_engine->sub_order_detail(id(), stdCode);
	return ret;
}

/**
 * @brief 获取委托队列数据
 * @param stdCode 标准化合约代码
 * @param count 请求的委托队列数量
 * @return 委托队列数据切片指针
 * @details 获取指定合约的历史委托队列数据切片
 * 获取数据后会自动订阅该合约的委托队列数据
 */
WTSOrdQueSlice* HftStraBaseCtx::stra_get_order_queue(const char* stdCode, uint32_t count)
{
	// 从引擎中获取委托队列切片
	WTSOrdQueSlice* ret = _engine->get_order_queue_slice(_context_id, stdCode, count);

	// 如果成功获取切片，自动订阅委托队列数据
	if (ret)
		_engine->sub_order_queue(id(), stdCode);
	return ret;
}


/**
 * @brief 获取成交明细数据
 * @param stdCode 标准化合约代码
 * @param count 请求的成交明细数量
 * @return 成交明细数据切片指针
 * @details 获取指定合约的历史成交明细数据切片
 * 获取数据后会自动订阅该合约的成交明细数据
 */
WTSTransSlice* HftStraBaseCtx::stra_get_transaction(const char* stdCode, uint32_t count)
{
	// 从引擎中获取成交明细切片
	WTSTransSlice* ret = _engine->get_transaction_slice(_context_id, stdCode, count);

	// 如果成功获取切片，自动订阅成交明细数据
	if (ret)
		_engine->sub_transaction(id(), stdCode);
	return ret;
}


/**
 * @brief 获取最新Tick数据
 * @param stdCode 标准化合约代码
 * @return 最新Tick数据指针
 * @details 获取指定合约的最新市场行情数据
 * 这个函数不会自动订阅数据，只是返回当前缓存中的最新数据
 */
WTSTickData* HftStraBaseCtx::stra_get_last_tick(const char* stdCode)
{
	return _engine->get_last_tick(_context_id, stdCode);
}

/**
 * @brief 订阅Tick数据
 * @param stdCode 标准化合约代码
 * @details 订阅指定合约的Tick数据
 * 订阅后会在本地记录订阅信息，并在tick数据回调时进行检查
 */
void HftStraBaseCtx::stra_sub_ticks(const char* stdCode)
{
	/*
	 *	By Wesley @ 2022.03.01
	 *	主动订阅tick会在本地记一下
	 *	tick数据回调的时候先检查一下
	 */
	// 在本地订阅集合中记录该合约
	_tick_subs.insert(stdCode);

	// 向引擎发送订阅请求
	_engine->sub_tick(id(), stdCode);
	log_info("Market Data subscribed: {}", stdCode);
}

/**
 * @brief 订阅委托明细数据
 * @param stdCode 标准化合约代码
 * @details 订阅指定合约的委托明细数据
 * 订阅后将会收到该合约的委托明细数据回调
 */
void HftStraBaseCtx::stra_sub_order_details(const char* stdCode)
{
	// 向引擎发送订阅请求
	_engine->sub_order_detail(id(), stdCode);
	log_info("Order details subscribed: {}", stdCode);
}

/**
 * @brief 订阅委托队列数据
 * @param stdCode 标准化合约代码
 * @details 订阅指定合约的委托队列数据
 * 订阅后将会收到该合约的委托队列数据回调
 */
void HftStraBaseCtx::stra_sub_order_queues(const char* stdCode)
{
	// 向引擎发送订阅请求
	_engine->sub_order_queue(id(), stdCode);
	log_info("Order queues subscribed: {}", stdCode);
}

/**
 * @brief 订阅成交明细数据
 * @param stdCode 标准化合约代码
 * @details 订阅指定合约的成交明细数据
 * 订阅后将会收到该合约的成交明细数据回调
 */
void HftStraBaseCtx::stra_sub_transactions(const char* stdCode)
{
	// 向引擎发送订阅请求
	_engine->sub_transaction(id(), stdCode);
	log_info("Transactions subscribed: {}", stdCode);
}

/**
 * @brief 记录信息级别日志
 * @param message 日志消息
 * @details 记录信息级别的日志，使用策略名称作为标识
 * 这个函数可以在策略中用于记录一般性的信息
 */
void HftStraBaseCtx::stra_log_info(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_INFO, message);
}

/**
 * @brief 记录调试级别日志
 * @param message 日志消息
 * @details 记录调试级别的日志，使用策略名称作为标识
 * 这个函数可以在策略中用于记录详细的调试信息，仅在调试模式下可见
 */
void HftStraBaseCtx::stra_log_debug(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, message);
}

/**
 * @brief 记录警告级别日志
 * @param message 日志消息
 * @details 记录警告级别的日志，使用策略名称作为标识
 * 这个函数可以在策略中用于记录需要注意的警告信息
 */
void HftStraBaseCtx::stra_log_warn(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_WARN, message);
}

/**
 * @brief 记录错误级别日志
 * @param message 日志消息
 * @details 记录错误级别的日志，使用策略名称作为标识
 * 这个函数可以在策略中用于记录严重的错误信息，通常需要立即处理
 */
void HftStraBaseCtx::stra_log_error(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_ERROR, message);
}

/**
 * @brief 成交回调函数
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入
 * @param vol 成交数量
 * @param price 成交价格
 * @details 当有订单成交时触发此回调，更新持仓信息并记录信号日志
 * 如果用户数据被修改，会先保存用户数据
 */
void HftStraBaseCtx::on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
	// 如果用户数据被修改，先保存
	if (_ud_modified)
	{
		save_userdata();
		_ud_modified = false;
	}

	// 如果启用了信号日志，记录成交信息
	if(_sig_logs && _data_agent)
	{
		double curPos = stra_get_position(stdCode);
		_sig_logs->write_file(fmt::format("{}.{}.{},{}{},{},{}\n", stra_get_date(), stra_get_time(), stra_get_secs(), isBuy ? "+" : "-", vol, curPos, price));
	}

	// 更新持仓信息
	const PosInfo& posInfo = _pos_map[stdCode];
	double curPos = posInfo._volume + vol * (isBuy ? 1 : -1);
	do_set_position(stdCode, curPos, price, getOrderTag(localid));
}

/**
 * @brief 订单状态回调函数
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入
 * @param totalQty 订单总数量
 * @param leftQty 剩余未成交数量
 * @param price 订单价格
 * @param isCanceled 是否已取消
 * @details 当订单状态发生变化时触发此回调，如订单成交、取消等
 * 如果用户数据被修改，会先保存用户数据
 * 当订单完成或取消时，应该清理相关资源
 */
void HftStraBaseCtx::on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled /* = false */)
{
	// 如果用户数据被修改，先保存
	if (_ud_modified)
	{
		save_userdata();
		_ud_modified = false;
	}

	// 如果订单已取消或已完全成交，清理相关资源
	if(isCanceled || decimal::eq(leftQty, 0))
	{
		//订单结束了，要把订单号清理掉，不然开销太大
		// 注意：这里没有实现订单清理逻辑，可能存在内存泄漏风险
	}
}

/**
 * @brief 交易通道就绪回调函数
 * @details 当交易通道准备就绪可以使用时触发此回调
 * 如果用户数据被修改，会先保存用户数据
 * 可以在此函数中执行交易通道就绪后的初始化操作
 */
void HftStraBaseCtx::on_channel_ready()
{
	// 如果用户数据被修改，先保存
	if (_ud_modified)
	{
		save_userdata();
		_ud_modified = false;
	}
	// 注意：这里没有实现具体的初始化操作，可能需要在子类中重写
}

/**
 * @brief 交易通道断开回调函数
 * @details 当交易通道断开连接时触发此回调
 * 如果用户数据被修改，会先保存用户数据
 * 可以在此函数中处理交易通道断开后的清理工作
 */
void HftStraBaseCtx::on_channel_lost()
{
	// 如果用户数据被修改，先保存
	if (_ud_modified)
	{
		save_userdata();
		_ud_modified = false;
	}
	// 注意：这里没有实现具体的清理操作，可能需要在子类中重写
}

/**
 * @brief 委托回报回调函数
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param bSuccess 委托是否成功
 * @param message 委托回报消息
 * @details 当委托发送到交易所后收到回报时触发此回调
 * 如果用户数据被修改，会先保存用户数据
 * 可以在此函数中处理委托成功或失败的后续操作
 */
void HftStraBaseCtx::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message)
{
	// 如果用户数据被修改，先保存
	if (_ud_modified)
	{
		save_userdata();
		_ud_modified = false;
	}
	// 注意：这里没有实现具体的委托处理逻辑，可能需要在子类中重写
}

/**
 * @brief 持仓变动回调函数
 * @param stdCode 标准化合约代码
 * @param isLong 是否为多头仓位
 * @param prevol 之前的持仓量
 * @param preavail 之前的可用持仓量
 * @param newvol 新的持仓量
 * @param newavail 新的可用持仓量
 * @param tradingday 交易日
 * @details 当持仓发生变化时触发此回调
 * 可以在此函数中处理持仓变化的相关逻辑
 */
void HftStraBaseCtx::on_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday)
{
	// 注意：这里没有实现具体的持仓处理逻辑，可能需要在子类中重写
}

/**
 * @brief 获取指定合约的浮动盈亏
 * @param stdCode 标准化合约代码
 * @return 浮动盈亏金额
 * @details 获取指定合约当前持仓的浮动盈亏情况
 * 如果没有该合约的持仓，返回0
 */
double HftStraBaseCtx::stra_get_position_profit(const char* stdCode)
{
	// 在持仓映射中查找指定合约
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0.0;

	// 返回该合约的浮动盈亏
	const PosInfo& pInfo = it->second;
	return pInfo._dynprofit;
}

/**
 * @brief 获取指定合约的持仓均价
 * @param stdCode 标准化合约代码
 * @return 持仓均价
 * @details 获取指定合约当前持仓的平均价格
 * 如果没有该合约的持仓或持仓量为0，返回0
 */
double HftStraBaseCtx::stra_get_position_avgpx(const char* stdCode)
{
	// 在持仓映射中查找指定合约
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	// 获取持仓信息
	const PosInfo& pInfo = it->second;
	if (pInfo._volume == 0)
		return 0.0;

	// 计算持仓总金额
	double amount = 0.0;
	for (auto dit = pInfo._details.begin(); dit != pInfo._details.end(); dit++)
	{
		const DetailInfo& dInfo = *dit;
		amount += dInfo._price*dInfo._volume;
	}

	// 返回平均价格
	return amount / pInfo._volume;
}

/**
 * @brief 获取指定合约的持仓量
 * @param stdCode 标准化合约代码
 * @param bOnlyValid 是否只返回可用持仓
 * @param flag 持仓标志，默认为3（全部持仓）
 * @return 持仓数量
 * @details 获取指定合约当前的持仓数量
 * 如果是规则合约（如主力合约），会先转换为实际合约再查询持仓
 */
double HftStraBaseCtx::stra_get_position(const char* stdCode, bool bOnlyValid /* = false */, int flag /* = 3*/)
{
	// 解析标准化合约代码
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _engine->get_hot_mgr());
	if (strlen(cInfo._ruletag) > 0)
	{
		// 如果是规则合约，先转换为实际合约
		std::string code = _engine->get_hot_mgr()->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _engine->get_trading_date());
		std::string realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);

		// 记录实际合约和标准化合约的映射关系
		_code_map[realCode] = stdCode;

		// 查询实际合约的持仓
		return _trader->getPosition(realCode.c_str(), bOnlyValid, flag);
	}
	else
	{
		// 如果不是规则合约，直接查询
		return _trader->getPosition(stdCode, bOnlyValid, flag);
	}
}

/**
 * @brief 获取指定合约的未完成委托量
 * @param stdCode 标准化合约代码
 * @return 未完成委托数量
 * @details 获取指定合约当前的未完成委托数量
 * 如果是规则合约（如主力合约），会先转换为实际合约再查询未完成委托
 */
double HftStraBaseCtx::stra_get_undone(const char* stdCode)
{
	// 解析标准化合约代码
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _engine->get_hot_mgr());
	if (strlen(cInfo._ruletag) > 0)
	{
		// 如果是规则合约，先转换为实际合约
		std::string code = _engine->get_hot_mgr()->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _engine->get_trading_date());
		std::string realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);

		// 记录实际合约和标准化合约的映射关系
		_code_map[realCode] = stdCode;

		// 查询实际合约的未完成委托
		return _trader->getUndoneQty(realCode.c_str());
	}
	// 以下是被注释的代码，可能是旧版本的实现方式
	//else if (CodeHelper::isStdFutHotCode(stdCode))
	//{
	//	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	//	std::string code = _engine->get_hot_mgr()->getRawCode(cInfo._exchg, cInfo._product, _engine->get_trading_date());
	//	std::string realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);

	//	_code_map[realCode] = stdCode;

	//	return _trader->getUndoneQty(realCode.c_str());
	//}
	//else if (CodeHelper::isStdFut2ndCode(stdCode))
	//{
	//	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	//	std::string code = _engine->get_hot_mgr()->getSecondRawCode(cInfo._exchg, cInfo._product, _engine->get_trading_date());
	//	std::string realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);

	//	_code_map[realCode] = stdCode;

	//	return _trader->getUndoneQty(realCode.c_str());
	//}
	else
	{
		// 如果不是规则合约，直接查询
		return _trader->getUndoneQty(stdCode);
	}
}

/**
 * @brief 获取指定合约的当前价格
 * @param stdCode 标准化合约代码
 * @return 当前价格
 * @details 获取指定合约的当前价格
 * 先从内部缓存中查找，如果没有则从引擎中获取
 */
double HftStraBaseCtx::stra_get_price(const char* stdCode)
{
	// 先从内部价格缓存中查找
	auto it = _price_map.find(stdCode);
	if (it != _price_map.end())
		return it->second;

	// 如果缓存中没有，从引擎中获取当前价格
	return _engine->get_cur_price(stdCode);
}

/**
 * @brief 获取当前交易日期
 * @return 当前交易日期，格式为YYYYMMDD
 * @details 从引擎中获取当前的交易日期
 */
uint32_t HftStraBaseCtx::stra_get_date()
{
	return _engine->get_date();
}

/**
 * @brief 获取当前交易时间
 * @return 当前交易时间，格式为HHMMSS
 * @details 从引擎中获取当前的交易时间（不包含毫秒）
 */
uint32_t HftStraBaseCtx::stra_get_time()
{
	return _engine->get_raw_time();
}

/**
 * @brief 获取当前交易时间的毫秒部分
 * @return 当前交易时间的毫秒部分
 * @details 从引擎中获取当前的交易时间的毫秒部分
 */
uint32_t HftStraBaseCtx::stra_get_secs()
{
	return _engine->get_secs();
}

/**
 * @brief 加载用户自定义数据
 * @param key 数据键名
 * @param defVal 默认值，当键不存在时返回此值
 * @return 数据值或默认值
 * @details 根据键名从用户数据存储中加载数据
 * 如果指定的键不存在，则返回默认值
 */
const char* HftStraBaseCtx::stra_load_user_data(const char* key, const char* defVal /*= ""*/)
{
	// 在用户数据映射中查找指定的键
	auto it = _user_datas.find(key);
	if (it != _user_datas.end())
		return it->second.c_str();

	// 如果键不存在，返回默认值
	return defVal;
}

/**
 * @brief 保存用户自定义数据
 * @param key 数据键名
 * @param val 数据值
 * @details 将用户数据保存到内存中，并标记数据已修改
 * 数据会在适当的时机（如回调函数执行时）被写入到文件中
 */
void HftStraBaseCtx::stra_save_user_data(const char* key, const char* val)
{
	// 将数据保存到用户数据映射中
	_user_datas[key] = val;
	// 标记数据已修改，需要写入文件
	_ud_modified = true;
}

/**
 * @brief 将用户数据保存到文件
 * @details 将内存中的用户数据写入到JSON文件中
 * 文件名格式为"ud_策略名.json"，保存在策略用户数据目录下
 */
void HftStraBaseCtx::save_userdata()
{
	//ini.save(filename.c_str()); // 旧版本的INI格式保存方式
	// 创建JSON文档对象
	rj::Document root(rj::kObjectType);
	rj::Document::AllocatorType &allocator = root.GetAllocator();
	// 遍历所有用户数据添加到JSON文档中
	for (auto it = _user_datas.begin(); it != _user_datas.end(); it++)
	{
		root.AddMember(rj::Value(it->first.c_str(), allocator), rj::Value(it->second.c_str(), allocator), allocator);
	}

	{
		// 构造文件路径
		std::string filename = WtHelper::getStraUsrDatDir();
		filename += "ud_";
		filename += _name;
		filename += ".json";

		// 创建并写入文件
		BoostFile bf;
		if (bf.create_new_file(filename.c_str()))
		{
			rj::StringBuffer sb;
			rj::PrettyWriter<rj::StringBuffer> writer(sb);
			root.Accept(writer);
			bf.write_file(sb.GetString());
			bf.close_file();
		}
	}
}

/**
 * @brief 从文件加载用户数据
 * @details 从策略用户数据目录下的JSON文件中加载用户数据到内存
 * 文件名格式为"ud_策略名.json"
 * 如果文件不存在或内容为空或解析错误，则不加载任何数据
 */
void HftStraBaseCtx::load_userdata()
{
	// 构造文件路径
	std::string filename = WtHelper::getStraUsrDatDir();
	filename += "ud_";
	filename += _name;
	filename += ".json";

	// 检查文件是否存在
	if (!StdFile::exists(filename.c_str()))
	{
		return;
	}

	// 读取文件内容
	std::string content;
	StdFile::read_file_content(filename.c_str(), content);
	if (content.empty())
		return;

	// 解析JSON文件
	rj::Document root;
	root.Parse(content.c_str());

	// 检查解析是否成功
	if (root.HasParseError())
		return;

	// 遍历JSON对象中的所有键值对，加载到内存
	for (auto& m : root.GetObject())
	{
		const char* key = m.name.GetString();
		const char* val = m.value.GetString();
		_user_datas[key] = val;
	}
}

void HftStraBaseCtx::do_set_position(const char* stdCode, double qty, double price /* = 0.0 */, const char* userTag /*= ""*/)
{
    // 获取合约的持仓信息
    PosInfo& pInfo = _pos_map[stdCode];
    // 处理价格，如果传入价格为0，则使用当前市场价格
    double curPx = price;
    if (decimal::eq(price, 0.0))
        curPx = _price_map[stdCode];
    // 生成当前时间戳
    uint64_t curTm = (uint64_t)_engine->get_date() * 1000000000 + (uint64_t)_engine->get_raw_time() * 100000 + _engine->get_secs();
    // 获取当前交易日
    uint32_t curTDate = _engine->get_trading_date();

    //手数相等则不用操作了
    if (decimal::eq(pInfo._volume, qty))
        return;

    // 记录持仓变化日志
    log_info("Target position updated: {} -> {}", pInfo._volume, qty);

    // 获取合约信息
    WTSCommodityInfo* commInfo = _engine->get_commodity_info(stdCode);

    //成交价
    double trdPx = curPx;

    // 计算持仓变化量
    double diff = qty - pInfo._volume;
    // 判断是买入还是卖出
    bool isBuy = decimal::gt(diff, 0.0);
    if (decimal::gt(pInfo._volume*diff, 0))//当前持仓和仓位变化方向一致, 增加一条明细, 增加数量即可
    {
        // 更新持仓数量
        pInfo._volume = qty;

        // 如果设置了滑点，调整成交价格
        if (_slippage != 0)
        {
            trdPx += _slippage * commInfo->getPriceTick()*(isBuy ? 1 : -1);
        }

        // 创建新的持仓明细
        DetailInfo dInfo;
        dInfo._long = decimal::gt(qty, 0);
        dInfo._price = trdPx;
        dInfo._volume = abs(diff);
        dInfo._opentime = curTm;
        dInfo._opentdate = curTDate;
        wt_strcpy(dInfo._usertag, userTag);
        pInfo._details.emplace_back(dInfo);

        // 计算并记录手续费
        double fee = commInfo->calcFee(trdPx, abs(diff), 0);
        _fund_info._total_fees += fee;

        // 记录成交日志
        log_trade(stdCode, dInfo._long, true, curTm, trdPx, abs(diff), fee, userTag);
    }
    else
    {//持仓方向和仓位变化方向不一致,需要平仓
        // 计算需要平仓的数量
        double left = abs(diff);

        // 如果设置了滑点，调整成交价格
        if (_slippage != 0)
            trdPx += _slippage * commInfo->getPriceTick()*(isBuy ? 1 : -1);

        // 更新持仓数量
        pInfo._volume = qty;
        // 如果持仓量变为0，清空浮动盈亏
        if (decimal::eq(pInfo._volume, 0))
            pInfo._dynprofit = 0;
        // 用于记录需要清理的持仓明细数量
        uint32_t count = 0;
        // 遍历所有持仓明细，进行平仓处理
        for (auto it = pInfo._details.begin(); it != pInfo._details.end(); it++)
        {
            DetailInfo& dInfo = *it;
            // 计算当前明细需要平仓的数量
            double maxQty = min(dInfo._volume, left);
            if (decimal::eq(maxQty, 0))
                continue;

            // 按比例计算最大盈利和最大亏损
            double maxProf = dInfo._max_profit * maxQty / dInfo._volume;
            double maxLoss = dInfo._max_loss * maxQty / dInfo._volume;

            // 更新当前明细的持仓量
            dInfo._volume -= maxQty;
            left -= maxQty;

            // 如果明细持仓量变为0，标记需要清理
            if (decimal::eq(dInfo._volume, 0))
                count++;

            // 计算平仓盈亏
            double profit = (trdPx - dInfo._price) * maxQty * commInfo->getVolScale();
            if (!dInfo._long)
                profit *= -1;
            // 更新平仓盈亏和浮动盈亏
            pInfo._closeprofit += profit;
            pInfo._dynprofit = pInfo._dynprofit*dInfo._volume / (dInfo._volume + maxQty);//浮盈也要做等比缩放
            _fund_info._total_profit += profit;

            // 计算并记录手续费
            double fee = commInfo->calcFee(trdPx, maxQty, dInfo._opentdate == curTDate ? 2 : 1);
            _fund_info._total_fees += fee;
            //这里写成交记录
            log_trade(stdCode, dInfo._long, false, curTm, trdPx, maxQty, fee, userTag);
            //这里写平仓记录
            log_close(stdCode, dInfo._long, dInfo._opentime, dInfo._price, curTm, trdPx, maxQty, profit, maxProf, maxLoss, pInfo._closeprofit, dInfo._usertag, userTag);

            // 如果已经平仓完所需数量，结束循环
            if (left == 0)
                break;
        }

        //需要清理掉已经平仓完的明细
        while (count > 0)
        {
            auto it = pInfo._details.begin();
            pInfo._details.erase(it);
            count--;
        }

        //最后,如果还有剩余的,则需要反手了
        if (left > 0)
        {
            // 计算反手数量，保持方向一致
            left = left * qty / abs(qty);

            // 创建新的持仓明细
            DetailInfo dInfo;
            dInfo._long = decimal::gt(qty, 0);
            dInfo._price = trdPx;
            dInfo._volume = abs(left);
            dInfo._opentime = curTm;
            dInfo._opentdate = curTDate;
            wt_strcpy(dInfo._usertag, userTag);
            pInfo._details.emplace_back(dInfo);

            //这里还需要写一笔成交记录
            double fee = commInfo->calcFee(trdPx, abs(left), 0);
            _fund_info._total_fees += fee;
            //_engine->mutate_fund(fee, FFT_Fee);
            log_trade(stdCode, dInfo._long, true, curTm, trdPx, abs(left), fee, userTag);
        }
    }
}

/**
 * @brief 更新浮动盈亏
 * @param stdCode 标准化合约代码
 * @param newTick 最新的Tick数据
 * @details 根据最新的市场行情数据更新指定合约的浮动盈亏
 * 对于多头持仓，使用买一价计算浮动盈亏；对于空头持仓，使用卖一价计算浮动盈亏
 * 同时记录每个明细的最大盈利和最大亏损
 */
void HftStraBaseCtx::update_dyn_profit(const char* stdCode, WTSTickData* newTick)
{
	// 在持仓映射中查找指定合约
	auto it = _pos_map.find(stdCode);
	if (it != _pos_map.end())	{
		// 获取持仓信息
		PosInfo& pInfo = (PosInfo&)it->second;
		// 如果持仓量为0，浮动盈亏也为0
		if (pInfo._volume == 0)
		{
			pInfo._dynprofit = 0;
		}
		else
		{
			// 判断是多头还是空头
			bool isLong = decimal::gt(pInfo._volume, 0);
			// 多头使用买一价，空头使用卖一价
			double price = isLong ? newTick->bidprice(0) : newTick->askprice(0);

			// 获取合约信息
			WTSCommodityInfo* commInfo = _engine->get_commodity_info(stdCode);
			// 初始化总浮动盈亏
			double dynprofit = 0;
			// 遍历所有持仓明细
			for (auto pit = pInfo._details.begin(); pit != pInfo._details.end(); pit++)
			{
				// 获取当前明细
				DetailInfo& dInfo = *pit;
				// 计算当前明细的浮动盈亏
				// 盈亏 = 持仓量 * (当前价格 - 开仓价格) * 合约乘数 * 方向系数
				dInfo._profit = dInfo._volume*(price - dInfo._price)*commInfo->getVolScale()*(dInfo._long ? 1 : -1);
				// 更新最大盈利
				if (dInfo._profit > 0)
					dInfo._max_profit = std::max(dInfo._profit, dInfo._max_profit);
				// 更新最大亏损
				else if (dInfo._profit < 0)
					dInfo._max_loss = std::min(dInfo._profit, dInfo._max_loss);

				// 累加总浮动盈亏
				dynprofit += dInfo._profit;
			}

			// 更新持仓的总浮动盈亏
			pInfo._dynprofit = dynprofit;
		}
	}
}

/**
 * @brief 交易会话开始回调
 * @param uTDate 交易日期
 * @details 当交易会话开始时触发此回调
 * 可以在此函数中执行交易会话开始时的初始化操作
 * 注意：当前实现为空，可能需要在子类中重写
 */
void HftStraBaseCtx::on_session_begin(uint32_t uTDate)
{
	// 注意：当前实现为空，需要在子类中重写
}

/**
 * @brief 交易会话结束回调
 * @param uTDate 交易日期
 * @details 当交易会话结束时触发此回调
 * 会计算当日的总盈亏和浮动盈亏，并将结算数据写入资金日志
 */
void HftStraBaseCtx::on_session_end(uint32_t uTDate)
{
	// 获取当前交易日
	uint32_t curDate = uTDate;//_engine->get_trading_date();

	// 初始化总平仓盈亏和总浮动盈亏
	double total_profit = 0;
	double total_dynprofit = 0;

	// 遍历所有持仓，累计总盈亏
	for (auto it = _pos_map.begin(); it != _pos_map.end(); it++)
	{
		const char* stdCode = it->first.c_str();
		const PosInfo& pInfo = it->second;
		total_profit += pInfo._closeprofit;
		total_dynprofit += pInfo._dynprofit;
	}

	//这里要把当日结算的数据写到日志文件里
	//而且这里回测和实盘写法不同, 先留着, 后面来做
	// 如果启用了资金日志并且是数据代理模式，则写入资金日志
	if (_fund_logs && _data_agent)
		_fund_logs->write_file(fmt::format("{},{:.2f},{:.2f},{:.2f},{:.2f}\n", curDate,
			_fund_info._total_profit, _fund_info._total_dynprofit,
			_fund_info._total_profit + _fund_info._total_dynprofit - _fund_info._total_fees, _fund_info._total_fees));
}

/**
 * @brief 记录成交日志
 * @param stdCode 标准化合约代码
 * @param isLong 是否为多头
 * @param isOpen 是否为开仓
 * @param curTime 当前时间戳
 * @param price 成交价格
 * @param qty 成交数量
 * @param fee 手续费
 * @param userTag 用户自定义标签
 * @details 将成交信息写入交易日志文件
 * 日志格式：合约代码,时间戳,方向,开平标志,价格,数量,手续费,用户标签
 * 只有在启用交易日志并且是数据代理模式时才会写入
 */
void HftStraBaseCtx::log_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, double fee, const char* userTag/* = ""*/)
{
	// 如果启用了交易日志并且是数据代理模式，则写入交易日志
	if(_trade_logs && _data_agent)
	{
		// 构建日志内容
		std::stringstream ss;
		ss << stdCode << "," << curTime << "," << (isLong ? "LONG" : "SHORT") << "," << (isOpen ? "OPEN" : "CLOSE")
			<< "," << price << "," << qty << "," << fee << "," << userTag << "\n";
		// 写入日志文件
		_trade_logs->write_file(ss.str());
	}
}

/**
 * @brief 记录平仓日志
 * @param stdCode 标准化合约代码
 * @param isLong 是否为多头
 * @param openTime 开仓时间戳
 * @param openpx 开仓价格
 * @param closeTime 平仓时间戳
 * @param closepx 平仓价格
 * @param qty 平仓数量
 * @param profit 平仓盈亏
 * @param maxprofit 最大盈利
 * @param maxloss 最大亏损
 * @param totalprofit 总盈亏
 * @param enterTag 开仓标签
 * @param exitTag 平仓标签
 * @details 将平仓信息写入平仓日志文件
 * 日志格式：合约代码,方向,开仓时间,开仓价格,平仓时间,平仓价格,数量,盈亏,最大盈利,最大亏损,总盈亏,开仓标签,平仓标签
 * 只有在启用平仓日志并且是数据代理模式时才会写入
 */
void HftStraBaseCtx::log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty, double profit, double maxprofit, double maxloss,
	double totalprofit /* = 0 */, const char* enterTag/* = ""*/, const char* exitTag/* = ""*/)
{
	// 如果启用了平仓日志并且是数据代理模式，则写入平仓日志
	if (_close_logs && _data_agent)
	{
		// 构建日志内容
		std::stringstream ss;
		ss << stdCode << "," << (isLong ? "LONG" : "SHORT") << "," << openTime << "," << openpx
			<< "," << closeTime << "," << closepx << "," << qty << "," << profit << "," << maxprofit << "," << maxloss << ","
			<< totalprofit << "," << enterTag << "," << exitTag << "\n";
		// 写入日志文件
		_close_logs->write_file(ss.str());
	}
}
