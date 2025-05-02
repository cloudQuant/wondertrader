/*!
 * \file PorterDefs.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 回测框架接口定义文件
 * \details 本文件定义了WonderTrader回测系统中使用的各类回调函数类型、常量和事件类型。
 * 包括数据获取回调、策略事件回调、高频交易回调等。这些定义用于实现C++和其他语言之间的接口通信。
 */
#pragma once

#include <stdint.h>
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
 * @brief 策略上下文句柄类型
 * @details 用于标识和操作策略上下文的句柄类型，在多种回调函数中作为参数传递
 */
typedef unsigned long		CtxHandler;

/**
 * @brief 事件类型定义
 * @details 定义了回测框架中使用的各种事件类型的常量值
 */
static const WtUInt32	EVENT_ENGINE_INIT		= 1;	///<框架初始化事件
static const WtUInt32	EVENT_SESSION_BEGIN		= 2;	///<交易日开始事件
static const WtUInt32	EVENT_SESSION_END		= 3;	///<交易日结束事件
static const WtUInt32	EVENT_ENGINE_SCHDL		= 4;	///<框架调度事件
static const WtUInt32	EVENT_BACKTEST_END		= 5;	///<回测结束事件

/**
 * @brief 日志级别定义
 * @details 定义了回测框架中使用的各种日志级别的常量值，用于控制日志输出
 */
static const WtUInt32	LOG_LEVEL_DEBUG			= 0;	///<调试级别，最详细的日志输出
static const WtUInt32	LOG_LEVEL_INFO			= 1;	///<信息级别，一般信息日志
static const WtUInt32	LOG_LEVEL_WARN			= 2;	///<警告级别，可能存在问题的日志
static const WtUInt32	LOG_LEVEL_ERROR			= 3;	///<错误级别，发生错误的日志

/**
 * @brief 回调函数类型定义
 * @details 定义了各种策略开发和数据获取相关的回调函数类型
 */

/**
 * @brief K线数据获取回调函数
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准化合约代码
 * @param period K线周期，如“m1”、“d1”等
 * @param bar K线数据指针
 * @param count 返回的K线数量
 * @param isLast 是否为最后一批数据
 */
typedef void(PORTER_FLAG *FuncGetBarsCallback)(CtxHandler cHandle, const char* stdCode, const char* period, WTSBarStruct* bar, WtUInt32 count, bool isLast);

/**
 * @brief Tick数据获取回调函数
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准化合约代码
 * @param tick Tick数据指针
 * @param count 返回的Tick数量
 * @param isLast 是否为最后一批数据
 */
typedef void(PORTER_FLAG *FuncGetTicksCallback)(CtxHandler cHandle, const char* stdCode, WTSTickStruct* tick, WtUInt32 count, bool isLast);

/**
 * @brief 策略初始化回调函数
 * @param cHandle 策略上下文句柄
 */
typedef void(PORTER_FLAG *FuncStraInitCallback)(CtxHandler cHandle);

/**
 * @brief 交易日事件回调函数
 * @param cHandle 策略上下文句柄
 * @param curTDate 当前交易日，格式为YYYYMMDD
 * @param isBegin 是否为交易日开始，true表示开始，false表示结束
 */
typedef void(PORTER_FLAG *FuncSessionEvtCallback)(CtxHandler cHandle, WtUInt32 curTDate, bool isBegin);
/**
 * @brief 策略Tick数据回调函数
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准化合约代码
 * @param tick Tick数据指针
 * @details 当收到新的Tick数据时触发该回调
 */
typedef void(PORTER_FLAG *FuncStraTickCallback)(CtxHandler cHandle, const char* stdCode, WTSTickStruct* tick);

/**
 * @brief 策略定时计算回调函数
 * @param cHandle 策略上下文句柄
 * @param uDate 日期，格式为YYYYMMDD
 * @param uTime 时间，格式为HHMMSS或HHMMSS000
 * @details 在策略需要进行定时计算时触发该回调
 */
