/*!
 * \file WtExecuter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * \brief 本地执行器头文件
 * \details 定义了本地执行器类，用于处理交易指令的执行、仓位管理和交易回报等
 */
#pragma once
#include "ITrdNotifySink.h"
#include "IExecCommand.h"
#include "WtExecuterFactory.h"
#include "../Includes/ExecuteDefs.h"
#include "../Share/threadpool.hpp"
#include "../Share/SpinMutex.hpp"

NS_WTP_BEGIN
class WTSVariant;
class IDataManager;
class TraderAdapter;
class IHotMgr;

/**
 * @brief 本地执行器类
 * @details 实现了交易指令的执行、仓位管理和交易回报处理功能，继承自ExecuteContext、ITrdNotifySink和IExecCommand
 */
class WtLocalExecuter : public ExecuteContext,
	public ITrdNotifySink, public IExecCommand
{
public:
	/**
	 * @brief 构造函数
	 * @param factory 执行器工厂指针
	 * @param name 执行器名称
	 * @param dataMgr 数据管理器指针
	 */
	WtLocalExecuter(WtExecuterFactory* factory, const char* name, IDataManager* dataMgr);

	/**
	 * @brief 析构函数
	 * @details 等待线程池中的任务完成
	 */
	virtual ~WtLocalExecuter();

public:
	/**
	 * @brief 初始化执行器
	 * @param params 初始化参数
	 * @return 初始化是否成功
	 * @details 设置执行器的参数，包括放大倍数、严格同步标志、线程池大小、自动清理策略等
	 */
	bool init(WTSVariant* params);

	/**
	 * @brief 设置交易适配器
	 * @param adapter 交易适配器指针
	 * @details 设置交易适配器并读取其状态
	 */
	void setTrader(TraderAdapter* adapter);

private:
	/**
	 * @brief 获取执行单元
	 * @param code 标准合约代码
	 * @param bAutoCreate 如果不存在是否自动创建，默认为true
	 * @return 执行单元指针
	 * @details 根据合约代码获取对应的执行单元，如果不存在且bAutoCreate为true则自动创建
	 */
	ExecuteUnitPtr	getUnit(const char* code, bool bAutoCreate = true);

public:
	//////////////////////////////////////////////////////////////////////////
	//ExecuteContext
	/**
	 * @brief 获取指定数量的历史tick数据
	 * @param code 标准合约代码
	 * @param count 请求的数据条数
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return 历史tick数据切片
	 * @details 从数据管理器中获取指定合约的历史tick数据
	 */
	virtual WTSTickSlice*	getTicks(const char* code, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 获取最新的tick数据
	 * @param code 标准合约代码
	 * @return 最新的tick数据
	 * @details 从数据管理器中获取指定合约的最新tick数据
	 */
	virtual WTSTickData*	grabLastTick(const char* code) override;

	/**
	 * @brief 获取指定合约的仓位
	 * @param stdCode 标准合约代码
	 * @param validOnly 是否只返回可用仓位，默认为true
	 * @param flag 仓位标志，默认为3（全部仓位）
	 * @return 仓位数量
	 * @details 从交易适配器中获取指定合约的仓位信息
	 */
	virtual double		getPosition(const char* stdCode, bool validOnly = true, int32_t flag = 3) override;

	/**
	 * @brief 获取指定合约的订单集合
	 * @param code 标准合约代码
	 * @return 订单集合
	 * @details 从交易适配器中获取指定合约的订单集合
	 */
	virtual OrderMap*	getOrders(const char* code) override;

	/**
	 * @brief 获取指定合约的未完成数量
	 * @param code 标准合约代码
	 * @return 未完成数量
	 * @details 从交易适配器中获取指定合约的未完成数量
	 */
	virtual double		getUndoneQty(const char* code) override;

	/**
	 * @brief 买入操作
	 * @param code 标准合约代码
	 * @param price 买入价格
	 * @param qty 买入数量
	 * @param bForceClose 是否强制平仓，默认为false
	 * @return 订单ID集合
	 * @details 通过交易适配器发送买入指令
	 */
	virtual OrderIDs	buy(const char* code, double price, double qty, bool bForceClose = false) override;

	/**
	 * @brief 卖出操作
	 * @param code 标准合约代码
	 * @param price 卖出价格
	 * @param qty 卖出数量
	 * @param bForceClose 是否强制平仓，默认为false
	 * @return 订单ID集合
	 * @details 通过交易适配器发送卖出指令
	 */
	virtual OrderIDs	sell(const char* code, double price, double qty, bool bForceClose = false) override;

	/**
	 * @brief 撤单操作
	 * @param localid 本地订单ID
	 * @return 撤单是否成功
	 * @details 通过交易适配器撤销指定本地ID的订单
	 */
	virtual bool		cancel(uint32_t localid) override;

	/**
	 * @brief 批量撤单操作
	 * @param code 标准合约代码
	 * @param isBuy 是否为买单
	 * @param qty 撤单数量
	 * @return 撤销的订单ID集合
	 * @details 通过交易适配器批量撤销指定合约和方向的订单
	 */
	virtual OrderIDs	cancel(const char* code, bool isBuy, double qty) override;

	/**
	 * @brief 写日志
	 * @param message 日志信息
	 * @details 记录日志信息
	 */
	virtual void		writeLog(const char* message) override;

	/**
	 * @brief 获取商品信息
	 * @param stdCode 标准合约代码
	 * @return 商品信息指针
	 * @details 从交易适配器中获取指定合约的商品信息
	 */
	virtual WTSCommodityInfo*	getCommodityInfo(const char* stdCode) override;

	/**
	 * @brief 获取交易时段信息
	 * @param stdCode 标准合约代码
	 * @return 交易时段信息指针
	 * @details 从交易适配器中获取指定合约的交易时段信息
	 */
	virtual WTSSessionInfo*		getSessionInfo(const char* stdCode) override;

	/**
	 * @brief 获取当前时间
	 * @return 当前时间戳
	 * @details 获取当前系统时间
	 */
	virtual uint64_t	getCurTime() override;

public:
	/**
	 * @brief 设置目标仓位
	 * @param targets 目标仓位映射表，键为合约代码，值为目标仓位
	 * @details 设置所有合约的目标仓位，并根据当前仓位进行调整
	 */
	virtual void set_position(const wt_hashmap<std::string, double>& targets) override;


	/**
	 * @brief 合约仓位变动回调
	 * @param stdCode 标准合约代码
	 * @param diffPos 仓位变化量
	 * @details 当合约仓位发生变化时调用此函数
	 */
	virtual void on_position_changed(const char* stdCode, double diffPos) override;

	/**
	 * @brief 实时行情回调
	 * @param stdCode 标准合约代码
	 * @param newTick 新的行情数据
	 * @details 当收到新的行情数据时调用此函数
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* newTick) override;

	/**
	 * @brief 成交回报回调
	 * @param localid 本地订单ID
	 * @param stdCode 标准合约代码
	 * @param isBuy 是否为买入
	 * @param vol 成交数量
	 * @param price 成交价格
	 * @details 当收到成交回报时调用此函数
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override;

	/**
	 * @brief 订单回报回调
	 * @param localid 本地订单ID
	 * @param stdCode 标准合约代码
	 * @param isBuy 是否为买入
	 * @param totalQty 总数量
	 * @param leftQty 剩余数量
	 * @param price 价格
	 * @param isCanceled 是否已撤销，默认为false
	 * @details 当收到订单回报时调用此函数
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled = false) override;

	/**
	 * @brief 仓位回报回调
	 * @param stdCode 标准合约代码
	 * @param isLong 是否为多头仓位
	 * @param prevol 前仓位量
	 * @param preavail 前可用仓位量
	 * @param newvol 新仓位量
	 * @param newavail 新可用仓位量
	 * @param tradingday 交易日
	 * @details 当收到仓位回报时调用此函数
	 */
	virtual void on_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday) override;

	/**
	 * @brief 委托回报回调
	 * @param localid 本地订单ID
	 * @param stdCode 标准合约代码
	 * @param bSuccess 是否成功
	 * @param message 消息
	 * @details 当收到委托回报时调用此函数
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override;

	/**
	 * @brief 交易通道就绪回调
	 * @details 当交易通道就绪时调用此函数
	 */
	virtual void on_channel_ready() override;

	/**
	 * @brief 交易通道丢失回调
	 * @details 当交易通道丢失时调用此函数
	 */
	virtual void on_channel_lost() override;

	/**
	 * @brief 资金回报回调
	 * @param currency 货币
	 * @param prebalance 前结余额
	 * @param balance 结余额
	 * @param dynbalance 动态结余额
	 * @param avaliable 可用资金
	 * @param closeprofit 平仓盈亏
	 * @param dynprofit 浮动盈亏
	 * @param margin 保证金
	 * @param fee 手续费
	 * @param deposit 入金
	 * @param withdraw 出金
	 * @details 当收到资金回报时调用此函数
	 */
	virtual void on_account(const char* currency, double prebalance, double balance, double dynbalance, 
		double avaliable, double closeprofit, double dynprofit, double margin, double fee, double deposit, double withdraw) override;

private:
	/**
	 * @brief 执行单元映射表
	 * @details 存储合约代码与执行单元的映射关系
	 */
	ExecuteUnitMap		_unit_map;

	/**
	 * @brief 交易适配器指针
	 * @details 用于处理交易请求和回报
	 */
	TraderAdapter*		_trader;

	/**
	 * @brief 执行器工厂指针
	 * @details 用于创建和管理执行器
	 */
	WtExecuterFactory*	_factory;

	/**
	 * @brief 数据管理器指针
	 * @details 用于获取市场数据
	 */
	IDataManager*		_data_mgr;

	/**
	 * @brief 配置参数
	 * @details 存储执行器的配置信息
	 */
	WTSVariant*			_config;

	/**
	 * @brief 仓位放大倍数
	 * @details 用于调整仓位的大小
	 */
	double				_scale;

	/**
	 * @brief 是否自动清理上一期的主力合约头寸
	 * @details 如果为true，则在合约切换时自动清理旧仓位
	 */
	bool				_auto_clear;

	/**
	 * @brief 是否严格同步目标仓位
	 * @details 如果为true，则严格按照目标仓位进行同步
	 */
	bool				_strict_sync;

	/**
	 * @brief 交易通道是否就绪
	 * @details 标记交易通道的状态
	 */
	bool				_channel_ready;

	/**
	 * @brief 执行单元互斥锁
	 * @details 用于保护执行单元映射表的线程安全
	 */
	SpinMutex			_mtx_units;

	/**
	 * @brief 合约组结构
	 * @details 定义了合约组的名称和合约项目
	 */
	typedef struct _CodeGroup
	{
		/**
		 * @brief 组名称
		 * @details 合约组的唯一标识名称
		 */
		char	_name[32] = { 0 };

		/**
		 * @brief 合约项目映射
		 * @details 存储合约代码与权重的映射关系
		 */
		wt_hashmap<std::string, double>	_items;
	} CodeGroup;

	/**
	 * @brief 合约组指针类型
	 * @details 定义了合约组的智能指针类型
	 */
	typedef std::shared_ptr<CodeGroup> CodeGroupPtr;

	/**
	 * @brief 合约组映射表类型
	 * @details 定义了字符串到合约组指针的映射表类型
	 */
	typedef wt_hashmap<std::string, CodeGroupPtr>	CodeGroups;

	/**
	 * @brief 合约组映射表
	 * @details 存储组合名称到组合的映射关系
	 */
	CodeGroups				_groups;

	/**
	 * @brief 合约代码到组合的映射表
	 * @details 存储合约代码到组合的映射关系
	 */
	CodeGroups				_code_to_groups;

	/**
	 * @brief 自动清理包含品种集合
	 * @details 存储需要自动清理的品种代码
	 */
	wt_hashset<std::string>	_clear_includes;

	/**
	 * @brief 自动清理排除品种集合
	 * @details 存储不需要自动清理的品种代码
	 */
	wt_hashset<std::string>	_clear_excludes;

	/**
	 * @brief 通道持仓集合
	 * @details 存储通道中的持仓合约代码
	 */
	wt_hashset<std::string> _channel_holds;

	/**
	 * @brief 目标仓位映射表
	 * @details 存储合约代码与目标仓位的映射关系
	 */
	wt_hashmap<std::string, double> _target_pos;

	/**
	 * @brief 线程池指针类型
	 * @details 定义了线程池的智能指针类型
	 */
	typedef std::shared_ptr<boost::threadpool::pool> ThreadPoolPtr;

	/**
	 * @brief 线程池指针
	 * @details 用于并行处理任务
	 */
	ThreadPoolPtr		_pool;
};

typedef std::shared_ptr<IExecCommand> ExecCmdPtr;

NS_WTP_END
