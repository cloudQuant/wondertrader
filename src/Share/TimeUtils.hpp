/*!
 * \file TimeUtils.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 时间处理的封装
 * 
 * \details 本文件封装了各种时间处理相关的工具函数和类，包括：
 * - 获取当前时间（精确到毫秒级）
 * - 时间格式转换（日期、时间的各种格式互转）
 * - 日期计算（获取下一天、下一分钟、下一月等）
 * - 时间字符串格式化
 * - 高精度计时器
 */
#pragma once

#include <cstddef>  // 对于 C++ 语言, NULL
#include <stddef.h> // 对于 C 语言, NULL

#include <stdint.h>  // 定义了固定宽度的整数类型（如 int64_t）
#include <sys/timeb.h> // 定义了 ftime 函数，用于获取当前时间（精确到毫秒）。
#ifdef _MSC_VER
#include <time.h>  // 定义了时间相关的函数和结构体（如 localtime、mktime 等）
#else
#include <sys/time.h>  // 定义了时间相关的函数和结构体（如 localtime、mktime 等）
#endif
#include <string> // 提供了 std::string 类型。
#include <string.h> // 提供了字符串操作函数（如 memset、strftime 等）
#include <chrono>  // 提供了高精度时间库（如 std::chrono）。
#include <thread>  // 提供了多线程支持（如 std::thread）。
#include <cmath>  // 提供了数学函数（如 std::pow）。

#ifdef _MSC_VER   // 定义 Windows 平台相关的时间结构体和宏。
#define CTIME_BUF_SIZE 64

#define WIN32_LEAN_AND_MEAN

#include <windows.h>

typedef struct _KSYSTEM_TIME
{
	ULONG LowPart;
	LONG High1Time;
	LONG High2Time;
} KSYSTEM_TIME, *PKSYSTEM_TIME;

struct KUSER_SHARED_DATA
{
	ULONG TickCountLowDeprecated;
	ULONG TickCountMultiplier;
	volatile KSYSTEM_TIME InterruptTime;
	volatile KSYSTEM_TIME SystemTime;
	volatile KSYSTEM_TIME TimeZoneBias;
};

#define KI_USER_SHARED_DATA   0x7FFE0000
#define SharedUserData   ((KUSER_SHARED_DATA * const)KI_USER_SHARED_DATA)

#define TICKSPERSEC        10000000L
#endif

/**
 * @brief 时间处理工具类
 * 
 * @details TimeUtils类提供了一系列与时间相关的静态方法，包括时间获取、格式转换、日期计算等功能。
 * 包含两个内部类：Time32（时间处理类）和Ticker（高精度计时器）。
 * 所有方法都是静态的，可以直接通过类名调用，无需创建类的实例。
 */
/**
 * @brief 时间工具类，提供各种时间相关的工具函数和类
 * 
 * @details TimeUtils类是一个静态工具类，封装了各种时间相关的实用函数和子类。
 * 它提供了以下功能：
 * - 获取当前时间：精确到毫秒级别的当前时间获取
 * - 时间格式转换：各种时间格式之间的相互转换，如日期、时间的整数表示和字符串表示
 * - 日期计算：计算下一天、下一分钟、下一月等
 * - 分钟线ID转换：将日期和时间转换为唯一的分钟线ID，以及反向转换
 * - 时间处理工具类：Time32类、Ticker高精度计时器等
 * 
 * 该类设计为静态类，所有方法都是静态的，不需要创建类的实例即可使用。
 * 其中许多方法都是inline的，这样可以提高运行效率，特别是在高频交易系统中。
 * 该类考虑了跨平台兼容性，在Windows和Unix/Linux系统上都能正常工作。
 */
class TimeUtils 
{
	
public:
//	static inline int64_t getLocalTimeNowOld(void)
//	{
//		thread_local static timeb now;
//		ftime(&now);
//		return now.time * 1000 + now.millitm;
//	}
	/**
	 * @brief 获取当前本地时间（旧版方法）
	 * 
	 * @return int64_t 当前时间的毫秒级时间戳，从1970年1月1日起算的毫秒数
	 * 
	 * @details 该方法使用clock_gettime函数获取当前系统时间，并将其转换为毫秒级时间戳。
	 * 使用thread_local关键字确保每个线程都有自己的timespec实例，避免多线程竞争。
	 * 这是旧版的获取时间方法，保留为了兼容性目的。
	 */
	static inline int64_t getLocalTimeNowOld(void)
	{
		thread_local static struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
	}

