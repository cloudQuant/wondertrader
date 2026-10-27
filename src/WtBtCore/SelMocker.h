/*!
* \file SelMocker.h
* \project	WonderTrader
*
* \author Wesley
* \date 2020/03/30
*
* \brief 选股策略回测模拟器头文件
* \details 该文件定义了选股策略回测模拟器的接口和实现，用于在回测环境中模拟选股策略的运行
*          实现了ISelStraCtx和IDataSink接口，提供了策略上下文和数据接收的功能
*/
#pragma once
#include <sstream>
#include "HisDataReplayer.h"

#include "../Includes/FasterDefs.h"
#include "../Includes/ISelStraCtx.h"
#include "../Includes/SelStrategyDefs.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Share/fmtlib.h"
#include "../Share/DLLHelper.hpp"

class SelStrategy;

USING_NS_WTP;

class HisDataReplayer;

/**
 * @brief 选股策略回测模拟器类
 * @details 该类用于模拟选股策略在回测环境中的运行，实现了ISelStraCtx和IDataSink接口
 *          提供了策略上下文环境和数据接收处理的功能
 */
class SelMocker : public ISelStraCtx, public IDataSink
{
public:
	/**
	 * @brief 选股策略模拟器构造函数
	 * @details 初始化选股策略模拟器实例，设置回测环境参数
	 * @param replayer 历史数据回放器指针，用于提供历史数据
	 * @param name 策略名称
	 * @param slippage 滑点设置，默认为0
	 * @param isRatioSlp 是否使用比例滑点，默认为false
	 */
	SelMocker(HisDataReplayer* replayer, const char* name, int32_t slippage = 0, bool isRatioSlp = false);
	/**
	 * @brief 选股策略模拟器析构函数
	 * @details 清理选股策略模拟器实例的资源
	 */
	virtual ~SelMocker();

private:
	/**
	 * @brief 输出调试日志
	 * @details 使用格式化字符串输出调试级别的日志信息
	 * @tparam Args 可变参数类型
	 * @param format 格式化字符串模板
	 * @param args 格式化参数
	 */
	template<typename... Args>
	void log_debug(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_debug(buffer);
	}

	/**
	 * @brief 输出信息日志
	 * @details 使用格式化字符串输出信息级别的日志信息
	 * @tparam Args 可变参数类型
	 * @param format 格式化字符串模板
	 * @param args 格式化参数
	 */
	template<typename... Args>
	void log_info(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_info(buffer);
	}

