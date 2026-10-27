/**
 * @file HftStraContext.h
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief 高频交易策略上下文定义
 * @details 定义了高频交易策略的上下文环境，提供策略与交易引擎之间的交互接口，
 *          处理各类行情和交易事件并转发到策略实例
 */
#pragma once
#include "HftStraBaseCtx.h"


USING_NS_WTP;

class HftStrategy;

/**
 * @brief 高频交易策略上下文类
 * @details 继承自HftStraBaseCtx基础类，负责连接交易引擎和具体的策略实现，
 *          接收并处理各类行情和交易事件，将其转发到策略实例进行处理
 */
class HftStraContext : public HftStraBaseCtx
{
public:
	/**
	 * @brief 构造函数
	 * @param engine 高频交易引擎指针
	 * @param name 策略名称
	 * @param bAgent 是否做代理
	 * @param slippage 滑点设置
	 */
	HftStraContext(WtHftEngine* engine, const char* name, bool bAgent, int32_t slippage);

	/**
	 * @brief 析构函数
	 */
	virtual ~HftStraContext();

	/**
	 * @brief 设置策略实例
	 * @param stra 策略实例指针
	 */
	void set_strategy(HftStrategy* stra){ _strategy = stra; }

	/**
	 * @brief 获取策略实例
	 * @return 策略实例指针
	 */
	HftStrategy* get_stragety() { return _strategy; }

public:
	/**
	 * @brief 策略初始化
	 * @details 被引擎调用，直接转发到策略实例的初始化函数
	 */
	virtual void on_init() override;

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
	 * @brief 行情Tick数据回调
	 * @param code 合约代码
	 * @param newTick Tick数据
	 */
	virtual void on_tick(const char* code, WTSTickData* newTick) override;

	/**
	 * @brief 委托队列数据回调
	 * @param stdCode 标准合约代码
	 * @param newOrdQue 委托队列数据
	 */
	virtual void on_order_queue(const char* stdCode, WTSOrdQueData* newOrdQue) override;

	/**
	 * @brief 委托明细数据回调
	 * @param stdCode 标准合约代码
	 * @param newOrdDtl 委托明细数据
	 */
	virtual void on_order_detail(const char* stdCode, WTSOrdDtlData* newOrdDtl) override;

	/**
	 * @brief 逐笔成交数据回调
	 * @param stdCode 标准合约代码
	 * @param newTrans 逐笔成交数据
	 */
	virtual void on_transaction(const char* stdCode, WTSTransData* newTrans) override;

	/**
	 * @brief K线数据回调
	 * @param code 合约代码
	 * @param period 周期标识
	 * @param times 周期倍数
	 * @param newBar K线数据
	 */
	virtual void on_bar(const char* code, const char* period, uint32_t times, WTSBarStruct* newBar) override;

	/**
	 * @brief 成交回报回调
	 * @param localid 本地委托ID
	 * @param stdCode 标准合约代码
	 * @param isBuy 是否买入
	 * @param vol 成交数量
	 * @param price 成交价格
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override;

	/**
	 * @brief 委托回报回调
	 * @param localid 本地委托ID
	 * @param stdCode 标准合约代码
	 * @param isBuy 是否买入
	 * @param totalQty 总委托量
	 * @param leftQty 剩余委托量
	 * @param price 委托价格
	 * @param isCanceled 是否已撤销
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled = false) override;

	/**
	 * @brief 交易通道就绪回调
	 * @details 当交易通道就绪时被触发
	 */
	virtual void on_channel_ready() override;

	/**
	 * @brief 交易通道断开回调
	 * @details 当交易通道断开时被触发
	 */
	virtual void on_channel_lost() override;

	/**
	 * @brief 委托提交回报回调
	 * @param localid 本地委托ID
	 * @param stdCode 标准合约代码
	 * @param bSuccess 是否成功
	 * @param message 错误信息（当失败时）
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override;

	/**
	 * @brief 持仓回报回调
	 * @param stdCode 标准合约代码
	 * @param isLong 是否多仓
	 * @param prevol 前储持仓量
	 * @param preavail 前储可用量
	 * @param newvol 新持仓量
	 * @param newavail 新可用量
	 * @param tradingday 交易日
	 */
	virtual void on_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday) override;


private:
	HftStrategy*		_strategy;		//!< 策略实例指针
};

