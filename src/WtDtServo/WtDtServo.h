/*!
 * \file WtDtServo.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据服务对外接口定义
 * 
 * \details 本文件定义了WtDtServo模块对外提供的C接口函数，
 * 包括模块初始化、数据查询、数据订阅等功能。
 * 这些接口可以被其他语言调用，使用C接口保证了跨语言兼容性。
 */
#pragma once
#include "PorterDefs.h"


#ifdef __cplusplus
extern "C"
{
#endif

	/**
	 * @brief 初始化数据服务模块
	 * @param cfgFile 配置文件路径或配置内容字符串
	 * @param isFile 是否为文件路径，如果为true则cfgFile为文件路径，否则为配置内容字符串
	 * @param logCfg 日志配置文件路径
	 * @param cbTick Tick数据回调函数，用于接收实时Tick数据
	 * @param cbBar K线数据回调函数，用于接收实时K线数据
	 * 
	 * @details 该函数用于初始化数据服务模块，加载配置文件，初始化日志系统，
	 * 并设置实时数据的回调函数。这个函数必须在使用其他接口前调用。
	 */
	EXPORT_FLAG void		initialize(WtString cfgFile, bool isFile, WtString logCfg, FuncOnTickCallback cbTick, FuncOnBarCallback cbBar);

	/**
	 * @brief 获取数据服务模块版本号
	 * @return WtString 版本号字符串
	 * 
	 * @details 该函数返回数据服务模块的版本号字符串，可用于在运行时检查模块版本。
	 */
	EXPORT_FLAG	WtString	get_version();

	/**
	 * @brief 根据时间范围获取K线数据
	 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
	 * @param period K线周期，如"m1"/"d1"
	 * @param beginTime 开始时间，格式为YYYYMMDDHHmmss
	 * @param endTime 结束时间，格式为YYYYMMDDHHmmss
	 * @param cb K线数据获取回调函数，用于接收查询结果
	 * @param cbCnt 数据计数回调函数，用于接收数据总数
	 * @return WtUInt32 返回查询到的数据数量
	 * 
	 * @details 该函数根据指定的合约代码、周期和时间范围查询K线数据，
	 * 并通过回调函数返回查询结果。数据可能分批返回，每批数据通过cb回调函数传递。
	 */
	EXPORT_FLAG	WtUInt32	get_bars_by_range(const char* stdCode, const char* period, WtUInt64 beginTime, WtUInt64 endTime, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt);

	/**
	 * @brief 根据时间范围获取Tick数据
	 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
	 * @param beginTime 开始时间，格式为YYYYMMDDHHmmss
	 * @param endTime 结束时间，格式为YYYYMMDDHHmmss
	 * @param cb Tick数据获取回调函数，用于接收查询结果
	 * @param cbCnt 数据计数回调函数，用于接收数据总数
	 * @return WtUInt32 返回查询到的数据数量
	 * 
	 * @details 该函数根据指定的合约代码和时间范围查询Tick数据，
	 * 并通过回调函数返回查询结果。数据可能分批返回，每批数据通过cb回调函数传递。
	 */
	EXPORT_FLAG	WtUInt32	get_ticks_by_range(const char* stdCode, WtUInt64 beginTime, WtUInt64 endTime, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt);

	/**
	 * @brief 根据数量获取K线数据
	 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
	 * @param period K线周期，如"m1"/"d1"
	 * @param count 要获取的数据数量
	 * @param endTime 结束时间，格式为YYYYMMDDHHmmss，如果为0则表示当前时间
	 * @param cb K线数据获取回调函数，用于接收查询结果
	 * @param cbCnt 数据计数回调函数，用于接收数据总数
	 * @return WtUInt32 返回查询到的数据数量
	 * 
	 * @details 该函数根据指定的合约代码、周期和数量查询K线数据，
	 * 返回从结束时间往前数的指定数量的K线数据。
	 * 数据可能分批返回，每批数据通过cb回调函数传递。
	 */
	EXPORT_FLAG	WtUInt32	get_bars_by_count(const char* stdCode, const char* period, WtUInt32 count, WtUInt64 endTime, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt);

	/**
	 * @brief 根据数量获取Tick数据
	 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
	 * @param count 要获取的数据数量
	 * @param endTime 结束时间，格式为YYYYMMDDHHmmss，如果为0则表示当前时间
	 * @param cb Tick数据获取回调函数，用于接收查询结果
	 * @param cbCnt 数据计数回调函数，用于接收数据总数
	 * @return WtUInt32 返回查询到的数据数量
	 * 
	 * @details 该函数根据指定的合约代码和数量查询Tick数据，
	 * 返回从结束时间往前数的指定数量的Tick数据。
	 * 数据可能分批返回，每批数据通过cb回调函数传递。
	 */
	EXPORT_FLAG	WtUInt32	get_ticks_by_count(const char* stdCode, WtUInt32 count, WtUInt64 endTime, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt);

	/**
	 * @brief 根据日期获取Tick数据
	 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
	 * @param uDate 交易日期，格式为YYYYMMDD
	 * @param cb Tick数据获取回调函数，用于接收查询结果
	 * @param cbCnt 数据计数回调函数，用于接收数据总数
	 * @return WtUInt32 返回查询到的数据数量
	 * 
	 * @details 该函数根据指定的合约代码和交易日期查询当天的所有Tick数据。
	 * 数据可能分批返回，每批数据通过cb回调函数传递。
	 */
	EXPORT_FLAG	WtUInt32	get_ticks_by_date(const char* stdCode, WtUInt32 uDate, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt);

	/**
	 * @brief 根据日期获取秒线K线数据
	 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
	 * @param secs 秒线周期，如设置为5表示5秒线
	 * @param uDate 交易日期，格式为YYYYMMDD
	 * @param cb K线数据获取回调函数，用于接收查询结果
	 * @param cbCnt 数据计数回调函数，用于接收数据总数
	 * @return WtUInt32 返回查询到的数据数量
	 * 
	 * @details 该函数根据指定的合约代码、秒线周期和交易日期查询当天的所有秒线K线数据。
	 * 数据可能分批返回，每批数据通过cb回调函数传递。
	 */
	EXPORT_FLAG	WtUInt32	get_sbars_by_date(const char* stdCode, WtUInt32 secs, WtUInt32 uDate, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt);

	/**
	 * @brief 根据日期获取K线数据
	 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
	 * @param period K线周期，如"m1"/"d1"
	 * @param uDate 交易日期，格式为YYYYMMDD
	 * @param cb K线数据获取回调函数，用于接收查询结果
	 * @param cbCnt 数据计数回调函数，用于接收数据总数
	 * @return WtUInt32 返回查询到的数据数量
	 * 
	 * @details 该函数根据指定的合约代码、K线周期和交易日期查询当天的所有K线数据。
	 * 数据可能分批返回，每批数据通过cb回调函数传递。
	 */
	EXPORT_FLAG	WtUInt32	get_bars_by_date(const char* stdCode, const char* period, WtUInt32 uDate, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt);

	/**
	 * @brief 订阅Tick数据
	 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
	 * @param bReplace 是否替换已有订阅，如果为true则清空已有订阅后添加新订阅
	 * 
	 * @details 该函数用于订阅指定合约的实时Tick数据。
	 * 当有新的Tick数据到达时，会通过初始化时设置的FuncOnTickCallback回调函数推送给调用者。
	 */
	EXPORT_FLAG void		subscribe_tick(const char* stdCode, bool bReplace);

	/**
	 * @brief 订阅K线数据
	 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
	 * @param period K线周期，如"m1"/"d1"
	 * 
	 * @details 该函数用于订阅指定合约的指定周期的实时K线数据。
	 * 当有新的K线数据生成或更新时，会通过初始化时设置的FuncOnBarCallback回调函数推送给调用者。
	 */
	EXPORT_FLAG void		subscribe_bar(const char* stdCode, const char* period);

	/**
	 * @brief 清理数据缓存
	 * 
	 * @details 该函数用于清理数据服务模块内部的数据缓存，释放内存资源。
	 * 在不再需要数据服务或需要重新加载数据时调用。
	 */
	EXPORT_FLAG void		clear_cache();

#ifdef __cplusplus
}
#endif