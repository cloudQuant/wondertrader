/*!
 * \file UftStraContext.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief UFT策略执行环境上下文定义文件
 * \details 该文件定义了UFT策略的执行环境上下文，为策略提供了交易接口、行情数据访问和订单管理等功能
 */
#pragma once
#include "ITrdNotifySink.h"
#include "UftDataDefs.h"
#include "../Includes/IUftStraCtx.h"
#include "../Includes/FasterDefs.h"
#include "../Share/fmtlib.h"

#include "../Share/BoostMappingFile.hpp"
typedef std::shared_ptr<BoostMappingFile> BoostMFPtr;

class UftStrategy;

NS_WTP_BEGIN
class WtUftEngine;
class TraderAdapter;

/**
 * @brief UFT策略执行上下文类
 * @details 该类提供了UFT策略的执行环境，实现了IUftStraCtx接口和ITrdNotifySink接口，
 * 用于管理策略的执行、行情数据的处理、交易指令的发送以及持仓管理等功能
 */
class UftStraContext : public IUftStraCtx, public ITrdNotifySink
{
public:
	/**
	 * @brief 构造函数
	 * @param engine 策略引擎指针
	 * @param name 策略名称
	 */
	UftStraContext(WtUftEngine* engine, const char* name);

	/**
	 * @brief 析构函数
	 */
	virtual ~UftStraContext();

	/**
	 * @brief 设置策略对象
	 * @param stra 策略对象指针
	 */
	void set_strategy(UftStrategy* stra){ _strategy = stra; }

	/**
	 * @brief 获取策略对象
	 * @return 策略对象指针
	 */
	UftStrategy* get_stragety() { return _strategy; }

	/**
	 * @brief 设置交易适配器
	 * @param trader 交易适配器指针
	 */
	void setTrader(TraderAdapter* trader);

public:
	/**
	 * @brief 获取上下文ID
	 * @return 上下文唯一标识符
	 */
	virtual uint32_t id() { return _context_id; }

	/**
	 * @brief 策略初始化回调
	 * @details 在策略初始化阶段被调用，用于执行策略的初始化操作
	 */
	virtual void on_init() override;

	/**
	 * @brief 逐笔行情数据回调
	 * @param code 合约代码
	 * @param newTick 最新的tick数据
	 */
	virtual void on_tick(const char* code, WTSTickData* newTick) override;

	/**
	 * @brief 委托队列数据回调
	 * @param stdCode 合约代码
	 * @param newOrdQue 最新的委托队列数据
	 */
	virtual void on_order_queue(const char* stdCode, WTSOrdQueData* newOrdQue) override;

	/**
	 * @brief 委托明细数据回调
	 * @param stdCode 合约代码
	 * @param newOrdDtl 最新的委托明细数据
	 */
	virtual void on_order_detail(const char* stdCode, WTSOrdDtlData* newOrdDtl) override;

	/**
	 * @brief 成交明细数据回调
	 * @param stdCode 合约代码
	 * @param newTrans 最新的成交明细数据
	 */
	virtual void on_transaction(const char* stdCode, WTSTransData* newTrans) override;

	/**
	 * @brief K线数据回调
	 * @param code 合约代码
	 * @param period 周期，如m1/m5等
	 * @param times 倍数
	 * @param newBar 最新的K线数据
	 */
	virtual void on_bar(const char* code, const char* period, uint32_t times, WTSBarStruct* newBar) override;

	/**
	 * @brief 成交回调
	 * @param localid 本地订单ID
	 * @param stdCode 合约代码
	 * @param isLong 是否为多仓
	 * @param offset 开平标记
	 * @param vol 成交量
	 * @param price 成交价格
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double vol, double price) override;

	/**
	 * @brief 订单状态回调
	 * @param localid 本地订单ID
	 * @param stdCode 合约代码
	 * @param isLong 是否为多仓
	 * @param offset 开平标记
	 * @param totalQty 总数量
	 * @param leftQty 剩余数量
	 * @param price 委托价格
	 * @param isCanceled 是否已撤销
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double totalQty, double leftQty, double price, bool isCanceled = false) override;

	/**
	 * @brief 通道就绪回调
	 * @param tradingday 交易日
	 */
	virtual void on_channel_ready(uint32_t tradingday) override;

	/**
	 * @brief 通道断开回调
	 */
	virtual void on_channel_lost() override;

