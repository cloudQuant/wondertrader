/*!
 * \file WtDtPorter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief WonderTrader数据处理模块的C接口定义
 * \details 定义了WonderTrader数据处理模块的所有对外接口，包括模块初始化、解析器和导出器的创建、
 *          回调函数的注册等功能，为引擎与插件深入集成提供支持
 */
#pragma once
#include "PorterDefs.h"


#ifdef __cplusplus
extern "C"
{
#endif

	/**
	 * @brief 初始化数据处理模块
	 * @details 根据给定的配置文件初始化数据处理模块
	 * @param cfgFile 配置文件路径
	 * @param logCfg 日志配置文件路径
	 * @param bCfgFile 是否为文件路径，true表示文件路径，false表示内存配置内容
	 * @param bLogCfgFile 是否为日志配置文件路径，true表示文件路径，false表示内存配置内容
	 */
	EXPORT_FLAG void		initialize(WtString cfgFile, WtString logCfg, bool bCfgFile, bool bLogCfgFile);
	
	/**
	 * @brief 启动数据处理模块
	 * @details 启动数据处理模块，开始正常工作
	 * @param bAsync 是否异步启动，true表示异步启动，false表示同步启动
	 */
	EXPORT_FLAG void		start(bool bAsync = false);

	/**
	 * @brief 获取模块版本信息
	 * @details 返回包含平台名称、版本号和编译日期的字符串
	 * @return 版本信息字符串
	 */
	EXPORT_FLAG	WtString	get_version();
	
	/**
	 * @brief 写入日志
	 * @details 将指定级别的日志写入指定的日志类别
	 * @param level 日志级别，参见WTSLogLevel枚举
	 * @param message 日志消息内容
	 * @param catName 日志类别名称，空字符串表示使用默认类别
	 */
	EXPORT_FLAG	void		write_log(unsigned int level, const char* message, const char* catName);


#pragma region "扩展Parser接口"
	/**
	 * @brief 创建扩展解析器
	 * @details 创建一个外部扩展的行情解析器
	 * @param id 解析器标识符，必须全局唯一
	 * @return 创建是否成功，true成功，false失败
	 */
	EXPORT_FLAG	bool		create_ext_parser(const char* id);

	/**
	 * @brief 注册解析器回调函数
	 * @details 注册行情解析器的事件和订阅回调函数
	 * @param cbEvt 行情解析器事件回调函数，处理连接、断开连接、初始化、释放等事件
	 * @param cbSub 行情订阅结果回调函数，处理订阅和取消订阅操作
	 */
	EXPORT_FLAG void		register_parser_callbacks(FuncParserEvtCallback cbEvt, FuncParserSubCallback cbSub);

	/**
	 * @brief 推送行情数据
	 * @details 将tick行情数据推送给数据处理模块进行处理
	 * @param id 解析器标识符
	 * @param curTick 当前tick数据结构指针
	 * @param uProcFlag 处理标记：0-切片行情，无需处理(ParserUDP)；1-完整快照，需要切片(国内各路通道)；2-極简快照，需要缓存累加（主要针对日线、tick，m1和m5都是自动累加的，虚拟货币行情）
	 */
	EXPORT_FLAG	void		parser_push_quote(const char* id, WTSTickStruct* curTick, WtUInt32 uProcFlag);
#pragma endregion "扩展Parser接口"

#pragma region "扩展Dumper接口"
	/**
	 * @brief 创建扩展数据导出器
	 * @details 创建一个外部扩展的数据导出器
	 * @param id 导出器标识符，必须全局唯一
	 * @return 创建是否成功，true成功，false失败
	 */
	EXPORT_FLAG	bool		create_ext_dumper(const char* id);

	/**
	 * @brief 注册扩展数据导出器的基础数据回调
	 * @details 注册K线和适价数据的导出回调函数
	 * @param barDumper K线数据导出回调函数
	 * @param tickDumper 适价数据导出回调函数
	 */
	EXPORT_FLAG void		register_extended_dumper(FuncDumpBars barDumper, FuncDumpTicks tickDumper);

	/**
	 * @brief 注册扩展数据导出器的高频数据回调
	 * @details 注册委托队列、委托明细和成交数据的导出回调函数
	 * @param ordQueDumper 委托队列数据导出回调函数
	 * @param ordDtlDumper 委托明细数据导出回调函数
	 * @param transDumper 成交数据导出回调函数
	 */
	EXPORT_FLAG void		register_extended_hftdata_dumper(FuncDumpOrdQue ordQueDumper, FuncDumpOrdDtl ordDtlDumper, FuncDumpTrans transDumper);
#pragma endregion "扩展Dumper接口"

#ifdef __cplusplus
}
#endif