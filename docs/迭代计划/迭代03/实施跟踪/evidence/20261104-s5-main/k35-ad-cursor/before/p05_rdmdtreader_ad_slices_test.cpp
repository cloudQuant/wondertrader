// p05 / WtDataStorageAD：WtRdmDtReaderAD 的 LMDB 切片接口（写入→读回→逐字段比对）
//
// 覆盖对象：
//  - WtRdmDtReaderAD::init（cfg==NULL / cfg!=NULL）
//  - WtRdmDtReaderAD::readKlineSliceByRange（全量载入 / bNeedNewer 增量 / 窗口裁剪 / 未实现周期）
//  - WtRdmDtReaderAD::readTickSliceByCount / readTickSliceByDate / readKlineSliceByCount
//  - WtRdmDtReaderAD::readOrdDtlSliceByRange / readOrdQueSliceByRange / readTransSliceByRange
//  - WtRdmDtReaderAD::get_k_db / get_t_db（交易所级 db + fs::create_directories 自愈）
//  - IRdmDtReader::init / IBaseDataMgr / IHotMgr 的宿主注入面（本文件自带 mock，不引核心）
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

#include "Includes/WTSMarcos.h"
#include "Includes/WTSTypes.h"
#include "Includes/WTSStruct.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/IBaseDataMgr.h"
#include "Includes/IHotMgr.h"
#include "Includes/IRdmDtReader.h"
#include "Share/TimeUtils.hpp"
#include "WtDataStorageAD/LMDBKeys.h"
#include "WtDataStorageAD/WtRdmDtReaderAD.h"
#include "WTSUtils/WtLMDB.hpp"

using namespace wtp;

#pragma pack(push, 1)
struct P05BarPackedV2
{
	uint32_t date;
	uint64_t time;
	double open, high, low, close, settle, money, vol, hold, add;
};

struct P05TickPackedV2
{
	char exchg[MAX_EXCHANGE_LENGTH];
	char code[MAX_INSTRUMENT_LENGTH];
	double price, open, high, low, settle_price, upper_limit, lower_limit;
	double total_volume, volume, total_turnover, turn_over, open_interest, diff_interest;
	uint32_t trading_date, action_date, action_time;
	double pre_close, pre_settle, pre_interest;
	double bid_prices[10], ask_prices[10], bid_qty[10], ask_qty[10];
};
#pragma pack(pop)

static_assert(sizeof(P05BarPackedV2) == 84, "packed-v2 bar test ABI mismatch");
static_assert(sizeof(P05TickPackedV2) == 508, "packed-v2 tick test ABI mismatch");

namespace wtp {

struct WtRdmDtReaderADTestAccess
{
	static void set_cursor_get_override(LMDBRangeCursorGetOverride callback, void* context)
	{
		WtRdmDtReaderAD::set_test_cursor_get_override(callback, context);
	}

	static void set_range_results_override(LMDBRangeResultsOverride callback, void* context)
	{
		WtRdmDtReaderAD::set_test_range_results_override(callback, context);
	}

	static bool tick_cache_state(WtRdmDtReaderAD& reader, const std::string& cache_key,
		std::vector<WTSTickStruct>& ticks, uint64_t& first_time, uint64_t& last_time)
	{
		auto it = reader._ticks_cache.find(cache_key);
		if (it == reader._ticks_cache.end())
			return false;
		ticks = it->second._ticks;
		first_time = it->second._first_tick_time;
		last_time = it->second._last_tick_time;
		return true;
	}

	static bool reserve_tick_cache(WtRdmDtReaderAD& reader, const std::string& cache_key, std::size_t capacity)
	{
		auto it = reader._ticks_cache.find(cache_key);
		if (it == reader._ticks_cache.end())
			return false;
		it->second._ticks.reserve(capacity);
		return true;
	}

	static bool has_tick_cache(const WtRdmDtReaderAD& reader, const std::string& cache_key)
	{
		return reader._ticks_cache.find(cache_key) != reader._ticks_cache.end();
	}

	static bool has_bar_cache(const WtRdmDtReaderAD& reader, const std::string& cache_key)
	{
		return reader._bars_cache.find(cache_key) != reader._bars_cache.end();
	}

	static bool bar_cache_state(WtRdmDtReaderAD& reader, const std::string& cache_key,
		std::vector<WTSBarStruct>& bars, uint64_t& last_time)
	{
		auto it = reader._bars_cache.find(cache_key);
		if (it == reader._bars_cache.end())
			return false;
		bars = it->second._bars;
		last_time = it->second._last_bar_time;
		return true;
	}

	static WtLMDB* tick_db(WtRdmDtReaderAD& reader, const std::string& db_key)
	{
		auto it = reader._tick_dbs.find(db_key);
		return it == reader._tick_dbs.end() ? NULL : it->second.get();
	}

	static WtLMDB* bar_db(WtRdmDtReaderAD& reader, const char* exchg, WTSKlinePeriod period)
	{
		WtRdmDtReaderAD::WtLMDBMap* dbs = NULL;
		if (period == KP_DAY)
			dbs = &reader._exchg_d1_dbs;
		else if (period == KP_Minute1)
			dbs = &reader._exchg_m1_dbs;
		else if (period == KP_Minute5)
			dbs = &reader._exchg_m5_dbs;
		if (dbs == NULL)
			return NULL;
		auto it = dbs->find(exchg);
		return it == dbs->end() ? NULL : it->second.get();
	}

	static bool replace_record(WtLMDB* db, const std::string& key, const std::string& value)
	{
		if (db == NULL || db->env() == NULL)
			return false;
		MDB_txn* txn = NULL;
		if (mdb_txn_begin(db->env(), NULL, 0, &txn) != MDB_SUCCESS)
			return false;
		MDB_val mkey = { key.size(), const_cast<char*>(key.data()) };
		MDB_val mdata = { value.size(), const_cast<char*>(value.data()) };
		const int put_status = mdb_put(txn, db->dbi(), &mkey, &mdata, 0);
		if (put_status != MDB_SUCCESS)
		{
			mdb_txn_abort(txn);
			return false;
		}
		return mdb_txn_commit(txn) == MDB_SUCCESS;
	}

	static bool delete_record(WtLMDB* db, const std::string& key)
	{
		if (db == NULL || db->env() == NULL)
			return false;
		MDB_txn* txn = NULL;
		if (mdb_txn_begin(db->env(), NULL, 0, &txn) != MDB_SUCCESS)
			return false;
		MDB_val mkey = { key.size(), const_cast<char*>(key.data()) };
		const int delete_status = mdb_del(txn, db->dbi(), &mkey, NULL);
		if (delete_status != MDB_SUCCESS)
		{
			mdb_txn_abort(txn);
			return false;
		}
		return mdb_txn_commit(txn) == MDB_SUCCESS;
	}
};

} // namespace wtp

namespace {

static void set_str(char* dst, size_t cap, const char* src)
{
	snprintf(dst, cap, "%s", src);
}

struct RdmRangeFailureState
{
	int range_starts = 0;
	int fail_on_range = 2;
};

struct RdmRangeResultsMismatchState
{
	int callback_count = 0;
	int mismatch_on_callback = 1;
	bool add_extra_key = true;
	bool injected = false;
};

static int fail_second_checked_range(void* context, MDB_cursor* cursor, MDB_val* key, MDB_val* data, MDB_cursor_op op)
{
	RdmRangeFailureState* state = static_cast<RdmRangeFailureState*>(context);
	if (op == MDB_SET_RANGE || op == MDB_FIRST)
	{
		++state->range_starts;
		if (state->range_starts == state->fail_on_range)
			return MDB_CORRUPTED;
	}
	return mdb_cursor_get(cursor, key, data, op);
}

static void inject_range_results_count_mismatch(void* context, ValueArray& keys, ValueArray& values)
{
	RdmRangeResultsMismatchState* state = static_cast<RdmRangeResultsMismatchState*>(context);
	++state->callback_count;
	if (state->callback_count != state->mismatch_on_callback || keys.empty() || values.empty())
		return;
	if (state->add_extra_key)
		keys.emplace_back(keys.front());
	else
		values.emplace_back(values.front());
	state->injected = true;
}

class ScopedRangeResultsMismatch
{
public:
	explicit ScopedRangeResultsMismatch(RdmRangeResultsMismatchState& state)
	{
		WtRdmDtReaderADTestAccess::set_range_results_override(&inject_range_results_count_mismatch, &state);
	}

	~ScopedRangeResultsMismatch()
	{
		WtRdmDtReaderADTestAccess::set_range_results_override(NULL, NULL);
	}

