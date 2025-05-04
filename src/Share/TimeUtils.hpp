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

	/*
	 *	生成带毫秒的timestamp
	 *	@lDate			日期，yyyymmdd
	 *	@lTimeWithMs	带毫秒的时间，HHMMSSsss
	 *	@isToUTC		是否转成UTC时间
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

    /*
     * @curMonth: YYYYMM
     * @return: YYYYMM
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

	static inline uint64_t timeToMinBar(uint32_t uDate, uint32_t uTime)
	{
		return (uint64_t)((uDate-19900000)*10000) + uTime;
	}

	static inline uint32_t minBarToDate(uint64_t minTime)
	{
		return (uint32_t)(minTime/10000 + 19900000);
	}

	static inline uint32_t minBarToTime(uint64_t minTime)
	{
		return (uint32_t)(minTime%10000);
	}

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
	class Time32
	{
	public:
		Time32():_msec(0){}

		Time32(time_t _time, uint32_t msecs = 0)
		{
#ifdef _WIN32
			localtime_s(&t, &_time);
#else
			localtime_r(&_time, &t);
#endif
			_msec = msecs;
		}

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

		uint32_t date()
		{
			return (t.tm_year + 1900)*10000 + (t.tm_mon + 1)*100 + t.tm_mday;
		}

		uint32_t time()
		{
			return t.tm_hour*10000 + t.tm_min*100 + t.tm_sec;
		}

		uint32_t time_ms()
		{
			return t.tm_hour*10000000 + t.tm_min*100000 + t.tm_sec*1000 + _msec;
		}

		const char* fmt(const char* sfmt = "%Y.%m.%d %H:%M:%S", bool hasMilliSec = false) const
		{
			static char buff[1024];
			uint32_t length = (uint32_t)strftime(buff, 1023, sfmt, &t);
			if (hasMilliSec)
				sprintf(buff + length, ",%03u", _msec);
			return buff;
		}

	protected:
		struct tm t;
		uint32_t _msec;
	};

	class Ticker
	{
	public:
		Ticker()
		{
			_tick = std::chrono::high_resolution_clock::now();
		}

		void reset()
		{
			_tick = std::chrono::high_resolution_clock::now();
		}

		inline int64_t seconds() const 
		{
			auto now = std::chrono::high_resolution_clock::now();
			auto td = now - _tick;
			return std::chrono::duration_cast<std::chrono::seconds>(td).count();
		}

		inline int64_t milli_seconds() const
		{
			auto now = std::chrono::high_resolution_clock::now();
			auto td = now - _tick;
			return std::chrono::duration_cast<std::chrono::milliseconds>(td).count();
		}

		inline int64_t micro_seconds() const
		{
			auto now = std::chrono::high_resolution_clock::now();
			auto td = now - _tick;
			return std::chrono::duration_cast<std::chrono::microseconds>(td).count();
		}

		inline int64_t nano_seconds() const
		{
			auto now = std::chrono::high_resolution_clock::now();
			auto td = now - _tick;
			return std::chrono::duration_cast<std::chrono::nanoseconds>(td).count();
		}

	private:
		std::chrono::time_point<std::chrono::high_resolution_clock> _tick;
	};
};
