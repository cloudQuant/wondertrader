/*!
 * \file WTSBaseDataMgr.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "WTSBaseDataMgr.h"
#include "../WTSUtils/WTSCfgLoader.h"
#include "WTSLogger.h"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSMarcos.h"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <locale>
#include <memory>
#include <set>
#include <sstream>

namespace {
using wtp::DateMappingStatus;
using wtp::DateMappingResult32;
using wtp::DateMappingResult64;
DateMappingResult32 dateResult(DateMappingStatus status, uint32_t value = 0) noexcept
{ return {status, status == DateMappingStatus::Ok ? value : 0}; }
DateMappingResult64 boundaryResult(DateMappingStatus status, uint64_t value = 0) noexcept
{ return {status, status == DateMappingStatus::Ok ? value : 0}; }
DateMappingStatus dateStatus(uint32_t date) noexcept
{
    const uint32_t year = date / 10000;
    if (year < 1000 || year > 9999) return DateMappingStatus::OutOfRange;
    uint32_t weekday;
    return TimeUtils::tryGetWeekDay(date, weekday) ? DateMappingStatus::Ok : DateMappingStatus::InvalidDate;
}
DateMappingStatus currentDateTime(uint32_t& date, uint32_t& time) noexcept
{
    uint32_t clockTime;
    if (!TimeUtils::tryGetDateTime(date, clockTime)) return DateMappingStatus::ClockFailure;
    time = clockTime / 100000;
    return dateStatus(date);
}
uint32_t daysInMonth(uint32_t year, uint32_t month) noexcept
{
    static const uint32_t lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return lengths[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}
bool moveOneDate(uint32_t& date, int direction) noexcept
{
    uint32_t year = date / 10000, month = date / 100 % 100, day = date % 100;
    if (direction > 0) {
        if (++day > daysInMonth(year, month)) {
            day = 1;
            if (++month > 12) { month = 1; if (++year > 9999) return false; }
        }
    } else if (--day == 0) {
        if (--month == 0) { month = 12; if (--year < 1000) return false; }
        day = daysInMonth(year, month);
    }
    date = year * 10000 + month * 100 + day;
    return true;
}
DateMappingResult32 moveTradingDays(const TradingDayTpl& calendar, uint32_t date, uint32_t count, int direction)
{
    if (count == 0) return dateResult(DateMappingStatus::InvalidArgument);
    while (count != 0) {
        if (!moveOneDate(date, direction)) return dateResult(DateMappingStatus::OutOfRange);
        uint32_t weekday;
        if (!TimeUtils::tryGetWeekDay(date, weekday)) return dateResult(DateMappingStatus::InternalError);
        if (weekday != 0 && weekday != 6 && calendar._holidays.find(date) == calendar._holidays.end()) --count;
    }
    return dateResult(DateMappingStatus::Ok, date);
}
}

const char* DEFAULT_HOLIDAY_TPL = "CHINA";

WTSBaseDataMgr::WTSBaseDataMgr()
	: _session_map(NULL)
{
	_session_map = WTSStringMap::create();
	_code_map = WTSStringMap::create();
	_fullcode_map = WTSStringMap::create();

	_pid_map = WTSStringMap::create();
	_fullpid_map = WTSStringMap::create();

	_code_list = WTSArray::create();
	_comm_list = WTSArray::create();
}


WTSBaseDataMgr::~WTSBaseDataMgr()
{
	if (_code_map)
	{
		_code_map->release();
		_code_map = NULL;
	}

	if (_session_map)
	{
		_session_map->release();
		_session_map = NULL;
	}

	if (_fullpid_map)
	{
		_fullpid_map->release();
		_fullpid_map = NULL;
	}

	if(_pid_map)
	{
		_pid_map->release();
		_pid_map = NULL;
	}

	if(_fullcode_map)
	{
		_fullcode_map->release();
		_fullcode_map = NULL;
	}

	if (_code_list) _code_list->release();
	if (_comm_list) _comm_list->release();
}

WTSCommodityInfo* WTSBaseDataMgr::getCommodity(const char* fullPid)
{
	return static_cast<WTSCommodityInfo*>(_fullpid_map->get(fullPid));
}


WTSCommodityInfo* WTSBaseDataMgr::getCommodity(const char* exchg, const char* pid)
{
	if (_fullpid_map == NULL)
		return NULL;

	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}.{}", exchg, pid);

	return static_cast<WTSCommodityInfo*>(_fullpid_map->get(key));
}

WTSContractInfo* WTSBaseDataMgr::getContract(const char* code, const char* exchg /* = "" */, uint32_t uDate /* = 0 */)
{
	//如果直接找到对应的市场代码,则直接
	if (strlen(exchg) == 0)
	{
		auto it = _code_map->find(code);
		if (it == _code_map->end())
			return NULL;

		WTSArray* ayInst = (WTSArray*)it->second;
		if (ayInst == NULL || ayInst->size() == 0)
			return NULL;

		for(std::size_t i = 0; i < ayInst->size(); i++)
		{
			WTSContractInfo* cInfo = static_cast<WTSContractInfo*>(ayInst->at(i));
			/*
			 *	By Wesley @ 2023.10.23
			 *	if param uDate is not zero, need to check whether contract is valid
			 */
			if (uDate == 0 || (cInfo->getOpenDate() <= uDate && cInfo->getExpireDate() >= uDate))
				return cInfo;
		}
		return NULL;
	}
	else
	{
		thread_local static char key[64] = { 0 };
		fmtutil::format_to(key, "{}.{}", exchg, code);
		auto it = _fullcode_map->find(key);
		if (it != _fullcode_map->end())
		{
			WTSContractInfo* cInfo = static_cast<WTSContractInfo*>(it->second);
			/*
			 *	By Wesley @ 2023.10.23
			 *	if param uDate is not zero, need to check whether contract is valid
			 */
			if (uDate == 0 || (cInfo->getOpenDate() <= uDate && cInfo->getExpireDate() >= uDate))
				return cInfo;
		}
	}

	return NULL;
}

uint32_t  WTSBaseDataMgr::getContractSize(const char* exchg /* = "" */, uint32_t uDate /* = 0 */)
{
	uint32_t ret = 0;
	auto it = _code_list->begin();
	for (; it != _code_list->end(); it++)
	{
		WTSContractInfo* cInfo = static_cast<WTSContractInfo*>(*it);
		if(strlen(exchg) > 0 && strcmp(cInfo->getExchg(), exchg) != 0)
			continue;

		if (uDate == 0 || (cInfo->getOpenDate() <= uDate && cInfo->getExpireDate() >= uDate))
			ret++;
	}

	return ret;
}

WTSContractInfo* WTSBaseDataMgr::getContractByIndex(uint32_t idx)
{
	if (idx >= _code_list->size())
		return NULL;

	return static_cast<WTSContractInfo*>(_code_list->at(idx));
}

WTSArray* WTSBaseDataMgr::getContracts(const char* exchg /* = "" */, uint32_t uDate /* = 0 */)
{
	WTSArray* ay = WTSArray::create();
	auto it = _code_list->begin();
	for (; it != _code_list->end(); it++)
	{
		WTSContractInfo* cInfo = static_cast<WTSContractInfo*>(*it);
		if (strlen(exchg) > 0 && strcmp(cInfo->getExchg(), exchg) != 0)
			continue;

		if (uDate == 0 || (cInfo->getOpenDate() <= uDate && cInfo->getExpireDate() >= uDate))
			ay->append(cInfo, true);
	}
	return ay;
}