	ScopedRangeResultsMismatch(const ScopedRangeResultsMismatch&) = delete;
	ScopedRangeResultsMismatch& operator=(const ScopedRangeResultsMismatch&) = delete;
};

template<typename Value>
static std::string raw_bytes(const Value& value)
{
	return std::string((const char*)&value, sizeof(value));
}

static P05BarPackedV2 packed_bar_v2(const WTSBarStruct& bar)
{
	P05BarPackedV2 packed = {};
	packed.date = bar.date;
	packed.time = bar.time;
	packed.open = bar.open;
	packed.high = bar.high;
	packed.low = bar.low;
	packed.close = bar.close;
	packed.settle = bar.settle;
	packed.money = bar.money;
	packed.vol = bar.vol;
	packed.hold = bar.hold;
	packed.add = bar.add;
	return packed;
}

static WTSBarStructOld old_bar_v1(const WTSBarStruct& bar)
{
	WTSBarStructOld old_bar;
	old_bar.date = bar.date;
	old_bar.time = (uint32_t)bar.time;
	old_bar.open = bar.open;
	old_bar.high = bar.high;
	old_bar.low = bar.low;
	old_bar.close = bar.close;
	old_bar.settle = bar.settle;
	old_bar.money = bar.money;
	old_bar.vol = (uint32_t)bar.vol;
	old_bar.hold = (uint32_t)bar.hold;
	old_bar.add = (int32_t)bar.add;
	return old_bar;
}

static P05TickPackedV2 packed_tick_v2(const WTSTickStruct& tick)
{
	P05TickPackedV2 packed = {};
	memcpy(packed.exchg, tick.exchg, sizeof(packed.exchg));
	memcpy(packed.code, tick.code, sizeof(packed.code));
	packed.price = tick.price;
	packed.open = tick.open;
	packed.high = tick.high;
	packed.low = tick.low;
	packed.settle_price = tick.settle_price;
	packed.upper_limit = tick.upper_limit;
	packed.lower_limit = tick.lower_limit;
	packed.total_volume = tick.total_volume;
	packed.volume = tick.volume;
	packed.total_turnover = tick.total_turnover;
	packed.turn_over = tick.turn_over;
	packed.open_interest = tick.open_interest;
	packed.diff_interest = tick.diff_interest;
	packed.trading_date = tick.trading_date;
	packed.action_date = tick.action_date;
	packed.action_time = tick.action_time;
	packed.pre_close = tick.pre_close;
	packed.pre_settle = tick.pre_settle;
	packed.pre_interest = tick.pre_interest;
	memcpy(packed.bid_prices, tick.bid_prices, sizeof(packed.bid_prices));
	memcpy(packed.ask_prices, tick.ask_prices, sizeof(packed.ask_prices));
	memcpy(packed.bid_qty, tick.bid_qty, sizeof(packed.bid_qty));
	memcpy(packed.ask_qty, tick.ask_qty, sizeof(packed.ask_qty));
	return packed;
}

static WTSTickStructOld old_tick_v1(const WTSTickStruct& tick)
{
	WTSTickStructOld old_tick;
	set_str(old_tick.exchg, sizeof(old_tick.exchg), tick.exchg);
	set_str(old_tick.code, sizeof(old_tick.code), tick.code);
	old_tick.price = tick.price;
	old_tick.open = tick.open;
	old_tick.high = tick.high;
	old_tick.low = tick.low;
	old_tick.settle_price = tick.settle_price;
	old_tick.upper_limit = tick.upper_limit;
	old_tick.lower_limit = tick.lower_limit;
	old_tick.total_volume = (uint32_t)tick.total_volume;
	old_tick.volume = (uint32_t)tick.volume;
	old_tick.total_turnover = tick.total_turnover;
	old_tick.turn_over = tick.turn_over;
	old_tick.open_interest = (uint32_t)tick.open_interest;
	old_tick.diff_interest = (int32_t)tick.diff_interest;
	old_tick.trading_date = tick.trading_date;
	old_tick.action_date = tick.action_date;
	old_tick.action_time = tick.action_time;
	old_tick.pre_close = tick.pre_close;
	old_tick.pre_settle = tick.pre_settle;
	old_tick.pre_interest = (int32_t)tick.pre_interest;
	for (std::size_t level = 0; level < 10; ++level)
	{
		old_tick.bid_prices[level] = tick.bid_prices[level];
		old_tick.ask_prices[level] = tick.ask_prices[level];
		old_tick.bid_qty[level] = (uint32_t)tick.bid_qty[level];
		old_tick.ask_qty[level] = (uint32_t)tick.ask_qty[level];
	}
	return old_tick;
}

class ScopedCurrentPath
{
public:
	explicit ScopedCurrentPath(const std::filesystem::path& path)
		: _previous(std::filesystem::current_path())
	{
		std::filesystem::current_path(path);
	}

	~ScopedCurrentPath()
	{
		std::error_code ec;
		std::filesystem::current_path(_previous, ec);
	}

private:
	std::filesystem::path _previous;
};

//////////////////////////////////////////////////////////////////////////
// 宿主 mock：只提供本模块用到的三个回调
//////////////////////////////////////////////////////////////////////////
class P05BaseDataMgr : public IBaseDataMgr
{
public:
	P05BaseDataMgr()
	{
		_session = WTSSessionInfo::create("SHFE_DAY", "日盘", 0);
		_session->addTradingSection(0, 2359);
		_comm = WTSCommodityInfo::create("rb", "螺纹钢", "SHFE", "SHFE_DAY", "CHN");
		_comm->setSessionInfo(_session);
		_comm->setCategory(ContractCategory::CC_Future);
	}

	virtual ~P05BaseDataMgr()
	{
		_comm->release();
		_session->release();
	}

	WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		_last_pid = exchgpid == NULL ? "" : exchgpid;
		return _comm;
	}

	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		_last_pid = std::string(exchg == NULL ? "" : exchg) + "." + (pid == NULL ? "" : pid);
		return _comm;
	}

	WTSContractInfo* getContract(const char*, const char*, uint32_t) override { return NULL; }
	WTSArray* getContracts(const char*, uint32_t) override { return NULL; }
	WTSSessionInfo* getSession(const char* sid) override { _last_sid = sid; return _session; }
	WTSSessionInfo* getSessionByCode(const char*, const char*) override { return _session; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool) override { return false; }

	// 不做日历推导，直接令交易日 == 自然日，便于用固定键值断言
	uint32_t calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession) override
	{
		_pid_seen = stdPID == NULL ? "" : stdPID;
		_time_seen = uTime;
		_is_session = isSession;
		return uDate;
	}

	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool, bool isStart) override
	{
		return isStart ? ((uint64_t)tDate) * 1000000000ull + 90000000ull
			: ((uint64_t)tDate) * 1000000000ull + 150000000ull;
	}

	const std::string& last_pid() const { return _last_pid; }
	const std::string& pid_seen() const { return _pid_seen; }
	uint32_t time_seen() const { return _time_seen; }
	bool is_session() const { return _is_session; }

private:
	WTSSessionInfo*	_session = NULL;
	WTSCommodityInfo*	_comm = NULL;
	std::string		_last_pid;
	std::string		_pid_seen;
	std::string		_last_sid;
	uint32_t		_time_seen = 0;
	bool			_is_session = false;
};

class P05HotMgr : public IHotMgr
{
public:
	virtual ~P05HotMgr() {}

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
	const char* getRuleTag(const char* /*stdCode*/) override { return _tag.empty() ? "" : _tag.c_str(); }
	double getRuleFactor(const char*, const char*, uint32_t) override { return 1.0; }

	void set_rule_tag(const char* tag) { _tag = tag; }

private:
	std::string	_tag;
};

class P05RdmSink : public IRdmDtReaderSink
{
public:
	P05RdmSink(IBaseDataMgr* bd, IHotMgr* hot) : _bd(bd), _hot(hot) {}
	virtual ~P05RdmSink() {}


	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return _hot; }
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		_logs[ll]++;
		_total++;
		_messages.emplace_back(message == NULL ? "" : message);
	}

	uint32_t count_of(WTSLogLevel ll) const
	{
		auto it = _logs.find(ll);
		return it == _logs.end() ? 0u : it->second;
	}
	uint32_t total() const { return _total; }
	bool has_log(const std::string& text) const
	{
		for (const std::string& message : _messages)
			if (message.find(text) != std::string::npos)
				return true;
		return false;
	}

private:
	IBaseDataMgr*	_bd;
	IHotMgr*		_hot;
	wt_hashmap<WTSLogLevel, uint32_t> _logs;
	std::vector<std::string> _messages;
	uint32_t		_total = 0;
};

