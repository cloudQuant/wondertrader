// r4 / WtDataStorageAD：WtDataReaderAD 的 LMDB 切片读取（tick 增量/全量、K 线命中、RT 缓存补线）
//
// 覆盖对象（src/WtDataStorageAD/WtDataReaderAD.cpp）：
//  - init（L60-78）：cfg==NULL 早退 / cfg!=NULL 记根目录
//  - readTickSlice（L80-206）：etime 拆分、reload_flag 1/2 两条取数路径、
//    循环缓冲区 array_one/array_two 两种切片拼装
//  - readKlineSlice（L368-494）：容量不足回源、LMDB 增量更新、RT 缓存补最后一根
//  - cacheBarsFromStorage（L230-267）：get_lowers + 左边界过滤
//  - update_cache_from_lmdb（L269-303）：get_uppers 与 _last_from_cache 回写
//  - get_rt_cache_bar（L305-366）：缓存块懒加载 / _last_size 增量建索引 / 未命中
//  - onMinuteEnd（L496-552）：时间闸门 + on_all_bar_updated
//  - get_k_db（L554-595）/ get_t_db（L597-619）：子目录映射与只读环境
// 只用 AD 侧类型；库文件全部由本文件在临时目录里自己造，读前先关闭写入环境。
#include "r4_ad_fixture.h"
#include "Includes/IDataReader.h"
#include "WtDataStorageAD/WtDataReaderAD.h"
#include "WTSTools/WTSHotMgr.h"

#include <fstream>
#include <functional>

namespace wtp {

struct WtLMDBCheckedLowersTestAccess
{
	static void set_cursor_get_override(WtLMDBQuery& query, LMDBRangeCursorGetOverride callback, void* context)
	{
		query._range_cursor_get_override = callback;
		query._range_cursor_get_context = context;
	}

	static void abort_transaction(WtLMDBQuery& query)
	{
		if (query._txn != NULL)
		{
			mdb_txn_abort(query._txn);
			query._txn = NULL;
		}
		query._db.update_errno(MDB_BAD_TXN);
	}
};

struct WtDataReaderADTestAccess
{
	static void set_cursor_get_override(LMDBRangeCursorGetOverride callback, void* context)
	{
		WtDataReaderAD::set_test_cursor_get_override(callback, context);
	}
	// P05-009 is an intentionally isolated test of an otherwise unused private
	// helper; other reader tests in this file exercise their public interfaces.
	static bool read_bars_to_buffer(WtDataReaderAD& reader, const char* exchg,
		const char* code, WTSKlinePeriod period, std::string& output)
	{
		return reader.read_bars_to_buffer(exchg, code, period, output);
	}
	static std::string read_bars_legacy(WtDataReaderAD& reader, const char* exchg,
		const char* code, WTSKlinePeriod period)
	{
		return reader.read_bars_to_buffer(exchg, code, period);
	}
	static bool read_rt_snapshot(WtDataReaderAD& reader, const char* exchg,
		const char* code, WTSKlinePeriod period, WTSBarStruct& result)
	{
		return reader.get_rt_cache_bar(exchg, code, period, result) == WtDataReaderAD::RTBarRead::Found;
	}
};

} // namespace wtp

namespace {

struct R4CursorFailureState
{
	int calls = 0;
	int fail_on_call = 3;
};

struct R4CursorCallCounter
{
	int calls = 0;
};

int count_cursor_get_calls(void* context, MDB_cursor* cursor, MDB_val* key, MDB_val* value, MDB_cursor_op op)
{
	R4CursorCallCounter* counter = static_cast<R4CursorCallCounter*>(context);
	++counter->calls;
	return mdb_cursor_get(cursor, key, value, op);
}

int fail_on_selected_cursor_call(void* context, MDB_cursor* cursor, MDB_val* key, MDB_val* value, MDB_cursor_op op)
{
	R4CursorFailureState* state = static_cast<R4CursorFailureState*>(context);
	if (++state->calls == state->fail_on_call)
		return MDB_CORRUPTED;
	return mdb_cursor_get(cursor, key, value, op);
}

class ScopedR4CursorOverride
{
public:
	explicit ScopedR4CursorOverride(R4CursorFailureState* state)
	{
		WtDataReaderADTestAccess::set_cursor_get_override(&fail_on_selected_cursor_call, state);
	}

	~ScopedR4CursorOverride()
	{
		WtDataReaderADTestAccess::set_cursor_get_override(NULL, NULL);
	}
};

//////////////////////////////////////////////////////////////////////////
// IDataReaderSink 替身
//////////////////////////////////////////////////////////////////////////
class R4ReaderSink : public IDataReaderSink
{
public:
	explicit R4ReaderSink(IBaseDataMgr* bd) : _bd(bd) {}
	virtual ~R4ReaderSink() {}

	void on_bar(const char* stdCode, WTSKlinePeriod period, WTSBarStruct* newBar) override
	{
		_bar_calls++;
		if (newBar != NULL)
		{
			_last_bar = *newBar;
			_last_code = stdCode == NULL ? "" : stdCode;
			_last_period = period;
			_bar_events.push_back({ _last_code, period, newBar->date, newBar->time, newBar->open });
			if (_bar_hook)
				_bar_hook(newBar);
		}
	}

	void on_all_bar_updated(uint32_t updateTime) override
	{
		_all_updated++;
		_last_update_time = updateTime;
	}

	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return NULL; }
	uint32_t get_date() override { return _date; }
	uint32_t get_min_time() override { return _min_time; }
	uint32_t get_secs() override { return _secs; }

	void reader_log(WTSLogLevel ll, const char* message) override
	{
		_logs[ll]++;
		_total++;
		if (message != NULL)
		{
			_last[ll] = message;
			_all.push_back(std::string(ll == LL_DEBUG ? "D|" : ll == LL_INFO ? "I|" :
				ll == LL_WARN ? "W|" : ll == LL_ERROR ? "E|" : "T|") + message);
		}
	}

	void set_clock(uint32_t d, uint32_t m, uint32_t s) { _date = d; _min_time = m; _secs = s; }
	void set_bar_hook(std::function<void(WTSBarStruct*)> hook) { _bar_hook = std::move(hook); }
	uint32_t bar_calls() const { return _bar_calls; }
	uint32_t all_updated() const { return _all_updated; }
	uint32_t last_update_time() const { return _last_update_time; }
	const WTSBarStruct& last_bar() const { return _last_bar; }
	const std::string& last_bar_code() const { return _last_code; }
	WTSKlinePeriod last_period() const { return _last_period; }
	struct BarEvent
	{
		std::string code;
		WTSKlinePeriod period;
		uint32_t date;
		uint64_t time;
		double open;
	};
	const std::vector<BarEvent>& bar_events() const { return _bar_events; }
	uint32_t log_count(WTSLogLevel ll) const
	{
		auto it = _logs.find(ll);
		return it == _logs.end() ? 0u : it->second;
	}
	std::string last_log(WTSLogLevel ll) const
	{
		auto it = _last.find(ll);
		return it == _last.end() ? std::string() : it->second;
	}
	// 全量日志里包含子串的条数（last_log 会被后续日志覆盖，"开环/回源"这类
	// 一次性语句只能靠它断言）
	uint32_t count_containing(const char* sub) const
	{
		uint32_t ret = 0;
		const std::string s(sub == NULL ? "" : sub);
		for (const std::string& item : _all)
		{
			if (item.find(s) != std::string::npos)
				ret++;
		}
		return ret;
	}

private:
	IBaseDataMgr*	_bd;
	uint32_t		_date = 20230103;
	uint32_t		_min_time = 1000;
	uint32_t		_secs = 0;
	uint32_t		_bar_calls = 0;
	uint32_t		_all_updated = 0;
	uint32_t		_last_update_time = 0;
	WTSBarStruct	_last_bar;
	std::string		_last_code;
	WTSKlinePeriod	_last_period = KP_DAY;
	uint32_t		_total = 0;
	std::vector<BarEvent> _bar_events;
	std::function<void(WTSBarStruct*)> _bar_hook;
	std::vector<std::string> _all;
	wt_hashmap<WTSLogLevel, uint32_t> _logs;
	wt_hashmap<WTSLogLevel, std::string> _last;
};

class R4ReaderHotSink : public R4ReaderSink
{
public:
	R4ReaderHotSink(IBaseDataMgr* bd, IHotMgr* hot) : R4ReaderSink(bd), _hot(hot) {}
	IHotMgr* get_hot_mgr() override { return _hot; }

private:
	IHotMgr* _hot;
};

static const uint32_t R4_RD1 = 20230103;
static const uint32_t R4_M1_931 = 3301030931u;
static const uint32_t R4_M1_932 = 3301030932u;
static const uint32_t R4_M1_933 = 3301030933u;
static const uint32_t R4_M1_940 = 3301030940u;
// readKlineSlice 内部把 etime 折成 (date-19900000)*10000 + HHMM = 3301031000

[[maybe_unused]] static std::string hft_key_str(const char* exchg, const char* code,
	uint32_t date, uint32_t time)
{
	LMDBHftKey key(exchg, code, date, time);
	return std::string((const char*)&key, sizeof(key));
}

[[maybe_unused]] static std::string bar_key_str(const char* exchg, const char* code, uint32_t bt)
{
	LMDBBarKey key(exchg, code, bt);
	return std::string((const char*)&key, sizeof(key));
}

enum class R4RawBarKeyLength { Short, Long };

static std::string raw_bar_key_str(const char* exchg, const char* code, uint32_t bt,
	R4RawBarKeyLength length)
{
	LMDBBarKey key(exchg, code, bt);
	std::string raw((const char*)&key, sizeof(key));
	if (length == R4RawBarKeyLength::Short)
		raw.resize(sizeof(key) - 1);
	else
		raw.push_back('\x7f');
	return raw;
}