	/**
	 * @brief 获取当前本地时间，精确到毫秒
	 * 
	 * @return int64_t 当前时间的毫秒级时间戳，从1970年1月1日起算的毫秒数，例如 1734350031606
	 * 
	 * @details 该方法根据不同的操作系统提供了不同的实现：
	 * - Windows平台：使用SharedUserData结构直接访问系统时间，避免了系统调用开销，性能更高
	 * - Unix/Linux平台：使用clock_gettime函数获取系统时间，相比ftime提升了约10%的性能
	 * 
	 * 这是获取当前时间的首选方法，比getLocalTimeNowOld更高效。
	 */
	static inline int64_t getLocalTimeNow(void)
	{
#ifdef _MSC_VER
		LARGE_INTEGER SystemTime;
		do
		{
			SystemTime.HighPart = SharedUserData->SystemTime.High1Time;
			SystemTime.LowPart = SharedUserData->SystemTime.LowPart;
		} while (SystemTime.HighPart != SharedUserData->SystemTime.High2Time);

		uint64_t t = SystemTime.QuadPart;
		t = t - 11644473600L * TICKSPERSEC;
		return t / 10000;
#else
		//timeb now;
		//ftime(&now);
		//return now.time * 1000 + now.millitm;
		/*
		 *	clock_gettime比ftime会提升约10%的性能
		 */
		thread_local static struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		return now.tv_sec * 1000 + now.tv_nsec / 1000000;
#endif
	}

     /**
      * @brief 获取当前本地时间的字符串表示
      * 
      * @param bIncludeMilliSec 是否包含毫秒信息，默认为true
      * @return std::string 当前时间的字符串表示，格式为 "HH:MM:SS" 或 "HH:MM:SS,mmm"
      * 
      * @details 该方法返回当前时间的字符串表示，可以选择是否包含毫秒信息：
      * - 包含毫秒时格式例如："19:53:51,606"
      * - 不包含毫秒时格式例如："19:53:51"
      * 
      * 该方法内部调用getLocalTimeNow获取当前时间的毫秒级时间戳，
      * 然后将其转换为所需的字符串格式。
      */
	static inline std::string getLocalTime(bool bIncludeMilliSec = true)
	{
		uint64_t ltime = getLocalTimeNow();
		time_t now = ltime / 1000;
		uint32_t millitm = ltime % 1000;
		tm * tNow = localtime(&now);

		char str[64] = {0};
		if(bIncludeMilliSec)
			sprintf(str, "%02d:%02d:%02d,%03d", tNow->tm_hour, tNow->tm_min, tNow->tm_sec, millitm);
		else
			sprintf(str, "%02d:%02d:%02d", tNow->tm_hour, tNow->tm_min, tNow->tm_sec);
		return str;
	}

    /**
     * @brief 获取当前日期和时间的整数表示
     * 
     * @return uint64_t 当前日期和时间的整数表示，格式为 YYYYMMDDhhmmss，例如 20241216195351
     * 
     * @details 该方法返回当前日期和时间的整数表示，将年月日时分秒组合成一个整数。
     * 计算过程如下：
     * 1. 日期部分：年*10000 + 月*100 + 日，例如 20241216
     * 2. 时间部分：时*10000 + 分*100 + 秒，例如 195351
     * 3. 组合结果：日期*1000000 + 时间，例如 20241216195351
     * 
     * 该格式常用于日志记录、数据存储等需要紧凑表示时间的场景。
     */
	static inline uint64_t getYYYYMMDDhhmmss()
	{
		uint64_t ltime = getLocalTimeNow();
		time_t now = ltime / 1000;

		tm * tNow = localtime(&now);

		uint64_t date = (tNow->tm_year + 1900) * 10000 + (tNow->tm_mon + 1) * 100 + tNow->tm_mday;

		uint64_t time = tNow->tm_hour * 10000 + tNow->tm_min * 100 + tNow->tm_sec;
		return date * 1000000 + time;
	}