//////////////////////////////////////////////////////////////////////////
// 夹具
//////////////////////////////////////////////////////////////////////////
class P05RdmReaderADTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		_root = std::filesystem::temp_directory_path()
			/ ("p05_rdmad_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
		std::filesystem::remove_all(_root);
		std::filesystem::create_directories(_root);
	}

	void TearDown() override
	{
		_reader.reset();
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	static std::string key_str(const LMDBBarKey& key)
	{
		return std::string((const char*)&key, sizeof(key));
	}

	static std::string key_str(const LMDBHftKey& key)
	{
		return std::string((const char*)&key, sizeof(key));
	}

	static std::string raw_of(const WTSBarStruct& bar)
	{
		return std::string((const char*)&bar, sizeof(bar));
	}

	static std::string raw_of(const WTSTickStruct& tick)
	{
		return std::string((const char*)&tick, sizeof(tick));
	}

	static WTSBarStruct make_bar(uint32_t date, uint64_t time, double base)
	{
		WTSBarStruct bar;
		bar.date = date;
		bar.time = time;
		bar.open = base;
		bar.high = base + 2.0;
		bar.low = base - 2.0;
		bar.close = base + 1.0;
		bar.settle = base + 1.0;
		bar.money = base * 300.0;
		bar.vol = base * 3.0;
		bar.hold = base;
		bar.add = 1.0;
		return bar;
	}

	static WTSTickStruct make_tick(const char* exchg, const char* code, uint32_t tdate,
		uint32_t adate, uint32_t atime, double price)
	{
		WTSTickStruct tick;
		set_str(tick.exchg, sizeof(tick.exchg), exchg);
		set_str(tick.code, sizeof(tick.code), code);
		tick.price = price;
		tick.open = price - 1.0;
		tick.high = price + 1.0;
		tick.low = price - 1.0;
		tick.total_volume = price * 10.0;
		tick.volume = price;
		tick.trading_date = tdate;
		tick.action_date = adate;
		tick.action_time = atime;
		return tick;
	}

	// 返回按日期升序排列的 n 根日线（20240102 起，逐日 +1）
	std::vector<WTSBarStruct> make_day_bars(uint32_t n, double base)
	{
		std::vector<WTSBarStruct> bars;
		for(uint32_t i = 0; i < n; i++)
		{
			const uint32_t dt = 20240102 + i;
			bars.push_back(make_bar(dt, dt, base + i));
		}
		return bars;
	}

	void put_day_bars(const char* exchg, const char* code, const std::vector<WTSBarStruct>& bars)
	{
		const std::filesystem::path dir = _root / "day" / exchg;
		std::filesystem::create_directories(dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(dir.string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for(const WTSBarStruct& bar : bars)
			ASSERT_TRUE(query.put(key_str(LMDBBarKey(exchg, code, (uint32_t)bar.date)), raw_of(bar)));
		query.commit();
	}

	void put_min_bars(const char* subdir, const char* exchg, const char* code,
		const std::vector<WTSBarStruct>& bars)
	{
		const std::filesystem::path dir = _root / subdir / exchg;
		std::filesystem::create_directories(dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(dir.string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for(const WTSBarStruct& bar : bars)
			ASSERT_TRUE(query.put(key_str(LMDBBarKey(exchg, code, (uint32_t)bar.time)), raw_of(bar)));
		query.commit();
	}

	void put_ticks(const char* exchg, const char* code, const std::vector<WTSTickStruct>& ticks)
	{
		const std::filesystem::path dir = _root / "ticks" / exchg / code;
		std::filesystem::create_directories(dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(dir.string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for (const WTSTickStruct& tick : ticks)
			ASSERT_TRUE(query.put(key_str(LMDBHftKey(exchg, code, tick.trading_date, tick.action_time)), raw_of(tick)));
		query.commit();
	}

	void put_tick_records(const char* exchg, const char* code,
		const std::vector<std::pair<std::string, std::string>>& records)
	{
		const std::filesystem::path dir = _root / "ticks" / exchg / code;
		std::filesystem::create_directories(dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(dir.string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for (const auto& record : records)
			ASSERT_TRUE(query.put(record.first, record.second));
		query.commit();
	}

	void put_day_records(const char* exchg,
		const std::vector<std::pair<std::string, std::string>>& records)
	{
		const std::filesystem::path dir = _root / "day" / exchg;
		std::filesystem::create_directories(dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(dir.string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for (const auto& record : records)
			ASSERT_TRUE(query.put(record.first, record.second));
		query.commit();
	}

	void init_reader(bool with_cfg)
	{
		_bd.reset(new P05BaseDataMgr());
		_hot.reset(new P05HotMgr());
		_sink.reset(new P05RdmSink(_bd.get(), _hot.get()));
		_reader.reset(new WtRdmDtReaderAD());
		if(with_cfg)
		{
			WTSVariant* cfg = WTSVariant::createObject();
			cfg->append("path", _root.string().c_str());
			_reader->init(cfg, _sink.get());
			cfg->release();
		}
		else
		{
			_reader->init(NULL, _sink.get());
		}
	}

	std::filesystem::path _root;
	std::unique_ptr<P05BaseDataMgr>	_bd;
	std::unique_ptr<P05HotMgr>		_hot;
	std::unique_ptr<P05RdmSink>		_sink;
	std::unique_ptr<WtRdmDtReaderAD>	_reader;
};

}
//////////////////////////////////////////////////////////////////////////

TEST_F(P05RdmReaderADTest, NullCfgLeavesEmptyBaseDirButSinksAreWired)
{
	init_reader(false);

	// cfg==NULL：IRdmDtReader::init 仍拿到 sink/basedata，只是 _base_dir 为空
	EXPECT_EQ(_bd->last_pid(), "");
	// 空路径读请求与 CWD 无写入由下面的隔离目录回归单独验证。
	EXPECT_EQ(_sink->count_of(LL_DEBUG), 0u);
}

// cfg==NULL used to make the database getters create relative directories under CWD.
// Run both public range paths from a private empty working directory.
TEST_F(P05RdmReaderADTest, NullCfgRangeReadsRejectWithoutCwdWrites)
{
	const std::filesystem::path isolated_cwd = _root / "isolated-cwd";
	std::filesystem::create_directories(isolated_cwd);
	ScopedCurrentPath cwd(isolated_cwd);
	init_reader(false);

	EXPECT_EQ(_reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
		202401020000, 202401200000), nullptr);
	EXPECT_EQ(_reader->readTickSliceByRange("SHFE.rb.rb2409",
		20240101090000000ull, 20240101150000000ull), nullptr);
	EXPECT_FALSE(std::filesystem::exists(isolated_cwd / "day"));
	EXPECT_FALSE(std::filesystem::exists(isolated_cwd / "ticks"));
	EXPECT_EQ(_sink->count_of(LL_ERROR), 2u);
	EXPECT_TRUE(_sink->has_log("base data path is not configured"));
}

TEST_F(P05RdmReaderADTest, NotProvidedSlicesReturnNull)
{
	init_reader(true);

	// AD reader 尚未提供的明细接口与默认复权因子行为。
	EXPECT_EQ(_reader->readOrdDtlSliceByRange("SHFE.rb.rb2409", 202401020000, 202401200000), nullptr);
	EXPECT_EQ(_reader->readOrdQueSliceByRange("SHFE.rb.rb2409", 202401020000, 202401200000), nullptr);
	EXPECT_EQ(_reader->readTransSliceByRange("SHFE.rb.rb2409", 202401020000, 202401200000), nullptr);
	EXPECT_EQ(_reader->getAdjFactorByDate("SHFE.rb.rb2409", 20240102), 1.0);
	_reader->clearCache();
}

TEST_F(P05RdmReaderADTest, DayKlineSliceRoundTripFieldByField)
{
	const std::vector<WTSBarStruct> bars = make_day_bars(15, 3500.0);
	put_day_bars("SHFE", "rb2409", bars);

	init_reader(true);

	WTSKlineSlice* slice = _reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY, 202401020000, 202401200000);
	ASSERT_NE(slice, nullptr);
	EXPECT_EQ(slice->get_block_size(0), 15u);
	// calcTradingDate 的入参要能被宿主看到：交易日口径 = 自然日 + 时分
	EXPECT_EQ(_bd->pid_seen(), "SHFE.rb");
	EXPECT_EQ(_bd->is_session(), false);

	ASSERT_EQ(slice->get_block_counts(), (std::size_t)1);
	const WTSBarStruct* out = (const WTSBarStruct*)slice->get_block_addr(0);
	ASSERT_NE(out, nullptr);
	for(uint32_t i = 0; i < 15; i++)
	{
		EXPECT_EQ(out[i].date, bars[i].date) << i;
		EXPECT_DOUBLE_EQ(out[i].open, bars[i].open) << i;
		EXPECT_DOUBLE_EQ(out[i].high, bars[i].high) << i;
		EXPECT_DOUBLE_EQ(out[i].low, bars[i].low) << i;
		EXPECT_DOUBLE_EQ(out[i].close, bars[i].close) << i;
		EXPECT_DOUBLE_EQ(out[i].money, bars[i].money) << i;
	}
	slice->release();
}

TEST_F(P05RdmReaderADTest, ConfiguredRootReadsKlineAndTick)
{
	const std::vector<WTSBarStruct> bars = make_day_bars(1, 3500.0);
	put_day_bars("SHFE", "rb2409", bars);
	const std::vector<WTSTickStruct> ticks = {
		make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 3600.0)
	};
	put_ticks("SHFE", "rb2409", ticks);

	init_reader(true);
	WTSKlineSlice* kline_slice = _reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
		202401020000, 202401020000);
	ASSERT_NE(kline_slice, nullptr);
	ASSERT_EQ(kline_slice->get_block_counts(), (std::size_t)1);
	EXPECT_EQ(kline_slice->get_block_size(0), 1u);
	const WTSBarStruct* kline = (const WTSBarStruct*)kline_slice->get_block_addr(0);
	ASSERT_NE(kline, nullptr);
	EXPECT_EQ(kline[0].date, bars[0].date);
	EXPECT_DOUBLE_EQ(kline[0].close, bars[0].close);
	kline_slice->release();

	WTSTickSlice* tick_slice = _reader->readTickSliceByRange("SHFE.rb.rb2409",
		20240102ull * 1000000000ull + 90000000ull,
		20240102ull * 1000000000ull + 150000000ull);
	ASSERT_NE(tick_slice, nullptr);
	ASSERT_EQ(tick_slice->size(), 1u);
	const WTSTickStruct* tick = tick_slice->at(0);
	ASSERT_NE(tick, nullptr);
	EXPECT_EQ(tick->trading_date, ticks[0].trading_date);
	EXPECT_EQ(tick->action_time, ticks[0].action_time);
	EXPECT_DOUBLE_EQ(tick->price, ticks[0].price);
	tick_slice->release();
}

TEST_F(P05RdmReaderADTest, WindowClippingUsesCachedBars)
{
	std::vector<WTSBarStruct> bars = make_day_bars(20, 100.0);
	// 乱序入库，读回必须是 LMDB 键序（日期升序）
	std::reverse(bars.begin(), bars.end());
	put_day_bars("DCE", "i2409", bars);
	std::reverse(bars.begin(), bars.end());

	init_reader(true);

	// 只要 [20240105, 20240112]，即 bars[3..10]
	WTSKlineSlice* slice = _reader->readKlineSliceByRange("DCE.i.i2409", KP_DAY, 202401050000, 202401120000);
	if(slice == nullptr)
	{
		// DCE 的品种段会被解析成 product="i"，交易所/代码组合仍会命中同一个 db
		FAIL() << "窗口切片返回空，键区间与 db 布局不符";
	}
	const WTSBarStruct* out = (const WTSBarStruct*)slice->get_block_addr(0);
	const uint32_t cnt = slice->get_block_size(0);
	ASSERT_GT(cnt, 0u);
	// 返回的每一根都必须能在入库集合里逐字段找到
	for(uint32_t i = 0; i < cnt; i++)
	{
		bool bFound = false;
		for(const WTSBarStruct& seed : bars)
		{
			if(seed.date == out[i].date)
			{
				EXPECT_DOUBLE_EQ(seed.open, out[i].open) << i;
				EXPECT_DOUBLE_EQ(seed.close, out[i].close) << i;
				bFound = true;
				break;
			}
		}
		EXPECT_TRUE(bFound) << "bar @" << out[i].date;
	}
	// 重复窗口查询应继续从有效的交易所级 LMDB 句柄读取并返回相同窗口。
	WTSKlineSlice* repeated = _reader->readKlineSliceByRange("DCE.i.i2409", KP_DAY, 202401050000, 202401120000);
	ASSERT_NE(repeated, nullptr);
	EXPECT_EQ(repeated->get_block_size(0), cnt);
	const WTSBarStruct* repeated_out = (const WTSBarStruct*)repeated->get_block_addr(0);
	ASSERT_NE(repeated_out, nullptr);
	for(uint32_t i = 0; i < cnt; i++)
	{
		EXPECT_EQ(repeated_out[i].date, out[i].date) << i;
		EXPECT_DOUBLE_EQ(repeated_out[i].open, out[i].open) << i;
	}
	repeated->release();
	slice->release();
}

// 同一交易所/周期的数据库句柄会被不同合约与重复查询共享，缓存命中应保持有效。
TEST_F(P05RdmReaderADTest, RepeatedKlineSlicesAndCodesReuseExchangeDatabase)
{
	const std::vector<WTSBarStruct> bars = make_day_bars(6, 700.0);
	const std::vector<WTSBarStruct> other_bars = make_day_bars(4, 900.0);
	put_day_bars("GFEX", "lc2409", bars);
	put_day_bars("GFEX", "lc2410", other_bars);
	init_reader(true);

	for(uint32_t round = 0; round < 3; round++)
	{
		WTSKlineSlice* slice = _reader->readKlineSliceByRange("GFEX.lc.lc2409", KP_DAY,
			202401020000, 202401100000);
		ASSERT_NE(slice, nullptr) << round;
		EXPECT_EQ(slice->get_block_size(0), 6u) << round;
		slice->release();
	}

	WTSKlineSlice* other = _reader->readKlineSliceByRange("GFEX.lc.lc2410", KP_DAY,
		202401020000, 202401100000);
	ASSERT_NE(other, nullptr);
	ASSERT_EQ(other->get_block_size(0), 4u);
	EXPECT_DOUBLE_EQ(other->get_block_addr(0)[0].open, 900.0);
	other->release();

	WTSKlineSlice* first_again = _reader->readKlineSliceByRange("GFEX.lc.lc2409", KP_DAY,
		202401020000, 202401100000);
	ASSERT_NE(first_again, nullptr);
	ASSERT_EQ(first_again->get_block_size(0), 6u);
	EXPECT_DOUBLE_EQ(first_again->get_block_addr(0)[0].open, 700.0);
	first_again->release();
}

TEST_F(P05RdmReaderADTest, MinuteBarsUseTimeAsKeyTail)
{
	std::vector<WTSBarStruct> m1;
	for(uint32_t i = 0; i < 12; i++)
	{
		const uint64_t t = TimeUtils::timeToMinBar(20240301, 931 + i);
		m1.push_back(make_bar(20240301, t, 50.0 + i));
	}
	put_min_bars("min1", "CZCE", "SR409", m1);

	init_reader(true);

	WTSKlineSlice* slice = _reader->readKlineSliceByRange("CZCE.SR.SR409", KP_Minute1,
		202403010931, 202403011000);
	ASSERT_NE(slice, nullptr);
	const WTSBarStruct* out = (const WTSBarStruct*)slice->get_block_addr(0);
	const uint32_t cnt = slice->get_block_size(0);
	EXPECT_GT(cnt, 0u);
	EXPECT_LE(cnt, 12u);
	// 分钟线时间形如 (date-19900000)*10000+hhmm，必然大于裸日期
	if(cnt > 0)
		EXPECT_GT(out[0].time, 20240301u);
	slice->release();

	// min5 目录不存在 → db 是空的；空结果应安全返回。
}

TEST_F(P05RdmReaderADTest, OutOfRangeEndBeforeFirstBarYieldsEmptySlice)
{
	const std::vector<WTSBarStruct> bars = make_day_bars(8, 200.0);
	put_day_bars("INE", "sc2409", bars);

	init_reader(true);

	// etime 早于第一根（20240102）：lower_bound 命中首元素后被 pBar-- 掏成 0 条
	WTSKlineSlice* slice = _reader->readKlineSliceByRange("INE.sc.sc2409", KP_DAY,
		202312010000, 202312310000);
	if(slice != NULL)
	{
		EXPECT_EQ(slice->get_block_size(0), 0u);
		slice->release();
	}
}

TEST_F(P05RdmReaderADTest, UnknownPeriodYieldsNullSlice)
{
	init_reader(true);
	// get_k_db 只认 min1/min5/day
	EXPECT_EQ(_reader->readKlineSliceByRange("SHFE.rb.rb2409", (WTSKlinePeriod)200,
		202401020000, 202401200000), nullptr);
}

//////////////////////////////////////////////////////////////////////////
// Empty-result boundaries in WtRdmDtReaderAD. Each case owns a new
// reader and a single database setup so one failure cannot hide later cases.
// The AD slice API represents no matching records as NULL or an empty slice.
//////////////////////////////////////////////////////////////////////////
static void expect_empty_kline_slice(WTSKlineSlice* slice)
{
	if (slice == NULL)
		return;
	for (std::size_t i = 0; i < slice->get_block_counts(); i++)
		EXPECT_EQ(0u, slice->get_block_size(i));
	slice->release();
}

static void expect_empty_tick_slice(WTSTickSlice* slice)
{
	if (slice == NULL)
		return;
	for (std::size_t i = 0; i < slice->get_block_counts(); i++)
		EXPECT_EQ(0u, slice->get_block_size(i));
	slice->release();
}

TEST_F(P05RdmReaderADTest, EmptyDailyDatabaseReturnsEmptyKlineSlice)
{
	init_reader(true);
	// No prior read populates or moves the day database cache.
	expect_empty_kline_slice(_reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
		202401020000, 202401200000));
}

TEST_F(P05RdmReaderADTest, DailyDatabaseWithOtherContractReturnsEmptyKlineSlice)
{
	init_reader(true);
	put_day_bars("SHFE", "hc2409", make_day_bars(4, 4000.0));
	// The existing SHFE DB contains bars, but none for rb2409.
	expect_empty_kline_slice(_reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
		202401020000, 202401200000));
}

TEST_F(P05RdmReaderADTest, EmptyMinuteDatabaseReturnsEmptyKlineSlice)
{
	init_reader(true);
	// get_k_db creates a valid, empty min1/CZCE database for this reader.
	expect_empty_kline_slice(_reader->readKlineSliceByRange("CZCE.SR.SR999", KP_Minute1,
		202403010931, 202403011000));
}

TEST_F(P05RdmReaderADTest, EmptyTickDatabaseReturnsEmptyTickSlice)
{
	init_reader(true);
	// get_t_db creates an empty but valid database for this exact contract.
	expect_empty_tick_slice(_reader->readTickSliceByRange("SHFE.rb.rb2409",
		20240101090000000ull, 20240101150000000ull));
}

TEST_F(P05RdmReaderADTest, TickCountCheckedQueryFailureCanRetry)
{
	const WTSTickStruct tick = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 77.0);
	put_ticks("SHFE", "rb2409", {tick});
	init_reader(true);

	RdmRangeFailureState failure;
	failure.fail_on_range = 1;
	WtRdmDtReaderADTestAccess::set_cursor_get_override(&fail_second_checked_range, &failure);
	WTSTickSlice* failed = _reader->readTickSliceByCount("SHFE.rb.rb2409", 1, 20240102150000000ull);
	WtRdmDtReaderADTestAccess::set_cursor_get_override(NULL, NULL);
	EXPECT_EQ(failed, nullptr);
	if (failed != nullptr)
		failed->release();
	EXPECT_EQ(failure.range_starts, 1);
	EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_tick_cache(*_reader, "SHFE.rb.rb2409#count-history"));

	WTSTickSlice* recovered = _reader->readTickSliceByCount("SHFE.rb.rb2409", 1, 20240102150000000ull);
	ASSERT_NE(recovered, nullptr);
	ASSERT_EQ(recovered->size(), 1u);
	EXPECT_DOUBLE_EQ(recovered->at(0)->price, tick.price);
	recovered->release();
}

TEST_F(P05RdmReaderADTest, TickDateCheckedQueryFailureCanRetry)
{
	const WTSTickStruct tick = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 77.0);
	put_ticks("SHFE", "rb2409", {tick});
	init_reader(true);

	RdmRangeFailureState failure;
	failure.fail_on_range = 1;
	WtRdmDtReaderADTestAccess::set_cursor_get_override(&fail_second_checked_range, &failure);
	WTSTickSlice* failed = _reader->readTickSliceByDate("SHFE.rb.rb2409", 20240102);
	WtRdmDtReaderADTestAccess::set_cursor_get_override(NULL, NULL);
	EXPECT_EQ(failed, nullptr);
	if (failed != nullptr)
		failed->release();
	EXPECT_EQ(failure.range_starts, 1);
	EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_tick_cache(*_reader, "SHFE.rb.rb2409#date#20240102"));

	WTSTickSlice* recovered = _reader->readTickSliceByDate("SHFE.rb.rb2409", 20240102);
	ASSERT_NE(recovered, nullptr);
	ASSERT_EQ(recovered->size(), 1u);
	EXPECT_DOUBLE_EQ(recovered->at(0)->price, tick.price);
	recovered->release();
}

