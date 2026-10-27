/*!
 * \file WtUftDtMgr.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief UFT数据管理器头文件
 * \details 定义了WtUftDtMgr类，负责在UFT（超高频交易）策略中处理行情数据的管理、缓存和访问
 */
#pragma once
#include <vector>
#include "../Includes/IDataReader.h"
#include "../Includes/IDataManager.h"

#include "../Includes/FasterDefs.h"
#include "../Includes/WTSCollection.hpp"

NS_WTP_BEGIN
class WTSVariant;
class WTSTickData;
class WTSKlineSlice;
class WTSTickSlice;
class IBaseDataMgr;
class IBaseDataMgr;
class WtUftEngine;

/**
 * @brief UFT策略数据管理器类
 * @details 继承自IDataManager接口，负责超高频交易策略中的数据管理、缓存和访问
 * 包括行情数据、K线数据的获取与处理
 */
class WtUftDtMgr : public IDataManager
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化数据管理器对象，将所有指针成员变量设置为Null
	 */
	WtUftDtMgr();

	/**
	 * @brief 析构函数
	 * @details 释放数据缓存相关资源
	 */
	~WtUftDtMgr();

public:
	/**
	 * @brief 初始化数据管理器
	 * @param cfg 配置项变量集
	 * @param engine UFT引擎指针
	 * @return 初始化是否成功
	 */
	bool	init(WTSVariant* cfg, WtUftEngine* engine);

	/**
	 * @brief 处理实时推送的行情数据
	 * @param stdCode 标准化的合约代码
	 * @param newTick 新的Tick数据指针
	 */
	void	handle_push_quote(const char* stdCode, WTSTickData* newTick);

	//////////////////////////////////////////////////////////////////////////
	//IDataManager 接口
	/**
	 * @brief 获取Tick切片数据
	 * @param stdCode 标准化的合约代码
	 * @param count 请求的数据条数
	 * @param etime 结束时间，默认为0（当前时间）
	 * @return Tick切片数据指针
	 */
	virtual WTSTickSlice* get_tick_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 获取委托队列切片数据
	 * @param stdCode 标准化的合约代码
	 * @param count 请求的数据条数
	 * @param etime 结束时间，默认为0（当前时间）
	 * @return 委托队列切片数据指针
	 */
	virtual WTSOrdQueSlice* get_order_queue_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 获取委托明细切片数据
	 * @param stdCode 标准化的合约代码
	 * @param count 请求的数据条数
	 * @param etime 结束时间，默认为0（当前时间）
	 * @return 委托明细切片数据指针
	 */
	virtual WTSOrdDtlSlice* get_order_detail_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 获取成交切片数据
	 * @param stdCode 标准化的合约代码
	 * @param count 请求的数据条数
	 * @param etime 结束时间，默认为0（当前时间）
	 * @return 成交切片数据指针
	 */
	virtual WTSTransSlice* get_transaction_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 获取K线切片数据
	 * @param stdCode 标准化的合约代码
	 * @param period K线周期
	 * @param times 周期倍数
	 * @param count 请求的数据条数
	 * @param etime 结束时间，默认为0（当前时间）
	 * @return K线切片数据指针
	 */
	virtual WTSKlineSlice* get_kline_slice(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 获取最新的Tick数据
	 * @param stdCode 标准化的合约代码
	 * @return 最新的Tick数据指针
	 */
	virtual WTSTickData* grab_last_tick(const char* stdCode) override;

private:
	WtUftEngine*		_engine;		///< UFT引擎指针

	wt_hashset<std::string> _subed_basic_bars; ///< 订阅的基础K线列表
	
	/**
	 * @brief 数据缓存映射类型
	 * @details 使用WTSHashMap模板实现的字符串映射类型，用于存储数据缓存
	 */
	typedef WTSHashMap<std::string> DataCacheMap;
	DataCacheMap*	_bars_cache;	///< K线缓存
	DataCacheMap*	_ticks_cache;	///< 历史Tick缓存
	DataCacheMap*	_rt_tick_map;	///< 实时tick缓存

	/**
	 * @brief K线通知项结构体
	 * @details 用于存储K线通知相关信息
	 */
	typedef struct _NotifyItem
	{
		std::string _code;     ///< 合约代码
		std::string _period;   ///< 周期标识
		uint32_t	_times;    ///< 周期倍数
		WTSBarStruct* _newBar; ///< 新K线结构指针
	} NotifyItem;

	std::vector<NotifyItem> _bar_notifies; ///< K线通知项列表
};

NS_WTP_END