    /**
     * @brief 获取当前日期和时间，分别存储到两个参数中
     * 
     * @param[out] date 当前日期，格式为 YYYYMMDD，例如 20220309
     * @param[out] time 当前时间，包含毫秒，格式为 HHMMSSmmmm，例如 103029500
     * 
     * @details 该方法获取当前的日期和时间，并将其分别存储到两个参数中。
     * 计算过程如下：
     * 1. 日期部分：年*10000 + 月*100 + 日，例如 20220309
     * 2. 时间部分：时*10000 + 分*100 + 秒，然后乘以1000再加上毫秒，例如 103029500
     * 
     * 该方法常用于需要分别处理日期和时间的场景，如交易数据存储、日志记录等。
     */
	static inline void getDateTime(uint32_t &date, uint32_t &time)
	{
		uint64_t ltime = getLocalTimeNow();
		time_t now = ltime / 1000;
		uint32_t millitm = ltime % 1000;

		tm * tNow = localtime(&now);

		date = (tNow->tm_year+1900)*10000 + (tNow->tm_mon+1)*100 + tNow->tm_mday;
		
		time = tNow->tm_hour*10000 + tNow->tm_min*100 + tNow->tm_sec;
		time *= 1000;
		time += millitm;
	}

    /**
     * @brief 获取当前日期
     * 
     * @return uint32_t 当前日期的整数表示，格式为 YYYYMMDD，例如 20241216
     * 
     * @details 该方法返回当前日期的整数表示，计算方式为：年*10000 + 月*100 + 日。
     * 该格式常用于交易系统中的日期表示、数据存储和日志记录等。
     */
	static inline uint32_t getCurDate()
	{
		uint64_t ltime = getLocalTimeNow();
		time_t now = ltime / 1000;
		tm * tNow = localtime(&now);
		uint32_t date = (tNow->tm_year+1900)*10000 + (tNow->tm_mon+1)*100 + tNow->tm_mday;

		return date;
	}
    /**
     * @brief 获取指定日期或当前日期的星期
     * 
     * @param uDate 要查询的日期，格式为 YYYYMMDD，默认为0（表示当前日期）
     * @return uint32_t 星期几，范围为1-7，其中1表示周一，7表示周日
     * 
     * @details 该方法返回指定日期或当前日期的星期几。
     * 如果参数uDate为0，则获取当前日期的星期；否则获取指定日期的星期。
     * 返回值为1-7的整数，分别对应周一到周日。
     * 该方法在交易日判断、日历处理等场景中非常有用。
     */
	static inline uint32_t getWeekDay(uint32_t uDate = 0)
	{
		time_t ts = 0;
		if(uDate == 0)
		{
			ts = getLocalTimeNow()/1000;
		}
		else
		{
			tm t;	
			memset(&t,0,sizeof(tm));
			t.tm_year = uDate/10000 - 1900;
			t.tm_mon = (uDate%10000)/100 - 1;
			t.tm_mday = uDate % 100;
			ts = mktime(&t);
		}

		tm * tNow = localtime(&ts);
	
		return tNow->tm_wday;
	}
    /**
     * @brief 获取当前时间（时分秒）
     * 
     * @return uint32_t 当前时间的整数表示，格式为 HHMMSS，例如 195351
     * 
     * @details 该方法返回当前时间的整数表示，计算方式为：时*10000 + 分*100 + 秒。
     * 该格式常用于交易系统中的时间表示、数据存储和日志记录等。
     * 与getDateTime方法不同，该方法只返回时分秒部分，不包含毫秒信息。
     */
	static inline uint32_t getCurMin()
	{
		uint64_t ltime = getLocalTimeNow();
		time_t now = ltime / 1000;
		tm * tNow = localtime(&now);
		uint32_t time = tNow->tm_hour*10000 + tNow->tm_min*100 + tNow->tm_sec;

		return time;
	}

	/**
	 * @brief 获取本地时区相对于UTC的偏移量（小时）
	 * 
	 * @return int32_t 时区偏移量，单位为小时，东时区为正数（如北京时间为+8），西时区为负数
	 * 
	 * @details 该方法返回本地时区相对于UTC（协调世界时）的偏移量，单位为小时。
	 * 为了提高性能，方法使用静态变量缓存第一次计算的结果，后续调用直接返回缓存值。
	 * 该方法在处理跨时区的时间计算中非常有用，如将本地时间转换为UTC时间或将UTC时间转换为本地时间。
	 */
	static inline int32_t getTZOffset()
	{
		static int32_t offset = 99;
		if(offset == 99)
		{
			time_t now = time(NULL);
			tm tm_ltm = *localtime(&now);
			tm tm_gtm = *gmtime(&now);

			time_t _gt = mktime(&tm_gtm);
			tm _gtm2 = *localtime(&_gt);

			offset = (uint32_t)(((now - _gt) + (_gtm2.tm_isdst ? 3600 : 0)) / 60);
			offset /= 60;
		}

		return offset;
	}

