/*!
 * \file PorterDefs.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief WonderTrader导出接口定义文件
 * \details 定义了WonderTrader框架中用于外部模块交互的各种回调函数类型、事件常量和数据结构
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
 * @brief 上下文处理器类型定义
 * @details 用于标识策略上下文的句柄，在回调函数中用于确定回调的目标上下文
 */
typedef unsigned long		CtxHandler;

/**
 * @brief 引擎事件类型常量
 * @details 定义了引擎的各种事件类型，用于事件回调中识别事件类型
 */
static const WtUInt32	EVENT_ENGINE_INIT	= 1;	//框架初始化
static const WtUInt32	EVENT_SESSION_BEGIN = 2;	//交易日开始
static const WtUInt32	EVENT_SESSION_END	= 3;	//交易日结束
static const WtUInt32	EVENT_ENGINE_SCHDL	= 4;	//框架调度

/**
 * @brief 通道事件类型常量
 * @details 定义了交易通道的各种事件类型，用于通道事件回调中识别事件类型
 */
static const WtUInt32	CHNL_EVENT_READY	= 1000;	//通道就绪事件
static const WtUInt32	CHNL_EVENT_LOST		= 1001;	//通道断开事件

/**
 * @brief 日志级别常量
 * @details 定义了日志的不同级别，用于日志输出控制
 */
static const WtUInt32	LOG_LEVEL_DEBUG		= 0;	//调试级别
static const WtUInt32	LOG_LEVEL_INFO		= 1;	//信息级别
static const WtUInt32	LOG_LEVEL_WARN		= 2;	//警告级别
static const WtUInt32	LOG_LEVEL_ERROR		= 3;	//错误级别


/**
 * @brief 策略回调函数定义
 * @details 定义了策略模块使用的各种回调函数类型，用于接收行情数据、事件通知等
 */
/**
 * @brief K线数据获取回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param stdCode 标准化合约代码
 * @param period K线周期，如m1/m5
 * @param bar K线数据数组
 * @param count 数据数量
 * @param isLast 是否为最后一批数据
 */
typedef void(PORTER_FLAG *FuncGetBarsCallback)(CtxHandler cHandle, const char* stdCode, const char* period, WTSBarStruct* bar, WtUInt32 count, bool isLast);
/**
 * @brief Tick数据获取回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param stdCode 标准化合约代码
 * @param tick Tick数据数组
 * @param count 数据数量
 * @param isLast 是否为最后一批数据
 */
typedef void(PORTER_FLAG *FuncGetTicksCallback)(CtxHandler cHandle, const char* stdCode, WTSTickStruct* tick, WtUInt32 count, bool isLast);
/**
 * @brief 策略初始化回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 */
typedef void(PORTER_FLAG *FuncStraInitCallback)(CtxHandler cHandle);
/**
 * @brief 交易日事件回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param curTDate 当前交易日，格式为YYYYMMDD
 * @param isBegin 是否为交易日开始，true为开始，false为结束
 */
typedef void(PORTER_FLAG *FuncSessionEvtCallback)(CtxHandler cHandle, WtUInt32 curTDate, bool isBegin);
/**
 * @brief 策略Tick数据回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param stdCode 标准化合约代码
 * @param tick Tick数据
 */
typedef void(PORTER_FLAG *FuncStraTickCallback)(CtxHandler cHandle, const char* stdCode, WTSTickStruct* tick);
/**
 * @brief 策略计算回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param curDate 当前日期，格式为YYYYMMDD
 * @param curTime 当前时间，格式为HHMMSS或HHMMSS000
 */
typedef void(PORTER_FLAG *FuncStraCalcCallback)(CtxHandler cHandle, WtUInt32 curDate, WtUInt32 curTime);
/**
 * @brief 策略K线回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param stdCode 标准化合约代码
 * @param period K线周期，如m1/m5
 * @param newBar 新的K线数据
 */
typedef void(PORTER_FLAG *FuncStraBarCallback)(CtxHandler cHandle, const char* stdCode, const char* period, WTSBarStruct* newBar);
/**
 * @brief 持仓获取回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param stdCode 标准化合约代码
 * @param position 持仓量
 * @param isLast 是否为最后一批数据
 */
