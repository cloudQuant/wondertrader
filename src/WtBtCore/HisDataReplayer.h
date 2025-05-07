/*!
 * \file HisDataReplayer.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 历史数据回放器声明
 * \details 定义了历史数据回放器类，用于重放历史行情数据并接收策略回调。
 * 包含了回放各种周期的历史K线、Tick数据等，可以快速模拟交易全过程。
 */
#pragma once
#include <string>
#include <set>
#include "HisDataMgr.h"
#include "../WtDataStorage/DataDefine.h"

#include "../Includes/FasterDefs.h"
#include "../Includes/WTSMarcos.h"
#include "../Includes/WTSTypes.h"

#include "../WTSTools/WTSHotMgr.h"
#include "../WTSTools/WTSBaseDataMgr.h"

NS_WTP_BEGIN
class WTSTickData;
class WTSVariant;
class WTSKlineSlice;
class WTSTickSlice;
class WTSOrdDtlSlice;
class WTSOrdQueSlice;
class WTSTransSlice;
class WTSSessionInfo;
class WTSCommodityInfo;

class WTSOrdDtlData;
class WTSOrdQueData;
class WTSTransData;

class EventNotifier;
NS_WTP_END

USING_NS_WTP;

/**
 * @brief 数据接收器接口
 * @details 定义了接收并处理回放器生成的各类行情数据的方法
 */
class IDataSink
{
public:
	/**
	 * @brief 处理Tick数据
	 * @param stdCode 标准合约代码
	 * @param curTick 当前的Tick数据
	 * @param pxType 价格类型（0-开盘价，1-最高价，2-最低价，3-收盘价）
	 */
	virtual void	handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType) = 0;
	/**
	 * @brief 处理委托队列数据
	 * @param stdCode 标准合约代码
	 * @param curOrdQue 当前的委托队列数据
	 */
	virtual void	handle_order_queue(const char* stdCode, WTSOrdQueData* curOrdQue) {};
	/**
	 * @brief 处理委托明细数据
	 * @param stdCode 标准合约代码
	 * @param curOrdDtl 当前的委托明细数据
	 */
	virtual void	handle_order_detail(const char* stdCode, WTSOrdDtlData* curOrdDtl) {};
	/**
	 * @brief 处理逐笔成交数据
	 * @param stdCode 标准合约代码
	 * @param curTrans 当前的逐笔成交数据
	 */
	virtual void	handle_transaction(const char* stdCode, WTSTransData* curTrans) {};
	/**
	 * @brief 处理K线周期结束事件
	 * @param stdCode 标准合约代码
	 * @param period 周期标识符（如"m1"、"d1"等）
	 * @param times 周期倍数
	 * @param newBar 新生成的K线数据
	 */
	virtual void	handle_bar_close(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) = 0;
	/**
	 * @brief 处理定时任务
	 * @param uDate 日期，格式为YYYYMMDD
	 * @param uTime 时间，格式为HHMMSS
	 */
	virtual void	handle_schedule(uint32_t uDate, uint32_t uTime) = 0;

	/**
	 * @brief 处理初始化事件
	 * @details 回测引擎初始化时调用，策略可以在这里进行初始化操作
	 */
	virtual void	handle_init() = 0;
	/**
	 * @brief 处理交易日开始事件
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 * @details 在每个新交易日开始时调用
	 */
	virtual void	handle_session_begin(uint32_t curTDate) = 0;
	/**
	 * @brief 处理交易日结束事件
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 * @details 在每个交易日结束时调用
	 */
	virtual void	handle_session_end(uint32_t curTDate) = 0;
	/**
	 * @brief 处理回放完成事件
	 * @details 当所有回测数据全部回放完成时调用
	 */
	virtual void	handle_replay_done() {}

	/**
	 * @brief 处理交易时段结束事件
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMMSS
	 * @details 当交易时段结束时调用，一个交易日可能包含多个交易时段
	 */
	virtual void	handle_section_end(uint32_t curTDate, uint32_t curTime) {}
};

/**
 * @brief 历史K线数据加载回调函数类型
 * @param obj 回传用的对象指针，原样返回即可
 * @param firstBar 第一个K线数据指针，连续内存中的数据结构数组
 * @param count 数据条数
 * @details 用于加载历史K线数据并传递给回放器
 */
typedef void(*FuncReadBars)(void* obj, WTSBarStruct* firstBar, uint32_t count);

/**
 * @brief 复权因子加载回调函数类型
 * @param obj 回传用的对象指针，原样返回即可
 * @param stdCode 标准合约代码
 * @param dates 日期数组，格式为YYYYMMDD
 * @param factors 复权因子数组
 * @param count 因子数量
 * @details 用于加载股票复权因子，进行前复权或后复权计算
 */
typedef void(*FuncReadFactors)(void* obj, const char* stdCode, uint32_t* dates, double* factors, uint32_t count);

/**
 * @brief Tick数据加载回调函数类型
 * @param obj 回传用的对象指针，原样返回即可
 * @param firstItem 第一个Tick数据指针，连续内存中的数据结构数组
 * @param count 数据条数
 * @details 用于加载逐笔行情数据，提供更高精度的回测
 */
typedef void(*FuncReadTicks)(void* obj, WTSTickStruct* firstItem, uint32_t count);

/**
 * @brief 委托明细数据加载回调函数类型
 * @param obj 回传用的对象指针，原样返回即可
 * @param firstItem 第一个委托明细数据指针，连续内存中的数据结构数组
 * @param count 数据条数
 * @details 用于加载高频委托明细数据，提供更精确的市场循边信息
 */
typedef void(*FuncReadOrdDtl)(void* obj, WTSOrdDtlStruct* firstItem, uint32_t count);

