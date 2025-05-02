/*!
 * \file ExpHftMocker.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 高频交易策略回测模拟器对外封装实现
 * \details 实现了对外提供的高频交易策略回测模拟器类，可在回测引擎中实现高频交易策略的模拟测试
 */

#include "ExpHftMocker.h"
#include "WtBtRunner.h"

#include "../Share/StrUtil.hpp"

/**
 * @brief 获取全局回测引擎实例
 * @return WtBtRunner& 全局回测引擎实例的引用
 * @details 外部声明的函数，用于获取全局唯一的回测引擎实例
 *          所有的策略模拟器都通过该函数与外部回测引擎进行交互
 */
extern WtBtRunner& getRunner();

/**
 * @brief 构造函数
 * @param replayer 历史数据回放器指针，用于提供回测所需的各类历史数据
 * @param name 策略名称，用于唯一标识策略实例
 * @details 初始化高频交易策略回测模拟器，通过调用父类构造函数来初始化
 *          基类 HftMocker负责大部分策略模拟器的基础功能
 *          ExpHftMocker主要添加了对外部接口的调用和处理
 */
ExpHftMocker::ExpHftMocker(HisDataReplayer* replayer, const char* name)
	: HftMocker(replayer, name)
{

}

/**
 * @brief K线数据回调函数
 * @param stdCode 标准化合约代码
 * @param period K线周期基础单位，如"d"(日线)、"m"(分钟线)
 * @param times 周期倍数，结合period使用，如period=d,times=1表示日线
 * @param newBar 新的K线数据结构
 * @details 当新的K线数据到达时触发此回调
 *          1. 首先检查newBar是否为空，如果为空则直接返回
 *          2. 根据period类型和times值构造实际周期名称，如"d1"或"m5"
 *          3. 调用父类的on_bar函数处理基本逻辑
 *          4. 通过getRunner调用外部回测引擎的接口，将K线数据传递给外部系统
 */
void ExpHftMocker::on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	if (newBar == NULL)
		return;

	std::string realPeriod;
	if (period[0] == 'd')
		realPeriod = StrUtil::printf("%s%u", period, times);
	else
		realPeriod = StrUtil::printf("m%u", times);

	HftMocker::on_bar(stdCode, period, times, newBar);

	getRunner().ctx_on_bar(_context_id, stdCode, realPeriod.c_str(), newBar, ET_HFT);
}

/**
 * @brief 交易通道就绪回调函数
 * @details 当交易通道准备就绪可以进行交易时触发此回调
 *          1. 首先调用父类的on_channel_ready函数处理基本需求
 *          2. 然后通过getRunner调用外部回测引擎的接口，将通道就绪事件通知给外部系统
 *          3. 通道就绪后，策略可以开始发送交易指令
 */
void ExpHftMocker::on_channel_ready()
{
	HftMocker::on_channel_ready();

	getRunner().hft_on_channel_ready(_context_id, "");
}

/**
 * @brief 委托回报回调函数
 * @param localid 本地委托ID，用于唯一标识委托
 * @param stdCode 标准化合约代码
 * @param bSuccess 委托是否成功的标志，true表示成功，false表示失败
 * @param message 委托回报消息，包含成功或失败的详细信息
 * @param userTag 用户自定义标签，用于识别不同的交易目的
 * @details 当委托发出后收到回报时触发此回调
 *          1. 首先调用父类的on_entrust函数处理基本委托回报逻辑
 *          2. 然后通过getRunner调用外部回测引擎的接口，将委托回报事件通知给外部系统
 *          3. 高频交易策略可以根据委托回报结果快速调整后续交易策略
 */
void ExpHftMocker::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message, const char* userTag)
{
	HftMocker::on_entrust(localid, stdCode, bSuccess, message, userTag);

	getRunner().hft_on_entrust(_context_id, localid, stdCode, bSuccess, message, userTag);
}

