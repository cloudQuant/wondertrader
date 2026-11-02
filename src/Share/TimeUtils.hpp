/*!
 * \file TimeUtils.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 时间处理的封装
 */
#pragma once
#include <stdint.h>
#include <time.h>
#include <sys/timeb.h>
#ifndef _MSC_VER
#include <sys/time.h>
#endif
#include <string>
#include <string.h>
#include <chrono>
#include <thread>
#include <cmath>

#ifdef _MSC_VER
#define CTIME_BUF_SIZE 64

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _MSC_VER
#pragma warning(disable:4996)
#endif

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

namespace TimeUtils 
{
	// Sunday is 0, matching tm_wday. Values >= 7 indicate an invalid date or
	// an unavailable local date when getWeekDay() is called without an argument.
	constexpr uint32_t INVALID_WEEKDAY = 7u;

	static inline int32_t getTZOffset() noexcept
	{
		static int32_t offset = 99;
		if (offset == 99)
		{
			time_t now = time(NULL);
			[[maybe_unused]] tm tm_ltm = *localtime(&now);
			tm tm_gtm = *gmtime(&now);

			time_t _gt = mktime(&tm_gtm);
			tm _gtm2 = *localtime(&_gt);

			offset = (uint32_t)(((now - _gt) + (_gtm2.tm_isdst ? 3600 : 0)) / 60);
			offset /= 60;
		}

		return offset;
	}

	constexpr static inline int fasttime(const time_t& unix_sec, struct tm* tm, int time_zone) noexcept
	{
		// Split into a floor-normalized day and seconds within that day. C++ integer
		// division truncates towards zero, which produces negative tm fields before
		// the Unix epoch unless the remainder is normalized explicitly.
		int64_t days = static_cast<int64_t>(unix_sec) / 86400;
		int64_t seconds_of_day = static_cast<int64_t>(unix_sec) % 86400;
		if (seconds_of_day < 0)
		{
			--days;
			seconds_of_day += 86400;
		}

		// Apply timezone hours without multiplying an extreme int offset by 3600.
		const int64_t zone_hours = static_cast<int64_t>(time_zone);
		days += zone_hours / 24;
		int64_t hour = seconds_of_day / 3600 + zone_hours % 24;
		if (hour < 0)
		{
			hour += 24;
			--days;
		}
		else if (hour >= 24)
		{
			hour -= 24;
			++days;
		}

		// Gregorian civil date from days since 1970-01-01 (proleptic Gregorian).
		// Use 64-bit intermediates so valid time_t values do not overflow the old
		// 32-bit date arithmetic.
		int64_t civil_days = days + 719468;
		const int64_t era = (civil_days >= 0 ? civil_days : civil_days - 146096) / 146097;
		const int64_t day_of_era = civil_days - era * 146097;
		const int64_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
		int64_t year = year_of_era + era * 400;
		const int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
		const int64_t month_prime = (5 * day_of_year + 2) / 153;
		const int64_t day = day_of_year - (153 * month_prime + 2) / 5 + 1;
		const int64_t month = month_prime + (month_prime < 10 ? 3 : -9);
		year += month <= 2;

		const int64_t tm_year = year - 1900;
		if (tm_year < INT32_MIN || tm_year > INT32_MAX)
			return -1;
		tm->tm_hour = static_cast<int>(hour);
		tm->tm_min = static_cast<int>((seconds_of_day % 3600) / 60);
		tm->tm_sec = static_cast<int>(seconds_of_day % 60);
		tm->tm_mday = static_cast<int>(day);
		tm->tm_mon = static_cast<int>(month - 1);
		tm->tm_year = static_cast<int>(tm_year);
		return 0;
	}

	/*
	 *	获取本地时间，精确到毫秒
	 */
	static inline int64_t getLocalTimeNow(void) noexcept
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

	static inline std::string getLocalTime(bool bIncludeMilliSec = true) noexcept
	{
		uint64_t ltime = getLocalTimeNow();
		time_t now = ltime / 1000;
		uint32_t millitm = ltime % 1000;
		
		thread_local static tm tNow;
		fasttime(now, &tNow, getTZOffset());

		char str[64] = {0};
		if(bIncludeMilliSec)
			snprintf(str, sizeof(str), "%02d:%02d:%02d,%03d", tNow.tm_hour, tNow.tm_min, tNow.tm_sec, millitm);
		else
			snprintf(str, sizeof(str), "%02d:%02d:%02d", tNow.tm_hour, tNow.tm_min, tNow.tm_sec);
		return str;
	}

	static inline uint64_t getYYYYMMDDhhmmss() noexcept
	{
		uint64_t ltime = getLocalTimeNow();
		time_t now = ltime / 1000;

		thread_local static tm tNow;
		fasttime(now, &tNow, getTZOffset());

		uint64_t date = (tNow.tm_year + 1900) * 10000 + (tNow.tm_mon + 1) * 100 + tNow.tm_mday;

		uint64_t time = tNow.tm_hour * 10000 + tNow.tm_min * 100 + tNow.tm_sec;
		return date * 1000000 + time;
	}

