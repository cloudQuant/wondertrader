/*!
 * \file WtDtServo.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据服务对外接口实现
 * 
 * \details 本文件实现了WtDtServo模块对外提供的C接口函数，
 * 包括模块初始化、数据查询、数据订阅等功能。
 * 这些接口封装了WtDtRunner类的功能，使其可以被其他语言调用。
 */
#include "WtDtServo.h"
#include "WtDtRunner.h"

#include "../WtDtCore/WtHelper.h"
#include "../WTSTools/WTSLogger.h"

#include "../Share/ModuleHelper.hpp"
#include "../Includes/WTSVersion.h"
#include "../Includes/WTSDataDef.hpp"

#include <boost/filesystem.hpp>

#ifdef _MSC_VER
#ifdef _WIN64
char PLATFORM_NAME[] = "X64";
#else
char PLATFORM_NAME[] = "X86";
#endif
#else
char PLATFORM_NAME[] = "UNIX";
#endif

#ifdef _MSC_VER
#include "../Common/mdump.h"
/**
 * @brief 获取模块名称
 * @return const char* 模块名称字符串
 * 
 * @details 该函数用于获取当前模块的文件名称，仅在Windows平台下可用。
 * 使用静态字符数组缓存模块名称，避免重复获取。
 */
const char* getModuleName()
{
	static char MODULE_NAME[250] = { 0 };
	if (strlen(MODULE_NAME) == 0)
	{
		GetModuleFileName(g_dllModule, MODULE_NAME, 250);
		boost::filesystem::path p(MODULE_NAME);
		strcpy(MODULE_NAME, p.filename().string().c_str());
	}

	return MODULE_NAME;
}
#endif

/**
 * @brief 获取数据服务运行器单例
 * @return WtDtRunner& 数据服务运行器引用
 * 
 * @details 该函数使用单例模式创建并返回数据服务运行器实例。
 * 所有对外接口函数都通过这个单例来调用WtDtRunner类的相应方法。
 */
WtDtRunner& getRunner()
{
	static WtDtRunner runner;
	return runner;
}

/**
 * @brief 初始化数据服务模块
 * @param cfgFile 配置文件路径或配置内容字符串
 * @param isFile 是否为文件路径，如果为true则cfgFile为文件路径，否则为配置内容字符串
 * @param logCfg 日志配置文件路径
 * @param cbTick Tick数据回调函数，用于接收实时Tick数据
 * @param cbBar K线数据回调函数，用于接收实时K线数据
 * 
 * @details 该函数调用WtDtRunner的initialize方法初始化数据服务模块，
 * 加载配置文件，初始化日志系统，并设置实时数据的回调函数。
 * 这个函数必须在使用其他接口前调用。
 */
void initialize(WtString cfgFile, bool isFile, WtString logCfg, FuncOnTickCallback cbTick, FuncOnBarCallback cbBar)
{
	getRunner().initialize(cfgFile, isFile, getBinDir(), logCfg, cbTick, cbBar);
}

/**
 * @brief 获取数据服务模块版本号
 * @return const char* 版本号字符串
 * 
 * @details 该函数返回数据服务模块的版本号字符串，包含平台名称、版本号和编译时间。
 * 使用静态字符串缓存版本信息，避免重复生成。
 * 返回的字符串格式例如："X64 1.0.0 Build@Mar 30 2020 10:00:00"
 */
const char* get_version()
{
	static std::string _ver;
	if (_ver.empty())
	{
		_ver = PLATFORM_NAME;
		_ver += " ";
		_ver += WT_VERSION;
		_ver += " Build@";
		_ver += __DATE__;
		_ver += " ";
		_ver += __TIME__;
	}
	return _ver.c_str();
}

/**
 * @brief 根据时间范围获取K线数据
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param period K线周期，如"m1"/"d1"
 * @param beginTime 开始时间，格式为YYYYMMDDHHmmss
 * @param endTime 结束时间，格式为YYYYMMDDHHmmss
 * @param cb K线数据获取回调函数，用于接收查询结果
 * @param cbCnt 数据计数回调函数，用于接收数据总数
 * @return WtUInt32 返回查询到的数据数量
 * 
 * @details 该函数调用WtDtRunner的get_bars_by_range方法获取指定时间范围的K线数据。
 * 获取到数据后，首先通过cbCnt回调函数返回数据总数，
 * 然后分块通过cb回调函数返回数据内容。
 * 最后释放数据切片并返回数据数量。
 */
WtUInt32 get_bars_by_range(const char* stdCode, const char* period, WtUInt64 beginTime, WtUInt64 endTime, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt)
{
	WTSKlineSlice* kData = getRunner().get_bars_by_range(stdCode, period, beginTime, endTime);
	if (kData)
	{
		uint32_t reaCnt = kData->size();
		cbCnt(kData->size());

		for (std::size_t i = 0; i < kData->get_block_counts(); i++)
			cb(kData->get_block_addr(i), kData->get_block_size(i), i == kData->get_block_counts() - 1);

		kData->release();
		return reaCnt;
	}
	else
	{
		return 0;
	}
}