TEST_F(P05RdmReaderADTest, KlineCountCheckedQueryFailureCanRetry)
{
	const WTSBarStruct bar = make_bar(20240102, 20240102, 77.0);
	put_day_bars("SHFE", "rb2409", {bar});
	init_reader(true);
	const std::string cache_key = "SHFE.rb.rb2409#" + std::to_string((uint32_t)KP_DAY);

	RdmRangeFailureState failure;
	failure.fail_on_range = 1;
	WtRdmDtReaderADTestAccess::set_cursor_get_override(&fail_second_checked_range, &failure);
	WTSKlineSlice* failed = _reader->readKlineSliceByCount("SHFE.rb.rb2409", KP_DAY, 1, 202401030000ull);
	WtRdmDtReaderADTestAccess::set_cursor_get_override(NULL, NULL);
	EXPECT_EQ(failed, nullptr);
	if (failed != nullptr)
		failed->release();
	EXPECT_EQ(failure.range_starts, 1);
	EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_bar_cache(*_reader, cache_key));

	WTSKlineSlice* recovered = _reader->readKlineSliceByCount("SHFE.rb.rb2409", KP_DAY, 1, 202401030000ull);
	ASSERT_NE(recovered, nullptr);
	ASSERT_EQ(recovered->get_block_size(0), 1u);
	const WTSBarStruct* recovered_bars = (const WTSBarStruct*)recovered->get_block_addr(0);
	ASSERT_NE(recovered_bars, nullptr);
	EXPECT_DOUBLE_EQ(recovered_bars[0].open, bar.open);
	recovered->release();
}

TEST_F(P05RdmReaderADTest, TickCountRejectsMismatchedCheckedRangeResultsAndCanRetry)
{
	const WTSTickStruct tick = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 77.0);
	put_ticks("SHFE", "rb2409", {tick});
	for (int direction = 0; direction != 2; ++direction)
	{
		SCOPED_TRACE(direction == 0 ? "extra key" : "extra value");
		_reader.reset();
		init_reader(true);
		RdmRangeResultsMismatchState mismatch;
		mismatch.add_extra_key = (direction == 0);
		WTSTickSlice* failed = NULL;
		{
			ScopedRangeResultsMismatch injection(mismatch);
			failed = _reader->readTickSliceByCount("SHFE.rb.rb2409", 1, 20240102150000000ull);
		}
		EXPECT_EQ(failed, nullptr);
		if (failed != nullptr)
			failed->release();
		EXPECT_EQ(mismatch.callback_count, 1);
		EXPECT_TRUE(mismatch.injected);
		EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_tick_cache(*_reader, "SHFE.rb.rb2409#count-history"));

		WTSTickSlice* recovered = _reader->readTickSliceByCount("SHFE.rb.rb2409", 1, 20240102150000000ull);
		ASSERT_NE(recovered, nullptr);
		ASSERT_EQ(recovered->size(), 1u);
		EXPECT_DOUBLE_EQ(recovered->at(0)->price, tick.price);
		recovered->release();
	}
}

TEST_F(P05RdmReaderADTest, TickDateRejectsMismatchedCheckedRangeResultsAndCanRetry)
{
	const WTSTickStruct tick = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 77.0);
	put_ticks("SHFE", "rb2409", {tick});
	for (int direction = 0; direction != 2; ++direction)
	{
		SCOPED_TRACE(direction == 0 ? "extra key" : "extra value");
		_reader.reset();
		init_reader(true);
		RdmRangeResultsMismatchState mismatch;
		mismatch.add_extra_key = (direction == 0);
		WTSTickSlice* failed = NULL;
		{
			ScopedRangeResultsMismatch injection(mismatch);
			failed = _reader->readTickSliceByDate("SHFE.rb.rb2409", 20240102);
		}
		EXPECT_EQ(failed, nullptr);
		if (failed != nullptr)
			failed->release();
		EXPECT_EQ(mismatch.callback_count, 1);
		EXPECT_TRUE(mismatch.injected);
		EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_tick_cache(*_reader, "SHFE.rb.rb2409#date#20240102"));

		WTSTickSlice* recovered = _reader->readTickSliceByDate("SHFE.rb.rb2409", 20240102);
		ASSERT_NE(recovered, nullptr);
		ASSERT_EQ(recovered->size(), 1u);
		EXPECT_DOUBLE_EQ(recovered->at(0)->price, tick.price);
		recovered->release();
	}
}

