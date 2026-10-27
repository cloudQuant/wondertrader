/**
 * @file HftStraContext.cpp
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 *
 * @brief 高频交易策略上下文实现
 * @details 实现了高频交易策略的上下文环境，处理行情和交易事件并转发到策略实例
 */
#include "HftStraContext.h"
#include "../Includes/HftStrategyDefs.h"


/**
 * @brief 构造函数
 * @details 初始化高频交易策略上下文
 * @param engine 高频交易引擎指针
 * @param name 策略名称
 * @param bAgent 是否做代理
 * @param slippage 滑点设置
 */
HftStraContext::HftStraContext(WtHftEngine* engine, const char* name, bool bAgent, int32_t slippage)
	: HftStraBaseCtx(engine, name, bAgent, slippage)
	, _strategy(NULL)
{
	// 基类已完成大部分初始化工作
}


/**
 * @brief 析构函数
 * @details 清理高频交易策略上下文资源
 */
HftStraContext::~HftStraContext()
{
	// 基类已完成大部分清理工作
}

/**
 * @brief 策略初始化回调
 * @details 在策略实例初始化时被调用，首先调用基类的初始化函数，然后调用策略实例的初始化函数
 */
void HftStraContext::on_init()
{
	// 首先调用基类的初始化函数
	HftStraBaseCtx::on_init();

	// 然后调用策略实例的初始化函数
	if (_strategy)
		_strategy->on_init(this);
}

/**
 * @brief 交易会话开始回调
 * @details 在交易日开始时被调用，首先调用基类的会话开始函数，然后调用策略实例的会话开始函数
 * @param uTDate 交易日期，格式为YYYYMMDD
 */
void HftStraContext::on_session_begin(uint32_t uTDate)
{
	// 首先调用基类的会话开始函数
	HftStraBaseCtx::on_session_begin(uTDate);

	// 然后调用策略实例的会话开始函数
	if (_strategy)
		_strategy->on_session_begin(this, uTDate);
}

/**
 * @brief 交易会话结束回调
 * @details 在交易日结束时被调用，首先调用策略实例的会话结束函数，然后调用基类的会话结束函数
 * @param uTDate 交易日期，格式为YYYYMMDD
 */
void HftStraContext::on_session_end(uint32_t uTDate)
{
	// 首先调用策略实例的会话结束函数
	if (_strategy)
		_strategy->on_session_end(this, uTDate);

	// 然后调用基类的会话结束函数
	HftStraBaseCtx::on_session_end(uTDate);
}

/**
 * @brief 行情Tick数据回调
 * @details 当新的Tick数据到来时被调用，更新动态持仓盈亏并查找订阅记录，如果该合约订阅了Tick，则调用策略实例的Tick回调
 * @param stdCode 标准合约代码
 * @param newTick 新的Tick数据
 */
void HftStraContext::on_tick(const char* stdCode, WTSTickData* newTick)
{
	// 更新动态盈亏计算
	update_dyn_profit(stdCode, newTick);

	// 查找该合约是否在Tick订阅列表中
	auto it = _tick_subs.find(stdCode);
	if (it != _tick_subs.end())
	{
		// 如果订阅了且策略实例存在，则调用策略实例的Tick回调
		if (_strategy)
			_strategy->on_tick(this, stdCode, newTick);
	}

	// 调用基类的Tick处理函数
	HftStraBaseCtx::on_tick(stdCode, newTick);
}

/**
 * @brief 委托队列数据回调
 * @details 当新的委托队列数据到来时被调用，转发给策略实例的委托队列回调函数
 * @param stdCode 标准合约代码
 * @param newOrdQue 新的委托队列数据
 */
void HftStraContext::on_order_queue(const char* stdCode, WTSOrdQueData* newOrdQue)
{
	// 调用策略实例的委托队列回调
	if (_strategy)
		_strategy->on_order_queue(this, stdCode, newOrdQue);

	// 调用基类的委托队列处理函数
	HftStraBaseCtx::on_order_queue(stdCode, newOrdQue);
}

/**
 * @brief 委托明细数据回调
 * @details 当新的委托明细数据到来时被调用，转发给策略实例的委托明细回调函数
 * @param stdCode 标准合约代码
 * @param newOrdDtl 新的委托明细数据
 */
void HftStraContext::on_order_detail(const char* stdCode, WTSOrdDtlData* newOrdDtl)
{
	// 调用策略实例的委托明细回调
	if (_strategy)
		_strategy->on_order_detail(this, stdCode, newOrdDtl);

	// 调用基类的委托明细处理函数
	HftStraBaseCtx::on_order_detail(stdCode, newOrdDtl);
}

/**
 * @brief 逐笔成交数据回调
 * @details 当新的逐笔成交数据到来时被调用，转发给策略实例的逐笔成交回调函数
 * @param stdCode 标准合约代码
 * @param newTrans 新的逐笔成交数据
 */
