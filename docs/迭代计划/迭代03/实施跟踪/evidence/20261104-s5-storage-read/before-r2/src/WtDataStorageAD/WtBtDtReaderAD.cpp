#include "WtBtDtReaderAD.h"
#include "LMDBKeys.h"
#include "DayBarContractAD.h"

#include "../Includes/WTSStruct.h"
#include "../Includes/WTSVariant.hpp"
#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>

USING_NS_WTP;

namespace {

// Value ABI introduced by 12098b5b: packed(1), before current structures added
// their 8-byte layout/reserve fields. Keep this reader-local so no public ABI changes.
#pragma pack(push, 1)
struct WTSBarStructPackedV2
{
	uint32_t date;
	uint64_t time;
	double open;
	double high;
	double low;
	double close;
	double settle;
	double money;
	double vol;
	double hold;
	double add;
};

struct WTSTickStructPackedV2
{
	char exchg[MAX_EXCHANGE_LENGTH];
	char code[MAX_INSTRUMENT_LENGTH];
	double price;
	double open;
	double high;
	double low;
	double settle_price;
	double upper_limit;
	double lower_limit;
	double total_volume;
	double volume;
	double total_turnover;
	double turn_over;
	double open_interest;
	double diff_interest;
	uint32_t trading_date;
	uint32_t action_date;
	uint32_t action_time;
	double pre_close;
	double pre_settle;
	double pre_interest;
	double bid_prices[10];
	double ask_prices[10];
	double bid_qty[10];
	double ask_qty[10];
};
#pragma pack(pop)

static_assert(sizeof(WTSBarStructPackedV2) == 84, "12098b5b packed bar ABI changed");
static_assert(sizeof(WTSTickStructPackedV2) == 508, "12098b5b packed tick ABI changed");

template<typename SourceBar>
void copy_bar_fields(const SourceBar& source, WTSBarStruct& target)
{
	target.date = source.date;
	target.time = source.time;
	target.open = source.open;
	target.high = source.high;
	target.low = source.low;
	target.close = source.close;
	target.settle = source.settle;
	target.money = source.money;
	target.vol = source.vol;
	target.hold = source.hold;
	target.add = source.add;
}

template<typename SourceTick>
void copy_tick_fields(const SourceTick& source, WTSTickStruct& target)
{
	const std::size_t exchg_size = (std::min)(sizeof(source.exchg), sizeof(target.exchg));
	const std::size_t code_size = (std::min)(sizeof(source.code), sizeof(target.code));
	std::memcpy(target.exchg, source.exchg, exchg_size);
	std::memcpy(target.code, source.code, code_size);
	target.price = source.price;
	target.open = source.open;
	target.high = source.high;
	target.low = source.low;
	target.settle_price = source.settle_price;
	target.upper_limit = source.upper_limit;
	target.lower_limit = source.lower_limit;
	target.total_volume = source.total_volume;
	target.volume = source.volume;
	target.total_turnover = source.total_turnover;
	target.turn_over = source.turn_over;
	target.open_interest = source.open_interest;
	target.diff_interest = source.diff_interest;
	target.trading_date = source.trading_date;
	target.action_date = source.action_date;
	target.action_time = source.action_time;
	target.pre_close = source.pre_close;
	target.pre_settle = source.pre_settle;
	target.pre_interest = source.pre_interest;
	for (std::size_t level = 0; level < 10; ++level)
	{
		target.bid_prices[level] = source.bid_prices[level];
		target.ask_prices[level] = source.ask_prices[level];
		target.bid_qty[level] = source.bid_qty[level];
		target.ask_qty[level] = source.ask_qty[level];
	}
}

} // namespace

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void pipe_btreader_log(IBtDtReaderSink* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	// Messages can carry caller-supplied codes and configured paths; bound the
	// formatter writes to the per-thread buffer instead of trusting their size.
	static thread_local char buffer[512] = { 0 };
	try
	{
		const std::size_t max_output_size = sizeof(buffer) - 1;
		const auto formatted = fmt::format_to_n(buffer, max_output_size, format, args...);
		if (formatted.size > max_output_size)
		{
			constexpr char truncation_marker[] = "...[truncated]";
			constexpr std::size_t marker_size = sizeof(truncation_marker) - 1;
			std::memcpy(buffer + max_output_size - marker_size, truncation_marker, marker_size);
			buffer[max_output_size] = '\0';
		}
		else
		{
			buffer[formatted.size] = '\0';
		}
	}
	catch (...)
	{
		constexpr char fallback[] = "WtBtDtReaderAD log formatting failed";
		std::memcpy(buffer, fallback, sizeof(fallback));
	}

	sink->reader_log(ll, buffer);
}