class R4ReaderADTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		_root = std::filesystem::temp_directory_path()
			/ ("r4_rad_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
		std::filesystem::create_directories(_root);
		_bd = new R4BaseDataMgr();
		_sink = new R4ReaderSink(_bd);
	}

	void TearDown() override
	{
		_reader.reset();
		delete _sink;
		_sink = NULL;
		if (_owned_hot_mgr != NULL)
			_owned_hot_mgr->release();
		_owned_hot_mgr.reset();
		delete _bd;
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	void init_reader(bool with_cfg = true)
	{
		_reader.reset(new WtDataReaderAD());
		if (with_cfg)
		{
			WTSVariant* cfg = WTSVariant::createObject();
			cfg->append("path", _root.string().c_str());
			_reader->init(cfg, _sink, NULL);
			cfg->release();
		}
		else
		{
			_reader->init(NULL, _sink, NULL);
		}
	}

	// 注册一个现货品种（isFuture()==false → 不碰 _hot_mgr）
	WTSContractInfo* add_stock(const char* exchg, const char* code)
	{
		WTSCommodityInfo* comm = _bd->add_commodity(exchg, code, ContractCategory::CC_Stock);
		return _bd->add_contract(exchg, code, comm);
	}

	std::string tick_dir(const char* exchg, const char* code) const
	{
		return (_root / "ticks" / exchg / code).string();
	}
	std::string kline_dir(const char* subdir, const char* exchg) const
	{
		return (_root / subdir / exchg).string();
	}

	// 造 tick 库（环境在块结束时关闭，之后才允许读取器再开同一路径）
	void seed_ticks(const char* exchg, const char* code,
		const std::vector<std::pair<uint32_t, WTSTickStruct>>& rows)
	{
		std::filesystem::create_directories(_root / "ticks" / exchg / code);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(tick_dir(exchg, code).c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for (const auto& row : rows)
		{
			const WTSTickStruct& t = row.second;
			query.put(hft_key_str(exchg, code, row.first, t.action_time),
				std::string((const char*)&t, sizeof(t)));
		}
		query.commit();
	}

	// 造 K 线库
	void seed_bars(const char* subdir, const char* exchg, const char* code,
		const std::vector<std::pair<uint32_t, WTSBarStruct>>& rows)
	{
		std::filesystem::create_directories(_root / subdir / exchg);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(kline_dir(subdir, exchg).c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for (const auto& row : rows)
		{
			query.put(bar_key_str(exchg, code, row.first),
				std::string((const char*)&row.second, sizeof(WTSBarStruct)));
		}
		query.commit();
	}

	// 允许注入/修复长度错误的 value；reader 仍通过 public readKlineSlice 读取。
	void seed_bar_values(const char* subdir, const char* exchg, const char* code,
		const std::vector<std::pair<uint32_t, std::string>>& rows)
	{
		std::filesystem::create_directories(_root / subdir / exchg);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(kline_dir(subdir, exchg).c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for (const auto& row : rows)
			ASSERT_TRUE(query.put(bar_key_str(exchg, code, row.first), row.second));
		query.commit();
	}

	void seed_raw_bar_values(const char* subdir, const char* exchg,
		const std::vector<std::pair<std::string, std::string>>& rows)
	{
		std::filesystem::create_directories(_root / subdir / exchg);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(kline_dir(subdir, exchg).c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for (const auto& row : rows)
			ASSERT_TRUE(query.put(row.first, row.second));
		query.commit();
	}

	static WTSTickStruct tick_at(const char* exchg, const char* code, uint32_t at, double px)
	{
		return make_tick(exchg, code, R4_RD1, R4_RD1, at, px, 1, 10, 5, 0, 100, 1000);
	}

	static WTSBarStruct bar_at(const char* /*code*/, uint32_t bt, double px)
	{
		WTSBarStruct bar;
		memset(&bar, 0, sizeof(bar));
		bar.date = R4_RD1;
		bar.time = bt;
		bar.open = px;
		bar.high = px + 1;
		bar.low = px - 1;
		bar.close = px + 0.5;
		bar.vol = 10;
		bar.money = px * 10;
		// 归属信息在 LMDBBarKey 里，WTSBarStruct 本身不带代码字段
		return bar;
	}

	std::filesystem::path	_root;
	R4BaseDataMgr*			_bd = NULL;
	R4ReaderSink*			_sink = NULL;
	std::unique_ptr<WtDataReaderAD> _reader;
	std::unique_ptr<WTSHotMgr> _owned_hot_mgr;
};

class R4NightSessionBaseDataMgr : public R4BaseDataMgr
{
public:
	R4NightSessionBaseDataMgr() : R4BaseDataMgr("R4_NIGHT", 2100, 2359, 300) {}

	uint32_t calcTradingDate(const char*, uint32_t date, uint32_t time, bool) override
	{
		// A futures night session on Jan 3 belongs to the Jan 4 trading date.
		return date == 20230103 && time >= 2100 ? 20230104 : date;
	}
};

class R4ReaderADRawKeyTest : public R4ReaderADTest,
	public ::testing::WithParamInterface<R4RawBarKeyLength>
{};

//////////////////////////////////////////////////////////////////////////
// 1. init：cfg==NULL 只接基础数据，不打印根目录；cfg!=NULL 打印并建目录映射
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, InitLogsRootOnlyWhenCfgProvided)
{
	init_reader(false);
	EXPECT_EQ(_sink->log_count(LL_INFO), 0u);

	// 重新带 cfg 初始化，走 L70-77
	_reader.reset(new WtDataReaderAD());
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", _root.string().c_str());
	_reader->init(cfg, _sink, NULL);
	cfg->release();

	EXPECT_EQ(_sink->log_count(LL_INFO), 1u);
	EXPECT_NE(_sink->last_log(LL_INFO).find("WtDataReaderAD initialized"), std::string::npos);
	// standardisePath 补了结尾斜杠
	EXPECT_NE(_sink->last_log(LL_INFO).find(_root.string() + "/"), std::string::npos)
		<< _sink->last_log(LL_INFO);
}

//////////////////////////////////////////////////////////////////////////
// 2. 首读使用 count 限量回源，而不是扫描完整历史区间后再裁剪。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, FirstTickReadUsesCountLimitedReload)
{
	add_stock("EXR1", "rb2301");
	seed_ticks("EXR1", "rb2301", {
		{R4_RD1, tick_at("EXR1", "rb2301", 93000000, 3900.0)},
		{R4_RD1, tick_at("EXR1", "rb2301", 93100000, 3910.0)},
		{R4_RD1, tick_at("EXR1", "rb2301", 93200000, 3920.0)}
		});
	init_reader();

	// etime = 20230103 10:00:00.000；历史 3 笔，只应限量读取最近 2 笔。
	WTSTickSlice* slice = _reader->readTickSlice("EXR1.rb2301", 2, 20230103100000000ull);
	ASSERT_TRUE(slice != NULL);
	ASSERT_EQ(slice->get_block_counts(), (std::size_t)1);
	ASSERT_EQ(slice->get_block_size(0), 2u);
	EXPECT_DOUBLE_EQ(slice->get_block_addr(0)[0].price, 3910.0);
	EXPECT_EQ(slice->get_block_addr(0)[0].action_time, 93100000u);
	EXPECT_DOUBLE_EQ(slice->get_block_addr(0)[1].price, 3920.0);
	EXPECT_EQ(slice->get_block_addr(0)[1].action_time, 93200000u);
	slice->release();

	EXPECT_EQ(_sink->count_containing("2 ticks of EXR1.rb2301 loaded to cache for the first time"), 1u);
	EXPECT_EQ(_sink->count_containing("append to cache"), 0u);
}

//////////////////////////////////////////////////////////////////////////
// 3. checked lower query 区分完整空结果与游标失败，不向失败回调部分记录。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, CheckedLowerQuerySeparatesEmptyAndCursorFailure)
{
	const std::filesystem::path dir = _root / "checked_lowers";
	std::filesystem::create_directories(dir);
	{
		WtLMDB writer_db(false);
		ASSERT_TRUE(writer_db.open(dir.string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery writer(writer_db);
		ASSERT_TRUE(writer.put("a", "first"));
		ASSERT_TRUE(writer.put("b", "second"));
		ASSERT_TRUE(writer.put("c", "third"));
		writer.commit();
	}
	WtLMDB db(true);
	ASSERT_TRUE(db.open(dir.string().c_str(), 8 * 1024 * 1024));

	WtLMDBQuery query(db);
	int callback_calls = 0;
	EXPECT_EQ(0, query.get_lowers_checked("z", "z", 5,
		[&callback_calls](const ValueArray& keys, const ValueArray& values) {
			++callback_calls;
			EXPECT_TRUE(keys.empty());
			EXPECT_TRUE(values.empty());
		}));
	EXPECT_EQ(1, callback_calls);
	EXPECT_FALSE(db.has_error());

	R4CursorFailureState failure;
	WtLMDBCheckedLowersTestAccess::set_cursor_get_override(query, &fail_on_selected_cursor_call, &failure);
	EXPECT_EQ(-1, query.get_lowers_checked("a", "z", 5,
		[&callback_calls](const ValueArray&, const ValueArray&) { ++callback_calls; }));
	EXPECT_EQ(3, failure.calls);
	EXPECT_EQ(1, callback_calls);
	EXPECT_TRUE(db.has_error());
	EXPECT_STREQ(mdb_strerror(MDB_CORRUPTED), db.errmsg());

	// 清除注入后，相同边界重试可完整返回；此前失败没有发布部分结果。
	WtLMDBCheckedLowersTestAccess::set_cursor_get_override(query, NULL, NULL);
	ValueArray keys;
	ValueArray values;
	EXPECT_EQ(3, query.get_lowers_checked("a", "z", 5,
		[&callback_calls, &keys, &values](const ValueArray& found_keys, const ValueArray& found_values) {
			++callback_calls;
			keys = found_keys;
			values = found_values;
		}));
	EXPECT_EQ(2, callback_calls);
	EXPECT_FALSE(db.has_error());
	ASSERT_EQ(3u, keys.size());
	ASSERT_EQ(3u, values.size());
	EXPECT_EQ("a", keys[0]);
	EXPECT_EQ("c", keys[2]);

	R4CursorCallCounter counter;
	WtLMDBCheckedLowersTestAccess::set_cursor_get_override(query, &count_cursor_get_calls, &counter);
	keys.clear();
	values.clear();
	EXPECT_EQ(2, query.get_lowers_checked("a", "z", 2,
		[&keys, &values](const ValueArray& found_keys, const ValueArray& found_values) {
			keys = found_keys;
			values = found_values;
		}));
	EXPECT_EQ(3, counter.calls);
	ASSERT_EQ(2u, keys.size());
	EXPECT_EQ("b", keys[0]);
	EXPECT_EQ("c", keys[1]);
	WtLMDBCheckedLowersTestAccess::set_cursor_get_override(query, NULL, NULL);

	WtLMDBQuery invalid_query(db);
	WtLMDBCheckedLowersTestAccess::abort_transaction(invalid_query);
	EXPECT_EQ(-1, invalid_query.get_lowers_checked("a", "z", 5,
		[&callback_calls](const ValueArray&, const ValueArray&) { ++callback_calls; }));
	EXPECT_EQ(2, callback_calls);
}

//////////////////////////////////////////////////////////////////////////
// 4. 首读打开数据库失败后不提交缓存/cursor；修复库文件后同 etime 重试成功。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, FailedInitialTickDbOpenCanRetrySameEtime)
{
	add_stock("EXR_FAIL", "rb2301");
	seed_ticks("EXR_FAIL", "rb2301", {
		{R4_RD1, tick_at("EXR_FAIL", "rb2301", 93000000, 3900.0)},
		{R4_RD1, tick_at("EXR_FAIL", "rb2301", 93100000, 3910.0)},
		{R4_RD1, tick_at("EXR_FAIL", "rb2301", 93200000, 3920.0)}
		});
	init_reader();

	const std::filesystem::path db_path = std::filesystem::path(tick_dir("EXR_FAIL", "rb2301")) / "data.mdb";
	const std::filesystem::path backup_path = _root / "valid_ticks.mdb";
	ASSERT_TRUE(std::filesystem::exists(db_path));
	std::filesystem::copy_file(db_path, backup_path);
	{
		std::ofstream corrupt(db_path.string().c_str(), std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(corrupt.good());
		corrupt << "not an LMDB data file";
	}
	const uint64_t etime = 20230103100000000ull;
	EXPECT_EQ(NULL, _reader->readTickSlice("EXR_FAIL.rb2301", 2, etime));
	EXPECT_EQ(1u, _sink->log_count(LL_ERROR));
	EXPECT_EQ(0u, _sink->count_containing("loaded to cache for the first time"));

	std::filesystem::copy_file(backup_path, db_path, std::filesystem::copy_options::overwrite_existing);
	WTSTickSlice* retry = _reader->readTickSlice("EXR_FAIL.rb2301", 2, etime);
	ASSERT_TRUE(retry != NULL);
	ASSERT_EQ(retry->get_block_counts(), (std::size_t)1);
	ASSERT_EQ(retry->get_block_size(0), 2u);
	EXPECT_DOUBLE_EQ(retry->get_block_addr(0)[0].price, 3910.0);
	EXPECT_DOUBLE_EQ(retry->get_block_addr(0)[1].price, 3920.0);
	retry->release();
	EXPECT_EQ(1u, _sink->count_containing("2 ticks of EXR_FAIL.rb2301 loaded to cache for the first time"));
}

//////////////////////////////////////////////////////////////////////////
// 首次 get_lowers checked 扫描中途失败时不发布部分 cache，并可用同 etime 重试。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, FailedInitialTickLowerQueryCanRetrySameEtime)
{
	add_stock("EXR_LOWER_FAIL", "rb2301");
	seed_ticks("EXR_LOWER_FAIL", "rb2301", {
		{R4_RD1, tick_at("EXR_LOWER_FAIL", "rb2301", 93000000, 3900.0)},
		{R4_RD1, tick_at("EXR_LOWER_FAIL", "rb2301", 93100000, 3910.0)},
		{R4_RD1, tick_at("EXR_LOWER_FAIL", "rb2301", 93200000, 3920.0)}
		});
	init_reader();

	R4CursorFailureState failure;
	failure.fail_on_call = 3; // SET_RANGE miss, LAST finds one row, PREV then fails
	const uint64_t etime = 20230103100000000ull;
	{
		ScopedR4CursorOverride injected(&failure);
		EXPECT_EQ(NULL, _reader->readTickSlice("EXR_LOWER_FAIL.rb2301", 2, etime));
	}
	EXPECT_EQ(3, failure.calls);
	EXPECT_EQ(1u, _sink->log_count(LL_ERROR));
	EXPECT_EQ(0u, _sink->count_containing("loaded to cache for the first time"));

	WTSTickSlice* retry = _reader->readTickSlice("EXR_LOWER_FAIL.rb2301", 2, etime);
	ASSERT_TRUE(retry != NULL);
	ASSERT_EQ(retry->get_block_counts(), (std::size_t)1);
	ASSERT_EQ(retry->get_block_size(0), 2u);
	EXPECT_DOUBLE_EQ(retry->get_block_addr(0)[0].price, 3910.0);
	EXPECT_DOUBLE_EQ(retry->get_block_addr(0)[1].price, 3920.0);
	retry->release();
	EXPECT_EQ(1u, _sink->count_containing("2 ticks of EXR_LOWER_FAIL.rb2301 loaded to cache for the first time"));
}

//////////////////////////////////////////////////////////////////////////
// 增量 get_range checked 扫描中途失败时 cursor/cache 不推进，同 etime 重试补入新 tick。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, FailedIncrementalTickRangeQueryCanRetrySameEtime)
{
	add_stock("EXR_RANGE_FAIL", "rb2301");
	seed_ticks("EXR_RANGE_FAIL", "rb2301", {
		{R4_RD1, tick_at("EXR_RANGE_FAIL", "rb2301", 93000000, 3900.0)},
		{R4_RD1, tick_at("EXR_RANGE_FAIL", "rb2301", 93100000, 3910.0)},
		{R4_RD1, tick_at("EXR_RANGE_FAIL", "rb2301", 93200000, 3920.0)}
		});
	init_reader();

	const uint64_t initial_etime = 20230103093130000ull;
	WTSTickSlice* initial = _reader->readTickSlice("EXR_RANGE_FAIL.rb2301", 2, initial_etime);
	ASSERT_TRUE(initial != NULL);
	ASSERT_EQ(initial->get_block_size(0), 2u);
	EXPECT_DOUBLE_EQ(initial->get_block_addr(0)[0].price, 3900.0);
	EXPECT_DOUBLE_EQ(initial->get_block_addr(0)[1].price, 3910.0);
	initial->release();

	R4CursorFailureState failure;
	failure.fail_on_call = 2; // SET_RANGE finds the new row, NEXT fails after collecting it
	const uint64_t next_etime = 20230103093230000ull;
	{
		ScopedR4CursorOverride injected(&failure);
		EXPECT_EQ(NULL, _reader->readTickSlice("EXR_RANGE_FAIL.rb2301", 2, next_etime));
	}
	EXPECT_EQ(2, failure.calls);
	EXPECT_EQ(1u, _sink->log_count(LL_ERROR));
	EXPECT_EQ(0u, _sink->count_containing("ticks after 20230103093130001"));

	WTSTickSlice* retry = _reader->readTickSlice("EXR_RANGE_FAIL.rb2301", 2, next_etime);
	ASSERT_TRUE(retry != NULL);
	ASSERT_EQ(retry->get_block_counts(), (std::size_t)2);
	ASSERT_EQ(retry->get_block_size(0), 1u);
	ASSERT_EQ(retry->get_block_size(1), 1u);
	EXPECT_DOUBLE_EQ(retry->get_block_addr(0)[0].price, 3910.0);
	EXPECT_DOUBLE_EQ(retry->get_block_addr(1)[0].price, 3920.0);
	retry->release();
	EXPECT_EQ(1u, _sink->count_containing("1 ticks after 20230103093130001 of EXR_RANGE_FAIL.rb2301 append to cache"));
}

//////////////////////////////////////////////////////////////////////////
// 7. 同一 etime 复读和时间推进均返回预期 tick 窗口
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, RepeatTickReadPreservesDataAndAdvancesWindow)
{
	add_stock("EXR2", "rb2301");
	seed_ticks("EXR2", "rb2301", {
		{R4_RD1, tick_at("EXR2", "rb2301", 93000000, 3900.0)},
		{R4_RD1, tick_at("EXR2", "rb2301", 93100000, 3910.0)},
		{R4_RD1, tick_at("EXR2", "rb2301", 93200000, 3920.0)}
		});
	init_reader();

	// 第 1 次：窗口 [date0/time1, 20230103/093130000] 命中 930、931 两笔 → 容量 2 刚好装满
	WTSTickSlice* s1 = _reader->readTickSlice("EXR2.rb2301", 2, 20230103093130000ull);
	ASSERT_TRUE(s1 != NULL);
	ASSERT_EQ(s1->get_block_counts(), (std::size_t)1);
	ASSERT_EQ(s1->get_block_size(0), 2u);
	EXPECT_DOUBLE_EQ(s1->get_block_addr(0)[0].price, 3900.0);
	EXPECT_DOUBLE_EQ(s1->get_block_addr(0)[1].price, 3910.0);
	s1->release();

	// IDataReader 只约定返回指定窗口的数据；是否跳过查询或重开 LMDB 是 AD
	// reader 的实现细节。这里检查公开结果，重开行为仅作为诊断时可观察的日志。
	WTSTickSlice* s2 = _reader->readTickSlice("EXR2.rb2301", 2, 20230103093130000ull);
	ASSERT_TRUE(s2 != NULL);
	ASSERT_EQ(s2->get_block_counts(), (std::size_t)1);
	ASSERT_EQ(s2->get_block_size(0), 2u);
	EXPECT_DOUBLE_EQ(s2->get_block_addr(0)[0].price, 3900.0);
	EXPECT_DOUBLE_EQ(s2->get_block_addr(0)[1].price, 3910.0);
	s2->release();

	// 第 3 次：etime 前进 1 分钟，结果应包含窗口尾部的 931、932 两笔。
	WTSTickSlice* s3 = _reader->readTickSlice("EXR2.rb2301", 2, 20230103093230000ull);
	ASSERT_TRUE(s3 != NULL);
	// 缓冲区已回绕：array_two 有 1 条，必须靠 appendBlock 拼出第二个块
	EXPECT_EQ(s3->get_block_counts(), (std::size_t)2);
	ASSERT_EQ(s3->get_block_size(0), 1u);
	EXPECT_DOUBLE_EQ(s3->get_block_addr(0)[0].price, 3910.0);
	ASSERT_EQ(s3->get_block_size(1), 1u);
	EXPECT_DOUBLE_EQ(s3->get_block_addr(1)[0].price, 3920.0);
	EXPECT_EQ(s3->get_block_addr(0)[0].action_time, 93100000u);
	EXPECT_EQ(s3->get_block_addr(1)[0].action_time, 93200000u);
	s3->release();
}

//////////////////////////////////////////////////////////////////////////
// 8. 空库读取：0 命中，切片区容量为 count 且块大小为 0
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, TickReadAgainstEmptyDbYieldsEmptySlice)
{
	add_stock("EXR3", "rb2301");
	std::filesystem::create_directories(_root / "ticks" / "EXR3" / "rb2301");
	init_reader();

	WTSTickSlice* slice = _reader->readTickSlice("EXR3.rb2301", 3, 20230103100000000ull);
	ASSERT_TRUE(slice != NULL);
	// 0 条命中 → count 被修正为 0 → array_two 分支给出空块
	EXPECT_EQ(slice->get_block_counts(), (std::size_t)1);
	EXPECT_EQ(slice->get_block_size(0), 0u);
	slice->release();

	// 空目录也会自建（fs::create_directories 自愈）
	EXPECT_TRUE(std::filesystem::exists(_root / "ticks" / "EXR3" / "rb2301" / "data.mdb"));
}

//////////////////////////////////////////////////////////////////////////
// 5. K 线首读：get_lowers 回源 + 增量检查 + 无 RT 缓存
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, KlineFirstReadLoadsLowerBarsAndChecksLatest)
{
	add_stock("EXK1", "rb2301");
	seed_bars("min1", "EXK1", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)},
		{R4_M1_932, bar_at("rb2301", R4_M1_932, 3910.0)},
		{R4_M1_933, bar_at("rb2301", R4_M1_933, 3920.0)}
		});
	init_reader();

	WTSKlineSlice* slice = _reader->readKlineSlice("EXK1.rb2301", KP_Minute1, 3, 20230103100000000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->get_block_counts(), (std::size_t)1);
	ASSERT_EQ(slice->get_block_size(0), 3u);
	const WTSBarStruct* b = slice->get_block_addr(0);
	EXPECT_EQ(b[0].time, (uint64_t)R4_M1_931);
	EXPECT_DOUBLE_EQ(b[0].open, 3900.0);
	EXPECT_EQ(b[2].time, (uint64_t)R4_M1_933);
	EXPECT_DOUBLE_EQ(b[2].close, 3920.5);
	slice->release();

	// 回源日志（cacheBarsFromStorage L265 的格式是 "{} {} bars of {} loaded to cache"：
	// 周期名取 PERIOD_NAME[KP_Minute1]="min1"（WTSTypes.h:115-123），代码位传的是入参
	// stdCode 原文 "EXK1.rb2301"，不是 rdm 侧的 "exchg.product.code" 三段拼接。
	// 且本用例后面还有 update_cache_from_lmdb L301 的 "…updated to…" DEBUG，
	// last_log 会被覆盖 ⇒ 用 count_containing 断言（R32 修正，依据行号见前））。
	EXPECT_EQ(_sink->count_containing("min1 bars of EXK1.rb2301 loaded to cache"), 1u);
	// 本次读取消费了两个 min1 句柄：第 1 个来自缓存（返回后被 std::move 掏空）
	EXPECT_TRUE(std::filesystem::exists(_root / "min1" / "EXK1" / "data.mdb"));
}

