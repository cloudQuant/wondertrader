/*!
 * @file WtRunner.h
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief WtRunner类定义，实现交易引擎的运行管理
 * @details WtRunner是WonderTrader的主要运行器，负责管理交易引擎的初始化、配置和运行
 */
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../Includes/ILogHandler.h"

#include "../WtCore/EventNotifier.h"
#include "../WtCore/CtaStrategyMgr.h"
#include "../WtCore/HftStrategyMgr.h"
#include "../WtCore/SelStrategyMgr.h"

#include "../WtCore/WtCtaEngine.h"
#include "../WtCore/WtHftEngine.h"
#include "../WtCore/WtSelEngine.h"
#include "../WtCore/WtExecMgr.h"
#include "../WtCore/WtLocalExecuter.h"
#include "../WtCore/WtDistExecuter.h"
#include "../WtCore/TraderAdapter.h"
#include "../WtCore/ParserAdapter.h"
#include "../WtCore/WtDtMgr.h"
#include "../WtCore/ActionPolicyMgr.h"

#include "../WTSTools/WTSHotMgr.h"
#include "../WTSTools/WTSBaseDataMgr.h"

NS_WTP_BEGIN
class WTSVariant;
class WtDataStorage;
NS_WTP_END

USING_NS_WTP;

/**
 * @brief WonderTrader运行器类
 * @details 负责管理交易引擎的初始化、配置和运行，包括CTA策略、HFT策略和选股策略的管理
 */
class WtRunner : public ILogHandler
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化成员变量并安装信号钩子
	 */
	WtRunner();

	/**
	 * @brief 析构函数
	 */
	~WtRunner();

public:
	/**
	 * @brief 初始化日志系统
	 * @details 根据配置文件初始化日志系统和设置安装目录
	 * @param filename 日志配置文件路径
	 */
	void init(const std::string& filename);

	/**
	 * @brief 配置交易引擎
	 * @details 加载配置文件并初始化各个组件，包括基础数据、交易引擎、数据管理器等
	 * @param filename 配置文件路径
	 * @return 是否配置成功
	 */
	bool config(const std::string& filename);

	/**
	 * @brief 运行交易引擎
	 * @details 启动解析器、交易适配器和交易引擎
	 * @param bAsync 是否异步运行，默认为false
	 */
	void run(bool bAsync = false);

	/** Whether all parser connection commands in the latest start were submitted.
	 * False includes not yet started, startup in progress, or a rejected command.
	 * True does not indicate provider connection or readiness.
	 */
	bool parserCommandsSubmitted() const { return _parser_commands_submitted.load(); }
	/** Whether the accepted run invocation returned normally after startup.
	 * False includes not started, still running synchronously, early shutdown,
	 * and an exception. This does not imply parser submission or readiness.
	 */
	bool runSucceeded() const { return _run_succeeded.load(std::memory_order_acquire); }

	/**
	 * @brief Stop this runner and quiesce every executor manager.
	 * @details Call from a control thread. A false result leaves the runner and
	 *          its executor owners alive; call again from a control thread after
	 *          the active callback has returned to retry teardown.
	 */
	bool shutdown();