WTSArray* WTSBaseDataMgr::getAllSessions()
{
	WTSArray* ay = WTSArray::create();
	for (auto it = _session_map->begin(); it != _session_map->end(); it++)
	{
		ay->append(it->second, true);
	}
	return ay;
}

WTSSessionInfo* WTSBaseDataMgr::getSession(const char* sid)
{
	return (WTSSessionInfo*)_session_map->get(sid);
}

WTSSessionInfo* WTSBaseDataMgr::getSessionByCode(const char* code, const char* exchg /* = "" */)
{
	WTSContractInfo* ct = getContract(code, exchg);
	if (ct == NULL)
		return NULL;

	return ct->getCommInfo()->getSessionInfo();
}

bool WTSBaseDataMgr::isHoliday(const char* pid, uint32_t uDate, bool isTpl /* = false */)
{
	uint32_t wd = TimeUtils::getWeekDay(uDate);
	if (wd >= TimeUtils::INVALID_WEEKDAY)
		return true; // Invalid dates fail closed: they cannot be treated as workdays.
	if (wd == 0 || wd == 6)
		return true;

	std::string tplid;
	if (isTpl)
	{
		if (pid != NULL)
			tplid = pid;
	}
	else
	{
		tplid = getTplIDByPID(pid);
	}

	auto it = _trading_days.find(tplid.c_str());
	if(it != _trading_days.end())
	{
		const TradingDayTpl& tpl = it->second;
		return (tpl._holidays.find(uDate) != tpl._holidays.end());
	}

	return false;
}


//void WTSBaseDataMgr::release()
//{
//	if (m_mapExchgContract)
//	{
//		m_mapExchgContract->release();
//		m_mapExchgContract = NULL;
//	}
//
//	if (_session_map)
//	{
//		_session_map->release();
//		_session_map = NULL;
//	}
//
//	if (m_mapCommodities)
//	{
//		m_mapCommodities->release();
//		m_mapCommodities = NULL;
//	}
//}

namespace {
struct VariantReleaser { void operator()(WTSVariant* p) const { if (p) p->release(); } };
struct SessionMapReleaser { void operator()(WTSStringMap* p) const { if (p) p->release(); } };

// YAML scalars arrive as strings. Parse their entire text without the floating
// point narrowing performed by WTSVariant::asInt32/asUInt32.
bool sessionInteger(const WTSVariant* value, int64_t minimum, int64_t maximum, int64_t& result)
{
	if (!value) return false;
	switch (value->type())
	{
	case WTSVariant::VT_Int32: case WTSVariant::VT_Uint32:
	case WTSVariant::VT_Int64: case WTSVariant::VT_Uint64:
	case WTSVariant::VT_String: break;
	default: return false;
	}
	const char* text = value->asCString();
	if (!text || !*text) return false;
	char* end = nullptr;
	errno = 0;
	const long long parsed = std::strtoll(text, &end, 10);
	if (errno == ERANGE || end == text || *end != '\0' || parsed < minimum || parsed > maximum)
		return false;
	result = static_cast<int64_t>(parsed);
	return true;
}

bool sessionTime(const WTSVariant* section, const char* key, uint32_t& result)
{
	if (!section || !section->isObject()) return false;
	int64_t value = 0;
	if (!sessionInteger(section->get(key), 0, 2400, value) || value % 100 >= 60 ||
		(value / 100 == 24 && value != 2400)) return false;
	result = static_cast<uint32_t>(value);
	return true;
}

bool sessionRange(const WTSVariant* section, uint32_t& from, uint32_t& to)
{
	return sessionTime(section, "from", from) && sessionTime(section, "to", to);
}
}

bool WTSBaseDataMgr::loadSessions(const char* filename)
{
	if (!StdFile::exists(filename))
	{
		WTSLogger::error("Trading sessions configuration file {} not exists", filename);
		return false;
	}

	std::unique_ptr<WTSVariant, VariantReleaser> root(WTSCfgLoader::load_from_file(filename));
	if (!root || !root->isObject())
	{
		WTSLogger::error("Loading session config file {} failed", filename);
		return false;
	}

	std::unique_ptr<WTSStringMap, SessionMapReleaser> staged(WTSStringMap::create());
	for(const std::string& id : root->memberNames())
	{
		WTSVariant* jVal = root->get(id);
		if (!jVal || !jVal->isObject())
		{
			WTSLogger::error("Invalid session {} in {}", id, filename);
			return false;
		}
		int64_t offset = 0;
		if (jVal->has("offset") && !sessionInteger(jVal->get("offset"), -1440, 1440, offset))
		{
			WTSLogger::error("Invalid session offset for {} in {}", id, filename);
			return false;
		}
		WTSVariant* jSecs = jVal->get("sections");
		if (!jSecs || !jSecs->isArray())
		{
			WTSLogger::error("Invalid session sections for {} in {}", id, filename);
			return false;
		}
		WTSSessionInfo* sInfo = WTSSessionInfo::create(id.c_str(), jVal->getCString("name"), static_cast<int32_t>(offset));
		// The staged map owns the new object, including every early return below.
		staged->add(id, sInfo, false);

		if (jVal->has("auction"))
		{
			WTSVariant* jAuc = jVal->get("auction");
			uint32_t from, to;
			if (!sessionRange(jAuc, from, to) || !sInfo->trySetAuctionTime(from, to))
			{
				WTSLogger::error("Invalid auction for session {} in {}", id, filename);
				return false;
			}
		}
		else if (jVal->has("auctions"))
		{
			WTSVariant* jAucs = jVal->get("auctions");
			if (!jAucs || !jAucs->isArray())
			{
				WTSLogger::error("Invalid auctions for session {} in {}", id, filename);
				return false;
			}
			for (uint32_t i = 0; i < jAucs->size(); i++)
			{
				WTSVariant* jSec = jAucs->get(i);
				uint32_t from, to;
				if (!sessionRange(jSec, from, to) || !sInfo->tryAddAuctionTime(from, to))
				{
					WTSLogger::error("Invalid auction {} for session {} in {}", i, id, filename);
					return false;
				}
			}
		}

		for (uint32_t i = 0; i < jSecs->size(); i++)
		{
			WTSVariant* jSec = jSecs->get(i);
			uint32_t from, to;
			if (!sessionRange(jSec, from, to) || !sInfo->tryAddTradingSection(from, to))
			{
				WTSLogger::error("Invalid section {} for session {} in {}", i, id, filename);
				return false;
			}
		}
	}
	for (auto it = staged->begin(); it != staged->end(); ++it)
		_session_map->add(it->first, it->second);

	return true;
}

// Parse using the same numeric convention during validation and publication.
static bool commodityNumber(WTSVariant* config, const char* key, double& value)
{
	WTSVariant* field = config->get(key);
	if (!field || field->isArray() || field->isObject()) return false;
	const std::string text = field->asString();
	if (text.empty()) return false;
	std::istringstream stream(text);
	stream.imbue(std::locale::classic());
	double parsed;
	if (!(stream >> parsed) || stream.peek() != std::char_traits<char>::eof() || !std::isfinite(parsed))
		return false;
	value = parsed;
	return true;
}

