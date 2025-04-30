/*!
 * \file CtaMocker.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief CTA策略回测模拟器头文件
 * \details 该文件定义了CTA策略回测模拟器，用于模拟策略在历史数据上的运行过程。
 *          模拟器实现了ICtaStraCtx和IDataSink接口，可以处理历史数据并执行策略逻辑。
 *          通过该模拟器，可以对CTA策略进行回测、调试和优化。
 */
#pragma once
#include <sstream>
#include <atomic>
#include <unordered_map>
#include "HisDataReplayer.h"

#include "../Includes/FasterDefs.h"
#include "../Includes/ICtaStraCtx.h"
#include "../Includes/CtaStrategyDefs.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSCollection.hpp"

#include "../Share/DLLHelper.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/fmtlib.h"

NS_WTP_BEGIN
class EventNotifier;
NS_WTP_END

USING_NS_WTP;

class HisDataReplayer;
class CtaStrategy;

/**
 * @brief 条件单动作类型常量定义
 * @details 定义了各种条件单的动作类型，用于标识不同的交易操作
 */
/**
 * @brief 条件单动作类型常量定义
 * @details 定义了各种条件单的动作类型，用于标识不同的交易操作
 */
const char COND_ACTION_OL = 0;	///<开多，建立多头仓位
const char COND_ACTION_CL = 1;	///<平多，平掉多头仓位
const char COND_ACTION_OS = 2;	///<开空，建立空头仓位
const char COND_ACTION_CS = 3;	///<平空，平掉空头仓位
const char COND_ACTION_SP = 4;	///<直接设置仓位，不区分多空

/**
 * @brief 条件委托结构体
 * @details 定义了条件委托的各个属性，用于实现条件单功能
 *          条件单是指当满足特定条件时触发的交易指令
 */
typedef struct _CondEntrust
{
	WTSCompareField _field;    ///< 比较字段，指定比较的是什么数据字段
	WTSCompareType	_alg;     ///< 比较算法，指定如何进行比较（大于、小于等）
	double			_target;    ///< 目标值，比较的目标数值

	double			_qty;       ///< 委托数量

	char			_action;    ///< 委托动作：0-开多,1-平多,2-开空,3-平空,4-设置仓位

	char			_code[MAX_INSTRUMENT_LENGTH];  ///< 标的码，合约代码
	char			_usertag[32];                  ///< 用户标记，用于标识不同的委托

	/**
	 * @brief 构造函数
	 * @details 初始化条件委托结构体，将所有成员设置为0
	 */
	_CondEntrust()
	{
		memset(this, 0, sizeof(_CondEntrust));
	}

} CondEntrust;

/**
 * @brief 条件委托列表类型
 * @details 定义了条件委托的列表类型，用于存储多个条件委托
 */
/**
 * @brief 条件委托列表类型
 * @details 定义了条件委托的列表类型，用于存储多个条件委托
 */
typedef std::vector<CondEntrust>	CondList;

/**
 * @brief 条件委托映射类型
 * @details 定义了从合约代码到条件委托列表的映射，用于按合约管理条件委托
 */
/**
 * @brief 条件委托映射类型
 * @details 定义了从合约代码到条件委托列表的映射，用于按合约管理条件委托
 */
typedef wt_hashmap<std::string, CondList>	CondEntrustMap;


/**
 * @brief CTA策略回测模拟器类
 * @details 实现了ICtaStraCtx和IDataSink接口，用于模拟策略在历史数据上的运行过程。
 *          该类是CTA策略回测的核心组件，负责处理历史数据、执行策略逻辑、模拟交易和记录回测结果。
 *          通过该类，可以对CTA策略进行全面的回测、调试和优化。
 */