	/**
	 * @brief 生成带毫秒的时间戳
	 * 
	 * @param lDate 日期，格式为 YYYYMMDD，例如 20220309
	 * @param lTimeWithMs 带毫秒的时间，格式为 HHMMSSmmm，例如 103029500
	 * @param isToUTC 是否转成UTC时间，默认为 false
	 * @return int64_t 毫秒级时间戳，从1970年1月1日起算的毫秒数
	 * 
	 * @details 该方法根据给定的日期和时间生成毫秒级时间戳。
	 * 首先将日期和时间解析成tm结构体，然后使用mktime函数转换为时间戳。
	 * 如果isToUTC为true，则会根据本地时区将时间转换为UTC时间。
	 * 这个方法常用于将特定格式的日期时间转换为时间戳，方便进行时间计算和比较。
	 */
	static inline int64_t makeTime(long lDate, long lTimeWithMs, bool isToUTC = false)
	{
		tm t;	
		memset(&t,0,sizeof(tm));
		t.tm_year = lDate/10000 - 1900;
		t.tm_mon = (lDate%10000)/100 - 1;
		t.tm_mday = lDate % 100;
		t.tm_hour = lTimeWithMs/10000000;
		t.tm_min = (lTimeWithMs%10000000)/100000;
		t.tm_sec = (lTimeWithMs%100000)/1000;
		int millisec = lTimeWithMs%1000;
		//t.tm_isdst 	
		time_t ts = mktime(&t);
		if (ts == -1) return 0;
		//如果要转成UTC时间，则需要根据时区进行转换
		if (isToUTC)
			ts -= getTZOffset() * 3600;
		return ts * 1000+ millisec;
	}

	/**
	 * @brief 将毫秒级时间戳转换为字符串表示
	 * 
	 * @param mytime 毫秒级时间戳，从1970年1月1日起算的毫秒数
	 * @return std::string 格式化后的时间字符串，格式为 "YYYYMMDDHHmmss" 或 "YYYYMMDDHHmmss.mmm"
	 * 
	 * @details 该方法将毫秒级时间戳转换为字符串格式。
	 * 如果时间戳包含毫秒部分，则格式为 "YYYYMMDDHHmmss.mmm"；
	 * 如果没有毫秒部分，则格式为 "YYYYMMDDHHmmss"。
	 * 如果传入的时间戳为0或无效，则返回空字符串。
	 * 该方法考虑了跨平台兼容性，在Windows和Unix/Linux系统上使用不同的本地时间转换函数。
	 */
	static std::string timeToString(int64_t mytime)
	{
		if (mytime == 0) return "";
		int64_t sec = mytime/1000;
		int msec = (int) (mytime - sec * 1000);
		if (msec < 0) return "";
		time_t tt =  sec;
		struct tm t;
#ifdef _WIN32
		localtime_s(&t, &tt);
#else
		localtime_r(&tt, &t);
#endif
		char tm_buf[64] = {'\0'};
		if (msec > 0) //是否有毫秒
		   sprintf(tm_buf,"%4d%02d%02d%02d%02d%02d.%03d",t.tm_year+1900, t.tm_mon+1, t.tm_mday,
			t.tm_hour, t.tm_min, t.tm_sec, msec);
		else 
		   sprintf(tm_buf,"%4d%02d%02d%02d%02d%02d",t.tm_year+1900, t.tm_mon+1, t.tm_mday,
			t.tm_hour, t.tm_min, t.tm_sec);
		return tm_buf;
	};

	/**
	 * @brief 获取指定日期后的第 N 天日期
	 * 
	 * @param curDate 当前日期，格式为 YYYYMMDD，例如 20220309
	 * @param days 要增加的天数，默认为1，可以为负数（表示往前计算）
	 * @return uint32_t 计算后的日期，格式为 YYYYMMDD
	 * 
	 * @details 该方法用于计算指定日期之后（或之前）的日期。
	 * 利用标准时间库中的mktime和localtime函数进行日期计算，
	 * 可以正确处理月底和年底的进位问题，以及闰年等特殊情况。
	 * 该方法在交易系统中常用于计算交易日、结算日等。
	 */
	static uint32_t getNextDate(uint32_t curDate, int days = 1)
	{
		tm t;	
		memset(&t,0,sizeof(tm));
		t.tm_year = curDate/10000 - 1900;
		t.tm_mon = (curDate%10000)/100 - 1;
		t.tm_mday = curDate % 100;
		//t.tm_isdst 	
		time_t ts = mktime(&t);
		ts += days*86400;

		tm* newT = localtime(&ts);
		return (newT->tm_year+1900)*10000 + (newT->tm_mon+1)*100 + newT->tm_mday;
	}

