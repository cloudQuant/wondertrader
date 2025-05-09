/*!
 * \file ParserAdapter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 行情解析器适配器定义
 * 
 * 本文件定义了行情解析器的适配器类，用于封装不同行情源的接入逻辑。
 * 适配器类实现了IParserSpi接口，用于接收并处理各种行情数据，包括快照、委托队列、逐笔委托和逐笔成交等。
 */
#pragma once
#include <memory>
#include <boost/core/noncopyable.hpp>

#include "../Includes/FasterDefs.h"
#include "../Includes/IParserApi.h"


NS_WTP_BEGIN
class WTSVariant;

/**
 * @brief 行情解析器回调接口
 * 
 * 定义了接收行情解析器数据的回调方法
 * 任何需要处理行情数据的组件都需要实现该接口
 */
class IParserStub
{
public:
	/**
	 * @brief 处理快照行情数据
	 * 
	 * @param curTick 当前快照数据
	 */
	virtual void			handle_push_quote(WTSTickData* curTick){}

	/**
	 * @brief 处理逐笔委托数据
	 * 
	 * @param curOrdDtl 当前逐笔委托数据
	 */
	virtual void			handle_push_order_detail(WTSOrdDtlData* curOrdDtl){}

	/**
	 * @brief 处理委托队列数据
	 * 
	 * @param curOrdQue 当前委托队列数据
	 */
	virtual void			handle_push_order_queue(WTSOrdQueData* curOrdQue) {}

	/**
	 * @brief 处理逐笔成交数据
	 * 
	 * @param curTrans 当前逐笔成交数据
	 */
	virtual void			handle_push_transaction(WTSTransData* curTrans) {}
};

/**
 * @brief 行情解析器适配器类
 * 
 * 负责加载和初始化行情解析器插件，并将收到的各类行情数据转发给注册的回调接口
 * 实现了IParserSpi接口以接收各类行情数据
 * 支持按符号和交易所进行过滤
 */