/**
 * @brief 委托队列数据加载回调函数类型
 * @param obj 回传用的对象指针，原样返回即可
 * @param firstItem 第一个委托队列数据指针，连续内存中的数据结构数组
 * @param count 数据条数
 * @details 用于加载高频委托队列数据，可以观察市场深度
 */
typedef void(*FuncReadOrdQue)(void* obj, WTSOrdQueStruct* firstItem, uint32_t count);

/**
 * @brief 逐笔成交数据加载回调函数类型
 * @param obj 回传用的对象指针，原样返回即可
 * @param firstItem 第一个逐笔成交数据指针，连续内存中的数据结构数组
 * @param count 数据条数
 * @details 用于加载高频逐笔成交数据，提供更精确的市场成交信息
 */
typedef void(*FuncReadTrans)(void* obj, WTSTransStruct* firstItem, uint32_t count);

/**
 * @brief 回测数据加载器接口
 * @details 用于自定义数据源的加载器接口，实现了加载各种历史数据的方法
 * 包括K线数据、Tick数据、复权因子等
 */
class IBtDataLoader
{
public:
	/**
	 * @brief 加载最终历史K线数据
	 * @param obj 回传用的对象指针，原样返回即可
	 * @param stdCode 合约代码
	 * @param period K线周期
	 * @param cb 回调函数
	 * @return 是否成功加载数据
	 * @details 加载已经处理好的最终数据，如复权后的股票数据、主力合约连续数据等
	 * 与loadRawHisBars的区别在于，loadFinalHisBars返回的数据直接使用，不再进行加工
	 */
	virtual bool loadFinalHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) = 0;

	/**
	 * @brief 加载原始历史K线数据
	 * @param obj 回传用的对象指针，原样返回即可
	 * @param stdCode 合约代码
	 * @param period K线周期
	 * @param cb 回调函数
	 * @return 是否成功加载数据
	 * @details 加载未经处理的原始K线数据，加载后可能需要进一步处理，如复权、连续合约生成等
	 */
	virtual bool loadRawHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) = 0;

	/**
	 * @brief 加载全部除权因子
	 * @param obj 回传用的对象指针，原样返回即可
	 * @param cb 回调函数
	 * @return 是否成功加载数据
	 * @details 加载所有股票的除权因子数据，用于股票数据的前复权或后复权处理
	 */
	virtual bool loadAllAdjFactors(void* obj, FuncReadFactors cb) = 0;

	/**
	 * @brief 根据合约加载除权因子
	 * @param obj 回传用的对象指针，原样返回即可
	 * @param stdCode 合约代码
	 * @param cb 回调函数
	 * @return 是否成功加载数据
	 * @details 加载指定股票的除权因子数据，用于该股票数据的前复权或后复权处理
	 */
	virtual bool loadAdjFactors(void* obj, const char* stdCode, FuncReadFactors cb) = 0;

	/**
	 * @brief 加载历史Tick数据
	 * @param obj 回传用的对象指针，原样返回即可
	 * @param stdCode 合约代码
	 * @param uDate 日期，格式为YYYYMMDD
	 * @param cb 回调函数
	 * @return 是否成功加载数据
	 * @details 加载指定日期的历史Tick数据，用于高精度回测或模拟交易
	 */
	virtual bool loadRawHisTicks(void* obj, const char* stdCode, uint32_t uDate, FuncReadTicks cb) = 0;

	/**
	 * @brief 是否自动转存为DSB格式
	 * @return 默认返回true
	 * @details 控制数据是否自动转存为高效的二进制DSB格式，加快下次读取速度
	 */
	virtual bool isAutoTrans() { return true; }
};

/**
 * @brief 历史数据回放器类
 * @details 实现了各种历史数据的加载、缓存和回放功能，
 * 支持K线、Tick、逐笔成交等多种数据的处理和回放，
 * 可用于实现策略回测。
 */
class HisDataReplayer
{

private:
	/**
	 * @brief 高频数据列表模板类
	 * @tparam T 数据类型，如WTSTickStruct、WTSOrdQueStruct等
	 * @details 存储和管理高频数据，如Tick、委托队列、委托明细等
	 */
	template <typename T>
	class HftDataList
	{
	public:
		std::string		_code;		///< 合约代码
		uint32_t		_date;		///< 数据日期，格式为YYYYMMDD
		/*
		 * By Wesley @ 2022.03.21
		 * 游标，用于标记下一条数据的位置，或者说已经回放过的条数
		 * 未初始化时，游标为UINT_MAX，一旦初始化，游标必然是大于0的
		 */
		std::size_t		_cursor;	///< 数据遍历游标，标记当前读取位置
		std::size_t		_count;	///< 数据项总数

		std::vector<T> _items;	///< 数据项存储容器

		/**
		 * @brief 构造函数
		 * @details 初始化游标为UINT_MAX，计数为0，日期为0
		 */
		HftDataList() :_cursor(UINT_MAX), _count(0), _date(0){}
	};

	typedef wt_hashmap<std::string, HftDataList<WTSTickStruct>>		TickCache;
	typedef wt_hashmap<std::string, HftDataList<WTSOrdDtlStruct>>	OrdDtlCache;
	typedef wt_hashmap<std::string, HftDataList<WTSOrdQueStruct>>	OrdQueCache;
	typedef wt_hashmap<std::string, HftDataList<WTSTransStruct>>	TransCache;