TEST_F(P05RdmReaderADTest, TickRangeRejectsMismatchedCheckedRangeResultsWithoutPartialPublish)
{
	const WTSTickStruct older = make_tick("SHFE", "rb2409", 20240102, 20240102, 92900000, 10.0);
	const WTSTickStruct middle = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 20.0);
	const WTSTickStruct newer = make_tick("SHFE", "rb2409", 20240102, 20240102, 93100000, 30.0);
	put_ticks("SHFE", "rb2409", {older, middle, newer});
	const uint64_t base = 20240102ull * 1000000000ull;
	for (int direction = 0; direction != 2; ++direction)
	{
		SCOPED_TRACE(direction == 0 ? "extra key" : "extra value");
		_reader.reset();
		init_reader(true);
		RdmRangeResultsMismatchState initial_mismatch;
		initial_mismatch.add_extra_key = (direction == 0);
		WTSTickSlice* initial_failure = NULL;
		{
			ScopedRangeResultsMismatch injection(initial_mismatch);
			initial_failure = _reader->readTickSliceByRange("SHFE.rb.rb2409",
				base + 93000000ull, base + 93000001ull);
		}
		EXPECT_EQ(initial_failure, nullptr);
		if (initial_failure != nullptr)
			initial_failure->release();
		EXPECT_EQ(initial_mismatch.callback_count, 1);
		EXPECT_TRUE(initial_mismatch.injected);
		EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_tick_cache(*_reader, "SHFE.rb.rb2409"));

		WTSTickSlice* warm = _reader->readTickSliceByRange("SHFE.rb.rb2409",
			base + 93000000ull, base + 93000001ull);
		ASSERT_NE(warm, nullptr);
		ASSERT_EQ(warm->size(), 1u);
		warm->release();
		std::vector<WTSTickStruct> before_ticks;
		uint64_t before_first = 0;
		uint64_t before_last = 0;
		ASSERT_TRUE(WtRdmDtReaderADTestAccess::tick_cache_state(
			*_reader, "SHFE.rb.rb2409", before_ticks, before_first, before_last));
		ASSERT_EQ(before_ticks.size(), 1u);
		EXPECT_EQ(before_ticks[0].action_time, middle.action_time);

		RdmRangeResultsMismatchState later_mismatch;
		later_mismatch.add_extra_key = (direction == 0);
		later_mismatch.mismatch_on_callback = 2;
		WTSTickSlice* staged_failure = NULL;
		{
			ScopedRangeResultsMismatch injection(later_mismatch);
			staged_failure = _reader->readTickSliceByRange("SHFE.rb.rb2409",
				base + 92900000ull, base + 93100001ull);
		}
		EXPECT_EQ(staged_failure, nullptr);
		if (staged_failure != nullptr)
			staged_failure->release();
		EXPECT_EQ(later_mismatch.callback_count, 2);
		EXPECT_TRUE(later_mismatch.injected);

		std::vector<WTSTickStruct> after_ticks;
		uint64_t after_first = 0;
		uint64_t after_last = 0;
		ASSERT_TRUE(WtRdmDtReaderADTestAccess::tick_cache_state(
			*_reader, "SHFE.rb.rb2409", after_ticks, after_first, after_last));
		ASSERT_EQ(after_ticks.size(), before_ticks.size());
		EXPECT_EQ(after_first, before_first);
		EXPECT_EQ(after_last, before_last);
		EXPECT_EQ(after_ticks[0].action_time, middle.action_time);
		EXPECT_DOUBLE_EQ(after_ticks[0].price, middle.price);

		WTSTickSlice* recovered = _reader->readTickSliceByRange("SHFE.rb.rb2409",
			base + 92900000ull, base + 93100001ull);
		ASSERT_NE(recovered, nullptr);
		ASSERT_EQ(recovered->size(), 3u);
		EXPECT_DOUBLE_EQ(recovered->at(0)->price, older.price);
		EXPECT_DOUBLE_EQ(recovered->at(1)->price, middle.price);
		EXPECT_DOUBLE_EQ(recovered->at(2)->price, newer.price);
		recovered->release();
	}
}

TEST_F(P05RdmReaderADTest, KlineCountRejectsMismatchedCheckedRangeResultsAndCanRetry)
{
	const WTSBarStruct bar = make_bar(20240102, 20240102, 77.0);
	put_day_bars("SHFE", "rb2409", {bar});
	for (int direction = 0; direction != 2; ++direction)
	{
		SCOPED_TRACE(direction == 0 ? "extra key" : "extra value");
		_reader.reset();
		init_reader(true);
		const std::string cache_key = "SHFE.rb.rb2409#" + std::to_string((uint32_t)KP_DAY);
		RdmRangeResultsMismatchState mismatch;
		mismatch.add_extra_key = (direction == 0);
		WTSKlineSlice* failed = NULL;
		{
			ScopedRangeResultsMismatch injection(mismatch);
			failed = _reader->readKlineSliceByCount("SHFE.rb.rb2409", KP_DAY, 1, 202401030000ull);
		}
		EXPECT_EQ(failed, nullptr);
		if (failed != nullptr)
			failed->release();
		EXPECT_EQ(mismatch.callback_count, 1);
		EXPECT_TRUE(mismatch.injected);
		EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_bar_cache(*_reader, cache_key));

		WTSKlineSlice* recovered = _reader->readKlineSliceByCount("SHFE.rb.rb2409", KP_DAY, 1, 202401030000ull);
		ASSERT_NE(recovered, nullptr);
		ASSERT_EQ(recovered->get_block_size(0), 1u);
		const WTSBarStruct* recovered_bars = (const WTSBarStruct*)recovered->get_block_addr(0);
		ASSERT_NE(recovered_bars, nullptr);
		EXPECT_DOUBLE_EQ(recovered_bars[0].open, bar.open);
		recovered->release();
	}
}

TEST_F(P05RdmReaderADTest, KlineRangeRejectsMismatchedCheckedRangeResultsWithoutPartialPublish)
{
	const WTSBarStruct first = make_bar(20240102, 20240102, 77.0);
	const WTSBarStruct second = make_bar(20240103, 20240103, 88.0);
	put_day_bars("SHFE", "rb2409", {first});
	const std::string second_key = key_str(LMDBBarKey("SHFE", "rb2409", second.date));
	for (int direction = 0; direction != 2; ++direction)
	{
		SCOPED_TRACE(direction == 0 ? "extra key" : "extra value");
		_reader.reset();
		if (direction != 0)
		{
			WtLMDB cleanup_db(false);
			const std::filesystem::path day_dir = _root / "day" / "SHFE";
			ASSERT_TRUE(cleanup_db.open(day_dir.string().c_str(), 8 * 1024 * 1024));
			{
				WtLMDBQuery initialize_default_dbi(cleanup_db);
				initialize_default_dbi.commit();
			}
			ASSERT_TRUE(WtRdmDtReaderADTestAccess::delete_record(&cleanup_db, second_key));
		}
		init_reader(true);
		const std::string cache_key = "SHFE.rb.rb2409#" + std::to_string((uint32_t)KP_DAY);
		RdmRangeResultsMismatchState initial_mismatch;
		initial_mismatch.add_extra_key = (direction == 0);
		WTSKlineSlice* initial_failure = NULL;
		{
			ScopedRangeResultsMismatch injection(initial_mismatch);
			initial_failure = _reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
				202401020000ull, 202401030000ull);
		}
		EXPECT_EQ(initial_failure, nullptr);
		if (initial_failure != nullptr)
			initial_failure->release();
		EXPECT_EQ(initial_mismatch.callback_count, 1);
		EXPECT_TRUE(initial_mismatch.injected);
		EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_bar_cache(*_reader, cache_key));

		WTSKlineSlice* warm = _reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
			202401020000ull, 202401030000ull);
		ASSERT_NE(warm, nullptr);
		ASSERT_EQ(warm->get_block_size(0), 1u);
		warm->release();
		std::vector<WTSBarStruct> before_bars;
		uint64_t before_last = 0;
		ASSERT_TRUE(WtRdmDtReaderADTestAccess::bar_cache_state(*_reader, cache_key, before_bars, before_last));
		ASSERT_EQ(before_bars.size(), 1u);
		ASSERT_TRUE(WtRdmDtReaderADTestAccess::replace_record(
			WtRdmDtReaderADTestAccess::bar_db(*_reader, "SHFE", KP_DAY),
			second_key, raw_of(second)));

		RdmRangeResultsMismatchState later_mismatch;
		later_mismatch.add_extra_key = (direction == 0);
		WTSKlineSlice* staged_failure = NULL;
		{
			ScopedRangeResultsMismatch injection(later_mismatch);
			staged_failure = _reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
				202401020000ull, 202401040000ull);
		}
		EXPECT_EQ(staged_failure, nullptr);
		if (staged_failure != nullptr)
			staged_failure->release();
		EXPECT_EQ(later_mismatch.callback_count, 1);
		EXPECT_TRUE(later_mismatch.injected);

		std::vector<WTSBarStruct> after_bars;
		uint64_t after_last = 0;
		ASSERT_TRUE(WtRdmDtReaderADTestAccess::bar_cache_state(*_reader, cache_key, after_bars, after_last));
		ASSERT_EQ(after_bars.size(), before_bars.size());
		EXPECT_EQ(after_last, before_last);
		EXPECT_EQ(after_bars[0].date, before_bars[0].date);
		EXPECT_DOUBLE_EQ(after_bars[0].open, before_bars[0].open);

		WTSKlineSlice* recovered = _reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
			202401020000ull, 202401040000ull);
		ASSERT_NE(recovered, nullptr);
		ASSERT_EQ(recovered->get_block_size(0), 2u);
		const WTSBarStruct* recovered_bars = (const WTSBarStruct*)recovered->get_block_addr(0);
		ASSERT_NE(recovered_bars, nullptr);
		EXPECT_DOUBLE_EQ(recovered_bars[0].open, first.open);
		EXPECT_DOUBLE_EQ(recovered_bars[1].open, second.open);
		recovered->release();
	}
}

TEST_F(P05RdmReaderADTest, TickDatabaseWithoutMatchingTradingDateReturnsEmptySlice)
{
	init_reader(true);
	const std::filesystem::path tdir = _root / "ticks" / "SHFE" / "rb2409";
	std::filesystem::create_directories(tdir);
	{
		WtLMDB db(false);
		ASSERT_TRUE(db.open(tdir.string().c_str(), 4 * 1024 * 1024));
		WtLMDBQuery query(db);
		const WTSTickStruct tick = make_tick("SHFE", "rb2409", 20240201, 20240201,
			93000000, 3600.0);
		ASSERT_TRUE(query.put(key_str(LMDBHftKey("SHFE", "rb2409", 20240201, 93000000)),
			raw_of(tick)));
		query.commit();
	}
	// Same contract has a record, but the query is for the prior trading day.
	expect_empty_tick_slice(_reader->readTickSliceByRange("SHFE.rb.rb2409",
		20240101090000000ull, 20240101150000000ull));
}