    /*
     * 读取当前时间
     * @date    当前日期，格式如20220309
     * @time    当前时间，精确到毫秒，格式如103029500
     */
	static inline void getDateTime(uint32_t &date, uint32_t &time) noexcept
	{
		uint64_t ltime = getLocalTimeNow();
		time_t now = ltime / 1000;
		uint32_t millitm = ltime % 1000;

		thread_local static tm tNow;
		fasttime(now, &tNow, getTZOffset());

		date = (tNow.tm_year+1900)*10000 + (tNow.tm_mon+1)*100 + tNow.tm_mday;
		
		time = tNow.tm_hour*10000 + tNow.tm_min*100 + tNow.tm_sec;
		time *= 1000;
		time += millitm;
	}

	static inline uint32_t getCurDate() noexcept
	{
		uint64_t ltime = getLocalTimeNow();
		time_t now = ltime / 1000;

		thread_local static tm tNow;
		fasttime(now, &tNow, getTZOffset());

		uint32_t date = (tNow.tm_year+1900)*10000 + (tNow.tm_mon+1)*100 + tNow.tm_mday;

		return date;
	}

	static inline bool tryGetWeekDay(uint32_t uDate, uint32_t& weekDay) noexcept
	{
		weekDay = INVALID_WEEKDAY;
		if (uDate == 0)
		{
			// Preserve the no-argument current-local-date behavior, while checking
			// the platform conversion result before reading the tm structure.
			time_t now = time(NULL);
			tm localTime = {};
#ifdef _MSC_VER
			if (localtime_s(&localTime, &now) != 0)
				return false;
#else
			if (localtime_r(&now, &localTime) == NULL)
				return false;
#endif
			const int64_t year = static_cast<int64_t>(localTime.tm_year) + 1900;
			if (year < 1000 || year > 9999 || localTime.tm_mon < 0 || localTime.tm_mon > 11 ||
				localTime.tm_mday < 1 || localTime.tm_mday > 31)
				return false;
			uDate = static_cast<uint32_t>(year * 10000 + (localTime.tm_mon + 1) * 100 + localTime.tm_mday);
		}

		const int64_t year = static_cast<int64_t>(uDate / 10000);
		const uint32_t month = (uDate / 100) % 100;
		const uint32_t day = uDate % 100;
		if (year < 1000 || year > 9999 || month < 1 || month > 12 || day < 1)
			return false;

		static const uint8_t daysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
		uint32_t maxDay = daysInMonth[month - 1];
		const bool isLeapYear = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
		if (month == 2 && isLeapYear)
			++maxDay;
		if (day > maxDay)
			return false;

		// Count Gregorian days from 0001-01-01 (a Monday). This avoids mktime
		// normalization and gives identical results for dates before/after 1970.
		const int64_t previousYear = year - 1;
		int64_t days = previousYear * 365 + previousYear / 4 - previousYear / 100 + previousYear / 400;
		for (uint32_t currentMonth = 1; currentMonth < month; ++currentMonth)
			days += daysInMonth[currentMonth - 1] + (currentMonth == 2 && isLeapYear ? 1 : 0);
		days += day - 1;
		weekDay = static_cast<uint32_t>((days + 1) % 7); // Monday=1, Sunday=0.
		return true;
	}

	static inline uint32_t getWeekDay(uint32_t uDate = 0) noexcept
	{
		uint32_t weekDay = INVALID_WEEKDAY;
		(void)tryGetWeekDay(uDate, weekDay);
		return weekDay;
	}

	static inline uint32_t getCurMin() noexcept
	{
		uint64_t ltime = getLocalTimeNow();
		time_t now = ltime / 1000;
		thread_local static tm tNow;
		fasttime(now, &tNow, getTZOffset());
		uint32_t time = tNow.tm_hour*10000 + tNow.tm_min*100 + tNow.tm_sec;

		return time;
	}

	/*
	 *	生成带毫秒的timestamp
	 *	@lDate			日期，yyyymmdd
	 *	@lTimeWithMs	带毫秒的时间，HHMMSSsss
	 *	@isToUTC		是否转成UTC时间
	 */
	static inline int64_t makeTime(long lDate, long lTimeWithMs, bool isToUTC = false) noexcept
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

