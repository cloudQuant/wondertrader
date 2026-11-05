// 迭代03 s5 storage-read / WtDataStorageAD：WtDataReaderAD 与 WtBtDtReaderAD 的读端边界。
//
//  - BUG-L04-004：LMDB 键构造器有界复制、16/32 拒绝与 15/31 合法对照；读端入口在建目录/
//    开库/建缓存之前拒绝不可表示身份，并给出 ERROR 诊断（不再静默 NULL）。
//  - BUG-R100-003 / BUG-R32-003：2033 年起分钟 cutoff 超出 32 位 V1 分钟键，readKlineSlice
//    与 onMinuteEnd 明确拒绝而不是回绕到更早区间；日线（按日期键）不受影响。
//  - BUG-R122-002：增量 tick / 前向 K 线更新的暂存有界；损坏数据整段拒绝、不部分提交/回调；
//    分段应用期间并发改写只在已应用位置停止且重试不重复回调；只读目录缺库明确失败、可重试。
//
// 依赖新测试钩子（set_test_staging_hooks）或新诊断的用例以 WT_S5_BASELINE 排除，
// 便于用改前产品源码做红控构建。
#include "r4_ad_fixture.h"
#include "Includes/IDataReader.h"
#include "Includes/IBtDtReader.h"
#include "WtDataStorageAD/WtDataReaderAD.h"
#include "WtDataStorageAD/WtBtDtReaderAD.h"

#include <functional>
#include <stdexcept>
#include <sys/stat.h>

namespace wtp {

#ifndef WT_S5_BASELINE
struct WtDataReaderADBoundTestAccess
{
	static void set_staging_hooks(std::size_t chunk, void (*observer)(void*, std::size_t), void* context)
	{
		WtDataReaderAD::set_test_staging_hooks(chunk, observer, context);
	}
};
#endif

} // namespace wtp

namespace s5rd {

class S5ReaderSink : public IDataReaderSink
{
public:
	explicit S5ReaderSink(IBaseDataMgr* bd) : _bd(bd) {}
	virtual ~S5ReaderSink() {}

	void on_bar(const char*, WTSKlinePeriod, WTSBarStruct* newBar) override
	{
		_bar_times.push_back(newBar == NULL ? 0 : newBar->time);
		if (_bar_hook)
			_bar_hook();
	}
	void on_all_bar_updated(uint32_t) override { _all_updated++; }
	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return NULL; }
	uint32_t get_date() override { return 20230103; }
	uint32_t get_min_time() override { return 1000; }
	uint32_t get_secs() override { return 0; }
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		_logs.push_back(std::make_pair(ll, std::string(message == NULL ? "" : message)));
	}

	uint32_t count(WTSLogLevel ll, const char* sub = "") const
	{
		uint32_t ret = 0;
		for (const auto& item : _logs)
			if (item.first == ll && item.second.find(sub) != std::string::npos)
				++ret;
		return ret;
	}
	const std::vector<std::pair<WTSLogLevel, std::string>>& logs() const { return _logs; }

	std::vector<uint64_t> _bar_times;
	uint32_t _all_updated = 0;
	std::function<void()> _bar_hook;

private:
	IBaseDataMgr* _bd;
	std::vector<std::pair<WTSLogLevel, std::string>> _logs;
};

class S5BtSink : public IBtDtReaderSink
{
public:
	virtual ~S5BtSink() {}
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		_logs.push_back(std::make_pair(ll, std::string(message == NULL ? "" : message)));
	}
	uint32_t count(WTSLogLevel ll, const char* sub = "") const
	{
		uint32_t ret = 0;
		for (const auto& item : _logs)
			if (item.first == ll && item.second.find(sub) != std::string::npos)
				++ret;
		return ret;
	}

private:
	std::vector<std::pair<WTSLogLevel, std::string>> _logs;
};

static const uint32_t S5_DATE = 20230103;

static std::string bar_key(const char* exchg, const char* code, uint32_t bt)
{
	LMDBBarKey key(exchg, code, bt);
	return std::string((const char*)&key, sizeof(key));
}

static std::string tick_key(const char* exchg, const char* code, uint32_t date, uint32_t time)
{
	LMDBHftKey key(exchg, code, date, time);
	return std::string((const char*)&key, sizeof(key));
}

