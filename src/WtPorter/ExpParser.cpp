/*!
 * \file ExpParser.cpp
 * \project WonderTrader
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 行情解析器导出模块实现
 * \details 实现了行情解析器的导出接口，用于将内部行情解析器接口暴露给外部模块
 */

#include "ExpParser.h"
#include "WtRtRunner.h"

/**
 * @brief 获取运行器实例的外部函数声明
 * @return WtRtRunner& 运行器实例的引用
 */
extern WtRtRunner& getRunner();

/**
 * @brief 初始化解析器
 * @details 调用运行器的解析器初始化接口
 * @param config 配置项，当前未使用
 * @return 始终返回true，表示初始化成功
 */
bool ExpParser::init(WTSVariant* config)
{
	// 调用运行器的解析器初始化接口，传入解析器ID
	getRunner().parser_init(_id.c_str());
	return true;
}

/**
 * @brief 释放解析器资源
 * @details 调用运行器的解析器释放接口
 */
void ExpParser::release()
{
	// 调用运行器的解析器释放接口，传入解析器ID
	getRunner().parser_release(_id.c_str());
}

/**
 * @brief 连接行情数据源
 * @details 调用运行器的解析器连接接口
 * @return 始终返回true，表示连接成功
 */
bool ExpParser::connect()
{
	// 调用运行器的解析器连接接口，传入解析器ID
	getRunner().parser_connect(_id.c_str());
	return true;
}

/**
 * @brief 断开行情数据源连接
 * @details 调用运行器的解析器断开连接接口
 * @return 始终返回true，表示断开成功
 */
bool ExpParser::disconnect()
{
	// 调用运行器的解析器断开连接接口，传入解析器ID
	getRunner().parser_disconnect(_id.c_str());
	return true;
}

/**
 * @brief 订阅行情数据
 * @details 逐个调用运行器的解析器订阅接口
 * @param setCodes 要订阅的合约代码集合
 */
void ExpParser::subscribe(const CodeSet& setCodes)
{
	// 遍历合约代码集合，逐个订阅
	for(const auto& code : setCodes)
		// 调用运行器的解析器订阅接口，传入解析器ID和合约代码
		getRunner().parser_subscribe(_id.c_str(), code.c_str());
}

/**
 * @brief 取消订阅行情数据
 * @details 逐个调用运行器的解析器取消订阅接口
 * @param setCodes 要取消订阅的合约代码集合
 */
void ExpParser::unsubscribe(const CodeSet& setCodes)
{
	// 遍历合约代码集合，逐个取消订阅
	for (const auto& code : setCodes)
		// 调用运行器的解析器取消订阅接口，传入解析器ID和合约代码
		getRunner().parser_unsubscribe(_id.c_str(), code.c_str());
}

/**
 * @brief 注册回调接口
 * @details 设置回调接口并获取基础数据管理器
 * @param listener 回调接口指针
 */
void ExpParser::registerSpi(IParserSpi* listener)
{
	// 设置回调接口
	m_sink = listener;

	// 如果回调接口有效，获取基础数据管理器
	if (m_sink)
		m_pBaseDataMgr = m_sink->getBaseDataMgr();
}
