/*!
 * \file WtDiffExecuter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 差价执行器头文件
 * \details 差价执行器主要用于实现目标仓位和当前仓位之间的差价交易执行策略，是WonderTrader执行模块的重要组件
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
class IBaseDataMgr;
class TraderAdapter;
class IHotMgr;

/*!
 * \brief 差价执行器类
 * \details 差价执行器继承自ExecuteContext、ITrdNotifySink和IExecCommand接口，负责根据目标仓位和当前仓位的差值执行交易操作
 *          差价执行器通过计算目标仓位与实际仓位的差异，自动生成交易指令，实现仓位管理和调整
 *          同时处理交易通道的推送回调，如成交、委托、资金、持仓等信息
 */
class WtDiffExecuter : public ExecuteContext,
		public ITrdNotifySink, public IExecCommand
{
public:
	/*!
	 * \brief 构造函数
	 * \details 创建差价执行器对象，并初始化各种依赖组件
	 * \param factory 执行器工厂指针，用于创建执行单元
	 * \param name 执行器名称
	 * \param dataMgr 数据管理器指针，用于获取市场数据
	 * \param bdMgr 基础数据管理器指针，用于获取合约信息
	 */
	WtDiffExecuter(WtExecuterFactory* factory, const char* name, IDataManager* dataMgr, IBaseDataMgr* bdMgr);

	/*!
	 * \brief 析构函数
	 * \details 清理差价执行器的资源，包括线程池和配置
	 */
	virtual ~WtDiffExecuter();

public:
	/*!
	 * \brief 初始化执行器
	 * \details 根据传入的参数配置初始化差价执行器，设置缩放倍数、线程池等
	 * \param params 初始化参数，包含执行器的各种配置
	 * \return 是否初始化成功
	 */
	bool init(WTSVariant* params);

	/*!
	 * \brief 设置交易适配器
	 * \details 设置执行器使用的交易适配器，并读取其当前状态以更新通道就绪状态
	 * \param adapter 交易适配器指针
	 */
	void setTrader(TraderAdapter* adapter);

private:
	/*!
	 * \brief 获取执行单元
	 * \details 根据标准合约代码获取执行单元，如果单元不存在且允许自动创建，则创建新的执行单元
	 * \param code 标准合约代码
	 * \param bAutoCreate 是否自动创建，默认为true
	 * \return 执行单元指针
	 */
	ExecuteUnitPtr	getUnit(const char* code, bool bAutoCreate = true);

	/*!
	 * \brief 保存数据
	 * \details 保存差价执行器的状态数据，包括目标仓位和差异仓位等
	 */
	void	save_data();

	/*!
	 * \brief 加载数据
	 * \details 从存储中读取差价执行器的状态数据，恢复运行状态
	 */
	void	load_data();

public:
	//////////////////////////////////////////////////////////////////////////
	//ExecuteContext
	/*!
	 * \brief 获取Tick切片数据
	 * \details 从数据管理器获取指定合约的Tick切片数据
	 * \param code 标准合约代码
	 * \param count 请求的Tick数量
	 * \param etime 结束时间，默认为0表示当前
	 * \return Tick切片数据指针
	 */
	virtual WTSTickSlice* getTicks(const char* code, uint32_t count, uint64_t etime = 0) override;

	/*!
	 * \brief 获取最新Tick数据
	 * \details 从数据管理器中获取指定合约的最新Tick数据
	 * \param code 标准合约代码
	 * \return 最新Tick数据指针
	 */
	virtual WTSTickData*	grabLastTick(const char* code) override;

	/*!
	 * \brief 获取持仓量
	 * \details 从交易适配器中获取指定合约的持仓量
	 * \param stdCode 标准合约代码
	 * \param validOnly 是否只计算有效仓位，默认为true
	 * \param flag 仓位标记，默认为3
	 * \return 持仓数量
	 */
	virtual double		getPosition(const char* stdCode, bool validOnly = true, int32_t flag = 3) override;

	/*!
	 * \brief 获取委托映射
	 * \details 从交易适配器中获取指定合约的委托映射
	 * \param code 标准合约代码
	 * \return 委托映射指针
	 */
	virtual OrderMap*	getOrders(const char* code) override;

	/*!
	 * \brief 获取未完成数量
	 * \details 从交易适配器中获取指定合约的未完成委托数量
	 * \param code 标准合约代码
	 * \return 未完成委托数量
	 */
	virtual double		getUndoneQty(const char* code) override;

	/*!
	 * \brief 买入委托
	 * \details 发出买入委托，需要交易通道就绪
	 * \param code 标准合约代码
	 * \param price 委托价格
	 * \param qty 委托数量
	 * \param bForceClose 是否强制平仓，默认为false
	 * \return 委托ID列表
	 */
	virtual OrderIDs	buy(const char* code, double price, double qty, bool bForceClose = false) override;

	/*!
	 * \brief 卖出委托
	 * \details 发出卖出委托，需要交易通道就绪
	 * \param code 标准合约代码
	 * \param price 委托价格
	 * \param qty 委托数量
	 * \param bForceClose 是否强制平仓，默认为false
	 * \return 委托ID列表
	 */
	virtual OrderIDs	sell(const char* code, double price, double qty, bool bForceClose = false) override;

	/*!
	 * \brief 撤销委托
	 * \details 根据委托ID撤销指定委托，需要交易通道就绪
	 * \param localid 本地委托ID
	 * \return 是否撤销成功
	 */
	virtual bool		cancel(uint32_t localid) override;

	/*!
	 * \brief 撤销委托
	 * \details 根据合约代码、方向和数量撤销委托，需要交易通道就绪
	 * \param code 标准合约代码
	 * \param isBuy 是否买入方向
	 * \param qty 要撤销的数量
	 * \return 撤销的委托ID列表
	 */
	virtual OrderIDs	cancel(const char* code, bool isBuy, double qty) override;

	/*!
	 * \brief 输出日志
	 * \details 输出日志信息，并添加执行器名称前缀
	 * \param message 日志消息
	 */
	virtual void		writeLog(const char* message) override;

	/*!
	 * \brief 获取商品信息
	 * \details 从执行器绑定的执行核心中获取商品信息
	 * \param stdCode 标准合约代码
	 * \return 商品信息指针
	 */
	virtual WTSCommodityInfo*	getCommodityInfo(const char* stdCode) override;

	/*!
	 * \brief 获取交易时段信息
	 * \details 从执行器绑定的执行核心中获取交易时段信息
	 * \param stdCode 标准合约代码
	 * \return 交易时段信息指针
	 */
	virtual WTSSessionInfo*		getSessionInfo(const char* stdCode) override;

	/*!
	 * \brief 获取当前时间
	 * \details 从执行器绑定的执行核心中获取当前时间
	 * \return 当前时间戳
	 */
	virtual uint64_t	getCurTime() override;

public:
	/*!
	 * \brief 设置目标仓位
	 * \details 根据传入的目标仓位映射表设置合约的目标仓位，并自动处理交易逻辑
	 *          根据目标仓位与实际仓位的差异生成交易指令
	 * \param targets 目标仓位映射表，键为标准合约代码，值为目标仓位
	 */
	virtual void set_position(const wt_hashmap<std::string, double>& targets) override;


	/*!
	 * \brief 处理仓位变更
	 * \details 接收仓位变更通知，更新内部仓位计算并生成新的交易指令
	 * \param stdCode 标准合约代码
	 * \param diffPos 仓位变化量
	 */
	virtual void on_position_changed(const char* stdCode, double diffPos) override;

	/*!
	 * \brief 处理实时行情
	 * \details 接收并处理实时行情数据，将行情转发给相应执行单元
	 * \param stdCode 标准合约代码
	 * \param newTick 最新的行情数据
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* newTick) override;

	/*!
	 * \brief 处理成交回报
	 * \details 接收并处理成交回报，将成交信息转发给相应的执行单元
	 *          并更新差价执行器的仓位管理记录
	 * \param localid 本地委托ID
	 * \param stdCode 标准合约代码
	 * \param isBuy 是否买入方向
	 * \param vol 成交数量
	 * \param price 成交价格
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override;

	/*!
	 * \brief 处理委托回报
	 * \details 接收并处理委托回报，将委托信息转发给相应的执行单元
	 *          更新委托状态处理和统计
	 * \param localid 本地委托ID
	 * \param stdCode 标准合约代码
	 * \param isBuy 是否买入方向
	 * \param totalQty 总委托数量
	 * \param leftQty 剩余委托数量
	 * \param price 委托价格
	 * \param isCanceled 是否已撤销，默认为false
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled = false) override;

	/*!
	 * \brief 处理持仓更新通知
	 * \details 接收交易账户持仓更新通知，更新差价执行器的持仓记录
	 * \param stdCode 标准合约代码
	 * \param isLong 是否为多头持仓
	 * \param prevol 之前的持仓量
	 * \param preavail 之前的可用持仓量
	 * \param newvol 新的持仓量
	 * \param newavail 新的可用持仓量
	 * \param tradingday 交易日
	 */
	virtual void on_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday) override;

	/*!
	 * \brief 处理委托下达回报
	 * \details 接收并处理委托下达回报，将委托下达结果转发给相应的执行单元
	 *          委托下达回报包含委托是否成功下达以及相关消息
	 * \param localid 本地委托ID
	 * \param stdCode 标准合约代码
	 * \param bSuccess 委托是否成功下达
	 * \param message 相关消息，如错误原因
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override;

	/*!
	 * \brief 处理交易通道就绪事件
	 * \details 当交易通道就绪时，更新内部通道就绪状态并通知所有执行单元
	 *          通道就绪后可以开始发送交易指令
	 */
	virtual void on_channel_ready() override;

	/*!
	 * \brief 处理交易通道丢失事件
	 * \details 当交易通道丢失时，更新内部通道状态并通知所有执行单元
	 *          通道丢失后不能发送交易指令，需要等待通道恢复
	 */
	virtual void on_channel_lost() override;

	/*!
	 * \brief 处理账户资金更新
	 * \details 接收并处理账户资金更新通知，更新差价执行器的资金记录
	 *          资金信息包含余额、可用资金、盈亏、保证金等
	 * \param currency 货币代码
	 * \param prebalance 前一交易日结算后的账户余额
	 * \param balance 当前结算后的账户余额
	 * \param dynbalance 动态权益（包含浮动盈亏）
	 * \param avaliable 可用资金
	 * \param closeprofit 平仓盈亏
	 * \param dynprofit 浮动盈亏
	 * \param margin 保证金
	 * \param fee 手续费
	 * \param deposit 入金
	 * \param withdraw 出金
	 */
	virtual void on_account(const char* currency, double prebalance, double balance, double dynbalance,
		double avaliable, double closeprofit, double dynprofit, double margin, double fee, double deposit, double withdraw) override;


