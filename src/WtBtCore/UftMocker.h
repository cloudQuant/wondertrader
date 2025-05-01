/*!
 * \file UftMocker.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief UFT策略回测模拟器头文件
 * \details 定义了UFT策略回测模拟器的接口和数据结构，用于模拟UFT策略在历史数据上的运行
 *          实现了IDataSink接口用于接收回放的历史数据，实现了IUftStraCtx接口用于提供策略运行的上下文环境
 */
#pragma once
#include <queue>
#include <sstream>

#include "HisDataReplayer.h"

#include "../Includes/FasterDefs.h"
#include "../Includes/IUftStraCtx.h"
#include "../Includes/UftStrategyDefs.h"

#include "../Share/StdUtils.hpp"
#include "../Share/DLLHelper.hpp"
#include "../Share/fmtlib.h"

class HisDataReplayer;

/**
 * @brief UFT策略回测模拟器类
 * @details 该类实现了两个接口：
 *          1. IDataSink接口：用于接收回放的历史数据
 *          2. IUftStraCtx接口：用于为策略提供执行环境
 *          
 *          模拟器核心功能包括：
 *          - 加载并初始化UFT策略
 *          - 接收历史数据并传递给策略
 *          - 模拟策略交易并维护仓位、资金等信息
 *          - 计算策略绩效并生成回测日志
 */
class UftMocker : public IDataSink, public IUftStraCtx
{
public:
	/**
	 * @brief UFT策略回测模拟器构造函数
	 * @param replayer 历史数据回放器指针
	 * @param name 策略名称
	 * @details 构造一个新的UFT策略回测模拟器实例，用于执行回测
	 */
	UftMocker(HisDataReplayer* replayer, const char* name);

	/**
	 * @brief UFT策略回测模拟器析构函数
	 * @details 释放模拟器占用的资源，包括策略对象、订单缓存等
	 */
	virtual ~UftMocker();

private:
	/**
	 * @brief 记录调试级别的日志
	 * @tparam Args 可变参数类型
	 * @param format 格式化字符串
	 * @param args 格式化参数
	 * @details 使用fmt库格式化字符串并记录调试级别的日志
	 */
	template<typename... Args>
	void log_debug(const char* format, const Args& ...args)
	{
		std::string s = fmt::format(format, args...);
		stra_log_debug(s.c_str());
	}

	/**
	 * @brief 记录信息级别的日志
	 * @tparam Args 可变参数类型
	 * @param format 格式化字符串
	 * @param args 格式化参数
	 * @details 使用fmt库格式化字符串并记录信息级别的日志
	 */
	template<typename... Args>
	void log_info(const char* format, const Args& ...args)
	{
		std::string s = fmt::format(format, args...);
		stra_log_info(s.c_str());
	}