/**
 * @brief 根据日期获取K线数据
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param period K线周期，如"m1"/"d1"
 * @param uDate 交易日期，格式为YYYYMMDD
 * @param cb K线数据获取回调函数，用于接收查询结果
 * @param cbCnt 数据计数回调函数，用于接收数据总数
 * @return WtUInt32 返回查询到的数据数量
 * 
 * @details 该函数调用WtDtRunner的get_bars_by_date方法获取指定日期的K线数据。
 * 获取到数据后，首先通过cbCnt回调函数返回数据总数，
 * 然后分块通过cb回调函数返回数据内容。
 * 最后释放数据切片并返回数据数量。
 */
WtUInt32 get_bars_by_date(const char* stdCode, const char* period, WtUInt32 uDate, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt)
{
	WTSKlineSlice* kData = getRunner().get_bars_by_date(stdCode, period, uDate);
	if (kData)
	{
		uint32_t reaCnt = kData->size();
		cbCnt(kData->size());

		for (std::size_t i = 0; i < kData->get_block_counts(); i++)
			cb(kData->get_block_addr(i), kData->get_block_size(i), i == kData->get_block_counts() - 1);

		kData->release();
		return reaCnt;
	}
	else
	{
		return 0;
	}
}

/**
 * @brief 根据时间范围获取Tick数据
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param beginTime 开始时间，格式为YYYYMMDDHHmmss
 * @param endTime 结束时间，格式为YYYYMMDDHHmmss
 * @param cb Tick数据获取回调函数，用于接收查询结果
 * @param cbCnt 数据计数回调函数，用于接收数据总数
 * @return WtUInt32 返回查询到的数据数量
 * 
 * @details 该函数调用WtDtRunner的get_ticks_by_range方法获取指定时间范围的Tick数据。
 * 获取到数据后，首先通过cbCnt回调函数返回数据总数，
 * 然后分块通过cb回调函数返回数据内容，并累计返回的数据数量。
 * 最后释放数据切片并返回数据数量。
 */
WtUInt32	get_ticks_by_range(const char* stdCode, WtUInt64 beginTime, WtUInt64 endTime, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt)
{
	WTSTickSlice* slice = getRunner().get_ticks_by_range(stdCode, beginTime, endTime);
	if (slice)
	{
		uint32_t reaCnt = 0;
		uint32_t blkCnt = slice->get_block_counts();
		cbCnt(slice->size());

		for(uint32_t sIdx = 0; sIdx < blkCnt; sIdx++)
		{
			cb(slice->get_block_addr(sIdx), slice->get_block_size(sIdx), sIdx == blkCnt - 1);
			reaCnt += slice->get_block_size(sIdx);
		}
		
		slice->release();
		return reaCnt;
	}
	else
	{
		return 0;
	}
}

/**
 * @brief 根据数量和结束时间获取K线数据
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param period K线周期，如"m1"/"d1"
 * @param count 要获取的K线数量
 * @param endTime 结束时间，格式为YYYYMMDDHHmmss，如果为0则表示当前时间
 * @param cb K线数据获取回调函数，用于接收查询结果
 * @param cbCnt 数据计数回调函数，用于接收数据总数
 * @return WtUInt32 返回查询到的数据数量
 * 
 * @details 该函数调用WtDtRunner的get_bars_by_count方法获取指定数量的K线数据。
 * 从结束时间往前查询指定数量的K线数据。
 * 获取到数据后，首先通过cbCnt回调函数返回数据总数，
 * 然后分块通过cb回调函数返回数据内容。
 * 最后释放数据切片并返回数据数量。
 */
WtUInt32 get_bars_by_count(const char* stdCode, const char* period, WtUInt32 count, WtUInt64 endTime, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt)
{
	WTSKlineSlice* kData = getRunner().get_bars_by_count(stdCode, period, count, endTime);
	if (kData)
	{
		uint32_t reaCnt = kData->size();
		cbCnt(kData->size());

		for(std::size_t i = 0; i< kData->get_block_counts(); i++)
			cb(kData->get_block_addr(i), kData->get_block_size(i), i == kData->get_block_counts()-1);

		kData->release();
		return reaCnt;
	}
	else
	{
		return 0;
	}
}

/**
 * @brief 根据数量和结束时间获取Tick数据
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param count 要获取的Tick数量
 * @param endTime 结束时间，格式为YYYYMMDDHHmmss，如果为0则表示当前时间
 * @param cb Tick数据获取回调函数，用于接收查询结果
 * @param cbCnt 数据计数回调函数，用于接收数据总数
 * @return WtUInt32 返回查询到的数据数量
 * 
 * @details 该函数调用WtDtRunner的get_ticks_by_count方法获取指定数量的Tick数据。
 * 从结束时间往前查询指定数量的Tick数据。
 * 获取到数据后，首先通过cbCnt回调函数返回数据总数，
 * 然后分块通过cb回调函数返回数据内容，并累计返回的数据数量。
 * 最后释放数据切片并返回数据数量。
 */
