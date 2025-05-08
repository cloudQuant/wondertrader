/*!
 * \file WtDtPorter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief WonderTrader数据处理模块的C接口实现
 * \details 实现了WonderTrader数据处理模块的所有对外接口功能，
 *          包括模块初始化、数据接收和导出、解析器和导出器的创建等
 *          主要作为与外部系统集成的通道，将调用转发到WtDtRunner实例
 */
#include "WtDtPorter.h"
#include "WtDtRunner.h"

#include "../WtDtCore/WtHelper.h"
#include "../WTSTools/WTSLogger.h"

#include "../Share/ModuleHelper.hpp"
#include "../Includes/WTSVersion.h"

#ifdef _WIN32
#ifdef _WIN64
/**
 * @brief 平台名称常量，64位版本
 * @details 用于可执行文件版本标识，指示64位Windows平台
 */
char PLATFORM_NAME[] = "X64";
#else
/**
 * @brief 平台名称常量，32位版本
 * @details 用于可执行文件版本标识，指示32位Windows平台
 */
char PLATFORM_NAME[] = "X86";
#endif
#else
/**
 * @brief 平台名称常量，UNIX版本
 * @details 用于可执行文件版本标识，指示UNIX/Linux平台
 */
char PLATFORM_NAME[] = "UNIX";
#endif

#ifdef _MSC_VER
#include "../Common/mdump.h"
#include <boost/filesystem.hpp>

/**
 * @brief 获取模块名称
 * @details 获取当前动态链接库模块的文件名，用于生成崩溃转储时的标识
 * @return 模块文件名字符串
 */
const char* getModuleName()
{
	static char MODULE_NAME[250] = { 0 };
	if (strlen(MODULE_NAME) == 0)
	{
		// 获取当前模块文件路径
		GetModuleFileName(g_dllModule, MODULE_NAME, 250);
		// 提取文件名部分
		boost::filesystem::path p(MODULE_NAME);
		strcpy(MODULE_NAME, p.filename().string().c_str());
	}

	return MODULE_NAME;
}
#endif

/**
 * @brief 获取全局数据处理运行器实例
 * @details 获取WtDtRunner的全局单例，采用单例模式确保在整个运行期间只有一个实例
 * @return WtDtRunner& 数据处理运行器实例的引用
 */
WtDtRunner& getRunner()
{
	// 使用静态局部变量确保单例模式
	static WtDtRunner runner;
	return runner;
}

/**
 * @brief 初始化数据处理模块
 * @details 根据配置初始化数据处理模块，在Windows上还会启用崩溃转储功能
 * @param cfgFile 配置文件路径或内容
 * @param logCfg 日志配置文件路径或内容
 * @param bCfgFile 是否为配置文件路径，true表示文件路径，false表示内存配置内容
 * @param bLogCfgFile 是否为日志配置文件路径，true表示文件路径，false表示内存配置内容
 */
void initialize(WtString cfgFile, WtString logCfg, bool bCfgFile, bool bLogCfgFile)
{
#ifdef _MSC_VER
	// 在Windows平台上启用崩溃转储功能
	CMiniDumper::Enable(getModuleName(), true, WtHelper::get_cwd());
#endif
	// 调用运行器的初始化方法
	getRunner().initialize(cfgFile, logCfg, getBinDir(), bCfgFile, bLogCfgFile);
}

/**
 * @brief 启动数据处理模块
 * @details 启动数据处理模块，开始正常工作
 * @param bAsync 是否异步启动，true表示异步启动，false表示同步启动
 */
void start(bool bAsync/* = false*/)
{
	// 调用运行器的启动方法
	getRunner().start(bAsync);
}

/**
 * @brief 获取模块版本信息
 * @details 返回包含平台名称、版本号和编译日期时间的完整版本信息
 * @return 版本信息字符串
 */
