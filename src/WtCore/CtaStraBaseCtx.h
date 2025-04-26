/*!
 * \file CtaStraBaseCtx.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief CTA策略基础上下文
 * 该文件定义了CTA策略的基础上下文类，提供策略执行所需的各种功能接口
 * 包括信号管理、仓位管理、数据访问、交易执行、日志记录等功能
 */
#pragma once
#include "../Includes/ICtaStraCtx.h"
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSDataDef.hpp"

#include "../Share/BoostFile.hpp"
#include "../Share/fmtlib.h"
#include "../Share/SpinMutex.hpp"

#include <unordered_map>

class CtaStrategy;

NS_WTP_BEGIN

class WtCtaEngine;

// 条件交易的动作类型定义
const char COND_ACTION_OL = 0;	//开多，Open Long，建立多头仓位
const char COND_ACTION_CL = 1;	//平多，Close Long，平掉多头仓位
const char COND_ACTION_OS = 2;	//开空，Open Short，建立空头仓位
const char COND_ACTION_CS = 3;	//平空，Close Short，平掉空头仓位
const char COND_ACTION_SP = 4;	//直接设置仓位，Set Position，直接调整至目标仓位

/**
 * 条件委托结构体
 * 用于定义基于特定条件的交易委托，当条件满足时触发相应的交易操作
 */
typedef struct _CondEntrust
{
	WTSCompareField _field;    // 比较字段，指定用于条件判断的数据字段
	WTSCompareType	_alg;      // 比较算法，如大于、小于、等于等
	double			_target;   // 目标值，与实际数据比较的阈值

	double			_qty;      // 委托数量

	char			_action;	//0-开多, 1-平多, 2-开空, 3-平空，交易动作类型

	char			_code[MAX_INSTRUMENT_LENGTH];  // 标的代码
	char			_usertag[32];                  // 用户标签，用于标识特定委托


	_CondEntrust()
	{
		memset(this, 0, sizeof(_CondEntrust));
	}

} CondEntrust;

// 条件委托列表，用于存储同一标的的多个条件委托
typedef std::vector<CondEntrust>	CondList;
// 条件委托映射，按标的代码索引条件委托列表
typedef wt_hashmap<std::string, CondList>	CondEntrustMap;


/**
 * CTA策略基础上下文类
 * 
 * 该类继承自ICtaStraCtx接口，实现了CTA策略所需的各种功能，包括：
 * - 信号记录与管理
 * - 交易执行与记录
 * - 仓位管理
 * - 数据访问
 * - 日志记录
 * - 用户数据存储
 * - 图表与指标管理
 */
class CtaStraBaseCtx : public ICtaStraCtx
{
public:
	/**
	 * 构造函数
	 * @param engine CTA引擎指针，用于和引擎进行交互
	 * @param name 策略名称，用于标识策略
	 * @param slippage 滑点设置，交易执行时的价格调整
	 */
	CtaStraBaseCtx(WtCtaEngine* engine, const char* name, int32_t slippage);

	/**
	 * 析构函数，清理资源
	 */
	virtual ~CtaStraBaseCtx();

private:
	/**
	 * 初始化输出文件
	 * 创建并初始化策略相关的输出文件，如信号日志、交易日志等
	 */
	void	init_outputs();

	/**
	 * 记录交易信号
	 * @param stdCode 标准合约代码
	 * @param target 目标仓位
	 * @param price 信号价格
	 * @param gentime 信号生成时间
	 * @param usertag 用户标签，用于标识特定信号
	 */
	inline void log_signal(const char* stdCode, double target, double price, uint64_t gentime, const char* usertag = "");