static WTSBarStruct min_bar(uint64_t bt, double px)
{
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	bar.date = S5_DATE;
	bar.time = bt;
	bar.open = px;
	bar.high = px;
	bar.low = px;
	bar.close = px;
	return bar;
}

// Minute bar time of minute index i on 2023-01-03 (09:00 + i, no session gaps needed).
static uint32_t m1_time(uint32_t i)
{
	return (S5_DATE - 19900000u) * 10000u + (9 + i / 60) * 100 + i % 60;
}

static uint64_t m1_etime(uint32_t i)
{
	return (uint64_t)S5_DATE * 1000000000ull + (uint64_t)((9 + i / 60) * 100 + i % 60) * 100000ull;
}

class S5ReaderADTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		_root = std::filesystem::temp_directory_path()
			/ ("s5_rad_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
		std::filesystem::create_directories(_root);
		_bd = new R4BaseDataMgr();
		_sink = new S5ReaderSink(_bd);
	}

	void TearDown() override
	{
#ifndef WT_S5_BASELINE
		WtDataReaderADBoundTestAccess::set_staging_hooks(0, NULL, NULL);
#endif
		_reader.reset();
		delete _sink;
		delete _bd;
		std::error_code ec;
		std::filesystem::permissions(_root, std::filesystem::perms::owner_all,
			std::filesystem::perm_options::add, ec);
		std::filesystem::remove_all(_root, ec);
	}

	void init_reader()
	{
		_reader.reset(new WtDataReaderAD());
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", _root.string().c_str());
		_reader->init(cfg, _sink, NULL);
		cfg->release();
	}

	void add_stock(const char* exchg, const char* code)
	{
		WTSCommodityInfo* comm = _bd->add_commodity(exchg, code, ContractCategory::CC_Stock);
		_bd->add_contract(exchg, code, comm);
	}

	void put_rows(const std::filesystem::path& dir, const std::vector<std::pair<std::string, std::string>>& rows)
	{
		std::filesystem::create_directories(dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(dir.string().c_str(), 64 * 1024 * 1024));
		WtLMDBQuery query(db);
		for (const auto& row : rows)
			ASSERT_TRUE(query.put(row.first, row.second));
		query.commit();
	}

	void seed_m1(const char* exchg, const char* code, uint32_t first, uint32_t count, double base = 100.0)
	{
		std::vector<std::pair<std::string, std::string>> rows;
		for (uint32_t i = first; i < first + count; ++i)
		{
			const WTSBarStruct bar = min_bar(m1_time(i), base + i);
			rows.push_back({ bar_key(exchg, code, m1_time(i)), std::string((const char*)&bar, sizeof(bar)) });
		}
		put_rows(_root / "min1" / exchg, rows);
	}

	std::filesystem::path _root;
	R4BaseDataMgr* _bd = NULL;
	S5ReaderSink* _sink = NULL;
	std::unique_ptr<WtDataReaderAD> _reader;
};

struct StagingProbe
{
	std::size_t peak = 0;
	std::size_t calls = 0;
	std::function<void()> after;
	static void observe(void* context, std::size_t staged)
	{
		StagingProbe* self = static_cast<StagingProbe*>(context);
		self->calls++;
		if (staged > self->peak)
			self->peak = staged;
		if (self->after)
			self->after();
	}
};

} // namespace s5rd

using namespace s5rd;

//////////////////////////////////////////////////////////////////////////
// BUG-L04-004：键构造器（保留磁盘布局，有界复制，无法表示时抛 invalid_argument）
//////////////////////////////////////////////////////////////////////////
TEST(S5LmdbKeyBounds, LegalMaximumIdentityKeepsLayout)
{
	const std::string ex(MAX_EXCHANGE_LENGTH - 1, 'E');
	const std::string code(MAX_INSTRUMENT_LENGTH - 1, 'C');
	LMDBBarKey bar(ex.c_str(), code.c_str(), 0x01020304u);
	LMDBHftKey hft(ex.c_str(), code.c_str(), 0x01020304u, 0x11223344u);
	EXPECT_EQ(sizeof(LMDBBarKey), (size_t)MAX_EXCHANGE_LENGTH + MAX_INSTRUMENT_LENGTH + 4);
	EXPECT_EQ(sizeof(LMDBHftKey), (size_t)MAX_EXCHANGE_LENGTH + MAX_INSTRUMENT_LENGTH + 8);
	EXPECT_EQ(std::string(bar._exchg), ex);
	EXPECT_EQ(std::string(bar._code), code);
	const unsigned char* raw = (const unsigned char*)&hft;
	const unsigned char tail[] = { 1, 2, 3, 4, 0x11, 0x22, 0x33, 0x44 };
	EXPECT_EQ(0, memcmp(raw + MAX_EXCHANGE_LENGTH + MAX_INSTRUMENT_LENGTH, tail, sizeof(tail)));
	EXPECT_TRUE(lmdbKeyIdentityValid(ex.c_str(), code.c_str()));
}

