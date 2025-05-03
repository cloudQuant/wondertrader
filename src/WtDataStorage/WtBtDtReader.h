/*!
 * \file WtBtDtReader.h
 * \project WonderTrader
 *
 * \author Wesley
 * 
 * \brief WonderTrader回测数据读取器定义
 * \details 该文件定义了WtBtDtReader类，用于读取回测所需的各类原始行情数据
 *          支持读取不同时间周期的K线数据、Tick数据、逐笔成交/委托数据等
 */

#pragma once
#include <string>
#include <stdint.h>

#include "DataDefine.h"

#include "../Includes/FasterDefs.h"
#include "../Includes/IBtDtReader.h"

#include "../Share/BoostMappingFile.hpp"
#include "../Share/StdUtils.hpp"

NS_WTP_BEGIN
class WTSVariant;
class WTSTickSlice;
class WTSKlineSlice;
class WTSOrdDtlSlice;
class WTSOrdQueSlice;
class WTSTransSlice;
class WTSArray;

class IBaseDataMgr;
class IHotMgr;
typedef std::shared_ptr<BoostMappingFile> BoostMFPtr;

/**
 * @brief WonderTrader回测数据读取器类
 * @details 为回测引擎提供各类原始行情数据的读取功能
 *          继承自IBtDtReader接口，实现了标准的回测数据读取接口
 *          主要用于从存储介质中读取回测所需的原始数据
 */
class WtBtDtReader : public IBtDtReader
{
public:
	/**
	 * @brief 构造函数
	 */
	WtBtDtReader();
	/**
	 * @brief 析构函数
	 */
	virtual ~WtBtDtReader();	

//////////////////////////////////////////////////////////////////////////
//IBtDtReader
public:
	/**
	 * @brief 初始化函数
	 * @param cfg 配置信息
	 * @param sink 数据接收器
	 */
	virtual void init(WTSVariant* cfg, IBtDtReaderSink* sink);

	/**
	 * @brief 读取原始K线数据
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @param period K线周期
	 * @param buffer 输出参数，用于存储读取的原始数据
	 * @return 读取成功返回true，失败返回false
	 * @details 从存储介质中读取指定合约、指定周期的K线数据
	 *          读取到的原始数据保存到buffer参数中返回
	 */
	virtual bool read_raw_bars(const char* exchg, const char* code, WTSKlinePeriod period, std::string& buffer) override;
	/**
	 * @brief 读取原始Tick数据
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @param uDate 交易日期(YYYYMMDD格式)
	 * @param buffer 输出参数，用于存储读取的原始数据
	 * @return 读取成功返回true，失败返回false
	 * @details 从存储介质中读取指定合约、指定日期的Tick数据
	 *          读取到的原始数据保存到buffer参数中返回
	 */
	virtual bool read_raw_ticks(const char* exchg, const char* code, uint32_t uDate, std::string& buffer) override;

	/**
	 * @brief 读取原始逐笔委托数据
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @param uDate 交易日期(YYYYMMDD格式)
	 * @param buffer 输出参数，用于存储读取的原始数据
	 * @return 读取成功返回true，失败返回false
	 * @details 从存储介质中读取指定合约、指定日期的逐笔委托数据
	 *          这些数据可用于分析市场深度和委托流
	 */
	virtual bool read_raw_order_details(const char* exchg, const char* code, uint32_t uDate, std::string& buffer) override;
	/**
	 * @brief 读取原始委托队列数据
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @param uDate 交易日期(YYYYMMDD格式)
	 * @param buffer 输出参数，用于存储读取的原始数据
	 * @return 读取成功返回true，失败返回false
	 * @details 从存储介质中读取指定合约、指定日期的委托队列数据
	 *          这些数据提供了市场深度信息，基于委托的排队数据
	 */
	virtual bool read_raw_order_queues(const char* exchg, const char* code, uint32_t uDate, std::string& buffer) override;
	/**
	 * @brief 读取原始逐笔成交数据
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @param uDate 交易日期(YYYYMMDD格式)
	 * @param buffer 输出参数，用于存储读取的原始数据
	 * @return 读取成功返回true，失败返回false
	 * @details 从存储介质中读取指定合约、指定日期的逐笔成交数据
	 *          这些数据可用于分析市场活跃度和成交流
	 */
	virtual bool read_raw_transactions(const char* exchg, const char* code, uint32_t uDate, std::string& buffer) override;

private:
	std::string		_base_dir;  /**< 原始数据存储的基础路径 */
};

NS_WTP_END