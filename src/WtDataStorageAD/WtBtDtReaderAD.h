/*!
 * \file WtBtDtReaderAD.h
 * \brief 基于LMDB的回测数据读取器定义
 * 
 * 该文件定义了基于LMDB数据库的回测数据读取器，用于从LMDB数据库中读取历史行情数据供回测使用
 * 
 * \author Wesley
 * \date 2022/01/05
 */

#pragma once
#include <string>

#include "../Includes/FasterDefs.h"
#include "../Includes/IBtDtReader.h"

#include "../WTSUtils/WtLMDB.hpp"

NS_WTP_BEGIN

/**
 * \brief 基于LMDB的回测数据读取器类
 * 
 * 该类实现了IBtDtReader接口，用于从LMDB数据库中读取历史行情数据供回测使用，
 * 包括K线数据和Tick数据。
 */
class WtBtDtReaderAD : public IBtDtReader
{
public:
	/**
	 * \brief 构造函数
	 */
	WtBtDtReaderAD();

	/**
	 * \brief 析构函数
	 */
	virtual ~WtBtDtReaderAD();	

//////////////////////////////////////////////////////////////////////////
//IBtDtReader
public:
	/**
	 * \brief 初始化回测数据读取器
	 * 
	 * \param cfg 配置项，包含数据存储路径等信息
	 * \param sink 数据接收器，用于接收读取器返回的数据和日志
	 */
	virtual void init(WTSVariant* cfg, IBtDtReaderSink* sink);

	/**
	 * \brief 读取原始K线数据
	 * 
	 * 从LMDB数据库中读取指定交易所、合约和周期的K线数据
	 * 
	 * \param exchg 交易所代码
	 * \param code 合约代码
	 * \param period K线周期
	 * \param buffer 输出参数，用于存储读取的数据
	 * \return 返回true表示读取成功，false表示读取失败
	 */
	virtual bool read_raw_bars(const char* exchg, const char* code, WTSKlinePeriod period, std::string& buffer) override;

	/**
	 * \brief 读取原始Tick数据
	 * 
	 * 从LMDB数据库中读取指定交易所、合约和日期的Tick数据
	 * 
	 * \param exchg 交易所代码
	 * \param code 合约代码
	 * \param uDate 交易日期，格式为YYYYMMDD
	 * \param buffer 输出参数，用于存储读取的数据
	 * \return 返回true表示读取成功，false表示读取失败
	 */
	virtual bool read_raw_ticks(const char* exchg, const char* code, uint32_t uDate, std::string& buffer) override;

private:
	/**
	 * \brief 数据存储的基础目录
	 */
	std::string		_base_dir;

private:
	//////////////////////////////////////////////////////////////////////////
	/*
	 *	这里放LMDB的数据库定义
	 *	K线数据，按照每个市场m1/m5/d1三个周期一共三个数据库，路径如./m1/CFFEX
	 *	Tick数据，每个合约一个数据库，路径如./ticks/CFFEX/IF2101
	 */
	/**
	 * \brief LMDB数据库的智能指针类型定义
	 */
	typedef std::shared_ptr<WtLMDB> WtLMDBPtr;

	/**
	 * \brief LMDB数据库映射类型定义，以字符串为键
	 */
	typedef wt_hashmap<std::string, WtLMDBPtr> WtLMDBMap;

	/**
	 * \brief 1分钟K线数据库映射，以交易所为键
	 */
	WtLMDBMap	_exchg_m1_dbs;

	/**
	 * \brief 5分钟K线数据库映射，以交易所为键
	 */
	WtLMDBMap	_exchg_m5_dbs;

	/**
	 * \brief 日线K线数据库映射，以交易所为键
	 */
	WtLMDBMap	_exchg_d1_dbs;

	/**
	 * \brief Tick数据库映射，用exchg.code作为key，如BINANCE.BTCUSDT
	 */
	WtLMDBMap	_tick_dbs;

	/**
	 * \brief 获取指定交易所和周期的K线数据库
	 * 
	 * \param exchg 交易所代码
	 * \param period K线周期，支持1分钟、5分钟和日线
	 * \return 返回LMDB数据库指针，如果不存在则返回空指针
	 */
	WtLMDBPtr	get_k_db(const char* exchg, WTSKlinePeriod period);

	/**
	 * \brief 获取指定交易所和合约的Tick数据库
	 * 
	 * \param exchg 交易所代码
	 * \param code 合约代码
	 * \return 返回LMDB数据库指针，如果不存在则返回空指针
	 */
	WtLMDBPtr	get_t_db(const char* exchg, const char* code);
};

NS_WTP_END