	/**
	 * 记录交易明细
	 * @param stdCode 标准合约代码
	 * @param isLong 是否为多头交易
	 * @param isOpen 是否为开仓交易
	 * @param curTime 当前时间
	 * @param price 交易价格
	 * @param qty 交易数量
	 * @param userTag 用户标签
	 * @param fee 交易手续费
	 * @param barNo K线编号
	 */
	inline void	log_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, const char* userTag = "", double fee = 0.0, uint32_t barNo = 0);

	/**
	 * 记录平仓信息
	 * @param stdCode 标准合约代码
	 * @param isLong 是否为多头
	 * @param openTime 开仓时间
	 * @param openpx 开仓价格
	 * @param closeTime 平仓时间
	 * @param closepx 平仓价格
	 * @param qty 交易数量
	 * @param profit 盈亏金额
	 * @param totalprofit 总盈亏
	 * @param enterTag 开仓标签
	 * @param exitTag 平仓标签
	 * @param openBarNo 开仓K线编号
	 * @param closeBarNo 平仓K线编号
	 */
	inline void	log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty,
		double profit, double totalprofit = 0, const char* enterTag = "", const char* exitTag = "", uint32_t openBarNo = 0, uint32_t closeBarNo = 0);

	/**
	 * 保存策略数据
	 * @param flag 数据保存标志，指定需要保存的数据类型
	 */
	void	save_data(uint32_t flag = 0xFFFFFFFF);

	/**
	 * 加载策略数据
	 * @param flag 数据加载标志，指定需要加载的数据类型
	 */
	void	load_data(uint32_t flag = 0xFFFFFFFF);

	/**
	 * 加载用户自定义数据
	 * 从存储文件中读取用户定义的数据
	 */
	void	load_userdata();

	/**
	 * 保存用户自定义数据
	 * 将用户定义的数据写入存储文件
	 */
	void	save_userdata();

	/**
	 * 更新动态盈亏
	 * @param stdCode 标准合约代码
	 * @param price 当前价格
	 */
	void	update_dyn_profit(const char* stdCode, double price);

	/**
	 * 设置标的仓位
	 * @param stdCode 标准合约代码
	 * @param qty 目标仓位数量
	 * @param userTag 用户标签
	 * @param bFireAtOnce 是否立即触发交易
	 */
	void	do_set_position(const char* stdCode, double qty, const char* userTag = "", bool bFireAtOnce = false);

	/**
	 * 添加交易信号
	 * @param stdCode 标准合约代码
	 * @param qty 目标仓位数量
	 * @param userTag 用户标签
	 * @param sigType 信号类型
	 */
	void	append_signal(const char* stdCode, double qty, const char* userTag = "", uint32_t sigType = 0);

	/**
	 * 获取标的的条件委托列表
	 * @param stdCode 标准合约代码
	 * @return 条件委托列表的引用
	 */
	inline CondList& get_cond_entrusts(const char* stdCode);

protected:
	/**
	 * 记录调试日志
	 * @param format 格式化字符串
	 * @param args 可变参数，用于格式化
	 */
	template<typename... Args>
	void log_debug(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_debug(buffer);
	}

	/**
	 * 记录信息日志
	 * @param format 格式化字符串
	 * @param args 可变参数，用于格式化
	 */
	template<typename... Args>
	void log_info(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_info(buffer);
	}

	/**
	 * 记录错误日志
	 * @param format 格式化字符串
	 * @param args 可变参数，用于格式化
	 */
	template<typename... Args>
	void log_error(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_error(buffer);
	}

	/**
	 * 导出图表信息
	 * 将策略的图表数据输出到文件中
	 */
	void	dump_chart_info();

