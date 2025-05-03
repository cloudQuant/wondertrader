/*!
 * \file ParserAdapter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 行情解析模块适配类定义
 * 
 * \details 本文件定义了行情解析模块的适配器类，用于封装不同的行情解析器，
 * 并提供统一的接口进行行情数据的处理。
 * 包括行情解析器的初始化、运行、数据处理等功能。
 */
#pragma once
#include <set>
#include <vector>
#include <memory>
#include <boost/core/noncopyable.hpp>
#include "../Includes/IParserApi.h"

NS_WTP_BEGIN
class WTSVariant;
NS_WTP_END

USING_NS_WTP;
class WTSBaseDataMgr;
class WtDtRunner;

/**
 * @brief 行情解析器适配器类
 * 
 * @details 该类实现了IParserSpi接口，用于适配不同的行情解析器并处理行情数据。
 * 它封装了底层的行情解析器API，并将接收到的行情数据转发给数据管理器。
 * 类中实现了对交易所和合约代码的过滤功能。
 */
class ParserAdapter : public IParserSpi, private boost::noncopyable
{
public:
	/**
 * @brief 构造函数
 * @param bgMgr 基础数据管理器指针
 * @param runner 数据服务运行器指针
 */
ParserAdapter(WTSBaseDataMgr * bgMgr, WtDtRunner* runner);
	/**
 * @brief 析构函数
 */
~ParserAdapter();

public:
	/**
 * @brief 初始化解析器适配器
 * @param id 适配器标识符
 * @param cfg 配置项
 * @return bool 初始化是否成功
 * 
 * @details 根据配置创建并初始化行情解析器，设置过滤器等
 */
bool	init(const char* id, WTSVariant* cfg);

	/**
 * @brief 使用外部提供的解析器API初始化
 * @param id 适配器标识符
 * @param api 外部提供的解析器API指针
 * @return bool 初始化是否成功
 * 
 * @details 使用外部提供的解析器API初始化适配器，而不是通过工厂创建
 */
bool	initExt(const char* id, IParserApi* api);

	void	release();

	/**
 * @brief 启动解析器
 * @return bool 启动是否成功
 * 
 * @details 启动底层行情解析器，开始接收行情数据
 */
bool	run();

	/**
 * @brief 获取适配器标识符
 * @return const char* 适配器标识符
 */
const char* id() const { return _id.c_str(); }

public:
	/**
	 * @brief 处理合约列表
	 * @param aySymbols 合约列表数组
	 * 
	 * @details 实现IParserSpi接口的合约列表处理方法，接收并处理行情解析器返回的合约列表
	 */
	virtual void handleSymbolList(const WTSArray* aySymbols) override;

	/**
	 * @brief 处理Tick行情数据
	 * @param quote Tick数据指针
	 * @param procFlag 处理标志
	 * 
	 * @details 实现IParserSpi接口的Tick数据处理方法，接收并处理行情解析器返回的Tick数据
	 */
	virtual void handleQuote(WTSTickData *quote, uint32_t procFlag) override;

	/**
	 * @brief 处理委托队列数据
	 * @param ordQueData 委托队列数据指针
	 * 
	 * @details 实现IParserSpi接口的委托队列数据处理方法，接收并处理行情解析器返回的委托队列数据
	 */
	virtual void handleOrderQueue(WTSOrdQueData* ordQueData) override;

	/**
	 * @brief 处理成交数据
	 * @param transData 成交数据指针
	 * 
	 * @details 实现IParserSpi接口的成交数据处理方法，接收并处理行情解析器返回的成交数据
	 */
	virtual void handleTransaction(WTSTransData* transData) override;

	/**
	 * @brief 处理委托明细数据
	 * @param ordDetailData 委托明细数据指针
	 * 
	 * @details 实现IParserSpi接口的委托明细数据处理方法，接收并处理行情解析器返回的委托明细数据
	 */
	virtual void handleOrderDetail(WTSOrdDtlData* ordDetailData) override;