TEST(S5LmdbKeyBounds, OverlongNullAndUnterminatedIdentityAreRejected)
{
	const std::string ex(MAX_EXCHANGE_LENGTH - 1, 'E');
	const std::string code(MAX_INSTRUMENT_LENGTH - 1, 'C');
	const std::string longEx(MAX_EXCHANGE_LENGTH, 'E');
	const std::string longCode(MAX_INSTRUMENT_LENGTH, 'C');
	EXPECT_THROW(LMDBBarKey(longEx.c_str(), code.c_str(), 1), std::invalid_argument);
	EXPECT_THROW(LMDBHftKey(longEx.c_str(), code.c_str(), 1, 1), std::invalid_argument);
	EXPECT_THROW(LMDBBarKey(ex.c_str(), longCode.c_str(), 1), std::invalid_argument);
	EXPECT_THROW(LMDBHftKey(ex.c_str(), longCode.c_str(), 1, 1), std::invalid_argument);
	EXPECT_THROW(LMDBBarKey(NULL, code.c_str(), 1), std::invalid_argument);
	EXPECT_THROW(LMDBHftKey(ex.c_str(), NULL, 1, 1), std::invalid_argument);

	// Fixed-size buffers without a terminator: rejected after inspecting at most
	// the field capacity (ASan would report any read past these arrays).
	std::unique_ptr<char[]> exNoNul(new char[MAX_EXCHANGE_LENGTH]);
	std::unique_ptr<char[]> codeNoNul(new char[MAX_INSTRUMENT_LENGTH]);
	memset(exNoNul.get(), 'E', MAX_EXCHANGE_LENGTH);
	memset(codeNoNul.get(), 'C', MAX_INSTRUMENT_LENGTH);
	EXPECT_THROW(LMDBBarKey(exNoNul.get(), code.c_str(), 1), std::invalid_argument);
	EXPECT_THROW(LMDBHftKey(ex.c_str(), codeNoNul.get(), 1, 1), std::invalid_argument);
	EXPECT_FALSE(lmdbKeyIdentityValid(longEx.c_str(), code.c_str()));
	EXPECT_FALSE(lmdbKeyIdentityValid(ex.c_str(), longCode.c_str()));
}

TEST(S5LmdbKeyBounds, EmptyFieldsFitTheKeyButAreNotAStorageIdentity)
{
	// The historical strcpy constructor accepted empty fields (l04 lane relies on
	// it); entries still refuse them before touching storage.
	EXPECT_NO_THROW(LMDBBarKey("", "", 1u));
	EXPECT_NO_THROW(LMDBHftKey("", "", 1u, 1u));
	EXPECT_FALSE(lmdbKeyIdentityValid("", "rb2301"));
	EXPECT_FALSE(lmdbKeyIdentityValid("SHFE", ""));
}

//////////////////////////////////////////////////////////////////////////
// BUG-L04-004：读端入口在副作用前拒绝并可观察
//////////////////////////////////////////////////////////////////////////
TEST_F(S5ReaderADTest, OverlongExchangeIsRejectedBeforeAnyStorageSideEffect)
{
	init_reader();
	const std::string longEx(MAX_EXCHANGE_LENGTH, 'X');
	const std::string stdCode = longEx + ".rb2301";
	EXPECT_EQ(nullptr, _reader->readTickSlice(stdCode.c_str(), 3, 20230103093000000ull));
	EXPECT_EQ(nullptr, _reader->readKlineSlice(stdCode.c_str(), KP_Minute1, 3, 20230103093000000ull));
	EXPECT_EQ(nullptr, _reader->readKlineSlice(stdCode.c_str(), KP_DAY, 3, 20230103093000000ull));
	EXPECT_FALSE(std::filesystem::exists(_root / "ticks"));
	EXPECT_FALSE(std::filesystem::exists(_root / "min1"));
	EXPECT_FALSE(std::filesystem::exists(_root / "day"));
#ifndef WT_S5_BASELINE
	EXPECT_EQ(3u, _sink->count(LL_ERROR, "rejected storage identity"));
#endif
}

