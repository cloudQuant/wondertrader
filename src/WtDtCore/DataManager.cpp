/*!
 * \file DataManager.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据管理器实现
 * \details 该文件实现了WonderTrader数据管理器DataManager类，负责行情数据的接收、处理、存储和广播
 * 实现了IDataWriterSink接口，作为数据写入器的回调接口，处理各类行情数据并进行相应操作
 */
#include "DataManager.h"
#include "StateMonitor.h"
#include "UDPCaster.h"
#include "WtHelper.h"
#include "IDataCaster.h"

#include "../Includes/WTSVariant.hpp"
#include "../Share/DLLHelper.hpp"

#include "../WTSTools/WTSBaseDataMgr.h"
#include "../WTSTools/WTSLogger.h"


/**
 * @brief 构造函数
 * @details 初始化数据管理器对象，将所有指针成员变量初始化为NULL
 */
DataManager::DataManager()
	: _writer(NULL)
	, _bd_mgr(NULL)
	, _state_mon(NULL)
{
}


/**
 * @brief 析构函数
 * @details 清理数据管理器对象的资源，实际的资源释放在release方法中完成
 */
DataManager::~DataManager()
{
}

/**
 * @brief 检查会话是否已处理
 * @details 检查指定标识符的会话是否已经进行了数据处理
 * @param sid 会话标识符
 * @return bool 是否已处理，true表示已处理，false表示未处理或数据写入器不可用
 */
bool DataManager::isSessionProceeded(const char* sid)
{
	if (_writer == NULL)
		return false;

	return _writer->isSessionProceeded(sid);
}

/**
 * @brief 初始化数据管理器
 * @details 根据配置参数初始化数据管理器，加载数据存储模块，并设置基础数据管理器和状态监控器
 * @param params 配置参数，包含数据存储模块等配置
 * @param bdMgr 基础数据管理器指针
 * @param stMonitor 状态监控器指针
 * @return bool 初始化是否成功
 */
bool DataManager::init(WTSVariant* params, WTSBaseDataMgr* bdMgr, StateMonitor* stMonitor)
{
	_bd_mgr = bdMgr;
	_state_mon = stMonitor;

	std::string module = params->getCString("module");
	if (module.empty())
		module = WtHelper::get_module_dir() + DLLHelper::wrap_module("WtDataStorage");
	else
		module = WtHelper::get_module_dir() + DLLHelper::wrap_module(module.c_str());
	
	DllHandle libWriter = DLLHelper::load_library(module.c_str());
	if (libWriter)
	{
		FuncCreateWriter pFuncCreateWriter = (FuncCreateWriter)DLLHelper::get_symbol(libWriter, "createWriter");
		if (pFuncCreateWriter == NULL)
		{
			WTSLogger::error("Initializing of data writer failed: function createWriter not found...");
		}

		FuncDeleteWriter pFuncDeleteWriter = (FuncDeleteWriter)DLLHelper::get_symbol(libWriter, "deleteWriter");
		if (pFuncDeleteWriter == NULL)
		{
			WTSLogger::error("Initializing of data writer failed: function deleteWriter not found...");
		}

		if (pFuncCreateWriter && pFuncDeleteWriter)
		{
			_writer = pFuncCreateWriter();
			_remover = pFuncDeleteWriter;
		}
		WTSLogger::info("Data storage module {} loaded", module);
	}
	else
	{
		WTSLogger::error("Initializing of data writer failed: loading module {} failed...", module.c_str());
		return false;
	}

	return _writer->init(params, this);
}

/**
 * @brief 添加外部历史数据转储器
 * @details 注册一个外部的历史数据转储器，用于将数据转储到外部存储介质
 * @param id 转储器标识符
 * @param dumper 历史数据转储器指针
 */
void DataManager::add_ext_dumper(const char* id, IHisDataDumper* dumper)
{
	if (_writer == NULL)
		return;

	_writer->add_ext_dumper(id, dumper);
}

/**
 * @brief 释放数据管理器资源
 * @details 清理并释放数据管理器使用的资源，包括数据写入器等
 */
void DataManager::release()
{
	if (_writer)
	{
		_writer->release();
		_remover(_writer);
	}
}

/**
 * @brief 写入Tick数据
 * @details 将当前的Tick数据写入到存储中，并根据处理标志进行相应处理
 * @param curTick 当前的Tick数据指针
 * @param procFlag 处理标志，用于控制处理行为
 * @return bool 写入是否成功，true表示成功，false表示失败或数据写入器不可用
 */
bool DataManager::writeTick(WTSTickData* curTick, uint32_t procFlag)
{
	if (_writer == NULL)
		return false;

	return _writer->writeTick(curTick, procFlag);
}

/**
 * @brief 写入委托队列数据
 * @details 将当前的委托队列数据写入到存储中
 * @param curOrdQue 当前的委托队列数据指针
 * @return bool 写入是否成功，true表示成功，false表示失败或数据写入器不可用
 */
bool DataManager::writeOrderQueue(WTSOrdQueData* curOrdQue)
{
	if (_writer == NULL)
		return false;

	return _writer->writeOrderQueue(curOrdQue);
}

