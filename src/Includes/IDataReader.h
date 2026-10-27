/*!
 * \file IDataReader.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据读取接口定义
 * \details 本文件定义了WonderTrader框架中的数据读取接口，包括历史数据加载器和数据读取器的接口定义
 */
#pragma once
#include <stdint.h>

#include "../Includes/WTSMarcos.h"
#include "../Includes/WTSTypes.h"

NS_WTP_BEGIN
class WTSKlineData;
class WTSKlineSlice;
class WTSTickSlice;
class WTSOrdQueSlice;
class WTSOrdDtlSlice;
class WTSTransSlice;
struct WTSBarStruct;
class WTSVariant;
class IBaseDataMgr;
class IHotMgr;

/**
 * @brief 数据读取模块回调接口
 * @details 主要用于数据读取模块向调用模块回调事件和获取系统状态
 */
class IDataReaderSink
{
public:
    /**
     * @brief K线闭合事件回调
     * @details 当一根K线完成闭合时调用此回调函数，传递新生成的K线数据
     * 
     * @param stdCode 标准品种代码，如SSE.600000（上证指数）或SHFE.au.2005（上期黄2005合约）
     * @param period K线周期，如分钟线、小时线、日线等
     * @param newBar 闭合的K线结构指针，包含开高低收等信息
     */
    virtual void on_bar(const char* stdCode, WTSKlinePeriod period, WTSBarStruct* newBar) = 0;

    /**
     * @brief 所有缓存的K线全部更新的事件回调
     * @details 当所有订阅的品种的K线数据都已经更新完成时触发此回调
     *
     * @param updateTime K线更新时间，精确到分钟，格式为YYYYMMDDHHMM，如202004101500（表示2020年4月10日15:00）
     */
    virtual void on_all_bar_updated(uint32_t updateTime) = 0;

    /**
     * @brief 获取基础数据管理接口指针
     * @details 获取IBaseDataMgr接口指针，用于访问品种、合约、交易所等基础静态数据
     * @return 基础数据管理器指针
     */
    virtual IBaseDataMgr* get_basedata_mgr() = 0;

    /**
     * @brief 获取主力合约管理接口指针
     * @details 获取期货主力合约管理器接口指针，用于管理期货主力合约切换规则
     * @return 主力合约管理器指针
     */
    virtual IHotMgr* get_hot_mgr() = 0;

    /**
     * @brief 获取当前日期
     * @details 获取当前系统日期，不是交易日
     * @return 当前日期，格式为YYYYMMDD，如20100410（表示2010年4月10日）
     */
    virtual uint32_t get_date() = 0;

    /**
     * @brief 获取当前1分钟线的时间
     * @details 获取当前分钟线时间，这里的时间是处理过的，如当前真实时间是9:00:32秒，
     *          真实时间为0900，但是对应的分钟线时间为0901，因为9:00:32秒属于9:01这根分钟线
     * @return 当前分钟线时间，格式为HHMM，如0901（表示9点01分）
     */
    virtual uint32_t get_min_time() = 0;

    /**
     * @brief 获取当前的秒数
     * @details 获取当前时间的秒数部分，包含毫秒
     * @return 当前秒数，格式为SS.mmm，精确到毫秒，如37.500表示37秒500毫秒
     */
    virtual uint32_t get_secs() = 0;

    /**
     * @brief 输出数据读取模块的日志
     * @details 用于数据读取模块向系统输出日志信息
     * @param ll 日志级别，如信息、警告、错误等
     * @param message 日志内容
     */
    virtual void reader_log(WTSLogLevel ll, const char* message) = 0;
};

/**
 * @brief 历史数据加载器的回调函数
 * @details 用于历史数据加载器向调用模块回调历史K线数据
 * 
 * @param obj 回传用的上下文指针，原样返回即可
 * @param bars K线数据数组
 * @param count K线条数
 */
typedef void(*FuncReadBars)(void* obj, WTSBarStruct* bars, uint32_t count);

/**
 * @brief 加载复权因子回调
 * @details 用于加载股票除权除息因子的回调函数
 * 
 * @param obj 回传用的上下文指针，原样返回即可
 * @param stdCode 标准化合约代码
 * @param dates 除权除息日期数组，格式为YYYYMMDD
 * @param factors 对应的复权因子数组
 * @param count 复权因子的总数
 */
typedef void(*FuncReadFactors)(void* obj, const char* stdCode, uint32_t* dates, double* factors, uint32_t count);