//////////////////////////////////////////////////////////////////////////
// 首次回源遇到短 value 时拒绝整个装载；关闭 reader 修复存储后，合法重读成功。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, KlineInitialLoadRejectsShortValueAndRetriesAfterRepair)
{
	add_stock("EXK_BAD_LOAD", "rb2301");
	WTSBarStruct first = bar_at("rb2301", R4_M1_931, 3900.0);
	WTSBarStruct malformed = bar_at("rb2301", R4_M1_932, 3910.0);
	WTSBarStruct last = bar_at("rb2301", R4_M1_933, 3920.0);
	const std::string short_value((const char*)&malformed, sizeof(malformed) - 1);
	seed_bar_values("min1", "EXK_BAD_LOAD", "rb2301", {
		{R4_M1_931, std::string((const char*)&first, sizeof(first))},
		{R4_M1_932, short_value},
		{R4_M1_933, std::string((const char*)&last, sizeof(last))}
		});
	init_reader();

	EXPECT_EQ(_reader->readKlineSlice("EXK_BAD_LOAD.rb2301", KP_Minute1, 3,
		20230103100000000ull), nullptr);
	EXPECT_NE(_sink->last_log(LL_ERROR).find("Malformed bar key/value in LMDB while loading"), std::string::npos);
	EXPECT_EQ(_sink->bar_calls(), 0u);

	// 失败的首次装载没有发布部分数据；onMinuteEnd 也应安全跳过空缓存项。
	_reader->onMinuteEnd(R4_RD1, 1001, 0);
	EXPECT_EQ(_sink->bar_calls(), 0u);
	EXPECT_EQ(_sink->all_updated(), 1u);

	// 初次失败后重复请求会重新回源，而不是把失败当成已装载。
	EXPECT_EQ(_reader->readKlineSlice("EXK_BAD_LOAD.rb2301", KP_Minute1, 3,
		20230103100000000ull), nullptr);
	EXPECT_EQ(_sink->count_containing("Malformed bar key/value in LMDB while loading"), 2u);

	// 关闭 reader 后改正坏记录，再初始化新的 public reader 验证完整合法数据。
	_reader.reset();
	seed_bar_values("min1", "EXK_BAD_LOAD", "rb2301", {
		{R4_M1_932, std::string((const char*)&malformed, sizeof(malformed))}
		});
	init_reader();
	WTSKlineSlice* repaired = _reader->readKlineSlice("EXK_BAD_LOAD.rb2301", KP_Minute1, 3,
		20230103100000000ull);
	ASSERT_NE(repaired, nullptr);
	ASSERT_EQ(repaired->get_block_size(0), 3u);
	const WTSBarStruct* repaired_bars = repaired->get_block_addr(0);
	EXPECT_EQ(repaired_bars[0].time, (uint64_t)R4_M1_931);
	EXPECT_EQ(repaired_bars[1].time, (uint64_t)R4_M1_932);
	EXPECT_DOUBLE_EQ(repaired_bars[1].open, 3910.0);
	EXPECT_EQ(repaired_bars[2].time, (uint64_t)R4_M1_933);
	repaired->release();
}

//////////////////////////////////////////////////////////////////////////
// 增量回源先校验整批 value，坏项不能先追加前面的合法项或通知 sink。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, KlineIncrementalRejectsShortValueWithoutCachePollution)
{
	add_stock("EXK_BAD_UPDATE", "rb2301");
	WTSBarStruct malformed = bar_at("rb2301", R4_M1_933, 3920.0);
	WTSBarStruct pending = bar_at("rb2301", R4_M1_933, 3920.0);
	WTSBarStruct cached1 = bar_at("rb2301", R4_M1_931, 3900.0);
	WTSBarStruct cached2 = bar_at("rb2301", R4_M1_932, 3910.0);
	const std::string short_value((const char*)&malformed, sizeof(malformed) - 1);
	// 首读取最新两个 key (940/941)，value 的 bar.time 为 931/932；增量游标从
	// key 932 开始，先遇到一根会被追加的合法 bar，再遇到 key 933 的短 value。
	// 正常写入器会令 key.time == bar.time；这里的可控键值错位使 public API 能在
	// 单一 LMDB snapshot 中验证“合法前缀 + 坏记录”不会部分提交。
	seed_bar_values("min1", "EXK_BAD_UPDATE", "rb2301", {
		{R4_M1_932, std::string((const char*)&pending, sizeof(pending))},
		{R4_M1_933, short_value},
		{R4_M1_940, std::string((const char*)&cached1, sizeof(cached1))},
		{R4_M1_940 + 1, std::string((const char*)&cached2, sizeof(cached2))}
		});
	init_reader();

	EXPECT_EQ(_reader->readKlineSlice("EXK_BAD_UPDATE.rb2301", KP_Minute1, 2,
		20230103100000000ull), nullptr);
	EXPECT_NE(_sink->last_log(LL_ERROR).find("Malformed bar key/value in LMDB while updating"), std::string::npos);
	EXPECT_EQ(_sink->bar_calls(), 0u);

	// 故障返回后，用末 bar 时间作为截止值可直接读取首装缓存；两根数据未被
	// 增量查询中的合法前缀部分追加或覆盖。
	WTSKlineSlice* cached = _reader->readKlineSlice("EXK_BAD_UPDATE.rb2301", KP_Minute1, 2,
		20230103093200000ull);
	ASSERT_NE(cached, nullptr);
	ASSERT_EQ(cached->get_block_size(0), 2u);
	const WTSBarStruct* cached_bars = cached->get_block_addr(0);
	EXPECT_EQ(cached_bars[0].time, (uint64_t)R4_M1_931);
	EXPECT_DOUBLE_EQ(cached_bars[0].open, 3900.0);
	EXPECT_EQ(cached_bars[1].time, (uint64_t)R4_M1_932);
	EXPECT_DOUBLE_EQ(cached_bars[1].open, 3910.0);
	cached->release();
	EXPECT_EQ(_sink->bar_calls(), 0u);

	// onMinuteEnd 也必须将增量错误视为失败，不能再从 RT 缓存补一条绕过坏记录。
	_reader->onMinuteEnd(R4_RD1, 1001, 0);
	EXPECT_EQ(_sink->bar_calls(), 0u);

	_reader.reset();
	std::error_code ec;
	std::filesystem::remove_all(_root / "min1" / "EXK_BAD_UPDATE", ec);
	seed_bars("min1", "EXK_BAD_UPDATE", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)},
		{R4_M1_932, bar_at("rb2301", R4_M1_932, 3910.0)},
		{R4_M1_933, bar_at("rb2301", R4_M1_933, 3920.0)},
		{R4_M1_940, bar_at("rb2301", R4_M1_940, 3930.0)}
		});
	init_reader();
	WTSKlineSlice* repaired = _reader->readKlineSlice("EXK_BAD_UPDATE.rb2301", KP_Minute1, 4,
		20230103100000000ull);
	ASSERT_NE(repaired, nullptr);
	ASSERT_EQ(repaired->get_block_size(0), 4u);
	const WTSBarStruct* repaired_bars = repaired->get_block_addr(0);
	EXPECT_EQ(repaired_bars[0].time, (uint64_t)R4_M1_931);
	EXPECT_EQ(repaired_bars[1].time, (uint64_t)R4_M1_932);
	EXPECT_EQ(repaired_bars[2].time, (uint64_t)R4_M1_933);
	EXPECT_EQ(repaired_bars[3].time, (uint64_t)R4_M1_940);
	repaired->release();
}

//////////////////////////////////////////////////////////////////////////
// short/long raw key must be rejected by the actual first-load reader path.
//////////////////////////////////////////////////////////////////////////
TEST_P(R4ReaderADRawKeyTest, InitialLoadRejectsMalformedRawKeyAndCanRetry)
{
	const R4RawBarKeyLength keyLength = GetParam();
	const char* exchg = keyLength == R4RawBarKeyLength::Short ? "EXKSI" : "EXKLI";
	const std::string stdCode = std::string(exchg) + ".rb2301";
	add_stock(exchg, "rb2301");
	WTSBarStruct b931 = bar_at("rb2301", R4_M1_931, 3900.0);
	WTSBarStruct b932 = bar_at("rb2301", R4_M1_932, 3910.0);
	WTSBarStruct b933 = bar_at("rb2301", R4_M1_933, 3920.0);
	WTSBarStruct b940 = bar_at("rb2301", R4_M1_940, 3930.0);
	seed_raw_bar_values("min1", exchg, {
		{bar_key_str(exchg, "rb2301", R4_M1_931), std::string((const char*)&b931, sizeof(b931))},
		{raw_bar_key_str(exchg, "rb2301", R4_M1_932, keyLength), std::string((const char*)&b932, sizeof(b932))},
		{bar_key_str(exchg, "rb2301", R4_M1_933), std::string((const char*)&b933, sizeof(b933))},
		{bar_key_str(exchg, "rb2301", R4_M1_940), std::string((const char*)&b940, sizeof(b940))}
		});
	init_reader();

	EXPECT_EQ(_reader->readKlineSlice(stdCode.c_str(), KP_Minute1, 4,
		20230103100000000ull), nullptr);
	EXPECT_NE(_sink->last_log(LL_ERROR).find("Malformed bar key/value in LMDB while loading"), std::string::npos);
	EXPECT_EQ(_sink->bar_calls(), 0u);

	// 同一 reader 重试一个不包含坏 key 的较窄窗口，确认失败没有污染 cache。
	WTSKlineSlice* retry = _reader->readKlineSlice(stdCode.c_str(), KP_Minute1, 2,
		20230103100000000ull);
	ASSERT_NE(retry, nullptr);
	ASSERT_EQ(retry->get_block_size(0), 2u);
	const WTSBarStruct* retry_bars = retry->get_block_addr(0);
	EXPECT_EQ(retry_bars[0].time, (uint64_t)R4_M1_933);
	EXPECT_EQ(retry_bars[1].time, (uint64_t)R4_M1_940);
	retry->release();
	EXPECT_EQ(_sink->bar_calls(), 0u);

	// LMDB 环境由 reader 持有到析构；关闭 reader 后修复记录并用完整窗口重读。
	_reader.reset();
	std::error_code ec;
	std::filesystem::remove_all(_root / "min1" / exchg, ec);
	seed_bars("min1", exchg, "rb2301", {
		{R4_M1_931, b931}, {R4_M1_932, b932}, {R4_M1_933, b933}, {R4_M1_940, b940}
		});
	init_reader();
	WTSKlineSlice* repaired = _reader->readKlineSlice(stdCode.c_str(), KP_Minute1, 4,
		20230103100000000ull);
	ASSERT_NE(repaired, nullptr);
	ASSERT_EQ(repaired->get_block_size(0), 4u);
	EXPECT_EQ(repaired->get_block_addr(0)[0].time, (uint64_t)R4_M1_931);
	EXPECT_EQ(repaired->get_block_addr(0)[3].time, (uint64_t)R4_M1_940);
	repaired->release();
}