void parseCommodity(WTSCommodityInfo* pCommInfo, WTSVariant* jPInfo)
{
	double value = 0;
	commodityNumber(jPInfo, "pricetick", value);
	pCommInfo->setPriceTick(value);
	value = 0;
	commodityNumber(jPInfo, "volscale", value);
	pCommInfo->setVolScale(static_cast<uint32_t>(value));

	if (jPInfo->has("category"))
	{
		value = 0;
		commodityNumber(jPInfo, "category", value);
		pCommInfo->setCategory(static_cast<ContractCategory>(static_cast<uint32_t>(value)));
	}
	else
		pCommInfo->setCategory(CC_Future);

	value = 0;
	commodityNumber(jPInfo, "covermode", value);
	pCommInfo->setCoverMode(static_cast<CoverMode>(static_cast<uint32_t>(value)));
	value = 0;
	commodityNumber(jPInfo, "pricemode", value);
	pCommInfo->setPriceMode(static_cast<PriceMode>(static_cast<uint32_t>(value)));

	if (jPInfo->has("trademode"))
	{
		value = 0;
		commodityNumber(jPInfo, "trademode", value);
		pCommInfo->setTradingMode(static_cast<TradingMode>(static_cast<uint32_t>(value)));
	}
	else
		pCommInfo->setTradingMode(TM_Both);

	double lotsTick = 1;
	double minLots = 1;
	if (jPInfo->has("lotstick"))
		commodityNumber(jPInfo, "lotstick", lotsTick);
	if (jPInfo->has("minlots"))
		commodityNumber(jPInfo, "minlots", minLots);
	pCommInfo->setLotsTick(lotsTick);
	pCommInfo->setMinLots(minLots);
}

// Required: positive finite pricetick, positive integral volscale. Optional
// keys must be valid when present; a missing trademode keeps the documented
// loader default TM_Both (all shipped commodity files rely on it).
static bool commodityConfigValid(WTSVariant* config, std::string& reason)
{
	if (!config || !config->isObject()) { reason = "entry is not an object"; return false; }
	double tick, scale;
	if (!commodityNumber(config, "pricetick", tick) || tick <= 0)
	{
		reason = config->has("pricetick") ? "pricetick must be a positive finite number" : "pricetick is missing";
		return false;
	}
	if (!commodityNumber(config, "volscale", scale) || scale <= 0 ||
		scale > std::numeric_limits<uint32_t>::max() || std::floor(scale) != scale)
	{
		reason = config->has("volscale") ? "volscale must be a positive integer" : "volscale is missing";
		return false;
	}
	for (const char* key : {"lotstick", "minlots"})
	{
		double value;
		if (config->has(key) && (!commodityNumber(config, key, value) || value <= 0))
		{
			reason = std::string(key) + " must be a positive finite number";
			return false;
		}
	}
	for (const char* key : {"category", "covermode", "pricemode", "trademode"})
	{
		double value;
		if (config->has(key) && (!commodityNumber(config, key, value) || value < 0 ||
			value > std::numeric_limits<uint32_t>::max() || std::floor(value) != value))
		{
			reason = std::string(key) + " must be a non-negative integer";
			return false;
		}
	}
	return true;
}

// Builds a staged commodity and checks that it can be marked ready with the
// given session. Nothing is published.
static bool stagedCommodityReady(WTSCommodityInfo* commodity, WTSVariant* config, WTSSessionInfo* session, std::string& reason)
{
	parseCommodity(commodity, config);
	if (session == nullptr) { reason = "session is not loaded"; return false; }
	commodity->setSessionInfo(session);
	if (!commodity->markReady()) { reason = "unsupported category/covermode/pricemode/trademode or empty identity"; return false; }
	return true;
}

bool WTSBaseDataMgr::loadCommodities(const char* filename)
{
	if (!StdFile::exists(filename))
	{
		WTSLogger::error("Commodities configuration file {} not exists", filename);
		return false;
	}

	WTSVariant* root = WTSCfgLoader::load_from_file(filename);
	if (root == NULL)
	{
		WTSLogger::error("Loading commodities config file {} failed", filename);
		return false;
	}
	// Validate the complete import before publishing any commodity.
	if (!root->isObject()) { root->release(); return false; }
	for (const std::string& exchg : root->memberNames())
	{
		WTSVariant* exchange = root->get(exchg);
		if (!exchange || !exchange->isObject()) { root->release(); return false; }
		for (const std::string& pid : exchange->memberNames())
		{
			WTSVariant* config = exchange->get(pid);
			std::string reason;
			if (!commodityConfigValid(config, reason))
			{
				WTSLogger::error("Commodity {}.{} rejected, nothing loaded from {}: {}", exchg.c_str(), pid.c_str(), filename, reason.c_str());
				root->release();
				return false;
			}
			const char* sid = config->getCString("session");
			WTSCommodityInfo* commodity = WTSCommodityInfo::create(pid.c_str(), config->getCString("name"), exchg.c_str(), sid, config->getCString("holiday"));
			if (!commodity)
			{
				WTSLogger::error("Commodity {}.{} rejected, nothing loaded from {}: invalid identity", exchg.c_str(), pid.c_str(), filename);
				root->release();
				return false;
			}
			const bool valid = stagedCommodityReady(commodity, config, getSession(sid), reason);
			commodity->release();
			if (!valid)
			{
				WTSLogger::error("Commodity {}.{} rejected, nothing loaded from {}: {}", exchg.c_str(), pid.c_str(), filename, reason.c_str());
				root->release();
				return false;
			}
		}
	}
	bool allValid = true;
	uint32_t defaultTradeModeCnt = 0;
	std::string defaultTradeModeSample;

	for(const std::string& exchg : root->memberNames())
	{
		WTSVariant* jExchg = root->get(exchg);

		for (const std::string& pid : jExchg->memberNames())
		{
			WTSVariant* jPInfo = jExchg->get(pid);

			const char* name = jPInfo->getCString("name");
			const char* sid = jPInfo->getCString("session");
			const char* hid = jPInfo->getCString("holiday");
			if (!WTSCommodityInfo::isIdentityValid(pid.c_str(), name, exchg.c_str(), sid, hid))
			{
				WTSLogger::error("Commodity identity exceeds field capacity: {}.{}", exchg.c_str(), pid.c_str());
				allValid = false;
				continue;
			}

			if (strlen(sid) == 0)
			{
				WTSLogger::warn("No session configured for {}.{}", exchg.c_str(), pid.c_str());
				continue;
			}

			WTSCommodityInfo* commInfo = WTSCommodityInfo::create(pid.c_str(), name, exchg.c_str(), sid, hid);
			if (commInfo == NULL)
			{
				WTSLogger::error("Invalid commodity identity: {}.{}", exchg.c_str(), pid.c_str());
				allValid = false;
				continue;
			}
			parseCommodity(commInfo, jPInfo);

			WTSSessionInfo* sInfo = getSession(sid);
			if(sInfo == NULL)
			{
				WTSLogger::error("Session {} of {}.{} not exists", sid, exchg, pid);
				commInfo->release();
				continue;
			}
			commInfo->setSessionInfo(sInfo);
			// Only ready commodities are published; this is the admission gate.
			if (!commInfo->markReady())
			{
				WTSLogger::error("Commodity {}.{} is not ready and is not registered", exchg.c_str(), pid.c_str());
				commInfo->release();
				allValid = false;
				continue;
			}

			// Card 4 follow-up (2026-11-03): a missing trademode keeps the
			// documented TM_Both default, but is reported once per file.
			if (!jPInfo->has("trademode"))
			{
				if (defaultTradeModeCnt == 0)
					defaultTradeModeSample = commInfo->getFullPid();
				defaultTradeModeCnt++;
			}

			_comm_list->append(commInfo, false);

			WTSArray* ayInst = (WTSArray*)_pid_map->get(pid);
			if (ayInst == NULL)
			{
				ayInst = WTSArray::create();
				_pid_map->add(pid, ayInst, false);
			}
			ayInst->append(commInfo, true);

			_fullpid_map->add(commInfo->getFullPid(), commInfo, true);

			_session_to_pids[sid].insert(commInfo->getFullPid());
		}
	}

	if (defaultTradeModeCnt > 0)
		WTSLogger::warn("{} commodities in {} have no trademode and default to TM_Both (long and short), e.g. {}",
			defaultTradeModeCnt, filename, defaultTradeModeSample.c_str());

	WTSLogger::info("Commodities configuration file {} loaded", filename);
	root->release();
	return allValid;
}