	/**
	 * @brief 输出错误日志
	 * @details 使用格式化字符串输出错误级别的日志信息
	 * @tparam Args 可变参数类型
	 * @param format 格式化字符串模板
	 * @param args 格式化参数
	 */
	template<typename... Args>
	void log_error(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_error(buffer);
	}

private:
	/**
	 * @brief 输出回测结果
	 * @details 将回测过程中生成的日志信息输出到文件
	 */
	void	dump_outputs();
	/**
	 * @brief 保存策略数据
	 * @details 将策略运行过程中的持仓、资金、信号等数据保存为JSON格式
	 */
	void	dump_stradata();
	/**
	 * @brief 记录信号日志
	 * @details 记录策略产生的交易信号信息
	 * @param stdCode 标准合约代码
	 * @param target 目标仓位
	 * @param price 信号价格
	 * @param gentime 信号生成时间
	 * @param usertag 用户自定义标签
	 */
	inline void log_signal(const char* stdCode, double target, double price, uint64_t gentime, const char* usertag = "");
	/**
	 * @brief 记录交易日志
	 * @details 记录策略执行的交易明细
	 * @param stdCode 标准合约代码
	 * @param isLong 是否为多头交易
	 * @param isOpen 是否为开仓交易
	 * @param curTime 交易时间
	 * @param price 交易价格
	 * @param qty 交易数量
	 * @param userTag 用户自定义标签
	 * @param fee 交易手续费
	 */
	inline void	log_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, const char* userTag = "", double fee = 0.0);
	/**
	 * @brief 记录平仓日志
	 * @details 记录平仓的完整信息，包括开仓和平仓的相关数据
	 * @param stdCode 标准合约代码
	 * @param isLong 是否为多头交易
	 * @param openTime 开仓时间
	 * @param openpx 开仓价格
	 * @param closeTime 平仓时间
	 * @param closepx 平仓价格
	 * @param qty 交易数量
	 * @param profit 平仓盈亏
	 * @param maxprofit 最大浮盈
	 * @param maxloss 最大浮亏
	 * @param totalprofit 总盈亏
	 * @param enterTag 开仓标签
	 * @param exitTag 平仓标签
	 * @param openBarNo 开仓K线序号
	 * @param closeBarNo 平仓K线序号
	 */
	inline void	log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty,
		double profit, double maxprofit, double maxloss, double totalprofit = 0, const char* enterTag = "", const char* exitTag = "", uint32_t openBarNo = 0, uint32_t closeBarNo = 0);

	/**
	 * @brief 更新动态盈亏
	 * @details 根据当前价格计算持仓的动态盈亏
	 * @param stdCode 标准合约代码
	 * @param price 当前价格
	 */
	void	update_dyn_profit(const char* stdCode, double price);

	/**
	 * @brief 设置仓位
	 * @details 执行仓位调整操作，包括开仓和平仓
	 * @param stdCode 标准合约代码
	 * @param qty 目标仓位数量
	 * @param price 交易价格，默认为0表示使用当前市场价格
	 * @param userTag 用户自定义标签
	 * @param bTriggered 是否由条件触发
	 */
	void	do_set_position(const char* stdCode, double qty, double price = 0.0, const char* userTag = "", bool bTriggered = false);
	/**
	 * @brief 添加信号
	 * @details 添加交易信号到信号列表中
	 * @param stdCode 标准合约代码
	 * @param qty 目标仓位数量
	 * @param userTag 用户自定义标签
	 * @param price 信号价格，默认为0表示使用当前市场价格
	 */
	void	append_signal(const char* stdCode, double qty, const char* userTag = "", double price = 0.0);

	/**
	 * @brief 处理Tick数据
	 * @details 处理新到的Tick数据，更新动态盈亏并检查是否触发信号
	 * @param stdCode 标准合约代码
	 * @param last_px 上一次价格
	 * @param cur_px 当前价格
	 */
	void	proc_tick(const char* stdCode, double last_px, double cur_px);

public:
	/**
	 * @brief 初始化选股策略工厂
	 * @details 根据配置初始化选股策略工厂，加载策略模块并创建策略实例
	 * @param cfg 策略配置参数，包含策略模块路径、策略名称等信息
	 * @return 初始化成功返回true，失败返回false
	 */
	bool	init_sel_factory(WTSVariant* cfg);

