/*!
 * \file ICtaStraCtx.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief CTA策略上下文接口定义
 * 
 * 本文件定义了CTA策略与交易框架交互的标准接口
 * CTA（Commodity Trading Advisor）策略是基于商品期货、期权等品种交易的策略
 * 通过该接口，策略可以获取行情数据、发出交易信号、查询持仓等
 */
#pragma once
#include<string>
#include <stdint.h>
#include <functional>
#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN
class WTSCommodityInfo;
class WTSTickData;
struct WTSBarStruct;
class WTSKlineSlice;
class WTSTickSlice;

// 旧版持仓回调函数定义（已注释）
//typedef void(*FuncEnumPositionCallBack)(const char* stdCode, int32_t qty);
// 持仓回调函数定义，用于遍历策略持仓
typedef std::function<void(const char*, double)> FuncEnumCtaPosCallBack;

/**
 * @brief CTA策略上下文接口类
 * 
 * 定义了CTA策略与交易框架交互的标准接口
 * 策略通过该接口获取行情数据、历史数据、发出交易指令等
 */
class ICtaStraCtx
{
public:
	/**
	 * @brief 构造函数
	 * @param name 策略名称
	 */
	ICtaStraCtx(const char* name) :_name(name){}

	/**
	 * @brief 虚析构函数
	 */
	virtual ~ICtaStraCtx(){}

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

	//回调函数
	/**
	 * @brief 策略初始化回调
	 * 策略启动时调用一次，用于策略的初始化
	 */
	virtual void on_init() = 0;

	/**
	 * @brief 交易日开始事件
	 * @param uTDate 交易日，格式为YYYYMMDD
	 */
	virtual void on_session_begin(uint32_t uTDate) = 0;