	/**
	 * @brief 获取指定时间后的第 N 分钟的时间
	 * 
	 * @param curTime 当前时间，格式为 HHMM，例如 1030表示10点30分
	 * @param mins 要增加的分钟数，默认为1，可以为负数（表示往前计算）
	 * @return uint32_t 计算后的时间，格式为 HHMM
	 * 
	 * @details 该方法用于计算指定时间之后（或之前）的时间。
	 * 时间计算基于24小时制，同时处理了跨日的情况（如果计算结果超过1440分钟或小于0分钟）。
	 * 例如，如果当前时间是2350，增加20分钟，则结果将是0010。
	 * 该方法在交易系统中常用于计算交易时间、K线周期等。
	 */
	static uint32_t getNextMinute(int32_t curTime, int32_t mins = 1)
	{
		int32_t curHour = curTime / 100;
		int32_t curMin = curTime % 100;
		int32_t totalMins = curHour * 60 + curMin;
		totalMins += mins;

		if (totalMins >= 1440)
			totalMins -= 1440;
		else if (totalMins < 0)
			totalMins += 1440;

		int32_t ret = (totalMins / 60) * 100 + totalMins % 60;
		return (uint32_t)ret;
	}

    /**
     * @brief 获取指定月份后的第 N 个月的年月
     * 
     * @param curMonth 当前年月，格式为 YYYYMM，例如 202203表示2022年3月
     * @param months 要增加的月数，默认为1，可以为负数（表示往前计算）
     * @return uint32_t 计算后的年月，格式为 YYYYMM
     * 
     * @details 该方法用于计算指定年月之后（或之前）的年月。
     * 计算过程中会正确处理跨年的情况，例如，如果当前是202212（即•2022年12月”），
     * 增加2个月后将得到202302（即•2023年2月”）。
     * 该方法在处理月度数据、计算合约到期日等有关月度时间的场景中非常实用。
     */
    static uint32_t getNextMonth(uint32_t curMonth, int months = 1)
    {
        int32_t uYear = curMonth / 100;
        int32_t uMonth = curMonth % 100; // [1, 12]
     
		int32_t uAddYear = months / 12;
        int32_t uAddMon = months % 12;
        if (uAddMon < 0) uAddMon += 12;  // math modulus: [0, 11]
     
        uYear += uAddYear;
        uMonth += uAddMon;
        if (uMonth > 12) {
            ++uYear;
            uMonth -= 12;
        }
        return (uint32_t) (uYear*100 + uMonth);
    }

	/**
	 * @brief 将日期和时间转换为分钟线ID
	 * 
	 * @param uDate 日期，格式为 YYYYMMDD，例如 20220309
	 * @param uTime 时间，格式为 HHMM或HHMMSS，例如 1030或103000
	 * @return uint64_t 分钟线ID，用于唯一标识特定分钟的K线
	 * 
	 * @details 该方法将日期和时间转换为分钟线ID，计算方式为(uDate-19900000)*10000 + uTime。
	 * 例如，如果日期是20220309，时间是1030，则分钟线ID为(20220309-19900000)*10000 + 1030 = 23303091030。
	 * 该ID可以唯一标识交易系统中的每一分钟的K线数据，并支持数据存储和查询。
	 */
	static inline uint64_t timeToMinBar(uint32_t uDate, uint32_t uTime)
	{
		return (uint64_t)((uDate-19900000)*10000) + uTime;
	}

	/**
	 * @brief 从分钟线ID中提取日期部分
	 * 
	 * @param minTime 分钟线ID，由timeToMinBar方法生成
	 * @return uint32_t 日期，格式为 YYYYMMDD
	 * 
	 * @details 该方法从分钟线ID中提取日期部分，是timeToMinBar方法的逆操作。
	 * 计算方式为 minTime/10000 + 19900000。
	 * 例如，如果分钟线ID是23303091030，则日期为23303091030/10000 + 19900000 = 2330309 + 19900000 = 20220309。
	 */
	static inline uint32_t minBarToDate(uint64_t minTime)
	{
		return (uint32_t)(minTime/10000 + 19900000);
	}