private:
	/**
	 * @brief 初始化交易适配器
	 * @details 根据配置初始化交易适配器，建立交易通道
	 * @param cfgTrader 交易适配器配置
	 * @return 是否初始化成功
	 */
	bool initTraders(WTSVariant* cfgTrader);

	/**
	 * @brief 初始化行情解析器
	 * @details 根据配置初始化行情解析器，建立行情数据通道
	 * @param cfgParser 解析器配置
	 * @return 是否初始化成功
	 */
	bool initParsers(WTSVariant* cfgParser);

	/**
	 * @brief 初始化执行器
	 * @details 根据配置初始化交易指令执行器，包括本地执行器、差异执行器和分布式执行器
	 * @param cfgExecuter 执行器配置
	 * @return 是否初始化成功
	 */
	bool initExecuters(WTSVariant* cfgExecuter);

	/**
	 * @brief 初始化数据管理器
	 * @details 根据配置初始化数据管理器，设置数据存储路径
	 * @return 是否初始化成功
	 */
	bool initDataMgr();

	/**
	 * @brief 初始化事件通知器
	 * @details 根据配置初始化事件通知器，用于处理系统事件的通知
	 * @return 是否初始化成功
	 */
	bool initEvtNotifier();

	/**
	 * @brief 初始化CTA策略
	 * @details 从配置中加载CTA策略并创建相应的策略上下文
	 * @return 是否初始化成功
	 */
	bool initCtaStrategies();

	/**
	 * @brief 初始化HFT高频策略
	 * @details 从配置中加载HFT高频策略并创建相应的策略上下文
	 * @return 是否初始化成功
	 */
	bool initHftStrategies();

	/**
	 * @brief Initialize SEL strategies from strategies.sel
	 * @details Validates, creates and initializes the whole batch before any
	 *          SEL context is registered; any failure registers nothing.
	 * @return true when every active SEL strategy is registered
	 */
	bool initSelStrategies();

	/**
	 * @brief 初始化交易行为策略
	 * @details 根据配置初始化交易行为策略，加载交易限制和风控策略
	 * @return 是否初始化成功
	 */
	bool initActionPolicy();

	// Stops new notifications and quiesces all executor managers before releasing
	// adapters. False leaves all runtime owners alive for a later control-thread
	// retry; the persistent coordinator retains its manager/factory snapshots.
	bool shutdownRuntime(bool stopEngines = true, bool fromConfigFailure = false);

	/**
	 * @brief 初始化交易引擎
	 * @details 根据环境配置初始化交易引擎，设置相应的引擎类型（CTA、HFT或SEL）
	 * @return 是否初始化成功
	 */
	bool initEngine();

//////////////////////////////////////////////////////////////////////////
//ILogHandler
public:
	/**
	 * @brief 处理日志追加
	 * @details 实现ILogHandler接口，处理日志追加事件，将日志消息通过通知器发送
	 * @param ll 日志级别
	 * @param msg 日志消息
	 */
	virtual void handleLogAppend(WTSLogLevel ll, const char* msg) override;

private:
	WTSVariant*			_config;				//!< 配置对象
	TraderAdapterMgr	_traders;			//!< 交易适配器管理器
	ParserAdapterMgr	_parsers;			//!< 解析器适配器管理器
	WtExecuterFactory	_exe_factory;		//!< 执行器工厂

	WtCtaEngine			_cta_engine;			//!< CTA交易引擎
	WtHftEngine			_hft_engine;			//!< HFT高频交易引擎
	WtSelEngine			_sel_engine;			//!< 选股交易引擎
	std::unique_ptr<WtExecQuiesceCoordinator> _exec_quiesce; //!< Persistent coordinator for retries.
	WtEngine*			_engine;				//!< 当前使用的交易引擎指针

	[[maybe_unused]] WtDataStorage*		_data_store;			//!< 数据存储对象

	WtDtMgr				_data_mgr;			//!< 数据管理器

	WTSBaseDataMgr		_bd_mgr;				//!< 基础数据管理器
	WTSHotMgr			_hot_mgr;				//!< 主力合约管理器
	EventNotifier		_notifier;			//!< 事件通知器

	CtaStrategyMgr		_cta_stra_mgr;		//!< CTA策略管理器
	HftStrategyMgr		_hft_stra_mgr;		//!< HFT高频策略管理器
	SelStrategyMgr		_sel_stra_mgr;		//!< 选股策略管理器
	ActionPolicyMgr		_act_policy;			//!< 交易行为策略管理器

	bool				_is_hft;				//!< 是否为高频交易模式
	bool				_is_sel;				//!< 是否为选股模式

	std::atomic<bool> _to_exit;				//!< Exit flag shared by signal and control threads.
	mutable std::recursive_mutex _lifecycle_mutex;
	bool				_configuring = false;
	bool				_runtime_starting = false;
	bool				_shutdown_in_progress = false;
	bool				_runtime_shutdown_complete = false;
	std::atomic<bool> _runtime_started;	//!< Runtime config is frozen after run begins.
	std::atomic<bool> _parser_commands_submitted{false}; //!< Command submission, not provider readiness.
	std::atomic<bool> _run_succeeded{false}; //!< Normal completion of the accepted run invocation.
	std::atomic<bool> _shutdown_requested;	//!< Reject config after any shutdown attempt.
	std::vector<ParserAdapterPtr> _failed_parser_owners; //!< Retain partial parser initializations fail-closed.
};