//////////////////////////////////////////////////////////////////////////
// 增量查询先遇合法前缀再遇 raw key 坏记录，验证整批校验和 cache 不污染。
//////////////////////////////////////////////////////////////////////////
TEST_P(R4ReaderADRawKeyTest, IncrementalUpdateRejectsMalformedRawKeyWithoutPollution)
{
	const R4RawBarKeyLength keyLength = GetParam();
	const char* exchg = keyLength == R4RawBarKeyLength::Short ? "EXKSU" : "EXKLU";
	const std::string stdCode = std::string(exchg) + ".rb2301";
	const uint32_t initialTime1 = 3301030859u;
	const uint32_t initialTime2 = 3301030900u;
	const uint32_t badKeyTime = initialTime2 + 256u; // 11:56: bytewise after 09:00 and before 12:00.
	const uint32_t laterTime = 3301031157u;
	const uint32_t cutoffTime = 3301031200u;
	add_stock(exchg, "rb2301");
	WTSBarStruct badKeyValue = bar_at("rb2301", badKeyTime, 3930.0);
	WTSBarStruct cached1 = bar_at("rb2301", initialTime1, 3900.0);
	WTSBarStruct cached2 = bar_at("rb2301", initialTime2, 3910.0);
	WTSBarStruct later1 = bar_at("rb2301", laterTime, 3940.0);
	WTSBarStruct later2 = bar_at("rb2301", cutoffTime, 3950.0);
	// 先在 09:00 截止时缓存 08:59/09:00；11:56 的 malformed key 位于本次
	// 增量查询范围内，但不应影响更早的首读。它后面还有合法记录，验证整批校验。
	seed_raw_bar_values("min1", exchg, {
		{bar_key_str(exchg, "rb2301", initialTime1), std::string((const char*)&cached1, sizeof(cached1))},
		{bar_key_str(exchg, "rb2301", initialTime2), std::string((const char*)&cached2, sizeof(cached2))},
		{raw_bar_key_str(exchg, "rb2301", badKeyTime, keyLength),
			std::string((const char*)&badKeyValue, sizeof(badKeyValue))},
		{bar_key_str(exchg, "rb2301", laterTime), std::string((const char*)&later1, sizeof(later1))},
		{bar_key_str(exchg, "rb2301", cutoffTime), std::string((const char*)&later2, sizeof(later2))}
		});
	init_reader();

	WTSKlineSlice* first = _reader->readKlineSlice(stdCode.c_str(), KP_Minute1, 2,
		20230103090000000ull);
	ASSERT_NE(first, nullptr);
	ASSERT_EQ(first->size(), 2);
	EXPECT_EQ(first->at(0)->time, (uint64_t)initialTime1);
	EXPECT_EQ(first->at(1)->time, (uint64_t)initialTime2);
	first->release();

	EXPECT_EQ(_reader->readKlineSlice(stdCode.c_str(), KP_Minute1, 2,
		20230103120000000ull), nullptr);
	EXPECT_NE(_sink->last_log(LL_ERROR).find("Malformed bar key/value in LMDB while updating"), std::string::npos);
	EXPECT_EQ(_sink->bar_calls(), 0u);

	// 以末 bar 时间为截止值重读，公开切片仍只能看到最初装入的两根缓存数据。
	WTSKlineSlice* cached = _reader->readKlineSlice(stdCode.c_str(), KP_Minute1, 2,
		20230103090000000ull);
	ASSERT_NE(cached, nullptr);
	ASSERT_EQ(cached->get_block_size(0), 2u);
	const WTSBarStruct* cached_bars = cached->get_block_addr(0);
	EXPECT_EQ(cached_bars[0].time, (uint64_t)initialTime1);
	EXPECT_DOUBLE_EQ(cached_bars[0].open, 3900.0);
	EXPECT_EQ(cached_bars[1].time, (uint64_t)initialTime2);
	EXPECT_DOUBLE_EQ(cached_bars[1].open, 3910.0);
	cached->release();
	EXPECT_EQ(_sink->bar_calls(), 0u);

	// 再次请求同一窗口可从未污染缓存返回；更新错误依旧被隔离。
	WTSKlineSlice* retry = _reader->readKlineSlice(stdCode.c_str(), KP_Minute1, 1,
		20230103090000000ull);
	ASSERT_NE(retry, nullptr);
	EXPECT_DOUBLE_EQ(retry->get_block_addr(0)[0].open, 3910.0);
	retry->release();
	EXPECT_EQ(_sink->bar_calls(), 0u);

	// LMDB 环境不能在此 reader 存活时由 fixture 再开 writer；关闭后修复数据，
	// 新 reader 的完整读取作为存储修复后的重试控制。
	_reader.reset();
	std::error_code ec;
	std::filesystem::remove_all(_root / "min1" / exchg, ec);
	seed_bars("min1", exchg, "rb2301", {
		{initialTime1, bar_at("rb2301", initialTime1, 3900.0)},
		{initialTime2, bar_at("rb2301", initialTime2, 3910.0)},
		{badKeyTime, bar_at("rb2301", badKeyTime, 3920.0)},
		{cutoffTime, bar_at("rb2301", cutoffTime, 3930.0)}
		});
	init_reader();
	WTSKlineSlice* repaired = _reader->readKlineSlice(stdCode.c_str(), KP_Minute1, 4,
		20230103120000000ull);
	ASSERT_NE(repaired, nullptr);
	ASSERT_EQ(repaired->get_block_size(0), 4u);
	EXPECT_EQ(repaired->get_block_addr(0)[0].time, (uint64_t)initialTime1);
	EXPECT_EQ(repaired->get_block_addr(0)[3].time, (uint64_t)cutoffTime);
	repaired->release();
}

INSTANTIATE_TEST_SUITE_P(ShortAndLongRawKeys, R4ReaderADRawKeyTest,
	::testing::Values(R4RawBarKeyLength::Short, R4RawBarKeyLength::Long),
	[](const ::testing::TestParamInfo<R4RawBarKeyLength>& info) {
		return info.param == R4RawBarKeyLength::Short ? "ShortKey" : "LongKey";
	});

//////////////////////////////////////////////////////////////////////////
// 6. 同一交易所下第二个合约仍应读出它自己的已落盘 K 线；同合约同 etime
//    复读作为正常数据对照。资源句柄生命周期由 fixture/reader 析构清理。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, SecondCodeOnSameExchangeReadsSeededBars)
{
	add_stock("EXK2", "rb2301");
	add_stock("EXK2", "rb2302");
	seed_bars("min1", "EXK2", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)},
		{R4_M1_932, bar_at("rb2301", R4_M1_932, 3910.0)}
		});
	seed_bars("min1", "EXK2", "rb2302", {
		{R4_M1_931, bar_at("rb2302", R4_M1_931, 4900.0)}
		});
	init_reader();

	uint64_t etime = 20230103100000000ull;
	WTSKlineSlice* s1 = _reader->readKlineSlice("EXK2.rb2301", KP_Minute1, 2, etime);
	ASSERT_TRUE(s1 != NULL);
	EXPECT_EQ(s1->get_block_counts(), (std::size_t)1);
	EXPECT_EQ(s1->get_block_size(0), 2u);
	if (s1->get_block_counts() == 1 && s1->get_block_size(0) == 2)
	{
		EXPECT_DOUBLE_EQ(s1->get_block_addr(0)[0].open, 3900.0);
		EXPECT_DOUBLE_EQ(s1->get_block_addr(0)[1].open, 3910.0);
	}
	s1->release();

	// rb2302 的合法数据不能因 rb2301 首次读取而消失。
	WTSKlineSlice* s2 = _reader->readKlineSlice("EXK2.rb2302", KP_Minute1, 1, etime);
	ASSERT_TRUE(s2 != NULL);
	EXPECT_EQ(s2->get_block_counts(), (std::size_t)1);
	EXPECT_EQ(s2->get_block_size(0), 1u);
	if (s2->get_block_counts() == 1 && s2->get_block_size(0) == 1)
	{
		const WTSBarStruct* b2 = s2->get_block_addr(0);
		EXPECT_DOUBLE_EQ(b2[0].open, 4900.0);
		EXPECT_DOUBLE_EQ(b2[0].close, 4900.5);
		EXPECT_EQ(b2[0].time, (uint64_t)R4_M1_931);
	}
	s2->release();

	// 同一合约同 etime 再读仍返回首次读取的两根数据。
	WTSKlineSlice* s3 = _reader->readKlineSlice("EXK2.rb2301", KP_Minute1, 2, etime);
	ASSERT_TRUE(s3 != NULL);
	ASSERT_EQ(s3->get_block_size(0), 2u);
	const WTSBarStruct* b3 = s3->get_block_addr(0);
	EXPECT_DOUBLE_EQ(b3[0].open, 3900.0);
	EXPECT_DOUBLE_EQ(b3[1].open, 3910.0);
	s3->release();
}

//////////////////////////////////////////////////////////////////////////
// 7. RT 缓存补最后一根：cache_m1.dmb 里有更靠前的未完成 bar → 从缓存追加
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, KlineAppendsRtCacheBarWhenLmdbLags)
{
	add_stock("EXK3", "rb2301");
	seed_bars("min1", "EXK3", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)},
		{R4_M1_932, bar_at("rb2301", R4_M1_932, 3910.0)},
		{R4_M1_933, bar_at("rb2301", R4_M1_933, 3920.0)}
		});

	// 手写一个 m1 实时缓存块：rb2301 正在拼 09:40 这根（还没进 LMDB）
	std::vector<BarCacheItem> items;
	items.push_back(make_bar_item("EXK3", "rb2301", R4_RD1, R4_M1_940, 3930.0));
	write_block_file((_root / "cache_m1.dmb").string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1, bar_payload(items));

	init_reader();

	WTSKlineSlice* slice = _reader->readKlineSlice("EXK3.rb2301", KP_Minute1, 3, 20230103100000000ull);
	ASSERT_TRUE(slice != NULL);
	// 追加缓存 bar 时缓冲区已满 → 挤出 09:31，并回绕成两个块
	EXPECT_EQ(slice->get_block_counts(), (std::size_t)2);
	ASSERT_EQ(slice->get_block_size(0), 2u);
	const WTSBarStruct* b = slice->get_block_addr(0);
	EXPECT_EQ(b[0].time, (uint64_t)R4_M1_932);
	EXPECT_EQ(b[1].time, (uint64_t)R4_M1_933);
	ASSERT_EQ(slice->get_block_size(1), 1u);
	const WTSBarStruct* b2 = slice->get_block_addr(1);
	EXPECT_EQ(b2[0].time, (uint64_t)R4_M1_940);
	EXPECT_DOUBLE_EQ(b2[0].open, 3930.0);
	slice->release();

	// 命中"从缓存补线"分支的日志（L462-463）
	EXPECT_NE(_sink->last_log(LL_DEBUG).find("updated from cache instead of lmdb"), std::string::npos)
		<< _sink->last_log(LL_DEBUG);
}

//////////////////////////////////////////////////////////////////////////
// 8. RT 缓存里没有该合约 → 不补线；缓存文件不存在 → 也不建目录
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, KlineWithoutRtCacheEntryKeepsLmdbBarsOnly)
{
	add_stock("EXK4", "rb2301");
	seed_bars("min1", "EXK4", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)},
		{R4_M1_932, bar_at("rb2301", R4_M1_932, 3910.0)}
		});

	std::vector<BarCacheItem> items;
	items.push_back(make_bar_item("EXK4", "rb9999", R4_RD1, R4_M1_940, 1.0));
	write_block_file((_root / "cache_m1.dmb").string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1, bar_payload(items));

	init_reader();

	WTSKlineSlice* slice = _reader->readKlineSlice("EXK4.rb2301", KP_Minute1, 2, 20230103100000000ull);
	ASSERT_TRUE(slice != NULL);
	ASSERT_EQ(slice->get_block_size(0), 2u);
	const WTSBarStruct* b = slice->get_block_addr(0);
	EXPECT_EQ(b[0].time, (uint64_t)R4_M1_931);
	EXPECT_EQ(b[1].time, (uint64_t)R4_M1_932);
	slice->release();
	EXPECT_EQ(_sink->last_log(LL_DEBUG).find("updated from cache instead of lmdb"), std::string::npos);
}

