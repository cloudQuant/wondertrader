/*!
 * \file WtDtRunner.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief WonderTrader数据处理模块的运行器定义
 * \details 定义了WonderTrader数据处理模块的核心运行器类，负责数据模块的初始化、运行和管理
 *          包括数据解析器和导出器的管理、行情数据的处理和转发等功能
 */
#pragma once
#include "PorterDefs.h"
#include "ExpDumper.h"

#include "../WtDtCore/DataManager.h"
#include "../WtDtCore/ParserAdapter.h"
#include "../WtDtCore/StateMonitor.h"
#include "../WtDtCore/UDPCaster.h"
#include "../WtDtCore/IndexFactory.h"
#include "../WtDtCore/ShmCaster.h"

#include "../WTSTools/WTSHotMgr.h"
#include "../WTSTools/WTSBaseDataMgr.h"

#include <boost/asio.hpp>

NS_WTP_BEGIN
class WTSVariant;
NS_WTP_END

/**
 * @brief WonderTrader数据处理模块的运行器类
 * @details 负责数据处理模块的初始化、配置和运行，管理数据解析器和导出器
 *          实现了外部接口的转发和回调处理，是数据处理模块的核心类
 */
class WtDtRunner
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化运行器对象，设置所有回调指针为空，退出标志为false
	 */
	WtDtRunner();

	/**
	 * @brief 析构函数
	 * @details 清理运行器对象的资源
	 */
	~WtDtRunner();

public:
	/**
	 * @brief 初始化数据处理模块
	 * @details 加载配置文件，初始化日志、基础数据、解析器等组件
	 * @param cfgFile 配置文件路径或内容
	 * @param logCfg 日志配置文件路径或内容
	 * @param modDir 模块目录，默认为空字符串
	 * @param bCfgFile 是否为文件路径，true表示文件路径，false表示内存配置内容
	 * @param bLogCfgFile 是否为日志配置文件路径，true表示文件路径，false表示内存配置内容
	 */
	void	initialize(const char* cfgFile, const char* logCfg, const char* modDir = "", bool bCfgFile = true, bool bLogCfgFile = true);
	
	/**
	 * @brief 启动数据处理模块
	 * @details 启动数据解析器和状态监控器，开始处理数据
	 * @param bAsync 是否异步启动，true表示异步启动，false表示同步启动
	 * @param bAlldayMode 是否全天候模式，true表示全天候模式（不进行交易时段判断），false表示根据交易时段运行
	 */
	void	start(bool bAsync = false, bool bAlldayMode = false);

	/**
	 * @brief 创建扩展解析器
	 * @details 创建一个外部扩展的行情解析器
	 * @param id 解析器标识符，必须全局唯一
	 * @return 创建是否成功，true成功，false失败
	 */
	bool	createExtParser(const char* id);


//////////////////////////////////////////////////////////////////////////
/**
 * @brief 扩展解析器相关方法
 * @details 这组方法用于处理外部解析器的事件和数据
 */