WtUInt32	get_ticks_by_count(const char* stdCode, WtUInt32 count, WtUInt64 endTime, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt)
{
	WTSTickSlice* slice = getRunner().get_ticks_by_count(stdCode, count, endTime);
	if (slice)
	{
		uint32_t reaCnt = 0;
		uint32_t blkCnt = slice->get_block_counts();
		cbCnt(slice->size());

		for (uint32_t sIdx = 0; sIdx < blkCnt; sIdx++)
		{
			cb(slice->get_block_addr(sIdx), slice->get_block_size(sIdx), sIdx == blkCnt - 1);
			reaCnt += slice->get_block_size(sIdx);
		}

		slice->release();
		return reaCnt;
	}
	else
	{
		return 0;
	}
}

/**
 * @brief 根据日期获取Tick数据
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param uDate 交易日期，格式为YYYYMMDD
 * @param cb Tick数据获取回调函数，用于接收查询结果
 * @param cbCnt 数据计数回调函数，用于接收数据总数
 * @return WtUInt32 返回查询到的数据数量
 * 
 * @details 该函数调用WtDtRunner的get_ticks_by_date方法获取指定日期的Tick数据。
 * 获取到数据后，首先通过cbCnt回调函数返回数据总数，
 * 然后分块通过cb回调函数返回数据内容，并累计返回的数据数量。
 * 最后释放数据切片并返回数据数量。
 */
WtUInt32 get_ticks_by_date(const char* stdCode, WtUInt32 uDate, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt)
{
	WTSTickSlice* slice = getRunner().get_ticks_by_date(stdCode, uDate);
	if (slice)
	{
		uint32_t reaCnt = 0;
		uint32_t blkCnt = slice->get_block_counts();
		cbCnt(slice->size());

		for (uint32_t sIdx = 0; sIdx < blkCnt; sIdx++)
		{
			cb(slice->get_block_addr(sIdx), slice->get_block_size(sIdx), sIdx == blkCnt - 1);
			reaCnt += slice->get_block_size(sIdx);
		}

		slice->release();
		return reaCnt;
	}
	else
	{
		return 0;
	}
}

/**
 * @brief 根据日期获取秒线K线数据
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param secs 秒线周期，如常见的30秒成60秒
 * @param uDate 交易日期，格式为YYYYMMDD
 * @param cb K线数据获取回调函数，用于接收查询结果
 * @param cbCnt 数据计数回调函数，用于接收数据总数
 * @return WtUInt32 返回查询到的数据数量
 * 
 * @details 该函数调用WtDtRunner的get_sbars_by_date方法获取指定日期的秒线K线数据。
 * 秒线K线是指周期小于1分钟的K线，如30秒成60秒K线。
 * 获取到数据后，首先通过cbCnt回调函数返回数据总数，
 * 然后分块通过cb回调函数返回数据内容。
 * 最后释放数据切片并返回数据数量。
 */
WtUInt32 get_sbars_by_date(const char* stdCode, WtUInt32 secs, WtUInt32 uDate, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt)
{
	WTSKlineSlice* kData = getRunner().get_sbars_by_date(stdCode, secs, uDate);
	if (kData)
	{
		uint32_t reaCnt = kData->size();
		cbCnt(kData->size());

		for (std::size_t i = 0; i < kData->get_block_counts(); i++)
			cb(kData->get_block_addr(i), kData->get_block_size(i), i == kData->get_block_counts() - 1);

		kData->release();
		return reaCnt;
	}
	else
	{
		return 0;
	}
}

/**
 * @brief 订阅实时Tick数据
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param bReplace 是否替换已有订阅，如果为true则清空已有订阅后再订阅新的合约
 * 
 * @details 该函数调用WtDtRunner的sub_tick方法订阅实时Tick数据。
 * 订阅成功后，当有新的Tick数据到达时，会通过初始化时设置的回调函数通知调用者。
 * 如果bReplace为true，则会清空已有的所有订阅，只保留新订阅的合约。
 */
void subscribe_tick(const char* stdCode, bool bReplace)
{
	getRunner().sub_tick(stdCode, bReplace);
}

/**
 * @brief 订阅实时K线数据
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param period K线周期，如"m1"/"d1"
 * 
 * @details 该函数调用WtDtRunner的sub_bar方法订阅实时K线数据。
 * 订阅成功后，当有新的K线数据到达时，会通过初始化时设置的回调函数通知调用者。
 * 不同于Tick订阅，该方法不会清空已有订阅，而是累加订阅。
 */
void subscribe_bar(const char* stdCode, const char* period)
{
	getRunner().sub_bar(stdCode, period);
}

/**
 * @brief 清理数据缓存
 * 
 * @details 该函数调用WtDtRunner的clear_cache方法清理内存中的数据缓存。
 * 在需要释放内存或重新加载数据时调用该函数。
 * 清理缓存后，再次查询数据时会重新从存储中加载。
 */
void clear_cache()
{
	getRunner().clear_cache();
}