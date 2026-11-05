#pragma once
#include <stdint.h>

// Date mapping V1 contract:
//
// Obtain the table explicitly from a concrete WTSBaseDataMgr; there is no
// automatic negotiation or binding for arbitrary IBaseDataMgr implementations.
// The manager must remain alive during every call. Configuration and cache
// access retain the manager's existing external synchronization requirements.
// Pass the table itself as the first argument, leave context unchanged, and
// check status on every result. A failure has value=0, but value is not a
// substitute for status. A size/version mismatch returns Unsupported before
// context is used. This interface does not establish external plugin ABI
// compatibility.
//
// Dates are Gregorian YYYYMMDD, in the inclusive range 1000-01-01 through
// 9999-12-31. Times are HHMM: 0000..2359 with minutes below 60, plus 2400 as
// the endpoint label. For calcTradingDate, date=0 takes one checked local
// date/time snapshot and replaces the time argument, which must still be valid.
// For getBoundaryTime, date=0 takes the date from one checked local snapshot.
// For getTradingDate, date=0 returns a valid cached date if present, otherwise
// takes the date from one checked local snapshot; its time argument is checked
// but otherwise ignored for compatibility. Next/previous require an explicit
// valid date and a positive count.
//
// The referenced holiday calendar must be loaded, even for a 7x24 session.
// As in the legacy mapping, a weekday holiday is not itself advanced by
// calcTradingDate or getTradingDate; weekend and requested next/previous
// transitions skip weekends and loaded holidays. Empty sessions cannot supply
// a mapping. Session flags select a session ID and the default calendar for
// calc/boundary; template flags select a calendar ID for get/next/previous.
namespace wtp {
enum class DateMappingStatus : uint32_t {
    Ok = 0, InvalidArgument = 1, InvalidDate = 2, InvalidTime = 3,
    MissingCommodity = 4, MissingSession = 5, MissingCalendar = 6,
    ClockFailure = 7, OutOfRange = 8, Unsupported = 9, InternalError = 10
};
struct DateMappingResult32 { DateMappingStatus status; uint32_t value; };
struct DateMappingResult64 { DateMappingStatus status; uint64_t value; };
struct DateMappingV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    DateMappingResult32 (*calcTradingDate)(const DateMappingV1*, const char*, uint32_t, uint32_t, uint8_t) noexcept;
    DateMappingResult64 (*getBoundaryTime)(const DateMappingV1*, const char*, uint32_t, uint8_t, uint8_t) noexcept;
    DateMappingResult32 (*getTradingDate)(const DateMappingV1*, const char*, uint32_t, uint32_t, uint8_t) noexcept;
    DateMappingResult32 (*nextTradingDate)(const DateMappingV1*, const char*, uint32_t, uint32_t, uint8_t) noexcept;
    DateMappingResult32 (*prevTradingDate)(const DateMappingV1*, const char*, uint32_t, uint32_t, uint8_t) noexcept;
};
// A failure always has value=0; callers must inspect status, not value.
}