	/**
	 * @brief 处理解析器日志
	 * @param ll 日志级别
	 * @param message 日志消息
	 * 
	 * @details 实现IParserSpi接口的日志处理方法，接收并处理行情解析器返回的日志信息
	 */
	virtual void handleParserLog(WTSLogLevel ll, const char* message) override;

	/**
	 * @brief 获取基础数据管理器
	 * @return IBaseDataMgr* 基础数据管理器指针
	 * 
	 * @details 实现IParserSpi接口的获取基础数据管理器方法，返回内部保存的基础数据管理器指针
	 */
	virtual IBaseDataMgr* getBaseDataMgr() override;

private:
	/**
	 * @brief 行情解析器API指针
	 * @details 实际的行情解析器实例，用于执行具体的行情获取和处理操作
	 */
	IParserApi*			_parser_api;
	/**
	 * @brief 解析器实例删除函数
	 * @details 用于安全删除行情解析器实例的函数指针
	 */
	FuncDeleteParser	_remover;
	/**
	 * @brief 基础数据管理器指针
	 * @details 用于管理合约、交易所等基础数据
	 */
	WTSBaseDataMgr*		_bd_mgr;
	/**
	 * @brief 数据服务运行器指针
	 * @details 用于将接收到的行情数据转发给数据服务运行器进行处理
	 */
	WtDtRunner*			_dt_runner;

	/**
	 * @brief 是否已停止标志
	 * @details 用于标记解析器是否已停止运行
	 */
	bool				_stopped;

	/**
	 * @brief 交易所过滤器类型定义
	 * @details 用于存储需要过滤的交易所代码或合约代码
	 */
	typedef wt_hashset<std::string>	ExchgFilter;
	/**
	 * @brief 交易所过滤器
	 * @details 用于过滤特定交易所的行情数据
	 */
	ExchgFilter			_exchg_filter;
	/**
	 * @brief 合约代码过滤器
	 * @details 用于过滤特定合约的行情数据
	 */
	ExchgFilter			_code_filter;
	/**
	 * @brief 配置项指针
	 * @details 存储解析器的配置信息
	 */
	WTSVariant*			_cfg;
	/**
	 * @brief 适配器标识符
	 * @details 用于唯一标识当前适配器实例
	 */
	std::string			_id;
};

typedef std::shared_ptr<ParserAdapter>	ParserAdapterPtr;
typedef wt_hashmap<std::string, ParserAdapterPtr>	ParserAdapterMap;

/**
 * @brief 行情解析器适配器管理器
 * 
 * @details 该类用于管理多个行情解析器适配器实例，提供添加、获取、运行和释放适配器的方法。
 * 类中使用哈希表存储所有适配器实例，以标识符为键。
 */
class ParserAdapterMgr : private boost::noncopyable
{
public:
	void	release();

	/**
	 * @brief 启动所有适配器
	 * 
	 * @details 遍历并启动所有已注册的行情解析器适配器
	 */
	void	run();

	/**
	 * @brief 获取指定标识符的适配器
	 * @param id 适配器标识符
	 * @return ParserAdapterPtr 适配器指针，如果不存在则返回空指针
	 * 
	 * @details 根据标识符从管理器中查找并返回相应的行情解析器适配器
	 */
	ParserAdapterPtr getAdapter(const char* id);

	/**
	 * @brief 添加适配器到管理器
	 * @param id 适配器标识符
	 * @param adapter 适配器指针
	 * @return bool 添加是否成功，如果已存在相同标识符的适配器则返回false
	 * 
	 * @details 将指定的行情解析器适配器添加到管理器中，以标识符为键
	 */
	bool	addAdapter(const char* id, ParserAdapterPtr& adapter);

	/**
	 * @brief 获取适配器数量
	 * @return uint32_t 当前管理器中的适配器数量
	 */
	uint32_t size() const { return (uint32_t)_adapters.size(); }

public:
	/**
	 * @brief 适配器存储容器，以标识符为键
	 */
	ParserAdapterMap _adapters;
};