	typedef struct _BarsList
	{
		std::string		_code;
		WTSKlinePeriod	_period;
		/*
		 * By Wesley @ 2022.03.21
		 * 游标，用于标记下一条数据的位置，或者说已经回放过的条数
		 * 未初始化时，游标为UINT_MAX，一旦初始化，游标必然是大于0的
		 */
		uint32_t		_cursor;
		uint32_t		_count;
		uint32_t		_times;

		std::vector<WTSBarStruct>	_bars;
		double			_factor;	//最后一条复权因子

		uint32_t		_untouch_days;	//未用到的天数

		inline void mark()
		{
			_untouch_days = 0;
		}

		inline std::size_t size()
		{
			return sizeof(WTSBarStruct)*_bars.size();
		}

		_BarsList() :_cursor(UINT_MAX), _count(0), _times(1), _factor(1), _untouch_days(0){}
	} BarsList;

	/*
	 *	By Wesley @ 2022.03.13
	 *	这里把缓存改成智能指针
	 *	因为有用户发现如果在oncalc的时候获取未在oninit中订阅的K线的时候
	 *	因为使用BarList的引用，当K线缓存的map重新插入新的K线以后
	 *	引用的地方失效了，会引用到错误地址
	 *	我怀疑这里有可能是重新拷贝了一下数据
	 *	这里改成智能指针就能避免这个问题，因为不管map自己的内存如何组织
	 *	智能指针指向的地址都是不会变的
	 */
	typedef std::shared_ptr<BarsList> BarsListPtr;
	typedef wt_hashmap<std::string, BarsListPtr>	BarsCache;

	/**
	 * @brief 任务周期类型枚举
	 * @details 定义了定时任务的周期性类型，包括不重复、分钟线、日、周、月、年等周期
	 */
	typedef enum tagTaskPeriodType
	{
		TPT_None,		///< 不重复，只执行一次
		TPT_Minute = 4,	///< 分钟线周期，每分钟执行
		TPT_Daily = 8,	///< 每个交易日执行
		TPT_Weekly,		///< 每周执行，遇到节假日的话要顺延
		TPT_Monthly,	///< 每月执行，遇到节假日顺延
		TPT_Yearly		///< 每年执行，遇到节假日顺延
	}TaskPeriodType;

	/**
	 * @brief 任务信息结构体
	 * @details 定义了定时任务的各项配置信息，包括任务ID、名称、时间设置等
	 */
	typedef struct _TaskInfo
	{
		uint32_t	_id;			///< 任务全局唯一ID
		char		_name[16];		///< 任务名称
		char		_trdtpl[16];	///< 交易日模板
		char		_session[16];	///< 交易时间模板
		uint32_t	_day;			///< 日期设置，根据周期变化：每日为0，每周为0~6（对应周日到周六），每月为1~31，每年为0101~1231
		uint32_t	_time;			///< 时间设置，格式为HHMM，精确到分钟
		bool		_strict_time;	///< 是否严格时间，严格时间即只有时间相等才会执行，非严格时间则大于等于触发时间都会执行

		uint64_t	_last_exe_time;	///< 上次执行时间，用于防止重复执行

		TaskPeriodType	_period;	///< 任务周期类型
	} TaskInfo;

	typedef std::shared_ptr<TaskInfo> TaskInfoPtr;



public:
	HisDataReplayer();
	~HisDataReplayer();

private:
	/**
	 * @brief 从自定义数据文件缓存历史K线数据
	 * 
	 * @param key 缓存键值
	 * @param stdCode 标准合约代码
	 * @param period K线周期
	 * @param bForBars 是否用于K线回放，默认为true
	 * @return true 缓存成功
	 * @return false 缓存失败
	 */
	bool		cacheRawBarsFromBin(const std::string& key, const char* stdCode, WTSKlinePeriod period, bool bForBars = true);

	/**
	 * @brief 从CSV文件缓存历史K线数据
	 * 
	 * @param key 缓存键值
	 * @param stdCode 标准合约代码
	 * @param period K线周期
	 * @param bSubbed 是否已订阅，默认为true
	 * @return true 缓存成功
	 * @return false 缓存失败
	 */
	bool		cacheRawBarsFromCSV(const std::string& key, const char* stdCode, WTSKlinePeriod period, bool bSubbed = true);

	/**
	 * @brief 从自定义数据文件缓存历史Tick数据
	 * 
	 * @param key 缓存键值
	 * @param stdCode 标准合约代码
	 * @param uDate 日期，格式为YYYYMMDD
	 * @return true 缓存成功
	 * @return false 缓存失败
	 */
	bool		cacheRawTicksFromBin(const std::string& key, const char* stdCode, uint32_t uDate);

	/**
	 * @brief 从自定义数据文件缓存历史委托明细数据
	 * 
	 * @param key 缓存键值
	 * @param stdCode 标准合约代码
	 * @param uDate 日期，格式为YYYYMMDD
	 * @return true 缓存成功
	 * @return false 缓存失败
	 */
	bool		cacheRawOrdDtlFromBin(const std::string& key, const char* stdCode, uint32_t uDate);

	/**
	 * @brief 从自定义数据文件缓存历史委托队列数据
	 * 
	 * @param key 缓存键值
	 * @param stdCode 标准合约代码
	 * @param uDate 日期，格式为YYYYMMDD
	 * @return true 缓存成功
	 * @return false 缓存失败
	 */
	bool		cacheRawOrdQueFromBin(const std::string& key, const char* stdCode, uint32_t uDate);

	/**
	 * @brief 从自定义数据文件缓存历史成交明细数据
	 * 
	 * @param key 缓存键值
	 * @param stdCode 标准合约代码
	 * @param uDate 日期，格式为YYYYMMDD
	 * @return true 缓存成功
	 * @return false 缓存失败
	 */
	bool		cacheRawTransFromBin(const std::string& key, const char* stdCode, uint32_t uDate);