	/**
	 * @brief 记录错误级别的日志
	 * @tparam Args 可变参数类型
	 * @param format 格式化字符串
	 * @param args 格式化参数
	 * @details 使用fmt库格式化字符串并记录错误级别的日志
	 */
	template<typename... Args>
	void log_error(const char* format, const Args& ...args)
	{
		std::string s = fmt::format(format, args...);
		stra_log_error(s.c_str());
	}

public:
	//////////////////////////////////////////////////////////////////////////
	//IDataSink
	/**
	 * @brief 处理Tick数据
	 * @param stdCode 标准合约代码
	 * @param curTick 当前Tick数据
	 * @param pxType 价格类型
	 * @details 接收并处理回放器推送的Tick数据，主要用于更新最新价格和触发策略的on_tick回调
	 */
	virtual void	handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType) override;
	
	/**
	 * @brief 处理委托队列数据
	 * @param stdCode 标准合约代码
	 * @param curOrdQue 当前委托队列数据
	 * @details 接收并处理回放器推送的委托队列数据，触发策略的on_order_queue回调
	 */
	virtual void	handle_order_queue(const char* stdCode, WTSOrdQueData* curOrdQue) override;
	
	/**
	 * @brief 处理委托明细数据
	 * @param stdCode 标准合约代码
	 * @param curOrdDtl 当前委托明细数据
	 * @details 接收并处理回放器推送的委托明细数据，触发策略的on_order_detail回调
	 */
	virtual void	handle_order_detail(const char* stdCode, WTSOrdDtlData* curOrdDtl) override;
	
	/**
	 * @brief 处理逐笔成交数据
	 * @param stdCode 标准合约代码
	 * @param curTrans 当前逐笔成交数据
	 * @details 接收并处理回放器推送的逐笔成交数据，触发策略的on_transaction回调
	 */
	virtual void	handle_transaction(const char* stdCode, WTSTransData* curTrans) override;

	/**
	 * @brief 处理K线闭合事件
	 * @param stdCode 标准合约代码
	 * @param period 周期标识符
	 * @param times 周期倍数
	 * @param newBar 新的K线数据
	 * @details 当K线周期结束时触发，将新的K线数据传递给策略的on_bar回调
	 */
	virtual void	handle_bar_close(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;
	
	/**
	 * @brief 处理定时事件
	 * @param uDate 交易日期（YYYYMMDD）
	 * @param uTime 交易时间（HHMMSS）
	 * @details 根据回测时间定时触发策略的相关操作
	 */
	virtual void	handle_schedule(uint32_t uDate, uint32_t uTime) override;

	/**
	 * @brief 处理初始化事件
	 * @details 回测引擎初始化时触发，用于执行策略的初始化操作
	 */
	virtual void	handle_init() override;
	
	/**
	 * @brief 处理交易日开始事件
	 * @param curTDate 当前交易日（YYYYMMDD）
	 * @details 在每个交易日开始时触发，用于执行策略的日初操作
	 */
	virtual void	handle_session_begin(uint32_t curTDate) override;
	
	/**
	 * @brief 处理交易日结束事件
	 * @param curTDate 当前交易日（YYYYMMDD）
	 * @details 在每个交易日结束时触发，用于执行策略的日结操作
	 */
	virtual void	handle_session_end(uint32_t curTDate) override;

	/**
	 * @brief 处理回放完成事件
	 * @details 当历史数据回放完成时触发，用于执行回测结束操作（如生成结果分析）
	 */
	virtual void	handle_replay_done() override;

	/**
	 * @brief 当Tick数据更新时的回调
	 * @param stdCode 标准合约代码
	 * @param newTick 新的Tick数据
	 * @details 当授权的合约的Tick数据更新时触发此回调
	 */
	virtual void	on_tick_updated(const char* stdCode, WTSTickData* newTick) override;
	
	/**
	 * @brief 当委托队列数据更新时的回调
	 * @param stdCode 标准合约代码
	 * @param newOrdQue 新的委托队列数据
	 * @details 当授权的合约的委托队列数据更新时触发此回调
	 */
	virtual void	on_ordque_updated(const char* stdCode, WTSOrdQueData* newOrdQue) override;
	
	/**
	 * @brief 当委托明细数据更新时的回调
	 * @param stdCode 标准合约代码
	 * @param newOrdDtl 新的委托明细数据
	 * @details 当授权的合约的委托明细数据更新时触发此回调
	 */
	virtual void	on_orddtl_updated(const char* stdCode, WTSOrdDtlData* newOrdDtl) override;
	
	/**
	 * @brief 当逐笔成交数据更新时的回调
	 * @param stdCode 标准合约代码
	 * @param newTrans 新的逐笔成交数据
	 * @details 当授权的合约的逐笔成交数据更新时触发此回调
	 */
	virtual void	on_trans_updated(const char* stdCode, WTSTransData* newTrans) override;

	//////////////////////////////////////////////////////////////////////////
	//IUftStraCtx
	/**
	 * @brief Tick数据回调
	 * @param stdCode 标准合约代码
	 * @param newTick 新的Tick数据
	 * @details 当有订阅的合约有新的Tick数据到来时触发，策略可以通过该函数接收并处理最新行情
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* newTick) override;

	/**
	 * @brief 委托队列数据回调
	 * @param stdCode 标准合约代码
	 * @param newOrdQue 新的委托队列数据
	 * @details 当有订阅的合约有新的委托队列数据到来时触发
	 */
	virtual void on_order_queue(const char* stdCode, WTSOrdQueData* newOrdQue) override;

	/**
	 * @brief 委托明细数据回调
	 * @param stdCode 标准合约代码
	 * @param newOrdDtl 新的委托明细数据
	 * @details 当有订阅的合约有新的委托明细数据到来时触发
	 */
	virtual void on_order_detail(const char* stdCode, WTSOrdDtlData* newOrdDtl) override;

	/**
	 * @brief 逐笔成交数据回调
	 * @param stdCode 标准合约代码
	 * @param newTrans 新的逐笔成交数据
	 * @details 当有订阅的合约有新的逐笔成交数据到来时触发
	 */
	virtual void on_transaction(const char* stdCode, WTSTransData* newTrans) override;

	/**
	 * @brief 获取策略ID
	 * @return 策略上下文对象的全局唯一ID
	 * @details 用于标识不同的策略上下文实例
	 */
	virtual uint32_t id() override;

	/**
	 * @brief 策略初始化回调
	 * @details 当策略被加载并初始化后触发，策略可以在这里进行初始化操作
	 */
	virtual void on_init() override;

	/**
	 * @brief K线数据回调
	 * @param stdCode 标准合约代码
	 * @param period 周期标识符
	 * @param times 周期倍数
	 * @param newBar 新的K线数据
	 * @details 当K线周期结束并生成新的K线数据时触发
	 */
	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;

	/**
	 * @brief 交易日开始回调
	 * @param curTDate 当前交易日（YYYYMMDD）
	 * @details 在每个交易日开始时触发，策略可以在这里进行日初操作
	 */
	virtual void on_session_begin(uint32_t curTDate) override;

	/**
	 * @brief 交易日结束回调
	 * @param curTDate 当前交易日（YYYYMMDD）
	 * @details 在每个交易日结束时触发，策略可以在这里进行日结操作
	 */
	virtual void on_session_end(uint32_t curTDate) override;

	/**
	 * @brief 撤销订单
	 * @param localid 本地订单ID
	 * @return 撤单是否成功
	 * @details 用于撤销已提交但未完全成交的订单
	 */
	virtual bool stra_cancel(uint32_t localid) override;

	/**
	 * @brief 撤销指定合约的所有活跃订单
	 * @param stdCode 标准合约代码
	 * @return 被撤销订单的本地ID列表
	 * @details 用于批量撤销指定合约的所有未完全成交的订单
	 */
	virtual OrderIDs stra_cancel_all(const char* stdCode) override;

	/**
	 * @brief 买入交易
	 * @param stdCode 标准合约代码
	 * @param price 委托价格
	 * @param qty 委托数量
	 * @param flag 下单标志：0-normal，1-fak，2-fok，默认0
	 * @return 提交的订单本地ID列表
	 * @details 用于提交买入委托订单，返回生成的订单ID列表
	 */
	virtual OrderIDs stra_buy(const char* stdCode, double price, double qty, int flag = 0) override;

	/**
	 * @brief 卖出交易
	 * @param stdCode 标准合约代码
	 * @param price 委托价格
	 * @param qty 委托数量
	 * @param flag 下单标志：0-normal，1-fak，2-fok，默认0
	 * @return 提交的订单本地ID列表
	 * @details 用于提交卖出委托订单，返回生成的订单ID列表
	 */
	virtual OrderIDs stra_sell(const char* stdCode, double price, double qty, int flag = 0) override;

	/**
	 * @brief 开多仓交易
	 * @param stdCode 标准合约代码，格式如SSE.600000
	 * @param price 委托价格
	 * @param qty 下单数量
	 * @param flag 下单标志: 0-normal，1-fak，2-fok，默认0
	 * @return 成功提交的订单本地ID
	 * @details 提交开多委托订单，返回生成的订单ID
	 */
	virtual uint32_t	stra_enter_long(const char* stdCode, double price, double qty, int flag = 0) override;

	/**
	 * @brief 开空仓交易
	 * @param stdCode 标准合约代码，格式如SSE.600000
	 * @param price 委托价格
	 * @param qty 下单数量
	 * @param flag 下单标志: 0-normal，1-fak，2-fok，默认0
	 * @return 成功提交的订单本地ID
	 * @details 提交开空委托订单，返回生成的订单ID
	 */
	virtual uint32_t	stra_enter_short(const char* stdCode, double price, double qty, int flag = 0) override;

	/**
	 * @brief 平多仓交易
	 * @param stdCode 标准合约代码，格式如SSE.600000
	 * @param price 委托价格
	 * @param qty 下单数量
	 * @param isToday 是否平今仓，SHFE、INE等上海交易所品种专用
	 * @param flag 下单标志: 0-normal，1-fak，2-fok，默认0
	 * @return 成功提交的订单本地ID
	 * @details 提交平多委托订单，返回生成的订单ID
	 */
	virtual uint32_t	stra_exit_long(const char* stdCode, double price, double qty, bool isToday = false, int flag = 0) override;

	/**
	 * @brief 平空仓交易
	 * @param stdCode 标准合约代码，格式如SSE.600000
	 * @param price 委托价格
	 * @param qty 下单数量
	 * @param isToday 是否平今仓，SHFE、INE等上海交易所品种专用
	 * @param flag 下单标志: 0-normal，1-fak，2-fok，默认0
	 * @return 成功提交的订单本地ID
	 * @details 提交平空委托订单，返回生成的订单ID
	 */
	virtual uint32_t	stra_exit_short(const char* stdCode, double price, double qty, bool isToday = false, int flag = 0) override;

	/**
	 * @brief 获取合约基础信息
	 * @param stdCode 标准合约代码
	 * @return 合约基础信息对象指针
	 * @details 获取指定合约的基础信息，包括合约代码、品种、交易所、授权以及其他属性
	 */
	virtual WTSCommodityInfo* stra_get_comminfo(const char* stdCode) override;

	/**
	 * @brief 获取K线切片数据
	 * @param stdCode 标准合约代码
	 * @param period 周期标识符，如m1/m5/d1等
	 * @param count 请求的K线数量
	 * @return K线切片数据对象指针
	 * @details 获取指定合约的历史K线数据，包含指定数量的最新K线
	 */
	virtual WTSKlineSlice* stra_get_bars(const char* stdCode, const char* period, uint32_t count) override;

	/**
	 * @brief 获取Tick切片数据
	 * @param stdCode 标准合约代码
	 * @param count 请求的Tick数量
	 * @return Tick切片数据对象指针
	 * @details 获取指定合约的历史Tick数据，包含指定数量的最新Tick
	 */
	virtual WTSTickSlice* stra_get_ticks(const char* stdCode, uint32_t count) override;

	/**
	 * @brief 获取委托明细切片数据
	 * @param stdCode 标准合约代码
	 * @param count 请求的委托明细数量
	 * @return 委托明细切片数据对象指针
	 * @details 获取指定合约的历史委托明细数据，包含指定数量的最新委托明细
	 */
	virtual WTSOrdDtlSlice*	stra_get_order_detail(const char* stdCode, uint32_t count) override;

	/**
	 * @brief 获取委托队列切片数据
	 * @param stdCode 标准合约代码
	 * @param count 请求的委托队列数量
	 * @return 委托队列切片数据对象指针
	 * @details 获取指定合约的历史委托队列数据，包含指定数量的最新委托队列
	 */
	virtual WTSOrdQueSlice*	stra_get_order_queue(const char* stdCode, uint32_t count) override;

	/**
	 * @brief 获取逐笔成交切片数据
	 * @param stdCode 标准合约代码
	 * @param count 请求的逐笔成交数量
	 * @return 逐笔成交切片数据对象指针
	 * @details 获取指定合约的历史逐笔成交数据，包含指定数量的最新逐笔成交
	 */
	virtual WTSTransSlice*	stra_get_transaction(const char* stdCode, uint32_t count) override;

	/**
	 * @brief 获取最新Tick数据
	 * @param stdCode 标准合约代码
	 * @return 最新的Tick数据对象指针
	 * @details 获取指定合约的最新一笔Tick数据
	 */
	virtual WTSTickData* stra_get_last_tick(const char* stdCode) override;

	/**
	 * @brief 获取指定合约的持仓量
	 * @param stdCode 标准合约代码，格式如SSE.600000
	 * @param bOnlyValid 是否只获取可用持仓（不包含冻结部分）
	 * @param iFlag 持仓方向标记：1-多头，2-空头，3-净头寸（多空对冲后的净持仓）
	 * @return 指定合约的持仓数量
	 * @details 获取指定合约的持仓量，可以指定是否只返回可用持仓以及持仓方向
	 */
	virtual double stra_get_position(const char* stdCode, bool bOnlyValid = false, int32_t iFlag = 3) override;

	/**
	 * @brief 获取本地持仓量
	 * @param stdCode 标准合约代码
	 * @return 指定合约的本地持仓数量
	 * @details 获取本地计算的指定合约持仓量，通常用于与交易所持仓做对比
	 */
	virtual double stra_get_local_position(const char* stdCode) override;

	/**
	 * @brief 枚举指定合约的持仓
	 * @param stdCode 标准合约代码
	 * @return 指定合约的持仓量（通常是作为计数器使用）
	 * @details 枚举指定合约的持仓，主要用于实现策略中的持仓枚举功能
	 */
	virtual double stra_enum_position(const char* stdCode) override;

	/**
	 * @brief 获取未完成委托数量
	 * @param stdCode 标准合约代码
	 * @return 未完成委托数量
	 * @details 获取指定合约的未完成（未成交）委托数量
	 */
	virtual double stra_get_undone(const char* stdCode) override;

	/**
	 * @brief 获取合约当前价格
	 * @param stdCode 标准合约代码
	 * @return 当前价格
	 * @details 获取指定合约的当前最新价格，通常是最新成交价
	 */
	virtual double stra_get_price(const char* stdCode) override;

	/**
	 * @brief 获取当前交易日期
	 * @return 当前交易日期，格式YYYYMMDD
	 * @details 获取当前交易上下文中的日期，一般用于日志和计算
	 */
	virtual uint32_t stra_get_date() override;

	/**
	 * @brief 获取当前交易时间
	 * @return 当前交易时间，格式HHMMSS
	 * @details 获取当前交易上下文中的时间，一般用于限定交易时间范围
	 */
	virtual uint32_t stra_get_time() override;

	/**
	 * @brief 获取当前时间秒数
	 * @return 当前时间的秒数部分
	 * @details 获取当前交易上下文中的秒数部分，用于更精确的时间控制
	 */
	virtual uint32_t stra_get_secs() override;

	/**
	 * @brief 订阅Tick数据
	 * @param stdCode 标准合约代码
	 * @details 订阅指定合约的Tick数据，订阅后可以通过on_tick回调接收数据
	 */
	virtual void stra_sub_ticks(const char* stdCode) override;

	/**
	 * @brief 订阅委托队列数据
	 * @param stdCode 标准合约代码
	 * @details 订阅指定合约的委托队列数据，订阅后可以通过on_order_queue回调接收数据
	 */
	virtual void stra_sub_order_queues(const char* stdCode) override;

	/**
	 * @brief 订阅委托明细数据
	 * @param stdCode 标准合约代码
	 * @details 订阅指定合约的委托明细数据，订阅后可以通过on_order_detail回调接收数据
	 */
	virtual void stra_sub_order_details(const char* stdCode) override;

	/**
	 * @brief 订阅逐笔成交数据
	 * @param stdCode 标准合约代码
	 * @details 订阅指定合约的逐笔成交数据，订阅后可以通过on_transaction回调接收数据
	 */
	virtual void stra_sub_transactions(const char* stdCode) override;

	/**
	 * @brief 输出信息级别日志
	 * @param message 日志消息内容
	 * @details 输出策略的信息级别日志，用于记录普通信息，例如交易状态变化等
	 */
	virtual void stra_log_info(const char* message) override;

	/**
	 * @brief 输出调试级别日志
	 * @param message 日志消息内容
	 * @details 输出策略的调试级别日志，用于记录详细信息，通常在调试时使用
	 */
	virtual void stra_log_debug(const char* message) override;

	/**
	 * @brief 输出错误级别日志
	 * @param message 日志消息内容
	 * @details 输出策略的错误级别日志，用于记录错误信息，例如交易失败等
	 */
	virtual void stra_log_error(const char* message) override;


	//////////////////////////////////////////////////////////////////////////
	/**
	 * @brief 成交回调
	 * @param localid 本地委托ID
	 * @param stdCode 标准合约代码
	 * @param isLong 是否为多头方向
	 * @param offset 开平标记
	 * @param vol 成交数量
	 * @param price 成交价格
	 * @details 当有成交发生时调用此函数，更新持仓信息和相关统计数据
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double vol, double price);

	/**
	 * @brief 委托状态回调
	 * @param localid 本地委托ID
	 * @param stdCode 标准合约代码
	 * @param isLong 是否为多头方向
	 * @param offset 开平标记
	 * @param totalQty 委托总数量
	 * @param leftQty 剩余未成交数量
	 * @param price 委托价格
	 * @param isCanceled 是否已撤单
	 * @details 当委托状态变化时调用此函数，例如送单成功、部分成交、撤单等
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double totalQty, double leftQty, double price, bool isCanceled);

	/**
	 * @brief 通道就绪回调
	 * @details 当交易通道就绪时调用此函数，表示可以开始交易
	 */
	virtual void on_channel_ready();

	/**
	 * @brief 委托结果回调
	 * @param localid 本地委托ID
	 * @param stdCode 标准合约代码
	 * @param bSuccess 委托是否成功
	 * @param message 相关消息或错误原因
	 * @details 当委托下达并收到结果反馈时调用此函数
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message);

public:
	/**
	 * @brief 初始化UFT策略工厂
	 * @param cfg 配置项参数
	 * @return 初始化是否成功
	 * @details 根据提供的配置初始化UFT策略工厂，加载对应的策略模块
	 */
	bool	init_uft_factory(WTSVariant* cfg);

private:
	/**
	 * @brief 任务类型定义
	 * @details 定义一个可执行的任务对象，用于异步任务队列
	 */
	typedef std::function<void()> Task;

	/**
	 * @brief 提交任务到任务队列
	 * @param task 要提交的任务
	 * @details 将任务提交到内部任务队列，等待异步处理
	 */
	void	postTask(Task task);

	/**
	 * @brief 处理任务队列中的任务
	 * @details 处理内部任务队列中的所有任务，按提交顺序依次执行
	 */
	void	procTask();

	/**
	 * @brief 处理指定订单
	 * @param localid 本地订单ID
	 * @return 处理成功返回true，失败返回false
	 * @details 处理指定订单，包括模拟成交、撤单等操作
	 */
	bool	procOrder(uint32_t localid);

	/**
	 * @brief 更新持仓信息
	 * @param stdCode 标准合约代码
	 * @param isLong 是否为多头方向
	 * @param offset 开平仓标记
	 * @param qty 数量
	 * @param price 价格，默认为0.0
	 * @details 根据成交结果更新合约的持仓信息
	 */
	void	update_position(const char* stdCode, bool isLong, uint32_t offset, double qty, double price = 0.0);

	/**
	 * @brief 更新动态收益
	 * @param stdCode 标准合约代码
	 * @param newTick 最新的Tick数据
	 * @details 根据最新的市场数据更新持仓的动态收益
	 */
	void	update_dyn_profit(const char* stdCode, WTSTickData* newTick);

	/**
	 * @brief 输出回测结果
	 * @details 将回测结果输出到指定的文件或控制台
	 */
	void	dump_outputs();

	/**
	 * @brief 记录交易日志
	 * @param stdCode 标准合约代码
	 * @param isLong 是否为多头方向
	 * @param offset 开平仓标记
	 * @param curTime 当前时间
	 * @param price 成交价格
	 * @param qty 成交数量
	 * @param fee 交易手续费
	 * @details 记录交易日志，用于生成交易明细和分析报告
	 */
	void	log_trade(const char* stdCode, bool isLong, uint32_t offset, uint64_t curTime, double price, double qty, double fee);
	/**
	 * @brief 记录平仓日志
	 * @param stdCode 标准合约代码
	 * @param isLong 是否为多头方向
	 * @param openTime 开仓时间
	 * @param openpx 开仓价格
	 * @param closeTime 平仓时间
	 * @param closepx 平仓价格
	 * @param qty 数量
	 * @param profit 当次平仓盈亏
	 * @param maxprofit 最大浮盈
	 * @param maxloss 最大浮亏
	 * @param totalprofit 总盈亏
	 * @details 记录平仓操作相关数据，用于生成交易报告和回测分析
	 */
	void	log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty,
		double profit, double maxprofit, double maxloss, double totalprofit);