	[[maybe_unused]] static std::string timeToString(int64_t mytime) noexcept
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
		   snprintf(tm_buf,sizeof(tm_buf),"%4d%02d%02d%02d%02d%02d.%03d",t.tm_year+1900, t.tm_mon+1, t.tm_mday,
			t.tm_hour, t.tm_min, t.tm_sec, msec);
		else 
		   snprintf(tm_buf,sizeof(tm_buf),"%4d%02d%02d%02d%02d%02d",t.tm_year+1900, t.tm_mon+1, t.tm_mday,
			t.tm_hour, t.tm_min, t.tm_sec);
		return tm_buf;
	};

	[[maybe_unused]] static uint32_t getNextDate(uint32_t curDate, int days = 1) noexcept
	{
		tm t;	
		memset(&t,0,sizeof(tm));
		t.tm_year = curDate/10000 - 1900;
		t.tm_mon = (curDate%10000)/100 - 1;
		t.tm_mday = curDate % 100;
		//t.tm_isdst 	
		time_t ts = mktime(&t);
		ts += days*86400;

		thread_local static tm newT;
		fasttime(ts, &newT, getTZOffset());
		return (newT.tm_year+1900)*10000 + (newT.tm_mon+1)*100 + newT.tm_mday;
	}

	constexpr static uint32_t getNextMinute(int32_t curTime, int32_t mins = 1) noexcept
	{
		// HHMM is reduced to a minute-of-day, and the offset may span any number
		// of days. 64-bit arithmetic avoids overflow for INT32_MIN/MAX offsets.
		const int64_t cur_hour = static_cast<int64_t>(curTime) / 100;
		const int64_t cur_min = static_cast<int64_t>(curTime) % 100;
		int64_t total_mins = cur_hour * 60 + cur_min + static_cast<int64_t>(mins);
		total_mins %= 1440;
		if (total_mins < 0)
			total_mins += 1440;

		return static_cast<uint32_t>((total_mins / 60) * 100 + total_mins % 60);
	}

    /*
     * @curMonth: YYYYMM
     * @return: YYYYMM
     */
	constexpr static uint32_t getNextMonth(uint32_t curMonth, int months = 1) noexcept
    {
        // YYYYMM inputs use a one-based month. Convert to a zero-based
        // absolute month index so negative offsets use floor normalization
        // across year boundaries as well as positive offsets.
        const int64_t year = static_cast<int64_t>(curMonth / 100);
        const int64_t month = static_cast<int64_t>(curMonth % 100);
        int64_t absolute_month = year * 12 + (month - 1) + static_cast<int64_t>(months);

        int64_t result_year = absolute_month / 12;
        int64_t result_month = absolute_month % 12;
        if (result_month < 0)
        {
            --result_year;
            result_month += 12;
        }
        ++result_month; // Back to YYYYMM's one-based month.

        return static_cast<uint32_t>(result_year * 100 + result_month);
    }

	constexpr static inline uint64_t timeToMinBar(uint32_t uDate, uint32_t uTime) noexcept
	{
		const uint64_t dateOffset = static_cast<uint64_t>(uDate - 19900000u);
		return dateOffset * 10000ull + static_cast<uint64_t>(uTime);
	}

	constexpr static inline uint32_t minBarToDate(uint64_t minTime) noexcept
	{
		return (uint32_t)(minTime/10000 + 19900000);
	}

	constexpr static  inline uint32_t minBarToTime(uint64_t minTime) noexcept
	{
		return (uint32_t)(minTime%10000);
	}

	static inline bool isWeekends(uint32_t uDate) noexcept
	{
		const uint32_t weekDay = getWeekDay(uDate);
		return weekDay == 0 || weekDay == 6;
	}

	class Time32
	{
	public:
		// A default-constructed value is a deterministic 1970-01-01 00:00:00.000
		// wall-time representation. This is an initial value, not a conversion
		// from a local epoch; constructors taking a timestamp use local time below.
		Time32() : t{}, _msec(0)
		{
			t.tm_year = 70;
			t.tm_mon = 0;
			t.tm_mday = 1;
			t.tm_wday = 4;
			t.tm_yday = 0;
			t.tm_isdst = 0;
		}

		Time32(time_t _time, uint32_t msecs = 0)
		{
#ifdef _WIN32
			localtime_s(&t, &_time);
#else
			localtime_r(&_time, &t);
#endif
			_msec = msecs;
		}

		Time32(uint64_t timeWithMSecs)
		{
			time_t _t = timeWithMSecs/1000;
			_msec = static_cast<uint32_t>(timeWithMSecs % 1000);
#ifdef _WIN32
			localtime_s(&t, &_t);
#else
			localtime_r(&_t, &t);
#endif
		}

		void from_local_time(uint64_t timeWithMSecs)
		{
			time_t _t = timeWithMSecs/1000;
			_msec = (uint32_t)(timeWithMSecs%1000);
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
				snprintf(buff + length, sizeof(buff) - length, ",%03u", _msec);
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

		inline void reset() noexcept
		{
			_tick = std::chrono::high_resolution_clock::now();
		}

		inline int64_t seconds() const noexcept
		{
			auto now = std::chrono::high_resolution_clock::now();
			auto td = now - _tick;
			return std::chrono::duration_cast<std::chrono::seconds>(td).count();
		}

		inline int64_t milli_seconds() const noexcept
		{
			auto now = std::chrono::high_resolution_clock::now();
			auto td = now - _tick;
			return std::chrono::duration_cast<std::chrono::milliseconds>(td).count();
		}

		inline int64_t micro_seconds() const noexcept
		{
			auto now = std::chrono::high_resolution_clock::now();
			auto td = now - _tick;
			return std::chrono::duration_cast<std::chrono::microseconds>(td).count();
		}

		inline int64_t nano_seconds() const noexcept
		{
			auto now = std::chrono::high_resolution_clock::now();
			auto td = now - _tick;
			return std::chrono::duration_cast<std::chrono::nanoseconds>(td).count();
		}

	private:
		std::chrono::time_point<std::chrono::high_resolution_clock> _tick;
	};
};
