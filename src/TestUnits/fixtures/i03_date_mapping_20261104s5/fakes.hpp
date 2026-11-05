#pragma once
// Local fakes for the iteration-03 S5 date-mapping fixture. No vendor API,
// network or real storage is used.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "Includes/IBaseDataMgr.h"
#include "Includes/IHotMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSSessionInfo.hpp"

// Calling-thread clock interposer: when armed, CLOCK_REALTIME either fails or
// returns the scripted seconds (one entry per call, last entry repeats).
struct FakeClock
{
	static thread_local bool armed;
	static thread_local bool fail;
	static thread_local int calls;
	static thread_local std::vector<long> seconds;
	static void reset() { armed = false; fail = false; calls = 0; seconds.clear(); }
};
inline thread_local bool FakeClock::armed = false;
inline thread_local bool FakeClock::fail = false;
inline thread_local int FakeClock::calls = 0;
inline thread_local std::vector<long> FakeClock::seconds;

// Include this header in exactly one translation unit of an executable.
extern "C" int clock_gettime(clockid_t clock, struct timespec* result)
{
	if (FakeClock::armed && clock == CLOCK_REALTIME)
	{
		const int index = FakeClock::calls++;
		if (FakeClock::fail)
			return -1;
		if (!FakeClock::seconds.empty())
		{
			const std::size_t i = static_cast<std::size_t>(index) < FakeClock::seconds.size()
				? static_cast<std::size_t>(index) : FakeClock::seconds.size() - 1;
			result->tv_sec = FakeClock::seconds[i];
			result->tv_nsec = 0;
			return 0;
		}
	}
	using Fn = int (*)(clockid_t, struct timespec*);
	static Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "clock_gettime"));
	return real(clock, result);
}

// Legacy IBaseDataMgr whose two date virtuals mimic WTSBaseDataMgr's guarded
// legacy behaviour for valid input (identity on weekdays), answer date 0 with
// the manager's own current date (as a simulated/replay manager may) and
// record calls.
class FakeLegacyMgr : public wtp::IBaseDataMgr
{
public:
	struct Call { std::string id; uint32_t date; uint32_t time; bool isSession; };

	FakeLegacyMgr(const char* calendar = "CHINA", bool withSession = true, uint32_t sections = 1)
	{
		_session = wtp::WTSSessionInfo::create("DAY", "day", 0);
		if (sections > 0)
			_session->addTradingSection(900, 1500);
		_comm = wtp::WTSCommodityInfo::create("P", "product", "EX", "DAY", calendar);
		if (withSession)
			_comm->setSessionInfo(_session);
	}
	~FakeLegacyMgr() { _comm->release(); _session->release(); }

	bool hasCommodity = true;
	uint32_t currentDate = 20240227;
	bool throwOnCalc = false;
	bool overrideCalc = false;
	uint32_t calcValue = 0;
	bool overrideBoundary = false;
	uint64_t boundaryValue = 0;
	std::vector<Call> calcCalls;
	std::vector<Call> boundaryCalls;

	wtp::WTSCommodityInfo* getCommodity(const char*) override { return hasCommodity ? _comm : nullptr; }
	wtp::WTSCommodityInfo* getCommodity(const char*, const char*) override { return hasCommodity ? _comm : nullptr; }
	wtp::WTSContractInfo* getContract(const char*, const char* = "", uint32_t = 0) override { return nullptr; }
	wtp::WTSArray* getContracts(const char* = "", uint32_t = 0) override { return nullptr; }
	wtp::WTSSessionInfo* getSession(const char* sid) override
	{ return sid != nullptr && std::strcmp(sid, "DAY") == 0 ? _session : nullptr; }
	wtp::WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return _session; }
	wtp::WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char* id, uint32_t date, uint32_t time, bool isSession = false) override
	{
		calcCalls.push_back({ id == nullptr ? "" : id, date, time, isSession });
		if (throwOnCalc)
			throw std::runtime_error("injected calcTradingDate failure");
		if (overrideCalc)
			return calcValue;
		if (date == 0)
			return currentDate;
		uint32_t weekDay = 0;
		// The current manager returns 0 for invalid explicit input.
		if (!wtp::WTSSessionInfo::isValidSessionTime(time) || (date != 0 && !TimeUtils::tryGetWeekDay(date, weekDay)))
			return 0;
		return date;
	}
	uint64_t getBoundaryTime(const char* id, uint32_t date, bool isSession = false, bool isStart = true) override
	{
		boundaryCalls.push_back({ id == nullptr ? "" : id, date, 0, isSession });
		if (overrideBoundary)
			return boundaryValue;
		return static_cast<uint64_t>(date == 0 ? currentDate : date) * 10000 + (isStart ? 900 : 1500);
	}

private:
	wtp::WTSSessionInfo* _session = nullptr;
	wtp::WTSCommodityInfo* _comm = nullptr;
};

class FakeHotMgr : public wtp::IHotMgr
{
public:
	const char* getRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isHot(const char*, const char*, uint32_t) override { return false; }
	bool splitHotSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isSecond(const char*, const char*, uint32_t) override { return false; }
	bool splitSecondSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getCustomRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevCustomRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isCustomHot(const char*, const char*, uint32_t) override { return false; }
	bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getRuleTag(const char*) override { return ""; }
	double getRuleFactor(const char*, const char*, uint32_t) override { return 1.0; }
};
