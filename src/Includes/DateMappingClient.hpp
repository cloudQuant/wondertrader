#pragma once
// Typed date-mapping access for callers (decision card 1, A+C).
//
// A client is built either from a DateMappingV1 table obtained explicitly
// from a concrete WTSBaseDataMgr (path A), or from an arbitrary IBaseDataMgr
// whose only date entries are the two legacy virtuals (path C). Callers in
// plugins hold only IBaseDataMgr*; they must not cast it to WTSBaseDataMgr,
// because the object may come from another module or an older build.
//
// Path C validates every input before calling a legacy virtual and validates
// the returned value afterwards. The legacy virtuals only promise results for
// legal input: a non-empty id that resolves to a commodity (or, for session
// mode, a session) with at least one trading section and a non-empty holiday
// template name, a valid Gregorian YYYYMMDD date in 1000..9999 and a valid
// HHMM time. IBaseDataMgr has no way to report whether the named holiday
// calendar is loaded, so path C cannot detect a missing calendar; path A
// reports MissingCalendar.
//
// date=0 means "the manager's current date". Path A takes ONE checked local
// clock snapshot the first time it is needed and reuses it for every later
// date=0 request of the same client (for calcTradingDate the snapshot time also
// replaces the time argument, as in the legacy contract). Path C keeps the
// legacy meaning of 0 and passes it to the manager, whose notion of "current"
// may differ from the wall clock (simulated/replay managers); the client only
// requires that a checked clock snapshot can be taken first and validates the
// returned value. A clock failure is ClockFailure on both paths.
//
// Every failure has value=0; callers must check status before using value.
// The client is not thread-safe; create one per operation. The table/manager
// must outlive the client and keep its external synchronization contract.
#include "DateMappingV1.h"
#include "IBaseDataMgr.h"
#include "WTSContractInfo.hpp"
#include "WTSSessionInfo.hpp"
#include "../Share/TimeUtils.hpp"

NS_WTP_BEGIN

inline const char* dateMappingStatusName(DateMappingStatus status) noexcept
{
	switch (status)
	{
	case DateMappingStatus::Ok: return "Ok";
	case DateMappingStatus::InvalidArgument: return "InvalidArgument";
	case DateMappingStatus::InvalidDate: return "InvalidDate";
	case DateMappingStatus::InvalidTime: return "InvalidTime";
	case DateMappingStatus::MissingCommodity: return "MissingCommodity";
	case DateMappingStatus::MissingSession: return "MissingSession";
	case DateMappingStatus::MissingCalendar: return "MissingCalendar";
	case DateMappingStatus::ClockFailure: return "ClockFailure";
	case DateMappingStatus::OutOfRange: return "OutOfRange";
	case DateMappingStatus::Unsupported: return "Unsupported";
	case DateMappingStatus::InternalError: return "InternalError";
	}
	return "Unknown";
}

// Status of an explicit YYYYMMDD date under the V1 range rules.
inline DateMappingStatus checkedDateStatus(uint32_t date) noexcept
{
	const uint32_t year = date / 10000;
	if (year < 1000 || year > 9999)
		return DateMappingStatus::OutOfRange;
	uint32_t weekDay = 0;
	return TimeUtils::tryGetWeekDay(date, weekDay) ? DateMappingStatus::Ok : DateMappingStatus::InvalidDate;
}

class DateMappingClient
{
public:
	explicit DateMappingClient(const DateMappingV1* table) noexcept : _legacy(nullptr)
	{
		if (table != nullptr && table->size == sizeof(DateMappingV1) && table->version == 1 &&
			table->context != nullptr && table->calcTradingDate != nullptr && table->getBoundaryTime != nullptr)
		{
			_table = *table;
			_hasTable = true;
		}
	}

	explicit DateMappingClient(IBaseDataMgr* legacy) noexcept : _legacy(legacy) {}

	DateMappingClient(const DateMappingClient&) = delete;
	DateMappingClient& operator=(const DateMappingClient&) = delete;

	// Resolve the shared "now" snapshot (HHMM time) for this client.
	DateMappingStatus now(uint32_t& date, uint32_t& hhmm) noexcept
	{
		if (!_hasSnapshot)
		{
			uint32_t clockDate = 0, clockTime = 0;
			if (!TimeUtils::tryGetDateTime(clockDate, clockTime))
				return DateMappingStatus::ClockFailure;
			const DateMappingStatus status = checkedDateStatus(clockDate);
			if (status != DateMappingStatus::Ok)
				return status;
			_snapDate = clockDate;
			_snapTime = clockTime / 100000;
			_hasSnapshot = true;
		}
		date = _snapDate;
		hhmm = _snapTime;
		return DateMappingStatus::Ok;
	}

