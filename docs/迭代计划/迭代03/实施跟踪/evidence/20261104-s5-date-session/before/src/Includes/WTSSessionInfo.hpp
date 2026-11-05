/*!
 * \file WTSSessionInfo.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief Wt交易时间模板对象定义
 */
#pragma once
#include <vector>

#include "WTSObject.hpp"
#include "../Share/TimeUtils.hpp"

NS_WTP_BEGIN

static const char* DEFAULT_SESSIONID = "TRADING";

class WTSSessionInfo : public WTSObject
{
public:
	//交易时段
	//typedef std::pair<uint32_t, uint32_t>	TradingSection;
	typedef struct _TradingSection
	{
		uint32_t	first_raw;	//原始开始时间
		uint32_t	first;		//偏移过的开始时间

		uint32_t	second_raw;	//原始结束时间
		uint32_t	second;		//偏移过的结束时间

		_TradingSection(uint32_t stime, uint32_t etime, uint32_t stime_raw, uint32_t etime_raw)
			: first_raw(stime_raw), first(stime), second_raw(etime_raw)
		, second(etime){
		}
	} TradingSection;
	typedef std::vector<TradingSection>		TradingTimes;

protected:
	TradingTimes	m_tradingTimes;
	/*
	 *	By Wesley @ 2023.05.17
	 *	集合竞价时间改成多段
	 *	但是很多用到的地方还是只涉及第一个集合竞价时间，主要是一些状态判断
	 *	白盘的集合竞价会在开盘前一分钟撮合，状态机会往前延伸一分钟，所以原有逻辑不需要拓展
	 */
	TradingTimes	m_auctionTimes;
	int32_t			m_uOffsetMins;

	std::string		m_strID;
	std::string		m_strName;

	std::vector<uint32_t> _minutes;

protected:
	WTSSessionInfo(int32_t offset)
	{
		m_uOffsetMins = offset;
	}

	TradingSection makeAuctionSection(uint32_t sTime, uint32_t eTime) const
	{
		// Raw (0,0) is an empty auction range with a configured start threshold
		// of 0. Keep that threshold visible; endpoint mapping would expand it to
		// [00:00,24:00) at zero offset.
		if (sTime == 0 && eTime == 0)
			return TradingSection(0, 0, sTime, eTime);

		return TradingSection(offsetTime(sTime, true), offsetTime(eTime, false), sTime, eTime);
	}

	virtual ~WTSSessionInfo(){}

public:
	const char* id() const{ return m_strID.c_str(); }
	const char* name() const{ return m_strName.c_str(); }

	static WTSSessionInfo* create(const char* sid, const char* name, int32_t offset = 0)
	{
		WTSSessionInfo* pRet = new WTSSessionInfo(offset);
		pRet->m_strID = sid;
		pRet->m_strName = name;
		return pRet;
	}

public:
	int32_t	getOffsetMins() const{return m_uOffsetMins;}

	// HHMM accepts 00:00 through 23:59 and the 24:00 endpoint label.
	static bool isValidSessionTime(uint32_t time)
	{
		return time <= 2400 && time % 100 < 60 && (time / 100 < 24 || time == 2400);
	}

	bool tryAddTradingSection(uint32_t sTime, uint32_t eTime)
	{
		if (!isValidSessionTime(sTime) || !isValidSessionTime(eTime) ||
			((sTime % 2400 == eTime % 2400) && !(sTime == 0 && eTime == 0)))
			return false;
		m_tradingTimes.emplace_back(TradingSection(offsetTime(sTime, true), offsetTime(eTime, false), sTime, eTime));
		_minutes.clear();
		return true;
	}

	void addTradingSection(uint32_t sTime, uint32_t eTime)
	{
		(void)tryAddTradingSection(sTime, eTime);
	}

	bool trySetAuctionTime(uint32_t sTime, uint32_t eTime)
	{
		if (!isValidSessionTime(sTime) || !isValidSessionTime(eTime))
			return false;
		if (m_auctionTimes.empty())
		{
			m_auctionTimes.emplace_back(makeAuctionSection(sTime, eTime));
		}
		else
		{
			m_auctionTimes[0] = makeAuctionSection(sTime, eTime);
		}
		return true;
	}

