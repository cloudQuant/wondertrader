/**
 * @file WtSimpDataMgr.h
 * @author Wesley
 * @brief 简单数据管理器头文件
 * @details 定义了简单数据管理器类，用于管理行情数据，实现IDataReaderSink和IDataManager接口
 */

#pragma once
#include <vector>
#include "../Includes/IDataReader.h"
#include "../Includes/IDataManager.h"

#include "../Includes/WTSCollection.hpp"

class WtExecRunner;

NS_WTP_BEGIN
class WTSVariant;
class WTSHisTickData;
class WTSKlineData;
class WTSTickData;
class WTSKlineSlice;
class WTSTickSlice;
class IBaseDataMgr;
class IBaseDataMgr;
class WTSSessionInfo;

/**
 * @brief 简单数据管理器类
 * @details 实现了IDataReaderSink和IDataManager接口，用于管理行情数据，包括实时行情和历史数据
 */
class WtSimpDataMgr : public IDataReaderSink, public IDataManager
{
public:
	/**
	 * @brief 构造函数
	 */
	WtSimpDataMgr();

	/**
	 * @brief 析构函数
	 */
	~WtSimpDataMgr();

private:
	/**
	 * @brief 初始化数据存储模块
	 * @param cfg 数据存储配置
	 * @return 初始化是否成功
	 */
	bool	initStore(WTSVariant* cfg);

public:
	/**
	 * @brief 初始化数据管理器
	 * @param cfg 配置项
	 * @param runner 执行器运行器
	 * @return 初始化是否成功
	 */
	bool	init(WTSVariant* cfg, WtExecRunner* runner);

	/**
	 * @brief 处理推送的行情数据
	 * @param stdCode 标准合约代码
	 * @param newTick 最新的Tick数据
	 */
	void	handle_push_quote(const char* stdCode, WTSTickData* newTick);

	//////////////////////////////////////////////////////////////////////////
	// IDataManager接口实现
	/**
	 * @brief 获取Tick切片数据
	 * @param code 合约代码
	 * @param count 获取的数据条数
	 * @param etime 结束时间，默认为0表示当前
	 * @return Tick切片数据指针
	 */
	WTSTickSlice* get_tick_slice(const char* code, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 获取K线切片数据
	 * @param code 合约代码
	 * @param period K线周期
	 * @param times 周期倍数
	 * @param count 获取的数据条数
	 * @param etime 结束时间，默认为0表示当前
	 * @return K线切片数据指针
	 */
	WTSKlineSlice* get_kline_slice(const char* code, WTSKlinePeriod period, uint32_t times, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 获取最新的Tick数据
	 * @param code 合约代码
	 * @return 最新的Tick数据指针
	 */
	WTSTickData* grab_last_tick(const char* code) override;

	//////////////////////////////////////////////////////////////////////////
	// IDataReaderSink接口实现
	/**
	 * @brief 收到K线数据回调
	 * @param code 合约代码
	 * @param period K线周期
	 * @param newBar 新的K线数据
	 */
	virtual void	on_bar(const char* code, WTSKlinePeriod period, WTSBarStruct* newBar) override;

	/**
	 * @brief 所有K线数据更新完成回调
	 * @param updateTime 更新时间
	 */
	virtual void	on_all_bar_updated(uint32_t updateTime) override;

	/**
	 * @brief 获取基础数据管理器
	 * @return 基础数据管理器指针
	 */
	virtual IBaseDataMgr* get_basedata_mgr() override;

	/**
	 * @brief 获取主力合约管理器
	 * @return 主力合约管理器指针
	 */
	virtual IHotMgr*	get_hot_mgr() override;

	/**
	 * @brief 获取当前日期
	 * @return 当前日期，格式为YYYYMMDD
	 */
	virtual uint32_t	get_date() override;

	/**
	 * @brief 获取当前分钟时间
	 * @return 当前分钟时间，格式为HHMM
	 */
	virtual uint32_t	get_min_time()override;

	/**
	 * @brief 获取当前秒数
	 * @return 当前秒数，格式为SSMMM
	 */
	virtual uint32_t	get_secs() override;

	/**
	 * @brief 数据读取器日志回调
	 * @param ll 日志级别
	 * @param message 日志消息
	 */
	virtual void		reader_log(WTSLogLevel ll, const char* message) override;

	/**
	 * @brief 获取数据读取器
	 * @return 数据读取器指针
	 */
	inline IDataReader* reader() { return _reader; }

	/**
	 * @brief 获取当前原始时间
	 * @return 当前原始时间，格式为HHMM
	 */
	inline uint32_t	get_raw_time() const { return _cur_raw_time; }

	/**
	 * @brief 获取当前交易日
	 * @return 当前交易日，格式为YYYYMMDD
	 */
	inline uint32_t	get_trading_day() const { return _cur_tdate; }

private:
	IDataReader*	_reader;        ///< 数据读取器
	WtExecRunner*	_runner;        ///< 执行器运行器
	WTSSessionInfo*	_s_info;        ///< 交易时段信息

	typedef WTSHashMap<std::string> DataCacheMap;
	DataCacheMap* _bars_cache;      ///< K线缓存
	DataCacheMap* _rt_tick_map;     ///< 实时tick缓存

	uint32_t		_cur_date;      ///< 当前日期,格式如yyyyMMdd
	uint32_t		_cur_act_time;  ///< 当前完整时间,格式如hhmmssmmm
	uint32_t		_cur_raw_time;  ///< 当前真实分钟,格式如hhmm
	uint32_t		_cur_min_time;  ///< 当前1分钟线时间,格式如hhmm
	uint32_t		_cur_secs;      ///< 当前秒数,格式如ssmmm
	uint32_t		_cur_tdate;     ///< 当前交易日,格式如yyyyMMdd

};

NS_WTP_END