public:
	/**
	 * @brief 注册解析器回调函数
	 * @details 注册行情解析器的事件和订阅回调函数
	 * @param cbEvt 行情解析器事件回调函数，处理连接、断开连接、初始化、释放等事件
	 * @param cbSub 行情订阅结果回调函数，处理订阅和取消订阅操作
	 */
	void registerParserPorter(FuncParserEvtCallback cbEvt, FuncParserSubCallback cbSub);

	/**
	 * @brief 通知解析器初始化事件
	 * @details 触发解析器初始化事件回调
	 * @param id 解析器标识符
	 */
	void parser_init(const char* id);

	/**
	 * @brief 通知解析器连接事件
	 * @details 触发解析器连接事件回调
	 * @param id 解析器标识符
	 */
	void parser_connect(const char* id);

	/**
	 * @brief 通知解析器释放事件
	 * @details 触发解析器释放事件回调
	 * @param id 解析器标识符
	 */
	void parser_release(const char* id);

	/**
	 * @brief 通知解析器断开连接事件
	 * @details 触发解析器断开连接事件回调
	 * @param id 解析器标识符
	 */
	void parser_disconnect(const char* id);

	/**
	 * @brief 为解析器订阅行情
	 * @details 触发解析器订阅回调
	 * @param id 解析器标识符
	 * @param code 合约代码
	 */
	void parser_subscribe(const char* id, const char* code);

	/**
	 * @brief 为解析器取消订阅行情
	 * @details 触发解析器取消订阅回调
	 * @param id 解析器标识符
	 * @param code 合约代码
	 */
	void parser_unsubscribe(const char* id, const char* code);

	/**
	 * @brief 处理外部解析器的行情数据
	 * @details 接收并处理外部解析器推送的行情数据
	 * @param id 解析器标识符
	 * @param curTick 当前tick数据结构指针
	 * @param uProcFlag 处理标记：0-切片行情，无需处理；1-完整快照，需要切片；2-极简快照，需要缓存累加
	 */
	void on_ext_parser_quote(const char* id, WTSTickStruct* curTick, uint32_t uProcFlag);

//////////////////////////////////////////////////////////////////////////
/**
 * @brief 扩展导出器相关方法
 * @details 这组方法用于处理外部导出器的创建、注册和数据导出
 */
public:
	/**
	 * @brief 创建扩展数据导出器
	 * @details 创建一个外部扩展的数据导出器
	 * @param id 导出器标识符，必须全局唯一
	 * @return 创建是否成功，true成功，false失败
	 */
	bool createExtDumper(const char* id);

	/**
	 * @brief 注册扩展数据导出器的基础数据回调
	 * @details 注册K线和适价数据的导出回调函数
	 * @param barDumper K线数据导出回调函数
	 * @param tickDumper 适价数据导出回调函数
	 */
	void registerExtDumper(FuncDumpBars barDumper, FuncDumpTicks tickDumper);

	/**
	 * @brief 注册扩展数据导出器的高频数据回调
	 * @details 注册委托队列、委托明细和成交数据的导出回调函数
	 * @param ordQueDumper 委托队列数据导出回调函数
	 * @param ordDtlDumper 委托明细数据导出回调函数
	 * @param transDumper 成交数据导出回调函数
	 */
	void registerExtHftDataDumper(FuncDumpOrdQue ordQueDumper, FuncDumpOrdDtl ordDtlDumper, FuncDumpTrans transDumper);

	/**
	 * @brief 导出K线历史数据
	 * @details 将历史K线数据导出到外部存储引擎
	 * @param id 导出器标识符
	 * @param stdCode 标准化合约代码，格式如 SHFE.rb.HOT
	 * @param period 周期码，如m1/m5/d1等
	 * @param bars K线数据数组
	 * @param count 数组长度
	 * @return 处理是否成功
	 */
	bool dumpHisBars(const char* id, const char* stdCode, const char* period, WTSBarStruct* bars, uint32_t count);

	/**
	 * @brief 导出适价历史数据
	 * @details 将指定日期的适价数据导出到外部存储引擎
	 * @param id 导出器标识符
	 * @param stdCode 标准化合约代码，格式如 SHFE.rb.HOT
	 * @param uDate 交易日期，格式为YYYYMMDD
	 * @param ticks 适价数据数组
	 * @param count 数组长度
	 * @return 处理是否成功
	 */
	bool dumpHisTicks(const char* id, const char* stdCode, uint32_t uDate, WTSTickStruct* ticks, uint32_t count);

	/**
	 * @brief 导出委托队列历史数据
	 * @details 将指定日期的委托队列数据导出到外部存储引擎
	 * @param id 导出器标识符
	 * @param stdCode 标准化合约代码，格式如 SHFE.rb.HOT
	 * @param uDate 交易日期，格式为YYYYMMDD
	 * @param item 委托队列数据数组
	 * @param count 数组长度
	 * @return 处理是否成功
	 */
	bool dumpHisOrdQue(const char* id, const char* stdCode, uint32_t uDate, WTSOrdQueStruct* item, uint32_t count);

	/**
	 * @brief 导出委托明细历史数据
	 * @details 将指定日期的委托明细数据导出到外部存储引擎
	 * @param id 导出器标识符
	 * @param stdCode 标准化合约代码，格式如 SHFE.rb.HOT
	 * @param uDate 交易日期，格式为YYYYMMDD
	 * @param items 委托明细数据数组
	 * @param count 数组长度
	 * @return 处理是否成功
	 */
	bool dumpHisOrdDtl(const char* id, const char* stdCode, uint32_t uDate, WTSOrdDtlStruct* items, uint32_t count);

	/**
	 * @brief 导出成交历史数据
	 * @details 将指定日期的成交数据导出到外部存储引擎
	 * @param id 导出器标识符
	 * @param stdCode 标准化合约代码，格式如 SHFE.rb.HOT
	 * @param uDate 交易日期，格式为YYYYMMDD
	 * @param items 成交数据数组
	 * @param count 数组长度
	 * @return 处理是否成功
	 */
	bool dumpHisTrans(const char* id, const char* stdCode, uint32_t uDate, WTSTransStruct* items, uint32_t count);

