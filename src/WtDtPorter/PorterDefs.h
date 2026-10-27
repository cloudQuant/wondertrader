/*!
 * \file PorterDefs.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据处理模块的常量和回调函数定义
 * \details 定义了数据处理模块中使用的各种事件常量和回调函数类型，
 *          包括解析器事件常量、解析器回调函数和数据导出回调函数
 */
#pragma once

#include <stdint.h>
#include "../Includes/WTSTypes.h"

/**
 * @brief WonderTrader数据处理模块的命名空间开始
 */
NS_WTP_BEGIN
/**
 * @brief 适价数据结构声明
 * @details 存储行情适价信息，包括价格、量能、时间等数据
 */
struct WTSTickStruct;

/**
 * @brief K线数据结构声明
 * @details 存储不同周期的K线数据，包含开盘价、最高价、最低价、收盘价和成交量等
 */
struct WTSBarStruct;

/**
 * @brief 委托明细数据结构声明
 * @details 存储委托的详细信息，如委托价格、委托量、委托方向等
 */
struct WTSOrdDtlStruct;

/**
 * @brief 委托队列数据结构声明
 * @details 存储委托队列信息，包含买卖相关队列数据
 */
struct WTSOrdQueStruct;

/**
 * @brief 成交数据结构声明
 * @details 存储成交信息，如成交价格、成交量、成交时间等
 */
struct WTSTransStruct;
/**
 * @brief WonderTrader数据处理模块的命名空间结束
 */
NS_WTP_END

USING_NS_WTP;

//////////////////////////////////////////////////////////////////////////
/**
 * @brief 解析器相关常量和回调函数定义
 * @details 定义了与行情解析器相关的事件类型常量和回调函数类型
 */

/**
 * @brief 解析器初始化事件常量
 * @details 表示解析器已经初始化的事件ID
 */
static const WtUInt32	EVENT_PARSER_INIT = 1;

/**
 * @brief 解析器连接事件常量
 * @details 表示解析器已经连接到数据源的事件ID
 */
static const WtUInt32	EVENT_PARSER_CONNECT = 2;

/**
 * @brief 解析器断开连接事件常量
 * @details 表示解析器已经断开与数据源连接的事件ID
 */
static const WtUInt32	EVENT_PARSER_DISCONNECT = 3;

/**
 * @brief 解析器释放事件常量
 * @details 表示解析器已经释放资源的事件ID
 */
static const WtUInt32	EVENT_PARSER_RELEASE = 4;

/**
 * @brief 解析器事件回调函数类型
 * @details 在解析器发生事件时触发的回调函数类型
 * @param evtId 事件ID，对应上述定义的EVENT_PARSER_*常量
 * @param id 解析器标识符
 */
typedef void(PORTER_FLAG *FuncParserEvtCallback)(WtUInt32 evtId, const char* id);

/**
 * @brief 解析器订阅回调函数类型
 * @details 在执行订阅或取消订阅操作时触发的回调函数类型
 * @param id 解析器标识符
 * @param fullCode 合约完整代码
 * @param isForSub 是否为订阅操作，true表示订阅，false表示取消订阅
 */
typedef void(PORTER_FLAG *FuncParserSubCallback)(const char* id, const char* fullCode, bool isForSub);


//////////////////////////////////////////////////////////////////////////
/**
 * @brief 数据导出器相关回调函数定义
 * @details 定义了与数据导出器相关的各种类型数据的导出回调函数类型
 */

/**
 * @brief K线数据导出回调函数类型
 * @details 用于处理K线数据的导出操作
 * @param id 导出器标识符
 * @param stdCode 标准化合约代码，格式如 SHFE.rb.HOT
 * @param period 周期码，如m1/m5/d1等
 * @param bars K线数据数组
 * @param count 数组长度
 * @return 处理是否成功
 */
typedef bool(PORTER_FLAG *FuncDumpBars)(const char* id, const char* stdCode, const char* period, WTSBarStruct* bars, WtUInt32 count);

/**
 * @brief 适价数据导出回调函数类型
 * @details 用于处理适价数据的导出操作
 * @param id 导出器标识符
 * @param stdCode 标准化合约代码，格式如 SHFE.rb.HOT
 * @param uDate 交易日期，格式为YYYYMMDD
 * @param ticks 适价数据数组
 * @param count 数组长度
 * @return 处理是否成功
 */
typedef bool(PORTER_FLAG *FuncDumpTicks)(const char* id, const char* stdCode, WtUInt32 uDate, WTSTickStruct* ticks, WtUInt32 count);

/**
 * @brief 委托队列数据导出回调函数类型
 * @details 用于处理委托队列数据的导出操作
 * @param id 导出器标识符
 * @param stdCode 标准化合约代码，格式如 SHFE.rb.HOT
 * @param uDate 交易日期，格式为YYYYMMDD
 * @param items 委托队列数据数组
 * @param count 数组长度
 * @return 处理是否成功
 */
typedef bool(PORTER_FLAG *FuncDumpOrdQue)(const char* id, const char* stdCode, WtUInt32 uDate, WTSOrdQueStruct* items, WtUInt32 count);

/**
 * @brief 委托明细数据导出回调函数类型
 * @details 用于处理委托明细数据的导出操作
 * @param id 导出器标识符
 * @param stdCode 标准化合约代码，格式如 SHFE.rb.HOT
 * @param uDate 交易日期，格式为YYYYMMDD
 * @param items 委托明细数据数组
 * @param count 数组长度
 * @return 处理是否成功
 */
typedef bool(PORTER_FLAG *FuncDumpOrdDtl)(const char* id, const char* stdCode, WtUInt32 uDate, WTSOrdDtlStruct* items, WtUInt32 count);

/**
 * @brief 成交数据导出回调函数类型
 * @details 用于处理成交数据的导出操作
 * @param id 导出器标识符
 * @param stdCode 标准化合约代码，格式如 SHFE.rb.HOT
 * @param uDate 交易日期，格式为YYYYMMDD
 * @param items 成交数据数组
 * @param count 数组长度
 * @return 处理是否成功
 */
typedef bool(PORTER_FLAG *FuncDumpTrans)(const char* id, const char* stdCode, WtUInt32 uDate, WTSTransStruct* items, WtUInt32 count);
