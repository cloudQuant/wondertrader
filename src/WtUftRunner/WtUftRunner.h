/*!
 * @file WtUftRunner.h
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief UFT策略引擎运行器头文件
 * @details 定义了WtUftRunner类，该类负责初始化和管理UFT策略引擎的全生命周期，
 *          包括配置加载、策略初始化、行情和交易接口连接、事件处理等功能。
 */
#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../Includes/ILogHandler.h"

#include "../WtUftCore/EventNotifier.h"
#include "../WtUftCore/UftStrategyMgr.h"

#include "../WtUftCore/WtUftEngine.h"
#include "../WtUftCore/TraderAdapter.h"
#include "../WtUftCore/ParserAdapter.h"
#include "../WtUftCore/WtUftDtMgr.h"
#include "../WtUftCore/ActionPolicyMgr.h"

#include "../WTSTools/WTSHotMgr.h"
#include "../WTSTools/WTSBaseDataMgr.h"

NS_WTP_BEGIN
class WTSVariant;
NS_WTP_END

USING_NS_WTP;

/**
 * @brief UFT策略引擎运行器类
 * @details 负责初始化并运行整个UFT策略引擎，包括加载配置、初始化各个模块、
 *          管理策略和各种适配器、处理日志输出等。继承ILogHandler接口实现日志处理功能。
 */
class WtUftRunner final : public ILogHandler // final: owned/deleted by main (I03-NEW-058)
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化运行器对象并设置信号处理钩子
	 */
	WtUftRunner();

	/**
	 * @brief 析构函数
	 * @details 清理资源并释放内存
	 */
	~WtUftRunner();

public:
	/**
	 * @brief 初始化日志系统
	 * @param filename 日志配置文件路径
	 * @details 使用指定的配置文件初始化日志系统，并设置安装目录
	 */
	void init(const std::string& filename);

	/**
	 * @brief 加载并应用配置
	 * @param filename 主配置文件路径
	 * @return 配置加载是否成功；每个 runner 实例只允许尝试一次
	 * @details 加载主配置文件并初始化各个模块，包括基础数据、数据管理器、引擎、
	 *          行情和交易适配器等
	 */
	bool config(const std::string& filename);

	/**
	 * @brief 启动并运行引擎
	 * @param bAsync 是否以异步模式运行，默认为同步模式
	 * @details 两种模式都会在当前线程启动引擎、行情和交易适配器。异步模式在启动完成后返回；
	 *          每个实例最多尝试启动一次；重复调用在进入引擎前拒绝。
	 *          调用方必须保持 runner 存活，并在控制线程调用 shutdown()。同步模式等待退出信号并自行调用 shutdown()。
	 */
	void run(bool bAsync = false);

	/**
 * Request producer shutdown, join the ticker/share watcher, and drain trader
 * notifications. Returns false while parser callback quiescence is unproven or
 * adapter teardown remains pending or runner startup is active; keep the
 * runner alive and retry after startup or the provider-specific barrier.
 * A startup-time request is retried when run() leaves startup. Parser callback calls return false immediately;
 * retry from a control thread before destroying the runner.
	 */
	bool shutdown();

	// I03-NEW-058 / card 9a: true when the last shutdown attempt was blocked only by
	// parser owners lacking callback-drain proof; such a runner is retained (never
	// destroyed) by the process entry until process exit instead of aborting.
	bool parserOwnersRetained() const { return _parser_owners_retained.load(std::memory_order_acquire); }

private:
	bool configImpl(const std::string& filename);
	bool registerParserCandidate(const std::string& id, ParserAdapterPtr& adapter);
	bool releaseParserCandidateIfSafe(ParserAdapterPtr& adapter);
	bool startParsers();
	bool startParsers(bool runnerLease);
	void finishParserStart();
	bool closeParserStartAndDrain();
	bool isInParserCallbackOnCurrentThread() const;

	/**
	 * @brief 初始化交易适配器
	 * @param cfgTrader 交易适配器配置
	 * @return 初始化是否成功
	 * @details 加载并初始化所有交易适配器，适配器用于连接实际交易接口
	 */
	bool initTraders(WTSVariant* cfgTrader);

	/**
	 * @brief 初始化行情解析器
	 * @param cfgParser 行情解析器配置
	 * @return 初始化是否成功
	 * @details 加载并初始化所有行情解析器，解析器用于接收和解析市场行情数据
	 */
	bool initParsers(WTSVariant* cfgParser);

	/**
	 * @brief 初始化数据管理器
	 * @return 初始化是否成功
	 * @details 配置并初始化数据管理器，负责管理交易数据和存储
	 */
	bool initDataMgr();

	/**
	 * @brief 初始化UFT策略
	 * @return 初始化是否成功
	 * @details 加载策略工厂并创建策略实例，为每个策略创建上下文并绑定交易适配器
	 */
	bool initUftStrategies();

	/**
	 * @brief 初始化事件通知器
	 * @return 初始化是否成功
	 * @details 配置并初始化事件通知器，负责处理和分发系统事件
	 */
	bool initEvtNotifier();

	/**
	 * @brief 初始化引擎
	 * @return 初始化是否成功
	 * @details 配置并初始化UFT策略引擎，为引擎设置各种管理器和适配器
	 */
	bool initEngine();

//////////////////////////////////////////////////////////////////////////
//ILogHandler
public:
	/**
	 * @brief 日志处理回调函数
	 * @param ll 日志级别
	 * @param msg 日志消息内容
	 * @details 实现ILogHandler接口中的日志处理方法，用于处理系统日志
	 */
	virtual void handleLogAppend(WTSLogLevel ll, const char* msg) override;

private:
	WTSVariant*			_config;        ///< 系统配置
	TraderAdapterMgr	_traders;       ///< 交易适配器管理器
	ParserAdapterMgr	_parsers;       ///< 行情解析器管理器

	WtUftEngine			_uft_engine;    ///< UFT策略引擎实例

	WtUftDtMgr			_data_mgr;      ///< 数据管理器

	WTSBaseDataMgr		_bd_mgr;        ///< 基础数据管理器
	EventNotifier		_notifier;       ///< 事件通知器

	UftStrategyMgr		_uft_stra_mgr;  ///< UFT策略管理器

	ActionPolicyMgr		_act_policy;    ///< 操作策略管理器
	std::atomic<bool>   _shutdown_requested{false};
	std::atomic<bool>   _config_in_progress{false};
	std::atomic<bool>   _inputs_stopped{false};
	std::atomic<bool>   _shutdown_complete{false};
	std::atomic_flag    _shutdown_running = ATOMIC_FLAG_INIT;
	std::atomic<bool>   _config_attempted{false};
	std::atomic<bool>   _config_succeeded{false};
	std::atomic<bool>   _share_domain_attached{false};
	std::atomic<bool>   _parser_quarantine_pending{false};
	std::atomic<bool>   _parser_owners_retained{false};
	std::vector<ParserAdapterPtr> _quarantined_parsers;
	std::mutex _parser_start_mutex;
	std::condition_variable _parser_start_cv;
	std::thread::id _parser_start_owner;
	bool _parser_start_closed{false};
	bool _parser_start_active{false};
	bool _parser_start_attempted{false};

	std::atomic<bool>	_to_exit;        ///< 退出标志
};