bool WTSBaseDataMgr::loadContracts(const char* filename)
{
	if (!StdFile::exists(filename))
	{
		WTSLogger::error("Contracts configuration file {} not exists", filename);
		return false;
	}

	WTSVariant* root = WTSCfgLoader::load_from_file(filename);
	if (root == NULL)
	{
		WTSLogger::error("Loading contracts config file {} failed", filename);
		return false;
	}
	// Validate all entries before registering contracts, derived commodities,
	// or adding codes to already registered commodities.
	std::set<std::string> stagedCommodityIds;
	if (!root->isObject()) { root->release(); return false; }
	for (const std::string& exchg : root->memberNames())
	{
		WTSVariant* exchange = root->get(exchg);
		if (!exchange || !exchange->isObject()) { root->release(); return false; }
		for (const std::string& code : exchange->memberNames())
		{
			WTSVariant* config = exchange->get(code);
			if (!config || !config->isObject()) { root->release(); return false; }
			const char* contractExchg = config->getCString("exchg");
			const bool product = config->has("product"), rules = config->has("rules");
			const char* pid = product ? config->getCString("product") : (rules ? code.c_str() : "");
			const char* alt = config->getCString("altcode");
			if (!*alt) alt = code.c_str();
			if (exchg.size() >= MAX_EXCHANGE_LENGTH || code.size() >= MAX_INSTRUMENT_LENGTH ||
				strlen(contractExchg) >= MAX_EXCHANGE_LENGTH || strlen(contractExchg) + strlen(pid) + 1 >= 64 ||
				!WTSContractInfo::isIdentityValid(code.c_str(), config->getCString("name"), contractExchg, pid) ||
				!WTSContractInfo::isAltCodeValid(alt)) { root->release(); return false; }
			if (product)
			{
				WTSCommodityInfo* commodity = getCommodity(contractExchg, pid);
				if ((!commodity || !commodity->isReady()) &&
					stagedCommodityIds.count(std::string(contractExchg) + "." + pid) == 0)
				{
					WTSLogger::error("Contract {}.{} rejected, nothing loaded from {}: commodity {}.{} is missing or not ready",
						exchg.c_str(), code.c_str(), filename, contractExchg, pid);
					root->release();
					return false;
				}
			}
			else if (rules)
			{
				WTSVariant* rule = config->get("rules");
				std::string reason;
				if (!commodityConfigValid(rule, reason))
				{
					WTSLogger::error("Contract {}.{} rules rejected, nothing loaded from {}: {}", exchg.c_str(), code.c_str(), filename, reason.c_str());
					root->release();
					return false;
				}
				const char* sid = rule->getCString("session");
				if (!*sid) sid = "ALLDAY";
				WTSCommodityInfo* commodity = WTSCommodityInfo::create(pid, config->getCString("name"), exchg.c_str(), sid, rule->getCString("holiday"));
				if (!commodity)
				{
					WTSLogger::error("Contract {}.{} rules rejected, nothing loaded from {}: invalid identity", exchg.c_str(), code.c_str(), filename);
					root->release();
					return false;
				}
				const bool valid = stagedCommodityReady(commodity, rule, getSession(sid), reason);
				commodity->release();
				if (!valid)
				{
					WTSLogger::error("Contract {}.{} rules rejected, nothing loaded from {}: {}", exchg.c_str(), code.c_str(), filename, reason.c_str());
					root->release();
					return false;
				}
				stagedCommodityIds.insert(exchg + "." + pid);
			}
			else
			{
				WTSLogger::error("Contract {}.{} rejected, nothing loaded from {}: neither product nor rules configured", exchg.c_str(), code.c_str(), filename);
				root->release();
				return false;
			}
		}
	}
	bool allValid = true;

	for (const std::string& exchg : root->memberNames())
	{
		WTSVariant* jExchg = root->get(exchg);

		for (const std::string& code : jExchg->memberNames())
		{
			WTSVariant* jcInfo = jExchg->get(code);
			const char* contractName = jcInfo->getCString("name");
			const char* contractExchg = jcInfo->getCString("exchg");
			// Contract identities also enter AD's fixed-width exchange/code keys.
			// Reject before creating a rules-derived commodity or registering a contract.
			if (exchg.size() >= MAX_EXCHANGE_LENGTH || code.size() >= MAX_INSTRUMENT_LENGTH ||
				contractExchg == nullptr || strlen(contractExchg) >= MAX_EXCHANGE_LENGTH)
			{
				WTSLogger::error("Contract identity exceeds storage field capacity: {}.{}", exchg.c_str(), code.c_str());
				allValid = false;
				continue;
			}
			const bool hasProduct = jcInfo->has("product");
			const bool hasRules = jcInfo->has("rules");
			std::string identityPid;
			if (hasProduct)
				identityPid = jcInfo->getCString("product");
			else if (hasRules)
				identityPid = code;

			const char* configuredAltCode = jcInfo->getCString("altcode");
			const char* effectiveAltCode = jcInfo->has("altcode") && configuredAltCode[0] != '\0'
				? configuredAltCode : code.c_str();
			if (!WTSContractInfo::isIdentityValid(code.c_str(), contractName, contractExchg, identityPid.c_str()) ||
				!WTSContractInfo::isAltCodeValid(effectiveAltCode))
			{
				WTSLogger::error("Contract identity exceeds field capacity: {}.{}", contractExchg, code.c_str());
				allValid = false;
				continue;
			}

			/*
			 *	By Wesley @ 2021.12.28
			 *	这里做一个兼容，如果product为空,先检查是否配置了rules属性，如果配置了rules属性，把合约单独当成品种自动加入
			 *	如果没有配置rules，则直接跳过该合约
			 */
			WTSCommodityInfo* commInfo = NULL;
			std::string pid;
			if (hasProduct)
			{
				pid = jcInfo->getCString("product");
				commInfo = getCommodity(jcInfo->getCString("exchg"), pid.c_str());
			}
			else if (hasRules)
			{
				pid = code.c_str();
				WTSVariant* jPInfo = jcInfo->get("rules");
				const char* name = jcInfo->getCString("name");
				std::string sid = jPInfo->getCString("session");
				std::string hid;
				if (jPInfo->has("holiday"))
					hid = jPInfo->getCString("holiday");

				//这里不能像解析commodity那样处理，直接赋值为ALLDAY
				if (sid.empty())
					sid = "ALLDAY";

				commInfo = WTSCommodityInfo::create(pid.c_str(), name, exchg.c_str(), sid.c_str(), hid.c_str());
				if (commInfo == NULL)
				{
					WTSLogger::error("Invalid commodity identity generated by contract rules: {}.{}", exchg.c_str(), code.c_str());
					allValid = false;
					continue;
				}
				parseCommodity(commInfo, jPInfo);
				WTSSessionInfo* sInfo = getSession(sid.c_str());
				commInfo->setSessionInfo(sInfo);
				if (!commInfo->markReady())
				{
					WTSLogger::error("Commodity generated by contract rules {}.{} is not ready, contract skipped", exchg.c_str(), code.c_str());
					commInfo->release();
					allValid = false;
					continue;
				}

				_comm_list->append(commInfo, false);

				WTSArray* ayInst = (WTSArray*)_pid_map->get(pid);
				if (ayInst == NULL)
				{
					ayInst = WTSArray::create();
					_pid_map->add(pid, ayInst, false);
				}
				ayInst->append(commInfo, true);

				_fullpid_map->add(commInfo->getFullPid(), commInfo, true);

				WTSLogger::debug("Commodity {} has been automatically added", commInfo->getFullPid());
			}

			if (commInfo == NULL)
			{
				WTSLogger::warn("Commodity {}.{} not found, contract {} skipped", jcInfo->getCString("exchg"), jcInfo->getCString("product"), code.c_str());
				continue;
			}

			WTSContractInfo* cInfo = WTSContractInfo::create(code.c_str(),
				contractName,
				contractExchg,
				pid.c_str());
			if (cInfo == NULL)
			{
				WTSLogger::error("Invalid contract identity: {}.{}", contractExchg, code.c_str());
				allValid = false;
				continue;
			}

			/*
			 *	By Wesley @ 2024.04.12
			 *	如果配置了altcode，则使用配置的altcode
			 *	如果没有配置altcode，则使用code作为altcode
			 */
			bool bHasAltCode = jcInfo->has("altcode") && strlen(jcInfo->getCString("altcode")) > 0;
			if (bHasAltCode)
		{
			if (!cInfo->setAltCode(jcInfo->getCString("altcode")))
			{
				WTSLogger::error("Invalid alternate contract code for {}.{}", contractExchg, code.c_str());
				cInfo->release();
				allValid = false;
				continue;
			}
		}
			else
		{
			if (!cInfo->setAltCode(code.c_str()))
			{
				WTSLogger::error("Invalid alternate contract code for {}.{}", contractExchg, code.c_str());
				cInfo->release();
				allValid = false;
				continue;
			}
		}

			cInfo->setCommInfo(commInfo);

			uint32_t maxMktQty = 1000000;
			uint32_t maxLmtQty = 1000000;
			uint32_t minMktQty = 1;
			uint32_t minLmtQty = 1;
			if (jcInfo->has("maxmarketqty"))
				maxMktQty = jcInfo->getUInt32("maxmarketqty");
			if (jcInfo->has("maxlimitqty"))
				maxLmtQty = jcInfo->getUInt32("maxlimitqty");
			if (jcInfo->has("minmarketqty"))
				minMktQty = jcInfo->getUInt32("minmarketqty");
			if (jcInfo->has("minlimitqty"))
				minLmtQty = jcInfo->getUInt32("minlimitqty");
			cInfo->setVolumeLimits(maxMktQty, maxLmtQty, minMktQty, minLmtQty);

			uint32_t opendate = 0;
			uint32_t expiredate = 0;
			if (jcInfo->has("opendate"))
				opendate = jcInfo->getUInt32("opendate");
			if (jcInfo->has("expiredate"))
				expiredate = jcInfo->getUInt32("expiredate");
			cInfo->setDates(opendate, expiredate);

			if (jcInfo->has("volscale"))
				cInfo->setVolScale(jcInfo->getUInt32("volscale"));
			else
				cInfo->setVolScale(commInfo->getVolScale());

			double lMargin = 0;
			double sMargin = 0;
			if (jcInfo->has("longmarginratio"))
				lMargin = jcInfo->getDouble("longmarginratio");
			if (jcInfo->has("shortmarginratio"))
				sMargin = jcInfo->getDouble("shortmarginratio");
			cInfo->setMarginRatios(lMargin, sMargin);

			commInfo->addCode(code.c_str());
			_code_list->append(cInfo, false);

			WTSArray* ayInst = (WTSArray*)_code_map->get(cInfo->getCode());
			if(ayInst == NULL)
			{
				ayInst = WTSArray::create();
				_code_map->add(cInfo->getCode(), ayInst, false);
			}
			ayInst->append(cInfo, true);
			_fullcode_map->add(cInfo->getFullCode(), cInfo, true);

			if(bHasAltCode)
			{
				ayInst = (WTSArray*)_code_map->get(cInfo->getAltCode());
				if (ayInst == NULL)
				{
					ayInst = WTSArray::create();
					_code_map->add(cInfo->getAltCode(), ayInst, false);
				}
				ayInst->append(cInfo, true);

				_fullcode_map->add(cInfo->getFullAltCode(), cInfo, true);
			}
			
		}
	}

	WTSLogger::info("Contracts configuration file {} loaded, {} contracts", filename, _code_list->size());
	root->release();

	{
		std::sort(_code_list->begin(), _code_list->end(), [](WTSObject* left, WTSObject* right) {
			WTSContractInfo* cInfoA = static_cast<WTSContractInfo*>(left);
			WTSContractInfo* cInfoB = static_cast<WTSContractInfo*>(right);

			return strcmp(cInfoA->getFullCode(), cInfoB->getFullCode()) < 0;
		});

		uint32_t idx = 0;
		for (auto it = _code_list->begin(); it != _code_list->end(); it++, idx++)
		{
			WTSContractInfo* cInfo = static_cast<WTSContractInfo*>(*it);
			cInfo->setTotalIndex(idx);
		}
	}
	return allValid;
}