	void setAuctionTime(uint32_t sTime, uint32_t eTime)
	{
		(void)trySetAuctionTime(sTime, eTime);
	}

	bool tryAddAuctionTime(uint32_t sTime, uint32_t eTime)
	{
		if (!isValidSessionTime(sTime) || !isValidSessionTime(eTime))
			return false;
		m_auctionTimes.emplace_back(makeAuctionSection(sTime, eTime));
		return true;
	}

	void addAuctionTime(uint32_t sTime, uint32_t eTime)
	{
		(void)tryAddAuctionTime(sTime, eTime);
	}

	void setOffsetMins(int32_t offset)
	{
		m_uOffsetMins = offset;

		for (TradingSection& section : m_tradingTimes)
		{
			section.first = offsetTime(section.first_raw, true);
			section.second = offsetTime(section.second_raw, false);
		}

		for (TradingSection& section : m_auctionTimes)
		{
			section = makeAuctionSection(section.first_raw, section.second_raw);
		}

		_minutes.clear();
	}

	const TradingTimes&		getTradingSections() const{ return m_tradingTimes; }
	const TradingTimes&		getAuctionSections() const{ return m_auctionTimes; }

	//需要导出到脚本的函数
public:
	uint32_t getSectionCount() const{ return (uint32_t)m_tradingTimes.size(); }