class CtaMocker : public ICtaStraCtx, public IDataSink
{
public:
	/**
	 * @brief 构造函数
	 * @details 创建一个CTA策略回测模拟器实例
	 * @param replayer 历史数据回放器，用于提供回测所需的历史数据
	 * @param name 策略名称，用于标识不同的策略实例
	 * @param slippage 滑点设置，用于模拟实际交易中的滑点影响，默认为0
	 * @param persistData 是否持久化数据，如果为true则会保存回测结果，默认为true
	 * @param notifier 事件通知器，用于通知外部系统回测过程中的事件，默认为NULL
	 * @param isRatioSlp 是否使用比例滑点，如果为true则slippage表示万分比，默认为false
	 */
	CtaMocker(HisDataReplayer* replayer, const char* name, int32_t slippage = 0, bool persistData = true, EventNotifier* notifier = NULL, bool isRatioSlp = false);
	/**
	 * @brief 析构函数
	 * @details 释放CTA策略回测模拟器占用的资源
	 */
	virtual ~CtaMocker();

private:
	/**
	 * @brief 输出回测结果
	 * @details 将回测结果写入到输出文件中，包括交易日志、平仓日志、资金日志等
	 */
	void	dump_outputs();
	/**
	 * @brief 导出策略数据
	 * @details 将策略数据导出到JSON文件，包括仓位、资金、信号等信息
	 */
	void	dump_stradata();
	/**
	 * @brief 导出图表数据
	 * @details 将策略图表数据导出到JSON文件，用于回测结果可视化
	 */
	void	dump_chartdata();
	/**
	 * @brief 记录信号
	 * @details 记录策略生成的交易信号，包括目标仓位、价格、生成时间等
	 * @param stdCode 标准化合约代码
	 * @param target 目标仓位
	 * @param price 信号价格
	 * @param gentime 信号生成时间
	 * @param usertag 用户标记，默认为空字符串
	 */
	inline void log_signal(const char* stdCode, double target, double price, uint64_t gentime, const char* usertag = "");
	/**
	 * @brief 记录交易
	 * @details 记录策略执行的交易详情，包括合约、方向、开平仓、价格、数量等
	 * @param stdCode 标准化合约代码
	 * @param isLong 是否为多头交易
	 * @param isOpen 是否为开仓交易
	 * @param curTime 当前时间
	 * @param price 交易价格
	 * @param qty 交易数量
	 * @param userTag 用户标记，默认为空字符串
	 * @param fee 交易手续费，默认为0.0
	 * @param barNo K线序号，默认为0
	 */
	inline void	log_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, const char* userTag = "", double fee = 0.0, uint32_t barNo = 0);
	/**
	 * @brief 记录平仓
	 * @details 记录策略平仓的详细信息，包括开仓时间、平仓时间、价格、盈亏等
	 * @param stdCode 标准化合约代码
	 * @param isLong 是否为多头交易
	 * @param openTime 开仓时间
	 * @param openpx 开仓价格
	 * @param closeTime 平仓时间
	 * @param closepx 平仓价格
	 * @param qty 交易数量
	 * @param profit 交易盈亏
	 * @param maxprofit 最大盈利
	 * @param maxloss 最大亏损
	 * @param totalprofit 总盈亏，默认为0
	 * @param enterTag 开仓标记，默认为空字符串
	 * @param exitTag 平仓标记，默认为空字符串
	 * @param openBarNo 开仓K线序号，默认为0
	 * @param closeBarNo 平仓K线序号，默认为0
	 */
	inline void	log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty,
		double profit, double maxprofit, double maxloss, double totalprofit = 0, const char* enterTag = "", const char* exitTag = "", uint32_t openBarNo = 0, uint32_t closeBarNo = 0);

	/**
	 * @brief 更新动态盈亏
	 * @details 根据当前价格更新指定合约的浮动盈亏
	 * @param stdCode 标准化合约代码
	 * @param price 当前价格
	 */
	void	update_dyn_profit(const char* stdCode, double price);

	/**
	 * @brief 设置仓位
	 * @details 直接设置指定合约的目标仓位，系统会自动计算需要交易的数量
	 * @param stdCode 标准化合约代码
	 * @param qty 目标仓位数量，正数表示多头，负数表示空头
	 * @param price 交易价格，默认为0.0（市价单）
	 * @param userTag 用户标记，默认为空字符串
	 */
	void	do_set_position(const char* stdCode, double qty, double price = 0.0, const char* userTag = "");
	/**
	 * @brief 添加信号
	 * @details 添加交易信号到信号列表中
	 * @param stdCode 标准化合约代码
	 * @param qty 交易数量，正数表示多头，负数表示空头
	 * @param userTag 用户标记
	 * @param price 信号价格
	 * @param sigType 信号类型
	 */
	void	append_signal(const char* stdCode, double qty, const char* userTag, double price, uint32_t sigType);

	/**
	 * @brief 获取条件委托列表
	 * @details 获取指定合约的条件委托列表，如果不存在则创建新的列表
	 * @param stdCode 标准化合约代码
	 * @return 返回条件委托列表的引用
	 */
	inline CondList& get_cond_entrusts(const char* stdCode);

	/**
	 * @brief 处理Tick数据
	 * @details 处理Tick数据并检查条件委托是否触发
	 * @param stdCode 标准化合约代码
	 * @param last_px 上一次价格
	 * @param cur_px 当前价格
	 */
	void	proc_tick(const char* stdCode, double last_px, double cur_px);

public:
	/**
	 * @brief 初始化CTA策略工厂
	 * @details 根据配置初始化CTA策略工厂，并加载策略模块
	 * @param cfg 策略配置，包含策略模块路径等信息
	 * @return 返回初始化是否成功
	 */
	bool	init_cta_factory(WTSVariant* cfg);
	/**
	 * @brief 加载增量回测数据
	 * @details 从指定的上一次回测结果中加载增量数据，用于继续回测
	 * @param lastBacktestName 上一次回测的名称，用于定位数据文件
	 */
	void	load_incremental_data(const char* lastBacktestName);
	/**
	 * @brief 安装钩子
	 * @details 安装用于控制回测过程的钩子函数，使得回测可以暂停和步进
	 */
	void	install_hook();
	/**
	 * @brief 启用或禁用钩子
	 * @details 控制是否启用回测钩子函数
	 * @param bEnabled 是否启用钩子，默认为true
	 */
	void	enable_hook(bool bEnabled = true);
	/**
	 * @brief 单步计算
	 * @details 执行一步回测计算，用于手动控制回测过程
	 * @return 返回计算是否成功
	 */
	bool	step_calc();

