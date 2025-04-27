/*!
* \file SelStraBaseCtx.h
* \project	WonderTrader
*
* \author Wesley
* \date 2020/03/30
*
* \brief 选股策略基础上下文定义
* 本文件定义了选股策略(Selection Strategy)的基础上下文类 SelStraBaseCtx
* 该类实现了 ISelStraCtx 接口，提供了选股策略所需的各种基础功能
* 包括持仓管理、数据访问、信号管理、日志记录等功能
*/
#pragma once
#include "../Includes/FasterDefs.h"
#include "../Includes/ISelStraCtx.h"
#include "../Includes/WTSDataDef.hpp"

#include "../Share/BoostFile.hpp"
#include "../Share/fmtlib.h"

NS_WTP_BEGIN

class WtSelEngine;


/**
 * @brief 选股策略基础上下文类
 * @details 该类继承自 ISelStraCtx 接口，为选股策略提供完整的上下文环境
 *          支持监控行情数据、管理持仓、生成交易信号、记录日志等功能
 *          充当策略与引擎之间的桌缆，封装底层复杂逻辑，提供简洁API
 */
class SelStraBaseCtx : public ISelStraCtx
{
public:
	/**
	 * @brief 构造函数
	 * @param engine 选股引擎指针，用于与引擎实例交互
	 * @param name 策略名称，用于标识不同的选股策略
	 * @param slippage 滑点数，用于模拟交易时的滑点损失
	 * @details 实例化选股策略上下文，初始化相关资源和配置
	 */
	SelStraBaseCtx(WtSelEngine* engine, const char* name, int32_t slippage);

	/**
	 * @brief 析构函数
	 * @details 清理选股策略上下文的资源，处理内存释放等工作
	 */
	virtual ~SelStraBaseCtx();

private:
	/**
	 * @brief 初始化输出文件
	 * @details 初始化策略的日志输出和信号记录文件
	 */
	void	init_outputs();

	/**
	 * @brief 记录信号日志
	 * @param stdCode 标准化合约代码
	 * @param target 目标仓位
	 * @param price 信号价格
	 * @param gentime 信号生成时间
	 * @param usertag 用户标签，用于标识不同的信号
	 * @details 将选股策略生成的信号记录到日志文件中
	 */
	inline void log_signal(const char* stdCode, double target, double price, uint64_t gentime, const char* usertag = "");

	/**
	 * @brief 记录交易日志
	 * @param stdCode 标准化合约代码
	 * @param isLong 是否是多头方向
	 * @param isOpen 是否是开仓操作
	 * @param curTime 当前时间
	 * @param price 交易价格
	 * @param qty 交易数量
	 * @param userTag 用户标签
	 * @param fee 交易手续费
	 * @details 记录模拟交易的详细信息到日志文件
	 */
	inline void	log_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, const char* userTag = "", double fee = 0.0);

	/**
	 * @brief 记录平仓日志
	 * @param stdCode 标准化合约代码
	 * @param isLong 是否是多头方向
	 * @param openTime 开仓时间
	 * @param openpx 开仓价格
	 * @param closeTime 平仓时间
	 * @param closepx 平仓价格
	 * @param qty 交易数量
	 * @param profit 交易盈亏
	 * @param totalprofit 累计盈亏
	 * @param enterTag 开仓标签
	 * @param exitTag 平仓标签
	 * @details 记录平仓操作的详细信息到日志文件
	 */
	inline void	log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty,
		double profit, double totalprofit = 0, const char* enterTag = "", const char* exitTag = "");

	/**
	 * @brief 保存数据
	 * @param flag 数据保存标志，用于控制保存哪些类型的数据
	 * @details 将策略相关数据保存到磁盘，包括信号、持仓、资金等信息
	 */
	void	save_data(uint32_t flag = 0xFFFFFFFF);

	/**
	 * @brief 加载数据
	 * @param flag 数据加载标志，用于控制加载哪些类型的数据
	 * @details 从磁盘中加载策略必要的数据，包括信号、持仓、资金等信息
	 */
	void	load_data(uint32_t flag = 0xFFFFFFFF);

	/**
	 * @brief 加载用户数据
	 * @details 从磁盘中加载用户自定义数据
	 */
	void	load_userdata();

	/**
	 * @brief 保存用户数据
	 * @details 将用户自定义数据保存到磁盘
	 */
	void	save_userdata();

	/**
	 * @brief 更新动态盈亏
	 * @param stdCode 标准化合约代码
	 * @param price 当前价格
	 * @details 根据当前市场价格更新持仓的动态盈亏
	 */
	void	update_dyn_profit(const char* stdCode, double price);

	/**
	 * @brief 设置持仓量
	 * @param stdCode 标准化合约代码
	 * @param qty 目标仓位量
	 * @param userTag 用户标签
	 * @param bTriggered 是否由信号触发
	 * @details 执行持仓调整操作，生成相应的交易信号
	 */
	void	do_set_position(const char* stdCode, double qty, const char* userTag = "", bool bTriggered = false);

	/**
	 * @brief 添加信号
	 * @param stdCode 标准化合约代码
	 * @param qty 目标仓位量
	 * @param userTag 用户标签
	 * @details 将选股策略生成的信号添加到信号列表中
	 */
	void	append_signal(const char* stdCode, double qty, const char* userTag = "");

