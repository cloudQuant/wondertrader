/*!
 * @file WtExecRunner.h
 * @author wondertrader
 * @date 2022/05/01
 * 
 * @brief 执行监控系统运行器定义
 * @details 定义了执行监控系统的运行器，负责管理交易执行器、行情解析器和交易适配器
 */

#pragma once

#include "WtSimpDataMgr.h"

#include "../WtCore/WtExecMgr.h"
#include "../WtCore/TraderAdapter.h"
#include "../WtCore/ParserAdapter.h"
#include "../WtCore/ActionPolicyMgr.h"

#include "../WTSTools/WTSHotMgr.h"
#include "../WTSTools/WTSBaseDataMgr.h"

NS_WTP_BEGIN
class WTSVariant;
NS_WTP_END

USING_NS_WTP;

/**
 * @brief 执行监控系统运行器
 * @details 继承自IParserStub和IExecuterStub，负责协调执行器、解析器和交易适配器的工作
 *          实现了数据管理、交易执行和行情处理等功能
 */
class WtExecRunner : public IParserStub, public IExecuterStub
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化执行监控系统运行器，安装信号处理钩子
	 */
	WtExecRunner();

	/**
	 * @brief 初始化运行器
	 * @details 初始化日志系统和设置安装目录
	 * @param logCfg 日志配置文件路径或内容，默认为"logcfgexec.json"
	 * @param isFile 是否为文件路径，默认为true
	 * @return 初始化是否成功
	 */
	bool init(const char* logCfg = "logcfgexec.json", bool isFile = true);

	/**
	 * @brief 加载配置
	 * @details 加载执行监控系统的配置，包括基础数据、解析器、交易器和执行器等
	 * @param cfgFile 配置文件路径或内容
	 * @param isFile 是否为文件路径，默认为true
	 * @return 加载配置是否成功
	 */
	bool config(const char* cfgFile, bool isFile = true);

	/**
	 * @brief 运行执行监控系统
	 * @details 启动所有的解析器和交易器，开始执行监控
	 */
	void run();

	/**
	 * @brief 释放资源
	 * @details 释放执行监控系统的资源，停止日志系统
	 */
	void release();

	/**
	 * @brief 设置目标仓位
	 * @details 设置单个合约的目标仓位，添加到缓存中
	 * @param stdCode 标准化合约代码
	 * @param targetPos 目标仓位
	 */
	void setPosition(const char* stdCode, double targetPos);

	/**
	 * @brief 提交目标仓位
	 * @details 提交缓存中的所有目标仓位，触发实际交易执行
	 */
	void commitPositions();

	/**
	 * @brief 添加执行器工厂
	 * @details 从指定目录加载执行器工厂模块
	 * @param folder 工厂模块所在的目录
	 * @return 加载是否成功
	 */
	bool addExeFactories(const char* folder);

	/**
	 * @brief 获取基础数据管理器
	 * @return 基础数据管理器指针
	 */
	IBaseDataMgr*	get_bd_mgr() { return &_bd_mgr; }

	/**
	 * @brief 获取主力合约管理器
	 * @return 主力合约管理器指针
	 */
	IHotMgr* get_hot_mgr() { return &_hot_mgr; }

	/**
	 * @brief 获取交易时段信息
	 * @details 根据交易时段ID或合约代码获取交易时段信息
	 * @param sid 交易时段ID或合约代码
	 * @param isCode 是否为合约代码，默认为true
	 * @return 交易时段信息指针
	 */
	WTSSessionInfo* get_session_info(const char* sid, bool isCode = true);

	//////////////////////////////////////////////////////////////////////////
	/**
	 * @brief 处理实时主推行情
	 * @details 实现IParserStub接口的方法，处理实时行情数据
	 * @param curTick 最新的tick数据
	 */
	virtual void handle_push_quote(WTSTickData* curTick) override;

	///////////////////////////////////////////////////////////////////////////
	// IExecuterStub 接口实现
	/**
	 * @brief 获取当前实时时间
	 * @details 实现IExecuterStub接口的方法，返回当前系统的实时时间
	 * @return 当前时间，以毫秒为单位
	 */
	virtual uint64_t get_real_time() override;

	/**
	 * @brief 获取品种信息
	 * @details 实现IExecuterStub接口的方法，根据标准化合约代码获取品种信息
	 * @param stdCode 标准化合约代码
	 * @return 品种信息指针
	 */
	virtual WTSCommodityInfo* get_comm_info(const char* stdCode) override;

	/**
	 * @brief 获取交易时段信息
	 * @details 实现IExecuterStub接口的方法，根据标准化合约代码获取交易时段信息
	 * @param stdCode 标准化合约代码
	 * @return 交易时段信息指针
	 */
	virtual WTSSessionInfo* get_sess_info(const char* stdCode) override;

	/**
	 * @brief 获取主力合约管理器
	 * @details 实现IExecuterStub接口的方法，返回主力合约管理器
	 * @return 主力合约管理器指针
	 */
	virtual IHotMgr* get_hot_mon() override { return &_hot_mgr; }

	/**
	 * @brief 获取交易日
	 * @details 实现IExecuterStub接口的方法，返回当前交易日
	 * @return 交易日，格式YYYYMMDD
	 */
	virtual uint32_t get_trading_day() override;

private:
	/**
	 * @brief 初始化交易器
	 * @details 根据配置初始化交易适配器
	 * @param cfgTrader 交易器配置
	 * @return 初始化是否成功
	 */
	bool initTraders(WTSVariant* cfgTrader);

	/**
	 * @brief 初始化解析器
	 * @details 根据配置初始化行情解析器
	 * @param cfgParser 解析器配置
	 * @return 初始化是否成功
	 */
	bool initParsers(WTSVariant* cfgParser);

	/**
	 * @brief 初始化执行器
	 * @details 根据配置初始化交易执行器
	 * @param cfgExecuter 执行器配置
	 * @return 初始化是否成功
	 */
	bool initExecuters(WTSVariant* cfgExecuter);

	/**
	 * @brief 初始化数据管理器
	 * @details 初始化简单数据管理器
	 * @return 初始化是否成功
	 */
	bool initDataMgr();

	/**
	 * @brief 初始化行为策略
	 * @details 初始化开平仓行为策略
	 * @return 初始化是否成功
	 */
	bool initActionPolicy();

private:
	TraderAdapterMgr	_traders;      ///< 交易适配器管理器
	ParserAdapterMgr	_parsers;      ///< 解析器适配器管理器
	WtExecuterFactory	_exe_factory;  ///< 执行器工厂
	WtExecuterMgr		_exe_mgr;       ///< 执行管理器

	WTSVariant*			_config;        ///< 配置对象

	WtSimpDataMgr		_data_mgr;      ///< 简单数据管理器

	WTSBaseDataMgr		_bd_mgr;        ///< 基础数据管理器
	WTSHotMgr			_hot_mgr;       ///< 主力合约管理器
	ActionPolicyMgr		_act_policy;    ///< 行为策略管理器

	wt_hashmap<std::string, double> _positions;  ///< 目标仓位缓存
};

