/*!
* \file ISelStraCtx.h
* \project	WonderTrader
*
* \author Wesley
* \date 2020/03/30
*
* \brief 选股策略上下文接口定义
* \details 本文件定义了WonderTrader框架中选股策略上下文的接口，包括回调函数、交易接口、数据访问和用户数据管理等
*/
#pragma once
#include <stdint.h>
#include <functional>
#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN
class WTSCommodityInfo;
class WTSSessionInfo;
class WTSTickData;
struct WTSBarStruct;
class WTSKlineSlice;
class WTSTickSlice;

//typedef void(*FuncEnumPositionCallBack)(const char* stdCode, int32_t qty);
/**
 * @brief 选股策略持仓枚举回调函数类型
 * @details 定义了枚举选股策略持仓时的回调函数类型，用于遍历策略的所有持仓
 * @param stdCode 标准化合约代码
 * @param position 持仓量，正数为多头持仓，负数为空头持仓
 */
typedef std::function<void(const char*, double)> FuncEnumSelPositionCallBack;

/**
 * @brief 选股策略上下文接口
 * @details 定义了选股策略的上下文环境，包括回调接口、数据访问接口和交易操作接口
 * @details 选股策略主要用于定期或定时根据行情数据进行股票池的选择和权重调整
 */
class ISelStraCtx
{
public:
	/**
	 * @brief 构造函数
	 * @param name 策略名称
	 */
	ISelStraCtx(const char* name) :_name(name){}

	/**
	 * @brief 析构函数
	 */
	virtual ~ISelStraCtx(){}

	/**
	 * @brief 获取策略名称
	 * @return 策略名称
	 */
	inline const char* name() const{ return _name.c_str(); }

public:
	/**
	 * @brief 获取策略ID
	 * @return 策略ID
	 */
	virtual uint32_t id() = 0;

	/**
	 * @brief 回调函数组
	 * @details 这些回调函数由引擎调用，用于通知策略各种事件的发生
	 */

	/**
	 * @brief 策略初始化回调
	 * @details 策略启动时调用，用于策略的初始化工作
	 */
	virtual void on_init() = 0;

	/**
	 * @brief 交易日开始回调
	 * @param uTDate 交易日，格式为YYYYMMDD
	 */
	virtual void on_session_begin(uint32_t uTDate) = 0;

	/**
	 * @brief 交易日结束回调
	 * @param uTDate 交易日，格式为YYYYMMDD
	 */
	virtual void on_session_end(uint32_t uTDate) = 0;

