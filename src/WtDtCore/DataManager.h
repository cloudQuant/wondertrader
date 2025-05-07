/*!
 * \file DataManager.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据管理器定义
 * \details 该文件定义了WonderTrader数据管理器DataManager类，负责行情数据的接收、处理、存储和广播
 * 实现了IDataWriterSink接口，作为数据写入器的回调接口，处理各类行情数据并进行相应操作
 */
#pragma once

#include "../Includes/IDataWriter.h"
#include "../Share/StdUtils.hpp"
#include "../Share/BoostMappingFile.hpp"

NS_WTP_BEGIN
class WTSTickData;
class WTSOrdQueData;
class WTSOrdDtlData;
class WTSTransData;
class WTSVariant;
class IDataCaster;
NS_WTP_END

USING_NS_WTP;

class WTSBaseDataMgr;
class StateMonitor;
class UDPCaster;

/**
 * @brief 数据管理器类
 * @details 负责管理和处理各类行情数据，包括tick数据、委托队列、委托明细和成交数据
 * 实现了IDataWriterSink接口，作为数据写入器的回调接口
 * 支持数据存储、广播和历史数据转换等功能
 */
class DataManager : public IDataWriterSink
{
public:
	/**
	 * @brief 构造函数
	 * @details 创建数据管理器实例，初始化相关成员变量
	 */
	DataManager();

	/**
	 * @brief 析构函数
	 * @details 释放数据管理器占用的资源
	 */
	~DataManager();

public:
	/**
	 * @brief 初始化数据管理器
	 * @details 根据配置参数初始化数据管理器，设置基础数据管理器和状态监控器
	 * @param params 配置参数
	 * @param bdMgr 基础数据管理器指针
	 * @param stMonitor 状态监控器指针
	 * @return bool 初始化是否成功
	 */
	bool init(WTSVariant* params, WTSBaseDataMgr* bdMgr, StateMonitor* stMonitor);

	/**
	 * @brief 添加外部历史数据转储器
	 * @details 注册一个外部的历史数据转储器，用于将数据转储到外部存储介质
	 * @param id 转储器标识符
	 * @param dumper 历史数据转储器指针
	 */
	void add_ext_dumper(const char* id, IHisDataDumper* dumper);

	/**
	 * @brief 添加数据广播器
	 * @details 注册一个数据广播器，用于将数据广播到其他终端或系统
	 * @param caster 数据广播器指针
	 */
	inline void add_caster(IDataCaster* caster)
	{
		if (caster == NULL)
			return;

		_casters.emplace_back(caster);
	}

	/**
	 * @brief 释放数据管理器资源
	 * @details 清理并释放数据管理器使用的所有资源，包括广播器和数据写入器等
	 */
	void release();

	/**
	 * @brief 写入Tick数据
	 * @details 将当前的Tick数据写入到存储中，并根据处理标志进行相应处理
	 * @param curTick 当前的Tick数据指针
	 * @param procFlag 处理标志，用于控制处理行为
	 * @return bool 写入是否成功
	 */
	bool writeTick(WTSTickData* curTick, uint32_t procFlag);

	/**
	 * @brief 写入委托队列数据
	 * @details 将当前的委托队列数据写入到存储中
	 * @param curOrdQue 当前的委托队列数据指针
	 * @return bool 写入是否成功
	 */
	bool writeOrderQueue(WTSOrdQueData* curOrdQue);

	/**
	 * @brief 写入委托明细数据
	 * @details 将当前的委托明细数据写入到存储中
	 * @param curOrdDetail 当前的委托明细数据指针
	 * @return bool 写入是否成功
	 */
	bool writeOrderDetail(WTSOrdDtlData* curOrdDetail);

	/**
	 * @brief 写入成交数据
	 * @details 将当前的成交数据写入到存储中
	 * @param curTrans 当前的成交数据指针
	 * @return bool 写入是否成功
	 */
	bool writeTransaction(WTSTransData* curTrans);

	/**
	 * @brief 转换历史数据
	 * @details 根据会话标识符转换对应的历史数据
	 * @param sid 会话标识符
	 */
	void transHisData(const char* sid);
	