class ParserAdapter : public IParserSpi,
					private boost::noncopyable
{
public:
	/**
	 * @brief 构造函数
	 * 
	 * 初始化行情解析器适配器对象
	 */
	ParserAdapter();

	/**
	 * @brief 析构函数
	 * 
	 * 清理适配器资源
	 */
	~ParserAdapter();

public:
	/**
	 * @brief 初始化适配器
	 * 
	 * 根据配置加载行情解析器模块，并初始化适配器
	 * 
	 * @param id 适配器标识符
	 * @param cfg 配置信息
	 * @param stub 行情回调接口
	 * @param bgMgr 基础数据管理器
	 * @return bool 初始化是否成功
	 */
	bool	init(const char* id, WTSVariant* cfg, IParserStub* stub, IBaseDataMgr* bgMgr);

	/**
	 * @brief 使用外部API初始化适配器
	 * 
	 * 使用已创建的行情解析器API初始化适配器
	 * 
	 * @param id 适配器标识符
	 * @param api 已创建的行情解析器API对象
	 * @param stub 行情回调接口
	 * @param bgMgr 基础数据管理器
	 * @return bool 初始化是否成功
	 */
	bool	initExt(const char* id, IParserApi* api, IParserStub* stub, IBaseDataMgr* bgMgr);

	/**
	 * @brief 释放适配器资源
	 * 
	 * 清理适配器使用的内存和其他资源
	 */
	void	release();

	/**
	 * @brief 运行适配器
	 * 
	 * 启动行情解析器并连接到行情源
	 * 
	 * @return bool 启动是否成功
	 */
	bool	run();

	/**
	 * @brief 获取适配器标识符
	 * 
	 * @return const char* 适配器标识符
	 */
	const char* id() const{ return _id.c_str(); }

public:
	/**
	 * @brief 处理符号列表
	 * 
	 * 接收并处理行情源返回的符号列表
	 * 
	 * @param aySymbols 符号列表数组
	 */
	virtual void handleSymbolList(const WTSArray* aySymbols) override {}

	/**
	 * @brief 处理实时行情
	 * 
	 * 接收并处理行情源提供的实时快照数据
	 * 
	 * @param quote 实时行情数据
	 * @param procFlag 处理标志，指定是否需要特殊处理
	 */
	/**
	 * @brief 处理实时行情数据
	 * 
	 * 本方法接收并处理行情源提供的实时快照数据，包括最新价格、成交量、买卖盘等信息。
	 * 
	 * @param quote 实时行情数据
	 * @param procFlag 处理标志，指定是否需要特殊处理
	 */
	virtual void handleQuote(WTSTickData *quote, uint32_t procFlag) override;

	/**
	 * @brief 处理委托队列数据（股票level2）
	 * 
	 * 接收并处理行情源提供的委托队列数据，包括买卖盘信息、委托价格、委托量等。
	 * 
	 * @param ordQueData 委托队列数据
	 */
	virtual void handleOrderQueue(WTSOrdQueData* ordQueData) override;

	/**
	 * @brief 处理逐笔委托数据（股票level2）
	 * 
	 * 接收并处理行情源提供的逐笔委托数据，包括委托时间、委托价格、委托量等信息。
	 * 接收并处理行情源提供的逐笔委托数据
	 * 
	 * @param ordDetailData 逐笔委托数据
	 */
	virtual void handleOrderDetail(WTSOrdDtlData* ordDetailData) override;

	/**
	 * @brief 处理逐笔成交数据
	 * 
	 * 接收并处理行情源提供的逐笔成交数据
	 * 
	 * @param transData 逐笔成交数据
	 */
	virtual void handleTransaction(WTSTransData* transData) override;

	/**
	 * @brief 处理解析器日志
	 * 
	 * 接收并处理行情解析器输出的日志信息
	 * 
	 * @param ll 日志级别
	 * @param message 日志消息
	 */
	virtual void handleParserLog(WTSLogLevel ll, const char* message) override;

	/**
	 * @brief 获取基础数据管理器
	 * 
	 * @return IBaseDataMgr* 基础数据管理器指针
	 */
	virtual IBaseDataMgr* getBaseDataMgr() override { return _bd_mgr; }


private:
	IParserApi*			_parser_api;		/**< 行情解析器API对象指针 */
	FuncDeleteParser	_remover;		/**< 解析器删除函数指针 */

	bool				_stopped;		/**< 是否已停止标志 */

	typedef wt_hashset<std::string>	ExchgFilter;	/**< 交易所过滤器类型定义 */
	ExchgFilter			_exchg_filter;	/**< 交易所过滤器 */
	ExchgFilter			_code_filter;		/**< 合约代码过滤器 */
	IBaseDataMgr*		_bd_mgr;		/**< 基础数据管理器指针 */
	IParserStub*		_stub;			/**< 行情回调接口指针 */
	WTSVariant*			_cfg;			/**< 配置对象指针 */
	std::string			_id;			/**< 适配器标识符 */
};

/**
 * @brief 行情适配器智能指针类型
 */
typedef std::shared_ptr<ParserAdapter>	ParserAdapterPtr;

/**
 * @brief 行情适配器映射表类型
 * 
 * 通过标识符存储和映射行情适配器对象
 */
typedef wt_hashmap<std::string, ParserAdapterPtr>	ParserAdapterMap;

/**
 * @brief 行情适配器管理器类
 * 
 * 管理多个行情适配器实例，提供统一的控制和访问接口
 */
class ParserAdapterMgr : private boost::noncopyable
{
public:
	/**
	 * @brief 释放所有适配器资源
	 * 
	 * 清理并释放所有管理的行情适配器资源
	 */
	void	release();

	/**
	 * @brief 启动所有适配器
	 * 
	 * 调用所有管理的行情适配器的run方法，开始接收行情数据
	 */
	void	run();

	/**
	 * @brief 获取指定标识符的适配器
	 * 
	 * @param id 适配器标识符
	 * @return ParserAdapterPtr 行情适配器指针，如果不存在则返回空指针
	 */
	ParserAdapterPtr getAdapter(const char* id);

	/**
	 * @brief 添加新的适配器
	 * 
	 * 将行情适配器添加到管理器中
	 * 
	 * @param id 适配器标识符
	 * @param adapter 行情适配器指针
	 * @return bool 添加是否成功
	 */
	bool	addAdapter(const char* id, ParserAdapterPtr& adapter);


public:
	ParserAdapterMap _adapters;		/**< 所有管理的行情适配器映射表 */
};

NS_WTP_END