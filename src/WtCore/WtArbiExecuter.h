/*!
 * \file WtArbiExecuter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * \brief 套利交易执行器定义
 * \details 定义了套利交易执行器，用于执行多个相关合约的组合交易策略
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

/*!
 * \brief 套利交易执行器
 * \details 用于处理多个相关合约的组合交易和套利策略执行，支持合约组管理及自动仓位清理
 * \details 继承自ExecuteContext、ITrdNotifySink和IExecCommand，实现执行环境、交易通知和执行命令接口
 */
class WtArbiExecuter : public ExecuteContext,
	public ITrdNotifySink, public IExecCommand
{
public:
	/*!
	 * \brief 构造函数
	 * \param factory 执行器工厂指针
	 * \param name 执行器名称
	 * \param dataMgr 数据管理器指针
	 */
	WtArbiExecuter(WtExecuterFactory* factory, const char* name, IDataManager* dataMgr);

	/*!
	 * \brief 析构函数
	 */
	virtual ~WtArbiExecuter();

public:
	/*!
	 * \brief 初始化执行器
	 * \param params 初始化参数
	 * \return 是否初始化成功
	 * \details 根据传入的参数配置初始化套利执行器，设置参数如自动清理模式、放大倍数等
	 */
	bool init(WTSVariant* params);

	/*!
	 * \brief 设置交易适配器
	 * \param adapter 交易适配器指针
	 * \details 设置交易适配器，用于连接到交易接口
	 */
	void setTrader(TraderAdapter* adapter);

private:
	/*!
	 * \brief 获取单元执行器
	 * \param code 合约代码
	 * \param bAutoCreate 是否自动创建，默认为true
	 * \return 单元执行器指针
	 * \details 根据合约代码获取单元执行器，如果不存在且bAutoCreate为true则自动创建
	 */
	ExecuteUnitPtr	getUnit(const char* code, bool bAutoCreate = true);

public:
		//////////////////////////////////////////////////////////////////////////
	//ExecuteContext接口实现
	/*!
	 * \brief 获取Tick切片数据
	 * \param code 合约代码
	 * \param count 请求的Tick数量
	 * \param etime 结束时间，默认为0表示当前
	 * \return Tick切片数据指针
	 * \details 获取指定合约的历史Tick数据切片
	 */
	virtual WTSTickSlice*	getTicks(const char* code, uint32_t count, uint64_t etime = 0) override;

	/*!
	 * \brief 获取最新Tick数据
	 * \param code 合约代码
	 * \return 最新Tick数据指针
	 * \details 获取指定合约的最新市场Tick数据
	 */
	virtual WTSTickData*	grabLastTick(const char* code) override;

	/*!
	 * \brief 获取持仓量
	 * \param stdCode 标准合约代码
	 * \param validOnly 是否只计算有效仓位，默认为true
	 * \param flag 仓位标记，默认为3
	 * \return 持仓数量
	 * \details 获取指定合约的持仓量
	 */
	virtual double		getPosition(const char* stdCode, bool validOnly = true, int32_t flag = 3) override;

	/*!
	 * \brief 获取当前委托
	 * \param code 合约代码
	 * \return 委托映射指针
	 * \details 获取指定合约的当前委托映射
	 */
	virtual OrderMap*	getOrders(const char* code) override;

	/*!
	 * \brief 获取未完成数量
	 * \param code 合约代码
	 * \return 未完成数量
	 * \details 获取指定合约的未完成委托数量
	 */
	virtual double		getUndoneQty(const char* code) override;

	/*!
	 * \brief 买入委托
	 * \param code 合约代码
	 * \param price 委托价格
	 * \param qty 委托数量
	 * \param bForceClose 是否强制平仓，默认为false
	 * \return 委托ID列表
	 * \details 发出买入委托并返回委托ID列表
	 */
	virtual OrderIDs	buy(const char* code, double price, double qty, bool bForceClose = false) override;

	/*!
	 * \brief 卖出委托
	 * \param code 合约代码
	 * \param price 委托价格
	 * \param qty 委托数量
	 * \param bForceClose 是否强制平仓，默认为false
	 * \return 委托ID列表
	 * \details 发出卖出委托并返回委托ID列表
	 */
	virtual OrderIDs	sell(const char* code, double price, double qty, bool bForceClose = false) override;

	/*!
	 * \brief 撤销委托
	 * \param localid 本地委托ID
	 * \return 是否撤销成功
	 * \details 根据本地委托ID撤销对应的委托
	 */
	virtual bool		cancel(uint32_t localid) override;

	/*!
	 * \brief 撤销委托
	 * \param code 合约代码
	 * \param isBuy 是否买入方向
	 * \param qty 要撤销的数量
	 * \return 撤销的委托ID列表
	 * \details 根据合约代码、方向和数量撤销委托
	 */
	virtual OrderIDs	cancel(const char* code, bool isBuy, double qty) override;

	/*!
	 * \brief 输出日志
	 * \param message 日志消息
	 * \details 将消息写入日志
	 */
	virtual void		writeLog(const char* message) override;

	/*!
	 * \brief 获取商品信息
	 * \param stdCode 标准合约代码
	 * \return 商品信息指针
	 * \details 获取指定合约的商品信息
	 */
	virtual WTSCommodityInfo*	getCommodityInfo(const char* stdCode) override;

	/*!
	 * \brief 获取交易时段信息
	 * \param stdCode 标准合约代码
	 * \return 交易时段信息指针
	 * \details 获取指定合约的交易时段信息
	 */
	virtual WTSSessionInfo*		getSessionInfo(const char* stdCode) override;

	/*!
	 * \brief 获取当前时间
	 * \return 当前时间戳
	 * \details 获取系统当前时间
	 */
	virtual uint64_t	getCurTime() override;

public:
		//////////////////////////////////////////////////////////////////////////
	//IExecCommand接口实现
	/*!
	 * \brief 设置目标仓位
	 * \param targets 目标仓位映射，存储合约代码到目标仓位的映射关系
	 * \details 设置各合约的目标仓位，由执行器根据目标调整实际仓位
	 */
	virtual void set_position(const wt_hashmap<std::string, double>& targets) override;

	/*!
	 * \brief 处理仓位变化
	 * \param stdCode 标准合约代码
	 * \param diffPos 仓位变化量
	 * \details 处理合约仓位变化的回调函数
	 */
	virtual void on_position_changed(const char* stdCode, double diffPos) override;

	/*!
	 * \brief 行情数据回调
	 * \param stdCode 标准合约代码
	 * \param newTick 新的Tick数据
	 * \details 处理新到的行情数据更新
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* newTick) override;

		//////////////////////////////////////////////////////////////////////////
	//ITrdNotifySink接口实现
	/*!
	 * \brief 成交回报回调
	 * \param localid 本地委托ID
	 * \param stdCode 标准合约代码
	 * \param isBuy 是否买入方向
	 * \param vol 成交量
	 * \param price 成交价格
	 * \details 处理成交回报的回调函数
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override;

	/*!
	 * \brief 委托回报回调
	 * \param localid 本地委托ID
	 * \param stdCode 标准合约代码
	 * \param isBuy 是否买入方向
	 * \param totalQty 总数量
	 * \param leftQty 剩余数量
	 * \param price 委托价格
	 * \param isCanceled 是否已撤销，默认为false
	 * \details 处理委托状态变化的回调函数
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled = false) override;

	/*!
	 * \brief 持仓更新回调
	 * \param stdCode 标准合约代码
	 * \param isLong 是否多头仓位
	 * \param prevol 之前的总仓位
	 * \param preavail 之前的可用仓位
	 * \param newvol 新的总仓位
	 * \param newavail 新的可用仓位
	 * \param tradingday 交易日
	 * \details 处理持仓变化的回调函数
	 */
	virtual void on_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday) override;

	/*!
	 * \brief 下单结果回调
	 * \param localid 本地委托ID
	 * \param stdCode 标准合约代码
	 * \param bSuccess 是否成功
	 * \param message 错误消息（如有）
	 * \details 处理下单请求结果的回调函数
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override;

		/*!
	 * \brief 交易通道就绪回调
	 * \details 处理交易通道就绪状态的回调函数
	 */
	virtual void on_channel_ready() override;

	/*!
	 * \brief 交易通道断开回调
	 * \details 处理交易通道断开状态的回调函数
	 */
	virtual void on_channel_lost() override;

	/*!
	 * \brief 资金回报回调
	 * \param currency 货币代码
	 * \param prebalance 之前的余额
	 * \param balance 当前的余额
	 * \param dynbalance 动态余额
	 * \param avaliable 可用余额
	 * \param closeprofit 平仓盈亏
	 * \param dynprofit 动态盈亏
	 * \param margin 保证金
	 * \param fee 手续费
	 * \param deposit 入金金额
	 * \param withdraw 出金金额
	 * \details 处理资金变化的回调函数
	 */
	virtual void on_account(const char* currency, double prebalance, double balance, double dynbalance, 
		double avaliable, double closeprofit, double dynprofit, double margin, double fee, double deposit, double withdraw) override;

