/*!
 * \file HisDataMgr.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 历史数据管理器实现文件
 * \details 实现了历史数据管理器类的函数，处理各类历史数据的加载和管理
 */

#include "HisDataMgr.h"
#include "WtHelper.h"
#include "../Share/DLLHelper.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../WTSTools/WTSLogger.h"

/**
 * @brief 实现日志输出回调函数
 * @param ll 日志级别
 * @param message 日志消息
 * @details 将读取器的日志输出到全局日志系统
 */
void HisDataMgr::reader_log(WTSLogLevel ll, const char* message)
{
	// 直接调用全局日志函数将读取器的日志写入日志系统
	WTSLogger::log_raw(ll, message);
}

/**
 * @brief 初始化历史数据管理器
 * @param cfg 配置对象指针
 * @return 是否初始化成功
 * @details 加载数据读取模块并初始化数据读取器
 */
bool HisDataMgr::init(WTSVariant* cfg)
{
	// 获取模块名称，如果没有配置则使用默认的WtDataStorage
	std::string module = cfg->getCString("module");
	if (module.empty())
		module = WtHelper::getInstDir() + DLLHelper::wrap_module("WtDataStorage");
	else
		module = WtHelper::getInstDir() + DLLHelper::wrap_module(module.c_str());

	// 加载数据存储模块库
	DllHandle libParser = DLLHelper::load_library(module.c_str());
	if (libParser)
	{
		// 获取创建读取器的函数指针
		FuncCreateBtDtReader pFuncCreator = (FuncCreateBtDtReader)DLLHelper::get_symbol(libParser, "createBtDtReader");
		if (pFuncCreator == NULL)
		{
			WTSLogger::error("Initializing of backtest data reader failed: function createBtDtReader not found...");
		}

		// 如果函数指针获取成功，创建读取器实例
		if (pFuncCreator)
		{
			_reader = pFuncCreator();
		}

		WTSLogger::debug("Back data storage module {} loaded", module);
	}
	else
	{
		WTSLogger::error("Loading module back data storage module {} failed", module);

	}

	// 初始化读取器实例，传入配置和当前对象作为回调接收器
	_reader->init(cfg, this);

	return true;
}

/**
 * @brief 加载K线原始数据
 * @param exchg 交易所代码
 * @param code 合约代码
 * @param period K线周期
 * @param cb 数据加载完成后的回调函数
 * @return 是否加载成功
 * @details 从数据读取器加载指定合约的K线原始数据
 */
bool HisDataMgr::load_raw_bars(const char* exchg, const char* code, WTSKlinePeriod period, FuncLoadDataCallback cb)
{
	// 检查读取器是否初始化
	if(_reader == NULL)
	{
		WTSLogger::log_raw(LL_ERROR, "Backtest Data Reader not initialized");
		return false;
	}

	// 准备接收数据的缓冲区
	std::string buffer;
	// 调用读取器加载K线数据
	bool bSucc = _reader->read_raw_bars(exchg, code, period, buffer);
	// 如果加载成功，执行回调函数
	if (bSucc)
		cb(buffer);
	return bSucc;
}

/**
 * @brief 加载tick原始数据
 * @param exchg 交易所代码
 * @param code 合约代码
 * @param uDate 交易日期(YYYYMMDD格式)
 * @param cb 数据加载完成后的回调函数
 * @return 是否加载成功
 * @details 从数据读取器加载指定合约和交易日的tick原始数据
 */
bool HisDataMgr::load_raw_ticks(const char* exchg, const char* code, uint32_t uDate, FuncLoadDataCallback cb)
{
	// 检查读取器是否初始化
	if (_reader == NULL)
	{
		WTSLogger::log_raw(LL_ERROR, "Backtest Data Reader not initialized");
		return false;
	}

	// 准备接收数据的缓冲区
	std::string buffer;
	// 调用读取器加载tick数据
	bool bSucc = _reader->read_raw_ticks(exchg, code, uDate, buffer);
	// 如果加载成功，执行回调函数
	if (bSucc)
		cb(buffer);
	return bSucc;
}

/**
 * @brief 加载分笔成交原始数据
 * @param exchg 交易所代码
 * @param code 合约代码
 * @param uDate 交易日期(YYYYMMDD格式)
 * @param cb 数据加载完成后的回调函数
 * @return 是否加载成功
 * @details 从数据读取器加载指定合约和交易日的分笔成交原始数据
 */
bool HisDataMgr::load_raw_trans(const char* exchg, const char* code, uint32_t uDate, FuncLoadDataCallback cb)
{
	// 检查读取器是否初始化
	if (_reader == NULL)
	{
		WTSLogger::log_raw(LL_ERROR, "Backtest Data Reader not initialized");
		return false;
	}

	// 准备接收数据的缓冲区
	std::string buffer;
	// 调用读取器加载分笔成交数据
	bool bSucc = _reader->read_raw_transactions(exchg, code, uDate, buffer);
	// 如果加载成功，执行回调函数
	if (bSucc)
		cb(buffer);
	return bSucc;
}

/**
 * @brief 加载委托队列原始数据
 * @param exchg 交易所代码
 * @param code 合约代码
 * @param uDate 交易日期(YYYYMMDD格式)
 * @param cb 数据加载完成后的回调函数
 * @return 是否加载成功
 * @details 从数据读取器加载指定合约和交易日的委托队列原始数据
 */
bool HisDataMgr::load_raw_ordque(const char* exchg, const char* code, uint32_t uDate, FuncLoadDataCallback cb)
{
	// 检查读取器是否初始化
	if (_reader == NULL)
	{
		WTSLogger::log_raw(LL_ERROR, "Backtest Data Reader not initialized");
		return false;
	}

	// 准备接收数据的缓冲区
	std::string buffer;
	// 调用读取器加载委托队列数据
	bool bSucc = _reader->read_raw_order_queues(exchg, code, uDate, buffer);
	// 如果加载成功，执行回调函数
	if (bSucc)
		cb(buffer);
	return bSucc;
}

/**
 * @brief 加载逐笔成交原始数据
 * @param exchg 交易所代码
 * @param code 合约代码
 * @param uDate 交易日期(YYYYMMDD格式)
 * @param cb 数据加载完成后的回调函数
 * @return 是否加载成功
 * @details 从数据读取器加载指定合约和交易日的逐笔成交原始数据
 */
bool HisDataMgr::load_raw_orddtl(const char* exchg, const char* code, uint32_t uDate, FuncLoadDataCallback cb)
{
	// 检查读取器是否初始化
	if (_reader == NULL)
	{
		WTSLogger::log_raw(LL_ERROR, "Backtest Data Reader not initialized");
		return false;
	}

	// 准备接收数据的缓冲区
	std::string buffer;
	// 调用读取器加载逐笔成交数据
	bool bSucc = _reader->read_raw_order_details(exchg, code, uDate, buffer);
	// 如果加载成功，执行回调函数
	if (bSucc)
		cb(buffer);
	return bSucc;
}