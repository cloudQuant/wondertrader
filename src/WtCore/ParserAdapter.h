/*!
 * \file ParserAdapter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 行情解析器适配器定义
 * 
 * 本文件定义了WonderTrader的行情解析器适配器，用于将不同来源的行情数据
 * 统一转换为内部格式，并进行分发和处理。它充当了外部行情源和
 * 内部交易系统之间的桥梁，确保数据格式的一致性和处理的统一性。
 */
#pragma once
#include <memory>
#include <boost/core/noncopyable.hpp>

#include "../Includes/FasterDefs.h"
#include "../Includes/IParserApi.h"


NS_WTP_BEGIN
class WTSVariant;
class IHotMgr;

/**
 * @brief 行情解析器数据回调接口
 * @details 定义了行情解析器需要实现的回调接口，用于处理不同类型的行情数据
 * 实现该接口的类可以接收并处理从解析器推送的各种市场数据
 */
class IParserStub
{
public:
	/**
	 * @brief 处理推送的行情数据
	 * @param curTick 当前的Tick数据
	 * @details 当有新的Tick数据到达时调用此函数
	 */
	virtual void			handle_push_quote(WTSTickData* curTick){}

	/**
	 * @brief 处理推送的逐笔委托数据
	 * @param curOrdDtl 当前的逐笔委托数据
	 * @details 当有新的逐笔委托数据到达时调用此函数，主要用于Level2行情
	 */
	virtual void			handle_push_order_detail(WTSOrdDtlData* curOrdDtl){}

	/**
	 * @brief 处理推送的委托队列数据
	 * @param curOrdQue 当前的委托队列数据
	 * @details 当有新的委托队列数据到达时调用此函数，主要用于Level2行情
	 */
	virtual void			handle_push_order_queue(WTSOrdQueData* curOrdQue) {}

	/**
	 * @brief 处理推送的逐笔成交数据
	 * @param curTrans 当前的逐笔成交数据
	 * @details 当有新的逐笔成交数据到达时调用此函数，主要用于Level2行情
	 */
	virtual void			handle_push_transaction(WTSTransData* curTrans) {}
};

/**
 * @brief 行情解析器适配器类
 * @details 实现了IParserSpi接口，用于将各种行情源的数据统一转换为内部格式
 * 并将数据分发给相应的处理模块。该类不允许拷贝，继承自boost::noncopyable
 */