	/**
	 * @brief 检查会话是否已处理
	 * @details 检查指定标识符的会话是否已经进行了数据处理
	 * @param sid 会话标识符
	 * @return bool 是否已处理，true表示已处理，false表示未处理
	 */
	bool isSessionProceeded(const char* sid);

	/**
	 * @brief 获取当前的Tick数据
	 * @details 根据合约代码和交易所获取当前的Tick数据
	 * @param code 合约代码
	 * @param exchg 交易所代码，默认为空字符串
	 * @return WTSTickData* 当前的Tick数据指针，如果没有则返回null
	 */
	WTSTickData* getCurTick(const char* code, const char* exchg = "");

public:
	//////////////////////////////////////////////////////////////////////////
	//IDataWriterSink
	/**
	 * @brief 获取基础数据管理器
	 * @details 实现IDataWriterSink接口的方法，返回基础数据管理器实例
	 * @return IBaseDataMgr* 基础数据管理器指针
	 */
	virtual IBaseDataMgr* getBDMgr() override;

	/**
	 * @brief 检查会话是否可以接收数据
	 * @details 实现IDataWriterSink接口的方法，检查指定标识符的会话是否可以接收数据
	 * @param sid 会话标识符
	 * @return bool 是否可以接收数据，true表示可以，false表示不可以
	 */
	virtual bool canSessionReceive(const char* sid) override;

	/**
	 * @brief 广播Tick数据
	 * @details 实现IDataWriterSink接口的方法，将Tick数据广播给所有注册的广播器
	 * @param curTick 当前的Tick数据指针
	 */
	virtual void broadcastTick(WTSTickData* curTick) override;

	/**
	 * @brief 广播委托队列数据
	 * @details 实现IDataWriterSink接口的方法，将委托队列数据广播给所有注册的广播器
	 * @param curOrdQue 当前的委托队列数据指针
	 */
	virtual void broadcastOrdQue(WTSOrdQueData* curOrdQue) override;

	/**
	 * @brief 广播委托明细数据
	 * @details 实现IDataWriterSink接口的方法，将委托明细数据广播给所有注册的广播器
	 * @param curOrdDtl 当前的委托明细数据指针
	 */
	virtual void broadcastOrdDtl(WTSOrdDtlData* curOrdDtl) override;

	/**
	 * @brief 广播成交数据
	 * @details 实现IDataWriterSink接口的方法，将成交数据广播给所有注册的广播器
	 * @param curTrans 当前的成交数据指针
	 */
	virtual void broadcastTrans(WTSTransData* curTrans) override;

	/**
	 * @brief 获取会话关联的合约集合
	 * @details 实现IDataWriterSink接口的方法，获取指定会话关联的所有合约代码
	 * @param sid 会话标识符
	 * @return CodeSet* 合约代码集合指针
	 */
	virtual CodeSet* getSessionComms(const char* sid) override;

	/**
	 * @brief 获取交易日期
	 * @details 实现IDataWriterSink接口的方法，获取指定品种的当前交易日期
	 * @param pid 品种标识符
	 * @return uint32_t 交易日期，格式为YYYYMMDD
	 */
	virtual uint32_t getTradingDate(const char* pid) override;

	/**
	 * @brief 处理解析模块的日志
	 * @details 实现IDataWriterSink接口的方法，将解析模块产生的日志输出到日志系统
	 * @param ll 日志级别
	 * @param message 日志内容
	 */
	virtual void outputLog(WTSLogLevel ll, const char* message) override;

private:
	/**
	 * @brief 数据写入器指针
	 * @details 用于将数据写入到存储介质中
	 */
	IDataWriter*		_writer;

	/**
	 * @brief 数据写入器删除函数
	 * @details 用于释放数据写入器对象
	 */
	FuncDeleteWriter	_remover;

	/**
	 * @brief 基础数据管理器指针
	 * @details 用于获取基础数据信息
	 */
	WTSBaseDataMgr*		_bd_mgr;

	/**
	 * @brief 状态监控器指针
	 * @details 用于监控系统状态
	 */
	StateMonitor*		_state_mon;

	/**
	 * @brief 数据广播器列表
	 * @details 存储所有注册的数据广播器
	 */
	std::vector<IDataCaster*>	_casters;
};

