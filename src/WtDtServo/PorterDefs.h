/*!
 * \file PorterDefs.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据服务模块对外接口定义
 * 
 * \details 本文件定义了WtDtServo模块对外提供的回调函数类型，
 * 这些回调函数用于数据的获取和实时数据的推送。
 * 包括获取K线数据、Tick数据的回调，以及数据计数和实时数据推送的回调。
 */
#pragma once

#include <stdint.h>
#include "../Includes/WTSTypes.h"

NS_WTP_BEGIN
struct WTSBarStruct;
struct WTSTickStruct;
NS_WTP_END

USING_NS_WTP;

/**
 * @brief K线数据获取回调函数类型
 * @param bar K线数据结构指针，指向一个或多个K线数据
 * @param count 数据数量
 * @param isLast 是否为最后一批数据
 * 
 * @details 该回调函数用于接收查询的K线数据，数据可能分批返回，
 * 当isLast为true时表示所有数据已经返回完毕。
 */
typedef void(PORTER_FLAG *FuncGetBarsCallback)(WTSBarStruct* bar, WtUInt32 count, bool isLast);
/**
 * @brief Tick数据获取回调函数类型
 * @param tick Tick数据结构指针，指向一个或多个Tick数据
 * @param count 数据数量
 * @param isLast 是否为最后一批数据
 * 
 * @details 该回调函数用于接收查询的Tick数据，数据可能分批返回，
 * 当isLast为true时表示所有数据已经返回完毕。
 */
typedef void(PORTER_FLAG *FuncGetTicksCallback)(WTSTickStruct* tick, WtUInt32 count, bool isLast);
/**
 * @brief 数据计数回调函数类型
 * @param dataCnt 数据数量
 * 
 * @details 该回调函数用于返回查询结果的数据总数，
 * 通常在数据查询开始时调用，以便调用者提前知道数据量。
 */
typedef void(PORTER_FLAG *FuncCountDataCallback)(WtUInt32 dataCnt);
/**
 * @brief 实时Tick数据推送回调函数类型
 * @param stdCode 标准化合约代码
 * @param tick Tick数据结构指针
 * 
 * @details 该回调函数用于实时推送Tick数据，
 * 当有新的Tick数据到达时，系统会调用该函数将数据推送给订阅者。
 */
typedef void(PORTER_FLAG *FuncOnTickCallback)(const char* stdCode, WTSTickStruct* tick);
/**
 * @brief 实时K线数据推送回调函数类型
 * @param stdCode 标准化合约代码
 * @param period K线周期，如"m1"/"d1"
 * @param bar K线数据结构指针
 * 
 * @details 该回调函数用于实时推送K线数据，
 * 当有新的K线数据生成或更新时，系统会调用该函数将数据推送给订阅者。
 */
typedef void(PORTER_FLAG *FuncOnBarCallback)(const char* stdCode, const char* period, WTSBarStruct* bar);