// Display at most the bytes the key check inspects; the field may be unterminated.
static std::string key_field_for_log(const char* field, std::size_t capacity)
{
	if (field == NULL)
		return "(null)";
	std::size_t length = 0;
	while (length < capacity && field[length] != '\0')
		++length;
	std::string text(field, length);
	if (length == capacity)
		text += "...";
	return text;
}

// BUG-L04-004: reject identities the fixed LMDB key cannot represent before the
// database directory is created or opened, and make the rejection observable.
static bool accept_key_identity(IBtDtReaderSink* sink, const char* exchg, const char* code, const char* entry)
{
	if (lmdbKeyIdentityValid(exchg, code))
		return true;
	pipe_btreader_log(sink, LL_ERROR,
		"{} rejected storage identity {}.{}: exchange and code must be non-empty and shorter than {} and {} bytes",
		entry, key_field_for_log(exchg, MAX_EXCHANGE_LENGTH),
		key_field_for_log(code, MAX_INSTRUMENT_LENGTH), MAX_EXCHANGE_LENGTH, MAX_INSTRUMENT_LENGTH);
	return false;
}

static std::string lmdb_key_hex(const std::string& key, std::size_t max_prefix_bytes)
{
	static const char digits[] = "0123456789abcdef";
	std::string result;
	const std::size_t prefix_bytes = (std::min)(key.size(), max_prefix_bytes);
	result.reserve(prefix_bytes * 2 + (prefix_bytes < key.size() ? 3 : 0));
	for (std::size_t i = 0; i < prefix_bytes; ++i)
	{
		const unsigned char byte = static_cast<unsigned char>(key[i]);
		result.push_back(digits[byte >> 4]);
		result.push_back(digits[byte & 0x0f]);
	}
	if (prefix_bytes < key.size())
		result.append("...");
	return result;
}

static bool decode_bar_key_time(const std::string& key, uint32_t& time)
{
	if (key.size() != sizeof(LMDBBarKey))
		return false;
	uint32_t encoded = 0;
	std::memcpy(&encoded, key.data() + offsetof(LMDBBarKey, _bartime), sizeof(encoded));
	time = reverseEndian(encoded);
	return true;
}

static bool decode_tick_key_date_time(const std::string& key, uint32_t& date, uint32_t& time)
{
	if (key.size() != sizeof(LMDBHftKey))
		return false;
	uint32_t encoded_date = 0;
	uint32_t encoded_time = 0;
	std::memcpy(&encoded_date, key.data() + offsetof(LMDBHftKey, _date), sizeof(encoded_date));
	std::memcpy(&encoded_time, key.data() + offsetof(LMDBHftKey, _time), sizeof(encoded_time));
	date = reverseEndian(encoded_date);
	time = reverseEndian(encoded_time);
	return true;
}

extern "C"
{
	EXPORT_FLAG IBtDtReader* createBtDtReader()
	{
		IBtDtReader* ret = new WtBtDtReaderAD();
		return ret;
	}

	EXPORT_FLAG void deleteBtDtReader(IBtDtReader* reader)
	{
		if (reader != NULL)
			delete reader;
	}
};

WtBtDtReaderAD::WtBtDtReaderAD()
{
}


WtBtDtReaderAD::~WtBtDtReaderAD()
{

}

void WtBtDtReaderAD::init(WTSVariant* cfg, IBtDtReaderSink* sink)
{
	_sink = sink;

	if (cfg == NULL)
		return;

	_base_dir = cfg->getCString("path");
	_base_dir = StrUtil::standardisePath(_base_dir);

	pipe_btreader_log(_sink, LL_INFO, "WtBtDtReaderAD initialized, root data dir is {}", _base_dir);
}