	/**
	 * @brief 从CSV文件缓存历史Tick数据
	 * 
	 * @param key 缓存键值
	 * @param stdCode 标准合约代码
	 * @param uDate 日期，格式为YYYYMMDD
	 * @return true 缓存成功
	 * @return false 缓存失败
	 */
	bool		cacheRawTicksFromCSV(const std::string& key, const char* stdCode, uint32_t uDate);

	/**
	 * @brief 从外部数据加载器缓存历史K线数据
	 * 
	 * @param key 缓存键值
	 * @param stdCode 标准合约代码
	 * @param period K线周期
	 * @param bSubbed 是否已订阅，默认为true
	 * @return true 缓存成功
	 * @return false 缓存失败
	 */
	bool		cacheFinalBarsFromLoader(const std::string& key, const char* stdCode, WTSKlinePeriod period, bool bSubbed = true);

	/**
	 * @brief 从外部数据加载器缓存历史Tick数据
	 * 
	 * @param key 缓存键值
	 * @param stdCode 标准合约代码
	 * @param uDate 日期，格式为YYYYMMDD
	 * @return true 缓存成功
	 * @return false 缓存失败
	 */
	bool		cacheRawTicksFromLoader(const std::string& key, const char* stdCode, uint32_t uDate);

	/**
	 * @brief 缓存整合的期货合约历史K线数据（针对.HOT和.2ND等主力合约）
	 * 
	 * @param codeInfo 合约信息指针
	 * @param key 缓存键值
	 * @param stdCode 标准合约代码
	 * @param period K线周期
	 * @param bSubbed 是否已订阅，默认为true
	 * @return true 缓存成功
	 * @return false 缓存失败
	 */
	bool		cacheIntegratedFutBarsFromBin(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period, bool bSubbed = true);

	/**
	 * @brief 缓存复权后的股票K线数据
	 * 
	 * @param codeInfo 合约信息指针
	 * @param key 缓存键值
	 * @param stdCode 标准合约代码
	 * @param period K线周期
	 * @param bSubbed 是否已订阅，默认为true
	 * @return true 缓存成功
	 * @return false 缓存失败
	 */
	bool		cacheAdjustedStkBarsFromBin(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period, bool bSubbed = true);

	/**
	 * @brief 分钟结束时执行的回调函数
	 * 
	 * @param uDate 日期，格式为YYYYMMDD
	 * @param uTime 时间，格式为HHMMSS
	 * @param endTDate 结束交易日，默认为0
	 * @param tickSimulated 是否为tick模拟的分钟线，默认为true
	 */
	void		onMinuteEnd(uint32_t uDate, uint32_t uTime, uint32_t endTDate = 0, bool tickSimulated = true);

	/**
	 * @brief 从配置文件加载手续费设置
	 * 
	 * @param filename 手续费配置文件路径
	 */
	void		loadFees(const char* filename);

	/**
	 * @brief 回放高频数据（Tick、委托明细、委托队列、成交明细）
	 * 
	 * @param stime 开始时间，格式为时间戳
	 * @param etime 结束时间，格式为时间戳
	 * @return true 回放成功
	 * @return false 回放失败
	 */
	bool		replayHftDatas(uint64_t stime, uint64_t etime);

	/**
	 * @brief 按交易日回放高频数据
	 * 
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 * @return uint64_t 返回当日最后一条数据的时间戳
	 */
	uint64_t	replayHftDatasByDay(uint32_t curTDate);

	/**
	 * @brief 使用未订阅的K线模拟Tick数据
	 * 
	 * @param stime 开始时间戳
	 * @param etime 结束时间戳
	 * @param endTDate 结束交易日，默认为0
	 * @param pxType 价格类型，0-均价，1-最高价，2-最低价，3-收盘价，默认为0
	 */
	void		simTickWithUnsubBars(uint64_t stime, uint64_t etime, uint32_t endTDate = 0, int pxType = 0);

	/**
	 * @brief 模拟Tick数据
	 * 
	 * @param uDate 当前日期，格式为YYYYMMDD
	 * @param uTime 当前时间，格式为HHMMSS
	 * @param endTDate 结束交易日，默认为0
	 * @param pxType 价格类型，0-均价，1-最高价，2-最低价，3-收盘价，默认为0
	 */
	void		simTicks(uint32_t uDate, uint32_t uTime, uint32_t endTDate = 0, int pxType = 0);

	/**
	 * @brief 检查指定合约在指定交易日的Tick数据是否存在
	 * 
	 * @param stdCode 标准合约代码
	 * @param uDate 交易日，格式为YYYYMMDD
	 * @return true 存在数据
	 * @return false 数据不存在
	 */
	inline bool		checkTicks(const char* stdCode, uint32_t uDate);

	/**
	 * @brief 检查指定合约在指定交易日的委托明细数据是否存在
	 * 
	 * @param stdCode 标准合约代码
	 * @param uDate 交易日，格式为YYYYMMDD
	 * @return true 存在数据
	 * @return false 数据不存在
	 */
	inline bool		checkOrderDetails(const char* stdCode, uint32_t uDate);

	/**
	 * @brief 检查指定合约在指定交易日的委托队列数据是否存在
	 * 
	 * @param stdCode 标准合约代码
	 * @param uDate 交易日，格式为YYYYMMDD
	 * @return true 存在数据
	 * @return false 数据不存在
	 */
	inline bool		checkOrderQueues(const char* stdCode, uint32_t uDate);

	/**
	 * @brief 检查指定合约在指定交易日的成交明细数据是否存在
	 * 
	 * @param stdCode 标准合约代码
	 * @param uDate 交易日，格式为YYYYMMDD
	 * @return true 存在数据
	 * @return false 数据不存在
	 */
	inline bool		checkTransactions(const char* stdCode, uint32_t uDate);

	/**
	 * @brief 检查未订阅的K线数据
	 * @details 检查并处理未订阅的K线数据，用于模拟Tick的生成
	 */
	void		checkUnbars();