/**
 * @brief 写入委托明细数据
 * @details 将当前的委托明细数据写入到存储中
 * @param curOrdDtl 当前的委托明细数据指针
 * @return bool 写入是否成功，true表示成功，false表示失败或数据写入器不可用
 */
bool DataManager::writeOrderDetail(WTSOrdDtlData* curOrdDtl)
{
	if (_writer == NULL)
		return false;

	return _writer->writeOrderDetail(curOrdDtl);
}

/**
 * @brief 写入成交数据
 * @details 将当前的成交数据写入到存储中
 * @param curTrans 当前的成交数据指针
 * @return bool 写入是否成功，true表示成功，false表示失败或数据写入器不可用
 */
bool DataManager::writeTransaction(WTSTransData* curTrans)
{
	if (_writer == NULL)
		return false;

	return _writer->writeTransaction(curTrans);
}

/**
 * @brief 获取当前的Tick数据
 * @details 根据合约代码和交易所获取当前的Tick数据
 * @param code 合约代码
 * @param exchg 交易所代码，默认为空字符串
 * @return WTSTickData* 当前的Tick数据指针，如果没有或数据写入器不可用则返回NULL
 */
WTSTickData* DataManager::getCurTick(const char* code, const char* exchg/* = ""*/)
{
	if (_writer == NULL)
		return NULL;

	return _writer->getCurTick(code, exchg);
}

/**
 * @brief 转换历史数据
 * @details 根据会话标识符转换对应的历史数据
 * @param sid 会话标识符
 */
void DataManager::transHisData(const char* sid)
{
	if (_writer)
		_writer->transHisData(sid);
}

//////////////////////////////////////////////////////////////////////////
#pragma region "IDataWriterSink"
/**
 * @brief 获取基础数据管理器
 * @details 实现IDataWriterSink接口的方法，返回基础数据管理器实例
 * @return IBaseDataMgr* 基础数据管理器指针
 */
IBaseDataMgr* DataManager::getBDMgr()
{
	return _bd_mgr;
}

/**
 * @brief 检查会话是否可以接收数据
 * @details 实现IDataWriterSink接口的方法，检查指定标识符的会话是否可以接收数据
 * @param sid 会话标识符
 * @return bool 是否可以接收数据，true表示可以，false表示不可以
 */
bool DataManager::canSessionReceive(const char* sid)
{
	//By Wesley @ 2021.12.27
	//如果状态机为NULL，说明是全天候模式，直接返回true即可
	if (_state_mon == NULL)
		return true;

	return _state_mon->isInState(sid, SS_RECEIVING);
}

/**
 * @brief 广播Tick数据
 * @details 实现IDataWriterSink接口的方法，将Tick数据广播给所有注册的广播器
 * @param curTick 当前的Tick数据指针
 */
void DataManager::broadcastTick(WTSTickData* curTick)
{
	for(IDataCaster* caster : _casters)
		caster->broadcast(curTick);
}

/**
 * @brief 广播委托明细数据
 * @details 实现IDataWriterSink接口的方法，将委托明细数据广播给所有注册的广播器
 * @param curOrdDtl 当前的委托明细数据指针
 */
void DataManager::broadcastOrdDtl(WTSOrdDtlData* curOrdDtl)
{
	for (IDataCaster* caster : _casters)
		caster->broadcast(curOrdDtl);
}

/**
 * @brief 广播委托队列数据
 * @details 实现IDataWriterSink接口的方法，将委托队列数据广播给所有注册的广播器
 * @param curOrdQue 当前的委托队列数据指针
 */
void DataManager::broadcastOrdQue(WTSOrdQueData* curOrdQue)
{
	for (IDataCaster* caster : _casters)
		caster->broadcast(curOrdQue);
}

/**
 * @brief 广播成交数据
 * @details 实现IDataWriterSink接口的方法，将成交数据广播给所有注册的广播器
 * @param curTrans 当前的成交数据指针
 */
void DataManager::broadcastTrans(WTSTransData* curTrans)
{
	for (IDataCaster* caster : _casters)
		caster->broadcast(curTrans);
}

/**
 * @brief 获取会话关联的合约集合
 * @details 实现IDataWriterSink接口的方法，获取指定会话关联的所有合约代码
 * @param sid 会话标识符
 * @return CodeSet* 合约代码集合指针
 */
CodeSet* DataManager::getSessionComms(const char* sid)
{
	return  _bd_mgr->getSessionComms(sid);
}

/**
 * @brief 获取交易日期
 * @details 实现IDataWriterSink接口的方法，获取指定品种的当前交易日期
 * @param pid 品种标识符
 * @return uint32_t 交易日期，格式为YYYYMMDD
 */
uint32_t DataManager::getTradingDate(const char* pid)
{
	return  _bd_mgr->getTradingDate(pid);
}

/**
 * @brief 处理解析模块的日志
 * @details 实现IDataWriterSink接口的方法，将解析模块产生的日志输出到日志系统
 * @param ll 日志级别
 * @param message 日志内容
 */
void DataManager::outputLog(WTSLogLevel ll, const char* message)
{
	WTSLogger::log_raw(ll, message);
}

#pragma endregion "IDataWriterSink"