	/**
	 * @brief 委托回报回调
	 * @param localid 本地订单ID
	 * @param stdCode 合约代码
	 * @param bSuccess 委托是否成功
	 * @param message 错误信息（委托失败时）
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override;

	/**
	 * @brief 持仓回调
	 * @param stdCode 合约代码
	 * @param isLong 是否为多仓
	 * @param prevol 前持仓量
	 * @param preavail 前可用持仓量
	 * @param newvol 当前持仓量
	 * @param newavail 当前可用持仓量
	 * @param tradingday 交易日
	 */
	virtual void on_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday) override;

	/**
	 * @brief 交易会话开始回调
	 * @param uTDate 交易日期
	 */
	virtual void on_session_begin(uint32_t uTDate) override;

	/**
	 * @brief 交易会话结束回调
	 * @param uTDate 交易日期
	 */
	virtual void on_session_end(uint32_t uTDate) override;

	/**
	 * @brief 策略参数更新回调
	 */
	virtual void on_params_updated() override;


public:
	//virtual void watch_param(const char* name, const char* val) override;
	//virtual void watch_param(const char* name, double val) override;
	//virtual void watch_param(const char* name, uint32_t val) override;
	//virtual void watch_param(const char* name, uint64_t val) override;
	//virtual void watch_param(const char* name, int32_t val) override;
	//virtual void watch_param(const char* name, int64_t val) override;

	/**
	 * @brief 监控字符串类型参数
	 * @param name 参数名称
	 * @param initVal 参数初始值
	 * @return 字符串类型参数值
	 */
	virtual const char*	watch_param(const char* name, const char* initVal = "") override;

	/**
	 * @brief 监控浮点数类型参数
	 * @param name 参数名称
	 * @param initVal 参数初始值
	 * @return 浮点数类型参数值
	 */
	virtual double		watch_param(const char* name, double initVal = 0) override;

	/**
	 * @brief 监控32位无符号整数类型参数
	 * @param name 参数名称
	 * @param initVal 参数初始值
	 * @return 32位无符号整数类型参数值
	 */
	virtual uint32_t	watch_param(const char* name, uint32_t initVal = 0) override;

	/**
	 * @brief 监控64位无符号整数类型参数
	 * @param name 参数名称
	 * @param initVal 参数初始值
	 * @return 64位无符号整数类型参数值
	 */
	virtual uint64_t	watch_param(const char* name, uint64_t initVal = 0) override;

	/**
	 * @brief 监控32位有符号整数类型参数
	 * @param name 参数名称
	 * @param initVal 参数初始值
	 * @return 32位有符号整数类型参数值
	 */
	virtual int32_t		watch_param(const char* name, int32_t initVal = 0) override;

	/**
	 * @brief 监控64位有符号整数类型参数
	 * @param name 参数名称
	 * @param initVal 参数初始值
	 * @return 64位有符号整数类型参数值
	 */
	virtual int64_t		watch_param(const char* name, int64_t initVal = 0) override;

	/**
	 * @brief 提交参数监控器
	 * @details 将所有通过watch_param注册的参数提交给参数管理器
	 */
	virtual void commit_param_watcher() override;

	/**
	 * @brief 读取字符串类型参数
	 * @param name 参数名称
	 * @param defVal 默认值
	 * @return 字符串类型参数值
	 */
	virtual const char*	read_param(const char* name, const char* defVal = "") override;

	/**
	 * @brief 读取浮点数类型参数
	 * @param name 参数名称
	 * @param defVal 默认值
	 * @return 浮点数类型参数值
	 */
	virtual double		read_param(const char* name, double defVal = 0) override;

	/**
	 * @brief 读取32位无符号整数类型参数
	 * @param name 参数名称
	 * @param defVal 默认值
	 * @return 32位无符号整数类型参数值
	 */
	virtual uint32_t	read_param(const char* name, uint32_t defVal = 0) override;

	/**
	 * @brief 读取64位无符号整数类型参数
	 * @param name 参数名称
	 * @param defVal 默认值
	 * @return 64位无符号整数类型参数值
	 */
	virtual uint64_t	read_param(const char* name, uint64_t defVal = 0) override;

	/**
	 * @brief 读取32位有符号整数类型参数
	 * @param name 参数名称
	 * @param defVal 默认值
	 * @return 32位有符号整数类型参数值
	 */
	virtual int32_t		read_param(const char* name, int32_t defVal = 0) override;

	/**
	 * @brief 读取64位有符号整数类型参数
	 * @param name 参数名称
	 * @param defVal 默认值
	 * @return 64位有符号整数类型参数值
	 */
	virtual int64_t		read_param(const char* name, int64_t defVal = 0) override;

	/**
	 * @brief 同步字符串类型参数
	 * @param name 参数名称
	 * @param initVal 参数初始值
	 * @param bForceWrite 是否强制写入
	 * @return 字符串类型参数值
	 */
	virtual const char*	sync_param(const char* name, const char* initVal = "", bool bForceWrite = false) override;

	/**
	 * @brief 同步浮点数类型参数
	 * @param name 参数名称
	 * @param initVal 参数初始值
	 * @param bForceWrite 是否强制写入
	 * @return 浮点数类型参数指针
	 */
	virtual double*		sync_param(const char* name, double initVal = 0, bool bForceWrite = false) override;

	/**
	 * @brief 同步32位无符号整数类型参数
	 * @param name 参数名称
	 * @param initVal 参数初始值
	 * @param bForceWrite 是否强制写入
	 * @return 32位无符号整数类型参数指针
	 */
	virtual uint32_t*	sync_param(const char* name, uint32_t initVal = 0, bool bForceWrite = false) override;

	/**
	 * @brief 同步64位无符号整数类型参数
	 * @param name 参数名称
	 * @param initVal 参数初始值
	 * @param bForceWrite 是否强制写入
	 * @return 64位无符号整数类型参数指针
	 */
	virtual uint64_t*	sync_param(const char* name, uint64_t initVal = 0, bool bForceWrite = false) override;

	/**
	 * @brief 同步32位有符号整数类型参数
	 * @param name 参数名称
	 * @param initVal 参数初始值
	 * @param bForceWrite 是否强制写入
	 * @return 32位有符号整数类型参数指针
	 */
	virtual int32_t*		sync_param(const char* name, int32_t initVal = 0, bool bForceWrite = false) override;

	/**
	 * @brief 同步64位有符号整数类型参数
	 * @param name 参数名称
	 * @param initVal 参数初始值
	 * @param bForceWrite 是否强制写入
	 * @return 64位有符号整数类型参数指针
	 */
	virtual int64_t*		sync_param(const char* name, int64_t initVal = 0, bool bForceWrite = false) override;