bool WTSBaseDataMgr::loadHolidays(const char* filename)
{
	if (!StdFile::exists(filename))
	{
		WTSLogger::error("Holidays configuration file {} not exists", filename);
		return false;
	}

	WTSVariant* root = WTSCfgLoader::load_from_file(filename);
	if (root == NULL)
	{
		WTSLogger::error("Loading holidays config file {} failed", filename);
		return false;
	}

	for (const std::string& hid : root->memberNames())
	{
		WTSVariant* jHolidays = root->get(hid);
		if(!jHolidays->isArray())
			continue;

		TradingDayTpl& trdDayTpl = _trading_days[hid];
		for(uint32_t i = 0; i < jHolidays->size(); i++)
		{
			WTSVariant* hItem = jHolidays->get(i);
			trdDayTpl._holidays.insert(hItem->asUInt32());
		}
	}

	root->release();

	return true;
}

uint64_t WTSBaseDataMgr::getBoundaryTime(const char* stdPID, uint32_t tDate, bool isSession /* = false */, bool isStart /* = true */)
{
	if (!stdPID || !*stdPID || (tDate != 0 && dateStatus(tDate) != DateMappingStatus::Ok))
		return 0;
	if(tDate == 0)
		tDate = TimeUtils::getCurDate();
	
	std::string tplid = stdPID;
	bool isTpl = false;
	WTSSessionInfo* sInfo = NULL;
	if (isSession)
	{
		sInfo = getSession(stdPID);
		tplid = DEFAULT_HOLIDAY_TPL;
		isTpl = true;
	}
	else
	{
		WTSCommodityInfo* cInfo = getCommodity(stdPID);
		if (cInfo == NULL)
			return 0;

		sInfo = cInfo->getSessionInfo();
	}

	if (sInfo == NULL)
		return 0;

	uint32_t weekday = TimeUtils::getWeekDay(tDate);
	if (weekday == 6 || weekday == 0)
	{
		if (isStart)
			tDate = getNextTDate(tplid.c_str(), tDate, 1, isTpl);
		else
			tDate = getPrevTDate(tplid.c_str(), tDate, 1, isTpl);
	}

	//不偏移的最简单,只需要直接返回开盘和收盘时间即可
	if (sInfo->getOffsetMins() == 0)
	{
		if (isStart)
			return (uint64_t)tDate * 10000 + sInfo->getOpenTime();
		else
			return (uint64_t)tDate * 10000 + sInfo->getCloseTime();
	}

	if(sInfo->getOffsetMins() < 0)
	{
		//往前偏移,就是交易日推后,一般用于外盘
		//这个比较简单,只需要按自然日取即可
		if (isStart)
			return (uint64_t)tDate * 10000 + sInfo->getOpenTime();
		else
			return (uint64_t)TimeUtils::getNextDate(tDate) * 10000 + sInfo->getCloseTime();
	}
	else
	{
		//往后偏移,一般国内期货夜盘都是这个,即夜盘算是第二个交易日
		//这个比较复杂,主要节假日后第一天的边界很麻烦（如一般情况的周一）
		//这种情况唯一方便的就是,收盘时间不需要处理
		if(!isStart)
			return (uint64_t)tDate * 10000 + sInfo->getCloseTime();

		//想到一个简单的办法,就是不管怎么样,开始时间一定是上一个交易日的晚上
		//所以我只需要拿到上一个交易日即可
		tDate = getPrevTDate(tplid.c_str(), tDate, 1, isTpl);
		return (uint64_t)tDate * 10000 + sInfo->getOpenTime();
	}
}