typedef void(PORTER_FLAG *FuncStraCalcCallback)(CtxHandler cHandle, WtUInt32 uDate, WtUInt32 uTime);

/**
 * @brief 策略K线数据回调函数
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准化合约代码
 * @param period K线周期，如“m1”、“d1”等
 * @param newBar 新K线数据指针
 * @details 当新的K线数据生成时触发该回调
 */
typedef void(PORTER_FLAG *FuncStraBarCallback)(CtxHandler cHandle, const char* stdCode, const char* period, WTSBarStruct* newBar);

/**
 * @brief 持仓信息获取回调函数
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准化合约代码
 * @param position 持仓量，正数表示多头仓位，负数表示空头仓位
 * @param isLast 是否为最后一个持仓信息
 * @details 获取指定合约的持仓信息时触发该回调
 */
typedef void(PORTER_FLAG *FuncGetPositionCallback)(CtxHandler cHandle, const char* stdCode, double position, bool isLast);

/**
 * @brief 策略条件触发回调函数
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准化合约代码
 * @param target 目标值
 * @param price 触发价格
 * @param usertag 用户自定义标签
 * @details 当满足策略设定的条件时触发该回调
 */
typedef void(PORTER_FLAG *FuncStraCondTriggerCallback)(CtxHandler cHandle, const char* stdCode, double target, double price, const char* usertag);

/**
 * @brief 策略委托队列回调函数
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准化合约代码
 * @param ordQue 委托队列数据指针
 * @details 当收到新的委托队列数据时触发该回调
 */
typedef void(PORTER_FLAG *FuncStraOrdQueCallback)(CtxHandler cHandle, const char* stdCode, WTSOrdQueStruct* ordQue);

/**
 * @brief 委托队列数据获取回调函数
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准化合约代码
 * @param ordQue 委托队列数据指针
 * @param count 返回的委托队列数量
 * @param isLast 是否为最后一批数据
 * @details 当查询委托队列数据时触发该回调
 */
typedef void(PORTER_FLAG *FuncGetOrdQueCallback)(CtxHandler cHandle, const char* stdCode, WTSOrdQueStruct* ordQue, WtUInt32 count, bool isLast);

/**
 * @brief 策略委托明细回调函数
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准化合约代码
 * @param ordDtl 委托明细数据指针
 * @details 当收到新的委托明细数据时触发该回调
 */
typedef void(PORTER_FLAG *FuncStraOrdDtlCallback)(CtxHandler cHandle, const char* stdCode, WTSOrdDtlStruct* ordDtl);

/**
 * @brief 委托明细数据获取回调函数
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准化合约代码
 * @param ordDtl 委托明细数据指针
 * @param count 返回的委托明细数量
 * @param isLast 是否为最后一批数据
 * @details 当查询委托明细数据时触发该回调
 */
typedef void(PORTER_FLAG *FuncGetOrdDtlCallback)(CtxHandler cHandle, const char* stdCode, WTSOrdDtlStruct* ordDtl, WtUInt32 count, bool isLast);

/**
 * @brief 策略逆回成交回调函数
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准化合约代码
 * @param trans 逆回成交数据指针
 * @details 当收到新的逆回成交数据时触发该回调
 */
typedef void(PORTER_FLAG *FuncStraTransCallback)(CtxHandler cHandle, const char* stdCode, WTSTransStruct* trans);

/**
 * @brief 逆回成交数据获取回调函数
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准化合约代码
 * @param trans 逆回成交数据指针
 * @param count 返回的逆回成交数量
 * @param isLast 是否为最后一批数据
 * @details 当查询逆回成交数据时触发该回调
 */
typedef void(PORTER_FLAG *FuncGetTransCallback)(CtxHandler cHandle, const char* stdCode, WTSTransStruct* trans, WtUInt32 count, bool isLast);

//////////////////////////////////////////////////////////////////////////
/**
 * @brief 高频交易(HFT)回调函数定义
 * @details 定义了高频交易相关的回调函数，用于处理交易通道事件、委托响应、成交信息等
 */