private:
	//! 执行单元映射，键为合约代码，值为执行单元指针
	ExecuteUnitMap		_unit_map;
	//! 交易适配器指针，用于执行交易操作
	TraderAdapter*		_trader;
	//! 执行器工厂指针，用于创建执行单元
	WtExecuterFactory*	_factory;
	//! 数据管理器指针，用于获取市场数据
	IDataManager*		_data_mgr;
	//! 基础数据管理器指针，用于获取合约信息
	IBaseDataMgr*		_bd_mgr;
	//! 配置参数，包含执行器的各种配置项
	WTSVariant*			_config;

	//! 仓位放大倍数，用于调整实际交易数量
	double				_scale;
	//! 交易通道是否就绪
	bool				_channel_ready;

	//! 执行单元操作的互斥锁，用于多线程安全访问
	SpinMutex			_mtx_units;

	//! 目标仓位映射表，键为合约代码，值为目标仓位
	wt_hashmap<std::string, double> _target_pos;
	//! 差异仓位映射表，键为合约代码，值为仓位差异
	wt_hashmap<std::string, double> _diff_pos;

	//! 线程池指针类型定义
	typedef std::shared_ptr<boost::threadpool::pool> ThreadPoolPtr;
	//! 线程池指针，用于异步执行交易操作
	ThreadPoolPtr		_pool;
};
NS_WTP_END
