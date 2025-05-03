/**
 * @file WtDtHelper.h
 * @project WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief WonderTrader数据处理辅助工具头文件
 * 
 * @details 该文件定义了WonderTrader数据处理辅助工具的接口函数，
 * 包括数据转换、数据导出、数据读取和数据重采样等功能。
 * 这些函数主要用于处理K线、Tick、委托明细、委托队列和成交数据，
 * 支持二进制数据与CSV格式之间的转换，以及数据的读取和重采样操作。
 */
#pragma once

#include "../Includes/WTSTypes.h"

NS_WTP_BEGIN
struct WTSBarStruct;
struct WTSTickStruct;
struct WTSOrdDtlStruct;
struct WTSOrdQueStruct;
struct WTSTransStruct;
NS_WTP_END

USING_NS_WTP;

/**
 * @brief 日志回调函数类型
 * @param message 日志消息字符串
 * @details 用于输出日志信息的回调函数类型
 */
typedef void(PORTER_FLAG *FuncLogCallback)(WtString message);

/**
 * @brief K线数据获取回调函数类型
 * @param bar K线数据结构指针
 * @param count 数据数量
 * @param isLast 是否是最后一批数据
 * @details 用于接收读取的K线数据的回调函数类型
 */
typedef void(PORTER_FLAG *FuncGetBarsCallback)(WTSBarStruct* bar, WtUInt32 count, bool isLast);

/**
 * @brief Tick数据获取回调函数类型
 * @param tick Tick数据结构指针
 * @param count 数据数量
 * @param isLast 是否是最后一批数据
 * @details 用于接收读取的Tick数据的回调函数类型
 */
typedef void(PORTER_FLAG *FuncGetTicksCallback)(WTSTickStruct* tick, WtUInt32 count, bool isLast);

/**
 * @brief 委托明细数据获取回调函数类型
 * @param item 委托明细数据结构指针
 * @param count 数据数量
 * @param isLast 是否是最后一批数据
 * @details 用于接收读取的委托明细数据的回调函数类型
 */
typedef void(PORTER_FLAG *FuncGetOrdDtlCallback)(WTSOrdDtlStruct* item, WtUInt32 count, bool isLast);

/**
 * @brief 委托队列数据获取回调函数类型
 * @param item 委托队列数据结构指针
 * @param count 数据数量
 * @param isLast 是否是最后一批数据
 * @details 用于接收读取的委托队列数据的回调函数类型
 */
typedef void(PORTER_FLAG *FuncGetOrdQueCallback)(WTSOrdQueStruct* item, WtUInt32 count, bool isLast);

/**
 * @brief 成交数据获取回调函数类型
 * @param item 成交数据结构指针
 * @param count 数据数量
 * @param isLast 是否是最后一批数据
 * @details 用于接收读取的成交数据的回调函数类型
 */
typedef void(PORTER_FLAG *FuncGetTransCallback)(WTSTransStruct* item, WtUInt32 count, bool isLast);

/**
 * @brief 数据计数回调函数类型
 * @param dataCnt 数据总数量
 * @details 用于接收数据总数量的回调函数类型，通常在读取数据前调用
 */
typedef void(PORTER_FLAG *FuncCountDataCallback)(WtUInt32 dataCnt);

//改成直接从python传内存块的方式
//typedef bool(PORTER_FLAG *FuncGetBarItem)(WTSBarStruct* curBar,int idx);
//typedef bool(PORTER_FLAG *FuncGetTickItem)(WTSTickStruct* curTick, int idx);