// These red controls use physical LMDB records whose values are one byte
// shorter or longer than the current ABI. Run before the reader fix to prove
// the old code accepts/copies both malformed layouts.
TEST_F(P05RdmReaderADTest, TickCountRejectsShortAndLongLmdbValues)
{
	for (const int delta : {-1, 1})
	{
		SCOPED_TRACE(delta);
		_reader.reset();
		WTSTickStruct tick = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 77.0);
		std::string value = raw_of(tick);
		if (delta < 0)
			value.resize(value.size() - 1);
		else
			value.push_back('x');
		const std::string key = key_str(LMDBHftKey("SHFE", "rb2409", 20240102, 93000000));
		put_tick_records("SHFE", "rb2409", {{key, value}});
		init_reader(true);

		WTSTickSlice* slice = _reader->readTickSliceByCount("SHFE.rb.rb2409", 1, 20240102150000000ull);
		EXPECT_EQ(slice, nullptr);
		if (slice != nullptr)
			slice->release();
		EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_tick_cache(*_reader, "SHFE.rb.rb2409#count-history"));
		const WTSTickStruct repaired = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 88.0);
		ASSERT_TRUE(WtRdmDtReaderADTestAccess::replace_record(
			WtRdmDtReaderADTestAccess::tick_db(*_reader, "SHFE.rb2409"), key, raw_of(repaired)));
		slice = _reader->readTickSliceByCount("SHFE.rb.rb2409", 1, 20240102150000000ull);
		ASSERT_NE(slice, nullptr);
		ASSERT_EQ(slice->size(), 1u);
		EXPECT_DOUBLE_EQ(slice->at(0)->price, repaired.price);
		slice->release();
	}
}

TEST_F(P05RdmReaderADTest, TickDateRejectsShortAndLongLmdbValues)
{
	for (const int delta : {-1, 1})
	{
		SCOPED_TRACE(delta);
		_reader.reset();
		WTSTickStruct tick = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 77.0);
		std::string value = raw_of(tick);
		if (delta < 0)
			value.resize(value.size() - 1);
		else
			value.push_back('x');
		const std::string key = key_str(LMDBHftKey("SHFE", "rb2409", 20240102, 93000000));
		put_tick_records("SHFE", "rb2409", {{key, value}});
		init_reader(true);

		WTSTickSlice* slice = _reader->readTickSliceByDate("SHFE.rb.rb2409", 20240102);
		EXPECT_EQ(slice, nullptr);
		if (slice != nullptr)
			slice->release();
		EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_tick_cache(*_reader, "SHFE.rb.rb2409#date#20240102"));
		const WTSTickStruct repaired = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 88.0);
		ASSERT_TRUE(WtRdmDtReaderADTestAccess::replace_record(
			WtRdmDtReaderADTestAccess::tick_db(*_reader, "SHFE.rb2409"), key, raw_of(repaired)));
		slice = _reader->readTickSliceByDate("SHFE.rb.rb2409", 20240102);
		ASSERT_NE(slice, nullptr);
		ASSERT_EQ(slice->size(), 1u);
		EXPECT_DOUBLE_EQ(slice->at(0)->price, repaired.price);
		slice->release();
	}
}

TEST_F(P05RdmReaderADTest, TickRangeRejectsShortAndLongLmdbValues)
{
	for (const int delta : {-1, 1})
	{
		SCOPED_TRACE(delta);
		_reader.reset();
		WTSTickStruct tick = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 77.0);
		std::string value = raw_of(tick);
		if (delta < 0)
			value.resize(value.size() - 1);
		else
			value.push_back('x');
		const std::string key = key_str(LMDBHftKey("SHFE", "rb2409", 20240102, 93000000));
		put_tick_records("SHFE", "rb2409", {{key, value}});
		init_reader(true);

		WTSTickSlice* slice = _reader->readTickSliceByRange("SHFE.rb.rb2409",
			20240102ull * 1000000000ull + 90000000ull,
			20240102ull * 1000000000ull + 150000000ull);
		EXPECT_EQ(slice, nullptr);
		if (slice != nullptr)
			slice->release();
		EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_tick_cache(*_reader, "SHFE.rb.rb2409"));
		const WTSTickStruct repaired = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 88.0);
		ASSERT_TRUE(WtRdmDtReaderADTestAccess::replace_record(
			WtRdmDtReaderADTestAccess::tick_db(*_reader, "SHFE.rb2409"), key, raw_of(repaired)));
		slice = _reader->readTickSliceByRange("SHFE.rb.rb2409",
			20240102ull * 1000000000ull + 90000000ull,
			20240102ull * 1000000000ull + 150000000ull);
		ASSERT_NE(slice, nullptr);
		ASSERT_EQ(slice->size(), 1u);
		EXPECT_DOUBLE_EQ(slice->at(0)->price, repaired.price);
		slice->release();
	}
}

TEST_F(P05RdmReaderADTest, KlineCountRejectsShortAndLongLmdbValues)
{
	for (const int delta : {-1, 1})
	{
		SCOPED_TRACE(delta);
		_reader.reset();
		WTSBarStruct bar = make_bar(20240102, 20240102, 77.0);
		std::string value = raw_of(bar);
		if (delta < 0)
			value.resize(value.size() - 1);
		else
			value.push_back('x');
		const std::string key = key_str(LMDBBarKey("SHFE", "rb2409", 20240102));
		put_day_records("SHFE", {{key, value}});
		init_reader(true);

		WTSKlineSlice* slice = _reader->readKlineSliceByCount("SHFE.rb.rb2409", KP_DAY, 1, 202401030000ull);
		EXPECT_EQ(slice, nullptr);
		if (slice != nullptr)
			slice->release();
		const std::string cache_key = "SHFE.rb.rb2409#" + std::to_string((uint32_t)KP_DAY);
		EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_bar_cache(*_reader, cache_key));
		const WTSBarStruct repaired = make_bar(20240102, 20240102, 88.0);
		ASSERT_TRUE(WtRdmDtReaderADTestAccess::replace_record(
			WtRdmDtReaderADTestAccess::bar_db(*_reader, "SHFE", KP_DAY), key, raw_of(repaired)));
		slice = _reader->readKlineSliceByCount("SHFE.rb.rb2409", KP_DAY, 1, 202401030000ull);
		ASSERT_NE(slice, nullptr);
		ASSERT_EQ(slice->get_block_size(0), 1u);
		EXPECT_DOUBLE_EQ(((const WTSBarStruct*)slice->get_block_addr(0))[0].open, repaired.open);
		slice->release();
	}
}

TEST_F(P05RdmReaderADTest, KlineRangeRejectsShortAndLongLmdbValues)
{
	for (const int delta : {-1, 1})
	{
		SCOPED_TRACE(delta);
		_reader.reset();
		WTSBarStruct bar = make_bar(20240102, 20240102, 77.0);
		std::string value = raw_of(bar);
		if (delta < 0)
			value.resize(value.size() - 1);
		else
			value.push_back('x');
		const std::string key = key_str(LMDBBarKey("SHFE", "rb2409", 20240102));
		put_day_records("SHFE", {{key, value}});
		init_reader(true);

		WTSKlineSlice* slice = _reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
			202401020000ull, 202401030000ull);
		EXPECT_EQ(slice, nullptr);
		if (slice != nullptr)
			slice->release();
		const std::string cache_key = "SHFE.rb.rb2409#" + std::to_string((uint32_t)KP_DAY);
		EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_bar_cache(*_reader, cache_key));
		const WTSBarStruct repaired = make_bar(20240102, 20240102, 88.0);
		ASSERT_TRUE(WtRdmDtReaderADTestAccess::replace_record(
			WtRdmDtReaderADTestAccess::bar_db(*_reader, "SHFE", KP_DAY), key, raw_of(repaired)));
		slice = _reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
			202401020000ull, 202401030000ull);
		ASSERT_NE(slice, nullptr);
		ASSERT_EQ(slice->get_block_size(0), 1u);
		EXPECT_DOUBLE_EQ(((const WTSBarStruct*)slice->get_block_addr(0))[0].open, repaired.open);
		slice->release();
	}
}

TEST_F(P05RdmReaderADTest, TickRangeDoesNotPublishOlderBatchWhenNewerRecordIsMalformed)
{
	const WTSTickStruct older = make_tick("SHFE", "rb2409", 20240102, 20240102, 92900000, 10.0);
	const WTSTickStruct middle = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 20.0);
	const WTSTickStruct newer = make_tick("SHFE", "rb2409", 20240102, 20240102, 93100000, 30.0);
	std::string malformed_newer = raw_of(newer);
	malformed_newer.push_back('x');
	const std::string older_key = key_str(LMDBHftKey("SHFE", "rb2409", 20240102, older.action_time));
	const std::string middle_key = key_str(LMDBHftKey("SHFE", "rb2409", 20240102, middle.action_time));
	const std::string newer_key = key_str(LMDBHftKey("SHFE", "rb2409", 20240102, newer.action_time));
	put_tick_records("SHFE", "rb2409", {
		{older_key, raw_of(older)}, {middle_key, raw_of(middle)}, {newer_key, malformed_newer}});
	init_reader(true);

	const uint64_t base = 20240102ull * 1000000000ull;
	WTSTickSlice* warm = _reader->readTickSliceByRange("SHFE.rb.rb2409", base + 93000000ull, base + 93000001ull);
	ASSERT_NE(warm, nullptr);
	ASSERT_EQ(warm->size(), 1u);
	EXPECT_DOUBLE_EQ(warm->at(0)->price, middle.price);
	warm->release();

	std::vector<WTSTickStruct> before_ticks;
	uint64_t before_first = 0;
	uint64_t before_last = 0;
	ASSERT_TRUE(WtRdmDtReaderADTestAccess::tick_cache_state(
		*_reader, "SHFE.rb.rb2409", before_ticks, before_first, before_last));
	ASSERT_EQ(before_ticks.size(), 1u);
	EXPECT_EQ(before_ticks[0].action_time, middle.action_time);

	WTSTickSlice* failed = _reader->readTickSliceByRange("SHFE.rb.rb2409", base + 92900000ull, base + 93100001ull);
	EXPECT_EQ(failed, nullptr);
	if (failed != nullptr)
		failed->release();
	std::vector<WTSTickStruct> after_ticks;
	uint64_t after_first = 0;
	uint64_t after_last = 0;
	ASSERT_TRUE(WtRdmDtReaderADTestAccess::tick_cache_state(
		*_reader, "SHFE.rb.rb2409", after_ticks, after_first, after_last));
	ASSERT_EQ(after_ticks.size(), before_ticks.size());
	EXPECT_EQ(after_first, before_first);
	EXPECT_EQ(after_last, before_last);
	EXPECT_EQ(after_ticks[0].action_time, middle.action_time);
	EXPECT_DOUBLE_EQ(after_ticks[0].price, middle.price);

	const WTSTickStruct repaired = make_tick("SHFE", "rb2409", 20240102, 20240102, 93100000, 33.0);
	ASSERT_TRUE(WtRdmDtReaderADTestAccess::replace_record(
		WtRdmDtReaderADTestAccess::tick_db(*_reader, "SHFE.rb2409"), newer_key, raw_of(repaired)));
	WTSTickSlice* recovered = _reader->readTickSliceByRange("SHFE.rb.rb2409", base + 92900000ull, base + 93100001ull);
	ASSERT_NE(recovered, nullptr);
	ASSERT_EQ(recovered->size(), 3u);
	EXPECT_DOUBLE_EQ(recovered->at(0)->price, older.price);
	EXPECT_DOUBLE_EQ(recovered->at(1)->price, middle.price);
	EXPECT_DOUBLE_EQ(recovered->at(2)->price, repaired.price);
	recovered->release();
}