	/**
	 * @brief 交易日结束事件
	 * @param uTDate 交易日，格式为YYYYMMDD
	 */
	virtual void on_session_end(uint32_t uTDate) = 0;
	/**
	 * @brief 行情Tick数据推送
	 * @param stdCode 合约代码
	 * @param newTick 最新的tick数据
	 * @param bEmitStrategy 是否触发策略计算，用于控制外部驱动CTA策略
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* newTick, bool bEmitStrategy = true) = 0;

	/**
	 * @brief K线闭合事件回调
	 * @param stdCode 合约代码
	 * @param period 周期，如m1/m5/d1等
	 * @param times 周期倍数
	 * @param newBar 最新K线
	 */
	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) = 0;

	/**
	 * @brief 定时回调
	 * @param curDate 当前日期，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMMSS
	 * @return 是否要继续执行
	 */
	virtual bool on_schedule(uint32_t curDate, uint32_t curTime) = 0;
	/**
	 * @brief 回测结束事件
	 * 
	 * 只在回测模式下才会触发，用于回测结束后的数据处理和统计
	 * 策略可以在此方法中进行回测结果的汇总分析
	 */
	virtual void on_bactest_end() {};

	/**
	 * @brief 重算结束事件
	 * 
	 * 设计目的是要把on_calculate分成两步
	 * 方便一些外挂的逻辑接入进来，可以在on_calculate_done执行信号
	 * 
	 * @param curDate 当前日期，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMMSS
	 */
	virtual void on_calculate_done(uint32_t curDate, uint32_t curTime) { };

	/**
	 * @brief K线周期结束时的回调
	 * 
	 * 当某个周期的K线完成闭合时调用
	 * 
	 * @param stdCode 合约代码
	 * @param period 周期标识，如m1/m5/d1等
	 * @param newBar 最新闭合的K线数据
	 */
	virtual void on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) = 0;

	/**
	 * @brief 策略计算方法
	 * 
	 * 核心策略计算函数，每个计算周期都会调用一次
	 * 
	 * @param curDate 当前日期，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMMSS
	 */
	virtual void on_calculate(uint32_t curDate, uint32_t curTime) = 0;

	/**
	 * @brief Tick数据更新事件
	 * 
	 * 当最新的Tick数据到来时触发的回调
	 * 
	 * @param stdCode 合约代码
	 * @param newTick 最新的Tick数据
	 */
	virtual void on_tick_updated(const char* stdCode, WTSTickData* newTick){}

	/**
	 * @brief 条件触发事件
	 * 
	 * 当某些预设条件触发时的回调，如止损、止盈等
	 * 
	 * @param stdCode 合约代码
	 * @param target 目标价格
	 * @param price 当前价格
	 * @param usertag 用户标记
	 */
	virtual void on_condition_triggered(const char* stdCode, double target, double price, const char* usertag){}

	/**
	 * @brief 遍历持仓回调函数
	 * @param cb 回调函数
	 * @param bForExecute 是否用于执行，为true时会处理锁仓等情况
	 */
	virtual void enum_position(FuncEnumCtaPosCallBack cb, bool bForExecute = false) = 0;

	//策略接口
	/**
	 * @brief 多头进场
	 * 
	 * 开多仓交易接口
	 * 
	 * @param stdCode 合约代码
	 * @param qty 下单数量
	 * @param userTag 下单标记，用于标识本次交易
	 * @param limitprice 限价，0表示市价单
	 * @param stopprice 止损价，0表示不设止损
	 */
	virtual void stra_enter_long(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) = 0;

	/**
	 * @brief 空头进场
	 * 
	 * 开空仓交易接口
	 * 
	 * @param stdCode 合约代码
	 * @param qty 下单数量
	 * @param userTag 下单标记，用于标识本次交易
	 * @param limitprice 限价，0表示市价单
	 * @param stopprice 止损价，0表示不设止损
	 */
	virtual void stra_enter_short(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) = 0;

	/**
	 * @brief 多头出场
	 * 
	 * 平多仓交易接口
	 * 
	 * @param stdCode 合约代码
	 * @param qty 下单数量
	 * @param userTag 下单标记，用于标识本次交易
	 * @param limitprice 限价，0表示市价单
	 * @param stopprice 止损价，0表示不设止损
	 */
	virtual void stra_exit_long(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) = 0;

	/**
	 * @brief 空头出场
	 * 
	 * 平空仓交易接口
	 * 
	 * @param stdCode 合约代码
	 * @param qty 下单数量
	 * @param userTag 下单标记，用于标识本次交易
	 * @param limitprice 限价，0表示市价单
	 * @param stopprice 止损价，0表示不设止损
	 */
	virtual void stra_exit_short(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) = 0;

	/**
	 * @brief 获取当前持仓
	 * 
	 * 查询当前策略的持仓数量
	 * 
	 * @param stdCode 合约代码
	 * @param bOnlyValid 是否只读可用持仓，默认为false，只有当userTag为空时生效，主要针对T+1的品种
	 * @param userTag 下单标记，如果下单标记为空，则读取持仓汇总，如果下单标记不为空，则读取对应的持仓明细
	 * @return 持仓数量，正数表示多头持仓，负数表示空头持仓
	 */
	virtual double stra_get_position(const char* stdCode, bool bOnlyValid = false, const char* userTag = "") = 0;

	/**
	 * @brief 设置持仓数量
	 * 
	 * 设置策略指定合约的目标持仓数量
	 * 
	 * @param stdCode 合约代码
	 * @param qty 目标持仓数量，正数表示多头持仓，负数表示空头持仓
	 * @param userTag 下单标记
	 * @param limitprice 限价，0表示市价单
	 * @param stopprice 止损价，0表示不设止损
	 */
	virtual void stra_set_position(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) = 0;

	/**
	 * @brief 获取当前价格
	 * 
	 * 获取指定合约的最新价格
	 * 
	 * @param stdCode 合约代码
	 * @return 当前最新价格
	 */
	virtual double stra_get_price(const char* stdCode) = 0;

	/**
	 * @brief 读取当日价格
	 * 
	 * 读取指定合约的当日价格数据
	 * 
	 * @param stdCode 合约代码
	 * @param flag 价格标记：0-开盘价，1-最高价，2-最低价，3-收盘价/最新价
	 * @return 指定类型的价格
	 */
	virtual double stra_get_day_price(const char* stdCode, int flag = 0) = 0;

	/**
	 * @brief 获取当前交易日
	 * 
	 * @return 当前交易日，格式为YYYYMMDD
	 */
	virtual uint32_t stra_get_tdate() = 0;

	/**
	 * @brief 获取当前日期
	 * 
	 * @return 当前日期，格式为YYYYMMDD
	 */
	virtual uint32_t stra_get_date() = 0;

	/**
	 * @brief 获取当前时间
	 * 
	 * @return 当前时间，格式为HHMMSS，例如235959
	 */
	virtual uint32_t stra_get_time() = 0;

	/**
	 * @brief 获取资金数据
	 * 
	 * 获取策略账户相关的资金数据
	 * 
	 * @param flag 资金数据标记：0-动态权盈，1-静态权盈，2-已用保证金
	 * @return 指定类型的资金数据
	 */
	virtual double stra_get_fund_data(int flag = 0) = 0;

	/**
	 * @brief 获取首次进场时间
	 * 
	 * 获取指定合约的首次进场时间
	 * 
	 * @param stdCode 合约代码
	 * @return 时间戳，格式为YYYYMMDDHHMMSSsss
	 */
	virtual uint64_t stra_get_first_entertime(const char* stdCode) = 0;

	/**
	 * @brief 获取最近进场时间
	 * 
	 * 获取指定合约的最近进场时间
	 * 
	 * @param stdCode 合约代码
	 * @return 时间戳，格式为YYYYMMDDHHMMSSsss
	 */
	virtual uint64_t stra_get_last_entertime(const char* stdCode) = 0;

	/**
	 * @brief 获取最近出场时间
	 * 
	 * 获取指定合约的最近出场时间
	 * 
	 * @param stdCode 合约代码
	 * @return 时间戳，格式为YYYYMMDDHHMMSSsss
	 */
	virtual uint64_t stra_get_last_exittime(const char* stdCode) = 0;

	/**
	 * @brief 获取最近进场价格
	 * 
	 * 获取指定合约的最近进场价格
	 * 
	 * @param stdCode 合约代码
	 * @return 最近进场价格
	 */
	virtual double stra_get_last_enterprice(const char* stdCode) = 0;

	/**
	 * @brief 获取持仓均价
	 * 
	 * 获取指定合约的当前持仓均价
	 * 
	 * @param stdCode 合约代码
	 * @return 持仓均价
	 */
	virtual double stra_get_position_avgpx(const char* stdCode) = 0;

	/**
	 * @brief 获取持仓盈亏
	 * 
	 * 获取指定合约的当前持仓盈亏
	 * 
	 * @param stdCode 合约代码
	 * @return 持仓盈亏
	 */
	virtual double stra_get_position_profit(const char* stdCode) = 0;

	/**
	 * @brief 获取指定标记的进场时间
	 * 
	 * 获取指定合约和指定标记的进场时间
	 * 
	 * @param stdCode 合约代码
	 * @param userTag 用户标记
	 * @return 时间戳，格式为YYYYMMDDHHMMSSsss
	 */
	virtual uint64_t stra_get_detail_entertime(const char* stdCode, const char* userTag) = 0;

	/**
	 * @brief 获取指定标记的成本
	 * 
	 * 获取指定合约和指定标记的成本价格
	 * 
	 * @param stdCode 合约代码
	 * @param userTag 用户标记
	 * @return 成本价格
	 */
	virtual double stra_get_detail_cost(const char* stdCode, const char* userTag) = 0;

	/**
	 * @brief 读取持仓明细的浮盈
	 * 
	 * 获取指定合约和指定标记的持仓明细的浮动盈亏
	 * 
	 * @param stdCode 合约代码
	 * @param userTag 用户标记
	 * @param flag 浮盈标志：0-浮动盈亏，1-最大浮盈，2-最高浮动价格，-1-最大浮亏，-2-最小浮动价格
	 * @return 指定类型的浮动盈亏数据
	 */
	virtual double stra_get_detail_profit(const char* stdCode, const char* userTag, int flag = 0) = 0;

	/**
	 * @brief 获取品种信息
	 * 
	 * 获取指定品种的详细信息，包括合约代码、名称、手续费等
	 * 
	 * @param stdCode 合约代码
	 * @return 品种信息对象指针
	 */
	virtual WTSCommodityInfo* stra_get_comminfo(const char* stdCode) = 0;

	/**
	 * @brief 获取K线数据
	 * 
	 * 获取指定合约的历史K线数据
	 * 
	 * @param stdCode 合约代码
	 * @param period 周期标识，如m1/m5/d1等
	 * @param count 条数
	 * @param isMain 是否获取主力合约数据
	 * @return K线数据片段对象指针
	 */
	virtual WTSKlineSlice*	stra_get_bars(const char* stdCode, const char* period, uint32_t count, bool isMain = false) = 0;

	/**
	 * @brief 获取Tick数据
	 * 
	 * 获取指定合约的历史Tick数据
	 * 
	 * @param stdCode 合约代码
	 * @param count 条数
	 * @return Tick数据片段对象指针
	 */
	virtual WTSTickSlice*	stra_get_ticks(const char* stdCode, uint32_t count) = 0;

	/**
	 * @brief 获取最新Tick数据
	 * 
	 * 获取指定合约的最新的一条Tick数据
	 * 
	 * @param stdCode 合约代码
	 * @return 最新Tick数据对象指针
	 */
	virtual WTSTickData*	stra_get_last_tick(const char* stdCode) = 0;

	/**
	 * @brief 获取分月合约代码
	 * 
	 * 将标准化合约代码转换为符合交易所格式的原始分月合约代码
	 * 
	 * @param stdCode 标准化合约代码
	 * @return 原始分月合约代码
	 */
	virtual std::string		stra_get_rawcode(const char* stdCode) = 0;

	/**
	 * @brief 订阅通道Tick数据
	 * 
	 * 订阅指定合约的实时Tick数据
	 * 
	 * @param stdCode 合约代码
	 */
	virtual void stra_sub_ticks(const char* stdCode) = 0;

	/**
	 * @brief 订阅K线闭合事件
	 * 
	 * 订阅指定合约的指定周期的K线闭合事件
	 * 
	 * @param stdCode 合约代码
	 * @param period 周期标识，如m1/m5/d1等
	 */
	virtual void stra_sub_bar_events(const char* stdCode, const char* period) = 0;

	/**
	 * @brief 输出信息级别日志
	 * 
	 * 输出信息级别的日志消息
	 * 
	 * @param message 日志消息
	 */
	virtual void stra_log_info(const char* message) = 0;

	/**
	 * @brief 输出调试级别日志
	 * 
	 * 输出调试级别的日志消息
	 * 
	 * @param message 日志消息
	 */
	virtual void stra_log_debug(const char* message) = 0;

	/**
	 * @brief 输出错误级别日志
	 * 
	 * 输出错误级别的日志消息
	 * 
	 * @param message 日志消息
	 */
	virtual void stra_log_error(const char* message) = 0;

	/**
	 * @brief 输出警告级别日志
	 * 
	 * 输出警告级别的日志消息
	 * 
	 * @param message 日志消息
	 */
	virtual void stra_log_warn(const char* message) {}

	/**
	 * @brief 保存用户数据
	 * 
	 * 保存策略的自定义数据，便于在策略重启后恢复状态
	 * 
	 * @param key 数据键名
	 * @param val 数据值
	 */
	virtual void stra_save_user_data(const char* key, const char* val){}

	/**
	 * @brief 加载用户数据
	 * 
	 * 读取之前保存的策略自定义数据
	 * 
	 * @param key 数据键名
	 * @param defVal 默认值，当键名不存在时返回该值
	 * @return 读取到的数据值或默认值
	 */
	virtual const char* stra_load_user_data(const char* key, const char* defVal = "") { return defVal; }

	/**
	 * @brief 设置图表K线
	 * 
	 * 设置交易平台上的图表展示的K线周期和合约
	 * 
	 * @param stdCode 合约代码
	 * @param period 周期标识，如m1/m5/d1等
	 */
	virtual void set_chart_kline(const char* stdCode, const char* period) {}

	/**
	 * @brief 添加信号标记
	 * 
	 * 在交易平台图表上添加标记，比如买入、卖出等信号
	 * 
	 * @param price 价格位置
	 * @param icon 图标，如“⬆️”表示上涨，“⬇️”表示下跌
	 * @param tag 标记文字，用于描述信号
	 */
	virtual void add_chart_mark(double price, const char* icon, const char* tag) {}

	/**
	 * @brief 添加指标
	 * 
	 * 在交易平台图表上添加自定义指标
	 * 
	 * @param idxName 指标名称
	 * @param indexType 指标类型：0-主图指标，1-副图指标
	 */
	virtual void register_index(const char* idxName, uint32_t indexType) {}


	/**
	 * @brief 添加指标线
	 * 
	 * 在已添加的指标上添加一条指标线
	 * 
	 * @param idxName 指标名称
	 * @param lineName 线条名称
	 * @param lineType 线性，0-曲线
	 * @return 是否添加成功
	 */
	virtual bool register_index_line(const char* idxName, const char* lineName, uint32_t lineType) { return false; }

	/**
	 * @brief 添加基准线
	 * 
	 * 在指标上添加一条基准线，基准线是一条水平直线
	 * 
	 * @param idxName 指标名称
	 * @param lineName 线条名称
	 * @param val 数值，基准线的水平位置
	 * @return 是否添加成功
	 */
	virtual bool add_index_baseline(const char* idxName, const char* lineName, double val) { return false; }

	/**
	 * @brief 设置指标值
	 * 
	 * 为指定指标线设置当前值，用于在图表上显示
	 * 
	 * @param idxName 指标名称
	 * @param lineName 线条名称
	 * @param val 指标值
	 * @return 是否设置成功
	 */
	virtual bool set_index_value(const char* idxName, const char* lineName, double val) { return false; }

	/**
	 * @brief 获取最后的进场标记
	 * 
	 * 获取指定合约最近一次进场交易的用户标记
	 * 
	 * @param stdCode 合约代码
	 * @return 用户标记字符串
	 */
	virtual const char* stra_get_last_entertag(const char* stdCode) = 0;

protected:
	std::string _name;    ///< 策略名称
};

NS_WTP_END