#ifdef __cplusplus
extern "C"
{
#endif
	/**
	 * @brief 将二进制K线数据导出为CSV格式
	 * @param binFolder 二进制数据文件夹路径
	 * @param csvFolder CSV输出文件夹路径
	 * @param strFilter 合约过滤器，支持通配符，默认为空（处理所有合约）
	 * @param cbLogger 日志回调函数，默认为NULL
	 * @details 将指定文件夹中的二进制K线数据文件（.dsb格式）导出为CSV格式，
	 * 方便用户查看和分析。可以通过strFilter参数指定要处理的合约。
	 */
	EXPORT_FLAG	void		dump_bars(WtString binFolder, WtString csvFolder, WtString strFilter = "", FuncLogCallback cbLogger = NULL);

	/**
	 * @brief 将二进制Tick数据导出为CSV格式
	 * @param binFolder 二进制数据文件夹路径
	 * @param csvFolder CSV输出文件夹路径
	 * @param strFilter 合约过滤器，支持通配符，默认为空（处理所有合约）
	 * @param cbLogger 日志回调函数，默认为NULL
	 * @details 将指定文件夹中的二进制Tick数据文件（.dsb格式）导出为CSV格式，
	 * 方便用户查看和分析。可以通过strFilter参数指定要处理的合约。
	 */
	EXPORT_FLAG	void		dump_ticks(WtString binFolder, WtString csvFolder, WtString strFilter = "", FuncLogCallback cbLogger = NULL);

	/**
	 * @brief 将CSV格式的K线数据转换为二进制格式
	 * @param csvFolder CSV数据文件夹路径
	 * @param binFolder 二进制输出文件夹路径
	 * @param period K线周期，如"m1"/"d1"
	 * @param cbLogger 日志回调函数，默认为NULL
	 * @details 将指定文件夹中的CSV格式的K线数据转换为二进制格式（.dsb），
	 * 以便于WonderTrader系统使用。需要指定数据的周期，如分钟线、日线等。
	 */
	EXPORT_FLAG	void		trans_csv_bars(WtString csvFolder, WtString binFolder, WtString period, FuncLogCallback cbLogger = NULL);

	/**
	 * @brief 读取二进制Tick数据文件（.dsb格式）
	 * @param tickFile Tick数据文件路径
	 * @param cb Tick数据获取回调函数
	 * @param cbCnt 数据计数回调函数
	 * @param cbLogger 日志回调函数，默认为NULL
	 * @return WtUInt32 读取到的数据数量
	 * @details 从指定的.dsb格式文件中读取Tick数据，并通过回调函数返回给调用者。
	 * 读取前会先调用cbCnt回调函数通知总数量，然后通过cb回调函数分批返回数据。
	 */
	EXPORT_FLAG	WtUInt32	read_dsb_ticks(WtString tickFile, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL);

	/**
	 * @brief 读取二进制委托明细数据文件（.dsb格式）
	 * @param dataFile 委托明细数据文件路径
	 * @param cb 委托明细数据获取回调函数
	 * @param cbCnt 数据计数回调函数
	 * @param cbLogger 日志回调函数，默认为NULL
	 * @return WtUInt32 读取到的数据数量
	 * @details 从指定的.dsb格式文件中读取委托明细数据，并通过回调函数返回给调用者。
	 * 读取前会先调用cbCnt回调函数通知总数量，然后通过cb回调函数分批返回数据。
	 */
	EXPORT_FLAG	WtUInt32	read_dsb_order_details(WtString dataFile, FuncGetOrdDtlCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL);

	/**
	 * @brief 读取二进制委托队列数据文件（.dsb格式）
	 * @param dataFile 委托队列数据文件路径
	 * @param cb 委托队列数据获取回调函数
	 * @param cbCnt 数据计数回调函数
	 * @param cbLogger 日志回调函数，默认为NULL
	 * @return WtUInt32 读取到的数据数量
	 * @details 从指定的.dsb格式文件中读取委托队列数据，并通过回调函数返回给调用者。
	 * 读取前会先调用cbCnt回调函数通知总数量，然后通过cb回调函数分批返回数据。
	 */
	EXPORT_FLAG	WtUInt32	read_dsb_order_queues(WtString dataFile, FuncGetOrdQueCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL);

	/**
	 * @brief 读取二进制成交数据文件（.dsb格式）
	 * @param dataFile 成交数据文件路径
	 * @param cb 成交数据获取回调函数
	 * @param cbCnt 数据计数回调函数
	 * @param cbLogger 日志回调函数，默认为NULL
	 * @return WtUInt32 读取到的数据数量
	 * @details 从指定的.dsb格式文件中读取成交数据，并通过回调函数返回给调用者。
	 * 读取前会先调用cbCnt回调函数通知总数量，然后通过cb回调函数分批返回数据。
	 */
	EXPORT_FLAG	WtUInt32	read_dsb_transactions(WtString dataFile, FuncGetTransCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL);

	/**
	 * @brief 读取二进制K线数据文件（.dsb格式）
	 * @param barFile K线数据文件路径
	 * @param cb K线数据获取回调函数
	 * @param cbCnt 数据计数回调函数
	 * @param cbLogger 日志回调函数，默认为NULL
	 * @return WtUInt32 读取到的数据数量
	 * @details 从指定的.dsb格式文件中读取K线数据，并通过回调函数返回给调用者。
	 * 读取前会先调用cbCnt回调函数通知总数量，然后通过cb回调函数分批返回数据。
	 */
	EXPORT_FLAG	WtUInt32	read_dsb_bars(WtString barFile, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL);

	/**
	 * @brief 读取内存映射二进制Tick数据文件（.dmb格式）
	 * @param tickFile Tick数据文件路径
	 * @param cb Tick数据获取回调函数
	 * @param cbCnt 数据计数回调函数
	 * @param cbLogger 日志回调函数，默认为NULL
	 * @return WtUInt32 读取到的数据数量
	 * @details 从指定的.dmb格式文件中读取Tick数据，并通过回调函数返回给调用者。
	 * .dmb格式是内存映射二进制格式，读取效率更高。
	 * 读取前会先调用cbCnt回调函数通知总数量，然后通过cb回调函数分批返回数据。
	 */
	EXPORT_FLAG	WtUInt32	read_dmb_ticks(WtString tickFile, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL);

	/**
	 * @brief 读取内存映射二进制K线数据文件（.dmb格式）
	 * @param barFile K线数据文件路径
	 * @param cb K线数据获取回调函数
	 * @param cbCnt 数据计数回调函数
	 * @param cbLogger 日志回调函数，默认为NULL
	 * @return WtUInt32 读取到的数据数量
	 * @details 从指定的.dmb格式文件中读取K线数据，并通过回调函数返回给调用者。
	 * .dmb格式是内存映射二进制格式，读取效率更高。
	 * 读取前会先调用cbCnt回调函数通知总数量，然后通过cb回调函数分批返回数据。
	 */
	EXPORT_FLAG	WtUInt32	read_dmb_bars(WtString barFile, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL);

	//EXPORT_FLAG bool		trans_bars(WtString barFile, FuncGetBarItem getter, int count, WtString period, FuncLogCallback cbLogger = NULL);
	//EXPORT_FLAG bool		trans_ticks(WtString tickFile, FuncGetTickItem getter, int count, FuncLogCallback cbLogger = NULL);

	/**
	 * @brief 将K线数据存储为二进制文件
	 * @param barFile 输出文件路径
	 * @param firstBar 第一个K线数据结构指针，数据数组的起始地址
	 * @param count 数据数量
	 * @param period K线周期，如"m1"/"d1"
	 * @param cbLogger 日志回调函数，默认为NULL
	 * @return bool 存储是否成功
	 * @details 将内存中的K线数据数组存储为二进制文件（.dsb格式），
	 * 以便于WonderTrader系统使用。需要指定数据的周期，如分钟线、日线等。
	 */
	EXPORT_FLAG bool		store_bars(WtString barFile, WTSBarStruct* firstBar, int count, WtString period, FuncLogCallback cbLogger = NULL);

	/**
	 * @brief 将Tick数据存储为二进制文件
	 * @param tickFile 输出文件路径
	 * @param firstTick 第一个Tick数据结构指针，数据数组的起始地址
	 * @param count 数据数量
	 * @param cbLogger 日志回调函数，默认为NULL
	 * @return bool 存储是否成功
	 * @details 将内存中的Tick数据数组存储为二进制文件（.dsb格式），
	 * 以便于WonderTrader系统使用。
	 */
	EXPORT_FLAG bool		store_ticks(WtString tickFile, WTSTickStruct* firstTick, int count, FuncLogCallback cbLogger = NULL);

	/**
	 * @brief 将委托明细数据存储为二进制文件（股票Level2数据）
	 * @param tickFile 输出文件路径
	 * @param firstItem 第一个委托明细数据结构指针，数据数组的起始地址
	 * @param count 数据数量
	 * @param cbLogger 日志回调函数，默认为NULL
	 * @return bool 存储是否成功
	 * @details 将内存中的委托明细数据数组存储为二进制文件（.dsb格式），
	 * 主要用于股票Level2数据的存储。
	 */
	EXPORT_FLAG bool		store_order_details(WtString tickFile, WTSOrdDtlStruct* firstItem, int count, FuncLogCallback cbLogger = NULL);

	/**
	 * @brief 将委托队列数据存储为二进制文件（股票Level2数据）
	 * @param tickFile 输出文件路径
	 * @param firstItem 第一个委托队列数据结构指针，数据数组的起始地址
	 * @param count 数据数量
	 * @param cbLogger 日志回调函数，默认为NULL
	 * @return bool 存储是否成功
	 * @details 将内存中的委托队列数据数组存储为二进制文件（.dsb格式），
	 * 主要用于股票Level2数据的存储。
	 */
	EXPORT_FLAG bool		store_order_queues(WtString tickFile, WTSOrdQueStruct* firstItem, int count, FuncLogCallback cbLogger = NULL);

	/**
	 * @brief 将成交数据存储为二进制文件（股票Level2数据）
	 * @param tickFile 输出文件路径
	 * @param firstItem 第一个成交数据结构指针，数据数组的起始地址
	 * @param count 数据数量
	 * @param cbLogger 日志回调函数，默认为NULL
	 * @return bool 存储是否成功
	 * @details 将内存中的成交数据数组存储为二进制文件（.dsb格式），
	 * 主要用于股票Level2数据的存储。
	 */
	EXPORT_FLAG bool		store_transactions(WtString tickFile, WTSTransStruct* firstItem, int count, FuncLogCallback cbLogger = NULL);

	/**
	 * @brief 对K线数据进行重采样
	 * @param barFile 输入K线数据文件路径
	 * @param cb K线数据获取回调函数，用于返回重采样后的数据
	 * @param cbCnt 数据计数回调函数
	 * @param fromTime 开始时间
	 * @param endTime 结束时间
	 * @param period 目标K线周期，如"m5"/"d1"
	 * @param times 周期倍数，如将分钟线采样为5分钟线，则为5
	 * @param sessInfo 交易时段信息
	 * @param cbLogger 日志回调函数，默认为NULL
	 * @param bAlignSec 是否按秒对齐，默认为false
	 * @return WtUInt32 重采样后的数据数量
	 * @details 将指定文件中的K线数据进行重采样，生成新的周期的K线数据。
	 * 例如将分钟线采样为5分钟线、日线等。重采样的数据通过回调函数返回给调用者。
	 */
	EXPORT_FLAG WtUInt32	resample_bars(WtString barFile, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt, 
		WtUInt64 fromTime, WtUInt64 endTime, WtString period, WtUInt32 times, WtString sessInfo, FuncLogCallback cbLogger = NULL, bool bAlignSec = false);
#ifdef __cplusplus
}
#endif