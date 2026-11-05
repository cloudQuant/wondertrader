/*!
 * \file ActionDayNormalizer.hpp
 * \project	WonderTrader
 *
 * \brief 期货 parser 行情发生日（ActionDay）夜盘纠偏（迭代03 卡3 3b=C，默认 A；2026-11-04 用户裁定）
 *
 * 规则（仅当行情 ActionDay == TradingDay 时适用，此时夜盘的发生日不可信）：
 * - 本地时间 03:00–08:59：视为早盘启动后收到的隔夜残留行情，丢弃（保留原过滤语义）。
 * - 行情 20–23 时：以本地日期为发生日；行情 23 时而本地 00 时（行情慢于本地）取本地前一天。
 * - 行情 00 时（如 00:30）：
 *     模式 A（默认，NextLocalDay）：以本地日期为发生日；本地 23 时（本地慢于行情）取本地次日。
 *     模式 B（Legacy）：保持旧实现，00 时不进入纠偏分支，直接保留 ActionDay。
 * 日期推进是自然日（TimeUtils::getNextDate），不跳周末/节假日：周五夜盘 00:30 的发生日是周六。
 *
 * 真实 SDK（CTP/CTPMini/Femas）的 ActionDay/TradingDay 语义未经精确 SDK 证据验证（外部 X）。
 */
#pragma once
#include <stdint.h>
#include <cstring>
#include "TimeUtils.hpp"

namespace wt_actday
{
	enum class MidnightMode
	{
		NextLocalDay = 0,	//卡3 3b 默认 A
		Legacy = 1			//卡3 3b 选项 B：保持旧行为
	};

	//只有进入纠偏分支的时段才需要读本地时钟（避免日盘每笔行情一次系统调用）
	inline bool needsLocalClock(uint32_t actHour, MidnightMode mode = MidnightMode::NextLocalDay)
	{
		return actHour >= 20 || (mode == MidnightMode::NextLocalDay && actHour == 0);
	}

	/*
	 *	@actDate	输入为行情 ActionDay，输出为纠偏后的发生日
	 *	@return		false 表示该行情应丢弃
	 */
	inline bool normalize(uint32_t& actDate, uint32_t actHour, uint32_t tradingDate,
		uint32_t curDate, uint32_t curHour, MidnightMode mode = MidnightMode::NextLocalDay)
	{
		if (actDate != tradingDate)
			return true;

		if (!needsLocalClock(actHour, mode))
			return true;

		//早上启动以后,会收到昨晚收盘前的行情,这个时候可能会有发生日期=交易日的情况出现,这笔数据直接丢掉
		if (curHour >= 3 && curHour < 9)
			return false;

		actDate = curDate;
		if (actHour == 23 && curHour == 0)
			actDate = TimeUtils::getNextDate(curDate, -1);	//行情时间慢于系统时间
		else if (actHour == 0 && curHour == 23)
			actDate = TimeUtils::getNextDate(curDate, 1);	//系统时间慢于行情时间
		return true;
	}

	inline MidnightMode parseMode(const char* value)
	{
		if (value != NULL && (std::strcmp(value, "legacy") == 0 || std::strcmp(value, "B") == 0 || std::strcmp(value, "keep") == 0))
			return MidnightMode::Legacy;
		return MidnightMode::NextLocalDay;
	}

#ifdef WT_ACTDAY_TEST_CLOCK
	// 仅测试构建：由测试 TU 提供可控本地时钟
	void testLocalDateTime(uint32_t& date, uint32_t& time);
#endif

	inline void localDateTime(uint32_t& date, uint32_t& time)
	{
#ifdef WT_ACTDAY_TEST_CLOCK
		testLocalDateTime(date, time);
#else
		TimeUtils::getDateTime(date, time);
#endif
	}
}