/**
 * @brief 历史数据加载器
 * @details 定义了加载历史K线和复权因子的接口，用于从外部数据源加载历史数据
 */
class IHisDataLoader
{
public:
	/**
	 * @brief 加载最终历史K线数据
	 * @details 加载经过处理的最终历史K线数据，和loadRawHisBars的区别在于，
	 *          loadFinalHisBars加载的是经过框架处理好的最终数据（如已经过复权或合成了主力合约），
	 *          系统不再对这些数据进行处理。
	 * 
	 * @param obj 回传用的上下文指针，原样返回即可
	 * @param stdCode 标准化合约代码
	 * @param period K线周期
	 * @param cb 数据加载完成后的回调函数
	 * @return 是否成功加载数据
	 */
	virtual bool loadFinalHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) = 0;

	/**
	 * @brief 加载原始历史K线数据
	 * @details 加载未经处理的原始历史K线数据，这些数据可能需要经过框架的进一步处理，
	 *          如复权、主力合约合成等
	 * 
	 * @param obj 回传用的上下文指针，原样返回即可
	 * @param stdCode 标准化合约代码
	 * @param period K线周期
	 * @param cb 数据加载完成后的回调函数
	 * @return 是否成功加载数据
	 */
	virtual bool loadRawHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) = 0;

	/**
	 * @brief 加载全部除权因子
	 * @details 加载所有股票的除权除息因子，用于进行股票数据的复权处理
	 * 
	 * @param obj 回传用的上下文指针
	 * @param cb 数据加载完成后的回调函数
	 * @return 是否成功加载数据
	 */
	virtual bool loadAllAdjFactors(void* obj, FuncReadFactors cb) = 0;

	/**
	 * @brief 加载指定合约的除权因子
	 * @details 加载指定股票的除权除息因子，用于进行股票数据的复权处理
	 * 
	 * @param obj 回传用的上下文指针
	 * @param stdCode 标准化合约代码，通常是股票代码
	 * @param cb 数据加载完成后的回调函数
	 * @return 是否成功加载数据
	 */
	virtual bool loadAdjFactors(void* obj, const char* stdCode, FuncReadFactors cb) = 0;
};

/**
 * @brief 数据读取接口
 * @details 向核心模块提供行情数据（tick、K线、委托队列、逐笔委托、逐笔成交等）的读取接口
 *          提供统一的数据访问方式，封装不同数据源的差异
 */
class IDataReader
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化数据读取器，将回调接口设置为空
	 */
	IDataReader() :_sink(NULL), _loader(NULL) {}

	/**
	 * @brief 析构函数
	 * @details 虚函数，由子类实现
	 */
	virtual ~IDataReader(){}

