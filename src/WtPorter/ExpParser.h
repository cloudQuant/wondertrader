/*!
 * \file ExpParser.h
 * \project WonderTrader
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 行情解析器导出模块定义
 * \details 定义了行情解析器的导出接口，用于将内部行情解析器接口暴露给外部模块
 */

#pragma once
#include "../Includes/IParserApi.h"

USING_NS_WTP;

/**
 * @brief 行情解析器导出类
 * @details 继承自IParserApi基类，实现了行情解析器的导出接口，
 *          用于将内部行情解析器功能暴露给外部模块调用
 */
class ExpParser : public IParserApi
{
public:
	/**
	 * @brief 构造函数
	 * @param id 解析器ID
	 */
	ExpParser(const char* id):_id(id){}
	
	/**
	 * @brief 析构函数
	 */
	virtual ~ExpParser(){}

public:
	/**
	 * @brief 初始化解析器
	 * @details 调用运行器的解析器初始化接口
	 * @param config 配置项
	 * @return 是否初始化成功
	 */
	virtual bool init(WTSVariant* config) override;


	/**
	 * @brief 释放解析器资源
	 * @details 调用运行器的解析器释放接口
	 */
	virtual void release() override;


	/**
	 * @brief 连接行情数据源
	 * @details 调用运行器的解析器连接接口
	 * @return 是否连接成功
	 */
	virtual bool connect() override;


	/**
	 * @brief 断开行情数据源连接
	 * @details 调用运行器的解析器断开连接接口
	 * @return 是否断开成功
	 */
	virtual bool disconnect() override;


	/**
	 * @brief 检查是否已连接
	 * @details 始终返回true，表示连接状态良好
	 * @return 是否已连接
	 */
	virtual bool isConnected() override { return true; }


	/**
	 * @brief 订阅行情数据
	 * @details 逐个调用运行器的解析器订阅接口
	 * @param setCodes 要订阅的合约代码集合
	 */
	virtual void subscribe(const CodeSet& setCodes) override;


	/**
	 * @brief 取消订阅行情数据
	 * @details 逐个调用运行器的解析器取消订阅接口
	 * @param setCodes 要取消订阅的合约代码集合
	 */
	virtual void unsubscribe(const CodeSet& setCodes) override;


	/**
	 * @brief 注册回调接口
	 * @details 设置回调接口并获取基础数据管理器
	 * @param listener 回调接口指针
	 */
	virtual void registerSpi(IParserSpi* listener) override;

private:
	std::string			_id;            /**< 解析器ID */
	IParserSpi*			m_sink;         /**< 解析器回调接口 */
	IBaseDataMgr*		m_pBaseDataMgr; /**< 基础数据管理器 */
};