	/**
	 * @brief 逐笔数据回调
	 * @param stdCode 标准化合约代码
	 * @param newTick 最新的tick数据
	 * @param bEmitStrategy 是否触发策略计算
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* newTick, bool bEmitStrategy = true) = 0;

	/**
	 * @brief K线数据回调
	 * @param stdCode 标准化合约代码
	 * @param period 周期，如m1/m5/d1等
	 * @param times 周期倍数
	 * @param newBar 最新的K线数据
	 */
	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) = 0;

	/**
	 * @brief 定时回调
	 * @param curDate 当前日期，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMMSS
	 * @param fireTime 触发时间，格式为HHMMSS
	 * @return 是否要继续执行
	 */
	virtual bool on_schedule(uint32_t curDate, uint32_t curTime, uint32_t fireTime) = 0;
	/**
	 * @brief 回测结束事件回调
	 * @details 该回调仅在回测环境下会被触发，用于通知策略回测已结束
	 */
	virtual void on_bactest_end() {};

	/**
	 * @brief K线收盘回调
	 * @param stdCode 标准化合约代码
	 * @param period 周期，如m1/m5/d1等
	 * @param newBar 收盘的K线数据
	 */
	virtual void on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) = 0;

	/**
	 * @brief Tick数据更新回调
	 * @param stdCode 标准化合约代码
	 * @param newTick 最新的tick数据
	 */
	virtual void on_tick_updated(const char* stdCode, WTSTickData* newTick){}

	/**
	 * @brief 策略定时调度回调
	 * @param curDate 当前日期，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMMSS
	 */
	virtual void on_strategy_schedule(uint32_t curDate, uint32_t curTime) {}

	/**
	 * @brief 枚举策略持仓
	 * @param cb 持仓回调函数，用于接收每个持仓的信息
	 */
	virtual void enum_position(FuncEnumSelPositionCallBack cb) = 0;

	/**
	 * @brief 策略接口
	 * @details 用于策略中获取数据、设置仓位和访问交易信息的方法
	 */

	/**
	 * @brief 获取指定合约的持仓量
	 * @param stdCode 标准化合约代码
	 * @param bOnlyValid 是否只考虑有效仓位，默认为false
	 * @param userTag 用户标签，用于筛选特定标签的持仓
	 * @return 持仓量，正数为多头持仓，负数为空头持仓
	 */
	virtual double stra_get_position(const char* stdCode, bool bOnlyValid = false, const char* userTag = "") = 0;

	/**
	 * @brief 设置指定合约的目标持仓量
	 * @param stdCode 标准化合约代码
	 * @param qty 目标持仓量，正数为多头持仓，负数为空头持仓
	 * @param userTag 用户标签，用于标记不同来源的持仓
	 */
	virtual void stra_set_position(const char* stdCode, double qty, const char* userTag = "") = 0;

	/**
	 * @brief 获取合约当前市场价格
	 * @param stdCode 标准化合约代码
	 * @return 当前市场价格
	 */
	virtual double stra_get_price(const char* stdCode) = 0;

	/**
	 * @brief 读取当日特定类型的价格
	 * @param stdCode 标准化合约代码
	 * @param flag 价格类型标记：
	 *             0-开盘价
	 *             1-最高价
	 *             2-最低价
	 *             3-收盘价/最新价
	 * @return 指定类型的价格
	 */
	virtual double stra_get_day_price(const char* stdCode, int flag = 0) = 0;

	/**
	 * @brief 获取当前交易日
	 * @return 当前交易日，格式为YYYYMMDD
	 */
	virtual uint32_t stra_get_tdate() = 0;

	/**
	 * @brief 获取当前日期
	 * @return 当前日期，格式为YYYYMMDD
	 */
	virtual uint32_t stra_get_date() = 0;

	/**
	 * @brief 获取当前时间
	 * @return 当前时间，格式为HHMMSSmmm(毫秒)
	 */
	virtual uint32_t stra_get_time() = 0;

	/**
	 * @brief 获取资金数据
	 * @param flag 资金数据类型标记：
	 *             0-动态权益
	 *             1-静态权益
	 *             2-可用资金
	 * @return 指定类型的资金数据
	 */
	virtual double stra_get_fund_data(int flag = 0) = 0;

	/**
	 * @brief 获取指定合约的首次建仓时间
	 * @param stdCode 标准化合约代码
	 * @return 首次建仓时间，格式为YYYYMMDDHHMM
	 */
	virtual uint64_t stra_get_first_entertime(const char* stdCode) = 0;

	/**
	 * @brief 获取指定合约的最近建仓时间
	 * @param stdCode 标准化合约代码
	 * @return 最近建仓时间，格式为YYYYMMDDHHMM
	 */
	virtual uint64_t stra_get_last_entertime(const char* stdCode) = 0;

	/**
	 * @brief 获取指定合约的最近平仓时间
	 * @param stdCode 标准化合约代码
	 * @return 最近平仓时间，格式为YYYYMMDDHHMM
	 */
	virtual uint64_t stra_get_last_exittime(const char* stdCode) = 0;

	/**
	 * @brief 获取指定合约的最近建仓价格
	 * @param stdCode 标准化合约代码
	 * @return 最近建仓的价格
	 */
	virtual double stra_get_last_enterprice(const char* stdCode) = 0;

	/**
	 * @brief 获取指定合约的最近建仓标签
	 * @param stdCode 标准化合约代码
	 * @return 最近建仓的用户标签
	 */
	virtual const char* stra_get_last_entertag(const char* stdCode)  = 0;

	/**
	 * @brief 获取指定合约持仓的平均价格
	 * @param stdCode 标准化合约代码
	 * @return 持仓的平均价格
	 */
	virtual double stra_get_position_avgpx(const char* stdCode) = 0;

	/**
	 * @brief 获取指定合约持仓的浮动盈亏
	 * @param stdCode 标准化合约代码
	 * @return 持仓的浮动盈亏
	 */
	virtual double stra_get_position_profit(const char* stdCode) = 0;

	/**
	 * @brief 获取指定标签持仓的进场时间
	 * @param stdCode 标准化合约代码
	 * @param userTag 用户标签
	 * @return 指定标签持仓的进场时间，格式为YYYYMMDDHHMM
	 */
	virtual uint64_t stra_get_detail_entertime(const char* stdCode, const char* userTag) = 0;

	/**
	 * @brief 获取指定标签持仓的成本
	 * @param stdCode 标准化合约代码
	 * @param userTag 用户标签
	 * @return 指定标签持仓的成本
	 */
	virtual double stra_get_detail_cost(const char* stdCode, const char* userTag) = 0;

	/**
	 * @brief 读取指定标签持仓的浮动盈亏信息
	 * @param stdCode 标准化合约代码
	 * @param userTag 用户标签
	 * @param flag 浮盈标志：
	 *             0-当前浮动盈亏
	 *             1-最大浮盈
	 *             2-最高浮动价格
	 *             -1-最大浮亏
	 *             -2-最小浮动价格
	 * @return 指定类型的浮动盈亏信息
	 */
	virtual double stra_get_detail_profit(const char* stdCode, const char* userTag, int flag = 0) = 0;

	/**
	 * @brief 获取合约品种信息
	 * @param stdCode 标准化合约代码
	 * @return 合约品种信息对象指针
	 */
	virtual WTSCommodityInfo* stra_get_comminfo(const char* stdCode) = 0;

	/**
	 * @brief 获取交易时段信息
	 * @param stdCode 标准化合约代码
	 * @return 交易时段信息对象指针
	 */
	virtual WTSSessionInfo* stra_get_sessinfo(const char* stdCode) = 0;

	/**
	 * @brief 获取K线切片数据
	 * @param stdCode 标准化合约代码
	 * @param period K线周期，如m1/m5/d1等
	 * @param count 数据条数
	 * @return K线切片数据对象指针
	 */
	virtual WTSKlineSlice*	stra_get_bars(const char* stdCode, const char* period, uint32_t count) = 0;

	/**
	 * @brief 获取Tick切片数据
	 * @param stdCode 标准化合约代码
	 * @param count 数据条数
	 * @return Tick切片数据对象指针
	 */
	virtual WTSTickSlice*	stra_get_ticks(const char* stdCode, uint32_t count) = 0;

	/**
	 * @brief 获取最新的Tick数据
	 * @param stdCode 标准化合约代码
	 * @return 最新的Tick数据对象指针
	 */
	virtual WTSTickData*	stra_get_last_tick(const char* stdCode) = 0;

	/**
	 * @brief 获取原始合约代码（分月合约代码）
	 * @param stdCode 标准化合约代码
	 * @return 原始合约代码字符串
	 * @details 当使用主力合约时，该函数可以获取到对应的分月合约代码
	 */
	virtual std::string		stra_get_rawcode(const char* stdCode) = 0;

	/**
	 * @brief 订阅合约的Tick数据
	 * @param stdCode 标准化合约代码
	 * @details 订阅指定合约的Tick数据，以便能收到该合约的实时行情推送
	 */
	virtual void stra_sub_ticks(const char* stdCode) = 0;

	/**
	 * @brief 输出信息级别的日志
	 * @param message 日志消息
	 */
	virtual void stra_log_info(const char* message) = 0;

	/**
	 * @brief 输出调试级别的日志
	 * @param message 日志消息
	 */
	virtual void stra_log_debug(const char* message) = 0;

	/**
	 * @brief 输出错误级别的日志
	 * @param message 日志消息
	 */
	virtual void stra_log_error(const char* message) = 0;

	/**
	 * @brief 输出警告级别的日志
	 * @param message 日志消息
	 */
	virtual void stra_log_warn(const char* message) {}

	/**
	 * @brief 保存用户自定义数据
	 * @param key 数据键
	 * @param val 数据值
	 * @details 用于在策略中保存用户自定义的持久化数据，例如策略参数、状态等
	 */
	virtual void stra_save_user_data(const char* key, const char* val){}

	/**
	 * @brief 加载用户自定义数据
	 * @param key 数据键
	 * @param defVal 默认值，当数据不存在时返回该值
	 * @return 数据值或默认值
	 * @details 用于在策略中读取之前保存的用户自定义数据
	 */
	virtual const char* stra_load_user_data(const char* key, const char* defVal = "") { return defVal; }

protected:
	/**
	 * @brief 策略名称
	 */
	std::string _name;
};

NS_WTP_END