const char* get_version()
{
	// 使用静态字符串缓存版本信息，避免重复构建
	static std::string _ver;
	if (_ver.empty())
	{
		// 构建版本信息字符串，包含平台名称、版本号和编译日期时间
		_ver = PLATFORM_NAME;
		_ver += " ";
		_ver += WT_VERSION;
		_ver += " Build@";
		_ver += __DATE__;
		_ver += " ";
		_ver += __TIME__;
	}
	return _ver.c_str();
}

/**
 * @brief 写入日志
 * @details 将指定级别的日志写入指定的日志类别中
 * @param level 日志级别，参见WTSLogLevel枚举
 * @param message 日志消息内容
 * @param catName 日志类别名称，空字符串表示使用默认类别
 */
void write_log(unsigned int level, const char* message, const char* catName)
{
	// 如果指定了日志类别名称
	if (strlen(catName) > 0)
	{
		// 使用指定类别写入日志
		WTSLogger::log_raw_by_cat(catName, (WTSLogLevel)level, message);
	}
	else
	{
		// 使用默认类别写入日志
		WTSLogger::log_raw((WTSLogLevel)level, message);
	}
}

#pragma region "扩展Parser接口"

/**
 * @brief 创建扩展解析器
 * @details 创建一个外部扩展的行情解析器
 * @param id 解析器标识符，必须全局唯一
 * @return 创建是否成功，true成功，false失败
 */
bool create_ext_parser(const char* id)
{
	// 调用运行器的方法创建扩展解析器
	return getRunner().createExtParser(id);
}

/**
 * @brief 推送行情数据
 * @details 将tick行情数据推送给数据处理模块进行处理
 * @param id 解析器标识符
 * @param curTick 当前tick数据结构指针
 * @param uProcFlag 处理标记，参见接口定义
 */
void parser_push_quote(const char* id, WTSTickStruct* curTick, WtUInt32 uProcFlag)
{
	// 调用运行器的方法处理外部解析器推送的行情数据
	getRunner().on_ext_parser_quote(id, curTick, uProcFlag);
}

/**
 * @brief 注册解析器回调函数
 * @details 注册行情解析器的事件和订阅回调函数
 * @param cbEvt 行情解析器事件回调函数
 * @param cbSub 行情订阅结果回调函数
 */
void register_parser_callbacks(FuncParserEvtCallback cbEvt, FuncParserSubCallback cbSub)
{
	// 调用运行器的方法注册解析器回调函数
	getRunner().registerParserPorter(cbEvt, cbSub);
}


#pragma endregion "扩展Parser接口"

#pragma region "扩展Dumper接口"
/**
 * @brief 创建扩展数据导出器
 * @details 创建一个外部扩展的数据导出器
 * @param id 导出器标识符，必须全局唯一
 * @return 创建是否成功，true成功，false失败
 */
bool create_ext_dumper(const char* id)
{
	// 调用运行器的方法创建扩展导出器
	return getRunner().createExtDumper(id);
}

/**
 * @brief 注册扩展数据导出器的基础数据回调
 * @details 注册K线和适价数据的导出回调函数
 * @param barDumper K线数据导出回调函数
 * @param tickDumper 适价数据导出回调函数
 */
void register_extended_dumper(FuncDumpBars barDumper, FuncDumpTicks tickDumper)
{
	// 调用运行器的方法注册扩展导出器的基础数据回调
	getRunner().registerExtDumper(barDumper, tickDumper);
}

/**
 * @brief 注册扩展数据导出器的高频数据回调
 * @details 注册委托队列、委托明细和成交数据的导出回调函数
 * @param ordQueDumper 委托队列数据导出回调函数
 * @param ordDtlDumper 委托明细数据导出回调函数
 * @param transDumper 成交数据导出回调函数
 */
void register_extended_hftdata_dumper(FuncDumpOrdQue ordQueDumper, FuncDumpOrdDtl ordDtlDumper, FuncDumpTrans transDumper)
{
	// 调用运行器的方法注册扩展导出器的高频数据回调
	getRunner().registerExtHftDataDumper(ordQueDumper, ordDtlDumper, transDumper);
}
#pragma endregion "扩展Dumper接口"