public:
	//////////////////////////////////////////////////////////////////////////
	//IHftStraCtx 接口
	/**
	 * @brief 获取当前交易日期
	 * @return 交易日期
	 */
	virtual uint32_t stra_get_date() override;

	/**
	 * @brief 获取当前交易时间
	 * @return 交易时间（HHMM格式）
	 */
	virtual uint32_t stra_get_time() override;

	/**
	 * @brief 获取当前秒数
	 * @return 当前秒数
	 */
	virtual uint32_t stra_get_secs() override;

	/**
	 * @brief 取消订单
	 * @param localid 本地订单ID
	 * @return 是否取消成功
	 */
	virtual bool stra_cancel(uint32_t localid) override;

	/**
	 * @brief 取消所有订单
	 * @param stdCode 合约代码
	 * @return 取消的订单ID列表
	 */
	virtual OrderIDs stra_cancel_all(const char* stdCode) override;

	/*
	 *	下单接口: 买入
	 *
	 *	@stdCode	合约代码
	 *	@price		下单价格，0则是市价单
	 *	@qty		下单数量
	 *	@flag		下单标志: 0-normal，1-fak，2-fok，默认0
	 */
	/**
	 * @brief 策略买入接口
	 * @param stdCode 合约代码
	 * @param price 下单价格，0表示市价单
	 * @param qty 下单数量
	 * @param flag 下单标志：0-normal，1-fak，2-fok，默认0
	 * @return 订单ID列表
	 */
	virtual OrderIDs	stra_buy(const char* stdCode, double price, double qty, int flag = 0) override;

	/*
	 *	下单接口: 卖出
	 *
	 *	@stdCode	合约代码
	 *	@price		下单价格，0则是市价单
	 *	@qty		下单数量
	 *	@flag		下单标志: 0-normal，1-fak，2-fok，默认0
	 */
	/**
	 * @brief 策略卖出接口
	 * @param stdCode 合约代码
	 * @param price 下单价格，0表示市价单
	 * @param qty 下单数量
	 * @param flag 下单标志：0-normal，1-fak，2-fok，默认0
	 * @return 订单ID列表
	 */
	virtual OrderIDs	stra_sell(const char* stdCode, double price, double qty, int flag = 0) override;

	/*
	 *	下单接口: 开多
	 *
	 *	@stdCode	合约代码
	 *	@price		下单价格，0则是市价单
	 *	@qty		下单数量
	 *	@flag		下单标志: 0-normal，1-fak，2-fok
	 */
	/**
	 * @brief 策略开多接口
	 * @param stdCode 合约代码
	 * @param price 下单价格，0表示市价单
	 * @param qty 下单数量
	 * @param flag 下单标志：0-normal，1-fak，2-fok，默认0
	 * @return 订单ID
	 */
	virtual uint32_t	stra_enter_long(const char* stdCode, double price, double qty, int flag = 0) override;

	/*
	 *	下单接口: 开空
	 *
	 *	@stdCode	合约代码
	 *	@price		下单价格，0则是市价单
	 *	@qty		下单数量
	 *	@flag		下单标志: 0-normal，1-fak，2-fok
	 */
	/**
	 * @brief 策略开空接口
	 * @param stdCode 合约代码
	 * @param price 下单价格，0表示市价单
	 * @param qty 下单数量
	 * @param flag 下单标志：0-normal，1-fak，2-fok，默认0
	 * @return 订单ID
	 */
	virtual uint32_t	stra_enter_short(const char* stdCode, double price, double qty, int flag = 0) override;

	/*
	 *	下单接口: 平多
	 *
	 *	@stdCode	合约代码
	 *	@price		下单价格，0则是市价单
	 *	@qty		下单数量
	 *	@isToday	是否今仓，默认false
	 *	@flag		下单标志: 0-normal，1-fak，2-fok，默认0
	 */
	/**
	 * @brief 策略平多接口
	 * @param stdCode 合约代码
	 * @param price 下单价格，0表示市价单
	 * @param qty 下单数量
	 * @param isToday 是否为今仓，默认false
	 * @param flag 下单标志：0-normal，1-fak，2-fok，默认0
	 * @return 订单ID
	 */
	virtual uint32_t	stra_exit_long(const char* stdCode, double price, double qty, bool isToday = false, int flag = 0) override;

	/*
	 *	下单接口: 平空
	 *
	 *	@stdCode	合约代码
	 *	@price		下单价格，0则是市价单
	 *	@qty		下单数量
	 *	@isToday	是否今仓，默认false
	 *	@flag		下单标志: 0-normal，1-fak，2-fok，默认0
	 */
	/**
	 * @brief 策略平空接口
	 * @param stdCode 合约代码
	 * @param price 下单价格，0表示市价单
	 * @param qty 下单数量
	 * @param isToday 是否今仓，默认false
	 * @param flag 下单标志：0-normal，1-fak，2-fok，默认0
	 * @return 订单ID
	 */
	virtual uint32_t	stra_exit_short(const char* stdCode, double price, double qty, bool isToday = false, int flag = 0) override;

	/**
	 * @brief 获取合约信息
	 * @param stdCode 合约代码
	 * @return 商品信息对象指针
	 */
	virtual WTSCommodityInfo* stra_get_comminfo(const char* stdCode) override;

	/**
	 * @brief 获取K线数据
	 * @param stdCode 合约代码
	 * @param period 周期字符串，如m1/m5等
	 * @param count 要获取的K线数量
	 * @return K线数据切片指针
	 */
	virtual WTSKlineSlice* stra_get_bars(const char* stdCode, const char* period, uint32_t count) override;

	/**
	 * @brief 获取Tick数据
	 * @param stdCode 合约代码
	 * @param count 要获取的Tick数量
	 * @return Tick数据切片指针
	 */
	virtual WTSTickSlice* stra_get_ticks(const char* stdCode, uint32_t count) override;

	/**
	 * @brief 获取委托明细数据
	 * @param stdCode 合约代码
	 * @param count 要获取的委托明细数量
	 * @return 委托明细数据切片指针
	 */
	virtual WTSOrdDtlSlice*	stra_get_order_detail(const char* stdCode, uint32_t count) override;

	/**
	 * @brief 获取委托队列数据
	 * @param stdCode 合约代码
	 * @param count 要获取的委托队列数量
	 * @return 委托队列数据切片指针
	 */
	virtual WTSOrdQueSlice*	stra_get_order_queue(const char* stdCode, uint32_t count) override;

	/**
	 * @brief 获取成交明细数据
	 * @param stdCode 合约代码
	 * @param count 要获取的成交明细数量
	 * @return 成交明细数据切片指针
	 */
	virtual WTSTransSlice*	stra_get_transaction(const char* stdCode, uint32_t count) override;

	/**
	 * @brief 获取最新的Tick数据
	 * @param stdCode 合约代码
	 * @return 最新的Tick数据指针
	 */
	virtual WTSTickData* stra_get_last_tick(const char* stdCode) override;

	/**
	 * @brief 输出信息日志
	 * @param message 日志消息
	 */
	virtual void stra_log_info(const char* message) override;

	/**
	 * @brief 输出调试日志
	 * @param message 日志消息
	 */
	virtual void stra_log_debug(const char* message) override;

	/**
	 * @brief 输出错误日志
	 * @param message 日志消息
	 */
	virtual void stra_log_error(const char* message) override;

	/**
	 * @brief 获取持仓数量
	 * @param stdCode 合约代码
	 * @param bOnlyValid 是否只计算有效持仓，默认false
	 * @param iFlag 持仓标记，默认3（所有持仓）
	 * @return 持仓数量
	 */
	virtual double stra_get_position(const char* stdCode, bool bOnlyValid = false, int32_t iFlag = 3) override;

	/**
	 * @brief 获取本地持仓数量
	 * @param stdCode 合约代码
	 * @return 本地持仓数量
	 */
	virtual double stra_get_local_position(const char* stdCode) override;

	/**
	 * @brief 获取本地浮动盈亏
	 * @param stdCode 合约代码
	 * @return 本地浮动盈亏
	 */
	virtual double stra_get_local_posprofit(const char* stdCode) override;

	/**
	 * @brief 获取本地平仓盈亏
	 * @param stdCode 合约代码
	 * @return 本地平仓盈亏
	 */
	virtual double stra_get_local_closeprofit(const char* stdCode) override;

	/**
	 * @brief 枚举持仓
	 * @param stdCode 合约代码
	 * @return 持仓数量
	 */
	virtual double stra_enum_position(const char* stdCode) override;

	/**
	 * @brief 获取合约当前价格
	 * @param stdCode 合约代码
	 * @return 当前价格
	 */
	virtual double stra_get_price(const char* stdCode) override;

	/**
	 * @brief 获取未完成委托数量
	 * @param stdCode 合约代码
	 * @return 未完成委托数量
	 */
	virtual double stra_get_undone(const char* stdCode) override;

	/**
	 * @brief 获取合约信息标记
	 * @param stdCode 合约代码
	 * @return 信息标记
	 */
	virtual uint32_t stra_get_infos(const char* stdCode) override;

	/**
	 * @brief 订阅Tick数据
	 * @param stdCode 合约代码
	 */
	virtual void stra_sub_ticks(const char* stdCode) override;

	/**
	 * @brief 订阅委托明细数据
	 * @param stdCode 合约代码
	 */
	virtual void stra_sub_order_details(const char* stdCode) override;

	/**
	 * @brief 订阅委托队列数据
	 * @param stdCode 合约代码
	 */
	virtual void stra_sub_order_queues(const char* stdCode) override;

	/**
	 * @brief 订阅成交明细数据
	 * @param stdCode 合约代码
	 */
	virtual void stra_sub_transactions(const char* stdCode) override;

