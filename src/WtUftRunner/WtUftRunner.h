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
#include <string>
#include <unordered_map>

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
class WtUftRunner : public ILogHandler
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
	 * @return 配置加载是否成功
	 * @details 加载主配置文件并初始化各个模块，包括基础数据、数据管理器、引擎、
	 *          行情和交易适配器等
	 */
	bool config(const std::string& filename);

	/**
	 * @brief 启动并运行引擎
	 * @param bAsync 是否以异步模式运行，默认为同步模式
	 * @details 运行策略引擎、行情和交易适配器，并根据需要等待退出信号
	 */
	void run(bool bAsync = false);

private:
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

	bool				_to_exit;        ///< 退出标志
};