private:
	/**
	 * @brief 初始化数据管理器
	 * @details 根据配置初始化数据管理器的相关参数
	 * @param config 配置项指针
	 * @param bAlldayMode 是否全天候模式
	 */
	void initDataMgr(WTSVariant* config, bool bAlldayMode = false);

	/**
	 * @brief 初始化解析器
	 * @details 根据配置初始化所有的数据解析器
	 * @param cfg 解析器配置项指针
	 */
	void initParsers(WTSVariant* cfg);

private:
	//! 基础数据管理器，管理合约、交易时间等基础数据
	WTSBaseDataMgr	_bd_mgr;
	//! 主力合约管理器，管理主力合约映射关系
	WTSHotMgr		_hot_mgr;
	//! 异步IO服务，用于异步事件处理
	boost::asio::io_service _async_io;
	//! 状态监控器，监控系统状态
	StateMonitor	_state_mon;
	//! UDP广播器，用于将数据通过UDP广播出去
	UDPCaster		_udp_caster;
	//! 数据共享管理器，用于进程间数据共享
	ShmCaster		_shm_caster;
	//! 数据管理器，负责数据的缓存和管理
	DataManager		_data_mgr;
	//! 指标工厂，用于创建和管理指标
	IndexFactory	_idx_factory;
	//! 行情接入适配器管理器，管理所有数据解析器
	ParserAdapterMgr	_parsers;

	//! 解析器事件回调函数
	FuncParserEvtCallback	_cb_parser_evt;
	//! 解析器订阅回调函数
	FuncParserSubCallback	_cb_parser_sub;

	//! K线数据导出回调函数
	FuncDumpBars	_dumper_for_bars;
	//! 适价数据导出回调函数
	FuncDumpTicks	_dumper_for_ticks;

	//! 委托队列数据导出回调函数
	FuncDumpOrdQue	_dumper_for_ordque;
	//! 委托明细数据导出回调函数
	FuncDumpOrdDtl	_dumper_for_orddtl;
	//! 成交数据导出回调函数
	FuncDumpTrans	_dumper_for_trans;

	//! 导出器智能指针类型定义
	typedef std::shared_ptr<ExpDumper> ExpDumperPtr;
	//! 导出器映射表类型定义，将导出器ID映射到导出器实例
	typedef std::map<std::string, ExpDumperPtr>  ExpDumpers;
	//! 数据导出器集合，管理所有的数据导出器
	ExpDumpers		_dumpers;

	//! 退出标志，标记是否需要退出处理
	bool _to_exit;
};

