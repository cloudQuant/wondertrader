/*!
 * \file HisDataMgr.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 历史数据管理器头文件
 * \details 定义了历史数据管理器类，负责加载和管理回测所需的历史数据
 */

#pragma once
#include <functional>
#include "../Includes/IBtDtReader.h"

/**
 * @brief 数据加载回调函数类型
 * @details 用于在数据加载完成后回调处理数据的函数类型，参数为包含加载数据的字符串引用
 */
typedef std::function<void(std::string&)> FuncLoadDataCallback;

NS_WTP_BEGIN
class WTSVariant;
NS_WTP_END

USING_NS_WTP;

/**
 * @brief 历史数据管理器类
 * @details 继承自IBtDtReaderSink接口，负责管理和加载回测需要的各类历史数据，
 *          支持K线、tick、委托队列、逐笔成交和分笔成交等多种数据格式
 */
class HisDataMgr : public IBtDtReaderSink
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化历史数据读取器指针为空
	 */
	HisDataMgr() :_reader(NULL) {}
	
	/**
	 * @brief 析构函数
	 */
	~HisDataMgr(){}

public:
	/**
	 * @brief 实现日志输出回调函数
	 * @param ll 日志级别
	 * @param message 日志消息
	 * @details 实现IBtDtReaderSink接口中的日志回调函数，用于将读取器的日志写入全局日志
	 */
	virtual void reader_log(WTSLogLevel ll, const char* message) override;

public:
	/**
	 * @brief 初始化历史数据管理器
	 * @param cfg 配置对象指针
	 * @return 是否初始化成功
	 * @details 根据配置对象加载数据读取模块并初始化读取器
	 */
	bool	init(WTSVariant* cfg);

	/**
	 * @brief 加载K线原始数据
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @param period K线周期
	 * @param cb 数据加载完成后的回调函数
	 * @return 是否加载成功
	 * @details 加载指定合约的K线原始数据并通过回调返回结果
	 */
	bool	load_raw_bars(const char* exchg, const char* code, WTSKlinePeriod period, FuncLoadDataCallback cb);

	/**
	 * @brief 加载tick原始数据
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @param uDate 交易日期(YYYYMMDD格式)
	 * @param cb 数据加载完成后的回调函数
	 * @return 是否加载成功
	 * @details 加载指定合约和交易日的tick原始数据并通过回调返回结果
	 */
	bool	load_raw_ticks(const char* exchg, const char* code, uint32_t uDate, FuncLoadDataCallback cb);

	/**
	 * @brief 加载委托队列原始数据
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @param uDate 交易日期(YYYYMMDD格式)
	 * @param cb 数据加载完成后的回调函数
	 * @return 是否加载成功
	 * @details 加载指定合约和交易日的委托队列原始数据并通过回调返回结果
	 */
	bool	load_raw_ordque(const char* exchg, const char* code, uint32_t uDate, FuncLoadDataCallback cb);

	/**
	 * @brief 加载逐笔成交原始数据
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @param uDate 交易日期(YYYYMMDD格式)
	 * @param cb 数据加载完成后的回调函数
	 * @return 是否加载成功
	 * @details 加载指定合约和交易日的逐笔成交原始数据并通过回调返回结果
	 */
	bool	load_raw_orddtl(const char* exchg, const char* code, uint32_t uDate, FuncLoadDataCallback cb);

	/**
	 * @brief 加载分笔成交原始数据
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @param uDate 交易日期(YYYYMMDD格式)
	 * @param cb 数据加载完成后的回调函数
	 * @return 是否加载成功
	 * @details 加载指定合约和交易日的分笔成交原始数据并通过回调返回结果
	 */
	bool	load_raw_trans(const char* exchg, const char* code, uint32_t uDate, FuncLoadDataCallback cb);

private:
	/**
	 * @brief 回测数据读取器接口指针
	 * @details 指向实际的数据读取器实现，用于执行具体的数据读取操作
	 */
	IBtDtReader*	_reader;
};