	/**
	 * @brief 从分钟线ID中提取时间部分
	 * 
	 * @param minTime 分钟线ID，由timeToMinBar方法生成
	 * @return uint32_t 时间，格式为 HHMM或HHMMSS
	 * 
	 * @details 该方法从分钟线ID中提取时间部分，是timeToMinBar方法的逆操作。
	 * 计算方式为 minTime % 10000。
	 * 例如，如果分钟线ID是23303091030，则时间为23303091030 % 10000 = 1030。
	 * 返回的时间格式依赖于原始的时间格式，如果原始时间是4位数字，则返回的是HHMM格式，如果是6位数字，则返回的是HHMMSS格式。
	 */
	static inline uint32_t minBarToTime(uint64_t minTime)
	{
		return (uint32_t)(minTime%10000);
	}

	/**
	 * @brief 判断指定日期是否为周末（周六或周日）
	 * 
	 * @param uDate 日期，格式为 YYYYMMDD
	 * @return bool 如果是周末返回true，否则返回false
	 * 
	 * @details 该方法判断指定的日期是否为周末（即周六或周日）。
	 * 首先将日期转换为tm结构体，然后使用mktime和localtime函数获取对应的星期信息。
	 * 如果转换过程中出现错误，或者结果显示该日期是周六（tm_wday==6）或周日（tm_wday==0），
	 * 则返回true，否则返回false。
	 * 该方法在交易系统中常用于判断是否为交易日。
	 */
	static inline bool isWeekends(uint32_t uDate)
	{
		tm t;	
		memset(&t,0,sizeof(tm));
		t.tm_year = uDate/1/10000 - 1900;
		t.tm_mon = (uDate/1%10000)/100 - 1;
		t.tm_mday = uDate/1 % 100;

		time_t tt = mktime(&t);
		tm* tmt = localtime(&tt);
		if(tmt == NULL)
			return true;
	
		if(tmt->tm_wday == 0 || tmt->tm_wday==6)
			return true;

		return false;
	}

public:
	/**
	 * @brief 时间处理工具类，用于处理包含毫秒级精度的时间
	 * 
	 * @details Time32类封装了时间相关的操作，包括时间的格式化、转换、日期和时间的提取等功能。
	 * 类内部使用tm结构体存储时间信息，并额外记录毫秒信息。
	 * 该类提供了多种构造方式和各种时间格式化方法，在需要精确时间处理的场景中非常有用。
	 */
	class Time32
	{
	public:
		/**
		 * @brief 默认构造函数
		 * 
		 * @details 创建一个时间对象，并将毫秒部分初始化为0。
		 * 注意这个构造函数仅初始化了毫秒部分，时间结构体tm未进行初始化。
		 */
		Time32():_msec(0){}

		/**
		 * @brief 使用time_t时间戳和毫秒构造时间对象
		 * 
		 * @param _time 标准C/C++时间戳，从1970年1月1日起算的秒数
		 * @param msecs 毫秒部分，默认为0
		 * 
		 * @details 创建一个时间对象，使用给定的time_t时间戳和毫秒值。
		 * 该构造函数会将time_t时间戳转换为tm结构体，并存储毫秒值。
		 * 该函数考虑了跨平台兼容性，在Windows和Unix/Linux系统上使用不同的时间转换函数。
		 */
		Time32(time_t _time, uint32_t msecs = 0)
		{
#ifdef _WIN32
			localtime_s(&t, &_time);
#else
			localtime_r(&_time, &t);
#endif
			_msec = msecs;
		}

		/**
		 * @brief 使用毫秒级时间戳构造时间对象
		 * 
		 * @param _time 毫秒级时间戳，从1970年1月1日起算的毫秒数
		 * 
		 * @details 创建一个时间对象，使用给定的毫秒级时间戳。
		 * 该构造函数先将毫秒级时间戳拆分为秒数与毫秒两部分，
		 * 然后将秒数部分转换为tm结构体，并存储毫秒值。
		 * 该函数考虑了跨平台兼容性，在Windows和Unix/Linux系统上使用不同的时间转换函数。
		 */
		Time32(uint64_t _time)
		{
			time_t _t = _time/1000;
			_msec = (uint32_t)_time%1000;
#ifdef _WIN32
			localtime_s(&t, &_t);
#else
			localtime_r(&_t, &t);
#endif
		}

		/**
		 * @brief 从毫秒级本地时间戳设置时间对象
		 * 
		 * @param _time 毫秒级本地时间戳，从1970年1月1日起算的毫秒数
		 * 
		 * @details 该方法使用给定的毫秒级时间戳设置当前时间对象的值。
		 * 方法先将毫秒级时间戳拆分为秒数与毫秒两部分，
		 * 然后将秒数部分转换为tm结构体，并存储毫秒值。
		 * 该函数同样考虑了跨平台兼容性，在Windows和Unix/Linux系统上使用不同的时间转换函数。
		 */
		void from_local_time(uint64_t _time)
		{
			time_t _t = _time/1000;
			_msec = (uint32_t)(_time%1000);
#ifdef _WIN32
			localtime_s(&t, &_t);
#else
			localtime_r(&_t, &t);
#endif
		}