bool WtBtDtReaderAD::read_raw_bars(const char* exchg, const char* code, WTSKlinePeriod period, std::string& buffer)
{
	if (!accept_key_identity(_sink, exchg, code, "read_raw_bars"))
		return false;
	//直接从LMDB读取
	WtLMDBPtr db = get_k_db(exchg, period);
	if (db == NULL)
		return false;

	pipe_btreader_log(_sink, LL_DEBUG, "Reading back {} bars of {}.{}...", PERIOD_NAME[period], exchg, code);
	WtLMDBQuery query(*db);
	LMDBBarKey rKey(exchg, code, 0xffffffff);
	LMDBBarKey lKey(exchg, code, 0);
	bool values_valid = true;
	std::size_t invalid_key_size = 0;
	std::size_t invalid_value_size = 0;
	std::string invalid_key;
	int cnt = query.get_range_checked(std::string((const char*)&lKey, sizeof(lKey)), std::string((const char*)&rKey, sizeof(rKey)),
		[&buffer, &values_valid, &invalid_key_size, &invalid_value_size, &invalid_key](const ValueArray& ayKeys, const ValueArray& ayVals) {
		if (ayKeys.size() != ayVals.size())
		{
			values_valid = false;
			invalid_key_size = ayKeys.empty() ? 0 : ayKeys.front().size();
			invalid_value_size = ayVals.empty() ? 0 : ayVals.front().size();
			invalid_key = ayKeys.empty() ? std::string() : ayKeys.front();
			return;
		}
		if (ayVals.empty())
		{
			buffer.clear();
			return;
		}

		const std::size_t szUnit = sizeof(WTSBarStruct);
		if (ayVals.size() > std::numeric_limits<std::size_t>::max() / szUnit)
		{
			values_valid = false;
			invalid_key_size = ayKeys.empty() ? 0 : ayKeys.front().size();
			invalid_value_size = ayVals.empty() ? 0 : ayVals.front().size();
			invalid_key = ayKeys.empty() ? std::string() : ayKeys.front();
			return;
		}

		for (std::size_t i = 0; i < ayVals.size(); ++i)
		{
			if (ayKeys[i].size() != sizeof(LMDBBarKey))
			{
				values_valid = false;
				invalid_key_size = ayKeys[i].size();
				invalid_value_size = ayVals[i].size();
				invalid_key = ayKeys[i];
				return;
			}

			if (ayVals[i].size() != sizeof(WTSBarStruct) &&
				ayVals[i].size() != sizeof(WTSBarStructPackedV2) &&
				ayVals[i].size() != sizeof(WTSBarStructOld))
			{
				values_valid = false;
				invalid_key_size = ayKeys[i].size();
				invalid_value_size = ayVals[i].size();
				invalid_key = ayKeys[i];
				return;
			}
		}

		std::string result(szUnit * ayVals.size(), '\0');
		char* cursor = result.data();
		for (const std::string& item : ayVals)
		{
			if (item.size() == szUnit)
			{
				std::memcpy(cursor, item.data(), szUnit);
			}
			else if (item.size() == sizeof(WTSBarStructPackedV2))
			{
				WTSBarStructPackedV2 packed_bar;
				std::memcpy(&packed_bar, item.data(), sizeof(packed_bar));
				WTSBarStruct current_bar;
				copy_bar_fields(packed_bar, current_bar);
				std::memcpy(cursor, &current_bar, sizeof(current_bar));
			}
			else
			{
				WTSBarStructOld old_bar;
				std::memcpy(&old_bar, item.data(), sizeof(old_bar));
				WTSBarStruct current_bar;
				copy_bar_fields(old_bar, current_bar);
				std::memcpy(cursor, &current_bar, sizeof(current_bar));
			}
			cursor += szUnit;
		}
		buffer.swap(result);
	});
	if (cnt < 0)
	{
		pipe_btreader_log(_sink, LL_ERROR, "LMDB bar range read failed for {}.{}: {}", exchg, code, db->errmsg());
		return false;
	}
	if (!values_valid)
	{
		uint32_t bar_time = 0;
		const bool has_bar_time = decode_bar_key_time(invalid_key, bar_time);
		const std::string bar_time_text = has_bar_time ? std::to_string(bar_time) : std::string("unknown");
		pipe_btreader_log(_sink, LL_ERROR,
			"Malformed LMDB bar record for {}.{}: key bytes={}, key prefix hex={}, date/time={}, value size={} (expected key size {}, value size {}, {} or {})",
			exchg, code, invalid_key_size,
			lmdb_key_hex(invalid_key, 32), bar_time_text,
			invalid_value_size,
			sizeof(LMDBBarKey), sizeof(WTSBarStruct), sizeof(WTSBarStructPackedV2), sizeof(WTSBarStructOld));
		return false;
	}

	if (period == KP_DAY)
		wt_ad_daybar::normalizeRaw(buffer);	//卡3 3c=C：旧 day 记录 time=交易日，读端归一为 0

	return true;
}