/**
 * @brief 高频交易通道事件回调函数
 * @param cHandle 策略上下文句柄
 * @param trader 交易器ID
 * @param evtid 事件ID
 * @details 当交易通道状态发生变化时触发该回调，如连接、断开等
 */
typedef void(PORTER_FLAG *FuncHftChannelCallback)(CtxHandler cHandle, const char* trader, WtUInt32 evtid);

/**
 * @brief 高频交易委托回调函数
 * @param cHandle 策略上下文句柄
 * @param localid 委托本地ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入委托
 * @param totalQty 委托总数量
 * @param leftQty 剩余数量
 * @param price 委托价格
 * @param isCanceled 是否已撤销
 * @param userTag 用户自定义标签
 * @details 当委托状态发生变化时触发该回调，如送单、撤单、成交等
 */
typedef void(PORTER_FLAG *FuncHftOrdCallback)(CtxHandler cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag);

/**
 * @brief 高频交易成交回调函数
 * @param cHandle 策略上下文句柄
 * @param localid 委托本地ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入成交
 * @param vol 成交数量
 * @param price 成交价格
 * @param userTag 用户自定义标签
 * @details 当委托发生成交时触发该回调
 */
typedef void(PORTER_FLAG *FuncHftTrdCallback)(CtxHandler cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double vol, double price, const char* userTag);

/**
 * @brief 高频交易委托动作回调函数
 * @param cHandle 策略上下文句柄
 * @param localid 委托本地ID
 * @param stdCode 标准化合约代码
 * @param bSuccess 是否成功
 * @param message 错误消息
 * @param userTag 用户自定义标签
 * @details 当委托动作（下单、撤单）发生时触发该回调
 */
typedef void(PORTER_FLAG *FuncHftEntrustCallback)(CtxHandler cHandle, WtUInt32 localid, const char* stdCode, bool bSuccess, const char* message, const char* userTag);

/**
 * @brief 事件回调函数
 * @param evtId 事件ID
 * @param curDate 当前日期，格式为YYYYMMDD
 * @param curTime 当前时间，格式为HHMMSS或HHMMSS000
 * @details 当系统事件发生时触发该回调，如引擎初始化、交易日开始结束等
 */
typedef void(PORTER_FLAG *FuncEventCallback)(WtUInt32 evtId, WtUInt32 curDate, WtUInt32 curTime);

//////////////////////////////////////////////////////////////////////////
/**
 * @brief 外部数据加载模块回调函数定义
 * @details 定义了用于从外部数据源加载交易数据的回调函数类型，包括完成的K线、原始K线、复权因子和Tick数据
 */

/**
 * @brief 加载完成K线数据的回调函数
 * @param stdCode 标准化合约代码
 * @param period K线周期，如“m1”、“d1”等
 * @return 是否加载成功
 * @details 用于从外部数据源加载已经计算完成的K线数据
 */
typedef bool(PORTER_FLAG *FuncLoadFnlBars)(const char* stdCode, const char* period);

/**
 * @brief 加载原始K线数据的回调函数
 * @param stdCode 标准化合约代码
 * @param period K线周期，如“m1”、“d1”等
 * @return 是否加载成功
 * @details 用于从外部数据源加载原始的未经过复权处理的K线数据
 */
typedef bool(PORTER_FLAG *FuncLoadRawBars)(const char* stdCode, const char* period);

/**
 * @brief 加载复权因子的回调函数
 * @param stdCode 标准化合约代码
 * @return 是否加载成功
 * @details 用于从外部数据源加载合约的复权因子，用于处理除权除息等事件
 */
typedef bool(PORTER_FLAG *FuncLoadAdjFactors)(const char* stdCode);

/**
 * @brief 加载原始Tick数据的回调函数
 * @param stdCode 标准化合约代码
 * @param uDate 交易日期，格式为YYYYMMDD
 * @return 是否加载成功
 * @details 用于从外部数据源加载指定日期的原始Tick数据
 */
typedef bool(PORTER_FLAG *FuncLoadRawTicks)(const char* stdCode, uint32_t uDate);