private:
	/**
	 * @brief 格式化调试日志
	 * @tparam Args 参数类型列表
	 * @param format 格式化字符串
	 * @param args 参数列表
	 */
	template<typename... Args>
	void log_debug(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_debug(buffer);
	}

	/**
	 * @brief 格式化信息日志
	 * @tparam Args 参数类型列表
	 * @param format 格式化字符串
	 * @param args 参数列表
	 */
	template<typename... Args>
	void log_info(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_info(buffer);
	}

	/**
	 * @brief 格式化错误日志
	 * @tparam Args 参数类型列表
	 * @param format 格式化字符串
	 * @param args 参数列表
	 */
	template<typename... Args>
	void log_error(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_error(buffer);
	}

private:
	/**
	 * @brief 持仓块配对结构
	 * @details 包含持仓数据块、内存映射文件和互斥锁
	 */
	typedef struct _PosBlkPair
	{
		uft::PositionBlock*	_block; ///< 持仓数据块指针
		BoostMFPtr			_file;   ///< 内存映射文件指针
		SpinMutex			_mutex;  ///< 访问互斥锁

		/**
		 * @brief 构造函数
		 */
		_PosBlkPair()
		{
			_block = NULL;
			_file = NULL;
		}

	} PosBlkPair;

	/**
	 * @brief 订单块配对结构
	 * @details 包含订单数据块、内存映射文件和互斥锁
	 */
	typedef struct _OrdBlkPair
	{
		uft::OrderBlock*	_block; ///< 订单数据块指针
		BoostMFPtr			_file;   ///< 内存映射文件指针
		SpinMutex			_mutex;  ///< 访问互斥锁

		/**
		 * @brief 构造函数
		 */
		_OrdBlkPair()
		{
			_block = NULL;
			_file = NULL;
		}

	} OrdBlkPair;

	/**
	 * @brief 成交块配对结构
	 * @details 包含成交数据块、内存映射文件和互斥锁
	 */
	typedef struct _TrdBlkPair
	{
		uft::TradeBlock*	_block; ///< 成交数据块指针
		BoostMFPtr			_file;   ///< 内存映射文件指针
		SpinMutex			_mutex;  ///< 访问互斥锁

		/**
		 * @brief 构造函数
		 */
		_TrdBlkPair()
		{
			_block = NULL;
			_file = NULL;
		}

	} TrdBlkPair;

	/**
	 * @brief 回合块配对结构
	 * @details 包含回合数据块、内存映射文件和互斥锁
	 */
	typedef struct _RndBlkPair
	{
		uft::RoundBlock*	_block; ///< 回合数据块指针
		BoostMFPtr			_file;   ///< 内存映射文件指针
		SpinMutex			_mutex;  ///< 访问互斥锁

		/**
		 * @brief 构造函数
		 */
		_RndBlkPair()
		{
			_block = NULL;
			_file = NULL;
		}

	} RndBlkPair;

	/**
	 * @brief 加载本地数据
	 * @details 加载本地内存映射文件中的持仓、订单、成交和回合数据
	 */
	void	load_local_data();

	/**
	 * @brief 持仓块配对
	 */
	/**
	 * @brief 持仓块配对
	 */
	PosBlkPair		_pos_blk;

	/**
	 * @brief 订单块配对
	 */
	OrdBlkPair		_ord_blk;

	/**
	 * @brief 成交块配对
	 */
	TrdBlkPair		_trd_blk;

	/**
	 * @brief 回合块配对
	 */
	RndBlkPair		_rnd_blk;

		/**
	 * @brief 持仓信息结构
	 * @details 存储持仓数据和明细信息
	 */
	typedef struct _Position
	{
		//多仓数据
		double	_volume;       ///< 持仓量
		double	_opencost;     ///< 开仓成本
		double	_dynprofit;    ///< 动态浮动盈亏

		double	_total_profit; ///< 总盈亏

		uint32_t _valid_idx;   ///< 有效持仓明细索引

		std::vector<uft::DetailStruct*> _details; ///< 持仓明细列表

		/**
		 * @brief 构造函数
		 */
		_Position():_volume(0),_valid_idx(0), _total_profit(0),
			_opencost(0),_dynprofit(0)
		{
		}
	} PosInfo;

	/**
	 * @brief 持仓信息哈希表
	 * @details 按合约代码索引的持仓信息集合
	 */
	wt_hashmap<std::string, PosInfo> _positions;

	/**
	 * @brief 订单ID哈希表
	 * @details 按本地订单ID索引的订单结构指针集合
	 */
	wt_hashmap<uint32_t, uft::OrderStruct*> _order_ids;

	/**
	 * @brief 检查是否为本策略发出的订单
	 * @param localid 本地订单ID
	 * @return 是否属于本策略的订单
	 */
	inline bool is_my_order(uint32_t localid) const
	{
		auto it = _order_ids.find(localid);
		return it != _order_ids.end();
	}

private:
	/**
	 * @brief 上下文ID
	 * @details 全局唯一的上下文标识符
	 */
	uint32_t		_context_id;

	/**
	 * @brief 策略引擎指针
	 * @details 指向管理当前策略的引擎对象
	 */
	WtUftEngine*	_engine;

	/**
	 * @brief 交易适配器指针
	 * @details 处理实际交易指令发送和回报的适配器
	 */
	TraderAdapter*	_trader;

	/**
	 * @brief 交易日
	 * @details 当前交易所的交易日期
	 */
	uint32_t		_tradingday;

	/**
	 * @brief 策略对象指针
	 * @details 指向用户实现的策略类对象
	 */
	UftStrategy*	_strategy;
};

NS_WTP_END