private:
	/*!
	 * \brief 执行单元映射
	 * \details 存储合约代码到执行单元的映射关系
	 */
	ExecuteUnitMap		_unit_map;

	/*!
	 * \brief 交易适配器指针
	 * \details 提供交易接口的适配器，用于发送交易指令
	 */
	TraderAdapter*		_trader;

	/*!
	 * \brief 执行器工厂指针
	 * \details 用于创建和管理执行器实例
	 */
	WtExecuterFactory*	_factory;

	/*!
	 * \brief 数据管理器指针
	 * \details 提供市场数据访问接口
	 */
	IDataManager*		_data_mgr;

	/*!
	 * \brief 配置参数指针
	 * \details 存储执行器的配置参数
	 */
	WTSVariant*			_config;

		/*!
	 * \brief 仓位放大倍数
	 * \details 用于按比例放大交易仓位
	 */
	double				_scale;

	/*!
	 * \brief 是否自动清理上一期的主力合约头寸
	 * \details 当合约换月时，是否自动清理旧主力合约的仓位
	 */
	bool				_auto_clear;

	/*!
	 * \brief 是否严格同步目标仓位
	 * \details 设置为true时，执行器会严格按照目标仓位进行调整
	 */
	bool				_strict_sync;

	/*!
	 * \brief 交易通道是否就绪
	 * \details 标记当前交易通道的连接状态
	 */
	bool				_channel_ready;

		/*!
	 * \brief 单元互斥锁
	 * \details 用于保护执行单元映射的线程安全访问
	 */
	SpinMutex			_mtx_units;

		/*!
	 * \brief 代码组结构体
	 * \details 定义了一组相关合约及其权重，用于套利交易
	 */
	typedef struct _CodeGroup
	{
		/*!
		 * \brief 组名称
		 * \details 代码组的唯一标识名称
		 */
		char	_name[32] = { 0 };

		/*!
		 * \brief 组成员项
		 * \details 存储合约代码到权重的映射关系
		 */
		wt_hashmap<std::string, double>	_items;
	} CodeGroup;
		/*!
	 * \brief 代码组共享指针类型定义
	 */
	typedef std::shared_ptr<CodeGroup> CodeGroupPtr;

	/*!
	 * \brief 代码组映射类型定义
	 * \details 从字符串(组名或合约代码)到代码组的映射
	 */
	typedef wt_hashmap<std::string, CodeGroupPtr>	CodeGroups;

	/*!
	 * \brief 组映射集合
	 * \details 存储组名称到组对象的映射关系
	 */
	CodeGroups				_groups;

	/*!
	 * \brief 合约到组的映射
	 * \details 存储合约代码到所属组的映射关系
	 */
	CodeGroups				_code_to_groups;

		/*!
	 * \brief 自动清理包含品种集合
	 * \details 存储需要自动清理的品种名称
	 */
	wt_hashset<std::string>	_clear_includes;

	/*!
	 * \brief 自动清理排除品种集合
	 * \details 存储不需要自动清理的品种名称
	 */
	wt_hashset<std::string>	_clear_excludes;

		/*!
	 * \brief 通道持仓集合
	 * \details 存储当前通道已有的持仓合约代码
	 */
	wt_hashset<std::string> _channel_holds;

		/*!
	 * \brief 目标仓位映射
	 * \details 存储合约代码到目标仓位的映射关系
	 */
	wt_hashmap<std::string, double> _target_pos;

		/*!
	 * \brief 线程池指针类型定义
	 */
	typedef std::shared_ptr<boost::threadpool::pool> ThreadPoolPtr;

	/*!
	 * \brief 线程池指针
	 * \details 用于执行异步任务的线程池
	 */
	ThreadPoolPtr		_pool;
};

/*!
 * \brief 执行命令指针类型定义
 * \details 执行命令接口的智能指针类型，用于管理命令对象的生命周期
 */
typedef std::shared_ptr<IExecCommand> ExecCmdPtr;

NS_WTP_END