uint32_t WTSBaseDataMgr::calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession /* = false */)
{
	if (!stdPID || !*stdPID || !WTSSessionInfo::isValidSessionTime(uTime) ||
		(uDate != 0 && dateStatus(uDate) != DateMappingStatus::Ok))
		return 0;
	if (uDate == 0)
	{
		TimeUtils::getDateTime(uDate, uTime);
		uTime /= 100000;
	}

	std::string tplid = stdPID;
	bool isTpl = false;
	WTSSessionInfo* sInfo = NULL;
	if(isSession)
	{
		sInfo = getSession(stdPID);
		tplid = DEFAULT_HOLIDAY_TPL;
		isTpl = true;
	}
	else
	{
		WTSCommodityInfo* cInfo = getCommodity(stdPID);
		if (cInfo == NULL)
			return uDate;
		
		sInfo = cInfo->getSessionInfo();
	}

	if (sInfo == NULL)
		return uDate;
	
	uint32_t offMin = sInfo->offsetTime(uTime, true);
	//7*24交易时间单独算
	uint32_t total_mins = sInfo->getTradingMins();
	if(total_mins == 1440)
	{
		if(sInfo->getOffsetMins() > 0 && uTime > offMin)
		{
			return TimeUtils::getNextDate(uDate, 1);
		}
		else if (sInfo->getOffsetMins() < 0 && uTime < offMin)
		{
			return TimeUtils::getNextDate(uDate, -1);
		}

		return uDate;
	}

	uint32_t weekday = TimeUtils::getWeekDay(uDate);
	if (sInfo->getOffsetMins() > 0)
	{
		//如果向后偏移,且当前时间大于偏移时间,说明向后跨日了
		//这时交易日=下一个交易日
		if (uTime > offMin)
		{
			//如,20151016 23:00,偏移300分钟,为5:00
			return getNextTDate(tplid.c_str(), uDate, 1, isTpl);
		}
		else if (weekday == 6 || weekday == 0)
		{
			//如,20151017 1:00,周六,交易日为20151019
			return getNextTDate(tplid.c_str(), uDate, 1, isTpl);
		}
	}
	else if (sInfo->getOffsetMins() < 0)
	{
		//如果向前偏移,且当前时间小于偏移时间,说明还是前一个交易日
		//这时交易日=前一个交易日
		if (uTime < offMin)
		{
			//如20151017 1:00,偏移-300分钟,为20:00
			return getPrevTDate(tplid.c_str(), uDate, 1, isTpl);
		}
		else if (weekday == 6 || weekday == 0)
		{
			//因为向前偏移,如果在周末,则直接到下一个交易日
			return getNextTDate(tplid.c_str(), uDate, 1, isTpl);
		}
	}
	else if (weekday == 6 || weekday == 0)
	{
		//如果没有偏移,且在周末,则直接读取下一个交易日
		return getNextTDate(tplid.c_str(), uDate, 1, isTpl);;
	}

	//其他情况,交易日=自然日
	return uDate;
}

uint32_t WTSBaseDataMgr::getTradingDate(const char* pid, uint32_t uOffDate /* = 0 */, uint32_t uOffMinute /* = 0 */, bool isTpl /* = false */)
{
	if (!pid || !*pid || !WTSSessionInfo::isValidSessionTime(uOffMinute) ||
		(uOffDate != 0 && dateStatus(uOffDate) != DateMappingStatus::Ok))
		return 0;
	const char* tplID = isTpl ? pid : getTplIDByPID(pid);

	uint32_t curDate = TimeUtils::getCurDate();
	auto it = _trading_days.find(tplID);
	if (it == _trading_days.end())
	{
		return curDate;
	}

	TradingDayTpl* tpl = (TradingDayTpl*)&it->second;
	if (tpl->_cur_tdate != 0 && uOffDate == 0)
		return tpl->_cur_tdate;

	if (uOffDate == 0)
		uOffDate = curDate;

	uint32_t weekday = TimeUtils::getWeekDay(uOffDate);

	if (weekday == 6 || weekday == 0)
	{
		//如果没有偏移,且在周末,则直接读取下一个交易日
		tpl->_cur_tdate = getNextTDate(tplID, uOffDate, 1, true);
		uOffDate = tpl->_cur_tdate;
	}

	//其他情况,交易日=自然日
	return uOffDate;
}