public:
	/**
	 * @brief 初始化数据读取模块
	 * @details 根据配置项初始化数据读取模块，设置回调接口和历史数据加载器
	 *
	 * @param cfg 模块配置项，包含各种配置参数
	 * @param sink 模块回调接口，用于回调事件和获取系统状态
	 * @param loader 历史数据加载器，用于加载历史数据，默认为NULL
	 */
	virtual void init(WTSVariant* cfg, IDataReaderSink* sink, IHisDataLoader* loader = NULL) { _sink = sink; _loader = loader; }

	/**
	 * @brief 分钟线闭合事件处理接口
	 * @details 当一根分钟线数据闭合时，系统调用此方法进行处理，可以触发K线数据的更新
	 * 
	 * @param uDate 闭合的分钟线日期，格式为YYYYMMDD，如20200410，这里不一定是交易日
	 * @param uTime 闭合的分钟线的分钟时间，格式为HHMM，如1115（表示11点15分）
	 * @param endTDate 如果闭合的分钟线是交易日最后一条分钟线，则endTDate为当前交易日，其他情况为0
	 */
	virtual void onMinuteEnd(uint32_t uDate, uint32_t uTime, uint32_t endTDate = 0) = 0;

	/**
	 * @brief 读取tick数据切片
	 * @details 从缓存中读取指定数量的tick数据，切片不会复制数据，只把缓存中的数据指针传递出来
	 *
	 * @param stdCode 标准化品种代码，如SSE.600000（上证指数）或SHFE.au.2005（上期黄2005合约）
	 * @param count 要读取的tick条数
	 * @param etime 结束时间，精确到毫秒，格式为YYYYMMDDHHMMSSmmm，如果要读取到最后一条，etime设为0，默认为0
	 * @return tick数据片段对象指针
	 */
	virtual WTSTickSlice*	readTickSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) = 0;

	/**
	 * @brief 读取逐笔委托数据切片
	 * @details 从缓存中读取指定数量的逐笔委托数据，切片不会复制数据，只把缓存中的数据指针传递出来
	 *
	 * @param stdCode 标准化品种代码，如SSE.600000（上证指数）或SHFE.au.2005（上期黄2005合约）
	 * @param count 要读取的逐笔委托条数
	 * @param etime 结束时间，精确到毫秒，格式为YYYYMMDDHHMMSSmmm，如果要读取到最后一条，etime设为0，默认为0
	 * @return 逐笔委托数据片段对象指针，如果不支持返回NULL
	 */
	virtual WTSOrdDtlSlice*	readOrdDtlSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) { return NULL; }
	/**
	 * @brief 读取委托队列数据切片
	 * @details 从缓存中读取指定数量的委托队列数据，切片不会复制数据，只把缓存中的数据指针传递出来
	 *
	 * @param stdCode 标准化品种代码，如SSE.600000（上证指数）或SHFE.au.2005（上期黄2005合约）
	 * @param count 要读取的委托队列条数
	 * @param etime 结束时间，精确到毫秒，格式为YYYYMMDDHHMMSSmmm，如果要读取到最后一条，etime设为0，默认为0
	 * @return 委托队列数据片段对象指针，如果不支持返回NULL
	 */
	virtual WTSOrdQueSlice*	readOrdQueSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) { return NULL; }

	/**
	 * @brief 读取逐笔成交数据切片
	 * @details 从缓存中读取指定数量的逐笔成交数据，切片不会复制数据，只把缓存中的数据指针传递出来
	 *
	 * @param stdCode 标准化品种代码，如SSE.600000（上证指数）或SHFE.au.2005（上期黄2005合约）
	 * @param count 要读取的逐笔成交条数
	 * @param etime 结束时间，精确到毫秒，格式为YYYYMMDDHHMMSSmmm，如果要读取到最后一条，etime设为0，默认为0
	 * @return 逐笔成交数据片段对象指针，如果不支持返回NULL
	 */
	virtual WTSTransSlice*	readTransSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) { return NULL; }

	/**
	 * @brief 读取K线序列，并返回一个存储容器类
	 * @details 从缓存中读取指定数量的K线数据，切片不会复制数据，只把缓存中的数据指针传递出来
	 *
	 * @param stdCode 标准化品种代码，如SSE.600000（上证指数）或SHFE.au.2005（上期黄2005合约）
	 * @param period K线周期，如分钟线、小时线、日线等
	 * @param count 要读取的K线条数
	 * @param etime 结束时间，格式为YYYYMMDDHHMM，如果要读取到最后一条，etime设为0，默认为0
	 * @return K线数据片段对象指针
	 */
	virtual WTSKlineSlice*	readKlineSlice(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime = 0) = 0;

	/**
	 * @brief 获取个股指定日期的复权因子
	 * @details 获取指定股票在特定日期的复权因子，用于计算复权数据
	 *
	 * @param stdCode 标准化品种代码，如SSE.600000，通常是股票代码
	 * @param date 指定日期，格式为YYYYMMDD，默认为0，为0则按当前日期处理
	 * @return 复权因子值，无除权除息情况下为1.0
	 */
	virtual double		getAdjFactorByDate(const char* stdCode, uint32_t date = 0) { return 1.0; }

	/**
	 * @brief 获取复权标记
	 * @details 获取当前数据读取器的复权标记，标记采用位运算形式来表示不同复权项
	 * 
	 * @return 复权标记，采用位运算1|2|4的形式，1表示成交量复权，2表示成交额复权，4表示总持复权
	 */
	virtual uint32_t	getAdjustingFlag() { return 0; }

protected:
	IDataReaderSink*	_sink;    ///< 数据读取器回调接口，用于回调事件和获取系统状态
	IHisDataLoader*		_loader;  ///< 历史数据加载器，用于加载历史数据
};

/**
 * @brief 创建数据读取器对象的函数指针类型
 * @details 用于动态加载数据读取器模块时创建对象的函数指针
 */
typedef IDataReader* (*FuncCreateDataReader)();

/**
 * @brief 删除数据读取器对象的函数指针类型
 * @details 用于动态加载数据读取器模块时删除对象的函数指针
 */
typedef void(*FuncDeleteDataReader)(IDataReader* store);

NS_WTP_END