		/**
		 * @brief 获取日期部分
		 * 
		 * @return uint32_t 日期，格式为 YYYYMMDD
		 * 
		 * @details 该方法从当前时间对象中提取日期部分，并将其转换为 YYYYMMDD 格式的整数表示。
		 * 计算方式为：年*10000 + 月*100 + 日。
		 * 例如，2022年3月9日将返回 20220309。
		 */
		uint32_t date()
		{
			return (t.tm_year + 1900)*10000 + (t.tm_mon + 1)*100 + t.tm_mday;
		}

		/**
		 * @brief 获取时间部分（不包含毫秒）
		 * 
		 * @return uint32_t 时间，格式为 HHMMSS
		 * 
		 * @details 该方法从当前时间对象中提取时间部分，不包含毫秒，并将其转换为 HHMMSS 格式的整数表示。
		 * 计算方式为：小时*10000 + 分钟*100 + 秒。
		 * 例如，10点30分45秒将返回 103045。
		 */
		uint32_t time()
		{
			return t.tm_hour*10000 + t.tm_min*100 + t.tm_sec;
		}

		/**
		 * @brief 获取时间部分（包含毫秒）
		 * 
		 * @return uint32_t 时间，格式为 HHMMSSmmm
		 * 
		 * @details 该方法从当前时间对象中提取时间部分，包含毫秒，并将其转换为 HHMMSSmmm 格式的整数表示。
		 * 计算方式为：小时*10000000 + 分钟*100000 + 秒*1000 + 毫秒。
		 * 例如，10点30分45秒123毫秒将返回 103045123。
		 * 该方法在需要精确到毫秒级别的时间表示场景中很有用，如高频交易、性能测试等。
		 */
		uint32_t time_ms()
		{
			return t.tm_hour*10000000 + t.tm_min*100000 + t.tm_sec*1000 + _msec;
		}

		/**
		 * @brief 格式化时间为字符串
		 * 
		 * @param sfmt 时间格式化字符串，遵循strftime函数的格式规则，默认为"%Y.%m.%d %H:%M:%S"
		 * @param hasMilliSec 是否包含毫秒信息，默认为false
		 * @return const char* 格式化后的时间字符串
		 * 
		 * @details 该方法将当前时间对象格式化为指定格式的字符串。
		 * 首先使用strftime函数将tm结构体格式化为字符串，然后根据hasMilliSec参数决定是否需要在字符串末尾添加毫秒信息。
		 * 注意该方法返回的是一个静态内部缓冲区的指针，调用者不应修改该指针指向的内容，
		 * 且如果连续多次调用该方法，之前返回的结果可能会被覆盖。
		 * 常用于日志输出、调试信息打印等需要格式化时间的场景。
		 */
		const char* fmt(const char* sfmt = "%Y.%m.%d %H:%M:%S", bool hasMilliSec = false) const
		{
			static char buff[1024];
			uint32_t length = (uint32_t)strftime(buff, 1023, sfmt, &t);
			if (hasMilliSec)
				sprintf(buff + length, ",%03u", _msec);
			return buff;
		}

	protected:
		/**
		 * @brief 时间结构体
		 * 
		 * @details 存储日期和时间信息，包括年、月、日、时、分、秒等。
		 * 这是C/C++标准库中的时间结构体，用于时间操作和格式化。
		 */
		struct tm t;

		/**
		 * @brief 毫秒部分
		 * 
		 * @details 存储毫秒级别的时间精度，范围为0-999。
		 * 由于C/C++标准库中的tm结构体不包含毫秒信息，
		 * 因此需要额外使用该变量来存储毫秒部分。
		 */
		uint32_t _msec;
	};

	/**
	 * @brief 高精度计时器类
	 * 
	 * @details Ticker类提供了一个高精度的计时工具，基于C++11的std::chrono库实现。
	 * 该类可用于测量从实例化或重置开始到调用时的经过时间，
	 * 支持以秒、毫秒、微秒和纳秒为单位返回经过的时间。
	 * 该类在需要测量程序性能、执行时间或实现超时功能的场景中非常有用。
	 */
	class Ticker
	{
	public:
		/**
		 * @brief 构造函数，创建计时器并开始计时
		 * 
		 * @details 创建一个新的Ticker实例，并将内部的时间记录点初始化为当前时间。
		 * 该构造函数使用std::chrono::high_resolution_clock获取当前系统的高精度时间，
		 * 使计时器具有最高的时间精度。一旦实例化，计时器就开始运行。
		 */
		Ticker()
		{
			_tick = std::chrono::high_resolution_clock::now();
		}

