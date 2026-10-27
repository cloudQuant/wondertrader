/*!
 * \file ExpHftMocker.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 高频交易策略回测模拟器对外封装定义
 * \details 定义了对外提供的高频交易策略回测模拟器类，用于封装WtBtCore中的HftMocker，
 *          便于在回测引擎中实现高频交易策略的模拟测试。
 */
#pragma once
#include "../WtBtCore/HftMocker.h"

/**
 * @brief 高频交易策略回测模拟器对外封装类
 * @details 继承自HftMocker，用于在回测系统中模拟高频交易策略的行为和表现
 *          重写了父类的各个虚函数，实现对不同事件的响应处理
 *          该类主要用于对外接口封装，方便与其他系统集成
 *          包含处理行情数据、委托事件、成交事件、交易日事件等高频交易特有的功能
 */
class ExpHftMocker : public HftMocker
{
public:
	/**
	 * @brief 构造函数
	 * @param replayer 历史数据回放器指针
	 * @param name 策略名称
	 * @details 创建高频交易策略回测模拟器实例，并传入相关参数
	 */
	ExpHftMocker(HisDataReplayer* replayer, const char* name);
	
	/**
	 * @brief 析构函数
	 * @details 清理高频交易策略回测模拟器资源
	 *          实现为空函数体，因为父类的析构函数会处理资源释放
	 */
	virtual ~ExpHftMocker(){}

	/**
	 * @brief K线数据回调函数
	 * @param stdCode 标准化合约代码
	 * @param period K线周期，如"m1"、"d1"等
	 * @param times 重复次数
	 * @param newBar 新的K线数据
	 * @details 当收到新的K线数据时触发该回调
	 *          用于处理K线周期结束时的策略计算和信号生成
	 *          在高频交易中，通常也需要结合K线进行分析
	 */
	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;

	/**
	 * @brief 交易通道就绪回调函数
	 * @details 当交易通道就绪（可交易）时触发该回调
	 *          交易通道就绪后，策略可以开始发送交易指令
	 *          高频交易中特有的事件，用于确保交易请求只在通道就绪后发送
	 */
	virtual void on_channel_ready() override;

	/**
	 * @brief 委托回报回调函数
	 * @param localid 本地委托ID
	 * @param stdCode 标准化合约代码
	 * @param bSuccess 是否成功，true表示委托成功，false表示委托失败
	 * @param message 委托回报消息，如错误原因等
	 * @param userTag 用户自定义标签，用于识别不同的交易标签
	 * @details 委托发出后的回报处理
	 *          在高频交易中，需要快速响应委托回报，特别是失败的委托
	 *          可以根据委托结果调整后续策略行为
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message, const char* userTag) override;

	/**
	 * @brief 策略初始化回调函数
	 * @details 策略被创建后的初始化调用
	 *          在这里可以进行策略参数设置、数据订阅、变量初始化等操作
	 *          这是策略生命周期中的第一个回调函数
	 */
	virtual void on_init() override;


	/**
	 * @brief 交易日开始回调函数
	 * @param uDate 交易日日期，格式为YYYYMMDD
	 * @details 每个交易日开始时触发此回调
	 *          可以在此函数中进行交易日开始前的初始化工作
	 *          例如重置交易状态、准备每日数据等
	 */
	virtual void on_session_begin(uint32_t uDate) override;

	/**
	 * @brief 交易日结束回调函数
	 * @param uDate 交易日日期，格式为YYYYMMDD
	 * @details 每个交易日结束时触发此回调
	 *          可以在此函数中进行交易日结束后的清理工作
	 *          例如结算当日交易、记录统计数据、平仓操作等
	 */
	virtual void on_session_end(uint32_t uDate) override;

	/**
	 * @brief 回测结束回调函数
	 * @details 整个回测过程结束时触发此回调
	 *          可以在此函数中进行回测结果汇总、资源释放等操作
	 *          这是策略生命周期中的最后一个回调函数
	 */
	virtual void on_bactest_end() override;


	/**
	 * @brief 委托记录回调函数
	 * @param localid 本地委托ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否为买入，true表示买入，false表示卖出
	 * @param totalQty 委托总数量
	 * @param leftQty 剩余数量（未成交数量）
	 * @param price 委托价格
	 * @param isCanceled 是否已撤销
	 * @param userTag 用户自定义标签
	 * @details 当委托状态变化时触发此回调
	 *          包括委托创建、部分成交、全部成交、撤单等状态变化
	 *          高频交易中需要知道委托状态变化以做出相应策略调整
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag) override;


	/**
	 * @brief Tick数据更新回调函数
	 * @param stdCode 标准化合约代码
	 * @param newTick 新的Tick数据
	 * @details 当收到新的通用Tick数据时触发该回调
	 *          Tick数据更新是高频交易策略中最重要的事件
	 *          包含市场最新成交价、买一卖一、买卖排队深度等信息
	 *          高频策略通常会基于Tick数据生成交易信号
	 */
	virtual void on_tick_updated(const char* stdCode, WTSTickData* newTick) override;


	/**
	 * @brief 委托队列数据更新回调函数
	 * @param stdCode 标准化合约代码
	 * @param newOrdQue 新的委托队列数据
	 * @details 当收到新的委托队列数据时触发该回调
	 *          委托队列数据包含市场上各个价位的委托排队情况
	 *          高频策略可以基于委托队列数据分析市场深度和流动性
	 *          通常用于监控市场微观结构和对大单的反应
	 */
	virtual void on_ordque_updated(const char* stdCode, WTSOrdQueData* newOrdQue) override;

	/**
	 * @brief 委托明细数据更新回调函数
	 * @param stdCode 标准化合约代码
	 * @param newOrdDtl 新的委托明细数据
	 * @details 当收到新的委托明细数据时触发该回调
	 *          委托明细数据包含市场上全部或部分委托的详细信息
	 *          高频策略可以基于委托明细分析市场参与者的行为特征
	 *          对于监控市场操纵和高频交易年线非常有价值
	 */
	virtual void on_orddtl_updated(const char* stdCode, WTSOrdDtlData* newOrdDtl) override;


	/**
	 * @brief 成交明细数据更新回调函数
	 * @param stdCode 标准化合约代码
	 * @param newTrans 新的成交明细数据
	 * @details 当收到新的成交明细数据时触发该回调
	 *          成交明细数据包含市场上每笔成交的详细信息，如价格、数量、方向等
	 *          高频策略可以基于成交明细分析市场的变化趋势和流动性
	 *          对做市吧源分析和计算市场冲击指标非常重要
	 */
	virtual void on_trans_updated(const char* stdCode, WTSTransData* newTrans) override;

	/**
	 * @brief 交易成交回调函数
	 * @param localid 本地委托ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否为买入，true表示买入，false表示卖出
	 * @param vol 成交数量
	 * @param price 成交价格
	 * @param userTag 用户自定义标签
	 * @details 当策略发出的委托成交时触发该回调
	 *          与市场成交数据不同，此函数只处理策略自身发出的委托的成交情况
	 *          在高频策略中用于跟踪自身交易成交情况并根据成交结果调整后续策略
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price, const char* userTag) override;

};