typedef void(PORTER_FLAG *FuncGetPositionCallback)(CtxHandler cHandle, const char* stdCode, double position, bool isLast);
/**
 * @brief 策略条件触发回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param stdCode 标准化合约代码
 * @param target 目标价格
 * @param price 当前价格
 * @param usertag 用户标签
 */
typedef void(PORTER_FLAG *FuncStraCondTriggerCallback)(CtxHandler cHandle, const char* stdCode, double target, double price, const char* usertag);

/**
 * @brief 策略订单队列回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param stdCode 标准化合约代码
 * @param ordQue 订单队列数据
 */
typedef void(PORTER_FLAG *FuncStraOrdQueCallback)(CtxHandler cHandle, const char* stdCode, WTSOrdQueStruct* ordQue);
/**
 * @brief 订单队列获取回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param stdCode 标准化合约代码
 * @param ordQue 订单队列数据数组
 * @param count 数据数量
 * @param isLast 是否为最后一批数据
 */
typedef void(PORTER_FLAG *FuncGetOrdQueCallback)(CtxHandler cHandle, const char* stdCode, WTSOrdQueStruct* ordQue, WtUInt32 count, bool isLast);
/**
 * @brief 策略订单明细回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param stdCode 标准化合约代码
 * @param ordDtl 订单明细数据
 */
typedef void(PORTER_FLAG *FuncStraOrdDtlCallback)(CtxHandler cHandle, const char* stdCode, WTSOrdDtlStruct* ordDtl);
/**
 * @brief 订单明细获取回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param stdCode 标准化合约代码
 * @param ordDtl 订单明细数据数组
 * @param count 数据数量
 * @param isLast 是否为最后一批数据
 */
typedef void(PORTER_FLAG *FuncGetOrdDtlCallback)(CtxHandler cHandle, const char* stdCode, WTSOrdDtlStruct* ordDtl, WtUInt32 count, bool isLast);
/**
 * @brief 策略成交回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param stdCode 标准化合约代码
 * @param trans 成交数据
 */
typedef void(PORTER_FLAG *FuncStraTransCallback)(CtxHandler cHandle, const char* stdCode, WTSTransStruct* trans);
/**
 * @brief 成交数据获取回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param stdCode 标准化合约代码
 * @param trans 成交数据数组
 * @param count 数据数量
 * @param isLast 是否为最后一批数据
 */
typedef void(PORTER_FLAG *FuncGetTransCallback)(CtxHandler cHandle, const char* stdCode, WTSTransStruct* trans, WtUInt32 count, bool isLast);

//////////////////////////////////////////////////////////////////////////
/**
 * @brief HFT(高频交易)回调函数定义
 * @details 定义了高频交易模块使用的各种回调函数类型
 */
/**
 * @brief HFT交易通道事件回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param trader 交易通道标识
 * @param evtid 事件ID，如CHNL_EVENT_READY/CHNL_EVENT_LOST
 */
typedef void(PORTER_FLAG *FuncHftChannelCallback)(CtxHandler cHandle, const char* trader, WtUInt32 evtid);	//交易通道事件回调
/**
 * @brief HFT订单回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买单
 * @param totalQty 总数量
 * @param leftQty 剩余数量
 * @param price 价格
 * @param isCanceled 是否已撤销
 * @param userTag 用户标签
 */
typedef void(PORTER_FLAG *FuncHftOrdCallback)(CtxHandler cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag);
/**
 * @brief HFT成交回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买单
 * @param vol 成交量
 * @param price 成交价格
 * @param userTag 用户标签
 */
typedef void(PORTER_FLAG *FuncHftTrdCallback)(CtxHandler cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double vol, double price, const char* userTag);
/**
 * @brief HFT委托回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param bSuccess 是否成功
 * @param message 消息内容
 * @param userTag 用户标签
 */