/**
 * @brief 策略初始化回调函数
 * @details 当策略加载后需要进行初始化时触发此回调
 *          1. 首先调用父类的on_init函数完成基本初始化操作
 *          2. 然后通过getRunner调用外部回测引擎的ctx_on_init接口，将策略初始化事件通知给外部系统
 *          3. 最后调用全局初始化事件函数on_initialize_event，完成整个回测系统的初始化
 *          这是策略生命周期中的第一个回调，用于设置参数、数据订阅等
 */
void ExpHftMocker::on_init()
{
	HftMocker::on_init();

	getRunner().ctx_on_init(_context_id, ET_HFT);

	getRunner().on_initialize_event();
}

/**
 * @brief 交易日开始回调函数
 * @param uDate 交易日日期，格式为YYYYMMDD
 * @details 当每个交易日开始时触发此回调
 *          1. 首先调用父类的on_session_begin函数完成基本交易日开始处理
 *          2. 然后通过getRunner调用外部引擎的ctx_on_session_event接口，将交易日开始事件通知给上下文
 *          3. 最后调用全局交易日事件函数on_session_event，通知整个系统交易日开始
 *          在高频交易中，可以在交易日开始时准备当日策略参数和状态
 */
void ExpHftMocker::on_session_begin(uint32_t uDate)
{
	HftMocker::on_session_begin(uDate);

	getRunner().ctx_on_session_event(_context_id, uDate, true, ET_HFT);
	getRunner().on_session_event(uDate, true);
}

/**
 * @brief 交易日结束回调函数
 * @param uDate 交易日日期，格式为YYYYMMDD
 * @details 当每个交易日结束时触发此回调
 *          1. 首先调用父类的on_session_end函数完成基本交易日结束处理
 *          2. 然后通过getRunner调用外部引擎的ctx_on_session_event接口，将交易日结束事件通知给上下文
 *          3. 最后调用全局交易日事件函数on_session_event，通知整个系统交易日结束
 *          在高频交易中，可以在交易日结束时进行当日交易统计、清空所有持仓等操作
 */
void ExpHftMocker::on_session_end(uint32_t uDate)
{
	HftMocker::on_session_end(uDate);

	getRunner().ctx_on_session_event(_context_id, uDate, false, ET_HFT);
	getRunner().on_session_event(uDate, false);
}

/**
 * @brief 委托状态更新回调函数
 * @param localid 本地委托ID，用于唯一标识委托
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入委托，true表示买入，false表示卖出
 * @param totalQty 委托总数量
 * @param leftQty 剩余未成交数量
 * @param price 委托价格
 * @param isCanceled 是否已撤销
 * @param userTag 用户自定义标签
 * @details 当委托状态发生变化时触发此回调
 *          1. 首先调用父类的on_order函数完成基本委托状态处理
 *          2. 然后通过getRunner调用外部引擎的hft_on_order接口，将委托状态更新事件通知给外部系统
 *          在高频交易中，及时跟踪每个委托的状态变化非常重要
 *          包括委托创建、部分成交、全部成交或撤销等状态
 */
void ExpHftMocker::on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag)
{
	HftMocker::on_order(localid, stdCode, isBuy, totalQty, leftQty, price, isCanceled, userTag);

	getRunner().hft_on_order(_context_id, localid, stdCode, isBuy, totalQty, leftQty, price, isCanceled, userTag);
}

/**
 * @brief 实时行情数据更新回调函数
 * @param stdCode 标准化合约代码
 * @param newTick 新的Tick行情数据
 * @details 当收到新的Tick数据时触发此回调
 *          1. 首先检查合约是否已订阅(_tick_subs映射中是否存在)，如果未订阅则直接返回
 *          2. 然后调用父类的on_tick_updated函数完成基本的Tick数据处理
 *          3. 最后通过getRunner调用外部引擎的ctx_on_tick接口，将Tick数据通知给外部系统
 *          Tick数据是高频交易策略的主要数据来源，包含最新成交价、买卖相关信息等
 */