	/**
	 * @brief 从文件加载股票复权因子
	 * 
	 * @param adjfile 复权因子文件路径
	 * @return true 加载成功
	 * @return false 加载失败
	 */
	bool		loadStkAdjFactorsFromFile(const char* adjfile);

	/**
	 * @brief 从数据加载器加载股票复权因子
	 * 
	 * @return true 加载成功
	 * @return false 加载失败
	 */
	bool		loadStkAdjFactorsFromLoader();

	/**
	 * @brief 检查指定交易日所有合约的Tick数据是否存在
	 * 
	 * @param uDate 交易日，格式为YYYYMMDD
	 * @return true 存在所有合约的数据
	 * @return false 存在数据缺失
	 */
	bool		checkAllTicks(uint32_t uDate);

	/**
	 * @brief 获取下一个Tick数据的时间
	 * 
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 * @param stime 起始时间戳，默认为UINT64_MAX
	 * @return uint64_t 下一个Tick数据的时间戳
	 */
	inline	uint64_t	getNextTickTime(uint32_t curTDate, uint64_t stime = UINT64_MAX);
	/**
	 * @brief 获取下一个委托队列数据的时间
	 * 
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 * @param stime 起始时间戳，默认为UINT64_MAX
	 * @return uint64_t 下一个委托队列数据的时间戳
	 */
	inline	uint64_t	getNextOrdQueTime(uint32_t curTDate, uint64_t stime = UINT64_MAX);
	/**
	 * @brief 获取下一个委托明细数据的时间
	 * 
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 * @param stime 起始时间戳，默认为UINT64_MAX
	 * @return uint64_t 下一个委托明细数据的时间戳
	 */
	inline	uint64_t	getNextOrdDtlTime(uint32_t curTDate, uint64_t stime = UINT64_MAX);
	/**
	 * @brief 获取下一个成交明细数据的时间
	 * 
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 * @param stime 起始时间戳，默认为UINT64_MAX
	 * @return uint64_t 下一个成交明细数据的时间戳
	 */
	inline	uint64_t	getNextTransTime(uint32_t curTDate, uint64_t stime = UINT64_MAX);

	/**
	 * @brief 重置回测状态
	 * @details 重置所有内部状态和数据缓存，为新一轮回测做准备
	 */
	void		reset();


	/**
	 * @brief 将回测状态保存到文件
	 * 
	 * @param stdCode 标准合约代码
	 * @param period K线周期
	 * @param times 回测次数
	 * @param stime 开始时间戳
	 * @param etime 结束时间戳
	 * @param progress 回测进度，0.0-1.0之间
	 * @param elapse 经过的时间（毫秒）
	 */
	void		dump_btstate(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint64_t stime, uint64_t etime, double progress, int64_t elapse);
	
	/**
	 * @brief 通知回测状态给监听器
	 * 
	 * @param stdCode 标准合约代码
	 * @param period K线周期
	 * @param times 回测次数
	 * @param stime 开始时间戳
	 * @param etime 结束时间戳
	 * @param progress 回测进度，0.0-1.0之间
	 */
	void		notify_state(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint64_t stime, uint64_t etime, double progress);

	/**
	 * @brief 定位特定时间点在K线数据中的索引位置
	 * 
	 * @param key 缓存键值，通常是合约代码+周期
	 * @param curTime 当前时间戳
	 * @param bUpperBound 是否返回上界位置，默认为false
	 * @return uint32_t 返回找到的索引位置
	 */
	uint32_t	locate_barindex(const std::string& key, uint64_t curTime, bool bUpperBound = false);

	/**
	 * @brief 按照K线进行回测
	 * @details 使用K线数据执行回测过程，按照K线周期逐时间推进
	 * 
	 * @param bNeedDump 是否将回测进度落地到文件中，默认为false
	 */
	void	run_by_bars(bool bNeedDump = false);

	/**
	 * @brief 按照定时任务进行回测
	 * @details 使用定时任务机制执行回测过程，按照任务的时间周期触发
	 * 
	 * @param bNeedDump 是否将回测进度落地到文件中，默认为false
	 */
	void	run_by_tasks(bool bNeedDump = false);

	/**
	 * @brief 按照Tick数据进行回测
	 * @details 使用Tick数据执行回测过程，每个Tick逐一处理，提供最精细的回测粒度
	 * 
	 * @param bNeedDump 是否将回测进度落地到文件中，默认为false
	 */
	void	run_by_ticks(bool bNeedDump = false);

	/**
	 * @brief 检查数据缓存中的交易日
	 * @details 扫描所有缓存的数据，生成完整的交易日列表，用于回测时间推进
	 */
	void	check_cache_days();

public:
	/**
	 * @brief 初始化回放器
	 * @param cfg 配置项
	 * @param notifier 事件通知器，默认为NULL
	 * @param dataLoader 数据加载器，默认为NULL
	 * @return 是否初始化成功
	 * @details 初始化回放器，加载配置文件和设置数据加载器
	 */
	bool init(WTSVariant* cfg, EventNotifier* notifier = NULL, IBtDataLoader* dataLoader = NULL);

	/**
	 * @brief 准备回测环境
	 * @return 是否准备成功
	 * @details 在执行回测前准备必要的数据和环境
	 */
	bool prepare();

	/**
	 * @brief 运行回测
	 * @param bNeedDump 是否将回测进度落地到文件中，默认为false
	 * @details 执行回测过程，可选择是否将进度信息保存到文件
	 */
	void run(bool bNeedDump = false);
	
	/**
	 * @brief 停止回测
	 * @details 中止正在运行的回测过程
	 */
	void stop();