		/**
		 * @brief 重置计时器
		 * 
		 * @details 将计时器的内部时间记录点重置为当前时间，相当于重新开始计时。
		 * 该方法同样使用std::chrono::high_resolution_clock获取当前系统的高精度时间。
		 * 调用该方法后，后续的时间测量将以这个新的时间点作为起始点。
		 * 当需要多次使用同一个计时器实例测量不同时间段时，该方法非常有用。
		 */
		void reset()
		{
			_tick = std::chrono::high_resolution_clock::now();
		}

		/**
		 * @brief 获取从计时器初始化或重置开始经过的秒数
		 * 
		 * @return int64_t 经过的时间，单位为秒
		 * 
		 * @details 该方法计算从计时器初始化或最近一次重置到现在经过的时间，并以秒为单位返回。
		 * 该方法先获取当前时间，然后计算与计时器起始时间的差值，最后转换为秒单位。
		 * 该方法适用于测量较长时间的操作，如长时间运行的任务、超时检测等。
		 */
		inline int64_t seconds() const 
		{
			auto now = std::chrono::high_resolution_clock::now();
			auto td = now - _tick;
			return std::chrono::duration_cast<std::chrono::seconds>(td).count();
		}

		/**
		 * @brief 获取从计时器初始化或重置开始经过的毫秒数
		 * 
		 * @return int64_t 经过的时间，单位为毫秒
		 * 
		 * @details 该方法计算从计时器初始化或最近一次重置到现在经过的时间，并以毫秒为单位返回。
		 * 该方法先获取当前时间，然后计算与计时器起始时间的差值，最后转换为毫秒单位。
		 * 该方法适用于测量中等时间的操作，如网络请求响应时间、算法执行时间等。
		 * 1秒 = 1000毫秒。
		 */
		inline int64_t milli_seconds() const
		{
			auto now = std::chrono::high_resolution_clock::now();
			auto td = now - _tick;
			return std::chrono::duration_cast<std::chrono::milliseconds>(td).count();
		}

		/**
		 * @brief 获取从计时器初始化或重置开始经过的微秒数
		 * 
		 * @return int64_t 经过的时间，单位为微秒
		 * 
		 * @details 该方法计算从计时器初始化或最近一次重置到现在经过的时间，并以微秒为单位返回。
		 * 该方法先获取当前时间，然后计算与计时器起始时间的差值，最后转换为微秒单位。
		 * 该方法适用于测量较短时间的操作，如内存操作、CPU密集型计算等。
		 * 1毫秒 = 1000微秒，1秒 = 1000000微秒。
		 */
		inline int64_t micro_seconds() const
		{
			auto now = std::chrono::high_resolution_clock::now();
			auto td = now - _tick;
			return std::chrono::duration_cast<std::chrono::microseconds>(td).count();
		}

		/**
		 * @brief 获取从计时器初始化或重置开始经过的纳秒数
		 * 
		 * @return int64_t 经过的时间，单位为纳秒
		 * 
		 * @details 该方法计算从计时器初始化或最近一次重置到现在经过的时间，并以纳秒为单位返回。
		 * 该方法先获取当前时间，然后计算与计时器起始时间的差值，最后转换为纳秒单位。
		 * 该方法提供了最高精度的时间测量，适用于测量非常短的操作时间，如单条指令执行、高频交易环境中的时间关键操作等。
		 * 1微秒 = 1000纳秒，1毫秒 = 1000000纳秒，1秒 = 1000000000纳秒。
		 */
		inline int64_t nano_seconds() const
		{
			auto now = std::chrono::high_resolution_clock::now();
			auto td = now - _tick;
			return std::chrono::duration_cast<std::chrono::nanoseconds>(td).count();
		}

	private:
		/**
		 * @brief 计时器的起始时间点
		 * 
		 * @details 存储计时器初始化或最近一次重置时的高精度时间点。
		 * 该时间点用于计算经过的时间。使用std::chrono::high_resolution_clock提供的高精度时间点，
		 * 确保时间测量具有最高的精度。
		 * 该类型是C++11标准库中提供的高精度计时工具。
		 */
		std::chrono::time_point<std::chrono::high_resolution_clock> _tick;
	};
};