public:
	//////////////////////////////////////////////////////////////////////////
	//IDataSink
	/**
	 * @brief 处理Tick数据
	 * @details 实现IDataSink接口的handle_tick方法，处理新到的Tick数据
	 * @param stdCode 标准合约代码
	 * @param curTick 当前Tick数据指针
	 * @param pxType 价格类型
	 */
	virtual void	handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType) override;
	/**
	 * @brief 处理K线闭合事件
	 * @details 实现IDataSink接口的handle_bar_close方法，处理K线闭合事件
	 * @param stdCode 标准合约代码
	 * @param period K线周期，如"m1"、"d1"等
	 * @param times 周期倍数
	 * @param newBar 新的K线数据指针
	 */
	virtual void	handle_bar_close(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;
	/**
	 * @brief 处理定时调度
	 * @details 实现IDataSink接口的handle_schedule方法，处理定时调度事件
	 * @param uDate 当前日期，格式为YYYYMMDD
	 * @param uTime 当前时间，格式为HHMMSS或HHMMSS000
	 */
	virtual void	handle_schedule(uint32_t uDate, uint32_t uTime) override;

	/**
	 * @brief 处理初始化事件
	 * @details 实现IDataSink接口的handle_init方法，在回测开始前调用
	 */
	virtual void	handle_init() override;
	/**
	 * @brief 处理交易日开始事件
	 * @details 实现IDataSink接口的handle_session_begin方法，在每个交易日开始时调用
	 * @param uCurDate 当前交易日，格式为YYYYMMDD
	 */
	virtual void	handle_session_begin(uint32_t uCurDate) override;
	/**
	 * @brief 处理交易日结束事件
	 * @details 实现IDataSink接口的handle_session_end方法，在每个交易日结束时调用
	 * @param uCurDate 当前交易日，格式为YYYYMMDD
	 */
	virtual void	handle_session_end(uint32_t uCurDate) override;
	/**
	 * @brief 处理回放完成事件
	 * @details 实现IDataSink接口的handle_replay_done方法，在所有历史数据回放完成后调用
	 */
	virtual void	handle_replay_done() override;

	//////////////////////////////////////////////////////////////////////////
	//ICtaStraCtx
	/**
	 * @brief 获取策略上下文ID
	 * @return 策略上下文ID
	 */
	virtual uint32_t id() { return _context_id; }

	//回调函数
	/**
	 * @brief 策略初始化回调
	 * @details 在策略初始化时调用，用于执行策略的初始化操作
	 */
	virtual void on_init() override;
	/**
	 * @brief 交易日开始回调
	 * @details 在每个交易日开始时调用，用于执行交易日开始时的操作
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 */
	virtual void on_session_begin(uint32_t curTDate) override;
	/**
	 * @brief 交易日结束回调
	 * @details 在每个交易日结束时调用，用于执行交易日结束时的操作
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 */
	virtual void on_session_end(uint32_t curTDate) override;
	/**
	 * @brief Tick数据回调
	 * @details 在收到新的Tick数据时调用，用于处理实时行情数据
	 * @param stdCode 标准合约代码
	 * @param newTick 新的Tick数据指针
	 * @param bEmitStrategy 是否触发策略回调，默认为true
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* newTick, bool bEmitStrategy = true) override;
	/**
	 * @brief K线数据回调
	 * @details 在收到新的K线数据时调用，用于处理K线数据
	 * @param stdCode 标准合约代码
	 * @param period K线周期，如"m1"、"d1"等
	 * @param times 周期倍数
	 * @param newBar 新的K线数据指针
	 */
	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;
	/**
	 * @brief 定时调度回调
	 * @details 在定时调度时间点触发，用于执行定时任务
	 * @param curDate 当前日期，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMMSS或HHMMSS000
	 * @param fireTime 触发时间，格式为HHMMSS
	 * @return 调度是否成功
	 */
	virtual bool on_schedule(uint32_t curDate, uint32_t curTime, uint32_t fireTime) override;
	/**
	 * @brief 枚举持仓回调
	 * @details 枚举当前策略的所有持仓信息
	 * @param cb 持仓枚举回调函数
	 */
	virtual void enum_position(FuncEnumSelPositionCallBack cb) override;

	/**
	 * @brief Tick数据更新回调
	 * @details 在Tick数据更新时调用，用于处理更新的Tick数据
	 * @param stdCode 标准合约代码
	 * @param newTick 新的Tick数据指针
	 */
	virtual void on_tick_updated(const char* stdCode, WTSTickData* newTick) override;
	/**
	 * @brief K线闭合回调
	 * @details 在K线闭合时调用，用于处理已闭合的K线数据
	 * @param stdCode 标准合约代码
	 * @param period K线周期，如"m1"、"d1"等
	 * @param newBar 新的K线数据指针
	 */
	virtual void on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) override;
	/**
	 * @brief 策略定时调度回调
	 * @details 在策略定时调度时间点触发，用于执行策略的定时任务
	 * @param curDate 当前日期，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMMSS或HHMMSS000
	 */
	virtual void on_strategy_schedule(uint32_t curDate, uint32_t curTime) override;


	//////////////////////////////////////////////////////////////////////////
	//策略接口
	/**
	 * @brief 获取持仓量
	 * @details 获取指定合约的持仓量
	 * @param stdCode 标准合约代码
	 * @param bOnlyValid 是否只返回有效仓位（排除冻结仓位）
	 * @param userTag 用户自定义标签，用于筛选指定标签的持仓
	 * @return 持仓量，正数表示多头，负数表示空头
	 */
	virtual double stra_get_position(const char* stdCode, bool bOnlyValid = false, const char* userTag = "") override;
	/**
	 * @brief 设置持仓量
	 * @details 设置指定合约的目标持仓量，系统会自动完成交易
	 * @param stdCode 标准合约代码
	 * @param qty 目标持仓量，正数表示多头，负数表示空头
	 * @param userTag 用户自定义标签，用于标记交易
	 */
	virtual void stra_set_position(const char* stdCode, double qty, const char* userTag = "") override;
	/**
	 * @brief 获取当前价格
	 * @details 获取指定合约的当前市场价格
	 * @param stdCode 标准合约代码
	 * @return 当前价格
	 */
	virtual double stra_get_price(const char* stdCode) override;

	/**
	 * @brief 获取当日价格
	 * @details 获取指定合约的当日价格数据
	 * @param stdCode 标准合约代码
	 * @param flag 价格标记，0表示收盘价，1表示开盘价，2表示最高价，3表示最低价
	 * @return 当日价格
	 */
	virtual double stra_get_day_price(const char* stdCode, int flag = 0) override;

	/**
	 * @brief 获取交易日期
	 * @details 获取当前的交易日期
	 * @return 交易日期，格式为YYYYMMDD
	 */
	virtual uint32_t stra_get_tdate() override;
	/**
	 * @brief 获取当前日期
	 * @details 获取当前的自然日期
	 * @return 当前日期，格式为YYYYMMDD
	 */
	virtual uint32_t stra_get_date() override;
	/**
	 * @brief 获取当前时间
	 * @details 获取当前的时间
	 * @return 当前时间，格式为HHMMSS或HHMMSS000
	 */
	virtual uint32_t stra_get_time() override;

	/**
	 * @brief 获取资金数据
	 * @details 获取策略的资金相关数据
	 * @param flag 资金标记，0表示动态权益，1表示静态权益，2表示已用保证金
	 * @return 资金数据
	 */
	virtual double stra_get_fund_data(int flag = 0) override;

	/**
	 * @brief 获取首次入场时间
	 * @details 获取指定合约的首次入场时间
	 * @param stdCode 标准合约代码
	 * @return 首次入场时间，格式为YYYYMMDDHHMMSSsss
	 */
	virtual uint64_t stra_get_first_entertime(const char* stdCode) override;
	/**
	 * @brief 获取最近入场时间
	 * @details 获取指定合约的最近一次入场时间
	 * @param stdCode 标准合约代码
	 * @return 最近入场时间，格式为YYYYMMDDHHMMSSsss
	 */
	virtual uint64_t stra_get_last_entertime(const char* stdCode) override;
	/**
	 * @brief 获取最近出场时间
	 * @details 获取指定合约的最近一次出场时间
	 * @param stdCode 标准合约代码
	 * @return 最近出场时间，格式为YYYYMMDDHHMMSSsss
	 */
	virtual uint64_t stra_get_last_exittime(const char* stdCode) override;
	/**
	 * @brief 获取最近入场价格
	 * @details 获取指定合约的最近一次入场价格
	 * @param stdCode 标准合约代码
	 * @return 最近入场价格
	 */
	virtual double stra_get_last_enterprice(const char* stdCode) override;
	/**
	 * @brief 获取最近入场标签
	 * @details 获取指定合约的最近一次入场标签
	 * @param stdCode 标准合约代码
	 * @return 最近入场标签
	 */
	virtual const char* stra_get_last_entertag(const char* stdCode) override;
	/**
	 * @brief 获取持仓均价
	 * @details 获取指定合约的持仓均价
	 * @param stdCode 标准合约代码
	 * @return 持仓均价
	 */
	virtual double stra_get_position_avgpx(const char* stdCode) override;
	/**
	 * @brief 获取持仓盈亏
	 * @details 获取指定合约的持仓浮动盈亏
	 * @param stdCode 标准合约代码
	 * @return 持仓盈亏
	 */
	virtual double stra_get_position_profit(const char* stdCode) override;

	/**
	 * @brief 获取明细入场时间
	 * @details 获取指定合约和标签的入场时间
	 * @param stdCode 标准合约代码
	 * @param userTag 用户自定义标签
	 * @return 入场时间，格式为YYYYMMDDHHMMSSsss
	 */
	virtual uint64_t stra_get_detail_entertime(const char* stdCode, const char* userTag) override;
	/**
	 * @brief 获取明细入场成本
	 * @details 获取指定合约和标签的入场成本
	 * @param stdCode 标准合约代码
	 * @param userTag 用户自定义标签
	 * @return 入场成本
	 */
	virtual double stra_get_detail_cost(const char* stdCode, const char* userTag) override;
	/**
	 * @brief 获取明细盈亏
	 * @details 获取指定合约和标签的盈亏信息
	 * @param stdCode 标准合约代码
	 * @param userTag 用户自定义标签
	 * @param flag 盈亏标记，0表示当前盈亏，1表示最大盈利，-1表示最大亏损，2表示最高价，-2表示最低价
	 * @return 盈亏信息
	 */
	virtual double stra_get_detail_profit(const char* stdCode, const char* userTag, int flag = 0) override;

	/**
	 * @brief 获取品种信息
	 * @details 获取指定合约的品种信息
	 * @param stdCode 标准合约代码
	 * @return 品种信息指针
	 */
	virtual WTSCommodityInfo* stra_get_comminfo(const char* stdCode) override;
	/**
	 * @brief 获取交易时段信息
	 * @details 获取指定合约的交易时段信息
	 * @param stdCode 标准合约代码
	 * @return 交易时段信息指针
	 */
	virtual WTSSessionInfo* stra_get_sessinfo(const char* stdCode) override;
	/**
	 * @brief 获取K线数据切片
	 * @details 获取指定合约和周期的K线数据切片
	 * @param stdCode 标准合约代码
	 * @param period K线周期，如"m1"、"d1"等
	 * @param count 请求的K线数量
	 * @return K线数据切片指针
	 */
	virtual WTSKlineSlice*	stra_get_bars(const char* stdCode, const char* period, uint32_t count) override;
	/**
	 * @brief 获取Tick数据切片
	 * @details 获取指定合约的Tick数据切片
	 * @param stdCode 标准合约代码
	 * @param count 请求的Tick数量
	 * @return Tick数据切片指针
	 */
	virtual WTSTickSlice*	stra_get_ticks(const char* stdCode, uint32_t count) override;
	/**
	 * @brief 获取最新Tick数据
	 * @details 获取指定合约的最新Tick数据
	 * @param stdCode 标准合约代码
	 * @return 最新Tick数据指针
	 */
	virtual WTSTickData*	stra_get_last_tick(const char* stdCode) override;

	/**
	 * @brief 获取原始合约代码
	 * @details 获取标准合约代码对应的原始分月合约代码
	 * @param stdCode 标准合约代码
	 * @return 原始合约代码
	 */
	virtual std::string		stra_get_rawcode(const char* stdCode) override;

	/**
	 * @brief 订阅Tick数据
	 * @details 订阅指定合约的Tick数据
	 * @param stdCode 标准合约代码
	 */
	virtual void stra_sub_ticks(const char* stdCode) override;

	/**
	 * @brief 输出信息日志
	 * @details 输出信息级别的日志信息
	 * @param message 日志信息
	 */
	virtual void stra_log_info(const char* message) override;
	/**
	 * @brief 输出调试日志
	 * @details 输出调试级别的日志信息
	 * @param message 日志信息
	 */
	virtual void stra_log_debug(const char* message) override;
	/**
	 * @brief 输出警告日志
	 * @details 输出警告级别的日志信息
	 * @param message 日志信息
	 */
	virtual void stra_log_warn(const char* message) override;
	/**
	 * @brief 输出错误日志
	 * @details 输出错误级别的日志信息
	 * @param message 日志信息
	 */
	virtual void stra_log_error(const char* message) override;

	/**
	 * @brief 保存用户数据
	 * @details 保存策略的自定义数据，用于在不同的回调之间传递数据
	 * @param key 数据键
	 * @param val 数据值
	 */
	virtual void stra_save_user_data(const char* key, const char* val) override;

	/**
	 * @brief 加载用户数据
	 * @details 加载策略的自定义数据
	 * @param key 数据键
	 * @param defVal 默认值，当数据不存在时返回此值
	 * @return 数据值
	 */
	virtual const char* stra_load_user_data(const char* key, const char* defVal = "") override;