TEST_F(S5ReaderADTest, BtReaderRejectsOverlongCodeBeforeOpeningDatabases)
{
	S5BtSink btSink;
	WtBtDtReaderAD bt;
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", _root.string().c_str());
	bt.init(cfg, &btSink);
	cfg->release();
	const std::string longCode(MAX_INSTRUMENT_LENGTH, 'C');
	std::string buffer = "unchanged";
	EXPECT_FALSE(bt.read_raw_bars("SHFE", longCode.c_str(), KP_Minute1, buffer));
	EXPECT_FALSE(bt.read_raw_ticks("SHFE", longCode.c_str(), S5_DATE, buffer));
	EXPECT_EQ(buffer, "unchanged");
	EXPECT_FALSE(std::filesystem::exists(_root / "min1"));
	EXPECT_FALSE(std::filesystem::exists(_root / "ticks"));
#ifndef WT_S5_BASELINE
	EXPECT_EQ(2u, btSink.count(LL_ERROR, "rejected storage identity"));
#endif
}

TEST_F(S5ReaderADTest, LongConfiguredPathLogIsBoundedToTheMessageBuffer)
{
	// A configured root longer than the 512-byte per-thread log buffer.
	std::filesystem::path deep = _root;
	for (int i = 0; i < 6; ++i)
		deep /= std::string(100, (char)('a' + i));
	_reader.reset(new WtDataReaderAD());
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", deep.string().c_str());
	_reader->init(cfg, _sink, NULL);
	cfg->release();
	ASSERT_FALSE(_sink->logs().empty());
	const std::string& message = _sink->logs().back().second;
	EXPECT_LT(message.size(), 512u) << "formatter wrote past the fixed log buffer";
}

//////////////////////////////////////////////////////////////////////////
// BUG-R100-003 / BUG-R32-003：2033 年分钟时间不回绕
//////////////////////////////////////////////////////////////////////////
TEST_F(S5ReaderADTest, MinuteCutoffBeyondUint32KeyIsRefusedNotWrapped)
{
	add_stock("EX33", "rb2301");
	// A legal 2032-12-31 14:59 bar (fits uint32) and one early bar that a wrapped
	// 2033 cutoff (~6.06e6) would otherwise select.
	const uint32_t t2032 = (20321231u - 19900000u) * 10000u + 1459u;
	const WTSBarStruct early = min_bar(1000u, 1.0);
	const WTSBarStruct late = min_bar(t2032, 2.0);
	put_rows(_root / "min1" / "EX33", {
		{ bar_key("EX33", "rb2301", 1000u), std::string((const char*)&early, sizeof(early)) },
		{ bar_key("EX33", "rb2301", t2032), std::string((const char*)&late, sizeof(late)) } });
	init_reader();

	WTSKlineSlice* wrapped = _reader->readKlineSlice("EX33.rb2301", KP_Minute1, 5, 20330103093000000ull);
	EXPECT_EQ(nullptr, wrapped) << "2033 cutoff must not select bars through a wrapped 32-bit key";
	if (wrapped != nullptr)
	{
		EXPECT_NE(wrapped->at(-1)->time, (uint64_t)1000u) << "wrapped cutoff selected an unrelated early bar";
		wrapped->release();
	}
#ifndef WT_S5_BASELINE
	EXPECT_EQ(1u, _sink->count(LL_ERROR, "exceeds the 32-bit LMDB minute key range"));
#endif

	// 2032 cutoff still works and is exact.
	WTSKlineSlice* ok = _reader->readKlineSlice("EX33.rb2301", KP_Minute1, 5, 20321231145900000ull);
	ASSERT_NE(nullptr, ok);
	EXPECT_EQ(2, ok->size());
	EXPECT_EQ(ok->at(-1)->time, (uint64_t)t2032);
	ok->release();
}