uint32_t WTSBaseDataMgr::getNextTDate(const char* pid, uint32_t uDate, int days /* = 1 */, bool isTpl /* = false */)
{
	if (!pid || !*pid || days <= 0 || dateStatus(uDate) != DateMappingStatus::Ok) return 0;
	uint32_t curDate = uDate;
	int left = days;
	while (true)
	{
		tm t;
		memset(&t, 0, sizeof(tm));
		t.tm_year = curDate / 10000 - 1900;
		t.tm_mon = (curDate % 10000) / 100 - 1;
		t.tm_mday = curDate % 100;
		//t.tm_isdst 	
		time_t ts = mktime(&t);
		ts += 86400;

		tm* newT = localtime(&ts);
		curDate = (newT->tm_year + 1900) * 10000 + (newT->tm_mon + 1) * 100 + newT->tm_mday;
		if (newT->tm_wday != 0 && newT->tm_wday != 6 && !isHoliday(pid, curDate, isTpl))
		{
			//如果不是周末,也不是节假日,则剩余的天数-1
			left--;
			if (left == 0)
				return curDate;
		}
	}
}

uint32_t WTSBaseDataMgr::getPrevTDate(const char* pid, uint32_t uDate, int days /* = 1 */, bool isTpl /* = false */)
{
	if (!pid || !*pid || days <= 0 || dateStatus(uDate) != DateMappingStatus::Ok) return 0;
	uint32_t curDate = uDate;
	int left = days;
	while (true)
	{
		tm t;
		memset(&t, 0, sizeof(tm));
		t.tm_year = curDate / 10000 - 1900;
		t.tm_mon = (curDate % 10000) / 100 - 1;
		t.tm_mday = curDate % 100;
		//t.tm_isdst 	
		time_t ts = mktime(&t);
		ts -= 86400;

		tm* newT = localtime(&ts);
		curDate = (newT->tm_year + 1900) * 10000 + (newT->tm_mon + 1) * 100 + newT->tm_mday;
		if (newT->tm_wday != 0 && newT->tm_wday != 6 && !isHoliday(pid, curDate, isTpl))
		{
			//如果不是周末,也不是节假日,则剩余的天数-1
			left--;
			if (left == 0)
				return curDate;
		}
	}
}

bool WTSBaseDataMgr::isTradingDate(const char* pid, uint32_t uDate, bool isTpl /* = false */)
{
	uint32_t wd = TimeUtils::getWeekDay(uDate);
	if (wd >= TimeUtils::INVALID_WEEKDAY)
		return false;
	if (wd != 0 && wd != 6 && !isHoliday(pid, uDate, isTpl))
	{
		return true;
	}

	return false;
}

void WTSBaseDataMgr::setTradingDate(const char* pid, uint32_t uDate, bool isTpl /* = false */)
{
	std::string tplID = pid;
	if (!isTpl)
		tplID = getTplIDByPID(pid);

	auto it = _trading_days.find(tplID);
	if (it == _trading_days.end())
		return;

	TradingDayTpl* tpl = (TradingDayTpl*)&it->second;
	tpl->_cur_tdate = uDate;
}


CodeSet* WTSBaseDataMgr::getSessionComms(const char* sid)
{
	auto it = _session_to_pids.find(sid);
	if (it == _session_to_pids.end())
		return NULL;

	return (CodeSet*)&it->second;
}

const char* WTSBaseDataMgr::getTplIDByPID(const char* pid)
{
	if (pid == NULL || pid[0] == '\0')
		return "";

	const StringVector& ay = StrUtil::split(pid, ".");
	if (ay.size() < 2)
		return "";

	WTSCommodityInfo* commInfo = getCommodity(ay[0].c_str(), ay[1].c_str());
	if (commInfo == NULL)
		return "";

	return commInfo->getTradingTpl();
}

// Checked date mapping: no additional manager state and no IBaseDataMgr vtable changes.
DateMappingStatus WTSBaseDataMgr::dateMappingContext(const char* id, bool direct, bool needSession,
    WTSSessionInfo*& session, TradingDayTpl*& calendar) noexcept
{
    try {
        if (!id || !*id) return DateMappingStatus::InvalidArgument;
        session = nullptr;
        const char* tpl = id;
        if (direct && needSession) {
            session = getSession(id);
            if (!session) return DateMappingStatus::MissingSession;
            tpl = DEFAULT_HOLIDAY_TPL;
        } else if (!direct) {
            WTSCommodityInfo* commodity = getCommodity(id);
            if (!commodity) return DateMappingStatus::MissingCommodity;
            if (needSession) {
                session = commodity->getSessionInfo();
                if (!session) return DateMappingStatus::MissingSession;
            }
            tpl = commodity->getTradingTpl();
        }
        if (needSession && session->getSectionCount() == 0)
            return DateMappingStatus::MissingSession;
        if (!tpl || !*tpl) return DateMappingStatus::MissingCalendar;
        auto it = _trading_days.find(tpl);
        if (it == _trading_days.end()) return DateMappingStatus::MissingCalendar;
        calendar = &it->second;
        return DateMappingStatus::Ok;
    } catch (...) { return DateMappingStatus::InternalError; }
}