protected:
	/// @brief 策略上下文ID，用于唯一标识策略实例
	uint32_t			_context_id;
	/// @brief 历史数据回放器指针，用于提供历史数据
	HisDataReplayer*	_replayer;

	/// @brief 总计算时间，记录策略计算所用的总时间（纳秒）
	uint64_t		_total_calc_time;
	/// @brief 总计算次数，记录策略被调用的总次数
	uint32_t		_emit_times;
	/// @brief 成交滑点，模拟交易时的价格滑点
	int32_t			_slippage;
	/// @brief 是否使用比例滑点，true表示滑点按比例计算，false表示滑点按固定点数计算
	bool			_ratio_slippage;
	/// @brief 调度次数，记录策略被调度的总次数
	uint32_t		_schedule_times;

	/// @brief 主键，用于标识策略的唯一性
	std::string		_main_key;

	/**
	 * @brief K线标记结构体
	 * @details 用于记录K线的状态信息，包括是否已闭合和计数器
	 */
	typedef struct _KlineTag
	{
		/// @brief 是否已闭合，true表示K线已闭合，false表示K线未闭合
		bool		_closed;
		/// @brief 计数器，记录当前K线的计数
		uint32_t	_count;

		/**
		 * @brief 构造函数
		 * @details 初始化K线标记，默认为未闭合状态且计数为0
		 */
		_KlineTag() :_closed(false), _count(0){}

	} KlineTag;
	/// @brief K线标记映射表类型，用于存储各个合约的K线标记信息
	typedef wt_hashmap<std::string, KlineTag> KlineTags;
	/// @brief K线标记映射表，键为合约代码和周期组合，值为K线标记
	KlineTags	_kline_tags;

	/// @brief 价格信息类型，包含价格和时间戳
	typedef std::pair<double, uint64_t>	PriceInfo;
	/// @brief 价格映射表类型，用于存储各个合约的价格信息
	typedef wt_hashmap<std::string, PriceInfo> PriceMap;
	/// @brief 价格映射表，键为合约代码，值为价格信息
	PriceMap		_price_map;

	/**
	 * @brief 持仓明细结构体
	 * @details 用于记录持仓的详细信息，包括开仓价格、数量、时间等
	 */
	typedef struct _DetailInfo
	{
		/// @brief 是否为多头，true表示多头，false表示空头
		bool		_long;
		/// @brief 开仓价格
		double		_price;
		/// @brief 持仓数量
		double		_volume;
		/// @brief 开仓时间，格式为YYYYMMDDHHMMSSsss
		uint64_t	_opentime;
		/// @brief 开仓交易日，格式为YYYYMMDD
		uint32_t	_opentdate;
		/// @brief 最大浮盈，记录持仓期间的最大浮动盈利
		double		_max_profit;
		/// @brief 最大浮亏，记录持仓期间的最大浮动亏损
		double		_max_loss;
		/// @brief 最高价，记录持仓期间的最高价格
		double		_max_price;
		/// @brief 最低价，记录持仓期间的最低价格
		double		_min_price;
		/// @brief 当前盈亏，记录当前的浮动盈亏
		double		_profit;
		/// @brief 开仓标签，用于标记开仓的特殊含义
		char		_opentag[32];
		/// @brief 开仓K线序号，记录开仓发生在哪一根K线上
		uint32_t	_open_barno;

		/**
		 * @brief 构造函数
		 * @details 初始化持仓明细信息，将所有成员变量初始化为0
		 */
		_DetailInfo()
		{
			memset(this, 0, sizeof(_DetailInfo));
		}
	} DetailInfo;

	/**
	 * @brief 持仓信息结构体
	 * @details 用于记录指定合约的持仓汇总信息，包括总持仓量、盈亏、明细等
	 */
	typedef struct _PosInfo
	{
		/// @brief 总持仓量，正数表示多头，负数表示空头
		double		_volume;
		/// @brief 平仓盈亏，记录已平仓部分的盈亏
		double		_closeprofit;
		/// @brief 浮动盈亏，记录未平仓部分的浮动盈亏
		double		_dynprofit;
		/// @brief 最近入场时间，格式为YYYYMMDDHHMMSSsss
		uint64_t	_last_entertime;
		/// @brief 最近出场时间，格式为YYYYMMDDHHMMSSsss
		uint64_t	_last_exittime;
		/// @brief 冻结仓位数量，记录暂时冻结的仓位量
		double		_frozen;

		/// @brief 持仓明细列表，记录各笔交易的详细信息
		std::vector<DetailInfo> _details;

		/**
		 * @brief 构造函数
		 * @details 初始化持仓信息，将所有成员变量初始化为0
		 */
		_PosInfo()
		{
			_volume = 0;
			_closeprofit = 0;
			_dynprofit = 0;
			_frozen = 0;
			_last_entertime = 0;
			_last_exittime = 0;
		}

		/**
		 * @brief 获取有效仓位量
		 * @details 计算有效仓位量，排除冻结的部分
		 * @return 有效仓位量
		 */
		inline double valid() const { return _volume - _frozen; }
	} PosInfo;
	/// @brief 持仓映射表类型，用于存储各个合约的持仓信息
	typedef wt_hashmap<std::string, PosInfo> PositionMap;
	/// @brief 持仓映射表，键为合约代码，值为持仓信息
	PositionMap		_pos_map;

	/**
	 * @brief 信号信息结构体
	 * @details 用于记录交易信号的相关信息，包括目标仓位、价格、状态等
	 */
	typedef struct _SigInfo
	{
		/// @brief 目标仓位量，正数表示多头，负数表示空头
		double		_volume;
		/// @brief 用户自定义标签，用于标记信号的特殊含义
		std::string	_usertag;
		/// @brief 信号价格，记录生成信号时的价格
		double		_sigprice;
		/// @brief 目标价格，记录期望成交的价格
		double		_desprice;
		/// @brief 是否触发，标记信号是否已经被触发执行
		bool		_triggered;
		/// @brief 生成时间，信号生成的时间戳，格式为YYYYMMDDHHMMSSsss
		uint64_t	_gentime;

		/**
		 * @brief 构造函数
		 * @details 初始化信号信息，将所有成员变量初始化为0或false
		 */
		_SigInfo()
		{
			_volume = 0;
			_sigprice = 0;
			_desprice = 0;
			_triggered = false;
			_gentime = 0;
		}
	}SigInfo;
	/// @brief 信号映射表类型，用于存储各个合约的信号信息
	typedef wt_hashmap<std::string, SigInfo>	SignalMap;
	/// @brief 信号映射表，键为合约代码，值为信号信息
	SignalMap		_sig_map;

	/// @brief 交易日志流，记录交易相关的日志信息
	std::stringstream	_trade_logs;
	/// @brief 平仓日志流，记录平仓相关的日志信息
	std::stringstream	_close_logs;
	/// @brief 资金日志流，记录资金变动相关的日志信息
	std::stringstream	_fund_logs;
	/// @brief 信号日志流，记录交易信号相关的日志信息
	std::stringstream	_sig_logs;
	/// @brief 持仓日志流，记录持仓变动相关的日志信息
	std::stringstream	_pos_logs;

	/// @brief 是否处于调度中的标记，true表示当前正在执行调度，false表示非调度状态
	bool			_is_in_schedule;

	/// @brief 用户数据类型，用于存储策略的自定义数据
	typedef wt_hashmap<std::string, std::string> StringHashMap;
	/// @brief 用户数据映射表，键为数据名称，值为数据内容
	StringHashMap	_user_datas;
	/// @brief 用户数据是否被修改的标记，true表示数据已修改，false表示未修改
	bool			_ud_modified;

	/**
	 * @brief 策略资金信息结构体
	 * @details 用于记录策略的资金相关信息，包括盈亏和手续费
	 */
	typedef struct _StraFundInfo
	{
		/// @brief 总平仓盈亏，记录已平仓部分的总盈亏
		double	_total_profit;
		/// @brief 总浮动盈亏，记录未平仓部分的总浮动盈亏
		double	_total_dynprofit;
		/// @brief 总手续费，记录交易产生的总手续费
		double	_total_fees;

		/**
		 * @brief 构造函数
		 * @details 初始化策略资金信息，将所有成员变量初始化为0
		 */
		_StraFundInfo()
		{
			memset(this, 0, sizeof(_StraFundInfo));
		}
	} StraFundInfo;

	/// @brief 策略资金信息实例，记录当前策略的资金状况
	StraFundInfo		_fund_info;

	/**
	 * @brief 策略工厂信息结构体
	 * @details 用于记录选股策略工厂的相关信息，包括模块路径、工厂实例和函数指针等
	 */
	typedef struct _StraFactInfo
	{
		/// @brief 模块路径，记录存储策略库的路径
		std::string		_module_path;
		/// @brief 模块实例句柄，指向加载的动态链接库实例
		DllHandle		_module_inst;
		/// @brief 选股策略工厂指针，指向选股策略工厂实例
		ISelStrategyFact*		_fact;
		/// @brief 创建选股策略工厂的函数指针
		FuncCreateSelStraFact	_creator;
		/// @brief 删除选股策略工厂的函数指针
		FuncDeleteSelStraFact	_remover;

		/**
		 * @brief 构造函数
		 * @details 初始化策略工厂信息，将模块实例和工厂指针初始化为Null
		 */
		_StraFactInfo()
		{
			_module_inst = NULL;
			_fact = NULL;
		}

		/**
		 * @brief 析构函数
		 * @details 清理策略工厂资源，如果工厂实例存在则调用移除器函数删除
		 */
		~_StraFactInfo()
		{
			if (_fact)
				_remover(_fact);
		}
	} StraFactInfo;
	/// @brief 策略工厂信息实例，用于管理选股策略工厂
	StraFactInfo	_factory;

	/// @brief 选股策略指针，指向当前正在运行的选股策略实例
	SelStrategy*	_strategy;

	/// @brief 当前交易日，记录当前的交易日期，格式为YYYYMMDD
	uint32_t		_cur_tdate;

	/// @brief tick订阅列表，存储当前策略订阅的所有合约代码
	wt_hashset<std::string> _tick_subs;
};