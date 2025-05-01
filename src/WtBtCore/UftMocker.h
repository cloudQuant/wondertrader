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
	HisDataReplayer*	_replayer;

	bool			_use_newpx;
	uint32_t		_error_rate;
	bool			_match_this_tick;	//是否在当前tick撮合

	typedef wt_hashmap<std::string, double> PriceMap;
	PriceMap		_price_map;


	typedef struct _StraFactInfo
	{
		std::string		_module_path;
		DllHandle		_module_inst;
		IUftStrategyFact*	_fact;
		FuncCreateUftStraFact	_creator;
		FuncDeleteUftStraFact	_remover;

		_StraFactInfo()
		{
			_module_inst = NULL;
			_fact = NULL;
		}

		~_StraFactInfo()
		{
			if (_fact)
				_remover(_fact);
		}
	} StraFactInfo;
	StraFactInfo	_factory;

	UftStrategy*	_strategy;

	//StdThreadPtr		_thrd;
	StdUniqueMutex		_mtx;
	std::queue<Task>	_tasks;
	//bool				_stopped;

	StdRecurMutex		_mtx_control;

	typedef struct _OrderInfo
	{
		bool	_isLong;
		char	_code[32];
		double	_price;
		double	_total;
		double	_left;
		
		uint32_t	_offset;
		uint32_t	_localid;

		_OrderInfo()
		{
			memset(this, 0, sizeof(_OrderInfo));
		}

	} OrderInfo;
	typedef wt_hashmap<uint32_t, OrderInfo> Orders;
	StdRecurMutex	_mtx_ords;
	Orders			_orders;

	//用户数据
	typedef wt_hashmap<std::string, std::string> StringHashMap;
	StringHashMap	_user_datas;
	bool			_ud_modified;

	typedef struct _DetailInfo
	{
		double		_price;
		double		_volume;
		uint64_t	_opentime;
		uint32_t	_opentdate;
		double		_max_profit;
		double		_max_loss;
		double		_profit;

		_DetailInfo()
		{
			memset(this, 0, sizeof(_DetailInfo));
		}
	} DetailInfo;

	typedef struct _PosItem
	{
		bool		_long;
		double		_closeprofit;
		double		_dynprofit;

		double		_prevol;
		double		_newvol;
		double		_preavail;
		double		_newavail;

		std::vector<DetailInfo> _details;

		_PosItem()
		{
			_prevol = 0;
			_newvol = 0;
			_preavail = 0;
			_newavail = 0;

			_closeprofit = 0;
			_dynprofit = 0;
		}

		inline double valid() const { return _preavail + _newavail; }
		inline double volume() const { return _prevol + _newvol; }
		inline double frozen() const { return volume() - valid(); }
	} PosItem;

	typedef struct _PosInfo
	{
		PosItem	_long;
		PosItem	_short;

		inline double closeprofit() const{ return _long._closeprofit + _short._closeprofit; }
		inline double dynprofit() const { return _long._dynprofit + _short._dynprofit; }
	} PosInfo;
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