public:
	//////////////////////////////////////////////////////////////////////////
	//IDataSink
	/**
	 * @brief 处理Tick数据
	 * @details 实现IDataSink接口的方法，处理历史数据回放器提供的Tick数据
	 * @param stdCode 标准化合约代码
	 * @param curTick 当前Tick数据对象
	 * @param pxType 价格类型，默认为0
	 */
	virtual void	handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType = 0) override;
	/**
	 * @brief 处理K线关闭
	 * @details 实现IDataSink接口的方法，处理K线关闭事件，触发策略的on_bar_close回调
	 * @param stdCode 标准化合约代码
	 * @param period K线周期，如"m1"/"m5"/"d1"等
	 * @param times 周期倍数
	 * @param newBar 新的K线数据
	 */
	virtual void	handle_bar_close(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;
	/**
	 * @brief 处理调度事件
	 * @details 实现IDataSink接口的方法，处理定时调度事件，触发策略的on_schedule回调
	 * @param uDate 当前日期，格式为YYYYMMDD
	 * @param uTime 当前时间，格式为HHMMSS
	 */
	virtual void	handle_schedule(uint32_t uDate, uint32_t uTime) override;

	/**
	 * @brief 处理初始化
	 * @details 实现IDataSink接口的方法，处理初始化事件，触发策略的on_init回调
	 */
	virtual void	handle_init() override;
	/**
	 * @brief 处理交易日开始
	 * @details 实现IDataSink接口的方法，处理交易日开始事件，触发策略的on_session_begin回调
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 */
	virtual void	handle_session_begin(uint32_t curTDate) override;
	/**
	 * @brief 处理交易日结束
	 * @details 实现IDataSink接口的方法，处理交易日结束事件，触发策略的on_session_end回调
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 */
	virtual void	handle_session_end(uint32_t curTDate) override;

	/**
	 * @brief 处理交易时段结束
	 * @details 实现IDataSink接口的方法，处理交易时段结束事件，清理价格缓存
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMMSS
	 */
	virtual void	handle_section_end(uint32_t curTDate, uint32_t curTime) override;

	/**
	 * @brief 处理回放完成
	 * @details 实现IDataSink接口的方法，处理历史数据回放完成事件，输出回测结果
	 */
	virtual void	handle_replay_done() override;

	//////////////////////////////////////////////////////////////////////////
	//ICtaStraCtx
	/**
	 * @brief 获取策略上下文ID
	 * @details 实现ICtaStraCtx接口的方法，返回策略上下文的唯一标识符
	 * @return 返回策略上下文ID
	 */
	virtual uint32_t id() { return _context_id; }

	//回调函数
	/**
	 * @brief 初始化回调
	 * @details 实现ICtaStraCtx接口的方法，在策略初始化时调用，用于执行策略的初始化逻辑
	 */
	virtual void on_init() override;
	/**
	 * @brief 交易日开始回调
	 * @details 实现ICtaStraCtx接口的方法，在每个交易日开始时调用，用于执行策略在交易日开始时的逻辑
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 */
	virtual void on_session_begin(uint32_t curTDate) override;
	/**
	 * @brief 交易日结束回调
	 * @details 实现ICtaStraCtx接口的方法，在每个交易日结束时调用，用于执行策略在交易日结束时的逻辑
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 */
	virtual void on_session_end(uint32_t curTDate) override;
	/**
	 * @brief Tick数据回调
	 * @details 实现ICtaStraCtx接口的方法，在接收到Tick数据时调用，用于处理最新的市场行情数据
	 * @param stdCode 标准化合约代码
	 * @param newTick 新的Tick数据
	 * @param bEmitStrategy 是否触发策略计算，默认为true
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* newTick, bool bEmitStrategy = true) override;
	/**
	 * @brief K线数据回调
	 * @details 实现ICtaStraCtx接口的方法，在接收到K线数据时调用，用于处理K线数据
	 * @param stdCode 标准化合约代码
	 * @param period K线周期，如"m1"/"m5"/"d1"等
	 * @param times 周期倍数
	 * @param newBar 新的K线数据
	 */
	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;
	/**
	 * @brief 定时调度回调
	 * @details 实现ICtaStraCtx接口的方法，在定时调度时调用，用于执行定时任务
	 * @param curDate 当前日期，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMMSS
	 * @return 返回处理是否成功
	 */
	virtual bool on_schedule(uint32_t curDate, uint32_t curTime) override;
	/**
	 * @brief 枚举持仓回调
	 * @details 实现ICtaStraCtx接口的方法，用于枚举当前所有持仓信息
	 * @param cb 持仓回调函数
	 * @param bForExecute 是否为执行目的而枚举
	 */
	virtual void enum_position(FuncEnumCtaPosCallBack cb, bool bForExecute) override;

	/**
	 * @brief Tick数据更新回调
	 * @details 实现ICtaStraCtx接口的方法，在Tick数据更新时调用，与on_tick不同的是不触发策略计算
	 * @param stdCode 标准化合约代码
	 * @param newTick 新的Tick数据
	 */
	virtual void on_tick_updated(const char* stdCode, WTSTickData* newTick) override;
	/**
	 * @brief K线关闭回调
	 * @details 实现ICtaStraCtx接口的方法，在K线关闭时调用，用于处理K线关闭事件
	 * @param stdCode 标准化合约代码
	 * @param period K线周期，如"m1"/"m5"/"d1"等
	 * @param newBar 新的K线数据
	 */
	virtual void on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) override;
	/**
	 * @brief 计算回调
	 * @details 实现ICtaStraCtx接口的方法，在需要进行策略计算时调用
	 * @param curDate 当前日期，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMMSS
	 */
	virtual void on_calculate(uint32_t curDate, uint32_t curTime) override;

	//////////////////////////////////////////////////////////////////////////
	//策略接口
	/**
	 * @brief 开多仓接口
	 * @details 实现ICtaStraCtx接口的方法，用于执行多头开仓操作
	 * @param stdCode 标准化合约代码
	 * @param qty 交易数量
	 * @param userTag 用户标记，用于标识交易来源，默认为空字符串
	 * @param limitprice 限价，限定价格成交，默认为0.0（市价单）
	 * @param stopprice 止损价，止损价格，默认为0.0（不设置止损）
	 */
	virtual void stra_enter_long(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override;
	/**
	 * @brief 开空仓接口
	 * @details 实现ICtaStraCtx接口的方法，用于执行空头开仓操作
	 * @param stdCode 标准化合约代码
	 * @param qty 交易数量
	 * @param userTag 用户标记，用于标识交易来源，默认为空字符串
	 * @param limitprice 限价，限定价格成交，默认为0.0（市价单）
	 * @param stopprice 止损价，止损价格，默认为0.0（不设置止损）
	 */
	virtual void stra_enter_short(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override;
	/**
	 * @brief 平多仓接口
	 * @details 实现ICtaStraCtx接口的方法，用于执行多头平仓操作
	 * @param stdCode 标准化合约代码
	 * @param qty 交易数量
	 * @param userTag 用户标记，用于标识交易来源，默认为空字符串
	 * @param limitprice 限价，限定价格成交，默认为0.0（市价单）
	 * @param stopprice 止损价，止损价格，默认为0.0（不设置止损）
	 */
	virtual void stra_exit_long(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override;
	/**
	 * @brief 平空仓接口
	 * @details 实现ICtaStraCtx接口的方法，用于执行空头平仓操作
	 * @param stdCode 标准化合约代码
	 * @param qty 交易数量
	 * @param userTag 用户标记，用于标识交易来源，默认为空字符串
	 * @param limitprice 限价，限定价格成交，默认为0.0（市价单）
	 * @param stopprice 止损价，止损价格，默认为0.0（不设置止损）
	 */
	virtual void stra_exit_short(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override;

	/**
	 * @brief 获取仓位接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取指定合约的当前仓位
	 * @param stdCode 标准化合约代码
	 * @param bOnlyValid 是否只返回有效仓位（不包括冻结仓位），默认为false
	 * @param userTag 用户标记，用于过滤特定标记的仓位，默认为空字符串
	 * @return 返回仓位数量，正数表示多头，负数表示空头
	 */
	virtual double stra_get_position(const char* stdCode, bool bOnlyValid = false, const char* userTag = "") override;
	/**
	 * @brief 设置仓位接口
	 * @details 实现ICtaStraCtx接口的方法，用于直接设置指定合约的目标仓位
	 * @param stdCode 标准化合约代码
	 * @param qty 目标仓位数量，正数表示多头，负数表示空头
	 * @param userTag 用户标记，用于标识交易来源，默认为空字符串
	 * @param limitprice 限价，限定价格成交，默认为0.0（市价单）
	 * @param stopprice 止损价，止损价格，默认为0.0（不设置止损）
	 */
	virtual void stra_set_position(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override;
	/**
	 * @brief 获取当前价格接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取指定合约的当前市场价格
	 * @param stdCode 标准化合约代码
	 * @return 返回当前价格
	 */
	virtual double stra_get_price(const char* stdCode) override;

	/**
	 * @brief 获取当日价格接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取指定合约当日的开盘价、最高价、最低价或收盘价
	 * @param stdCode 标准化合约代码
	 * @param flag 价格标记，0-开盘价，1-最高价，2-最低价，3-收盘价，默认为0
	 * @return 返回当日价格
	 */
	virtual double stra_get_day_price(const char* stdCode, int flag = 0) override;

	/**
	 * @brief 获取当前交易日接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取当前的交易日期
	 * @return 返回当前交易日，格式为YYYYMMDD
	 */
	virtual uint32_t stra_get_tdate() override;
	/**
	 * @brief 获取当前日期接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取当前的自然日期
	 * @return 返回当前日期，格式为YYYYMMDD
	 */
	virtual uint32_t stra_get_date() override;
	/**
	 * @brief 获取当前时间接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取当前的时间
	 * @return 返回当前时间，格式为HHMMSS或HHMMSS.mmm
	 */
	virtual uint32_t stra_get_time() override;

	/**
	 * @brief 获取资金数据接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取策略账户的资金数据
	 * @param flag 资金数据标记，0-动态权益，1-静态权益，2-可用资金，默认为0
	 * @return 返回指定类型的资金数据
	 */
	virtual double stra_get_fund_data(int flag = 0) override;

	/**
	 * @brief 获取首次开仓时间接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取指定合约的首次开仓时间
	 * @param stdCode 标准化合约代码
	 * @return 返回首次开仓时间，以时间戳形式表示
	 */
	virtual uint64_t stra_get_first_entertime(const char* stdCode) override;
	/**
	 * @brief 获取最近开仓时间接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取指定合约的最近开仓时间
	 * @param stdCode 标准化合约代码
	 * @return 返回最近开仓时间，以时间戳形式表示
	 */
	virtual uint64_t stra_get_last_entertime(const char* stdCode) override;
	/**
	 * @brief 获取最近平仓时间接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取指定合约的最近平仓时间
	 * @param stdCode 标准化合约代码
	 * @return 返回最近平仓时间，以时间戳形式表示
	 */
	virtual uint64_t stra_get_last_exittime(const char* stdCode) override;
	/**
	 * @brief 获取最近开仓价格接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取指定合约的最近开仓价格
	 * @param stdCode 标准化合约代码
	 * @return 返回最近开仓价格
	 */
	virtual double stra_get_last_enterprice(const char* stdCode) override;
	/**
	 * @brief 获取最近开仓标记接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取指定合约的最近开仓标记
	 * @param stdCode 标准化合约代码
	 * @return 返回最近开仓标记
	 */
	virtual const char* stra_get_last_entertag(const char* stdCode) override;
	/**
	 * @brief 获取持仓均价接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取指定合约的持仓均价
	 * @param stdCode 标准化合约代码
	 * @return 返回持仓均价
	 */
	virtual double stra_get_position_avgpx(const char* stdCode) override;
	/**
	 * @brief 获取持仓盈亏接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取指定合约的持仓盈亏
	 * @param stdCode 标准化合约代码
	 * @return 返回持仓盈亏
	 */
	virtual double stra_get_position_profit(const char* stdCode) override;

	/**
	 * @brief 获取详细开仓时间接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取指定合约和标记的开仓时间
	 * @param stdCode 标准化合约代码
	 * @param userTag 用户标记
	 * @return 返回开仓时间，以时间戳形式表示
	 */
	virtual uint64_t stra_get_detail_entertime(const char* stdCode, const char* userTag) override;
	/**
	 * @brief 获取详细开仓成本接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取指定合约和标记的开仓成本
	 * @param stdCode 标准化合约代码
	 * @param userTag 用户标记
	 * @return 返回开仓成本
	 */
	virtual double stra_get_detail_cost(const char* stdCode, const char* userTag) override;
	/**
	 * @brief 获取详细盈亏接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取指定合约和标记的盈亏情况
	 * @param stdCode 标准化合约代码
	 * @param userTag 用户标记
	 * @param flag 盈亏标记，0-浮动盈亏，1-平仓盈亏，默认为0
	 * @return 返回盈亏金额
	 */
	virtual double stra_get_detail_profit(const char* stdCode, const char* userTag, int flag = 0) override;

	/**
	 * @brief 获取品种信息接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取指定合约的品种信息
	 * @param stdCode 标准化合约代码
	 * @return 返回品种信息对象指针，使用完毕需要释放
	 */
	virtual WTSCommodityInfo* stra_get_comminfo(const char* stdCode) override;
	/**
	 * @brief 获取K线切片接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取指定合约的历史K线数据
	 * @param stdCode 标准化合约代码
	 * @param period K线周期，如"m1"/"m5"/"d1"等
	 * @param count 请求的K线数量
	 * @param isMain 是否为主功K线，默认为false
	 * @return 返回K线切片对象指针，使用完毕需要释放
	 */
	virtual WTSKlineSlice*	stra_get_bars(const char* stdCode, const char* period, uint32_t count, bool isMain = false) override;
	/**
	 * @brief 获取Tick切片接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取指定合约的历史Tick数据
	 * @param stdCode 标准化合约代码
	 * @param count 请求的Tick数量
	 * @return 返回Tick切片对象指针，使用完毕需要释放
	 */
	virtual WTSTickSlice*	stra_get_ticks(const char* stdCode, uint32_t count) override;
	/**
	 * @brief 获取最新Tick接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取指定合约的最新Tick数据
	 * @param stdCode 标准化合约代码
	 * @return 返回最新Tick数据对象指针，使用完毕需要释放
	 */
	virtual WTSTickData*	stra_get_last_tick(const char* stdCode) override;

	/**
	 * @brief 订阅Tick数据接口
	 * @details 实现ICtaStraCtx接口的方法，用于订阅指定合约的Tick数据
	 * @param stdCode 标准化合约代码
	 */
	virtual void stra_sub_ticks(const char* stdCode) override;
	/**
	 * @brief 订阅K线事件接口
	 * @details 实现ICtaStraCtx接口的方法，用于订阅指定合约的K线事件
	 * @param stdCode 标准化合约代码
	 * @param period K线周期，如"m1"/"m5"/"d1"等
	 */
	virtual void stra_sub_bar_events(const char* stdCode, const char* period) override;

	/**
	 * @brief 获取原始合约代码接口
	 * @details 实现ICtaStraCtx接口的方法，用于获取标准化合约代码对应的原始合约代码
	 * @param stdCode 标准化合约代码
	 * @return 返回原始合约代码
	 */
	virtual std::string		stra_get_rawcode(const char* stdCode) override;

	/**
	 * @brief 记录信息日志接口
	 * @details 实现ICtaStraCtx接口的方法，用于记录策略信息日志
	 * @param message 日志消息
	 */
	virtual void stra_log_info(const char* message) override;
	/**
	 * @brief 记录调试日志接口
	 * @details 实现ICtaStraCtx接口的方法，用于记录策略调试日志
	 * @param message 日志消息
	 */
	virtual void stra_log_debug(const char* message) override;
	/**
	 * @brief 记录警告日志接口
	 * @details 实现ICtaStraCtx接口的方法，用于记录策略警告日志
	 * @param message 日志消息
	 */
	virtual void stra_log_warn(const char* message) override;
	/**
	 * @brief 记录错误日志接口
	 * @details 实现ICtaStraCtx接口的方法，用于记录策略错误日志
	 * @param message 日志消息
	 */
	virtual void stra_log_error(const char* message) override;

	/**
	 * @brief 保存用户数据接口
	 * @details 实现ICtaStraCtx接口的方法，用于保存策略用户数据
	 * @param key 数据键
	 * @param val 数据值
	 */
	virtual void stra_save_user_data(const char* key, const char* val) override;
	/**
	 * @brief 加载用户数据接口
	 * @details 实现ICtaStraCtx接口的方法，用于加载策略用户数据
	 * @param key 数据键
	 * @param defVal 默认值，当数据不存在时返回，默认为空字符串
	 * @return 返回数据值
	 */
	virtual const char* stra_load_user_data(const char* key, const char* defVal = "") override;

	/**
	 * @brief 设置图表K线接口
	 * @details 实现ICtaStraCtx接口的方法，用于设置图表展示的K线数据
	 * @param stdCode 标准化合约代码，指定要展示的合约
	 * @param period K线周期，如"m1"/"m5"/"d1"等
	 */
	virtual void set_chart_kline(const char* stdCode, const char* period) override;

	/**
	 * @brief 添加图表标记接口
	 * @details 实现ICtaStraCtx接口的方法，用于在图表上添加标记点
	 * @param price 标记价格，标记点的纵坐标位置
	 * @param icon 标记图标，用于指定标记的图标样式
	 * @param tag 标记标签，用于描述标记的含义
	 */
	virtual void add_chart_mark(double price, const char* icon, const char* tag) override;

	/**
	 * @brief 注册图表指标接口
	 * @details 实现ICtaStraCtx接口的方法，用于注册新的图表指标
	 * @param idxName 指标名称，用于唯一标识指标
	 * @param indexType 指标类型，用于指定指标的类型
	 */
	virtual void register_index(const char* idxName, uint32_t indexType) override;

	/**
	 * @brief 注册指标线接口
	 * @details 实现ICtaStraCtx接口的方法，用于在指定指标上注册新的线条
	 * @param idxName 指标名称，指定要添加线条的指标
	 * @param lineName 线条名称，用于唯一标识线条
	 * @param lineType 线条类型，用于指定线条的呈现方式
	 * @return 返回注册是否成功
	 */
	virtual bool register_index_line(const char* idxName, const char* lineName, uint32_t lineType) override;

	/**
	 * @brief 添加指标基准线接口
	 * @details 实现ICtaStraCtx接口的方法，用于在指定指标上添加基准线
	 * @param idxName 指标名称，指定要添加基准线的指标
	 * @param lineName 线条名称，用于唯一标识基准线
	 * @param val 基准线数值，基准线的固定值
	 * @return 返回添加是否成功
	 */
	virtual bool add_index_baseline(const char* idxName, const char* lineName, double val) override;

	/**
	 * @brief 设置指标值接口
	 * @details 实现ICtaStraCtx接口的方法，用于设置指定指标线的当前值
	 * @param idxName 指标名称，指定要设置值的指标
	 * @param lineName 线条名称，指定要设置值的线条
	 * @param val 指标值，要设置的数值
	 * @return 返回设置是否成功
	 */
	virtual bool set_index_value(const char* idxName, const char* lineName, double val) override;

private:
	/**
	 * @brief 调试日志辅助函数
	 * @details 用于格式化并输出调试日志，支持参数格式化
	 * @tparam Args 参数类型包，可变参数模板
	 * @param format 格式化字符串，类似printf的格式
	 * @param args 可变参数列表，用于填充格式化字符串
	 */
	template<typename... Args>
	void log_debug(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_debug(buffer);
	}

	/**
	 * @brief 信息日志辅助函数
	 * @details 用于格式化并输出信息日志，支持参数格式化
	 * @tparam Args 参数类型包，可变参数模板
	 * @param format 格式化字符串，类似printf的格式
	 * @param args 可变参数列表，用于填充格式化字符串
	 */
	template<typename... Args>
	void log_info(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_info(buffer);
	}

	/**
	 * @brief 错误日志辅助函数
	 * @details 用于格式化并输出错误日志，支持参数格式化
	 * @tparam Args 参数类型包，可变参数模板
	 * @param format 格式化字符串，类似printf的格式
	 * @param args 可变参数列表，用于填充格式化字符串
	 */
	template<typename... Args>
	void log_error(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_error(buffer);
	}

protected:
	uint32_t			_context_id;		///<策略上下文ID，策略实例的唯一标识符
	HisDataReplayer*	_replayer;		///<历史数据回放器，用于提供回测所需的历史数据

	uint64_t		_total_calc_time;	//总计算时间
	uint32_t		_emit_times;		//总计算次数

	int32_t			_slippage;			///<成交滑点，如果是比例滑点，则为万分比
	bool			_ratio_slippage;	///<是否比例滑点，标记滑点是固定点数还是按比例计算

	uint32_t		_schedule_times;	///<调度次数，记录策略被调度的次数

	std::string		_main_key;		///<主要合约的唯一键，由合约代码和周期组成

	std::string		_main_code;		///<主要合约代码
	std::string		_main_period;	///<主要周期

	/**
	 * @brief K线标记结构体
	 * @details 用于记录K线的状态信息，包括是否已关闭和是否已通知
	 */
	typedef struct _KlineTag
	{
		bool	_closed;		///< 是否已关闭
		bool	_notify;		///< 是否已通知

		/**
		 * @brief 构造函数
		 * @details 初始化K线标记，默认未关闭且未通知
		 */
		_KlineTag() :_closed(false), _notify(false){}

	} KlineTag;
	/**
	 * @brief K线标记映射类型
	 * @details 定义了从合约代码到K线标记的映射，用于管理不同合约的K线状态
	 */
	typedef wt_hashmap<std::string, KlineTag> KlineTags;
	KlineTags	_kline_tags;

	/**
	 * @brief 价格映射类型
	 * @details 定义了从合约代码到价格的映射，用于管理不同合约的当前价格
	 */
	typedef wt_hashmap<std::string, double> PriceMap;
	PriceMap		_price_map;

	/**
	 * @brief 交易详情结构体
	 * @details 用于记录每笔交易的详细信息，包括方向、价格、数量、时间和盈亏等
	 */
	typedef struct _DetailInfo
	{
		bool		_long;			///< 交易方向，true表示多头，false表示空头
		double		_price;			///< 开仓价格
		double		_volume;		///< 交易数量
		uint64_t	_opentime;		///< 开仓时间，以时间戳形式表示
		uint32_t	_opentdate;		///< 开仓交易日，格式为YYYYMMDD
		double		_max_profit;	///< 最大盈利
		double		_max_loss;		///< 最大亏损
		double		_max_price;		///< 持仓期间最高价
		double		_min_price;		///< 持仓期间最低价
		double		_profit;		///< 当前盈亏
		char		_opentag[32];	///< 开仓标记，用于标识交易来源
		uint32_t	_open_barno;	///< 开仓时K线序号

		/**
		 * @brief 构造函数
		 * @details 初始化交易详情，将所有成员设置为0
		 */
		_DetailInfo()
		{
			memset(this, 0, sizeof(_DetailInfo));
		}
	} DetailInfo;

	/**
	 * @brief 持仓信息结构体
	 * @details 用于记录合约的持仓情况，包括持仓量、盈亏、时间等信息
	 */
	typedef struct _PosInfo
	{
		double		_volume;			///< 持仓量，正数表示多头，负数表示空头
		double		_closeprofit;		///< 平仓盈亏
		double		_dynprofit;		///< 浮动盈亏
		uint64_t	_last_entertime;	///< 最近开仓时间
		uint64_t	_last_exittime;	///< 最近平仓时间
		double		_frozen;			///< 冻结仓位

		std::vector<DetailInfo> _details;	///< 交易详情列表

		/**
		 * @brief 构造函数
		 * @details 初始化持仓信息，将所有成员设置为0
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
		 * @brief 获取有效仓位
		 * @details 计算当前有效仓位，即总仓位减去冻结仓位
		 * @return 返回有效仓位数量
		 */
		inline double valid() const { return _volume - _frozen; }
	} PosInfo;
	/**
	 * @brief 持仓映射类型
	 * @details 定义了从合约代码到持仓信息的映射，用于管理不同合约的持仓情况
	 */
	typedef wt_hashmap<std::string, PosInfo> PositionMap;
	PositionMap		_pos_map;
	double	_total_closeprofit;		///< 总平仓盈亏，记录所有平仓交易的盈亏总和

	/**
	 * @brief 信号信息结构体
	 * @details 用于记录策略生成的交易信号信息，包括数量、价格、类型等
	 */
	typedef struct _SigInfo
	{
		double		_volume;		///< 信号数量，正数表示多头，负数表示空头
		std::string	_usertag;		///< 用户标记，用于标识信号来源
		double		_sigprice;		///< 信号价格，信号生成时的价格
		double		_desprice;		///< 目标价格，期望成交的价格
		uint32_t	_sigtype;		///< 信号类型
		uint64_t	_gentime;		///< 信号生成时间

		/**
		 * @brief 构造函数
		 * @details 初始化信号信息，将数值型成员设置为0
		 */
		_SigInfo()
		{
			_volume = 0;
			_sigprice = 0;
			_desprice = 0;
			_sigtype = 0;
			_gentime = 0;
		}
	}SigInfo;
	/**
	 * @brief 信号映射类型
	 * @details 定义了从合约代码到信号信息的映射，用于管理不同合约的信号
	 */
	typedef wt_hashmap<std::string, SigInfo>	SignalMap;
	SignalMap		_sig_map;

	std::stringstream	_trade_logs;		///< 交易日志流，用于记录所有交易操作
	std::stringstream	_close_logs;		///< 平仓日志流，用于记录平仓操作及其盈亏
	std::stringstream	_fund_logs;		///< 资金日志流，用于记录资金变化
	std::stringstream	_sig_logs;		///< 信号日志流，用于记录策略产生的信号
	std::stringstream	_pos_logs;		///< 持仓日志流，用于记录持仓变化
	std::stringstream	_index_logs;		///< 指标日志流，用于记录策略指标数据
	std::stringstream	_mark_logs;		///< 标记日志流，用于记录特定标记点

	CondEntrustMap		_condtions;		///< 条件委托映射，用于管理各个合约的条件委托

	/**
	 * @brief 调度标记
	 * @details 用于标记当前是否处于自动调度中
	 */
	bool			_is_in_schedule;	///< 是否在自动调度中

	/**
	 * @brief 用户数据相关
	 * @details 用于存储和管理用户自定义数据
	 */
	typedef wt_hashmap<std::string, std::string> StringHashMap;
	StringHashMap	_user_datas;		///< 用户数据映射，键值对形式存储
	bool			_ud_modified;		///< 用户数据是否已修改，用于标记是否需要持久化

	/**
	 * @brief 策略资金信息结构体
	 * @details 用于记录策略账户的资金状况，包括盈亏和手续费等
	 */
	typedef struct _StraFundInfo
	{
		double	_total_profit;		///< 总盈亏，已实现的盈亏
		double	_total_dynprofit;	///< 总浮动盈亏，未实现的盈亏
		double	_total_fees;		///< 总手续费

		/**
		 * @brief 构造函数
		 * @details 初始化策略资金信息，将所有成员设置为0
		 */
		_StraFundInfo()
		{
			memset(this, 0, sizeof(_StraFundInfo));
		}
	} StraFundInfo;

	StraFundInfo		_fund_info;		///< 策略资金信息，记录策略账户的资金状况

	/**
	 * @brief 策略工厂信息结构体
	 * @details 用于管理CTA策略工厂的动态库加载和实例创建
	 */
	typedef struct _StraFactInfo
	{
		std::string		_module_path;		///< 模块路径，动态库文件路径
		DllHandle		_module_inst;		///< 模块实例，动态库句柄
		ICtaStrategyFact*	_fact;			///< 策略工厂接口指针
		FuncCreateStraFact	_creator;		///< 创建工厂函数指针
		FuncDeleteStraFact	_remover;		///< 删除工厂函数指针

		/**
		 * @brief 构造函数
		 * @details 初始化策略工厂信息，将指针成员设置为NULL
		 */
		_StraFactInfo()
		{
			_module_inst = NULL;
			_fact = NULL;
		}

		/**
		 * @brief 析构函数
		 * @details 释放策略工厂资源，如果工厂存在则调用删除函数
		 */
		~_StraFactInfo()
		{
			if (_fact)
				_remover(_fact);
		}
	} StraFactInfo;
	StraFactInfo	_factory;			///< 策略工厂信息，管理策略工厂的加载和创建

	CtaStrategy*	_strategy;			///< 策略对象指针，指向实际的CTA策略实例
	EventNotifier*	_notifier;			///< 事件通知器指针，用于发送事件通知

	StdUniqueMutex	_mtx_calc;			///< 计算锁，用于同步策略计算
	StdCondVariable	_cond_calc;			///< 计算条件变量，用于等待和通知计算完成
	bool			_has_hook;		///< 是否有钩子，这是人为控制是否启用钩子
	bool			_hook_valid;		///< 钩子是否有效，这是根据是否是异步回测模式而确定钩子是否可用
	std::atomic<uint32_t>		_cur_step;	///< 当前步骤，临时变量，用于控制状态

	bool			_in_backtest;		///< 是否在回测中，标记当前是否处于回测过程中
	bool			_wait_calc;			///< 是否等待计算，标记是否需要等待策略计算完成

	/**
	 * @brief 持久化标记
	 * @details 标记是否需要对回测结果进行持久化存储
	 */
	bool			_persist_data;		///< 是否对回测结果持久化

	uint32_t		_cur_tdate;			///< 当前交易日，格式为YYYYMMDD
	uint32_t		_cur_bartime;		///< 当前K线时间，格式为HHMMSS
	uint64_t		_last_cond_min;		///< 最近条件检查时间，以分钟为单位

	/**
	 * @brief Tick订阅列表
	 * @details 存储已订阅Tick数据的合约代码集合
	 */
	wt_hashset<std::string> _tick_subs;	///< Tick订阅列表，存储已订阅的合约代码

	std::string		_chart_code;		///< 图表合约代码，用于图表展示的合约
	std::string		_chart_period;		///< 图表周期，用于图表展示的K线周期

	/**
	 * @brief 图表线结构体
	 * @details 用于定义图表中的线条信息，包括名称和类型
	 */
	typedef struct _ChartLine
	{
		std::string	_name;			///< 线条名称
		uint32_t	_lineType;		///< 线条类型
	} ChartLine;

	/**
	 * @brief 图表指标结构体
	 * @details 用于定义图表中的指标信息，包括名称、类型和线条等
	 */
	typedef struct _ChartIndex
	{
		std::string	_name;			///< 指标名称
		uint32_t	_indexType;		///< 指标类型
		std::unordered_map<std::string, ChartLine> _lines;		///< 指标线条映射，键为线条名称
		std::unordered_map<std::string, double> _base_lines;	///< 基准线映射，键为线条名称，值为基准线值
	} ChartIndex;

	std::unordered_map<std::string, ChartIndex>	_chart_indice;	///< 图表指标映射，键为指标名称

	/**
	 * @brief Tick缓存类型
	 * @details 定义了从合约代码到Tick数据的映射，用于缓存最新的Tick数据
	 */
	typedef wt_hashmap<std::string, WTSTickStruct>	TickCache;
	TickCache	_ticks;			///< Tick缓存，存储最新的Tick数据
};