TEST_F(S5ReaderADTest, MinuteEndBeyondUint32KeyDoesNotWrapOrSignalAllUpdated)
{
	add_stock("EX33M", "rb2301");
	seed_m1("EX33M", "rb2301", 0, 3);
	init_reader();
	WTSKlineSlice* first = _reader->readKlineSlice("EX33M.rb2301", KP_Minute1, 3, m1_etime(2));
	ASSERT_NE(nullptr, first);
	first->release();
	const std::size_t barsBefore = _sink->_bar_times.size();

	_reader->onMinuteEnd(20330103u, 931u, 20330103u);
	EXPECT_EQ(barsBefore, _sink->_bar_times.size());
	EXPECT_EQ(0u, _sink->_all_updated) << "a minute end the LMDB key cannot represent was reported as complete";
#ifndef WT_S5_BASELINE
	EXPECT_EQ(1u, _sink->count(LL_ERROR, "exceeds the 32-bit LMDB minute key range"));
#endif

	// A representable minute end afterwards still completes normally.
	_reader->onMinuteEnd(20330103u, 932u, 0);
	_reader.reset();
	init_reader();
	_reader->onMinuteEnd(S5_DATE, 1000u, 0);
	EXPECT_EQ(1u, _sink->_all_updated);
}

TEST_F(S5ReaderADTest, DayBarsIn2033UseDateKeysAndRemainReadable)
{
	add_stock("EX33D", "rb2301");
	WTSBarStruct day = min_bar(0, 7.0);
	day.date = 20330103u;
	put_rows(_root / "day" / "EX33D", {
		{ bar_key("EX33D", "rb2301", 20330103u), std::string((const char*)&day, sizeof(day)) } });
	init_reader();
	WTSKlineSlice* slice = _reader->readKlineSlice("EX33D.rb2301", KP_DAY, 3, 20330103150000000ull);
	ASSERT_NE(nullptr, slice);
	EXPECT_EQ(1, slice->size());
	EXPECT_EQ(20330103u, slice->at(0)->date);
	slice->release();
}

//////////////////////////////////////////////////////////////////////////
// BUG-R122-002：暂存有界、损坏不部分提交/回调、只读目录缺库
//////////////////////////////////////////////////////////////////////////
#ifndef WT_S5_BASELINE
TEST_F(S5ReaderADTest, IncrementalTickStagingIsBoundedByCacheCapacity)
{
	add_stock("EXTB", "rb2301");
	std::vector<std::pair<std::string, std::string>> rows;
	for (uint32_t i = 0; i < 3000; ++i)
	{
		const uint32_t at = 90000000u + (i / 60) * 100000u + (i % 60) * 1000u;	// HHMMSSmmm
		WTSTickStruct t = make_tick("EXTB", "rb2301", S5_DATE, S5_DATE, at, 1000.0 + i, 1, 1, 0, 0);
		rows.push_back({ tick_key("EXTB", "rb2301", S5_DATE, at), std::string((const char*)&t, sizeof(t)) });
	}
	put_rows(_root / "ticks" / "EXTB" / "rb2301", rows);
	init_reader();

	WTSTickSlice* first = _reader->readTickSlice("EXTB.rb2301", 4, 20230103090000000ull + 3000ull);
	ASSERT_NE(nullptr, first);
	first->release();

	StagingProbe probe;
	WtDataReaderADBoundTestAccess::set_staging_hooks(0, &StagingProbe::observe, &probe);
	WTSTickSlice* all = _reader->readTickSlice("EXTB.rb2301", 4, 20230103100000000ull);
	if (all == nullptr)
		for (const auto& item : _sink->logs())
			ADD_FAILURE() << "reader log: " << item.second;
	ASSERT_NE(nullptr, all);
	EXPECT_EQ(4u, all->size());
	EXPECT_DOUBLE_EQ(all->at(-1)->price, 1000.0 + 2999);
	EXPECT_DOUBLE_EQ(all->at(0)->price, 1000.0 + 2996);
	all->release();
	EXPECT_EQ(1u, probe.calls);
	EXPECT_LE(probe.peak, 4u) << "incremental tick staging grew with the update span";
}

