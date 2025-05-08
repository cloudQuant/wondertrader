/*!
 * \file ExpParser.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 行情解析器对外接口定义
 * \details 定义了ExpParser类，实现IParserApi接口，提供行情解析和分发功能
 */

#pragma once
#include "../Includes/IParserApi.h"

USING_NS_WTP;

/**
 * @brief 行情解析器类
 * @details 实现IParserApi接口，提供行情数据解析和分发功能，
 *          该类作为外部接口，转发调用到WtDtRunner来实现实际功能
 */
class ExpParser : public IParserApi
{
public:
	/**
	 * @brief 构造函数
	 * @details 创建并初始化一个行情解析器实例
	 * @param id 解析器唯一标识符
	 */
	ExpParser(const char* id):_id(id){}
	
	/**
	 * @brief 析构函数
	 * @details 虚函数，用于清理解析器资源
	 */
	virtual ~ExpParser(){}

public:
	/**
	 * @brief 初始化解析器
	 * @details 使用给定的配置初始化行情解析器
	 * @param config 解析器配置参数
	 * @return 初始化是否成功
	 */
	virtual bool init(WTSVariant* config) override;

	/**
	 * @brief 释放解析器资源
	 * @details 清理解析器占用的资源，在程序退出前调用
	 */
	virtual void release() override;

	/**
	 * @brief 连接行情服务器
	 * @details 建立与行情数据源的连接
	 * @return 连接是否成功
	 */
	virtual bool connect() override;

	/**
	 * @brief 断开行情服务器连接
	 * @details 断开与行情数据源的连接
	 * @return 断开连接是否成功
	 */
	virtual bool disconnect() override;


	/**
	 * @brief 检查连接状态
	 * @details 判断解析器是否已连接到行情数据源
	 * @return 始终返回true，表示已连接
	 */
	virtual bool isConnected() override { return true; }

	/**
	 * @brief 订阅行情代码
	 * @details 订阅指定代码集合的行情数据
	 * @param setCodes 合约代码集合
	 */
	virtual void subscribe(const CodeSet& setCodes) override;

	/**
	 * @brief 取消订阅行情代码
	 * @details 取消订阅指定代码集合的行情数据
	 * @param setCodes 合约代码集合
	 */
	virtual void unsubscribe(const CodeSet& setCodes) override;

	/**
	 * @brief 注册回调接口
	 * @details 注册行情数据处理回调接口，用于接收和处理行情数据
	 * @param listener 行情数据回调接口
	 */
	virtual void registerSpi(IParserSpi* listener) override;

private:
	std::string			_id;            ///< 解析器实例的唯一标识符
	IParserSpi*			m_sink;         ///< 行情数据回调接口
	IBaseDataMgr*		m_pBaseDataMgr;  ///< 基础数据管理器指针
};

