/*!
 * \file ExpDumper.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 历史数据导出器的实现文件
 * \details 实现了ExpDumper类的各种导出方法，通过将调用转发给WtDtRunner完成实际导出功能
 *          包括对K线数据、适价数据、委托队列、委托明细和成交数据的导出实现
 */
#include "ExpDumper.h"
#include "WtDtRunner.h"

/**
 * @brief 获取WtDtRunner全局实例的外部函数声明
 * @details 该函数由外部实现，用于获取WtDtRunner的全局单例
 * @return WtDtRunner& WtDtRunner的引用
 */
extern WtDtRunner& getRunner();

/**
 * @brief 导出委托队列历史数据
 * @details 将指定日期的委托队列数据导出到存储引擎
 * @param stdCode 标准合约代码，格式如 SHFE.rb.HOT
 * @param uDate 交易日，格式为YYYYMMDD
 * @param items 委托队列数据数组
 * @param count 数组长度
 * @return 导出是否成功
 */
bool ExpDumper::dumpHisOrdQue(const char* stdCode, uint32_t uDate, WTSOrdQueStruct* items, uint32_t count)
{
	// 调用WtDtRunner的相应方法来完成实际导出操作
	return getRunner().dumpHisOrdQue(_id.c_str(), stdCode, uDate, items, count);
}

/**
 * @brief 导出委托明细历史数据
 * @details 将指定日期的委托明细数据导出到存储引擎
 * @param stdCode 标准合约代码，格式如 SHFE.rb.HOT
 * @param uDate 交易日，格式为YYYYMMDD
 * @param items 委托明细数据数组
 * @param count 数组长度
 * @return 导出是否成功
 */
bool ExpDumper::dumpHisOrdDtl(const char* stdCode, uint32_t uDate, WTSOrdDtlStruct* items, uint32_t count)
{
	// 调用WtDtRunner的相应方法来完成实际导出操作
	return getRunner().dumpHisOrdDtl(_id.c_str(), stdCode, uDate, items, count);
}

/**
 * @brief 导出成交历史数据
 * @details 将指定日期的成交数据导出到存储引擎
 * @param stdCode 标准合约代码，格式如 SHFE.rb.HOT
 * @param uDate 交易日，格式为YYYYMMDD
 * @param items 成交数据数组
 * @param count 数组长度
 * @return 导出是否成功
 */
bool ExpDumper::dumpHisTrans(const char* stdCode, uint32_t uDate, WTSTransStruct* items, uint32_t count)
{
	// 调用WtDtRunner的相应方法来完成实际导出操作
	return getRunner().dumpHisTrans(_id.c_str(), stdCode, uDate, items, count);
}

/**
 * @brief 导出K线历史数据
 * @details 将历史K线数据导出到存储引擎
 * @param stdCode 标准合约代码，格式如 SHFE.rb.HOT
 * @param period 周期，如m1（一分钟）/m5（五分钟）/d1（日线）等
 * @param bars K线数据数组
 * @param count 数组长度
 * @return 导出是否成功
 */
bool ExpDumper::dumpHisBars(const char* stdCode, const char* period, WTSBarStruct* bars, uint32_t count)
{
	// 调用WtDtRunner的相应方法来完成实际导出操作
	return getRunner().dumpHisBars(_id.c_str(), stdCode, period, bars, count);
}

/**
 * @brief 导出适价历史数据
 * @details 将指定日期的适价数据导出到存储引擎
 * @param stdCode 标准合约代码，格式如 SHFE.rb.HOT
 * @param uDate 交易日，格式为YYYYMMDD
 * @param ticks 适价数据数组
 * @param count 数组长度
 * @return 导出是否成功
 */
bool ExpDumper::dumpHisTicks(const char* stdCode, uint32_t uDate, WTSTickStruct* ticks, uint32_t count)
{
	// 调用WtDtRunner的相应方法来完成实际导出操作
	return getRunner().dumpHisTicks(_id.c_str(), stdCode, uDate, ticks, count);
}