TEST_F(S5ReaderADTest, ForwardBarUpdateIsChunkBoundedAndCallsBackEachBarOnce)
{
	add_stock("EXCB", "rb2301");
	seed_m1("EXCB", "rb2301", 0, 50);
	init_reader();
	WTSKlineSlice* first = _reader->readKlineSlice("EXCB.rb2301", KP_Minute1, 3, m1_etime(2));
	ASSERT_NE(nullptr, first);
	first->release();
	_sink->_bar_times.clear();

	StagingProbe probe;
	WtDataReaderADBoundTestAccess::set_staging_hooks(4, &StagingProbe::observe, &probe);
	WTSKlineSlice* forward = _reader->readKlineSlice("EXCB.rb2301", KP_Minute1, 3, m1_etime(49));
	ASSERT_NE(nullptr, forward);
	ASSERT_EQ(3, forward->size());
	EXPECT_EQ((uint64_t)m1_time(47), forward->at(0)->time);
	EXPECT_EQ((uint64_t)m1_time(49), forward->at(2)->time);
	forward->release();
	EXPECT_LE(probe.peak, 4u);
	ASSERT_EQ(47u, _sink->_bar_times.size());
	for (uint32_t i = 0; i < 47; ++i)
		EXPECT_EQ((uint64_t)m1_time(3 + i), _sink->_bar_times[i]) << "callback order/duplicate at " << i;
}

TEST_F(S5ReaderADTest, MalformedRecordInLongForwardRangeCommitsAndCallsBackNothing)
{
	add_stock("EXMB", "rb2301");
	seed_m1("EXMB", "rb2301", 0, 50);
	init_reader();
	WTSKlineSlice* first = _reader->readKlineSlice("EXMB.rb2301", KP_Minute1, 3, m1_etime(2));
	ASSERT_NE(nullptr, first);
	first->release();
	_sink->_bar_times.clear();
	// Corrupt the last bar of the range (short value): phase 1 must reject it
	// before any of the 46 earlier bars are applied.
	put_rows(_root / "min1" / "EXMB", { { bar_key("EXMB", "rb2301", m1_time(49)), std::string(10, 'x') } });

	WtDataReaderADBoundTestAccess::set_staging_hooks(4, NULL, NULL);
	EXPECT_EQ(nullptr, _reader->readKlineSlice("EXMB.rb2301", KP_Minute1, 3, m1_etime(49)));
	EXPECT_TRUE(_sink->_bar_times.empty()) << "bars were called back before the range was validated";
	EXPECT_GE(_sink->count(LL_ERROR, "Malformed bar key/value"), 1u);

	// Repair and retry: every new bar arrives exactly once.
	seed_m1("EXMB", "rb2301", 49, 1);
	WTSKlineSlice* retry = _reader->readKlineSlice("EXMB.rb2301", KP_Minute1, 3, m1_etime(49));
	ASSERT_NE(nullptr, retry);
	EXPECT_EQ((uint64_t)m1_time(49), retry->at(-1)->time);
	retry->release();
	EXPECT_EQ(47u, _sink->_bar_times.size());
}

TEST_F(S5ReaderADTest, ConcurrentRewriteDuringChunkedApplyStopsAtAppliedBarsWithoutDuplicates)
{
	add_stock("EXCW", "rb2301");
	seed_m1("EXCW", "rb2301", 0, 30);
	init_reader();
	WTSKlineSlice* first = _reader->readKlineSlice("EXCW.rb2301", KP_Minute1, 3, m1_etime(2));
	ASSERT_NE(nullptr, first);
	first->release();
	_sink->_bar_times.clear();

	// Phase 1 validates a clean snapshot; after the first phase-2 chunk is staged a
	// concurrent writer corrupts a later bar of the same range.
	StagingProbe probe;
	probe.after = [this, &probe]() {
		if (probe.calls == 2)
			put_rows(_root / "min1" / "EXCW", { { bar_key("EXCW", "rb2301", m1_time(20)), std::string(9, 'y') } });
	};
	WtDataReaderADBoundTestAccess::set_staging_hooks(4, &StagingProbe::observe, &probe);
	EXPECT_EQ(nullptr, _reader->readKlineSlice("EXCW.rb2301", KP_Minute1, 3, m1_etime(29)));
	const std::size_t applied = _sink->_bar_times.size();
	EXPECT_GT(applied, 0u);
	EXPECT_LT(applied, 27u);
	EXPECT_GE(_sink->count(LL_ERROR, "changed while being applied"), 1u);

	// Repair; the retry continues after the bars already delivered, no duplicates.
	probe.after = nullptr;
	seed_m1("EXCW", "rb2301", 20, 1);
	WTSKlineSlice* retry = _reader->readKlineSlice("EXCW.rb2301", KP_Minute1, 3, m1_etime(29));
	ASSERT_NE(nullptr, retry);
	retry->release();
	ASSERT_EQ(27u, _sink->_bar_times.size());
	for (uint32_t i = 0; i < 27; ++i)
		EXPECT_EQ((uint64_t)m1_time(3 + i), _sink->_bar_times[i]) << "duplicate or gap at " << i;
}
#endif