typedef void(PORTER_FLAG *FuncHftEntrustCallback)(CtxHandler cHandle, WtUInt32 localid, const char* stdCode, bool bSuccess, const char* message, const char* userTag);
/**
 * @brief HFT持仓回调函数类型
 * @param cHandle 上下文句柄，标识策略实例
 * @param stdCode 标准化合约代码
 * @param isLong 是否为多仓
 * @param prevol 之前总持仓
 * @param preavail 之前可用持仓
 * @param newvol 新的总持仓
 * @param newavail 新的可用持仓
 */
typedef void(PORTER_FLAG *FuncHftPosCallback)(CtxHandler cHandle, const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail);


/**
 * @brief 事件回调函数类型
 * @param evtId 事件ID，如EVENT_ENGINE_INIT等
 * @param curDate 当前日期，格式为YYYYMMDD
 * @param curTime 当前时间，格式为HHMMSS或HHMMSS000
 */
typedef void(PORTER_FLAG *FuncEventCallback)(WtUInt32 evtId, WtUInt32 curDate, WtUInt32 curTime);

//////////////////////////////////////////////////////////////////////////
/**
 * @brief 扩展Parser回调函数相关定义
 * @details 定义了Parser模块的事件类型和回调函数
 */

/**
 * @brief Parser事件类型常量
 * @details 定义了Parser模块的各种事件类型
 */
static const WtUInt32	EVENT_PARSER_INIT		= 1;	//Parser初始化
static const WtUInt32	EVENT_PARSER_CONNECT	= 2;	//Parser连接
static const WtUInt32	EVENT_PARSER_DISCONNECT = 3;	//Parser断开连接
static const WtUInt32	EVENT_PARSER_RELEASE	= 4;	//Parser释放

/**
 * @brief Parser事件回调函数类型
 * @param evtId 事件ID，如EVENT_PARSER_INIT等
 * @param id Parser标识
 */
typedef void(PORTER_FLAG *FuncParserEvtCallback)(WtUInt32 evtId, const char* id);
/**
 * @brief Parser订阅回调函数类型
 * @param id Parser标识
 * @param fullCode 完整合约代码
 * @param isForSub 是否为订阅操作，true为订阅，false为取消订阅
 */
typedef void(PORTER_FLAG *FuncParserSubCallback)(const char* id, const char* fullCode, bool isForSub);

//////////////////////////////////////////////////////////////////////////
/**
 * @brief 扩展Executer回调函数相关定义
 * @details 定义了执行器模块的回调函数
 */
/**
 * @brief Executer初始化回调函数类型
 * @param id Executer标识
 */
typedef void(PORTER_FLAG *FuncExecInitCallback)(const char* id);
/**
 * @brief Executer命令回调函数类型
 * @param id Executer标识
 * @param StdCode 标准化合约代码
 * @param targetPos 目标仓位
 */
typedef void(PORTER_FLAG *FuncExecCmdCallback)(const char* id, const char* StdCode, double targetPos);

//////////////////////////////////////////////////////////////////////////
/**
 * @brief 外部数据加载模块相关定义
 * @details 定义了用于加载外部数据的回调函数
 */
/**
 * @brief 加载最终K线数据回调函数类型
 * @param stdCode 标准化合约代码
 * @param period K线周期，如m1/m5
 * @return 是否加载成功
 */
typedef bool(PORTER_FLAG *FuncLoadFnlBars)(const char* stdCode, const char* period);
/**
 * @brief 加载原始K线数据回调函数类型
 * @param stdCode 标准化合约代码
 * @param period K线周期，如m1/m5
 * @return 是否加载成功
 */
typedef bool(PORTER_FLAG *FuncLoadRawBars)(const char* stdCode, const char* period);
/**
 * @brief 加载复权因子回调函数类型
 * @param stdCode 标准化合约代码
 * @return 是否加载成功
 */
typedef bool(PORTER_FLAG *FuncLoadAdjFactors)(const char* stdCode);
/**
 * @brief 加载原始Tick数据回调函数类型
 * @param stdCode 标准化合约代码
 * @param uDate 日期，格式为YYYYMMDD
 * @return 是否加载成功
 */
typedef bool(PORTER_FLAG *FuncLoadRawTicks)(const char* stdCode, uint32_t uDate);