protected:
	/**
	 * @brief 输出调试日志
	 * @tparam Args 参数包类型
	 * @param format 格式化字符串，类似printf的格式
	 * @param args 可变参数列表
	 * @details 将调试级别的日志输出到策略日志文件中
	 */
	template<typename... Args>
	void log_debug(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_debug(buffer);
	}

	/**
	 * @brief 输出信息日志
	 * @tparam Args 参数包类型
	 * @param format 格式化字符串，类似printf的格式
	 * @param args 可变参数列表
	 * @details 将信息级别的日志输出到策略日志文件中
	 */
	template<typename... Args>
	void log_info(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_info(buffer);
	}

	/**
	 * @brief 输出错误日志
	 * @tparam Args 参数包类型
	 * @param format 格式化字符串，类似printf的格式
	 * @param args 可变参数列表
	 * @details 将错误级别的日志输出到策略日志文件中
	 */
	template<typename... Args>
	void log_error(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_error(buffer);
	}

public:
	/**
	 * @brief 获取策略上下文ID
	 * @return 策略上下文的唯一标识符
	 * @details 返回当前策略上下文的唯一ID，用于标识不同的策略实例
	 */
	virtual uint32_t id() { return _context_id; }

	//回调函数
	/**
	 * @brief 策略初始化回调
	 * @details 在策略初始化时被调用，用于初始化策略环境及加载必要数据
	 */
	virtual void on_init() override;

	/**
	 * @brief 交易会话开始回调
	 * @param uTDate 交易日期，格式YYYYMMDD
	 * @details 在每个交易日开始时被调用，用于处理会话开始时的初始化工作
	 */
	virtual void on_session_begin(uint32_t uTDate) override;

	/**
	 * @brief 交易会话结束回调
	 * @param uTDate 交易日期，格式YYYYMMDD
	 * @details 在每个交易日结束时被调用，用于处理会话结束时的清理工作
	 */
	virtual void on_session_end(uint32_t uTDate) override;

	/**
	 * @brief 逃价数据回调
	 * @param stdCode 标准化合约代码
	 * @param newTick 最新的Tick数据
	 * @param bEmitStrategy 是否触发策略计算
	 * @details 当有新的Tick数据到来时被调用，用于响应市场实时行情变化
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* newTick, bool bEmitStrategy = true) override;

	/**
	 * @brief K线数据回调
	 * @param stdCode 标准化合约代码
	 * @param period K线周期，如分钟线"m1"、小时线"h1"、日线"d1"等
	 * @param times 周期倍数，如分钟线的15分钟则是15
	 * @param newBar 最新的K线数据
	 * @details 当有新的K线数据生成时被调用，用于响应K线周期性的行情变化
	 */
	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;

	/**
	 * @brief 定时调度回调
	 * @param curDate 当前日期，格式YYYYMMDD
	 * @param curTime 当前时间，格式HHMMSS
	 * @param fireTime 触发时间，格式HHMM
	 * @return 是否成功处理调度任务
	 * @details 当达到计划的调度时间时被调用，用于执行定时任务，如定时选股
	 */
	virtual bool on_schedule(uint32_t curDate, uint32_t curTime, uint32_t fireTime) override;

	/**
	 * @brief 枚举持仓
	 * @param cb 持仓枚举回调函数
	 * @details 遍历当前策略的所有持仓信息，并传递给回调函数处理
	 */
	virtual void enum_position(FuncEnumSelPositionCallBack cb) override;

	//////////////////////////////////////////////////////////////////////////
	//策略接口
	/**
	 * @brief 获取持仓量
	 * @param stdCode 标准化合约代码
	 * @param bOnlyValid 是否只记录有效持仓，即非冻结部分
	 * @param userTag 用户自定义标签，用于标识指定的持仓
	 * @return 指定合约的当前持仓量
	 * @details 获取策略中指定代码的当前持仓量
	 */
	virtual double stra_get_position(const char* stdCode, bool bOnlyValid = false, const char* userTag = "") override;

	/**
	 * @brief 设置持仓量
	 * @param stdCode 标准化合约代码
	 * @param qty 目标持仓量
	 * @param userTag 用户自定义标签
	 * @details 设置策略对指定合约的目标持仓量，系统会自动生成交易信号
	 */
	virtual void stra_set_position(const char* stdCode, double qty, const char* userTag = "") override;

	/**
	 * @brief 获取当前市场价格
	 * @param stdCode 标准化合约代码
	 * @return 当前市场最新价格
	 * @details 获取指定合约的当前市场最新价格
	 */
	virtual double stra_get_price(const char* stdCode) override;

	/**
	 * @brief 获取当日特定价格
	 * @param stdCode 标准化合约代码
	 * @param flag 价格标志（0-开盘价，1-最高价，2-最低价，3-收盘价）
	 * @return 指定类型的价格
	 * @details 获取当日开盘价、最高价、最低价或收盘价
	 */
	virtual double stra_get_day_price(const char* stdCode, int flag = 0) override;

	/**
	 * @brief 获取当前交易日期
	 * @return 当前交易日YYYYMMDD格式
	 * @details 获取系统当前的交易日期
	 */
	virtual uint32_t stra_get_tdate() override;

	/**
	 * @brief 获取当前日历日期
	 * @return 当前日历日期，格式YYYYMMDD
	 * @details 获取系统当前的日历日期
	 */
	virtual uint32_t stra_get_date() override;

	/**
	 * @brief 获取当前时间
	 * @return 当前时间，格式HHMMSS
	 * @details 获取系统当前的时间
	 */
	virtual uint32_t stra_get_time() override;

	/**
	 * @brief 获取资金数据
	 * @param flag 数据标志（0-动态权益，1-总收益，2-手续费）
	 * @return 符合条件的资金数据
	 * @details 获取策略账户的各类资金数据
	 */
	virtual double stra_get_fund_data(int flag /* = 0 */) override;

	/**
	 * @brief 获取首次开仓时间
	 * @param stdCode 标准化合约代码
	 * @return 首次开仓时间，时间戳格式
	 * @details 获取策略中指定合约的首次开仓时间
	 */
	virtual uint64_t stra_get_first_entertime(const char* stdCode) override;

	/**
	 * @brief 获取最后开仓时间
	 * @param stdCode 标准化合约代码
	 * @return 最后开仓时间，时间戳格式
	 * @details 获取策略中指定合约的最近一次开仓时间
	 */
	virtual uint64_t stra_get_last_entertime(const char* stdCode) override;

	/**
	 * @brief 获取最后开仓价格
	 * @param stdCode 标准化合约代码
	 * @return 最后开仓价格
	 * @details 获取策略中指定合约的最近一次开仓价格
	 */
	virtual double stra_get_last_enterprice(const char* stdCode) override;

	/**
	 * @brief 获取最后开仓标签
	 * @param stdCode 标准化合约代码
	 * @return 最后开仓标签
	 * @details 获取策略中指定合约的最近一次开仓的用户标签
	 */
	virtual const char* stra_get_last_entertag(const char* stdCode) override;

	/**
	 * @brief 获取最后平仓时间
	 * @param stdCode 标准化合约代码
	 * @return 最后平仓时间，时间戳格式
	 * @details 获取策略中指定合约的最近一次平仓时间
	 */
	virtual uint64_t stra_get_last_exittime(const char* stdCode) override;

	/**
	 * @brief 获取持仓均价
	 * @param stdCode 标准化合约代码
	 * @return 持仓均价
	 * @details 获取策略中指定合约的当前持仓均价
	 */
	virtual double stra_get_position_avgpx(const char* stdCode) override;

	/**
	 * @brief 获取持仓收益
	 * @param stdCode 标准化合约代码
	 * @return 持仓收益
	 * @details 获取策略中指定合约的当前持仓收益
	 */
	virtual double stra_get_position_profit(const char* stdCode) override;

	/**
	 * @brief 获取详细开仓时间
	 * @param stdCode 标准化合约代码
	 * @param userTag 用户标签
	 * @return 指定标签持仓的开仓时间
	 * @details 获取指定合约和标签的开仓时间
	 */
	virtual uint64_t stra_get_detail_entertime(const char* stdCode, const char* userTag) override;

	/**
	 * @brief 获取详细成本
	 * @param stdCode 标准化合约代码
	 * @param userTag 用户标签
	 * @return 指定标签持仓的成本
	 * @details 获取指定合约和标签的开仓成本
	 */
	virtual double stra_get_detail_cost(const char* stdCode, const char* userTag) override;

	/**
	 * @brief 获取详细收益
	 * @param stdCode 标准化合约代码
	 * @param userTag 用户标签
	 * @param flag 收益标志（0-当前收益，1-最大收益，2-最大回撤）
	 * @return 指定标签持仓的收益
	 * @details 获取指定合约和标签的收益信息
	 */
	virtual double stra_get_detail_profit(const char* stdCode, const char* userTag, int flag = 0) override;

	/**
	 * @brief 获取商品信息
	 * @param stdCode 标准化合约代码
	 * @return 商品信息对象指针
	 * @details 获取指定合约的商品信息，包含交易所、品种等信息
	 */
	virtual WTSCommodityInfo* stra_get_comminfo(const char* stdCode) override;

	/**
	 * @brief 获取交易时段信息
	 * @param stdCode 标准化合约代码
	 * @return 交易时段信息对象指针
	 * @details 获取指定合约的交易时段信息，包含交易时间、休市时间等
	 */
	virtual WTSSessionInfo* stra_get_sessinfo(const char* stdCode) override;

	/**
	 * @brief 获取K线数据
	 * @param stdCode 标准化合约代码
	 * @param period K线周期标识，如"m1"(分钟)、"d1"(日线)等
	 * @param count 请求的K线条数
	 * @return K线切片数据
	 * @details 获取指定合约的历史K线数据
	 */
	virtual WTSKlineSlice*	stra_get_bars(const char* stdCode, const char* period, uint32_t count) override;

	/**
	 * @brief 获取Tick数据
	 * @param stdCode 标准化合约代码
	 * @param count 请求的Tick条数
	 * @return Tick切片数据
	 * @details 获取指定合约的历史Tick数据
	 */
	virtual WTSTickSlice*	stra_get_ticks(const char* stdCode, uint32_t count) override;

	/**
	 * @brief 获取最新Tick数据
	 * @param stdCode 标准化合约代码
	 * @return 最新的Tick数据指针
	 * @details 获取指定合约的最新市场行情数据
	 */
	virtual WTSTickData*	stra_get_last_tick(const char* stdCode) override;

	/**
	 * @brief 获取分月合约代码
	 * @param stdCode 标准化合约代码
	 * @return 分月合约代码
	 * @details 将标准化合约代码转换为原始分月合约代码
	 */
	virtual std::string		stra_get_rawcode(const char* stdCode) override;

	/**
	 * @brief 订阅Tick数据
	 * @param stdCode 标准化合约代码
	 * @details 订阅指定合约的实时Tick数据，当有新数据到来时会触发on_tick回调
	 */
	virtual void stra_sub_ticks(const char* stdCode) override;

	/**
	 * @brief 输出信息日志
	 * @param message 日志消息
	 * @details 输出信息级别的日志到策略日志文件
	 */
	virtual void stra_log_info(const char* message) override;

	/**
	 * @brief 输出调试日志
	 * @param message 日志消息
	 * @details 输出调试级别的日志到策略日志文件
	 */
	virtual void stra_log_debug(const char* message) override;

	/**
	 * @brief 输出警告日志
	 * @param message 日志消息
	 * @details 输出警告级别的日志到策略日志文件
	 */
	virtual void stra_log_warn(const char* message) override;

	/**
	 * @brief 输出错误日志
	 * @param message 日志消息
	 * @details 输出错误级别的日志到策略日志文件
	 */
	virtual void stra_log_error(const char* message) override;

	/**
	 * @brief 保存用户自定义数据
	 * @param key 数据键
	 * @param val 数据值
	 * @details 将用户自定义的数据保存到文件中，方便策略重启时恢复
	 */
	virtual void stra_save_user_data(const char* key, const char* val) override;

	/**
	 * @brief 加载用户自定义数据
	 * @param key 数据键
	 * @param defVal 默认值，当键不存在时返回
	 * @return 保存的数据值或默认值
	 * @details 从文件中加载用户自定义的数据
	 */
	virtual const char* stra_load_user_data(const char* key, const char* defVal = "") override;

