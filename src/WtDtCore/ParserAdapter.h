/*!
 * \file ParserAdapter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 行情解析模块适配类定义
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
class wxMainFrame;
class WTSBaseDataMgr;
class DataManager;
class IndexFactory;

/**
 * @brief 行情解析模块适配器
 * @details 该类实现了IParserSpi接口，用于适配不同的行情解析器，接收行情数据并转发到数据管理器和指数工厂
 * @note 该类禁止复制和赋值
 */
class ParserAdapter : public IParserSpi, private boost::noncopyable
{
public:
	/**
	 * @brief 构造函数
	 * @details 创建一个行情解析适配器实例
	 * @param bgMgr 基础数据管理器指针
	 * @param dtMgr 数据管理器指针
	 * @param idxFactory 指数工厂指针
	 */
	ParserAdapter(WTSBaseDataMgr * bgMgr, DataManager* dtMgr, IndexFactory *idxFactory);
	/**
	 * @brief 析构函数
	 * @details 释放解析适配器使用的资源
	 */
	~ParserAdapter();

public:
	/**
	 * @brief 初始化解析适配器
	 * @details 根据配置创建和初始化一个行情解析器实例
	 * @param id 适配器唯一标识
	 * @param cfg 配置对象
	 * @return bool 初始化是否成功
	 */
	bool	init(const char* id, WTSVariant* cfg);

	/**
	 * @brief 使用外部API初始化解析适配器
	 * @details 使用外部提供的解析器API实例初始化适配器
	 * @param id 适配器唯一标识
	 * @param api 外部提供的行情解析API实例
	 * @return bool 初始化是否成功
	 */
	bool	initExt(const char* id, IParserApi* api);

	void	release();

	/**
	 * @brief 启动解析器
	 * @details 启动并运行行情解析器，开始接收行情数据
	 * @return bool 启动是否成功
	 */
	bool	run();

	/**
	 * @brief 获取适配器标识
	 * @details 返回当前适配器的唯一标识
	 * @return const char* 适配器标识字符串
	 */
	const char* id() const { return _id.c_str(); }

public:
	/**
	 * @brief 处理合约列表
	 * @details 实现IParserSpi接口方法，处理解析器返回的合约列表
	 * @param aySymbols 合约数组
	 */
	virtual void handleSymbolList(const WTSArray* aySymbols) override;

	/**
	 * @brief 处理行情数据
	 * @details 实现IParserSpi接口方法，处理解析器返回的Tick数据
	 * @param quote Tick数据指针
	 * @param procFlag 处理标记，用于指示如何处理数据
	 */
	virtual void handleQuote(WTSTickData *quote, uint32_t procFlag) override;

	/**
	 * @brief 处理委托队列数据
	 * @details 实现IParserSpi接口方法，处理解析器返回的委托队列数据
	 * @param ordQueData 委托队列数据指针
	 */
	virtual void handleOrderQueue(WTSOrdQueData* ordQueData) override;

	/**
	 * @brief 处理逆序成交数据
	 * @details 实现IParserSpi接口方法，处理解析器返回的逆序递成交数据
	 * @param transData 逆序成交数据指针
	 */
	virtual void handleTransaction(WTSTransData* transData) override;

	/**
	 * @brief 处理逆序委托明细数据
	 * @details 实现IParserSpi接口方法，处理解析器返回的逆序委托明细数据
	 * @param ordDetailData 逆序委托明细数据指针
	 */
	virtual void handleOrderDetail(WTSOrdDtlData* ordDetailData) override;

	/**
	 * @brief 处理解析器日志
	 * @details 实现IParserSpi接口方法，处理解析器输出的日志信息
	 * @param ll 日志级别
	 * @param message 日志消息内容
	 */
	virtual void handleParserLog(WTSLogLevel ll, const char* message) override;

	/**
	 * @brief 获取基础数据管理器
	 * @details 实现IParserSpi接口方法，返回适配器的基础数据管理器
	 * @return IBaseDataMgr* 基础数据管理器指针
	 */
	virtual IBaseDataMgr* getBaseDataMgr() override;

private:
	/// @brief 行情解析器API接口实例
	IParserApi*			_parser_api;
	/// @brief 解析器对象删除函数指针
	FuncDeleteParser	_remover;
	/// @brief 基础数据管理器指针
	WTSBaseDataMgr*		_bd_mgr;
	/// @brief 数据管理器指针
	DataManager*		_dt_mgr;
	/// @brief 指数工厂指针
	IndexFactory*		_idx_fact;

	/// @brief 是否停止运行标记
	bool				_stopped;

	/// @brief 交易所和代码过滤器类型定义
	typedef wt_hashset<std::string>	ExchgFilter;
	/// @brief 交易所过滤器，用于过滤特定交易所的数据
	ExchgFilter			_exchg_filter;
	/// @brief 合约代码过滤器，用于过滤特定合约的数据
	ExchgFilter			_code_filter;
	/// @brief 配置对象指针
	WTSVariant*			_cfg;
	/// @brief 适配器唯一标识
	std::string			_id;
};

/**
 * @brief 行情解析适配器智能指针类型
 */
typedef std::shared_ptr<ParserAdapter>	ParserAdapterPtr;

/**
 * @brief 行情解析适配器映射表类型，用于存储ID到适配器的映射
 */
typedef wt_hashmap<std::string, ParserAdapterPtr>	ParserAdapterMap;

/**
 * @brief 行情解析适配器管理器
 * @details 负责管理多个行情解析适配器实例，实现添加、获取和管理适配器的功能
 * @note 该类禁止复制和赋值
 */
class ParserAdapterMgr : private boost::noncopyable
{
public:
	/**
	 * @brief 释放所有解析适配器资源
	 * @details 遍历并释放管理器中的所有解析适配器实例
	 */
	void	release();

	/**
	 * @brief 运行所有解析适配器
	 * @details 遍历并启动所有适配器实例，并输出日志信息
	 */
	void	run();

	/**
	 * @brief 获取指定的解析适配器
	 * @details 根据适配器ID从管理器中获取相应的适配器实例
	 * @param id 适配器唯一标识
	 * @return ParserAdapterPtr 适配器智能指针，如果不存在则返回空指针
	 */
	ParserAdapterPtr getAdapter(const char* id);

	/**
	 * @brief 添加解析适配器
	 * @details 将解析适配器实例添加到管理器中，不允许重复的ID
	 * @param id 适配器唯一标识
	 * @param adapter 适配器智能指针引用
	 * @return bool 添加是否成功
	 */
	bool	addAdapter(const char* id, ParserAdapterPtr& adapter);

	/**
	 * @brief 获取适配器数量
	 * @details 返回管理器中当前管理的适配器实例数量
	 * @return uint32_t 适配器数量
	 */
	uint32_t size() const { return (uint32_t)_adapters.size(); }

public:
	/// @brief 解析适配器映射表，ID到适配器实例的映射
	ParserAdapterMap _adapters;
};