	/**
	 * @brief 清理缓存
	 * @details 清除内存中的历史数据缓存，释放内存
	 */
	void clear_cache();

	/**
	 * @brief 设置回测时间范围
	 * @param stime 开始时间，格式为YYYYMMDDHHMMSS
	 * @param etime 结束时间，格式为YYYYMMDDHHMMSS
	 * @details 设置回测的起止时间范围，限定回测的数据区间
	 */
	inline void set_time_range(uint64_t stime, uint64_t etime)
	{
		_begin_time = stime;
		_end_time = etime;
	}

	/**
	 * @brief 启用或禁用Tick回测
	 * @param bEnabled 是否启用Tick回测，默认为true
	 * @details 控制是否启用Tick级别的回测，开启后可提供更高精度的回测结果
	 */
	inline void enable_tick(bool bEnabled = true)
	{
		_tick_enabled = bEnabled;
	}

	/**
	 * @brief 注册数据接收器
	 * @param listener 数据接收器对象指针
	 * @param sinkName 接收器名称
	 * @details 将数据接收器注册到回放器中，用于接收回放的各类数据和事件
	 */
	inline void register_sink(IDataSink* listener, const char* sinkName) 
	{
		_listener = listener; 
		_stra_name = sinkName;
	}

	/**
	 * @brief 注册定时任务
	 * @param taskid 任务ID
	 * @param date 日期参数，根据周期不同而变化：每日为0，每周为0~6（周日到周六），每月为1~31，每年为0101~1231
	 * @param time 时间，精确到分钟，格式为HHMM
	 * @param period 时间周期，可以是分钟、天、周、月、年
	 * @param trdtpl 交易日模板，默认为"CHINA"
	 * @param session 交易时段模板，默认为"TRADING"
	 * @details 注册一个定时任务，用于定期执行特定操作，如信号计算、新的交易决策等
	 */
	void register_task(uint32_t taskid, uint32_t date, uint32_t time, const char* period, const char* trdtpl = "CHINA", const char* session = "TRADING");

	/**
	 * @brief 获取K线切片数据
	 * @param stdCode 标准合约代码
	 * @param period 周期标识符（如"m1"、"d1"等）
	 * @param count 请求的K线数量
	 * @param times 周期倍数，默认为1
	 * @param isMain 是否为主图周期，默认为false
	 * @return K线切片数据指针
	 * @details 获取指定合约的历史K线数据切片，用于策略分析和参考
	 */
	WTSKlineSlice* get_kline_slice(const char* stdCode, const char* period, uint32_t count, uint32_t times = 1, bool isMain = false);

	/**
	 * @brief 获取Tick切片数据
	 * @param stdCode 标准合约代码
	 * @param count 请求的Tick数量
	 * @param etime 结束时间，默认为0（使用当前时间）
	 * @return Tick切片数据指针
	 * @details 获取指定合约的历史Tick数据切片，提供高精度的市场行情信息
	 */
	WTSTickSlice* get_tick_slice(const char* stdCode, uint32_t count, uint64_t etime = 0);

	/**
	 * @brief 获取委托明细切片数据
	 * @param stdCode 标准合约代码
	 * @param count 请求的委托明细数量
	 * @param etime 结束时间，默认为0（使用当前时间）
	 * @return 委托明细切片数据指针
	 * @details 获取指定合约的历史委托明细数据切片，提供高级别的委托信息
	 */
	WTSOrdDtlSlice* get_order_detail_slice(const char* stdCode, uint32_t count, uint64_t etime = 0);

	/**
	 * @brief 获取委托队列切片数据
	 * @param stdCode 标准合约代码
	 * @param count 请求的委托队列数量
	 * @param etime 结束时间，默认为0（使用当前时间）
	 * @return 委托队列切片数据指针
	 * @details 获取指定合约的历史委托队列数据切片，提供市场深度信息
	 */
	WTSOrdQueSlice* get_order_queue_slice(const char* stdCode, uint32_t count, uint64_t etime = 0);

	/**
	 * @brief 获取成交明细切片数据
	 * @param stdCode 标准合约代码
	 * @param count 请求的成交明细数量
	 * @param etime 结束时间，默认为0（使用当前时间）
	 * @return 成交明细切片数据指针
	 * @details 获取指定合约的历史成交明细数据切片，提供高精度的市场成交信息
	 */
	WTSTransSlice* get_transaction_slice(const char* stdCode, uint32_t count, uint64_t etime = 0);

	/**
	 * @brief 获取最新的Tick数据
	 * @param stdCode 标准合约代码
	 * @return 最新的Tick数据指针
	 * @details 获取指定合约的最新一条Tick数据，用于当前市场状态分析
	 */
	WTSTickData* get_last_tick(const char* stdCode);

	/**
	 * @brief 获取当前日期
	 * @return 当前日期，格式为YYYYMMDD
	 * @details 获取回测中当前模拟的日期
	 */
	uint32_t get_date() const{ return _cur_date; }
	
	/**
	 * @brief 获取当前分钟时间
	 * @return 当前分钟时间，格式为HHMM
	 * @details 获取回测中当前模拟的分钟时间
	 */
	uint32_t get_min_time() const{ return _cur_time; }
	
	/**
	 * @brief 获取原始时间
	 * @return 原始时间，格式为HHMMSS
	 * @details 获取回测中当前模拟的原始时间格式
	 */
	uint32_t get_raw_time() const{ return _cur_time; }
	
	/**
	 * @brief 获取当前秒数
	 * @return 当前秒数
	 * @details 获取回测中当前模拟的秒数部分
	 */
	uint32_t get_secs() const{ return _cur_secs; }
	
	/**
	 * @brief 获取当前交易日
	 * @return 当前交易日，格式为YYYYMMDD
	 * @details 获取回测中当前模拟的交易日期
	 */
	uint32_t get_trading_date() const{ return _cur_tdate; }