//////////////////////////////////////////////////////////////////////////
// 9. 日线：键用 bar.date，缓存文件走 _d1_cache
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, DayKlineUsesDateKeyAndDaySubdir)
{
	add_stock("EXD9", "rb2301");
	WTSBarStruct second = bar_at("rb2301", 0, 4000.0);
	second.date = 20230104;
	seed_bars("day", "EXD9", "rb2301", {
		{R4_RD1, bar_at("rb2301", R4_M1_931, 3900.0)},
		{20230104, second}
		});
	init_reader();

	// 自然日与交易日相同的股票在 20230103 cutoff 下不包含 20230104 日线。
	WTSKlineSlice* slice = _reader->readKlineSlice("EXD9.rb2301", KP_DAY, 2, 20230103100000000ull);
	ASSERT_TRUE(slice != NULL);
	ASSERT_EQ(slice->get_block_size(0), 1u);
	const WTSBarStruct* b = slice->get_block_addr(0);
	EXPECT_EQ(b[0].date, R4_RD1);
	EXPECT_DOUBLE_EQ(b[0].open, 3900.0);
	slice->release();
	EXPECT_TRUE(std::filesystem::exists(_root / "day" / "EXD9" / "data.mdb"));
}

//////////////////////////////////////////////////////////////////////////
// R122：AD K 线 cutoff 使用 17 位时间；首次读取仅返回 cutoff 及之前的 bars。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, KlineFirstReadHonorsInclusiveCutoffForMin1AndMin5)
{
	add_stock("EXCUT", "rb2301");
	const uint32_t m5_930 = 3301030930u;
	const uint32_t m5_935 = 3301030935u;
	const uint32_t m5_940 = 3301030940u;
	const uint32_t m5_945 = 3301030945u;
	seed_bars("min1", "EXCUT", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)},
		{R4_M1_932, bar_at("rb2301", R4_M1_932, 3910.0)},
		{R4_M1_933, bar_at("rb2301", R4_M1_933, 3920.0)},
		{R4_M1_940, bar_at("rb2301", R4_M1_940, 3930.0)}
		});
	seed_bars("min5", "EXCUT", "rb2301", {
		{m5_930, bar_at("rb2301", m5_930, 4000.0)},
		{m5_935, bar_at("rb2301", m5_935, 4010.0)},
		{m5_940, bar_at("rb2301", m5_940, 4020.0)},
		{m5_945, bar_at("rb2301", m5_945, 4030.0)}
		});
	init_reader();

	WTSKlineSlice* m1 = _reader->readKlineSlice("EXCUT.rb2301", KP_Minute1, 3,
		20230103093300000ull);
	ASSERT_NE(m1, nullptr);
	ASSERT_EQ(m1->get_block_size(0), 3u);
	const WTSBarStruct* m1_bars = m1->get_block_addr(0);
	EXPECT_EQ(m1_bars[0].time, (uint64_t)R4_M1_931);
	EXPECT_EQ(m1_bars[1].time, (uint64_t)R4_M1_932);
	EXPECT_EQ(m1_bars[2].time, (uint64_t)R4_M1_933);
	m1->release();

	// IDataReader's 12-digit minute format is also supported by this AD reader.
	WTSKlineSlice* m1_12_digit = _reader->readKlineSlice("EXCUT.rb2301", KP_Minute1, 3,
		202301030933ull);
	ASSERT_NE(m1_12_digit, nullptr);
	ASSERT_EQ(m1_12_digit->get_block_size(0), 3u);
	EXPECT_EQ(m1_12_digit->get_block_addr(0)[0].time, (uint64_t)R4_M1_931);
	EXPECT_EQ(m1_12_digit->get_block_addr(0)[2].time, (uint64_t)R4_M1_933);
	m1_12_digit->release();

	WTSKlineSlice* m5 = _reader->readKlineSlice("EXCUT.rb2301", KP_Minute5, 2,
		20230103094000000ull);
	ASSERT_NE(m5, nullptr);
	ASSERT_EQ(m5->get_block_size(0), 2u);
	const WTSBarStruct* m5_bars = m5->get_block_addr(0);
	EXPECT_EQ(m5_bars[0].time, (uint64_t)m5_935);
	EXPECT_EQ(m5_bars[1].time, (uint64_t)m5_940);
	m5->release();
}

TEST_F(R4ReaderADTest, KlineSameReaderEarlierCutoffReloadsHistoricalBars)
{
	add_stock("EXBACK", "rb2301");
	seed_bars("min1", "EXBACK", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)},
		{R4_M1_932, bar_at("rb2301", R4_M1_932, 3910.0)},
		{R4_M1_933, bar_at("rb2301", R4_M1_933, 3920.0)},
		{R4_M1_940, bar_at("rb2301", R4_M1_940, 3930.0)},
		{3301030941u, bar_at("rb2301", 3301030941u, 3940.0)},
		{3301030942u, bar_at("rb2301", 3301030942u, 3950.0)}
		});
	init_reader();

	WTSKlineSlice* late = _reader->readKlineSlice("EXBACK.rb2301", KP_Minute1, 3,
		20230103094200000ull);
	ASSERT_NE(late, nullptr);
	ASSERT_EQ(late->get_block_size(0), 3u);
	EXPECT_EQ(late->get_block_addr(0)[0].time, (uint64_t)R4_M1_940);
	EXPECT_EQ(late->get_block_addr(0)[2].time, 3301030942u);
	late->release();

	// 同一个 reader 回退到更早 cutoff，必须从足量 LMDB 历史中取早期窗口。
	WTSKlineSlice* early = _reader->readKlineSlice("EXBACK.rb2301", KP_Minute1, 3,
		20230103093300000ull);
	ASSERT_NE(early, nullptr);
	ASSERT_EQ(early->get_block_size(0), 3u);
	const WTSBarStruct* early_bars = early->get_block_addr(0);
	EXPECT_EQ(early_bars[0].time, (uint64_t)R4_M1_931);
	EXPECT_EQ(early_bars[1].time, (uint64_t)R4_M1_932);
	EXPECT_EQ(early_bars[2].time, (uint64_t)R4_M1_933);
	early->release();
}

TEST_F(R4ReaderADTest, KlineForwardIncrementIsBoundedByCutoff)
{
	add_stock("EXFWD", "rb2301");
	seed_bars("min1", "EXFWD", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)},
		{R4_M1_932, bar_at("rb2301", R4_M1_932, 3910.0)},
		{R4_M1_933, bar_at("rb2301", R4_M1_933, 3920.0)},
		{R4_M1_940, bar_at("rb2301", R4_M1_940, 3930.0)},
		{3301030945u, bar_at("rb2301", 3301030945u, 3940.0)}
		});
	init_reader();

	WTSKlineSlice* first = _reader->readKlineSlice("EXFWD.rb2301", KP_Minute1, 3,
		20230103093200000ull);
	ASSERT_NE(first, nullptr);
	ASSERT_EQ(first->get_block_size(0), 2u);
	first->release();

	// Existing capacity is sufficient, so this exercises the forward LMDB update path.
	WTSKlineSlice* forward = _reader->readKlineSlice("EXFWD.rb2301", KP_Minute1, 3,
		20230103094000000ull);
	ASSERT_NE(forward, nullptr);
	ASSERT_EQ(forward->size(), 3);
	EXPECT_EQ(forward->at(0)->time, (uint64_t)R4_M1_932);
	EXPECT_EQ(forward->at(1)->time, (uint64_t)R4_M1_933);
	EXPECT_EQ(forward->at(2)->time, (uint64_t)R4_M1_940);
	forward->release();
}

TEST_F(R4ReaderADTest, KlineCountGrowthReloadsTheRequestedCutoffWindow)
{
	add_stock("EXGROW", "rb2301");
	seed_bars("min1", "EXGROW", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)},
		{R4_M1_932, bar_at("rb2301", R4_M1_932, 3910.0)},
		{R4_M1_933, bar_at("rb2301", R4_M1_933, 3920.0)},
		{R4_M1_940, bar_at("rb2301", R4_M1_940, 3930.0)}
		});
	init_reader();

	WTSKlineSlice* small = _reader->readKlineSlice("EXGROW.rb2301", KP_Minute1, 1,
		20230103094000000ull);
	ASSERT_NE(small, nullptr);
	ASSERT_EQ(small->get_block_size(0), 1u);
	EXPECT_EQ(small->get_block_addr(0)[0].time, (uint64_t)R4_M1_940);
	small->release();

	WTSKlineSlice* larger = _reader->readKlineSlice("EXGROW.rb2301", KP_Minute1, 3,
		20230103094000000ull);
	ASSERT_NE(larger, nullptr);
	ASSERT_EQ(larger->size(), 3);
	EXPECT_EQ(larger->at(0)->time, (uint64_t)R4_M1_932);
	EXPECT_EQ(larger->at(1)->time, (uint64_t)R4_M1_933);
	EXPECT_EQ(larger->at(2)->time, (uint64_t)R4_M1_940);
	larger->release();
}

TEST_F(R4ReaderADTest, KlineZeroCountWithoutInitializedCacheReturnsNull)
{
	add_stock("EXZERO_EMPTY", "rb2301");
	init_reader();
	EXPECT_EQ(nullptr, _reader->readKlineSlice("EXZERO_EMPTY.rb2301", KP_Minute1, 0,
		20230103093300000ull));
}

TEST_F(R4ReaderADTest, KlineRealtimeBarCanFillAnEmptyLmDbCacheWithoutLeakingFuture)
{
	add_stock("EXRT_ONLY", "rb2301");
	seed_bars("min1", "EXRT_ONLY", "rb2301", {});
	std::vector<BarCacheItem> items{
		make_bar_item("EXRT_ONLY", "rb2301", R4_RD1, R4_M1_940, 3940.0)};
	write_block_file((_root / "cache_m1.dmb").string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1,
		bar_payload(items));
	init_reader();

	// The RT bar must stay hidden before its inclusive cutoff, even when history is empty.
	EXPECT_EQ(nullptr, _reader->readKlineSlice("EXRT_ONLY.rb2301", KP_Minute1, 3,
		20230103093300000ull));

	WTSKlineSlice* exact = _reader->readKlineSlice("EXRT_ONLY.rb2301", KP_Minute1, 3,
		20230103094000000ull);
	ASSERT_NE(exact, nullptr);
	ASSERT_EQ(exact->size(), 1);
	EXPECT_EQ(exact->at(0)->time, (uint64_t)R4_M1_940);
	EXPECT_DOUBLE_EQ(exact->at(0)->open, 3940.0);
	exact->release();
}

TEST_F(R4ReaderADTest, KlineRealtimeBarCanAppearOnARepeatedSameCutoffRead)
{
	add_stock("EXRT_LATE", "rb2301");
	seed_bars("min1", "EXRT_LATE", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)},
		{R4_M1_932, bar_at("rb2301", R4_M1_932, 3910.0)}
		});
	init_reader();

	const uint64_t cutoff = 20230103094000000ull;
	WTSKlineSlice* before = _reader->readKlineSlice("EXRT_LATE.rb2301", KP_Minute1, 3, cutoff);
	ASSERT_NE(before, nullptr);
	ASSERT_EQ(before->size(), 2);
	EXPECT_EQ(before->at(1)->time, (uint64_t)R4_M1_932);
	before->release();

	// Simulate the writer publishing the still-open bar after the first read.
	std::vector<BarCacheItem> items{
		make_bar_item("EXRT_LATE", "rb2301", R4_RD1, R4_M1_940, 3940.0)};
	write_block_file((_root / "cache_m1.dmb").string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1,
		bar_payload(items));

	WTSKlineSlice* after = _reader->readKlineSlice("EXRT_LATE.rb2301", KP_Minute1, 3, cutoff);
	ASSERT_NE(after, nullptr);
	ASSERT_EQ(after->size(), 3);
	EXPECT_EQ(after->at(0)->time, (uint64_t)R4_M1_931);
	EXPECT_EQ(after->at(1)->time, (uint64_t)R4_M1_932);
	EXPECT_EQ(after->at(2)->time, (uint64_t)R4_M1_940);
	EXPECT_DOUBLE_EQ(after->at(2)->open, 3940.0);
	after->release();
}

TEST_F(R4ReaderADTest, KlineForwardIncrementDoesNotStopAt9999Rows)
{
	add_stock("EXPAGE", "rb2301");

	auto next_date = [](uint32_t date) {
		uint32_t year = date / 10000;
		uint32_t month = (date / 100) % 100;
		uint32_t day = date % 100;
		static const uint32_t days_per_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
		uint32_t days = days_per_month[month - 1];
		if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
			days++;
		if (++day > days)
		{
			day = 1;
			if (++month > 12)
			{
				month = 1;
				++year;
			}
		}
		return year * 10000 + month * 100 + day;
	};
	auto make_row = [](uint32_t date, uint32_t minute_index, double px) {
		const uint32_t hhmm = (9 + minute_index / 60) * 100 + minute_index % 60;
		const uint32_t key_time = (date - 19900000u) * 10000u + hhmm;
		WTSBarStruct bar = bar_at("rb2301", key_time, px);
		bar.date = date;
		return std::make_pair(key_time, bar);
	};
	const uint32_t rows_count = 10003;
	std::vector<std::pair<uint32_t, WTSBarStruct>> rows;
	rows.reserve(rows_count);
	uint32_t date = R4_RD1;
	for (uint32_t i = 0; i < rows_count; i++)
	{
		if (i > 0 && i % 360 == 0)
			date = next_date(date);
		rows.push_back(make_row(date, i % 360, 3900.0 + i));
	}
	seed_bars("min1", "EXPAGE", "rb2301", rows);
	init_reader();

	auto etime_for = [&next_date](uint32_t row_index) {
		uint32_t date = R4_RD1;
		for (uint32_t d = 0; d < row_index / 360; d++)
			date = next_date(date);
		const uint32_t hhmm = (9 + (row_index % 360) / 60) * 100 + row_index % 60;
		return static_cast<uint64_t>(date) * 1000000000ull + static_cast<uint64_t>(hhmm) * 100000ull;
	};

	WTSKlineSlice* first = _reader->readKlineSlice("EXPAGE.rb2301", KP_Minute1, 3, etime_for(2));
	ASSERT_NE(first, nullptr);
	ASSERT_EQ(first->size(), 3);
	EXPECT_EQ(first->at(0)->time, rows[0].second.time);
	EXPECT_EQ(first->at(2)->time, rows[2].second.time);
	first->release();

	WTSKlineSlice* final = _reader->readKlineSlice("EXPAGE.rb2301", KP_Minute1, 3, etime_for(rows_count - 1));
	ASSERT_NE(final, nullptr);
	ASSERT_EQ(final->size(), 3);
	for (uint32_t i = 0; i < 3; i++)
	{
		EXPECT_EQ(final->at(i)->time, rows[rows_count - 3 + i].second.time);
		EXPECT_DOUBLE_EQ(final->at(i)->open, rows[rows_count - 3 + i].second.open);
	}
	final->release();
	// The inclusive lower row is already cached and must not trigger a duplicate callback.
	EXPECT_EQ(_sink->bar_calls(), rows_count - 3);
}