void HftStraContext::on_transaction(const char* stdCode, WTSTransData* newTrans)
{
	// 调用策略实例的逐笔成交回调
	if (_strategy)
		_strategy->on_transaction(this, stdCode, newTrans);

	// 调用基类的逐笔成交处理函数
	HftStraBaseCtx::on_transaction(stdCode, newTrans);
}

/**
 * @brief K线数据回调
 * @details 当新的K线数据到来时被调用，转发给策略实例的K线回调函数
 * @param code 合约代码
 * @param period 周期标识（如m1、m5等）
 * @param times 周期倍数
 * @param newBar 新的K线数据
 */
void HftStraContext::on_bar(const char* code, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	// 调用策略实例的K线回调
	if (_strategy)
		_strategy->on_bar(this, code, period, times, newBar);

	// 调用基类的K线处理函数
	HftStraBaseCtx::on_bar(code, period, times, newBar);
}

/**
 * @brief 成交回报回调
 * @details 当收到成交回报时被调用，将标准合约代码转换为内部代码并转发给策略实例
 * @param localid 本地委托ID
 * @param stdCode 标准合约代码
 * @param isBuy 是否买入
 * @param vol 成交数量
 * @param price 成交价格
 */
void HftStraContext::on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
	// 获取内部合约代码
	const char* innerCode = get_inner_code(stdCode);
	// 调用策略实例的成交回调，传入订单标签
	if (_strategy)
		_strategy->on_trade(this, localid, innerCode, isBuy, vol, price, getOrderTag(localid));

	// 调用基类的成交处理函数
	HftStraBaseCtx::on_trade(localid, innerCode, isBuy, vol, price);
}

/**
 * @brief 委托回报回调
 * @details 当收到委托状态变化回报时被调用，将标准合约代码转换为内部代码并转发给策略实例
 * @param localid 本地委托ID
 * @param stdCode 标准合约代码
 * @param isBuy 是否买入
 * @param totalQty 总委托量
 * @param leftQty 剩余委托量
 * @param price 委托价格
 * @param isCanceled 是否已撤销
 */
void HftStraContext::on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled /* = false */)
{
	// 获取内部合约代码
	const char* innerCode = get_inner_code(stdCode);
	// 调用策略实例的委托回调，传入订单标签
	if (_strategy)
		_strategy->on_order(this, localid, innerCode, isBuy, totalQty, leftQty, price, isCanceled, getOrderTag(localid));

	// 调用基类的委托处理函数
	HftStraBaseCtx::on_order(localid, innerCode, isBuy, totalQty, leftQty, price, isCanceled);
}

/**
 * @brief 持仓回报回调
 * @details 当收到持仓变化通知时被调用，直接转发给策略实例
 * @param stdCode 标准合约代码
 * @param isLong 是否多仓
 * @param prevol 前一日持仓量
 * @param preavail 前一日可用量
 * @param newvol 新的持仓量
 * @param newavail 新的可用量
 * @param tradingday 交易日期
 */
void HftStraContext::on_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday)
{
	// 只调用策略实例的持仓回调，此处没有调用基类的处理函数
	if (_strategy)
		_strategy->on_position(this, stdCode, isLong, prevol, preavail, newvol, newavail);
}

/**
 * @brief 交易通道就绪回调
 * @details 当交易通道就绪后被调用，转发给策略实例
 */
void HftStraContext::on_channel_ready()
{
	// 调用策略实例的交易通道就绪回调
	if (_strategy)
		_strategy->on_channel_ready(this);

	// 调用基类的交易通道就绪处理函数
	HftStraBaseCtx::on_channel_ready();
}

/**
 * @brief 交易通道断开回调
 * @details 当交易通道断开后被调用，转发给策略实例
 */
void HftStraContext::on_channel_lost()
{
	// 调用策略实例的交易通道断开回调
	if (_strategy)
		_strategy->on_channel_lost(this);

	// 调用基类的交易通道断开处理函数
	HftStraBaseCtx::on_channel_lost();
}

/**
 * @brief 委托提交回报回调
 * @details 当收到委托提交结果回报时被调用，转发给策略实例
 * @param localid 本地委托ID
 * @param stdCode 标准合约代码
 * @param bSuccess 是否提交成功
 * @param message 提交回报消息，失败时为错误信息
 */
void HftStraContext::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message)
{
	// 调用策略实例的委托提交回调，传入订单标签
	if (_strategy)
		_strategy->on_entrust(localid, bSuccess, message, getOrderTag(localid));

	// 调用基类的委托提交处理函数，将标准合约转换为内部合约代码
	HftStraBaseCtx::on_entrust(localid, get_inner_code(stdCode), bSuccess, message);
}