	/**
	 * @brief 计算交易费用
	 * @param stdCode 标准合约代码
	 * @param price 交易价格
	 * @param qty 交易数量
	 * @param offset 开平仓标记（0-开仓，1-平仓，2-平今）
	 * @return 计算出的交易费用
	 * @details 根据交易品种、价格、数量和开平仓标志计算交易费用
	 */
	double calc_fee(const char* stdCode, double price, double qty, uint32_t offset);
	/**
	 * @brief 获取交易时段信息
	 * @param sid 交易时段ID或合约代码
	 * @param isCode 是否是合约代码，默认为false
	 * @return 交易时段信息指针
	 * @details 获取指定交易时段的详细信息，如开盘收盘时间等
	 */
	WTSSessionInfo*		get_session_info(const char* sid, bool isCode = false);
	
	/**
	 * @brief 获取商品信息
	 * @param stdCode 标准合约代码
	 * @return 商品信息指针
	 * @details 获取指定合约的详细商品信息，如合约乘数、最小变动单位等
	 */
	WTSCommodityInfo*	get_commodity_info(const char* stdCode);
	/**
	 * @brief 获取当前价格
	 * @param stdCode 标准合约代码
	 * @return 当前价格
	 * @details 获取指定合约的当前最新价格
	 */
	double get_cur_price(const char* stdCode);
	
	/**
	 * @brief 获取日线价格
	 * @param stdCode 标准合约代码
	 * @param flag 价格标志（0-收盘价，1-开盘价，2-最高价，3-最低价）
	 * @return 日线价格
	 * @details 获取指定合约在当前交易日的开盘、收盘、最高或最低价格
	 */
	double get_day_price(const char* stdCode, int flag = 0);

	/**
	 * @brief 获取原始合约代码
	 * @param stdCode 标准合约代码
	 * @return 原始合约代码
	 * @details 将标准化的合约代码转换为原始交易所格式的合约代码
	 */
	std::string get_rawcode(const char* stdCode);

	/**
	 * @brief 订阅Tick数据
	 * @param sid 订阅者ID
	 * @param stdCode 标准合约代码
	 * @details 为指定的订阅者订阅特定合约的Tick数据
	 */
	void sub_tick(uint32_t sid, const char* stdCode);
	
	/**
	 * @brief 订阅委托队列数据
	 * @param sid 订阅者ID
	 * @param stdCode 标准合约代码
	 * @details 为指定的订阅者订阅特定合约的委托队列数据
	 */
	void sub_order_queue(uint32_t sid, const char* stdCode);
	
	/**
	 * @brief 订阅委托明细数据
	 * @param sid 订阅者ID
	 * @param stdCode 标准合约代码
	 * @details 为指定的订阅者订阅特定合约的委托明细数据
	 */
	void sub_order_detail(uint32_t sid, const char* stdCode);
	
	/**
	 * @brief 订阅成交数据
	 * @param sid 订阅者ID
	 * @param stdCode 标准合约代码
	 * @details 为指定的订阅者订阅特定合约的逐笔成交数据
	 */
	void sub_transaction(uint32_t sid, const char* stdCode);

	/**
	 * @brief 检查是否启用Tick回测
	 * @return 是否启用Tick回测
	 * @details 检查当前回测是否已启用Tick级别的回测模式
	 */
	inline bool	is_tick_enabled() const{ return _tick_enabled; }

	/**
	 * @brief 检查是否模拟Tick数据
	 * @return 是否模拟Tick数据
	 * @details 检查当前回测是否通过K线模拟Tick数据
	 */
	inline bool	is_tick_simulated() const { return _tick_simulated; }

	/**
	 * @brief 更新合约价格
	 * @param stdCode 标准合约代码
	 * @param price 新的价格
	 * @details 更新指定合约的当前价格缓存
	 */
	inline void update_price(const char* stdCode, double price)
	{
		_price_map[stdCode] = price;
	}

	/**
	 * @brief 获取主力合约管理器
	 * @return 主力合约管理器指针
	 * @details 获取用于管理主力合约和次主力合约的管理器对象
	 */
	inline IHotMgr*	get_hot_mgr() { return &_hot_mgr; }

private:
	IDataSink*		_listener;		///< 数据接收器指针，用于接收回放数据
	IBtDataLoader*	_bt_loader;		///< 回测数据加载器指针，用于加载历史数据
	std::string		_stra_name;		///< 策略名称

	TickCache		_ticks_cache;		///< Tick数据缓存，存储逐笔行情数据
	OrdDtlCache		_orddtl_cache;	///< 委托明细缓存，存储高级别委托数据
	OrdQueCache		_ordque_cache;	///< 委托队列缓存，存储市场深度数据
	TransCache		_trans_cache;	///< 成交数据缓存，存储逐笔成交数据

	BarsCache		_bars_cache;		///< K线缓存，存储已订阅的合约K线数据
	BarsCache		_unbars_cache;	///< 未订阅的K线缓存，存储未订阅但需要的合约K线数据
	wt_hashset<std::string> _codes_in_subbed;	///< 已订阅的合约集合
	wt_hashset<std::string> _codes_in_unsubbed;	///< 未订阅的合约集合

	TaskInfoPtr		_task;			///< 当前任务信息

	std::string		_main_key;		///< 主要的数据键值
	std::string		_min_period;	///< 最小K线周期，用于未订阅品种的信号处理
	std::string		_main_period;	///< 主图周期
	bool			_tick_enabled;	///< 是否启用Tick级别回测
	bool			_tick_simulated;	///< 是否需要模拟生成Tick数据
	bool			_align_by_section;	///< 重采样分钟线是否按交易时段对齐
	