TEST_F(R4ReaderADTest, KlineDoesNotAppendRealtimeBarAfterCutoff)
{
	add_stock("EXRT_AFTER", "rb2301");
	seed_bars("min1", "EXRT_AFTER", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)},
		{R4_M1_932, bar_at("rb2301", R4_M1_932, 3910.0)}
		});
	std::vector<BarCacheItem> items{
		make_bar_item("EXRT_AFTER", "rb2301", R4_RD1, R4_M1_940, 3940.0)};
	write_block_file((_root / "cache_m1.dmb").string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1,
		bar_payload(items));
	init_reader();

	WTSKlineSlice* slice = _reader->readKlineSlice("EXRT_AFTER.rb2301", KP_Minute1, 3,
		20230103093300000ull);
	ASSERT_NE(slice, nullptr);
	ASSERT_EQ(slice->get_block_size(0), 2u);
	EXPECT_EQ(slice->get_block_addr(0)[0].time, (uint64_t)R4_M1_931);
	EXPECT_EQ(slice->get_block_addr(0)[1].time, (uint64_t)R4_M1_932);
	slice->release();
}

TEST_F(R4ReaderADTest, KlineAppendsRealtimeBarAtInclusiveCutoff)
{
	add_stock("EXRT_EXACT", "rb2301");
	seed_bars("min1", "EXRT_EXACT", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)},
		{R4_M1_932, bar_at("rb2301", R4_M1_932, 3910.0)}
		});
	std::vector<BarCacheItem> items{
		make_bar_item("EXRT_EXACT", "rb2301", R4_RD1, R4_M1_940, 3940.0)};
	write_block_file((_root / "cache_m1.dmb").string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1,
		bar_payload(items));
	init_reader();

	WTSKlineSlice* slice = _reader->readKlineSlice("EXRT_EXACT.rb2301", KP_Minute1, 3,
		20230103094000000ull);
	ASSERT_NE(slice, nullptr);
	ASSERT_EQ(slice->get_block_size(0), 3u);
	EXPECT_EQ(slice->get_block_addr(0)[0].time, (uint64_t)R4_M1_931);
	EXPECT_EQ(slice->get_block_addr(0)[1].time, (uint64_t)R4_M1_932);
	EXPECT_EQ(slice->get_block_addr(0)[2].time, (uint64_t)R4_M1_940);
	slice->release();
}

TEST_F(R4ReaderADTest, KlineDoesNotDuplicateRealtimeBarAlreadyInLMDB)
{
	add_stock("EXRT_DUP", "rb2301");
	seed_bars("min1", "EXRT_DUP", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)},
		{R4_M1_932, bar_at("rb2301", R4_M1_932, 3910.0)},
		{R4_M1_940, bar_at("rb2301", R4_M1_940, 3930.0)}
		});
	std::vector<BarCacheItem> items{
		make_bar_item("EXRT_DUP", "rb2301", R4_RD1, R4_M1_940, 3940.0)};
	write_block_file((_root / "cache_m1.dmb").string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1,
		bar_payload(items));
	init_reader();

	WTSKlineSlice* slice = _reader->readKlineSlice("EXRT_DUP.rb2301", KP_Minute1, 3,
		20230103100000000ull);
	ASSERT_NE(slice, nullptr);
	ASSERT_EQ(slice->get_block_size(0), 3u);
	const WTSBarStruct* bars = slice->get_block_addr(0);
	EXPECT_EQ(bars[0].time, (uint64_t)R4_M1_931);
	EXPECT_EQ(bars[1].time, (uint64_t)R4_M1_932);
	EXPECT_EQ(bars[2].time, (uint64_t)R4_M1_940);
	slice->release();
}

TEST_F(R4ReaderADTest, KlineDatabaseDirectoryCreationFailureReturnsNullEvenWithRealtimeBar)
{
	add_stock("EXKDIR", "rb2301");
	{
		std::ofstream blocker(_root / "min1", std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(blocker.good());
		blocker << "min1 is a file, not a directory";
	}
	std::vector<BarCacheItem> items{
		make_bar_item("EXKDIR", "rb2301", R4_RD1, R4_M1_940, 3940.0)};
	write_block_file((_root / "cache_m1.dmb").string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1,
		bar_payload(items));
	init_reader();

	WTSKlineSlice* slice = nullptr;
	EXPECT_NO_THROW(slice = _reader->readKlineSlice("EXKDIR.rb2301", KP_Minute1, 1,
		20230103094000000ull));
	EXPECT_EQ(slice, nullptr);
	if (slice != nullptr)
		slice->release();
	ASSERT_GT(_sink->log_count(LL_ERROR), 0u);
	EXPECT_NE(_sink->last_log(LL_ERROR).find("EXKDIR"), std::string::npos);
	EXPECT_NE(_sink->last_log(LL_ERROR).find("min1"), std::string::npos);
}

TEST_F(R4ReaderADTest, TickDatabaseDirectoryCreationFailureReturnsNullWithoutThrow)
{
	add_stock("EXTICKDIR", "rb2301");
	{
		std::ofstream blocker(_root / "ticks", std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(blocker.good());
		blocker << "ticks is a file, not a directory";
	}
	init_reader();

	WTSTickSlice* slice = nullptr;
	EXPECT_NO_THROW(slice = _reader->readTickSlice("EXTICKDIR.rb2301", 1,
		20230103094000000ull));
	EXPECT_EQ(slice, nullptr);
	if (slice != nullptr)
		slice->release();
	ASSERT_GT(_sink->log_count(LL_ERROR), 0u);
	EXPECT_NE(_sink->last_log(LL_ERROR).find("EXTICKDIR"), std::string::npos);
	EXPECT_NE(_sink->last_log(LL_ERROR).find("tick"), std::string::npos);
}

TEST_F(R4ReaderADTest, KlineCreatesMissingDirectoryAndUsesRealtimeBar)
{
	add_stock("EXKDIR_RT", "rb2301");
	ASSERT_FALSE(std::filesystem::exists(_root / "min1"));
	std::vector<BarCacheItem> items{
		make_bar_item("EXKDIR_RT", "rb2301", R4_RD1, R4_M1_940, 3940.0)};
	write_block_file((_root / "cache_m1.dmb").string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1,
		bar_payload(items));
	init_reader();

	WTSKlineSlice* slice = _reader->readKlineSlice("EXKDIR_RT.rb2301", KP_Minute1, 1,
		20230103094000000ull);
	ASSERT_NE(slice, nullptr);
	ASSERT_EQ(slice->size(), 1);
	EXPECT_EQ(slice->at(0)->time, (uint64_t)R4_M1_940);
	EXPECT_DOUBLE_EQ(slice->at(0)->open, 3940.0);
	slice->release();
	EXPECT_TRUE(std::filesystem::is_directory(_root / "min1" / "EXKDIR_RT"));
}

TEST_F(R4ReaderADTest, HotWithoutProductHistoryFailsClosedWithoutRawReplay)
{
	WTSCommodityInfo* hotComm = _bd->add_commodity("EXHOT", "rb", ContractCategory::CC_Future);
	ASSERT_NE(hotComm, nullptr);
	ASSERT_NE(_bd->add_contract("EXHOT", "rb2401", hotComm), nullptr);

	const std::filesystem::path rulesPath = _root / "hot_rules.json";
	{
		std::ofstream rules(rulesPath);
		ASSERT_TRUE(rules.good());
		rules << R"({"EXHOT":{"rb":[
			{"date":20230102,"from":"","to":"rb2401","oldclose":0.0,"newclose":0.0},
			{"date":20230104,"from":"rb2401","to":"rb2405","oldclose":0.0,"newclose":0.0}
		]}})";
	}
	_owned_hot_mgr.reset(new WTSHotMgr());
	ASSERT_TRUE(_owned_hot_mgr->loadCustomRules("HOT", rulesPath.string().c_str()));
	ASSERT_STREQ(_owned_hot_mgr->getCustomRawCode("HOT", "EXHOT.rb", R4_RD1), "rb2401");
	ASSERT_STREQ(_owned_hot_mgr->getRuleTag("EXHOT.rb.HOT"), "HOT");

	delete _sink;
	_sink = new R4ReaderHotSink(_bd, _owned_hot_mgr.get());

	// AD writer keys physical futures contracts. A physical row alone does not
	// prove whether this HOT reader should use raw contracts or legacy premerged
	// product history, so the AD reader must fail closed rather than infer a source.
	seed_bars("min1", "EXHOT", "rb2401", {
		{R4_M1_931, bar_at("rb2401", R4_M1_931, 3900.0)},
		{R4_M1_932, bar_at("rb2401", R4_M1_932, 3910.0)},
		{R4_M1_933, bar_at("rb2401", R4_M1_933, 3920.0)},
		{R4_M1_940, bar_at("rb2401", R4_M1_940, 3930.0)}
		});
	init_reader();

	WTSKlineSlice* hot = _reader->readKlineSlice("EXHOT.rb.HOT", KP_Minute1, 1,
		20230103094000000ull);
	EXPECT_EQ(hot, nullptr);
	if (hot != nullptr)
		hot->release();
	EXPECT_TRUE(_sink->bar_events().empty())
		<< "an ambiguous HOT source must not replay physical-contract history";
}

TEST_F(R4ReaderADTest, HotBackwardCutoffWithEmptyProductHistoryFailsClosed)
{
	WTSCommodityInfo* hotComm = _bd->add_commodity("EXHOT_BACK", "rb", ContractCategory::CC_Future);
	ASSERT_NE(hotComm, nullptr);
	ASSERT_NE(_bd->add_contract("EXHOT_BACK", "rb2401", hotComm), nullptr);

	const std::filesystem::path rulesPath = _root / "hot_rules_backward.json";
	{
		std::ofstream rules(rulesPath);
		ASSERT_TRUE(rules.good());
		rules << R"({"EXHOT_BACK":{"rb":[
			{"date":20230102,"from":"","to":"rb2401","oldclose":0.0,"newclose":0.0},
			{"date":20230104,"from":"rb2401","to":"rb2405","oldclose":0.0,"newclose":0.0}
		]}})";
	}
	_owned_hot_mgr.reset(new WTSHotMgr());
	ASSERT_TRUE(_owned_hot_mgr->loadCustomRules("HOT", rulesPath.string().c_str()));
	ASSERT_STREQ(_owned_hot_mgr->getCustomRawCode("HOT", "EXHOT_BACK.rb", R4_RD1), "rb2401");
	ASSERT_STREQ(_owned_hot_mgr->getRuleTag("EXHOT_BACK.rb.HOT"), "HOT");
	delete _sink;
	_sink = new R4ReaderHotSink(_bd, _owned_hot_mgr.get());

	// The first cutoff is served by legacy product history. At the earlier
	// cutoff, only physical-contract history exists; the source must not switch.
	seed_bars("min1", "EXHOT_BACK", "rb", {
		{R4_M1_940, bar_at("rb", R4_M1_940, 4930.0)}
		});
	seed_bars("min1", "EXHOT_BACK", "rb2401", {
		{R4_M1_931, bar_at("rb2401", R4_M1_931, 3900.0)},
		{R4_M1_932, bar_at("rb2401", R4_M1_932, 3910.0)},
		{R4_M1_933, bar_at("rb2401", R4_M1_933, 3920.0)},
		{R4_M1_940, bar_at("rb2401", R4_M1_940, 3940.0)}
		});
	init_reader();

	// 决策 K34=C（2026-11-04 用户裁定，接受现有预合并 HOT 读取失败）：物理分段落地前 AD 读端对 HOT/custom
	// 规则代码 fail-closed，预合并品种历史也不再提供（改前期望 late 返回品种历史 4930 一根）。
	WTSKlineSlice* late = _reader->readKlineSlice("EXHOT_BACK.rb.HOT", KP_Minute1, 1,
		20230103094000000ull);
	EXPECT_EQ(late, nullptr);
	if (late != nullptr)
		late->release();
	ASSERT_TRUE(_sink->bar_events().empty());

	WTSKlineSlice* early = _reader->readKlineSlice("EXHOT_BACK.rb.HOT", KP_Minute1, 1,
		20230103093300000ull);
	EXPECT_EQ(early, nullptr);
	if (early != nullptr)
		early->release();
	EXPECT_TRUE(_sink->bar_events().empty())
		<< "backward cutoff must not switch from product to raw history and replay it";
}

TEST_F(R4ReaderADTest, ZeroKlineEtimeUsesSinkClockAsInclusiveCutoff)
{
	add_stock("EXZERO_K", "rb2301");
	seed_bars("min1", "EXZERO_K", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)},
		{R4_M1_932, bar_at("rb2301", R4_M1_932, 3910.0)},
		{R4_M1_933, bar_at("rb2301", R4_M1_933, 3920.0)}
		});
	_sink->set_clock(R4_RD1, 932, 0);
	init_reader();

	WTSKlineSlice* slice = _reader->readKlineSlice("EXZERO_K.rb2301", KP_Minute1, 3, 0);
	ASSERT_NE(slice, nullptr);
	ASSERT_EQ(slice->get_block_size(0), 2u);
	EXPECT_EQ(slice->get_block_addr(0)[0].time, (uint64_t)R4_M1_931);
	EXPECT_EQ(slice->get_block_addr(0)[1].time, (uint64_t)R4_M1_932);
	slice->release();
}

TEST_F(R4ReaderADTest, DayKlineNightSessionCutoffUsesTradingDate)
{
	// 让 AD reader 经实际接口询问 night-session 时间对应的交易日。
	delete _sink;
	_sink = NULL;
	delete _bd;
	_bd = new R4NightSessionBaseDataMgr();
	_sink = new R4ReaderSink(_bd);
	WTSCommodityInfo* comm = _bd->add_commodity("EXNIGHT", "rb2301", ContractCategory::CC_Future);
	_bd->add_contract("EXNIGHT", "rb2301", comm);

	WTSBarStruct jan4 = bar_at("rb2301", 0, 4010.0);
	jan4.date = 20230104;
	WTSBarStruct jan5 = bar_at("rb2301", 0, 4020.0);
	jan5.date = 20230105;
	seed_bars("day", "EXNIGHT", "rb2301", {
		{20230103u, bar_at("rb2301", 0, 4000.0)},
		{20230104u, jan4},
		{20230105u, jan5}
		});
	init_reader();

	// Jan 3 23:00 belongs to Jan 4's trading date: include that daily bar, exclude Jan 5.
	WTSKlineSlice* slice = _reader->readKlineSlice("EXNIGHT.rb2301", KP_DAY, 3,
		20230103230000000ull);
	ASSERT_NE(slice, nullptr);
	ASSERT_EQ(slice->get_block_size(0), 2u);
	EXPECT_EQ(slice->get_block_addr(0)[0].date, 20230103u);
	EXPECT_EQ(slice->get_block_addr(0)[1].date, 20230104u);
	slice->release();
}