bool WtBtDtReaderAD::read_raw_ticks(const char* exchg, const char* code, uint32_t uDate, std::string& buffer)
{
	if (!accept_key_identity(_sink, exchg, code, "read_raw_ticks"))
		return false;
	//直接从LMDB读取
	WtLMDBPtr db = get_t_db(exchg, code);
	if (db == NULL)
		return false;

	pipe_btreader_log(_sink, LL_DEBUG, "Reading back ticks on {} of {}.{}...", uDate, exchg, code);
	WtLMDBQuery query(*db);
	LMDBHftKey rKey(exchg, code, uDate, 240000000);
	LMDBHftKey lKey(exchg, code, uDate, 0);
	bool values_valid = true;
	std::size_t invalid_key_size = 0;
	std::size_t invalid_value_size = 0;
	std::string invalid_key;
	int cnt = query.get_range_checked(std::string((const char*)&lKey, sizeof(lKey)), std::string((const char*)&rKey, sizeof(rKey)),
		[&buffer, &values_valid, &invalid_key_size, &invalid_value_size, &invalid_key](const ValueArray& ayKeys, const ValueArray& ayVals) {
		if (ayKeys.size() != ayVals.size())
		{
			values_valid = false;
			invalid_key_size = ayKeys.empty() ? 0 : ayKeys.front().size();
			invalid_value_size = ayVals.empty() ? 0 : ayVals.front().size();
			invalid_key = ayKeys.empty() ? std::string() : ayKeys.front();
			return;
		}
		if (ayVals.empty())
		{
			buffer.clear();
			return;
		}

		const std::size_t szUnit = sizeof(WTSTickStruct);
		if (ayVals.size() > std::numeric_limits<std::size_t>::max() / szUnit)
		{
			values_valid = false;
			invalid_key_size = ayKeys.empty() ? 0 : ayKeys.front().size();
			invalid_value_size = ayVals.empty() ? 0 : ayVals.front().size();
			invalid_key = ayKeys.empty() ? std::string() : ayKeys.front();
			return;
		}

		for (std::size_t i = 0; i < ayVals.size(); ++i)
		{
			if (ayKeys[i].size() != sizeof(LMDBHftKey))
			{
				values_valid = false;
				invalid_key_size = ayKeys[i].size();
				invalid_value_size = ayVals[i].size();
				invalid_key = ayKeys[i];
				return;
			}

			if (ayVals[i].size() != sizeof(WTSTickStruct) &&
				ayVals[i].size() != sizeof(WTSTickStructPackedV2) &&
				ayVals[i].size() != sizeof(WTSTickStructOld))
			{
				values_valid = false;
				invalid_key_size = ayKeys[i].size();
				invalid_value_size = ayVals[i].size();
				invalid_key = ayKeys[i];
				return;
			}
		}

		std::string result(szUnit * ayVals.size(), '\0');
		char* cursor = result.data();
		for (const std::string& item : ayVals)
		{
			if (item.size() == szUnit)
			{
				std::memcpy(cursor, item.data(), szUnit);
			}
			else if (item.size() == sizeof(WTSTickStructPackedV2))
			{
				WTSTickStructPackedV2 packed_tick;
				std::memcpy(&packed_tick, item.data(), sizeof(packed_tick));
				WTSTickStruct current_tick;
				copy_tick_fields(packed_tick, current_tick);
				std::memcpy(cursor, &current_tick, sizeof(current_tick));
			}
			else
			{
				WTSTickStructOld old_tick;
				std::memcpy(&old_tick, item.data(), sizeof(old_tick));
				WTSTickStruct current_tick;
				copy_tick_fields(old_tick, current_tick);
				std::memcpy(cursor, &current_tick, sizeof(current_tick));
			}
			cursor += szUnit;
		}
		buffer.swap(result);
	});
	if (cnt < 0)
	{
		pipe_btreader_log(_sink, LL_ERROR, "LMDB tick range read failed for {}.{} on {}: {}", exchg, code, uDate, db->errmsg());
		return false;
	}
	if (!values_valid)
	{
		uint32_t record_date = 0;
		uint32_t record_time = 0;
		const bool has_tick_time = decode_tick_key_date_time(invalid_key, record_date, record_time);
		const std::string record_date_text = has_tick_time ? std::to_string(record_date) : std::string("unknown");
		const std::string record_time_text = has_tick_time ? std::to_string(record_time) : std::string("unknown");
		pipe_btreader_log(_sink, LL_ERROR,
			"Malformed LMDB tick record for {}.{}: key bytes={}, key prefix hex={}, date={}, time={}, value size={} (expected key size {}, value size {}, {} or {})",
			exchg, code, invalid_key_size,
			lmdb_key_hex(invalid_key, 32), record_date_text, record_time_text, invalid_value_size,
			sizeof(LMDBHftKey), sizeof(WTSTickStruct), sizeof(WTSTickStructPackedV2), sizeof(WTSTickStructOld));
		return false;
	}

	return true;
}