	/*
	 *	计算偏移以后的日期
	 *	主要用于各种日期比较
	 *	如夜盘的偏移日期都是下一日
	 */
	// Checked proleptic Gregorian date mapping. On failure result is unchanged.
	bool tryGetOffsetDate(uint32_t uDate, uint32_t uTime, uint32_t& result) const
	{
		if (uDate == 0)
		{
			uint32_t clockDate = 0, clockTime = 0;
			if (!TimeUtils::tryGetDateTime(clockDate, clockTime))
				return false;
			uDate = clockDate;
			uTime = clockTime / 100000;
		}
		const int64_t year = uDate / 10000;
		const unsigned month = (uDate / 100) % 100;
		const unsigned day = uDate % 100;
		if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1 ||
			uTime > 2400 || uTime % 100 >= 60 || (uTime / 100 == 24 && uTime != 2400))
			return false;
		static const unsigned monthDays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
		const unsigned maxDay = monthDays[month - 1] +
			(month == 2 && (year % 400 == 0 || (year % 4 == 0 && year % 100 != 0)) ? 1 : 0);
		if (day > maxDay)
			return false;
		const int64_t minute = static_cast<int64_t>(uTime / 100) * 60 + uTime % 100 + m_uOffsetMins;
		int64_t shift = minute / 1440;
		if (minute % 1440 < 0) --shift;
		int64_t y = year - (month <= 2 ? 1 : 0);
		const int64_t era = (y >= 0 ? y : y - 399) / 400;
		const unsigned yoe = static_cast<unsigned>(y - era * 400);
		const unsigned mp = month > 2 ? month - 3 : month + 9;
		const unsigned doy = (153 * mp + 2) / 5 + day - 1;
		const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
		int64_t z = era * 146097 + static_cast<int64_t>(doe) + shift;
		const int64_t destEra = (z >= 0 ? z : z - 146096) / 146097;
		const unsigned destDoe = static_cast<unsigned>(z - destEra * 146097);
		const unsigned destYoe = (destDoe - destDoe / 1460 + destDoe / 36524 - destDoe / 146096) / 365;
		y = static_cast<int64_t>(destYoe) + destEra * 400;
		const unsigned destDoy = destDoe - (365 * destYoe + destYoe / 4 - destYoe / 100);
		const unsigned destMp = (5 * destDoy + 2) / 153;
		const unsigned destDay = destDoy - (153 * destMp + 2) / 5 + 1;
		const unsigned destMonth = destMp < 10 ? destMp + 3 : destMp - 9;
		y += destMonth <= 2 ? 1 : 0;
		if (y < 1 || y > 9999)
			return false;
		result = static_cast<uint32_t>(y * 10000 + destMonth * 100 + destDay);
		return true;
	}

	uint32_t getOffsetDate(uint32_t uDate = 0, uint32_t uTime = 0)
	{
		uint32_t checkedDate = 0;
		if (tryGetOffsetDate(uDate, uTime, checkedDate))
			return checkedDate;
		// Preserve the legacy fallback until its callers gain a checked failure path.
		if(uDate == 0)
		{
			TimeUtils::getDateTime(uDate, uTime);
			uTime /= 100000;
		}

		int64_t curMinute = static_cast<int64_t>(uTime / 100) * 60 + uTime % 100;
		curMinute += m_uOffsetMins;

		if (curMinute >= 1440)
			return TimeUtils::getNextDate(uDate);

		if (curMinute < 0)
			return TimeUtils::getNextDate(uDate, -1);

		return uDate;
	}

	/*
	 *	将时间转换成分钟数
	 *	@uTime	当前时间,格式如0910
	 *	@autoAdjust	是否自动调整,如果开启,非交易时间内的行情,会自动对齐到下一个交易时间,如8点59分的行情,会自动算作9点00的行情
	 *				会不会有别的影响,暂时无法确定,主要是担心非交易时间里收到错误数据
	 *				但是有接收时间控制,应该没问题
	 */
	uint32_t timeToMinutes(uint32_t uTime, bool autoAdjust = false)
	{
		if(m_tradingTimes.empty())
			return INVALID_UINT32;

		if(isInAuctionTime(uTime))
			return 0;

		uint32_t offTime = offsetTime(uTime, true);
		uint32_t totalOffset = 0;
		uint32_t autoAdjustOffset = 0;
		bool hasAutoAdjustOffset = false;
		for (const TradingSection& section : m_tradingTimes)
		{
			if (section.first_raw == 0 && section.second_raw == 0)
				return totalOffset + (uTime / 100) * 60 + uTime % 100;
			if (section.first > section.second)
			{
				const uint32_t start = section.first / 100 * 60 + section.first % 100;
				const uint32_t end = section.second / 100 * 60 + section.second % 100;
				const uint32_t current = offTime / 100 * 60 + offTime % 100;
				const uint32_t length = end + 1440 - start;
				const uint32_t elapsed = (current + 1440 - start) % 1440;
				if (elapsed <= length) return totalOffset + elapsed;
				totalOffset += length;
				continue;
			}
			if (section.first <= offTime && offTime <= section.second)
			{
				int32_t hour = offTime / 100 - section.first / 100;
				int32_t minute = offTime % 100 - section.first % 100;
				return totalOffset + hour * 60 + minute;
			}

			if (autoAdjust && !hasAutoAdjustOffset && offTime <= section.second)
			{
				// Keep the legacy first auto-adjust point while continuing to
				// inspect later sections for an offset-time match.
				hasAutoAdjustOffset = true;
				autoAdjustOffset = totalOffset;
			}

			int32_t hour = section.second / 100 - section.first / 100;
			int32_t minute = section.second % 100 - section.first % 100;
			totalOffset += hour * 60 + minute;
		}

		if (autoAdjust && hasAutoAdjustOffset)
			return autoAdjustOffset;

		return INVALID_UINT32;
	}

	uint32_t minuteToTime(uint32_t uMinutes, bool bHeadFirst = false)
	{
		if(m_tradingTimes.empty())
			return INVALID_UINT32;

		uint32_t offset = uMinutes;
		TradingTimes::iterator it = m_tradingTimes.begin();
		for(; it != m_tradingTimes.end(); it++)
		{
			TradingSection &section = *it;
			if (section.first_raw == 0 && section.second_raw == 0)
			{
				if (offset < 1440)
					return (offset / 60) * 100 + offset % 60;
				if (offset == 1440 && !bHeadFirst)
					return 2400;
				offset -= 1440;
				continue;
			}
			uint32_t startMin = section.first/100*60 + section.first%100;
			uint32_t stopMin = section.second/100*60 + section.second%100;
			if (stopMin < startMin) stopMin += 1440;

			if(!bHeadFirst)
			{
				if (startMin + offset >= stopMin)
				{
					offset -= (stopMin - startMin);
					if (offset == 0)
					{
						const uint32_t end = stopMin > 1440 ? stopMin - 1440 : stopMin;
						return originalTime(end / 60 * 100 + end % 60);
					}
				}
				else
				{
					//干好位于该区间
					uint32_t desMin = startMin + offset;
					if (desMin >= 1440)
						desMin -= 1440;

					return originalTime(desMin / 60 * 100 + desMin % 60);
				}
			}
			else
			{
				if (startMin + offset < stopMin)
				{
					//干好位于该区间
					uint32_t desMin = startMin + offset;
					if (desMin >= 1440)
						desMin -= 1440;

					return originalTime(desMin / 60 * 100 + desMin % 60);
				}
				else
				{
					offset -= (stopMin - startMin);
				}
			}
		}

		return getCloseTime();
	}

	uint32_t timeToSeconds(uint32_t uTime)
	{
		if(m_tradingTimes.empty())
			return INVALID_UINT32;

		//如果是集合竞价的价格,则认为是0秒价格
		if(isInAuctionTime(uTime/100))
			return 0;

		uint32_t sec = uTime%100;
		uint32_t h = uTime/10000;
		uint32_t m = uTime%10000/100;
		uint32_t offMin = offsetTime(h*100 + m, true);
		h = offMin/100;
		m = offMin%100;
		uint32_t seconds = h*60*60 + m*60 + sec;

		uint32_t offset = 0;
		bool bFound = false;
		TradingTimes::iterator it = m_tradingTimes.begin();
		for(; it != m_tradingTimes.end(); it++)
		{
			TradingSection &section = *it;
			if (section.first_raw == 0 && section.second_raw == 0)
			{
				uint32_t rawSeconds = (uTime / 10000 * 60 + uTime / 100 % 100) * 60 + uTime % 100;
				return offset + (rawSeconds == 86400 ? 86399 : rawSeconds);
			}
			uint32_t startSecs = (section.first/100*60 + section.first%100)*60;
			uint32_t stopSecs = (section.second/100*60 + section.second%100)*60;
			if (stopSecs < startSecs)
			{
				const uint32_t length = stopSecs + 86400 - startSecs;
				const uint32_t elapsed = (seconds + 86400 - startSecs) % 86400;
				if (elapsed <= length)
					return offset + elapsed - (elapsed == length ? 1 : 0);
				offset += length;
				continue;
			}
			//uint32_t s = section.first;
			//uint32_t e = section.second;
			//uint32_t hour = (e/100 - s/100);
			//uint32_t minute = (e%100 - s%100);
			if(startSecs <= seconds && seconds <= stopSecs)
			{
				offset += seconds-startSecs;
				if(seconds == stopSecs)
					offset--;
				bFound = true;
				break;
			}
			else
			{
				offset += stopSecs - startSecs;
			}
		}

		//没找到就返回0
		if(!bFound)
			return INVALID_UINT32;

		return offset;
	}

	uint32_t secondsToTime(uint32_t seconds)
	{
		if(m_tradingTimes.empty())
			return INVALID_UINT32;

		uint32_t offset = seconds;
		TradingTimes::iterator it = m_tradingTimes.begin();
		for(; it != m_tradingTimes.end(); it++)
		{
			TradingSection &section = *it;
			if (section.first_raw == 0 && section.second_raw == 0)
			{
				if (offset <= 86400)
				{
					uint32_t mins = offset / 60;
					return ((mins / 60) * 100 + mins % 60) * 100 + offset % 60;
				}
				offset -= 86400;
				continue;
			}
			uint32_t startSecs = (section.first/100*60 + section.first%100)*60;
			uint32_t stopSecs = (section.second/100*60 + section.second%100)*60;
			if (stopSecs < startSecs) stopSecs += 86400;

			if(startSecs + offset >= stopSecs)
			{
				offset -= (stopSecs-startSecs);
				if(offset == 0)
				{
					uint32_t end = stopSecs > 86400 ? stopSecs - 86400 : stopSecs;
					uint32_t desMin = end/60;
					return originalTime((desMin/60*100 + desMin%60))*100 + stopSecs%60;
				}
			}
			else
			{
				//干好位于该区间
				uint32_t desSecs = startSecs+offset;
				if(desSecs >= 86400)
					desSecs -= 86400;

				uint32_t desMin = desSecs/60;
				return originalTime((desMin/60*100 + desMin%60))*100 + desSecs%60;
			}
		}

		return INVALID_UINT32;
	}

	inline uint32_t getOpenTime(bool bOffseted = false) const
	{
		if(m_tradingTimes.empty())
			return 0;

		return bOffseted ? m_tradingTimes[0].first : m_tradingTimes[0].first_raw;
	}

	inline uint32_t getAuctionStartTime(bool bOffseted = false) const
	{
		if (m_auctionTimes.empty())
			return -1;

		return bOffseted?m_auctionTimes[0].first: m_auctionTimes[0].first_raw;
	}

	inline uint32_t getCloseTime(bool bOffseted = false) const
	{
		if(m_tradingTimes.empty())
			return 0;

		uint32_t ret = bOffseted ? m_tradingTimes[m_tradingTimes.size() - 1].second : m_tradingTimes[m_tradingTimes.size() - 1].second_raw;

		// By Wesley @ 2021.12.25
		// 如果收盘时间是0点，无法跟开盘时间进行比较，所以这里要做一个修正
		if (ret == 0 && bOffseted)
			ret = 2400;

		return ret;
	}

	inline uint32_t getTradingSeconds()
	{
		uint32_t count = 0;
		TradingTimes::iterator it = m_tradingTimes.begin();
		for(; it != m_tradingTimes.end(); it++)
		{
			TradingSection &section = *it;
			if (section.first_raw == 0 && section.second_raw == 0)
			{
				count += 1440;
				continue;
			}
			uint32_t s = section.first;
			uint32_t e = section.second;
			if (e < s)
			{
				count += 1440 - (s / 100 * 60 + s % 100) + (e / 100 * 60 + e % 100);
				continue;
			}

			uint32_t hour = (e/100 - s/100);
			uint32_t minute = (e%100 - s%100);
			count += hour*60+minute;
		}

		//By Welsey @ 2021.12.25
		//这种只能是全天候交易时段
		if (count == 0) count = 1440;
		return count*60;
	}

	/*
	 *	获取交易的分钟数
	 */
	inline uint32_t getTradingMins()
	{
		uint32_t count = 0;
		TradingTimes::iterator it = m_tradingTimes.begin();
		for (; it != m_tradingTimes.end(); it++)
		{
			TradingSection &section = *it;
			if (section.first_raw == 0 && section.second_raw == 0)
			{
				count += 1440;
				continue;
			}
			uint32_t s = section.first;
			uint32_t e = section.second;
			if (e < s)
			{
				count += 1440 - (s / 100 * 60 + s % 100) + (e / 100 * 60 + e % 100);
				continue;
			}

			uint32_t hour = (e / 100 - s / 100);
			uint32_t minute = (e % 100 - s % 100);
			count += hour * 60 + minute;
		}
		//By Welsey @ 2021.12.25
		//这种只能是全天候交易时段
		if (count == 0) count = 1440;
		return count;
	}

	/*
	 *	获取小节分钟数列表
	 */
	inline const std::vector<uint32_t>& getSecMinList()
	{
		if(_minutes.empty())
		{
			uint32_t total = 0;
			TradingTimes::iterator it = m_tradingTimes.begin();
			for (; it != m_tradingTimes.end(); it++)
			{
				TradingSection &section = *it;
				// A single raw 00:00–00:00 section represents a full day.
				// Nonzero offsets can map both endpoints to the same clock time.
				if (section.first_raw == 0 && section.second_raw == 0)
				{
					total += 1440;
					_minutes.emplace_back(total);
					continue;
				}
				uint32_t s = section.first;
				uint32_t e = section.second;
				if (e < s)
				{
					total += 1440 - (s / 100 * 60 + s % 100) + (e / 100 * 60 + e % 100);
					_minutes.emplace_back(total);
					continue;
				}

				uint32_t hour = (e / 100 - s / 100);
				uint32_t minute = (e % 100 - s % 100);

				total += hour * 60 + minute;
				_minutes.emplace_back(total);
			}
			
			if (_minutes.empty())
				_minutes.emplace_back(1440);
		}
		
		return _minutes;
	}

	/*
	 *	是否处于交易时间
	 *	@uTime		时间，格式为hhmm
	 *	@bStrict	是否严格检查，如果是严格检查
	 *				则在每一交易时段最后一分钟，如1500，不属于交易时间
	 */
	bool	isInTradingTime(uint32_t uTime, bool bStrict = false)
	{
		if (!isValidSessionTime(uTime))
			return false;
		const uint32_t offTime = offsetTime(uTime, true);
		bool inTradingSection = false;
		for (const TradingSection& section : m_tradingTimes)
		{
			if ((section.first_raw == 0 && section.second_raw == 0) ||
				(section.first <= section.second ?
				 (section.first <= offTime && offTime <= section.second) :
				 (offTime >= section.first || offTime <= section.second)))
			{
				inTradingSection = true;
				break;
			}
		}

		if (!inTradingSection)
			return false;

		if (bStrict && isLastOfSection(uTime))
			return false;

		return true;
	}

	inline bool	isLastOfSection(uint32_t uTime)
	{
		//uint32_t offTime = offsetTime(uTime, false);
		TradingTimes::iterator it = m_tradingTimes.begin();
		for(; it != m_tradingTimes.end(); it++)
		{
			TradingSection &section = *it;
			uint32_t endTime = section.second_raw % 2400;
			if (uTime == section.second_raw || uTime == endTime)
				return true;
		}

		return false;
	}

	inline bool	isFirstOfSection(uint32_t uTime)
	{
		//uint32_t offTime = offsetTime(uTime, true);
		TradingTimes::iterator it = m_tradingTimes.begin();
		for(; it != m_tradingTimes.end(); it++)
		{
			TradingSection &section = *it;
			if(section.first_raw == uTime)
				return true;
		}

		return false;
	}

	inline bool	isInAuctionTime(uint32_t uTime)
	{
		if (!isValidSessionTime(uTime))
			return false;
		for(const TradingSection& aucSec : m_auctionTimes)
		{
			if (aucSec.first_raw == 0 && aucSec.second_raw == 2400)
				return true;
			if (aucSec.first_raw == aucSec.second_raw)
				continue;
			const uint32_t time = uTime % 2400;
			const uint32_t start = aucSec.first_raw % 2400;
			const uint32_t end = aucSec.second_raw % 2400;
			if (start < end ? (start <= time && time < end) :
				(start > end && (time >= start || time < end)))
				return true;
		}
		

		return false;
	}

	/*
	 *	计算偏移时间
	 *	@uTime		原始时间
	 *	@bAlignLeft	是否向左对齐，这个主要针对0点结束的情况
	 *				如果向左对齐，则0点就做0点算
	 *				如果向右对齐，则0点就做24点算
	 */
	inline uint32_t offsetTime(uint32_t uTime, bool bAlignLeft) const
	{
		int64_t value = (static_cast<int64_t>(uTime / 100) * 60 + uTime % 100 + m_uOffsetMins) % 1440;
		if (value < 0) value += 1440;
		if (!bAlignLeft && value == 0) return 2400;
		return static_cast<uint32_t>((value / 60) * 100 + value % 60);
	}

	inline uint32_t originalTime(uint32_t uTime) const
	{
		if (m_uOffsetMins == 0) return uTime;
		int64_t value = (static_cast<int64_t>(uTime / 100) * 60 + uTime % 100 - m_uOffsetMins) % 1440;
		if (value < 0) value += 1440;
		return static_cast<uint32_t>((value / 60) * 100 + value % 60);
	}

};

NS_WTP_END