DateMappingResult32 WTSBaseDataMgr::checkedNextTradingDate(const char* id, uint32_t date, uint32_t days, bool isTpl) noexcept
{
    try {
        if (!id || !*id || days == 0) return dateResult(DateMappingStatus::InvalidArgument);
        auto status = dateStatus(date);
        if (status != DateMappingStatus::Ok) return dateResult(status);
        WTSSessionInfo* session; TradingDayTpl* calendar;
        status = dateMappingContext(id, isTpl, false, session, calendar);
        if (status != DateMappingStatus::Ok) return dateResult(status);
        return moveTradingDays(*calendar, date, days, 1);
    } catch (...) { return dateResult(DateMappingStatus::InternalError); }
}
DateMappingResult32 WTSBaseDataMgr::checkedPrevTradingDate(const char* id, uint32_t date, uint32_t days, bool isTpl) noexcept
{
    try {
        if (!id || !*id || days == 0) return dateResult(DateMappingStatus::InvalidArgument);
        auto status = dateStatus(date);
        if (status != DateMappingStatus::Ok) return dateResult(status);
        WTSSessionInfo* session; TradingDayTpl* calendar;
        status = dateMappingContext(id, isTpl, false, session, calendar);
        if (status != DateMappingStatus::Ok) return dateResult(status);
        return moveTradingDays(*calendar, date, days, -1);
    } catch (...) { return dateResult(DateMappingStatus::InternalError); }
}
DateMappingResult32 WTSBaseDataMgr::checkedCalcTradingDate(const char* id, uint32_t date, uint32_t time, bool isSession) noexcept
{
    try {
        if (!id || !*id) return dateResult(DateMappingStatus::InvalidArgument);
        if (!WTSSessionInfo::isValidSessionTime(time)) return dateResult(DateMappingStatus::InvalidTime);
        DateMappingStatus status;
        if (date == 0) {
            status = currentDateTime(date, time);
            if (status != DateMappingStatus::Ok) return dateResult(status);
        } else if ((status = dateStatus(date)) != DateMappingStatus::Ok) return dateResult(status);
        WTSSessionInfo* session; TradingDayTpl* calendar;
        status = dateMappingContext(id, isSession, true, session, calendar);
        if (status != DateMappingStatus::Ok) return dateResult(status);
        const uint32_t shifted = session->offsetTime(time, true);
        const int32_t offset = session->getOffsetMins();
        if (session->getTradingMins() == 1440) {
            const int direction = offset > 0 && time > shifted ? 1 : offset < 0 && time < shifted ? -1 : 0;
            if (direction && !moveOneDate(date, direction)) return dateResult(DateMappingStatus::OutOfRange);
            return dateResult(DateMappingStatus::Ok, date);
        }
        uint32_t weekday;
        if (!TimeUtils::tryGetWeekDay(date, weekday)) return dateResult(DateMappingStatus::InternalError);
        if (offset > 0 && time > shifted) return moveTradingDays(*calendar, date, 1, 1);
        if (offset < 0 && time < shifted) return moveTradingDays(*calendar, date, 1, -1);
        if (weekday == 0 || weekday == 6) return moveTradingDays(*calendar, date, 1, 1);
        return dateResult(DateMappingStatus::Ok, date);
    } catch (...) { return dateResult(DateMappingStatus::InternalError); }
}
DateMappingResult64 WTSBaseDataMgr::checkedGetBoundaryTime(const char* id, uint32_t date, bool isSession, bool isStart) noexcept
{
    try {
        if (!id || !*id) return boundaryResult(DateMappingStatus::InvalidArgument);
        DateMappingStatus status;
        if (date == 0) {
            uint32_t time;
            status = currentDateTime(date, time);
            if (status != DateMappingStatus::Ok) return boundaryResult(status);
        } else if ((status = dateStatus(date)) != DateMappingStatus::Ok) return boundaryResult(status);
        WTSSessionInfo* session; TradingDayTpl* calendar;
        status = dateMappingContext(id, isSession, true, session, calendar);
        if (status != DateMappingStatus::Ok) return boundaryResult(status);
        uint32_t weekday;
        if (!TimeUtils::tryGetWeekDay(date, weekday)) return boundaryResult(DateMappingStatus::InternalError);
        if (weekday == 0 || weekday == 6) {
            const auto moved = moveTradingDays(*calendar, date, 1, isStart ? 1 : -1);
            if (moved.status != DateMappingStatus::Ok) return boundaryResult(moved.status);
            date = moved.value;
        }
        if (session->getOffsetMins() < 0 && !isStart) {
            if (!moveOneDate(date, 1)) return boundaryResult(DateMappingStatus::OutOfRange);
        } else if (session->getOffsetMins() > 0 && isStart) {
            const auto moved = moveTradingDays(*calendar, date, 1, -1);
            if (moved.status != DateMappingStatus::Ok) return boundaryResult(moved.status);
            date = moved.value;
        }
        return boundaryResult(DateMappingStatus::Ok, static_cast<uint64_t>(date) * 10000 +
            (isStart ? session->getOpenTime() : session->getCloseTime()));
    } catch (...) { return boundaryResult(DateMappingStatus::InternalError); }
}
DateMappingResult32 WTSBaseDataMgr::checkedGetTradingDate(const char* id, uint32_t date, uint32_t time, bool isTpl) noexcept
{
    try {
        if (!id || !*id) return dateResult(DateMappingStatus::InvalidArgument);
        if (!WTSSessionInfo::isValidSessionTime(time)) return dateResult(DateMappingStatus::InvalidTime);
        DateMappingStatus status;
        if (date != 0 && (status = dateStatus(date)) != DateMappingStatus::Ok) return dateResult(status);
        WTSSessionInfo* session; TradingDayTpl* calendar;
        status = dateMappingContext(id, isTpl, false, session, calendar);
        if (status != DateMappingStatus::Ok) return dateResult(status);
        if (date == 0 && calendar->_cur_tdate != 0) {
            status = dateStatus(calendar->_cur_tdate);
            return dateResult(status, calendar->_cur_tdate);
        }
        if (date == 0) {
            status = currentDateTime(date, time);
            if (status != DateMappingStatus::Ok) return dateResult(status);
        }
        uint32_t weekday;
        if (!TimeUtils::tryGetWeekDay(date, weekday)) return dateResult(DateMappingStatus::InternalError);
        if (weekday == 0 || weekday == 6) {
            const auto moved = moveTradingDays(*calendar, date, 1, 1);
            if (moved.status != DateMappingStatus::Ok) return moved;
            calendar->_cur_tdate = moved.value;
            return moved;
        }
        return dateResult(DateMappingStatus::Ok, date);
    } catch (...) { return dateResult(DateMappingStatus::InternalError); }
}

DateMappingV1 WTSBaseDataMgr::getDateMappingV1() noexcept
{
    return {sizeof(DateMappingV1), 1, this,
        [](const DateMappingV1* api, const char* id, uint32_t d, uint32_t t, uint8_t s) noexcept {
            if (!api) return dateResult(DateMappingStatus::InvalidArgument);
            if (api->size != sizeof(DateMappingV1) || api->version != 1) return dateResult(DateMappingStatus::Unsupported);
            if (!api->context || s > 1) return dateResult(DateMappingStatus::InvalidArgument);
            return static_cast<WTSBaseDataMgr*>(api->context)->checkedCalcTradingDate(id, d, t, s != 0);
        },
        [](const DateMappingV1* api, const char* id, uint32_t d, uint8_t s, uint8_t start) noexcept {
            if (!api) return boundaryResult(DateMappingStatus::InvalidArgument);
            if (api->size != sizeof(DateMappingV1) || api->version != 1) return boundaryResult(DateMappingStatus::Unsupported);
            if (!api->context || s > 1 || start > 1) return boundaryResult(DateMappingStatus::InvalidArgument);
            return static_cast<WTSBaseDataMgr*>(api->context)->checkedGetBoundaryTime(id, d, s != 0, start != 0);
        },
        [](const DateMappingV1* api, const char* id, uint32_t d, uint32_t t, uint8_t s) noexcept {
            if (!api) return dateResult(DateMappingStatus::InvalidArgument);
            if (api->size != sizeof(DateMappingV1) || api->version != 1) return dateResult(DateMappingStatus::Unsupported);
            if (!api->context || s > 1) return dateResult(DateMappingStatus::InvalidArgument);
            return static_cast<WTSBaseDataMgr*>(api->context)->checkedGetTradingDate(id, d, t, s != 0);
        },
        [](const DateMappingV1* api, const char* id, uint32_t d, uint32_t n, uint8_t s) noexcept {
            if (!api) return dateResult(DateMappingStatus::InvalidArgument);
            if (api->size != sizeof(DateMappingV1) || api->version != 1) return dateResult(DateMappingStatus::Unsupported);
            if (!api->context || s > 1) return dateResult(DateMappingStatus::InvalidArgument);
            return static_cast<WTSBaseDataMgr*>(api->context)->checkedNextTradingDate(id, d, n, s != 0);
        },
        [](const DateMappingV1* api, const char* id, uint32_t d, uint32_t n, uint8_t s) noexcept {
            if (!api) return dateResult(DateMappingStatus::InvalidArgument);
            if (api->size != sizeof(DateMappingV1) || api->version != 1) return dateResult(DateMappingStatus::Unsupported);
            if (!api->context || s > 1) return dateResult(DateMappingStatus::InvalidArgument);
            return static_cast<WTSBaseDataMgr*>(api->context)->checkedPrevTradingDate(id, d, n, s != 0);
        }};
}