class ParserAdapter : public IParserSpi,
					private boost::noncopyable
{
public:
	/**
	 * @brief 构造函数
	 */
	ParserAdapter();

	/**
	 * @brief 析构函数
	 */
	~ParserAdapter();

public:
	/**
	 * @brief 初始化解析器适配器
	 * @param id 适配器标识
	 * @param cfg 配置项
	 * @param stub 数据回调对象
	 * @param bgMgr 基础数据管理器
	 * @param hotMgr 主力合约管理器（可选）
	 * @return bool 初始化是否成功
	 */
	bool	init(const char* id, WTSVariant* cfg, IParserStub* stub, IBaseDataMgr* bgMgr, IHotMgr* hotMgr = NULL);

	/**
	 * @brief 使用外部API初始化解析器适配器
	 * @param id 适配器标识
	 * @param api 外部解析器API
	 * @param stub 数据回调对象
	 * @param bgMgr 基础数据管理器
	 * @param hotMgr 主力合约管理器（可选）
	 * @return bool 初始化是否成功
	 */
	bool	initExt(const char* id, IParserApi* api, IParserStub* stub, IBaseDataMgr* bgMgr, IHotMgr* hotMgr = NULL);

	/**
	 * @brief 释放解析器适配器资源
	 */
	void	release();

	/**
	 * @brief 启动解析器
	 * @return bool 启动是否成功
	 */
	bool	run();

	/**
	 * @brief 获取适配器标识
	 * @return const char* 适配器标识
	 */
	const char* id() const{ return _id.c_str(); }

public:
	/**
	 * @brief 处理合约列表
	 * @param aySymbols 合约列表数组
	 * @details 当接收到合约列表时调用此函数，默认实现为空
	 */
	virtual void handleSymbolList(const WTSArray* aySymbols) override {}

	/**
	 * @brief 处理实时行情
	 * @param quote 实时行情数据
	 * @param procFlag 处理标志，用于控制处理行为
	 * @details 当接收到实时行情数据时调用此函数，将数据转发给相应的处理模块
	 */
	virtual void handleQuote(WTSTickData *quote, uint32_t procFlag) override;

	/**
	 * @brief 处理委托队列数据（股票Level2）
	 * @param ordQueData 委托队列数据
	 * @details 当接收到委托队列数据时调用此函数，将数据转发给相应的处理模块
	 */
	virtual void handleOrderQueue(WTSOrdQueData* ordQueData) override;

	/**
	 * @brief 处理逐笔委托数据（股票Level2）
	 * @param ordDetailData 逐笔委托数据
	 * @details 当接收到逐笔委托数据时调用此函数，将数据转发给相应的处理模块
	 */
	virtual void handleOrderDetail(WTSOrdDtlData* ordDetailData) override;

	/**
	 * @brief 处理逐笔成交数据（股票Level2）
	 * @param transData 逐笔成交数据
	 * @details 当接收到逐笔成交数据时调用此函数，将数据转发给相应的处理模块
	 */
	virtual void handleTransaction(WTSTransData* transData) override;

	/**
	 * @brief 处理解析器日志
	 * @param ll 日志级别
	 * @param message 日志消息
	 * @details 当解析器产生日志时调用此函数，将日志输出到相应的日志系统
	 */
	virtual void handleParserLog(WTSLogLevel ll, const char* message) override;

	/**
	 * @brief 获取基础数据管理器
	 * @return IBaseDataMgr* 基础数据管理器指针
	 * @details 返回内部保存的基础数据管理器指针
	 */
	virtual IBaseDataMgr* getBaseDataMgr() override { return _bd_mgr; }


private:
	/**
	 * @brief 解析器API接口
	 * @details 实际的解析器API实现，用于与外部行情源进行交互
	 */
	IParserApi*			_parser_api;

	/**
	 * @brief 解析器删除函数
	 * @details 用于在需要时释放解析器API实例
	 */
	FuncDeleteParser	_remover;

	/**
	 * @brief 停止标志
	 * @details 标记解析器是否已经停止运行
	 */
	bool				_stopped;

	/**
	 * @brief 时间检查标志
	 * @details 如果为true，则在收到行情的时候进行时间检查
	 * 主要适用于直接从行情源接入，因为直接从行情源接入
	 * 很可能会有错误时间戳的数据进来，该选项默认为false
	 */
	bool				_check_time;

	/**
	 * @brief 字符串集合类型定义
	 * @details 用于存储交易所和合约代码的过滤集合
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
	 * @brief 基础数据管理器
	 * @details 用于管理合约、交易所等基础数据
	 */
	IBaseDataMgr*		_bd_mgr;

	/**
	 * @brief 主力合约管理器
	 * @details 用于管理期货等品种的主力合约映射
	 */
	IHotMgr*			_hot_mgr;

	/**
	 * @brief 数据回调对象
	 * @details 用于将解析后的数据转发给相应的处理模块
	 */
	IParserStub*		_stub;

	/**
	 * @brief 配置对象
	 * @details 存储解析器的配置信息
	 */
	WTSVariant*			_cfg;

	/**
	 * @brief 适配器标识
	 * @details 用于唯一标识当前解析器适配器
	 */
	std::string			_id;
};

/**
 * @brief 解析器适配器智能指针类型
 * @details 使用std::shared_ptr包装ParserAdapter对象，实现自动内存管理
 */
typedef std::shared_ptr<ParserAdapter>	ParserAdapterPtr;

/**
 * @brief 解析器适配器映射类型
 * @details 使用哈希表存储标识到解析器适配器的映射关系
 */
typedef wt_hashmap<std::string, ParserAdapterPtr>	ParserAdapterMap;

/**
 * @brief 解析器适配器管理器类
 * @details 管理多个解析器适配器实例，提供统一的接口进行操作
 * 该类不允许拷贝，继承自boost::noncopyable
 */
class ParserAdapterMgr : private boost::noncopyable
{
public:
	/**
	 * @brief 释放所有解析器适配器资源
	 * @details 清理管理器中的所有解析器适配器实例
	 */
	void	release();

	/**
	 * @brief 启动所有解析器
	 * @details 启动管理器中的所有解析器适配器实例
	 */
	void	run();

	/**
	 * @brief 获取指定标识的解析器适配器
	 * @param id 适配器标识
	 * @return ParserAdapterPtr 解析器适配器指针，如果不存在则返回null
	 */
	ParserAdapterPtr getAdapter(const char* id);

	/**
	 * @brief 添加解析器适配器
	 * @param id 适配器标识
	 * @param adapter 解析器适配器指针
	 * @return bool 添加是否成功
	 */
	bool	addAdapter(const char* id, ParserAdapterPtr& adapter);


public:
	/**
	 * @brief 解析器适配器映射表
	 * @details 存储所有注册的解析器适配器实例
	 */
	ParserAdapterMap _adapters;
};

NS_WTP_END