protected:
	/**
	 * @brief 策略上下文ID
	 * @details 唯一标识策略上下文的ID
	 */
	uint32_t		_context_id;

	/**
	 * @brief 选股引擎指针
	 * @details 指向管理策略的选股引擎实例
	 */
	WtSelEngine*	_engine;

	/**
	 * @brief 价格滑点
	 * @details 模拟交易价格滑点，用于计算交易成本
	 */
	int32_t			_slippage;

	/**
	 * @brief 总计算时间
	 * @details 策略总计算时间，用于性能统计
	 */
	uint64_t		_total_calc_time;

	/**
	 * @brief 总计算次数
	 * @details 策略总计算次数，用于性能统计
	 */
	uint32_t		_emit_times;

	/**
	 * @brief 定时调度日期
	 * @details 用于记录定时任务的调度日期
	 */
	uint32_t		_schedule_date;

	/**
	 * @brief 定时调度时间
	 * @details 用于记录定时任务的调度时间
	 */
	uint32_t		_schedule_time;

	/**
	 * @brief K线标签结构体
	 * @details 用于记录K线收盘状态的结构体
	 */
	typedef struct _KlineTag
	{
		/**
		 * @brief 是否已收盘
		 * @details 记录当前K线是否已经收盘
		 */
		bool			_closed;

		/**
		 * @brief 构造函数
		 * @details 初始化K线标签，默认未收盘
		 */
		_KlineTag() :_closed(false){}

	} KlineTag;

	/**
	 * @brief K线标签映射表类型
	 * @details 以合约代码为键，K线标签为值的映射表
	 */
	typedef wt_hashmap<std::string, KlineTag> KlineTags;

	/**
	 * @brief K线标签映射表
	 * @details 存储不同合约的K线状态
	 */
	KlineTags	_kline_tags;

	/**
	 * @brief 价格映射表类型
	 * @details 以合约代码为键，价格为值的映射表
	 */
	typedef wt_hashmap<std::string, double> PriceMap;

	/**
	 * @brief 价格映射表
	 * @details 存储不同合约的当前价格
	 */
	PriceMap		_price_map;

	/**
	 * @brief 详细仓位信息结构体
	 * @details 用于存储交易明细详细信息的结构体
	 */
	typedef struct _DetailInfo
	{
		/**
		 * @brief 是否为多头方向
		 * @details 记录仓位方向，true表示多头，false表示空头
		 */
		bool		_long;

		/**
		 * @brief 开仓价格
		 * @details 记录开仓价格
		 */
		double		_price;

		/**
		 * @brief 仓位数量
		 * @details 记录仓位数量
		 */
		double		_volume;

		/**
		 * @brief 开仓时间
		 * @details 记录开仓时间，时间戳格式
		 */
		uint64_t	_opentime;

		/**
		 * @brief 开仓交易日
		 * @details 记录开仓日期，格式YYYYMMDD
		 */
		uint32_t	_opentdate;

		/**
		 * @brief 最大盈利
		 * @details 记录持仓过程中的最大盈利
		 */
		double		_max_profit;

		/**
		 * @brief 最大亏损
		 * @details 记录持仓过程中的最大亏损
		 */
		double		_max_loss;

		/**
		 * @brief 最高价格
		 * @details 记录持仓过程中的最高价格
		 */
		double		_max_price;

		/**
		 * @brief 最低价格
		 * @details 记录持仓过程中的最低价格
		 */
		double		_min_price;

		/**
		 * @brief 当前盈亏
		 * @details 记录当前仓位的盈亏情况
		 */
		double		_profit;

		/**
		 * @brief 开仓标签
		 * @details 记录开仓时的用户标签
		 */
		char		_opentag[32];

		/**
		 * @brief 构造函数
		 * @details 初始化结构体成员变量为0
		 */
		_DetailInfo()
		{
			memset(this, 0, sizeof(_DetailInfo));
		}
	} DetailInfo;

	/**
	 * @brief 持仓信息结构体
	 * @details 用于管理交易品种的持仓信息
	 */
	typedef struct _PosInfo
	{
		/**
		 * @brief 持仓量
		 * @details 当前品种的持仓量
		 */
		double		_volume;

		/**
		 * @brief 平仓盈亏
		 * @details 已实现的平仓盈亏
		 */
		double		_closeprofit;

		/**
		 * @brief 浮动盈亏
		 * @details 当前持仓的浮动盈亏
		 */
		double		_dynprofit;

		/**
		 * @brief 最后开仓时间
		 * @details 最近一次开仓的时间，时间戳格式
		 */
		uint64_t	_last_entertime;

		/**
		 * @brief 最后平仓时间
		 * @details 最近一次平仓的时间，时间戳格式
		 */
		uint64_t	_last_exittime;

		/**
		 * @brief 冻结量
		 * @details 当前冻结的持仓量
		 */
		double		_frozen;

		/**
		 * @brief 冻结日期
		 * @details 持仓冻结的日期，格式YYYYMMDD
		 */
		uint32_t	_frozen_date;

		/**
		 * @brief 详细持仓列表
		 * @details 存储所有明细交易详情的列表
		 */
		std::vector<DetailInfo> _details;

		/**
		 * @brief 构造函数
		 * @details 初始化成员变量为默认值
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
	 * @brief 持仓映射表类型
	 * @details 以合约代码为键，持仓信息为值的映射表
	 */
	typedef wt_hashmap<std::string, PosInfo> PositionMap;

	/**
	 * @brief 持仓映射表
	 * @details 存储不同合约的持仓信息
	 */
	PositionMap		_pos_map;

	/**
	 * @brief 信号信息结构体
	 * @details 用于存储选股信号的相关信息
	 */
	typedef struct _SigInfo
	{
		/**
		 * @brief 目标仓位量
		 * @details 信号指示的目标持仓量
		 */
		double		_volume;

		/**
		 * @brief 用户标签
		 * @details 信号相关的用户自定义标签
		 */
		std::string	_usertag;

		/**
		 * @brief 信号价格
		 * @details 信号生成时的市场价格
		 */
		double		_sigprice;

		/**
		 * @brief 是否已触发
		 * @details 信号是否已经触发执行
		 */
		bool		_triggered;

		/**
		 * @brief 生成时间
		 * @details 信号生成的时间戳
		 */
		uint64_t	_gentime;

		/**
		 * @brief 构造函数
		 * @details 初始化成员变量为默认值
		 */
		_SigInfo()
		{
			_volume = 0;
			_sigprice = 0;
			_triggered = false;
			_gentime = 0;
		}
	}SigInfo;

	/**
	 * @brief 信号映射表类型
	 * @details 以合约代码为键，信号信息为值的映射表
	 */
	typedef wt_hashmap<std::string, SigInfo>	SignalMap;

	/**
	 * @brief 信号映射表
	 * @details 存储不同合约的信号信息
	 */
	SignalMap		_sig_map;

	/**
	 * @brief 交易日志文件
	 * @details 用于记录交易详情的日志文件
	 */
	BoostFilePtr	_trade_logs;

	/**
	 * @brief 平仓日志文件
	 * @details 用于记录平仓明细的日志文件
	 */
	BoostFilePtr	_close_logs;

	/**
	 * @brief 资金日志文件
	 * @details 用于记录资金变化的日志文件
	 */
	BoostFilePtr	_fund_logs;

	/**
	 * @brief 信号日志文件
	 * @details 用于记录选股信号的日志文件
	 */
	BoostFilePtr	_sig_logs;

	/**
	 * @brief 持仓日志文件
	 * @details 用于记录持仓变化的日志文件
	 */
	BoostFilePtr	_pos_logs;

	/**
	 * @brief 是否处于调度中的标记
	 * @details 记录策略当前是否在自动调度周期内
	 */
	bool			_is_in_schedule;

	/**
	 * @brief 用户数据哈希表类型
	 * @details 用于存储用户自定义数据的哈希表类型
	 */
	typedef wt_hashmap<std::string, std::string> StringHashMap;

	/**
	 * @brief 用户数据哈希表
	 * @details 存储用户自定义的键值对数据
	 */
	StringHashMap	_user_datas;

	/**
	 * @brief 用户数据是否被修改
	 * @details 记录用户数据是否被修改，用于决定是否需要写回磁盘
	 */
	bool			_ud_modified;

	/**
	 * @brief 策略资金信息结构体
	 * @details 存储策略资金相关信息的结构体
	 */
	typedef struct _StraFundInfo
	{
		/**
		 * @brief 总盈亏
		 * @details 策略的总体实现盈亏
		 */
		double	_total_profit;

		/**
		 * @brief 总浮动盈亏
		 * @details 策略的总体浮动盈亏
		 */
		double	_total_dynprofit;

		/**
		 * @brief 总手续费
		 * @details 策略的总交易手续费
		 */
		double	_total_fees;

		/**
		 * @brief 构造函数
		 * @details 初始化所有资金相关数据为0
		 */
		_StraFundInfo()
		{
			memset(this, 0, sizeof(_StraFundInfo));
		}
	} StraFundInfo;

	/**
	 * @brief 策略资金信息
	 * @details 存储当前策略的资金信息
	 */
	StraFundInfo		_fund_info;

	/**
	 * @brief Tick订阅列表
	 * @details 存储已订阅实时行情的合约清单
	 */
	wt_hashset<std::string> _tick_subs;
};


NS_WTP_END