	/**
	 * @brief 无成交时不模拟Tick标志
	 * @details By Wesley @ 2023.05.05
	 * 如果K线没有成交量，则不模拟tick
	 * 默认为false，主要是针对涨跌停的行情，也适用于不活跃的合约
	 */
	bool			_nosim_if_notrade;	///< 如果K线没有成交量，则不模拟Tick
	std::map<std::string, WTSTickStruct>	_day_cache;	///< 每日Tick缓存，当分钟K线回放时使用
	std::map<std::string, std::string>		_ticker_keys;	///< 合约代码到缓存键的映射

	/**
	 * @brief 未订阅但需要的合约集合
	 * @details By Wesley @ 2022.06.01
	 * 针对不订阅而直接指定合约下单的场景，需要跟踪这些合约
	 */
	wt_hashset<std::string>		_unsubbed_in_need;	///< 未订阅但需要的K线合约集合

	/**
	 * @brief 复权标记
	 * @details By Wesley @ 2022.08.15
	 * 采用位运算表示，1|2|4
	 * 1表示成交量复权，2表示成交额复权，4表示总持复权，其他待定
	 */
	uint32_t		_adjust_flag; 	///< 复权标记，控制不同字段的复权方式

	uint32_t		_cur_date;		///< 当前日期，格式为YYYYMMDD
	uint32_t		_cur_time;		///< 当前时间，格式为HHMMSS
	uint32_t		_cur_secs;		///< 当前秒数
	uint32_t		_cur_tdate;		///< 当前交易日，格式为YYYYMMDD
	uint32_t		_closed_tdate;	///< 上一个已结束的交易日
	uint32_t		_opened_tdate;	///< 下一个将开始的交易日

	WTSBaseDataMgr	_bd_mgr;		///< 基础数据管理器，管理合约、交易时段等信息
	WTSHotMgr		_hot_mgr;		///< 主力合约管理器，管理期货主力合约切换

	std::string		_base_dir;		///< 基础目录路径
	std::string		_mode;			///< 回测模式
	uint64_t		_begin_time;	///< 回测开始时间，格式为YYYYMMDDHHMMSS
	uint64_t		_end_time;		///< 回测结束时间，格式为YYYYMMDDHHMMSS

	uint32_t		_cache_clear_days;	///< 缓存自动清理天数，控制历史数据缓存时长

	bool			_running;		///< 是否正在运行回测
	bool			_terminated;	///< 是否已终止回测
	//////////////////////////////////////////////////////////////////////////
	/**
	 * @brief 手续费模板结构
	 * @details 定义了交易品种的手续费设置，包括开仓、平仓、平今等费率
	 */
	typedef struct _FeeItem
	{
		double	_open;			///< 开仓手续费率
		double	_close;		///< 平仓手续费率
		double	_close_today;	///< 平今手续费率
		bool	_by_volume;		///< 是否按照成交量计算，否则按金额计算

		/**
		 * @brief 构造函数
		 * @details 初始化所有费率为0
		 */
		_FeeItem()
		{
			memset(this, 0, sizeof(_FeeItem));
		}
	} FeeItem;
	/**
	 * @brief 手续费映射表类型
	 * @details 映射品种代码到手续费设置
	 */
	typedef wt_hashmap<std::string, FeeItem>	FeeMap;
	FeeMap		_fee_map;		///< 品种手续费设置映射表

	//////////////////////////////////////////////////////////////////////////
	//
	/**
	 * @brief 价格映射表类型
	 * @details 映射合约代码到当前价格
	 */
	typedef wt_hashmap<std::string, double> PriceMap;
	PriceMap		_price_map;		///< 合约当前价格映射表

	//////////////////////////////////////////////////////////////////////////
	//
	//By Wesley @ 2022.02.07
	//tick数据订阅项，first是contextid，second是订阅选项，0-原始订阅，1-前复权，2-后复权
	typedef std::pair<uint32_t, uint32_t> SubOpt;
	typedef wt_hashmap<uint32_t, SubOpt> SubList;
	typedef wt_hashmap<std::string, SubList>	StraSubMap;
	StraSubMap		_tick_sub_map;		//tick数据订阅表
	StraSubMap		_ordque_sub_map;	//orderqueue数据订阅表
	StraSubMap		_orddtl_sub_map;	//orderdetail数据订阅表
	StraSubMap		_trans_sub_map;		//transaction数据订阅表

	//除权因子
	/**
	 * @brief 除权因子结构
	 * @details 存储股票除权信息，包含日期和因子值
	 */
	typedef struct _AdjFactor
	{
		uint32_t	_date;		///< 除权日期，格式为YYYYMMDD
		double		_factor;	///< 除权因子值
	} AdjFactor;
	/**
	 * @brief 除权因子列表类型
	 * @details 存储一只股票的所有除权因子
	 */
	typedef std::vector<AdjFactor> AdjFactorList;
	
	/**
	 * @brief 除权因子映射表类型
	 * @details 映射股票代码到其除权因子列表
	 */
	typedef wt_hashmap<std::string, AdjFactorList>	AdjFactorMap;
	AdjFactorMap	_adj_factors;	///< 除权因子映射表

	/**
	 * @brief 获取除权因子列表
	 * @param code 股票代码
	 * @param exchg 交易所代码
	 * @param pid 品种代码
	 * @return 除权因子列表的引用
	 * @details 根据股票信息获取其对应的除权因子列表
	 */
	const AdjFactorList& getAdjFactors(const char* code, const char* exchg, const char* pid);

	EventNotifier*	_notifier;		///< 事件通知器，用于处理回测过程中的事件

	HisDataMgr		_his_dt_mgr;	///< 历史数据管理器，管理历史行情数据
};

