/*!
 * \file ExpParser.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 行情解析器的实现文件
 * \details 实现了ExpParser类的各种方法，通过将调用转发给WtDtRunner完成实际功能
 *          包括初始化、连接、订阅行情和回调注册等功能
 */

#include "ExpParser.h"
#include "WtDtRunner.h"

/**
 * @brief 获取WtDtRunner全局实例的外部函数声明
 * @details 该函数由外部实现，用于获取WtDtRunner的全局单例
 * @return WtDtRunner& WtDtRunner的引用
 */
extern WtDtRunner& getRunner();

/**
 * @brief 初始化解析器
 * @details 调用WtDtRunner的parser_init方法初始化行情解析器
 * @param config 解析器配置参数，当前实现中未使用
 * @return 始终返回true，表示初始化成功
 */
bool ExpParser::init(WTSVariant* config)
{
	// 调用WtDtRunner的相应方法来初始化解析器
	getRunner().parser_init(_id.c_str());
	return true;
}

/**
 * @brief 释放解析器资源
 * @details 调用WtDtRunner的parser_release方法释放解析器资源
 */
void ExpParser::release()
{
	// 调用WtDtRunner的相应方法来释放解析器资源
	getRunner().parser_release(_id.c_str());
}

/**
 * @brief 连接行情服务器
 * @details 调用WtDtRunner的parser_connect方法连接到行情数据源
 * @return 始终返回true，表示连接成功
 */
bool ExpParser::connect()
{
	// 调用WtDtRunner的相应方法来连接行情服务器
	getRunner().parser_connect(_id.c_str());
	return true;
}

/**
 * @brief 断开行情服务器连接
 * @details 调用WtDtRunner的parser_disconnect方法断开与行情数据源的连接
 * @return 始终返回true，表示断开连接成功
 */
bool ExpParser::disconnect()
{
	// 调用WtDtRunner的相应方法来断开行情服务器连接
	getRunner().parser_disconnect(_id.c_str());
	return true;
}

/**
 * @brief 订阅行情代码
 * @details 遂个处理代码集合中的每个合约代码，调用WtDtRunner的parser_subscribe方法进行订阅
 * @param setCodes 需要订阅的合约代码集合
 */
void ExpParser::subscribe(const CodeSet& setCodes)
{
	// 遂个遍历代码集合，通过WtDtRunner订阅每个合约的行情
	for(const auto& code : setCodes)
		getRunner().parser_subscribe(_id.c_str(), code.c_str());
}

/**
 * @brief 取消订阅行情代码
 * @details 遂个处理代码集合中的每个合约代码，调用WtDtRunner的parser_unsubscribe方法取消订阅
 * @param setCodes 需要取消订阅的合约代码集合
 */
void ExpParser::unsubscribe(const CodeSet& setCodes)
{
	// 遂个遍历代码集合，通过WtDtRunner取消订阅每个合约的行情
	for (const auto& code : setCodes)
		getRunner().parser_unsubscribe(_id.c_str(), code.c_str());
}

/**
 * @brief 注册行情数据回调接口
 * @details 设置行情数据回调对象，并获取基础数据管理器
 * @param listener 行情数据回调接口指针
 */
void ExpParser::registerSpi(IParserSpi* listener)
{
	// 设置回调接口
	m_sink = listener;

	// 如果回调接口有效，获取基础数据管理器
	if (m_sink)
		m_pBaseDataMgr = m_sink->getBaseDataMgr();
}