void ExpHftMocker::on_tick_updated(const char* stdCode, WTSTickData* newTick)
{
	auto it = _tick_subs.find(stdCode);
	if (it == _tick_subs.end())
		return;

	HftMocker::on_tick_updated(stdCode, newTick);
	getRunner().ctx_on_tick(_context_id, stdCode, newTick, ET_HFT);
}

/**
 * @brief 委托队列数据更新回调函数
 * @param stdCode 标准化合约代码
 * @param newOrdQue 新的委托队列数据
 * @details 当收到新的委托队列数据时触发此回调
 *          直接通过getRunner调用外部引擎的hft_on_order_queue接口，将委托队列数据通知给外部系统
 *          委托队列数据包含市场上不同价位的委托排队情况，是分析市场深度的重要数据
 *          注意这里与其他函数不同，没有调用父类方法，直接转发事件
 */
void ExpHftMocker::on_ordque_updated(const char* stdCode, WTSOrdQueData* newOrdQue)
{
	getRunner().hft_on_order_queue(_context_id, stdCode, newOrdQue);
}

/**
 * @brief 委托明细数据更新回调函数
 * @param stdCode 标准化合约代码
 * @param newOrdDtl 新的委托明细数据
 * @details 当收到新的委托明细数据时触发此回调
 *          直接通过getRunner调用外部引擎的hft_on_order_detail接口，将委托明细数据通知给外部系统
 *          委托明细数据包含市场上详细的委托信息，如委托类型、委托时间等
 *          注意这里与其他函数不同，没有调用父类方法，直接转发事件
 */
void ExpHftMocker::on_orddtl_updated(const char* stdCode, WTSOrdDtlData* newOrdDtl)
{
	getRunner().hft_on_order_detail(_context_id, stdCode, newOrdDtl);
}

/**
 * @brief 逐笔成交数据更新回调函数
 * @param stdCode 标准化合约代码
 * @param newTrans 新的逐笔成交数据
 * @details 当收到新的逐笔成交数据时触发此回调
 *          直接通过getRunner调用外部引擎的hft_on_transaction接口，将逐笔成交数据通知给外部系统
 *          逐笔成交数据包含市场上具体的逐笔成交信息，如价格、数量、方向等
 *          在高频交易中，逐笔成交数据是分析市场微观结构的重要数据来源
 *          注意这里与其他函数不同，没有调用父类方法，直接转发事件
 */
void ExpHftMocker::on_trans_updated(const char* stdCode, WTSTransData* newTrans)
{
	getRunner().hft_on_transaction(_context_id, stdCode, newTrans);
}

/**
 * @brief 委托成交回调函数
 * @param localid 本地委托ID，用于唯一标识委托
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入，true表示买入，false表示卖出
 * @param vol 成交数量
 * @param price 成交价格
 * @param userTag 用户自定义标签，用于识别不同的交易目的
 * @details 当策略发出的委托成交时触发此回调
 *          1. 首先调用父类的on_trade函数完成基本成交处理逻辑
 *          2. 然后通过getRunner调用外部引擎的hft_on_trade接口，将成交事件通知给外部系统
 *          在高频交易中，成交回调是策略获取自身交易状态的关键信息来源
 *          注意这与on_trans_updated不同，这里只处理策略自身发出的委托的成交情况
 */
void ExpHftMocker::on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price, const char* userTag)
{
	HftMocker::on_trade(localid, stdCode, isBuy, vol, price, userTag);

	getRunner().hft_on_trade(_context_id, localid, stdCode, isBuy, vol, price, userTag);
}

/**
 * @brief 回测结束回调函数
 * @details 当整个回测过程结束时触发此回调
 *          直接通过getRunner调用外部引擎的on_backtest_end接口，通知外部系统回测结束
 *          这里与其他函数不同，没有调用父类的相应函数，直接转发事件
 *          在回测结束时，可以进行策略结果汇总、统计分析和资源清理等操作
 */
void ExpHftMocker::on_bactest_end()
{
	getRunner().on_backtest_end();
}