public:
	/**
	 * 获取上下文ID
	 * @return 策略上下文唯一标识符
	 */
	virtual uint32_t id() { return _context_id; }

	//回调函数
	/**
	 * 策略初始化回调
	 * 在策略加载后首次运行时调用，用于初始化策略参数和状态
	 */
	virtual void on_init() override;

	/**
	 * 交易日开始回调
	 * @param uTDate 交易日期
	 */
	virtual void on_session_begin(uint32_t uTDate) override;

	/**
	 * 交易日结束回调
	 * @param uTDate 交易日期
	 */
	virtual void on_session_end(uint32_t uTDate) override;

	/**
	 * Tick数据回调
	 * @param stdCode 标准合约代码
	 * @param newTick 新的Tick数据
	 * @param bEmitStrategy 是否触发策略计算
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* newTick, bool bEmitStrategy = true) override;

	/**
	 * K线数据回调
	 * @param stdCode 标准合约代码
	 * @param period 周期类型
	 * @param times 周期倍数
	 * @param newBar 新的K线数据
	 */
	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;

	/**
	 * 定时任务回调
	 * @param curDate 当前日期
	 * @param curTime 当前时间
	 * @return 是否成功处理
	 */
	virtual bool on_schedule(uint32_t curDate, uint32_t curTime) override;

	/**
	 * 枚举所有持仓
	 * @param cb 回调函数，用于处理每个持仓
	 * @param bForExecute 是否为执行调用
	 */
	virtual void enum_position(FuncEnumCtaPosCallBack cb, bool bForExecute = false) override;


	//////////////////////////////////////////////////////////////////////////
	//策略接口
	/**
	 * 开多仓操作
	 * @param stdCode 标准合约代码
	 * @param qty 交易数量
	 * @param userTag 用户自定义标签，用于标识当前交易
	 * @param limitprice 限价，如果为0表示不限价
	 * @param stopprice 止损价，如果为0表示不设置止损
	 */
	virtual void stra_enter_long(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override;

	/**
	 * 开空仓操作
	 * @param stdCode 标准合约代码
	 * @param qty 交易数量
	 * @param userTag 用户自定义标签
	 * @param limitprice 限价
	 * @param stopprice 止损价
	 */
	virtual void stra_enter_short(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override;

	/**
	 * 平多仓操作
	 * @param stdCode 标准合约代码
	 * @param qty 平仓数量
	 * @param userTag 用户自定义标签
	 * @param limitprice 限价
	 * @param stopprice 止损价
	 */
	virtual void stra_exit_long(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override;

	/**
	 * 平空仓操作
	 * @param stdCode 标准合约代码
	 * @param qty 平仓数量
	 * @param userTag 用户自定义标签
	 * @param limitprice 限价
	 * @param stopprice 止损价
	 */
	virtual void stra_exit_short(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override;

	/**
	 * 获取当前持仓
	 * @param stdCode 标准合约代码
	 * @param bOnlyValid 是否只返回有效仓位（已成交的）
	 * @param userTag 用户标签，如果指定则只返回对应标签的持仓
	 * @return 持仓数量，正数表示多头，负数表示空头
	 */
	virtual double stra_get_position(const char* stdCode, bool bOnlyValid = false, const char* userTag = "") override;

	/**
	 * 设置目标仓位
	 * @param stdCode 标准合约代码
	 * @param qty 目标仓位数量，正数表示多头，负数表示空头
	 * @param userTag 用户标签
	 * @param limitprice 限价
	 * @param stopprice 止损价
	 */
	virtual void stra_set_position(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override;

	/**
	 * 获取当前最新价格
	 * @param stdCode 标准合约代码
	 * @return 最新价格
	 */
	virtual double stra_get_price(const char* stdCode) override;

	/**
	 * 读取当日价格
	 * @param stdCode 标准合约代码
	 * @param flag 价格标记（0-开盘价，1-最高价，2-最低价，3-收盘价）
	 * @return 当日价格
	 */
	virtual double stra_get_day_price(const char* stdCode, int flag = 0) override;

	/**
	 * 获取交易日期
	 * @return 交易日期，格式YYYYMMDD
	 */
	virtual uint32_t stra_get_tdate() override;

	/**
	 * 获取实际日期
	 * @return 实际日期，格式YYYYMMDD
	 */
	virtual uint32_t stra_get_date() override;

	/**
	 * 获取当前时间
	 * @return 当前时间，格式HHMMSS或HHMMSS*1000+毫秒
	 */
	virtual uint32_t stra_get_time() override;

	/**
	 * 获取资金数据
	 * @param flag 数据标记（0-动态权益, 1-总盈亏, 2-总手续费）
	 * @return 资金数据值
	 */
	virtual double stra_get_fund_data(int flag /* = 0 */) override;

	/**
	 * 获取标的第一次开仓时间
	 * @param stdCode 标准合约代码
	 * @return 第一次开仓时间
	 */
	virtual uint64_t stra_get_first_entertime(const char* stdCode) override;

	/**
	 * 获取标的最后一次开仓时间
	 * @param stdCode 标准合约代码
	 * @return 最后一次开仓时间
	 */
	virtual uint64_t stra_get_last_entertime(const char* stdCode) override;

	/**
	 * 获取标的最后一次平仓时间
	 * @param stdCode 标准合约代码
	 * @return 最后一次平仓时间
	 */
	virtual uint64_t stra_get_last_exittime(const char* stdCode) override;

	/**
	 * 获取标的最后一次开仓价格
	 * @param stdCode 标准合约代码
	 * @return 最后一次开仓价格
	 */
	virtual double stra_get_last_enterprice(const char* stdCode) override;

	/**
	 * 获取标的持仓均价
	 * @param stdCode 标准合约代码
	 * @return 持仓均价
	 */
	virtual double stra_get_position_avgpx(const char* stdCode) override;

	/**
	 * 获取标的持仓盈亏
	 * @param stdCode 标准合约代码
	 * @return 持仓盈亏
	 */
	virtual double stra_get_position_profit(const char* stdCode) override;

	/**
	 * 获取指定标签的开仓时间
	 * @param stdCode 标准合约代码
	 * @param userTag 用户标签
	 * @return 开仓时间
	 */
	virtual uint64_t stra_get_detail_entertime(const char* stdCode, const char* userTag) override;

	/**
	 * 获取指定标签的开仓成本
	 * @param stdCode 标准合约代码
	 * @param userTag 用户标签
	 * @return 开仓成本
	 */
	virtual double stra_get_detail_cost(const char* stdCode, const char* userTag) override;

	/**
	 * 获取指定标签的持仓盈亏
	 * @param stdCode 标准合约代码
	 * @param userTag 用户标签
	 * @param flag 数据标记（0-浮动盈亏，1-平仓盈亏）
	 * @return 持仓盈亏
	 */
	virtual double stra_get_detail_profit(const char* stdCode, const char* userTag, int flag = 0) override;

	/**
	 * 获取合约品种信息
	 * @param stdCode 标准合约代码
	 * @return 合约品种信息指针
	 */
	virtual WTSCommodityInfo* stra_get_comminfo(const char* stdCode) override;

	/**
	 * 获取K线数据切片
	 * @param stdCode 标准合约代码
	 * @param period 周期类型，如“day”, “min1”等
	 * @param count 数据条数
	 * @param isMain 是否为主力合约
	 * @return K线数据切片指针
	 */
	virtual WTSKlineSlice*	stra_get_bars(const char* stdCode, const char* period, uint32_t count, bool isMain = false) override;

	/**
	 * 获取Tick数据切片
	 * @param stdCode 标准合约代码
	 * @param count 数据条数
	 * @return Tick数据切片指针
	 */
	virtual WTSTickSlice*	stra_get_ticks(const char* stdCode, uint32_t count) override;

	/**
	 * 获取最新的Tick数据
	 * @param stdCode 标准合约代码
	 * @return 最新Tick数据指针
	 */
	virtual WTSTickData*	stra_get_last_tick(const char* stdCode) override;

	/**
	 * 获取分月合约代码
	 * 将标准化的合约代码转换为原始的分月合约代码
	 * @param stdCode 标准化的合约代码
	 * @return 原始的分月合约代码
	 */
	virtual std::string		stra_get_rawcode(const char* stdCode) override;

	/**
	 * 订阅Tick数据
	 * @param stdCode 标准合约代码
	 */
	virtual void stra_sub_ticks(const char* stdCode) override;

	/**
	 * 订阅K线事件
	 * @param stdCode 标准合约代码
	 * @param period 周期类型
	 */
	virtual void stra_sub_bar_events(const char* stdCode, const char* period) override;

	/**
	 * 记录信息日志
	 * @param message 日志内容
	 */
	virtual void stra_log_info(const char* message) override;

	/**
	 * 记录调试日志
	 * @param message 日志内容
	 */
	virtual void stra_log_debug(const char* message) override;

	/**
	 * 记录警告日志
	 * @param message 日志内容
	 */
	virtual void stra_log_warn(const char* message) override;

	/**
	 * 记录错误日志
	 * @param message 日志内容
	 */
	virtual void stra_log_error(const char* message) override;

	/**
	 * 保存用户自定义数据
	 * @param key 数据键名
	 * @param val 数据值
	 */
	virtual void stra_save_user_data(const char* key, const char* val) override;

	/**
	 * 加载用户自定义数据
	 * @param key 数据键名
	 * @param defVal 默认值，当数据不存在时返回
	 * @return 数据值或默认值
	 */
	virtual const char* stra_load_user_data(const char* key, const char* defVal = "") override;

	/**
	 * 获取最近一次开仓的用户标签
	 * @param stdCode 标准合约代码
	 * @return 最近一次开仓的用户标签
	 */
	virtual const char* stra_get_last_entertag(const char* stdCode) override;

public:
	/**
	 * 设置图表K线
	 * 设置当前策略图表中显示的K线
	 * @param stdCode 标准合约代码
	 * @param period 周期类型，如“day”、“min1”等
	 */
	virtual void set_chart_kline(const char* stdCode, const char* period) override;

	/**
	 * 添加图表标记
	 * 在图表上添加交易信号标记
	 * @param price 标记价格位置
	 * @param icon 图标类型，如“buy”、“sell”等
	 * @param tag 标记标签文字
	 */
	virtual void add_chart_mark(double price, const char* icon, const char* tag) override;

	/**
	 * 注册指标
	 * 为策略添加一个新的技术指标
	 * @param idxName 指标名称
	 * @param indexType 指标类型
	 */
	virtual void register_index(const char* idxName, uint32_t indexType) override;

	/**
	 * 注册指标线
	 * 为某个指标添加一条数据线
	 * @param idxName 指标名称
	 * @param lineName 线条名称
	 * @param lineType 线条类型
	 * @return 是否成功
	 */
	virtual bool register_index_line(const char* idxName, const char* lineName, uint32_t lineType) override;

	/**
	 * 添加基准线
	 * 为指标添加一条固定值的基准线
	 * @param idxName 指标名称
	 * @param lineName 线条名称
	 * @param val 基准线数值
	 * @return 是否成功
	 */
	virtual bool add_index_baseline(const char* idxName, const char* lineName, double val) override;

	/**
	 * 设置指标值
	 * 更新某个指标线的数值
	 * @param idxName 指标名称
	 * @param lineName 线条名称
	 * @param val 指标值
	 * @return 是否成功
	 */
	virtual bool set_index_value(const char* idxName, const char* lineName, double val) override;

protected:
	/**
	 * 策略上下文唯一标识符
	 * 每个策略实例在创建时生成的唯一ID
	 */
	uint32_t		_context_id;

	/**
	 * CTA引擎指针
	 * 指向策略所属的CTA引擎实例，用于策略和引擎之间的交互
	 */
	WtCtaEngine*	_engine;

	/**
	 * 滑点设置
	 * 交易执行时的价格调整，模拟实际交易中的滑点因素
	 */
	int32_t			_slippage;

	/**
	 * 总计算时间
	 * 策略计算耗费的总时间，用于统计策略性能
	 */
	uint64_t		_total_calc_time;

	/**
	 * 总计算次数
	 * 策略计算函数被调用的总次数，用于统计策略性能
	 */
	uint32_t		_emit_times;

	/**
	 * 主图表标签键
	 * 用于标识策略主图表的唯一键
	 */
	std::string		_main_key;

	/**
	 * 主图表合约代码
	 * 策略主图表中显示的合约代码
	 */
	std::string		_main_code;

	/**
	 * 主图表周期
	 * 策略主图表中使用的K线周期
	 */
	std::string		_main_period;

	/**
	 * K线标签结构体
	 * 用于记录K线的状态信息
	 */
	typedef struct _KlineTag
	{
		/**
		 * K线是否已完成闭合
		 * true表示K线周期已结束并生成完整K线
		 */
		bool	_closed;

		/**
		 * 是否需要通知策略
		 * true表示需要触发策略的on_bar回调
		 */
		bool	_notify;

		_KlineTag() :_closed(false), _notify(false){}

	} KlineTag;

	/**
	 * K线标签映射类型
	 * 使用哈希表存储不同合约和周期的K线状态信息
	 * 键为合约代码和周期的组合，值为K线标签信息
	 */
	typedef wt_hashmap<std::string, KlineTag> KlineTags;

	/**
	 * K线标签映射实例
	 * 记录策略关注的所有K线周期的状态信息
	 */
	KlineTags	_kline_tags;

	/**
	 * 价格映射类型
	 * 使用哈希表存储不同合约的最新价格
	 * 键为合约代码，值为对应的最新价格
	 */
	typedef wt_hashmap<std::string, double> PriceMap;

	/**
	 * 价格映射实例
	 * 用于快速获取策略交易标的的最新价格
	 */
	PriceMap		_price_map;

	/**
	 * 详细交易信息结构体
	 * 用于记录单笔交易的详细信息，包括开仓价格、数量、时间及损盈等
	 */
	typedef struct _DetailInfo
	{
		/**
		 * 方向标记
		 * true表示多头，false表示空头
		 */
		bool		_long;

		/**
		 * 开仓价格
		 * 建立仓位时的交易价格
		 */
		double		_price;

		/**
		 * 仓位数量
		 * 当前持有的合约数量
		 */
		double		_volume;

		/**
		 * 开仓时间
		 * 建立仓位的时间戳
		 */
		uint64_t	_opentime;

		/**
		 * 开仓交易日
		 * 建立仓位的交易日期，格式YYYYMMDD
		 */
		uint32_t	_opentdate;

		/**
		 * 最大盈利
		 * 单笔交易达到的最大盈利额
		 */
		double		_max_profit;

		/**
		 * 最大亏损
		 * 单笔交易经历的最大亏损额
		 */
		double		_max_loss;

		/**
		 * 最高价格
		 * 单笔交易期间遇到的最高价格
		 */
		double		_max_price;

		/**
		 * 最低价格
		 * 单笔交易期间遇到的最低价格
		 */
		double		_min_price;

		/**
		 * 当前盈亏
		 * 单笔交易当前的盈亏金额
		 */
		double		_profit;

		/**
		 * 开仓标签
		 * 开仓时指定的用户标签，用于标识交易来源
		 */
		char		_opentag[32];

		/**
		 * 开仓K线编号
		 * 开仓时对应的K线编号，用于回测时的成交分析
		 */
		uint32_t	_open_barno;

		_DetailInfo()
		{
			memset(this, 0, sizeof(_DetailInfo));
		}
	} DetailInfo;

	/**
	 * 仓位信息结构体
	 * 用于记录策略对某个合约的整体持仓信息
	 */
	typedef struct _PosInfo
	{
		/**
		 * 持仓数量
		 * 当前的持仓量，正数表示多头，负数表示空头
		 */
		double		_volume;

		/**
		 * 平仓盈亏
		 * 已平仓仓位的累计盈亏
		 */
		double		_closeprofit;

		/**
		 * 浮动盈亏
		 * 当前持仓的浮动盈亏
		 */
		double		_dynprofit;

		/**
		 * 最后开仓时间
		 * 最近一次开仓的时间戳
		 */
		uint64_t	_last_entertime;

		/**
		 * 最后平仓时间
		 * 最近一次平仓的时间戳
		 */
		uint64_t	_last_exittime;

		/**
		 * 冻结仓位
		 * 已下单尚未成交的单子数量
		 */
		double		_frozen;

		/**
		 * 冻结仓位日期
		 * 下单时的交易日期，格式YYYYMMDD
		 */
		uint32_t	_frozen_date;

		/**
		 * 详细交易列表
		 * 存储该合约所有单笔交易明细
		 */
		std::vector<DetailInfo> _details;

		/**
		 * 构造函数
		 * 初始化所有成员变量
		 */
		_PosInfo()
		{
			_volume = 0;
			_closeprofit = 0;
			_dynprofit = 0;
			_last_entertime = 0;
			_last_exittime = 0;
			_frozen = 0;
			_frozen_date = 0;
		}
	} PosInfo;

	/**
	 * 持仓映射类型
	 * 使用哈希表存储各个合约的持仓信息
	 * 键为合约代码，值为该合约的持仓信息
	 */
	typedef wt_hashmap<std::string, PosInfo> PositionMap;

	/**
	 * 持仓映射实例
	 * 用于记录策略的所有持仓情况
	 */
	PositionMap		_pos_map;

	/**
	 * 信号信息结构体
	 * 用于记录策略生成的交易信号信息
	 */
	typedef struct _SigInfo
	{
		/**
		 * 信号仓位数量
		 * 目标仓位量，正数表示多头，负数表示空头
		 */
		double		_volume;

		/**
		 * 用户标签
		 * 信号的自定义标签，用于识别信号来源
		 */
		std::string	_usertag;

		/**
		 * 信号价格
		 * 信号生成时的参考价格
		 */
		double		_sigprice;

		/**
		 * 信号类型
		 * 0-onschedule信号（定时任务中生成）
		 * 1-ontick信号（Tick数据触发的）
		 * 2-条件单信号（条件触发的）
		 */
		uint32_t	_sigtype;

		/**
		 * 信号生成时间
		 * 信号生成的时间戳
		 */
		uint64_t	_gentime;

		/**
		 * 是否已触发
		 * 该信号是否已被触发执行
		 */
		bool		_triggered;

		/**
		 * 构造函数
		 * 初始化信号信息各字段
		 */
		_SigInfo()
		{
			_volume = 0;
			_sigprice = 0;
			_sigtype = 0;
			_gentime = 0;
			_triggered = false;
		}
	}SigInfo;

	/**
	 * 信号映射类型
	 * 使用哈希表存储标的的交易信号
	 * 键为合约代码，值为信号信息
	 */
	typedef wt_hashmap<std::string, SigInfo>	SignalMap;

	/**
	 * 信号映射实例
	 * 用于策略对交易信号的管理
	 */
	SignalMap		_sig_map;

	/**
	 * 交易日志文件指针
	 * 记录所有交易明细
	 */
	BoostFilePtr	_trade_logs;

	/**
	 * 平仓日志文件指针
	 * 记录所有平仓记录及收益
	 */
	BoostFilePtr	_close_logs;

	/**
	 * 资金日志文件指针
	 * 记录资金变化情况
	 */
	BoostFilePtr	_fund_logs;

	/**
	 * 信号日志文件指针
	 * 记录策略生成的交易信号
	 */
	BoostFilePtr	_sig_logs;

	/**
	 * 持仓日志文件指针
	 * 记录持仓变动情况
	 */
	BoostFilePtr	_pos_logs;

	/**
	 * 指标日志文件指针
	 * 记录技术指标变化
	 */
	BoostFilePtr	_idx_logs;

	/**
	 * 标记日志文件指针
	 * 记录图表标记信息
	 */
	BoostFilePtr	_mark_logs;

	/**
	 * 条件委托映射
	 * 存储策略设置的所有条件委托
	 */
	CondEntrustMap	_condtions;

	/**
	 * 上次设置条件单的时间
	 * 记录上一次设置条件单的分钟时间戳
	 */
	uint64_t		_last_cond_min;

	/**
	 * 上次设置的K线编号
	 * 用于回测时记录上次处理的K线编号
	 */
	uint32_t		_last_barno;

	//是否处于调度中的标记
	/**
	 * 是否在自动调度中
	 * 用于标记策略当前是否处于调度执行中
	 */
	bool			_is_in_schedule;

	//用户数据
	/**
	 * 字符串映射类型
	 * 用于存储用户自定义数据的键值对
	 */
	typedef wt_hashmap<std::string, std::string> StringHashMap;

	/**
	 * 用户数据映射
	 * 存储策略的自定义数据，键值对格式
	 */
	StringHashMap	_user_datas;

	/**
	 * 用户数据是否被修改
	 * 标记用户数据是否需要保存
	 */
	bool			_ud_modified;

	/**
	 * 策略资金信息结构体
	 * 用于记录策略资金及收益相关信息
	 */
	typedef struct _StraFundInfo
	{
		/**
		 * 总平仓盈亏
		 * 已平仓的全部交易的累计盈亏
		 */
		double	_total_profit;

		/**
		 * 总浮动盈亏
		 * 当前所有持仓的浮动盈亏总和
		 */
		double	_total_dynprofit;

		/**
		 * 总手续费
		 * 所有交易产生的手续费总和
		 */
		double	_total_fees;

		/**
		 * 构造函数
		 * 初始化资金信息结构体
		 */
		_StraFundInfo()
		{
			memset(this, 0, sizeof(_StraFundInfo));
		}
	} StraFundInfo;

	/**
	 * 策略资金信息实例
	 * 记录策略的资金和收益数据
	 */
	StraFundInfo		_fund_info;

	//tick订阅列表
	/**
	 * Tick数据订阅集合
	 * 存储策略订阅的所有Tick数据的合约代码
	 */
	wt_hashset<std::string> _tick_subs;

	/**
	 * K线事件订阅集合
	 * 存储策略订阅的所有K线事件的合约代码和周期引用
	 */
	wt_hashset<std::string> _barevt_subs;

	//////////////////////////////////////////////////////////////////////////
	//图表相关
	/**
	 * 图表合约代码
	 * 当前图表显示的合约代码
	 */
	std::string		_chart_code;

	/**
	 * 图表周期
	 * 当前图表显示的K线周期
	 */
	std::string		_chart_period;

	/**
	 * 图表线条结构体
	 * 用于定义图表中的指标线条
	 */
	typedef struct _ChartLine
	{
		/**
		 * 线条名称
		 * 用于标识线条
		 */
		std::string	_name;

		/**
		 * 线条类型
		 * 定义线条的呈现方式
		 */
		uint32_t	_lineType;
	} ChartLine;

	/**
	 * 图表指标结构体
	 * 用于定义图表中的技术指标
	 */
	typedef struct _ChartIndex
	{
		/**
		 * 指标名称
		 * 用于标识指标，如MACD、KDJ等
		 */
		std::string	_name;

		/**
		 * 指标类型
		 * 定义指标的类型和显示方式
		 */
		uint32_t	_indexType;

		/**
		 * 指标线映射
		 * 存储指标包含的各个线条，键为线条名称
		 */
		wt_hashmap<std::string, ChartLine> _lines;

		/**
		 * 基准线映射
		 * 存储指标的基准线数值，键为基准线名称
		 */
		wt_hashmap<std::string, double> _base_lines;
	} ChartIndex;

	/**
	 * 图表指标映射
	 * 存储所有技术指标，键为指标名称
	 */
	wt_hashmap<std::string, ChartIndex>	_chart_indice;

private:
	/**
	 * 同步锁
	 * 用于策略内部的线程同步
	 */
	SpinMutex		_mutex;
};


NS_WTP_END