	DateMappingResult32 calcTradingDate(const char* id, uint32_t date, uint32_t time, bool isSession = false) noexcept
	{
		if (id == nullptr || *id == '\0')
			return fail32(DateMappingStatus::InvalidArgument);
		if (!WTSSessionInfo::isValidSessionTime(time))
			return fail32(DateMappingStatus::InvalidTime);
		uint32_t snapDate = 0, snapTime = 0;
		DateMappingStatus status = date == 0 ? now(snapDate, snapTime) : checkedDateStatus(date);
		if (status != DateMappingStatus::Ok)
			return fail32(status);

		if (_hasTable)
			return _table.calcTradingDate(&_table, id, date == 0 ? snapDate : date,
				date == 0 ? snapTime : time, isSession ? 1 : 0);
		if ((status = legacyContext(id, isSession)) != DateMappingStatus::Ok)
			return fail32(status);
		uint32_t value = 0;
		try { value = _legacy->calcTradingDate(id, date, time, isSession); }
		catch (...) { return fail32(DateMappingStatus::InternalError); }
		if (checkedDateStatus(value) != DateMappingStatus::Ok)
			return fail32(DateMappingStatus::InternalError);
		return { DateMappingStatus::Ok, value };
	}

	DateMappingResult64 getBoundaryTime(const char* id, uint32_t date, bool isSession = false, bool isStart = true) noexcept
	{
		if (id == nullptr || *id == '\0')
			return fail64(DateMappingStatus::InvalidArgument);
		uint32_t snapDate = 0, snapTime = 0;
		DateMappingStatus status = date == 0 ? now(snapDate, snapTime) : checkedDateStatus(date);
		if (status != DateMappingStatus::Ok)
			return fail64(status);

		if (_hasTable)
			return _table.getBoundaryTime(&_table, id, date == 0 ? snapDate : date, isSession ? 1 : 0, isStart ? 1 : 0);
		if ((status = legacyContext(id, isSession)) != DateMappingStatus::Ok)
			return fail64(status);
		uint64_t value = 0;
		try { value = _legacy->getBoundaryTime(id, date, isSession, isStart); }
		catch (...) { return fail64(DateMappingStatus::InternalError); }
		const uint64_t boundaryDate = value / 10000;
		const uint32_t boundaryTime = static_cast<uint32_t>(value % 10000);
		if (boundaryDate > UINT32_MAX || checkedDateStatus(static_cast<uint32_t>(boundaryDate)) != DateMappingStatus::Ok ||
			!WTSSessionInfo::isValidSessionTime(boundaryTime))
			return fail64(DateMappingStatus::InternalError);
		return { DateMappingStatus::Ok, value };
	}

private:
	static DateMappingResult32 fail32(DateMappingStatus status) noexcept { return { status, 0 }; }
	static DateMappingResult64 fail64(DateMappingStatus status) noexcept { return { status, 0 }; }

	DateMappingStatus legacyContext(const char* id, bool isSession) noexcept
	{
		if (_legacy == nullptr)
			return DateMappingStatus::Unsupported;
		try
		{
			WTSSessionInfo* session = nullptr;
			if (isSession)
			{
				session = _legacy->getSession(id);
			}
			else
			{
				WTSCommodityInfo* commodity = _legacy->getCommodity(id);
				if (commodity == nullptr)
					return DateMappingStatus::MissingCommodity;
				const char* calendar = commodity->getTradingTpl();
				if (calendar == nullptr || *calendar == '\0')
					return DateMappingStatus::MissingCalendar;
				session = commodity->getSessionInfo();
			}
			if (session == nullptr || session->getSectionCount() == 0)
				return DateMappingStatus::MissingSession;
			return DateMappingStatus::Ok;
		}
		catch (...)
		{
			return DateMappingStatus::InternalError;
		}
	}

	DateMappingV1	_table{};
	bool			_hasTable = false;
	IBaseDataMgr*	_legacy;
	bool			_hasSnapshot = false;
	uint32_t		_snapDate = 0;
	uint32_t		_snapTime = 0;
};

NS_WTP_END