TEST_F(S5ReaderADTest, ReadOnlyRootWithMissingDatabaseFailsClosedAndRecovers)
{
	if (::geteuid() == 0)
		GTEST_SKIP() << "permission checks are bypassed for root";
	add_stock("EXRO", "rb2301");
	std::vector<BarCacheItem> items{ make_bar_item("EXRO", "rb2301", S5_DATE, m1_time(40), 3940.0) };
	write_block_file((_root / "cache_m1.dmb").string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1, bar_payload(items));
	init_reader();
	std::filesystem::permissions(_root, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
		std::filesystem::perm_options::replace);

	WTSKlineSlice* denied = nullptr;
	EXPECT_NO_THROW(denied = _reader->readKlineSlice("EXRO.rb2301", KP_Minute1, 3, m1_etime(40)));
	EXPECT_EQ(nullptr, denied) << "missing history in a read-only root must not be served as an RT-only slice";
	if (denied != nullptr)
		denied->release();
	EXPECT_GE(_sink->count(LL_ERROR, "db directory"), 1u);
	EXPECT_FALSE(std::filesystem::exists(_root / "min1"));

	// Same reader retries after the directory becomes writable.
	std::filesystem::permissions(_root, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
	WTSKlineSlice* retry = _reader->readKlineSlice("EXRO.rb2301", KP_Minute1, 3, m1_etime(40));
	ASSERT_NE(nullptr, retry);
	EXPECT_EQ((uint64_t)m1_time(40), retry->at(-1)->time);
	retry->release();
}

TEST_F(S5ReaderADTest, ReadOnlyExchangeDirectoryWithoutDataFileFailsClosed)
{
	if (::geteuid() == 0)
		GTEST_SKIP() << "permission checks are bypassed for root";
	add_stock("EXRD", "rb2301");
	const std::filesystem::path dir = _root / "min1" / "EXRD";
	std::filesystem::create_directories(dir);
	init_reader();
	std::filesystem::permissions(dir, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
		std::filesystem::perm_options::replace);
	EXPECT_EQ(nullptr, _reader->readKlineSlice("EXRD.rb2301", KP_Minute1, 3, m1_etime(5)));
	EXPECT_GE(_sink->count(LL_ERROR, "Opening min1 db of EXRD failed"), 1u);
	EXPECT_FALSE(std::filesystem::exists(dir / "data.mdb"));
	std::filesystem::permissions(dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
}

//////////////////////////////////////////////////////////////////////////
// 第二轮：I03-NEW-052（未知品种：先报错拒绝，不依赖日期映射兜底）
//////////////////////////////////////////////////////////////////////////
TEST_F(S5ReaderADTest, UnknownCommodityIsRefusedBeforeMappingOrDereference)
{
	init_reader();	// no add_stock: getCommodity returns NULL
	EXPECT_EQ(nullptr, _reader->readTickSlice("EXNOPE.rb2301", 3, 20230103093000000ull));
	EXPECT_EQ(nullptr, _reader->readKlineSlice("EXNOPE.rb2301", KP_Minute1, 3, 20230103093000000ull));
	EXPECT_FALSE(std::filesystem::exists(_root / "ticks"));
	EXPECT_FALSE(std::filesystem::exists(_root / "min1"));
#ifndef WT_S5_BASELINE
	EXPECT_EQ(2u, _sink->count(LL_ERROR, "not found"));
	EXPECT_EQ(0u, _sink->count(LL_ERROR, "Mapping trading date"));
#endif
}