private:
	/**
	 * @brief 历史数据回放器指针
	 * @details 指向历史数据回放器的指针，用于获取回测所需的历史行情数据
	 */
	HisDataReplayer*	_replayer;

	/**
	 * @brief 是否使用最新价格标志
	 * @details 控制是否使用最新价格进行回测撮合，true表示使用最新价格，false表示使用其他价格（如开盘价或收盘价）
	 */
	bool			_use_newpx;

	/**
	 * @brief 错误率设置
	 * @details 模拟交易的错误率，用于模拟现实交易中可能出现的错误情况，单位为万分之一
	 */
	uint32_t		_error_rate;

	/**
	 * @brief 当前tick撮合标志
	 * @details 是否在当前tick数据到达时立即进行撮合，true表示立即撮合，false表示延迟撮合
	 */
	bool			_match_this_tick;

	/**
	 * @brief 价格映射类型定义
	 * @details 合约代码到价格的映射，用于快速查找特定合约的价格
	 */
	typedef wt_hashmap<std::string, double> PriceMap;

	/**
	 * @brief 价格映射表
	 * @details 存储各个合约的当前价格，键为合约代码，值为对应价格
	 */
	PriceMap		_price_map;

	/**
	 * @brief 策略工厂信息结构体
	 * @details 用于存储UFT策略工厂的相关信息，包括模块路径、DLL句柄、工厂实例和创建/删除函数
	 */
	typedef struct _StraFactInfo
	{
		/**
		 * @brief 策略模块文件路径
		 * @details 存储策略模块的完整文件路径
		 */
		std::string		_module_path;

		/**
		 * @brief 动态链接库句柄
		 * @details 加载的策略动态链接库的句柄
		 */
		DllHandle		_module_inst;

		/**
		 * @brief 策略工厂接口指针
		 * @details 指向UFT策略工厂实例的指针
		 */
		IUftStrategyFact*	_fact;

		/**
		 * @brief 创建策略工厂的函数指针
		 * @details 指向创建UFT策略工厂实例的函数
		 */
		FuncCreateUftStraFact	_creator;

		/**
		 * @brief 删除策略工厂的函数指针
		 * @details 指向删除UFT策略工厂实例的函数
		 */
		FuncDeleteUftStraFact	_remover;

		/**
		 * @brief 构造函数
		 * @details 初始化策略工厂信息结构体的成员变量
		 */
		_StraFactInfo()
		{
			_module_inst = NULL;
			_fact = NULL;
		}

		/**
		 * @brief 析构函数
		 * @details 释放策略工厂实例，使用_remover函数删除工厂实例
		 */
		~_StraFactInfo()
		{
			if (_fact)
				_remover(_fact);
		}
	} StraFactInfo;

	/**
	 * @brief UFT策略工厂实例
	 * @details 存储当前加载的UFT策略工厂的信息和实例
	 */
	StraFactInfo	_factory;

	/**
	 * @brief UFT策略实例指针
	 * @details 指向当前正在进行回测的UFT策略实例
	 */
	UftStrategy*	_strategy;

	//StdThreadPtr		_thrd;
	/**
	 * @brief 线程互斥锁
	 * @details 用于保护对共享数据的并发访问，确保线程安全
	 */
	StdUniqueMutex		_mtx;

	/**
	 * @brief 任务队列
	 * @details 存储待处理的任务列表，由postTask方法添加任务，procTask方法处理任务
	 */
	std::queue<Task>	_tasks;
	//bool				_stopped;

	/**
	 * @brief 递归互斥锁
	 * @details 用于保护对控制数据的并发访问，支持递归锁定，同一线程可以多次获取该锁
	 */
	StdRecurMutex		_mtx_control;

	/**
	 * @brief 订单信息结构体
	 * @details 存储订单的基本信息，包括方向、合约代码、数量等
	 */
	typedef struct _OrderInfo
	{
		/**
		 * @brief 是否为多头方向
		 * @details true表示多头，false表示空头
		 */
		bool	_isLong;

		/**
		 * @brief 合约代码
		 * @details 订单对应的合约代码，限制为32字节
		 */
		char	_code[32];

		/**
		 * @brief 委托价格
		 * @details 订单的委托价格
		 */
		double	_price;

		/**
		 * @brief 委托总数量
		 * @details 订单的总委托数量
		 */
		double	_total;

		/**
		 * @brief 剩余未成交数量
		 * @details 订单的剩余未成交数量
		 */
		double	_left;
		
		/**
		 * @brief 开平标记
		 * @details 订单的开平标记：0-开仓，1-平仓，2-平今仓
		 */
		uint32_t	_offset;

		/**
		 * @brief 本地订单ID
		 * @details 订单的本地唯一标识符
		 */
		uint32_t	_localid;

		/**
		 * @brief 构造函数
		 * @details 初始化订单信息结构体，将所有成员变量置零
		 */
		_OrderInfo()
		{
			memset(this, 0, sizeof(_OrderInfo));
		}

	} OrderInfo;

	/**
	 * @brief 订单映射类型定义
	 * @details 定义订单ID到订单信息的映射关系，用于快速查找和维护订单
	 */
	typedef wt_hashmap<uint32_t, OrderInfo> Orders;

	/**
	 * @brief 订单管理的互斥锁
	 * @details 用于保护对订单数据的并发访问，支持递归锁定
	 */
	StdRecurMutex	_mtx_ords;

	/**
	 * @brief 订单容器
	 * @details 存储所有当前活跃的订单，以本地ID为键
	 */
	Orders			_orders;

	//用户数据
	/**
	 * @brief 字符串映射类型定义
	 * @details 定义键值对映射关系，用于存储用户自定义数据
	 */
	typedef wt_hashmap<std::string, std::string> StringHashMap;

	/**
	 * @brief 用户数据容器
	 * @details 存储策略使用的自定义数据，支持持久化
	 */
	StringHashMap	_user_datas;

	/**
	 * @brief 用户数据修改标记
	 * @details 标记用户数据是否被修改，用于决定是否需要持久化
	 */
	bool			_ud_modified;

	/**
	 * @brief 交易明细信息结构体
	 * @details 存储交易明细的详细信息，包括价格、数量、时间、盈亏等
	 */
	typedef struct _DetailInfo
	{
		/**
		 * @brief 交易价格
		 * @details 成交价格
		 */
		double		_price;

		/**
		 * @brief 交易数量
		 * @details 成交数量
		 */
		double		_volume;

		/**
		 * @brief 开仓时间
		 * @details 开仓时间，整型表示的时间戳
		 */
		uint64_t	_opentime;

		/**
		 * @brief 开仓交易日期
		 * @details 开仓的交易日期，格式YYYYMMDD
		 */
		uint32_t	_opentdate;

		/**
		 * @brief 最大浮盈
		 * @details 持仓过程中的最大浮动盈利
		 */
		double		_max_profit;

		/**
		 * @brief 最大浮亏
		 * @details 持仓过程中的最大浮动亏损
		 */
		double		_max_loss;

		/**
		 * @brief 实际盈亏
		 * @details 平仓后的实际盈亏
		 */
		double		_profit;

		/**
		 * @brief 构造函数
		 * @details 初始化交易明细信息结构体，将所有成员变量置零
		 */
		_DetailInfo()
		{
			memset(this, 0, sizeof(_DetailInfo));
		}
	} DetailInfo;

	/**
	 * @brief 持仓项目结构体
	 * @details 存储持仓的详细信息，包括方向、数量、可用数量、盈亏等
	 */
	typedef struct _PosItem
	{
		/**
		 * @brief 持仓方向
		 * @details true表示多头持仓，false表示空头持仓
		 */
		bool		_long;

		/**
		 * @brief 平仓盈亏
		 * @details 已平仓部分的盈亏
		 */
		double		_closeprofit;

		/**
		 * @brief 浮动盈亏
		 * @details 未平仓部分的浮动盈亏
		 */
		double		_dynprofit;

		/**
		 * @brief 前一日持仓量
		 * @details 前一交易日结算后的持仓量
		 */
		double		_prevol;

		/**
		 * @brief 当日持仓量
		 * @details 当前的持仓量，包括前一日结转和当日新增
		 */
		double		_newvol;

		/**
		 * @brief 前一日可用持仓量
		 * @details 前一交易日结算后的可用持仓量
		 */
		double		_preavail;

		/**
		 * @brief 当日可用持仓量
		 * @details 当前的可用持仓量，即可以用于平仓的数量
		 */
		double		_newavail;

		/**
		 * @brief 持仓明细列表
		 * @details 存储持仓的各个分笔明细信息
		 */
		std::vector<DetailInfo> _details;

		/**
		 * @brief 构造函数
		 * @details 初始化持仓项目结构体，设置默认数值
		 */
		_PosItem()
		{
			_prevol = 0;
			_newvol = 0;
			_preavail = 0;
			_newavail = 0;

			_closeprofit = 0;
			_dynprofit = 0;
		}

		/**
		 * @brief 获取有效（可用）持仓量
		 * @return 总的可用持仓量
		 * @details 返回前一日和当日的所有可用持仓数量之和
		 */
		inline double valid() const { return _preavail + _newavail; }

		/**
		 * @brief 获取总持仓量
		 * @return 总的持仓量
		 * @details 返回前一日和当日的所有持仓数量之和
		 */
		inline double volume() const { return _prevol + _newvol; }

		/**
		 * @brief 获取冻结持仓量
		 * @return 冻结的持仓量
		 * @details 返回被冻结的持仓数量，即总持仓量减去可用持仓量
		 */
		inline double frozen() const { return volume() - valid(); }
	} PosItem;

	/**
	 * @brief 合约持仓信息结构体
	 * @details 存储单个合约的多空两个方向的完整持仓信息
	 */
	typedef struct _PosInfo
	{
		/**
		 * @brief 多头持仓信息
		 * @details 存储多头方向的持仓详细信息
		 */
		PosItem	_long;

		/**
		 * @brief 空头持仓信息
		 * @details 存储空头方向的持仓详细信息
		 */
		PosItem	_short;

		/**
		 * @brief 获取平仓盈亏
		 * @return 多空方向的平仓盈亏总和
		 * @details 返回多头和空头的平仓盈亏之和
		 */
		inline double closeprofit() const{ return _long._closeprofit + _short._closeprofit; }
		/**
		 * @brief 获取浮动盈亏
		 * @return 多空方向的浮动盈亏总和
		 * @details 返回多头和空头的未平仓浮动盈亏之和
		 */
		inline double dynprofit() const { return _long._dynprofit + _short._dynprofit; }
	} PosInfo;

	/**
	 * @brief 持仓映射类型定义
	 * @details 定义合约代码到持仓信息的映射关系，用于快速查找和维护各合约的持仓
	 */
	typedef wt_hashmap<std::string, PosInfo> PositionMap;
	PositionMap		_pos_map;

	std::stringstream	_trade_logs;
	std::stringstream	_close_logs;
	std::stringstream	_fund_logs;
	std::stringstream	_pos_logs;

	typedef struct _StraFundInfo
	{
		double	_total_profit;
		double	_total_dynprofit;
		double	_total_fees;

		_StraFundInfo()
		{
			memset(this, 0, sizeof(_StraFundInfo));
		}
	} StraFundInfo;

	StraFundInfo		_fund_info;

protected:
	uint32_t		_context_id;

	//tick订阅列表
	wt_hashset<std::string> _tick_subs;
};