TEST_F(P05RdmReaderADTest, TickRangeLeavesCacheUnchangedWhenNewerQueryFails)
{
	const WTSTickStruct older = make_tick("SHFE", "rb2409", 20240102, 20240102, 92900000, 10.0);
	const WTSTickStruct middle = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 20.0);
	const WTSTickStruct newer = make_tick("SHFE", "rb2409", 20240102, 20240102, 93100000, 30.0);
	put_ticks("SHFE", "rb2409", {older, middle, newer});
	init_reader(true);

	const uint64_t base = 20240102ull * 1000000000ull;
	WTSTickSlice* warm = _reader->readTickSliceByRange("SHFE.rb.rb2409", base + 93000000ull, base + 93000001ull);
	ASSERT_NE(warm, nullptr);
	ASSERT_EQ(warm->size(), 1u);
	warm->release();

	std::vector<WTSTickStruct> before_ticks;
	uint64_t before_first = 0;
	uint64_t before_last = 0;
	ASSERT_TRUE(WtRdmDtReaderADTestAccess::tick_cache_state(
		*_reader, "SHFE.rb.rb2409", before_ticks, before_first, before_last));
	RdmRangeFailureState failure;
	WtRdmDtReaderADTestAccess::set_cursor_get_override(&fail_second_checked_range, &failure);
	WTSTickSlice* failed = _reader->readTickSliceByRange("SHFE.rb.rb2409", base + 92900000ull, base + 93100001ull);
	WtRdmDtReaderADTestAccess::set_cursor_get_override(NULL, NULL);
	EXPECT_EQ(failed, nullptr);
	if (failed != nullptr)
		failed->release();
	EXPECT_EQ(failure.range_starts, 2);

	std::vector<WTSTickStruct> after_ticks;
	uint64_t after_first = 0;
	uint64_t after_last = 0;
	ASSERT_TRUE(WtRdmDtReaderADTestAccess::tick_cache_state(
		*_reader, "SHFE.rb.rb2409", after_ticks, after_first, after_last));
	ASSERT_EQ(after_ticks.size(), before_ticks.size());
	EXPECT_EQ(after_first, before_first);
	EXPECT_EQ(after_last, before_last);
	EXPECT_EQ(after_ticks[0].action_time, middle.action_time);

	WTSTickSlice* recovered = _reader->readTickSliceByRange("SHFE.rb.rb2409", base + 92900000ull, base + 93100001ull);
	ASSERT_NE(recovered, nullptr);
	ASSERT_EQ(recovered->size(), 3u);
	EXPECT_DOUBLE_EQ(recovered->at(0)->price, older.price);
	EXPECT_DOUBLE_EQ(recovered->at(1)->price, middle.price);
	EXPECT_DOUBLE_EQ(recovered->at(2)->price, newer.price);
	recovered->release();
}

TEST_F(P05RdmReaderADTest, NewerTickAppendPreservesActiveSliceWhenCapacityIsAvailable)
{
	const WTSTickStruct middle = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 20.0);
	const WTSTickStruct newer = make_tick("SHFE", "rb2409", 20240102, 20240102, 93100000, 30.0);
	put_ticks("SHFE", "rb2409", {middle, newer});
	init_reader(true);

	const uint64_t base = 20240102ull * 1000000000ull;
	WTSTickSlice* seed = _reader->readTickSliceByRange("SHFE.rb.rb2409", base + 93000000ull, base + 93000001ull);
	ASSERT_NE(seed, nullptr);
	ASSERT_EQ(seed->size(), 1u);
	seed->release();
	ASSERT_TRUE(WtRdmDtReaderADTestAccess::reserve_tick_cache(*_reader, "SHFE.rb.rb2409", 4));

	WTSTickSlice* active = _reader->readTickSliceByRange("SHFE.rb.rb2409", base + 93000000ull, base + 93000001ull);
	ASSERT_NE(active, nullptr);
	ASSERT_EQ(active->size(), 1u);
	EXPECT_DOUBLE_EQ(active->at(0)->price, middle.price);
	WTSTickSlice* expanded = _reader->readTickSliceByRange("SHFE.rb.rb2409", base + 93000000ull, base + 93100001ull);
	ASSERT_NE(expanded, nullptr);
	ASSERT_EQ(expanded->size(), 2u);
	EXPECT_DOUBLE_EQ(active->at(0)->price, middle.price);
	EXPECT_DOUBLE_EQ(expanded->at(0)->price, middle.price);
	EXPECT_DOUBLE_EQ(expanded->at(1)->price, newer.price);
	active->release();
	expanded->release();
}

TEST_F(P05RdmReaderADTest, KlineRangeKeepsExistingCacheWhenReloadFindsMalformedValue)
{
	const std::vector<WTSBarStruct> bars = make_day_bars(3, 10.0);
	put_day_bars("SHFE", "rb2409", bars);
	init_reader(true);
	const std::string cache_key = "SHFE.rb.rb2409#" + std::to_string((uint32_t)KP_DAY);

	WTSKlineSlice* warm = _reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
		202401020000ull, 202401050000ull);
	ASSERT_NE(warm, nullptr);
	ASSERT_EQ(warm->get_block_size(0), 3u);
	warm->release();
	std::vector<WTSBarStruct> before_bars;
	uint64_t before_last = 0;
	ASSERT_TRUE(WtRdmDtReaderADTestAccess::bar_cache_state(*_reader, cache_key, before_bars, before_last));

	const std::string second_key = key_str(LMDBBarKey("SHFE", "rb2409", bars[1].date));
	std::string malformed = raw_of(bars[1]);
	malformed.resize(malformed.size() - 1);
	ASSERT_TRUE(WtRdmDtReaderADTestAccess::replace_record(
		WtRdmDtReaderADTestAccess::bar_db(*_reader, "SHFE", KP_DAY), second_key, malformed));
	EXPECT_EQ(_reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
		202401020000ull, 202401050000ull), nullptr);

	std::vector<WTSBarStruct> after_bars;
	uint64_t after_last = 0;
	ASSERT_TRUE(WtRdmDtReaderADTestAccess::bar_cache_state(*_reader, cache_key, after_bars, after_last));
	ASSERT_EQ(after_bars.size(), before_bars.size());
	EXPECT_EQ(after_last, before_last);
	for (std::size_t i = 0; i < before_bars.size(); ++i)
	{
		EXPECT_EQ(after_bars[i].date, before_bars[i].date) << i;
		EXPECT_DOUBLE_EQ(after_bars[i].open, before_bars[i].open) << i;
	}

	const WTSBarStruct repaired = make_bar(bars[1].date, bars[1].time, 88.0);
	ASSERT_TRUE(WtRdmDtReaderADTestAccess::replace_record(
		WtRdmDtReaderADTestAccess::bar_db(*_reader, "SHFE", KP_DAY), second_key, raw_of(repaired)));
	WTSKlineSlice* recovered = _reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
		202401020000ull, 202401050000ull);
	ASSERT_NE(recovered, nullptr);
	ASSERT_EQ(recovered->get_block_size(0), 3u);
	const WTSBarStruct* recovered_bars = (const WTSBarStruct*)recovered->get_block_addr(0);
	ASSERT_NE(recovered_bars, nullptr);
	EXPECT_DOUBLE_EQ(recovered_bars[1].open, repaired.open);
	recovered->release();
}

TEST_F(P05RdmReaderADTest, KlineRangeKeepsExistingCacheWhenCheckedQueryFails)
{
	const std::vector<WTSBarStruct> bars = make_day_bars(3, 10.0);
	put_day_bars("SHFE", "rb2409", bars);
	init_reader(true);
	const std::string cache_key = "SHFE.rb.rb2409#" + std::to_string((uint32_t)KP_DAY);

	WTSKlineSlice* warm = _reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
		202401020000ull, 202401050000ull);
	ASSERT_NE(warm, nullptr);
	ASSERT_EQ(warm->get_block_size(0), 3u);
	warm->release();
	std::vector<WTSBarStruct> before_bars;
	uint64_t before_last = 0;
	ASSERT_TRUE(WtRdmDtReaderADTestAccess::bar_cache_state(*_reader, cache_key, before_bars, before_last));

	// 迭代03 s5（R21-BUG-07）：LMDB 环境无新提交时同窗口复读复用快照、不再扫描，
	// 因此先提交一次相同内容，使下一次读取必然走 checked range 失败注入路径；断言不变。
	put_day_bars("SHFE", "rb2409", bars);

	RdmRangeFailureState failure;
	failure.fail_on_range = 1;
	WtRdmDtReaderADTestAccess::set_cursor_get_override(&fail_second_checked_range, &failure);
	WTSKlineSlice* failed = _reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
		202401020000ull, 202401050000ull);
	WtRdmDtReaderADTestAccess::set_cursor_get_override(NULL, NULL);
	EXPECT_EQ(failed, nullptr);
	if (failed != nullptr)
		failed->release();
	EXPECT_EQ(failure.range_starts, 1);

	std::vector<WTSBarStruct> after_bars;
	uint64_t after_last = 0;
	ASSERT_TRUE(WtRdmDtReaderADTestAccess::bar_cache_state(*_reader, cache_key, after_bars, after_last));
	ASSERT_EQ(after_bars.size(), before_bars.size());
	EXPECT_EQ(after_last, before_last);
	for (std::size_t i = 0; i < before_bars.size(); ++i)
	{
		EXPECT_EQ(after_bars[i].date, before_bars[i].date) << i;
		EXPECT_DOUBLE_EQ(after_bars[i].open, before_bars[i].open) << i;
	}

	WTSKlineSlice* recovered = _reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
		202401020000ull, 202401050000ull);
	ASSERT_NE(recovered, nullptr);
	ASSERT_EQ(recovered->get_block_size(0), 3u);
	recovered->release();
}