//////////////////////////////////////////////////////////////////////////
// 10. etime==0 时用 sink 时钟补齐（L87-94 / L375-382）
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, ZeroEtimeFallsBackToSinkClock)
{
	add_stock("EXC0", "rb2301");
	seed_ticks("EXC0", "rb2301", {
		{R4_RD1, tick_at("EXC0", "rb2301", 93000000, 3900.0)},
		{R4_RD1, tick_at("EXC0", "rb2301", 101530000, 3915.0)}
		});
	_sink->set_clock(R4_RD1, 1015, 30000);
	init_reader();

	// etime = 20230103*1e9 + 1015*1e5 + 30000 = 20230103101530000
	WTSTickSlice* slice = _reader->readTickSlice("EXC0.rb2301", 1, 0);
	ASSERT_TRUE(slice != NULL);
	ASSERT_EQ(slice->get_block_size(0), 1u);
	EXPECT_DOUBLE_EQ(slice->get_block_addr(0)[0].price, 3915.0);
	slice->release();

	// 时钟往前挪一格：增量区间 (10:15:30.000, 10:16:00.000] 内没有新 tick
	// → 一条都不追加，切片仍复用缓存里那笔 10:15:30
	_sink->set_clock(R4_RD1, 1016, 0);
	WTSTickSlice* s2 = _reader->readTickSlice("EXC0.rb2301", 1, 0);
	ASSERT_TRUE(s2 != NULL);
	EXPECT_EQ(s2->get_block_counts(), (std::size_t)1);
	ASSERT_EQ(s2->get_block_size(0), 1u);
	EXPECT_DOUBLE_EQ(s2->get_block_addr(0)[0].price, 3915.0);
	EXPECT_EQ(s2->get_block_addr(0)[0].action_time, 101530000u);
	s2->release();
	// 0 命中不产生 append 日志；DB 打开日志可能被首次 full-reload 日志覆盖，
	// 因此只确认该日志曾产生，不绑定 last_log 的最终内容。
	EXPECT_EQ(_sink->last_log(LL_DEBUG).find("append to cache"), std::string::npos)
		<< _sink->last_log(LL_DEBUG);
	EXPECT_EQ(_sink->count_containing("Tick db of EXC0.rb2301 opened"), 1u);
}

//////////////////////////////////////////////////////////////////////////
// 11. onMinuteEnd 的时间闸门（K 线缓存为空时不碰任何库，最安全的一段）
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, OnMinuteEndGatesRepeatedAndOlderTimes)
{
	init_reader();

	// _bars_cache 为空 → 只触发 on_all_bar_updated
	_reader->onMinuteEnd(R4_RD1, 1000, 0);
	EXPECT_EQ(_sink->all_updated(), 1u);
	EXPECT_EQ(_sink->last_update_time(), 1000u);

	// nowTime 没前进 → L500-501 直接返回
	_reader->onMinuteEnd(R4_RD1, 1000, 0);
	EXPECT_EQ(_sink->all_updated(), 1u);
	_reader->onMinuteEnd(R4_RD1, 959, 0);
	EXPECT_EQ(_sink->all_updated(), 1u);

	// 前进一分钟 → 再次通知
	_reader->onMinuteEnd(R4_RD1, 1001, 0);
	EXPECT_EQ(_sink->all_updated(), 2u);
	EXPECT_EQ(_sink->last_update_time(), 1001u);
	EXPECT_EQ(_sink->bar_calls(), 0u);
}

//////////////////////////////////////////////////////////////////////////
// 12. 周期非法：get_k_db 走 L573-574 返回空指针 → 回源失败 → 切片 NULL
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, UnknownPeriodHasNoDbAndReturnsNull)
{
	add_stock("EXP8", "rb2301");
	init_reader();
	// KP_Hour 既不是 min1/min5/day → get_k_db 返回空指针 → 回源失败 → 空指针切片
	WTSKlineSlice* slice = _reader->readKlineSlice("EXP8.rb2301", KP_Hour, 3, 20230103100000000ull);
	EXPECT_TRUE(slice == NULL);
}

//////////////////////////////////////////////////////////////////////////
// 13. 登记：读取器在"第三次取同交易所同周期库"之后继续增量更新会解引用空句柄
//     触发条件：readKlineSlice 之后，用更大的 etime 再读一次同周期的 K 线
//     → update_cache_from_lmdb（L273）里 WtLMDBQuery query(*db) 解引用空 shared_ptr → 崩溃。
//     因此整条 DISABLED_，见 bugs/R4_adstorage.md R4-BUG-02。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, DISABLED_AdvancingEtimeAfterTwoHandlesCrashesOnNullDb)
{
	add_stock("EXX1", "rb2301");
	seed_bars("min1", "EXX1", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)}
		});
	init_reader();
	_reader->readKlineSlice("EXX1.rb2301", KP_Minute1, 1, 20230103100000000ull);
	// 第二次读到更新的 etime → 第 3 次 get_k_db 返回空 → *db 解引用
	_reader->readKlineSlice("EXX1.rb2301", KP_Minute1, 1, 20230103100500000ull);
	FAIL() << "启用即空指针解引用";
}

//////////////////////////////////////////////////////////////////////////
// onMinuteEnd guards both empty bar lists and optional RT-cache misses.
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, OnMinuteEndSkipsBarlessCacheEntry)
{
	add_stock("EXX2", "rb2301");
	init_reader();
	// 空 min1 库建立了缓存项，但没有可用 bar。
	EXPECT_EQ(_reader->readKlineSlice("EXX2.rb2301", KP_Minute1, 2, 20230103100000000ull), nullptr);
	_reader->onMinuteEnd(R4_RD1, 1000, 0);
	EXPECT_EQ(_sink->bar_calls(), 0u);
	EXPECT_EQ(_sink->all_updated(), 1u);
}

TEST_F(R4ReaderADTest, OnMinuteEndSkipsMissingMinuteRtCacheBar)
{
	add_stock("EXX3", "rb2301");
	seed_bars("min1", "EXX3", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)}
		});
	init_reader();

	WTSKlineSlice* slice = _reader->readKlineSlice("EXX3.rb2301", KP_Minute1, 1,
		20230103100000000ull);
	ASSERT_NE(slice, nullptr);
	slice->release();
	// No cache_m1.dmb means get_rt_cache_bar() returns NULL for this valid bar list.
	EXPECT_NO_THROW(_reader->onMinuteEnd(R4_RD1, 1000, 0));
	EXPECT_EQ(_sink->bar_calls(), 0u);
	EXPECT_EQ(_sink->all_updated(), 1u);
}

TEST_F(R4ReaderADTest, OnMinuteEndSkipsMissingDailyRtCacheBar)
{
	add_stock("EXX4", "rb2301");
	WTSBarStruct previous = bar_at("rb2301", 0, 3900.0);
	previous.date = 20230102;
	seed_bars("day", "EXX4", "rb2301", {{20230102, previous}});
	init_reader();

	WTSKlineSlice* slice = _reader->readKlineSlice("EXX4.rb2301", KP_DAY, 1,
		20230103100000000ull);
	ASSERT_NE(slice, nullptr);
	slice->release();
	// No cache_d1.dmb means the daily wrapper has no matching bar.
	EXPECT_NO_THROW(_reader->onMinuteEnd(R4_RD1, 1000, R4_RD1));
	EXPECT_EQ(_sink->bar_calls(), 0u);
	EXPECT_EQ(_sink->all_updated(), 1u);
}

TEST_F(R4ReaderADTest, OnMinuteEndEmitsMinuteRtCacheBarOnce)
{
	add_stock("EXX5", "rb2301");
	seed_bars("min1", "EXX5", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)}
		});
	std::vector<BarCacheItem> minute_items{
		make_bar_item("EXX5", "rb2301", R4_RD1, R4_M1_940, 3910.0)};
	write_block_file((_root / "cache_m1.dmb").string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1,
		bar_payload(minute_items));
	init_reader();

	WTSKlineSlice* minute = _reader->readKlineSlice("EXX5.rb2301", KP_Minute1, 1,
		20230103093100000ull);
	ASSERT_NE(minute, nullptr);
	minute->release();

	_reader->onMinuteEnd(R4_RD1, 1000, 0);
	EXPECT_EQ(_sink->bar_calls(), 1u);
	EXPECT_EQ(_sink->last_bar_code(), "EXX5.rb2301");
	EXPECT_EQ(_sink->last_period(), KP_Minute1);
	EXPECT_EQ(_sink->all_updated(), 1u);
}

TEST_F(R4ReaderADTest, OnMinuteEndEmitsDailyRtCacheBarOnce)
{
	add_stock("EXX6", "rb2301");
	WTSBarStruct previous = bar_at("rb2301", 0, 4000.0);
	previous.date = 20230102;
	seed_bars("day", "EXX6", "rb2301", {{20230102, previous}});
	std::vector<BarCacheItem> daily_items{
		make_bar_item("EXX6", "rb2301", R4_RD1, 0, 4010.0)};
	write_block_file((_root / "cache_d1.dmb").string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1,
		bar_payload(daily_items));
	init_reader();

	WTSKlineSlice* daily = _reader->readKlineSlice("EXX6.rb2301", KP_DAY, 1,
		20230102100000000ull);
	ASSERT_NE(daily, nullptr);
	daily->release();

	_reader->onMinuteEnd(R4_RD1, 1000, R4_RD1);
	EXPECT_EQ(_sink->bar_calls(), 1u);
	EXPECT_EQ(_sink->last_bar_code(), "EXX6.rb2301");
	EXPECT_EQ(_sink->last_period(), KP_DAY);
	EXPECT_EQ(_sink->all_updated(), 1u);
}

TEST_F(R4ReaderADTest, P05ReadBarsBufferHandlesEmptySingleAndClosedRange)
{
	const WTSBarStruct first = bar_at("rb2301", 0, 100.0);
	const WTSBarStruct middle = bar_at("rb2301", 123, 200.0);
	const WTSBarStruct last = bar_at("rb2301", UINT32_MAX, 300.0);
	const WTSBarStruct other = bar_at("rb2302", 42, 400.0);
	seed_bars("min1", "EXX", "rb2301", {{0, first}, {123, middle}, {UINT32_MAX, last}});
	seed_bars("min1", "EXX", "rb2302", {{42, other}});
	init_reader();

	std::string output = "old value";
	ASSERT_TRUE(WtDataReaderADTestAccess::read_bars_to_buffer(*_reader, "EXX", "missing", KP_Minute1, output));
	EXPECT_TRUE(output.empty());
	ASSERT_TRUE(WtDataReaderADTestAccess::read_bars_to_buffer(*_reader, "EXX", "rb2302", KP_Minute1, output));
	EXPECT_EQ(output, std::string((const char*)&other, sizeof(other)));
	ASSERT_TRUE(WtDataReaderADTestAccess::read_bars_to_buffer(*_reader, "EXX", "rb2301", KP_Minute1, output));
	std::string expected;
	expected.append((const char*)&first, sizeof(first));
	expected.append((const char*)&middle, sizeof(middle));
	expected.append((const char*)&last, sizeof(last));
	EXPECT_EQ(output.size(), 3 * sizeof(WTSBarStruct));
	EXPECT_EQ(output, expected);
	EXPECT_EQ(WtDataReaderADTestAccess::read_bars_legacy(*_reader, "EXX", "rb2301", KP_Minute1), expected);
}

TEST_F(R4ReaderADTest, P05ReadBarsBufferRejectsMalformedValuesWithoutPublishingPartial)
{
	const WTSBarStruct bar = bar_at("rb2301", 1, 123.0);
	const std::string valid((const char*)&bar, sizeof(bar));
	seed_bar_values("min1", "SHRT", "rb2301", {{1, valid}, {2, valid.substr(0, valid.size() - 1)}});
	seed_bar_values("min1", "LONG", "rb2301", {{1, valid}, {2, valid + "x"}});
	seed_raw_bar_values("min1", "KEYX", {{bar_key_str("KEYX", "rb2301", 1), valid},
		{bar_key_str("KEYX", "rb2301", 2) + "x", valid}});
	init_reader();
	for (const char* exchg : {"SHRT", "LONG", "KEYX"})
	{
		std::string output = "unchanged";
		EXPECT_FALSE(WtDataReaderADTestAccess::read_bars_to_buffer(*_reader, exchg, "rb2301", KP_Minute1, output));
		EXPECT_EQ(output, "unchanged");
	}
}

TEST_F(R4ReaderADTest, P05ReadBarsBufferRejectsQueryErrorsWithoutPublishingPartial)
{
	const WTSBarStruct first = bar_at("rb2301", 1, 123.0);
	const WTSBarStruct second = bar_at("rb2301", 2, 124.0);
	seed_bars("min1", "EXX", "rb2301", {{1, first}, {2, second}});
	init_reader();
	std::string output = "unchanged";
	R4CursorFailureState failure;
	failure.fail_on_call = 2;  // First item is read; the following cursor operation fails.
	{
		ScopedR4CursorOverride scoped(&failure);
		EXPECT_FALSE(WtDataReaderADTestAccess::read_bars_to_buffer(*_reader, "EXX", "rb2301", KP_Minute1, output));
	}
	EXPECT_EQ(failure.calls, 2);
	EXPECT_EQ(output, "unchanged");
	ASSERT_TRUE(WtDataReaderADTestAccess::read_bars_to_buffer(*_reader, "EXX", "rb2301", KP_Minute1, output));
	EXPECT_EQ(output, std::string((const char*)&first, sizeof(first)) +
		std::string((const char*)&second, sizeof(second)));
}

