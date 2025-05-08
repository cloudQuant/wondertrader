/*!
 * \file ExpDumper.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 历史数据导出器的头文件
 * \details 定义了ExpDumper类，用于将各种历史数据导出到存储引擎
 *          实现了IHisDataDumper接口，支持K线、适价、委托队列、委托明细、成交明细等数据的导出
 */
#pragma once
#include "../Includes/IDataWriter.h"

USING_NS_WTP;

/**
 * @brief 历史数据导出器类
 * @details 该类实现了IHisDataDumper接口，用于将不同类型的历史数据导出到存储引擎
 *          包括K线数据、适价数据、委托队列、委托明细和成交数据
 *          实际实现通过调用WtDtRunner的相应方法完成
 */
class ExpDumper : public IHisDataDumper
{
public:
	/**
	 * @brief 构造函数
	 * @details 创建一个历史数据导出器实例
	 * @param id 导出器标识符，用于在处理多个导出器时区分
	 */
	ExpDumper(const char* id) :_id(id) {}

	/**
	 * @brief 析构函数
	 * @details 虚函数，用于清理资源
	 */
	virtual ~ExpDumper() {}

public:
	/**
	 * @brief 导出K线历史数据
	 * @details 将历史K线数据导出到存储引擎
	 * @param stdCode 标准合约代码，格式如 SHFE.rb.HOT
	 * @param period 周期，如m1/m5/d1等
	 * @param bars K线数据数组
	 * @param count 数组长度
	 * @return 导出是否成功
	 */
	virtual bool dumpHisBars(const char* stdCode, const char* period, WTSBarStruct* bars, uint32_t count) override;

	/**
	 * @brief 导出适价历史数据
	 * @details 将指定日期的适价数据导出到存储引擎
	 * @param stdCode 标准合约代码，格式如 SHFE.rb.HOT
	 * @param uDate 交易日，格式为YYYYMMDD
	 * @param ticks 适价数据数组
	 * @param count 数组长度
	 * @return 导出是否成功
	 */
	virtual bool dumpHisTicks(const char* stdCode, uint32_t uDate, WTSTickStruct* ticks, uint32_t count) override;

	/**
	 * @brief 导出委托队列历史数据
	 * @details 将指定日期的委托队列数据导出到存储引擎
	 * @param stdCode 标准合约代码，格式如 SHFE.rb.HOT
	 * @param uDate 交易日，格式为YYYYMMDD
	 * @param items 委托队列数据数组
	 * @param count 数组长度
	 * @return 导出是否成功
	 */
	virtual bool dumpHisOrdQue(const char* stdCode, uint32_t uDate, WTSOrdQueStruct* items, uint32_t count) override;

	/**
	 * @brief 导出委托明细历史数据
	 * @details 将指定日期的委托明细数据导出到存储引擎
	 * @param stdCode 标准合约代码，格式如 SHFE.rb.HOT
	 * @param uDate 交易日，格式为YYYYMMDD
	 * @param items 委托明细数据数组
	 * @param count 数组长度
	 * @return 导出是否成功
	 */
	virtual bool dumpHisOrdDtl(const char* stdCode, uint32_t uDate, WTSOrdDtlStruct* items, uint32_t count) override;

	/**
	 * @brief 导出成交历史数据
	 * @details 将指定日期的成交数据导出到存储引擎
	 * @param stdCode 标准合约代码，格式如 SHFE.rb.HOT
	 * @param uDate 交易日，格式为YYYYMMDD
	 * @param items 成交数据数组
	 * @param count 数组长度
	 * @return 导出是否成功
	 */
	virtual bool dumpHisTrans(const char* stdCode, uint32_t uDate, WTSTransStruct* items, uint32_t count) override;

private:
	std::string	_id;  ///< 导出器的唯一标识符，用于区分不同的导出器实例
};