TEST_F(P05RdmReaderADTest, TickReadEntrypointsAcceptCurrentPackedV2AndOldValues)
{
	const WTSTickStruct current = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 111.0);
	const WTSTickStruct packed_source = make_tick("SHFE", "rb2409", 20240102, 20240102, 93100000, 222.0);
	const WTSTickStruct old_source = make_tick("SHFE", "rb2409", 20240102, 20240102, 93200000, 333.0);
	const P05TickPackedV2 packed = packed_tick_v2(packed_source);
	const WTSTickStructOld old_tick = old_tick_v1(old_source);
	put_tick_records("SHFE", "rb2409", {
		{key_str(LMDBHftKey("SHFE", "rb2409", 20240102, current.action_time)), raw_of(current)},
		{key_str(LMDBHftKey("SHFE", "rb2409", 20240102, packed_source.action_time)), raw_bytes(packed)},
		{key_str(LMDBHftKey("SHFE", "rb2409", 20240102, old_source.action_time)), raw_bytes(old_tick)}});
	init_reader(true);

	auto expect_three_ticks = [](WTSTickSlice* slice) {
		ASSERT_NE(slice, nullptr);
		ASSERT_EQ(slice->size(), 3u);
		EXPECT_DOUBLE_EQ(slice->at(0)->price, 111.0);
		EXPECT_DOUBLE_EQ(slice->at(1)->price, 222.0);
		EXPECT_DOUBLE_EQ(slice->at(2)->price, 333.0);
		slice->release();
	};

	expect_three_ticks(_reader->readTickSliceByCount("SHFE.rb.rb2409", 3, 20240102150000000ull));
	expect_three_ticks(_reader->readTickSliceByDate("SHFE.rb.rb2409", 20240102));
	const uint64_t base = 20240102ull * 1000000000ull;
	expect_three_ticks(_reader->readTickSliceByRange("SHFE.rb.rb2409", base + 93000000ull, base + 93200001ull));
}

TEST_F(P05RdmReaderADTest, KlineReadEntrypointsAcceptCurrentPackedV2AndOldValues)
{
	const WTSBarStruct current = make_bar(20240102, 20240102, 111.0);
	const WTSBarStruct packed_source = make_bar(20240103, 20240103, 222.0);
	const WTSBarStruct old_source = make_bar(20240104, 20240104, 333.0);
	const P05BarPackedV2 packed = packed_bar_v2(packed_source);
	const WTSBarStructOld old_bar = old_bar_v1(old_source);
	put_day_records("SHFE", {
		{key_str(LMDBBarKey("SHFE", "rb2409", current.date)), raw_of(current)},
		{key_str(LMDBBarKey("SHFE", "rb2409", packed_source.date)), raw_bytes(packed)},
		{key_str(LMDBBarKey("SHFE", "rb2409", old_source.date)), raw_bytes(old_bar)}});
	init_reader(true);

	auto expect_three_bars = [](WTSKlineSlice* slice) {
		ASSERT_NE(slice, nullptr);
		ASSERT_EQ(slice->get_block_size(0), 3u);
		const WTSBarStruct* bars = (const WTSBarStruct*)slice->get_block_addr(0);
		ASSERT_NE(bars, nullptr);
		EXPECT_DOUBLE_EQ(bars[0].open, 111.0);
		EXPECT_DOUBLE_EQ(bars[1].open, 222.0);
		EXPECT_DOUBLE_EQ(bars[2].open, 333.0);
		slice->release();
	};

	expect_three_bars(_reader->readKlineSliceByCount("SHFE.rb.rb2409", KP_DAY, 3, 202401050000ull));
	expect_three_bars(_reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY, 202401020000ull, 202401050000ull));
}

TEST_F(P05RdmReaderADTest, CountReadersRejectShortAndLongLmdbKeys)
{
	for (const int delta : {-1, 1})
	{
		SCOPED_TRACE(delta);
		_reader.reset();
		const WTSTickStruct tick = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 77.0);
		std::string tick_key = key_str(LMDBHftKey("SHFE", "rb2409", 20240102, tick.action_time));
		const WTSBarStruct bar = make_bar(20240102, 20240102, 77.0);
		std::string bar_key = key_str(LMDBBarKey("SHFE", "rb2409", bar.date));
		if (delta < 0)
		{
			tick_key.resize(tick_key.size() - 1);
			bar_key.resize(bar_key.size() - 1);
		}
		else
		{
			tick_key.push_back('x');
			bar_key.push_back('x');
		}
		put_tick_records("SHFE", "rb2409", {{tick_key, raw_of(tick)}});
		put_day_records("SHFE", {{bar_key, raw_of(bar)}});
		init_reader(true);

		EXPECT_EQ(_reader->readTickSliceByCount("SHFE.rb.rb2409", 1, 20240102150000000ull), nullptr);
		EXPECT_EQ(_reader->readKlineSliceByCount("SHFE.rb.rb2409", KP_DAY, 1, 202401030000ull), nullptr);
		EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_tick_cache(*_reader, "SHFE.rb.rb2409#count-history"));
		EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_bar_cache(*_reader,
			"SHFE.rb.rb2409#" + std::to_string((uint32_t)KP_DAY)));
	}
}

TEST_F(P05RdmReaderADTest, TickDateRejectsShortAndLongLmdbKeys)
{
	for (const int delta : {-1, 1})
	{
		SCOPED_TRACE(delta);
		_reader.reset();
		const WTSTickStruct tick = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 77.0);
		std::string malformed_key = key_str(LMDBHftKey("SHFE", "rb2409", 20240102, tick.action_time));
		const std::string valid_key = malformed_key;
		if (delta < 0)
			malformed_key.resize(malformed_key.size() - 1);
		else
			malformed_key.push_back('x');
		put_tick_records("SHFE", "rb2409", {{malformed_key, raw_of(tick)}});
		init_reader(true);

		WTSTickSlice* failed = _reader->readTickSliceByDate("SHFE.rb.rb2409", 20240102);
		EXPECT_EQ(failed, nullptr);
		if (failed != nullptr)
			failed->release();
		EXPECT_TRUE(_sink->has_log("Malformed LMDB"));
		EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_tick_cache(*_reader, "SHFE.rb.rb2409#date#20240102"));

		ASSERT_TRUE(WtRdmDtReaderADTestAccess::delete_record(
			WtRdmDtReaderADTestAccess::tick_db(*_reader, "SHFE.rb2409"), malformed_key));
		ASSERT_TRUE(WtRdmDtReaderADTestAccess::replace_record(
			WtRdmDtReaderADTestAccess::tick_db(*_reader, "SHFE.rb2409"), valid_key, raw_of(tick)));
		WTSTickSlice* recovered = _reader->readTickSliceByDate("SHFE.rb.rb2409", 20240102);
		ASSERT_NE(recovered, nullptr);
		ASSERT_EQ(recovered->size(), 1u);
		EXPECT_DOUBLE_EQ(recovered->at(0)->price, tick.price);
		recovered->release();
	}
}

TEST_F(P05RdmReaderADTest, TickRangeRejectsShortAndLongLmdbKeys)
{
	for (const int delta : {-1, 1})
	{
		SCOPED_TRACE(delta);
		_reader.reset();
		const WTSTickStruct tick = make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000, 77.0);
		std::string malformed_key = key_str(LMDBHftKey("SHFE", "rb2409", 20240102, tick.action_time));
		const std::string valid_key = malformed_key;
		if (delta < 0)
			malformed_key.resize(malformed_key.size() - 1);
		else
			malformed_key.push_back('x');
		put_tick_records("SHFE", "rb2409", {{malformed_key, raw_of(tick)}});
		init_reader(true);

		const uint64_t base = 20240102ull * 1000000000ull;
		WTSTickSlice* failed = _reader->readTickSliceByRange("SHFE.rb.rb2409",
			base + 90000000ull, base + 150000000ull);
		EXPECT_EQ(failed, nullptr);
		if (failed != nullptr)
			failed->release();
		EXPECT_TRUE(_sink->has_log("Malformed LMDB"));
		EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_tick_cache(*_reader, "SHFE.rb.rb2409"));

		ASSERT_TRUE(WtRdmDtReaderADTestAccess::delete_record(
			WtRdmDtReaderADTestAccess::tick_db(*_reader, "SHFE.rb2409"), malformed_key));
		ASSERT_TRUE(WtRdmDtReaderADTestAccess::replace_record(
			WtRdmDtReaderADTestAccess::tick_db(*_reader, "SHFE.rb2409"), valid_key, raw_of(tick)));
		WTSTickSlice* recovered = _reader->readTickSliceByRange("SHFE.rb.rb2409",
			base + 90000000ull, base + 150000000ull);
		ASSERT_NE(recovered, nullptr);
		ASSERT_EQ(recovered->size(), 1u);
		EXPECT_DOUBLE_EQ(recovered->at(0)->price, tick.price);
		recovered->release();
	}
}

TEST_F(P05RdmReaderADTest, KlineRangeRejectsShortAndLongLmdbKeys)
{
	for (const int delta : {-1, 1})
	{
		SCOPED_TRACE(delta);
		_reader.reset();
		const WTSBarStruct bar = make_bar(20240102, 20240102, 77.0);
		std::string malformed_key = key_str(LMDBBarKey("SHFE", "rb2409", bar.date));
		const std::string valid_key = malformed_key;
		if (delta < 0)
			malformed_key.resize(malformed_key.size() - 1);
		else
			malformed_key.push_back('x');
		put_day_records("SHFE", {{malformed_key, raw_of(bar)}});
		init_reader(true);
		const std::string cache_key = "SHFE.rb.rb2409#" + std::to_string((uint32_t)KP_DAY);

		WTSKlineSlice* failed = _reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
			202401020000ull, 202401030000ull);
		EXPECT_EQ(failed, nullptr);
		if (failed != nullptr)
			failed->release();
		EXPECT_TRUE(_sink->has_log("Malformed LMDB"));
		EXPECT_FALSE(WtRdmDtReaderADTestAccess::has_bar_cache(*_reader, cache_key));

		ASSERT_TRUE(WtRdmDtReaderADTestAccess::delete_record(
			WtRdmDtReaderADTestAccess::bar_db(*_reader, "SHFE", KP_DAY), malformed_key));
		ASSERT_TRUE(WtRdmDtReaderADTestAccess::replace_record(
			WtRdmDtReaderADTestAccess::bar_db(*_reader, "SHFE", KP_DAY), valid_key, raw_of(bar)));
		WTSKlineSlice* recovered = _reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
			202401020000ull, 202401030000ull);
		ASSERT_NE(recovered, nullptr);
		ASSERT_EQ(recovered->get_block_size(0), 1u);
		const WTSBarStruct* recovered_bars = (const WTSBarStruct*)recovered->get_block_addr(0);
		ASSERT_NE(recovered_bars, nullptr);
		EXPECT_DOUBLE_EQ(recovered_bars[0].open, bar.open);
		recovered->release();
	}
}