TEST_F(R4ReaderADTest, P05ReadBarsBufferRejectsInvalidIdentityAndPeriod)
{
	init_reader();
	std::string output = "unchanged";
	const std::string longExchg(MAX_EXCHANGE_LENGTH, 'E');
	const std::string longCode(MAX_INSTRUMENT_LENGTH, 'C');
	EXPECT_FALSE(WtDataReaderADTestAccess::read_bars_to_buffer(*_reader, nullptr, "rb2301", KP_Minute1, output));
	EXPECT_FALSE(WtDataReaderADTestAccess::read_bars_to_buffer(*_reader, "EXX", nullptr, KP_Minute1, output));
	EXPECT_FALSE(WtDataReaderADTestAccess::read_bars_to_buffer(*_reader, longExchg.c_str(), "rb2301", KP_Minute1, output));
	EXPECT_FALSE(WtDataReaderADTestAccess::read_bars_to_buffer(*_reader, "EXX", longCode.c_str(), KP_Minute1, output));
	EXPECT_FALSE(WtDataReaderADTestAccess::read_bars_to_buffer(*_reader, "EXX", "rb2301", static_cast<WTSKlinePeriod>(99), output));
	EXPECT_EQ(output, "unchanged");
}

TEST_F(R4ReaderADTest, P05ReadBarsBufferDistinguishesEmptyFromDatabaseOpenFailure)
{
	seed_bars("min1", "EMPTY", "rb2301", {});
	std::filesystem::create_directories(_root / "min1");
	{
		std::ofstream blocker(_root / "min1" / "BLOCKED");
		ASSERT_TRUE(blocker.good());
		blocker << "not a directory";
	}
	init_reader();

	std::string output = "unchanged";
	ASSERT_TRUE(WtDataReaderADTestAccess::read_bars_to_buffer(*_reader, "EMPTY", "rb2301", KP_Minute1, output));
	EXPECT_TRUE(output.empty());
	output = "unchanged";
	EXPECT_FALSE(WtDataReaderADTestAccess::read_bars_to_buffer(*_reader, "BLOCKED", "rb2301", KP_Minute1, output));
	EXPECT_EQ(output, "unchanged");
	// The legacy private string wrapper is retained for compatibility, but its
	// empty return cannot distinguish a database error from a valid empty scan.
	EXPECT_TRUE(WtDataReaderADTestAccess::read_bars_legacy(*_reader, "EMPTY", "rb2301", KP_Minute1).empty());
	EXPECT_TRUE(WtDataReaderADTestAccess::read_bars_legacy(*_reader, "BLOCKED", "rb2301", KP_Minute1).empty());
}

// Construct packed disk records bytewise: creating a BarCacheItem on the
// stack would itself bind an unaligned WTSBarStruct before exercising reader.
static std::string r4_raw_bar_payload(const char* exchange, const char* code,
	const WTSBarStruct& bar)
{
	std::string payload(sizeof(BarCacheItem), '\0');
	const size_t exchangeLength = std::strlen(exchange);
	const size_t codeLength = std::strlen(code);
	if (exchangeLength >= 16 || codeLength >= 32)
		return {};
	std::memcpy(&payload[0], exchange, exchangeLength);
	std::memcpy(&payload[16], code, codeLength);
	std::memcpy(&payload[48], &bar, sizeof(bar));
	return payload;
}

TEST_F(R4ReaderADTest, I03PackedRealtimeBarReadsAlignedSnapshot)
{
	add_stock("EXALN", "rb2301");
	seed_bars("min1", "EXALN", "rb2301", {});
	WTSBarStruct bar = bar_at("rb2301", R4_M1_940, 3940.0);
	write_block_file((_root / "cache_m1.dmb").string(), BLK_FLAG, BT_RT_Cache,
		1, 1, 1, r4_raw_bar_payload("EXALN", "rb2301", bar));
	init_reader();
	WTSKlineSlice* slice = _reader->readKlineSlice("EXALN.rb2301", KP_Minute1, 1,
		20230103100000000ull);
	ASSERT_NE(slice, nullptr);
	ASSERT_EQ(slice->size(), 1);
	EXPECT_DOUBLE_EQ(slice->at(0)->open, 3940.0);
	slice->release();
}

TEST_F(R4ReaderADTest, I03PackedRealtimeSnapshotsCoverAllPeriodsAndRecords)
{
	add_stock("EXALL", "rb2301");
	seed_bars("min1", "EXALL", "rb2301", {});
	seed_bars("min5", "EXALL", "rb2301", {});
	seed_bars("day", "EXALL", "rb2301", {});
	struct PeriodCase { const char* filename; WTSKlinePeriod period; uint64_t barTime; };
	const PeriodCase cases[] = {{"cache_m1.dmb", KP_Minute1, R4_M1_940},
		{"cache_m5.dmb", KP_Minute5, R4_M1_940}, {"cache_d1.dmb", KP_DAY, 0}};
	for (const auto& item : cases)
	{
		WTSBarStruct bar = bar_at("rb2301", item.barTime, 3940.0);
		bar.date = R4_RD1;
		const auto unwanted = r4_raw_bar_payload("EXALL", "rb9999", bar);
		const auto wanted = r4_raw_bar_payload("EXALL", "rb2301", bar);
		write_block_file((_root / item.filename).string(), BLK_FLAG, BT_RT_Cache,
			1, 2, 2, unwanted + wanted);
	}
	init_reader();
	for (const auto& item : cases)
	{
		WTSKlineSlice* slice = _reader->readKlineSlice("EXALL.rb2301", item.period, 1,
			20230103100000000ull);
		ASSERT_NE(slice, nullptr) << item.filename;
		ASSERT_EQ(slice->size(), 1) << item.filename;
		EXPECT_DOUBLE_EQ(slice->at(0)->open, 3940.0) << item.filename;
		slice->release();
	}
}

TEST_F(R4ReaderADTest, I03TruncatedRealtimeCacheIsNotPublishedAndCanRetry)
{
	add_stock("EXSHORT", "rb2301");
	seed_bars("min1", "EXSHORT", "rb2301", {{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)}});
	WTSBarStruct bar = bar_at("rb2301", R4_M1_940, 3940.0);
	const auto record = r4_raw_bar_payload("EXSHORT", "rb2301", bar);
	const auto path = (_root / "cache_m1.dmb").string();
	write_block_file(path, BLK_FLAG, BT_RT_Cache, 1, 2, 2, record);
	init_reader();
	WTSKlineSlice* before = _reader->readKlineSlice("EXSHORT.rb2301", KP_Minute1, 2,
		20230103100000000ull);
	EXPECT_EQ(before, nullptr);
	EXPECT_GE(_sink->count_containing("Invalid or truncated real-time bar cache"), 1u);
	write_block_file(path, BLK_FLAG, BT_RT_Cache, 1, 2, 2, record + record);
	WTSKlineSlice* after = _reader->readKlineSlice("EXSHORT.rb2301", KP_Minute1, 2,
		20230103100000000ull);
	ASSERT_NE(after, nullptr);
	ASSERT_EQ(after->size(), 2);
	EXPECT_DOUBLE_EQ(after->at(1)->open, 3940.0);
	after->release();
}

TEST_F(R4ReaderADTest, I03InvalidRealtimeIdentityDoesNotPublishPartialIndex)
{
	add_stock("EXBAD", "rb2301");
	seed_bars("min1", "EXBAD", "rb2301", {{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)}});
	WTSBarStruct bar = bar_at("rb2301", R4_M1_940, 3940.0);
	const auto good = r4_raw_bar_payload("EXBAD", "rb2301", bar);
	auto bad = good;
	std::memset(&bad[0], 'X', 16);
	const auto path = (_root / "cache_m1.dmb").string();
	write_block_file(path, BLK_FLAG, BT_RT_Cache, 1, 2, 2, good + bad);
	init_reader();
	WTSKlineSlice* before = _reader->readKlineSlice("EXBAD.rb2301", KP_Minute1, 2,
		20230103100000000ull);
	EXPECT_EQ(before, nullptr);
	write_block_file(path, BLK_FLAG, BT_RT_Cache, 1, 2, 2, good + good);
	WTSKlineSlice* after = _reader->readKlineSlice("EXBAD.rb2301", KP_Minute1, 2,
		20230103100000000ull);
	ASSERT_NE(after, nullptr);
	ASSERT_EQ(after->size(), 2);
	EXPECT_DOUBLE_EQ(after->at(1)->open, 3940.0);
	after->release();
}

TEST_F(R4ReaderADTest, I03MinuteEndKeepsGateAndRetriesAtNextMinuteAfterInvalidCache)
{
	add_stock("EXRETRY", "rb2301");
	seed_bars("min1", "EXRETRY", "rb2301", {{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)}});
	init_reader();
	WTSKlineSlice* initial = _reader->readKlineSlice("EXRETRY.rb2301", KP_Minute1, 1,
		20230103093100000ull);
	ASSERT_NE(initial, nullptr);
	initial->release();
	const auto path = (_root / "cache_m1.dmb").string();
	const auto record = r4_raw_bar_payload("EXRETRY", "rb2301", bar_at("rb2301", R4_M1_940, 3940.0));
	write_block_file(path, BLK_FLAG, BT_RT_Cache, 1, 2, 2, record);
	_reader->onMinuteEnd(R4_RD1, 1000, 0);
	EXPECT_EQ(_sink->bar_calls(), 0u);
	EXPECT_EQ(_sink->all_updated(), 0u);
	write_block_file(path, BLK_FLAG, BT_RT_Cache, 1, 2, 2, record + record);
	_reader->onMinuteEnd(R4_RD1, 1000, 0);
	EXPECT_EQ(_sink->bar_calls(), 0u);
	EXPECT_EQ(_sink->all_updated(), 0u);
	_reader->onMinuteEnd(R4_RD1, 1001, 0);
	EXPECT_EQ(_sink->bar_calls(), 1u);
	EXPECT_EQ(_sink->all_updated(), 1u);
}

TEST_F(R4ReaderADTest, I03MinuteEndFailureCanFollowAnEmittedBarButDoesNotRepeatMinute)
{
	add_stock("EXPART", "rb2301");
	add_stock("EXPART", "rb2302");
	seed_bars("min1", "EXPART", "rb2301", {
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)},
		{R4_M1_940, bar_at("rb2301", R4_M1_940, 3940.0)}});
	seed_bars("min1", "EXPART", "rb2302", {{R4_M1_931, bar_at("rb2302", R4_M1_931, 4000.0)}});
	init_reader();
	for (const char* code : {"EXPART.rb2301", "EXPART.rb2302"})
	{
		WTSKlineSlice* slice = _reader->readKlineSlice(code, KP_Minute1, 1, 20230103093100000ull);
		ASSERT_NE(slice, nullptr);
		slice->release();
	}
	const auto record = r4_raw_bar_payload("EXPART", "rb2302", bar_at("rb2302", R4_M1_940, 4040.0));
	const auto path = (_root / "cache_m1.dmb").string();
	write_block_file(path, BLK_FLAG, BT_RT_Cache, 1, 2, 2, record);
	_reader->onMinuteEnd(R4_RD1, 1000, 0);
	EXPECT_EQ(_sink->bar_calls(), 1u); // LMDB callback cannot be rolled back.
	EXPECT_EQ(_sink->all_updated(), 0u);
	write_block_file(path, BLK_FLAG, BT_RT_Cache, 1, 2, 2, record + record);
	_reader->onMinuteEnd(R4_RD1, 1000, 0);
	EXPECT_EQ(_sink->bar_calls(), 1u);
	EXPECT_EQ(_sink->all_updated(), 0u);
}

TEST_F(R4ReaderADTest, I03ChangedRealtimeIdentityDoesNotReturnAnotherContract)
{
	add_stock("EXCHG", "rb2301");
	WTSBarStruct bar = bar_at("rb2301", R4_M1_940, 3940.0);
	const auto path = (_root / "cache_m1.dmb").string();
	write_block_file(path, BLK_FLAG, BT_RT_Cache, 1, 1, 1,
		r4_raw_bar_payload("EXCHG", "rb2301", bar));
	init_reader();
	WTSBarStruct snapshot;
	ASSERT_TRUE(WtDataReaderADTestAccess::read_rt_snapshot(*_reader, "EXCHG", "rb2301", KP_Minute1, snapshot));
	EXPECT_DOUBLE_EQ(snapshot.open, 3940.0);
	{
		// Replace the identity without truncating the already-mapped file.
		std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
		ASSERT_TRUE(file.good());
		file.seekp(sizeof(RTBlockHeader) + 16);
		const char other[32] = "rb9999";
		file.write(other, sizeof(other));
		ASSERT_TRUE(file.good());
	}
	EXPECT_FALSE(WtDataReaderADTestAccess::read_rt_snapshot(*_reader, "EXCHG", "rb2301", KP_Minute1, snapshot));
	EXPECT_TRUE(WtDataReaderADTestAccess::read_rt_snapshot(*_reader, "EXCHG", "rb9999", KP_Minute1, snapshot));
}

TEST_F(R4ReaderADTest, I03PackedRealtimeBarCallbackSurvivesReentrantBufferGrowth)
{
	add_stock("EXCB", "rb2301");
	seed_bars("min1", "EXCB", "rb2301", {{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)}});
	WTSBarStruct bar = bar_at("rb2301", R4_M1_940, 3940.0);
	write_block_file((_root / "cache_m1.dmb").string(), BLK_FLAG, BT_RT_Cache,
		1, 1, 1, r4_raw_bar_payload("EXCB", "rb2301", bar));
	init_reader();
	WTSKlineSlice* first = _reader->readKlineSlice("EXCB.rb2301", KP_Minute1, 1,
		20230103093100000ull);
	ASSERT_NE(first, nullptr);
	first->release();
	bool invoked = false;
	_sink->set_bar_hook([&](WTSBarStruct* callbackBar) {
		if (invoked) return;
		invoked = true;
		ASSERT_NE(callbackBar, nullptr);
		EXPECT_DOUBLE_EQ(callbackBar->open, 3940.0);
		WTSKlineSlice* grown = _reader->readKlineSlice("EXCB.rb2301", KP_Minute1, 64,
			20230103100000000ull);
		ASSERT_NE(grown, nullptr);
		grown->release();
		EXPECT_DOUBLE_EQ(callbackBar->open, 3940.0);
	});
	_reader->onMinuteEnd(R4_RD1, 1000, 0);
	EXPECT_TRUE(invoked);
	_sink->set_bar_hook({});
}

}  // namespace