WtBtDtReaderAD::WtLMDBPtr WtBtDtReaderAD::get_k_db(const char* exchg, WTSKlinePeriod period)
{
	WtLMDBMap* the_map = NULL;
	std::string subdir;
	if (period == KP_Minute1)
	{
		the_map = &_exchg_m1_dbs;
		subdir = "min1";
	}
	else if (period == KP_Minute5)
	{
		the_map = &_exchg_m5_dbs;
		subdir = "min5";
	}
	else if (period == KP_DAY)
	{
		the_map = &_exchg_d1_dbs;
		subdir = "day";
	}
	else
		return WtLMDBPtr();

	auto it = the_map->find(exchg);
	if (it != the_map->end())
		return it->second;

	WtLMDBPtr dbPtr(new WtLMDB(true));
	std::string path = fmtutil::format("{}{}/{}/", _base_dir.c_str(), subdir.c_str(), exchg);
	if (!StdFile::exists(path.c_str()))
		return WtLMDBPtr();

	if (!dbPtr->open(path.c_str()))
	{
		pipe_btreader_log(_sink, LL_ERROR, "Opening {} db of {} failed: {}", subdir, exchg, dbPtr->errmsg());
		return WtLMDBPtr();
	}
	else
	{
		pipe_btreader_log(_sink, LL_DEBUG, "{} db of {} opened", subdir, exchg);
	}

	(*the_map)[exchg] = dbPtr;
	return dbPtr;
}

WtBtDtReaderAD::WtLMDBPtr WtBtDtReaderAD::get_t_db(const char* exchg, const char* code)
{
	std::string key = fmtutil::format<64>("{}.{}", exchg, code);
	auto it = _tick_dbs.find(key);
	if (it != _tick_dbs.end())
		return it->second;

	WtLMDBPtr dbPtr(new WtLMDB(true));
	std::string path = fmtutil::format("{}ticks/{}/{}", _base_dir.c_str(), exchg, code);
	if (!StdFile::exists(path.c_str()))
		return WtLMDBPtr();

	if (!dbPtr->open(path.c_str()))
	{
		pipe_btreader_log(_sink, LL_ERROR, "Opening tick db of {}.{} failed: {}", exchg, code, dbPtr->errmsg());
		return WtLMDBPtr();
	}
	else
	{
		pipe_btreader_log(_sink, LL_DEBUG, "Tick db of {}.{} opened", exchg, code);
	}

	_tick_dbs[key] = dbPtr;
	return dbPtr;
}
