// p05 / WtDataStorageAD：WtBtDtReaderAD 的 LMDB 取数链路（写入→读回→逐字段比对）
//
// 覆盖对象：
//  - WtBtDtReaderAD::init（cfg==NULL / cfg!=NULL 两条路径）
//  - WtBtDtReaderAD::read_raw_bars（min1/min5/day 三个子目录映射、非法周期、空库、缺失目录）
//  - WtBtDtReaderAD::read_raw_ticks（交易日分桶、时间上界哨兵 240000000）
//  - WtBtDtReaderAD::get_k_db / get_t_db（交易所级缓存 + WtLMDB 只读环境打开）
//  - LMDBBarKey / LMDBHftKey 与 WtLMDBQuery::put 的落库-读库回环
// 本文件不写仓库 dist/，所有落盘都在 temp_directory_path() 下的唯一目录。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
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
#include "Includes/IBtDtReader.h"
#include "Share/TimeUtils.hpp"
#include "WtDataStorageAD/LMDBKeys.h"
#include "WtDataStorageAD/WtBtDtReaderAD.h"
#include "WTSUtils/WtLMDB.hpp"

using namespace wtp;

static_assert(sizeof(WTSBarStructOld) == 68, "legacy AD bar value is the original pack(1) ABI");
static_assert(sizeof(WTSTickStructOld) == 402, "legacy AD tick value is the original pack(1) ABI");

#pragma pack(push, 1)
struct WTSBarStructPackedV2Fixture
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

struct WTSTickStructPackedV2Fixture
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

static_assert(sizeof(WTSBarStructPackedV2Fixture) == 84, "12098b5b bar fixture ABI mismatch");
static_assert(sizeof(WTSTickStructPackedV2Fixture) == 508, "12098b5b tick fixture ABI mismatch");

namespace wtp {

struct WtLMDBRangeTestAccess
{
	static MDB_dbi replace_dbi(WtLMDBQuery& query, MDB_dbi replacement)
	{
		const MDB_dbi previous = query._dbi;
		query._dbi = replacement;
		return previous;
	}

	static void set_cursor_get_override(WtLMDBQuery& query,
		WtLMDBQuery::RangeCursorGetOverride callback, void* context)
	{
		query._range_cursor_get_override = callback;
		query._range_cursor_get_context = context;
	}
};

} // namespace wtp

namespace {

struct RangeGetFailureState
{
	int calls = 0;
	int fail_on_call = 2;
};

int fail_second_range_get(void* context, MDB_cursor* cursor, MDB_val* key, MDB_val* data, MDB_cursor_op op)
{
	RangeGetFailureState* state = static_cast<RangeGetFailureState*>(context);
	if (++state->calls == state->fail_on_call)
		return MDB_CORRUPTED;
	return mdb_cursor_get(cursor, key, data, op);
}

// 复制 WtDataWriterAD::pipeToTicks 的时间换算：offTime = HHMM*100000 + SSsss
static void set_str(char* dst, size_t cap, const char* src)
{
	snprintf(dst, cap, "%s", src);
}

class P05BtSink : public IBtDtReaderSink
{
public:
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		_cnt[ll]++;
		_total++;
		_messages.emplace_back(message == NULL ? "" : message);
		if(ll <= LL_WARN && message != NULL)
			_last_warn = message;
		if(ll == LL_ERROR && message != NULL)
			_last_error = message;
	}

	uint32_t count_of(WTSLogLevel ll) const
	{
		auto it = _cnt.find(ll);
		return it == _cnt.end() ? 0u : it->second;
	}

	uint32_t total() const { return _total; }
	const std::string& last_warn() const { return _last_warn; }
	const std::string& last_error() const { return _last_error; }
	uint32_t count_containing(const std::string& text) const
	{
		return (uint32_t)std::count_if(_messages.begin(), _messages.end(), [&text](const std::string& message) {
			return message.find(text) != std::string::npos;
		});
	}

private:
	wt_hashmap<WTSLogLevel, uint32_t> _cnt;
	std::vector<std::string> _messages;
	uint32_t	_total = 0;
	std::string	_last_warn;
	std::string	_last_error;
};

class P05BtReaderADTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		_root = std::filesystem::temp_directory_path()
			/ ("p05_btdt_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
		std::filesystem::remove_all(_root);
		std::filesystem::create_directories(_root);
	}

	void TearDown() override
	{
		// 只读环境持有 mmap，析构必须在 remove_all 之前完成
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

	static std::string raw_of(const WTSBarStructOld& bar)
	{
		return std::string((const char*)&bar, sizeof(bar));
	}

	static std::string raw_of(const WTSBarStructPackedV2Fixture& bar)
	{
		return std::string((const char*)&bar, sizeof(bar));
	}

	static std::string raw_of(const WTSTickStruct& tick)
	{
		return std::string((const char*)&tick, sizeof(tick));
	}

	static std::string raw_of(const WTSTickStructOld& tick)
	{
		return std::string((const char*)&tick, sizeof(tick));
	}

	static std::string raw_of(const WTSTickStructPackedV2Fixture& tick)
	{
		return std::string((const char*)&tick, sizeof(tick));
	}

	static WTSBarStruct make_bar(uint32_t date, uint64_t time, double base)
	{
		WTSBarStruct bar;
		bar.date = date;
		bar.time = time;
		bar.open = base;
		bar.high = base + 1.5;
		bar.low = base - 1.5;
		bar.close = base + 0.25;
		bar.settle = base + 0.5;
		bar.money = base * 100.0;
		bar.vol = base * 3.0;
		bar.hold = base * 4.0;
		bar.add = base * 5.0;
		return bar;
	}

	static WTSTickStruct make_tick(const char* exchg, const char* code,
		uint32_t tdate, uint32_t adate, uint32_t atime, double price)
	{
		WTSTickStruct tick;
		set_str(tick.exchg, sizeof(tick.exchg), exchg);
		set_str(tick.code, sizeof(tick.code), code);
		tick.price = price;
		tick.open = price - 1.0;
		tick.high = price + 1.0;
		tick.low = price - 2.0;
		tick.settle_price = price;
		tick.upper_limit = price + 100.0;
		tick.lower_limit = price - 100.0;
		tick.total_volume = price * 10.0;
		tick.volume = price * 2.0;
		tick.total_turnover = price * 1000.0;
		tick.turn_over = price * 200.0;
		tick.open_interest = price * 5.0;
		tick.diff_interest = 1.0;
		tick.trading_date = tdate;
		tick.action_date = adate;
		tick.action_time = atime;
		return tick;
	}

	// 独立作用域写库，退出作用域即关闭读写环境，之后 reader 以只读方式再开
	void put_bars(const std::filesystem::path& db_dir, const char* exchg, const char* code,
		const std::vector<WTSBarStruct>& bars, bool key_by_date)
	{
		std::filesystem::create_directories(db_dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(db_dir.string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for(const WTSBarStruct& bar : bars)
		{
			const uint32_t tail = key_by_date ? (uint32_t)bar.date : (uint32_t)bar.time;
			ASSERT_TRUE(query.put(key_str(LMDBBarKey(exchg, code, tail)), raw_of(bar)));
		}
		query.commit();
	}

	void put_ticks(const std::filesystem::path& db_dir, const char* exchg, const char* code,
		const std::vector<WTSTickStruct>& ticks)
	{
		std::filesystem::create_directories(db_dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(db_dir.string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for(const WTSTickStruct& tick : ticks)
		{
			// 与 WtDataWriterAD::pipeToTicks 一致：action_time(=HHMMSSsss) 拆成 HHMM*100000+SSsss
			const uint32_t off = (tick.action_time / 100000) * 100000 + (tick.action_time % 100000);
			ASSERT_TRUE(query.put(key_str(LMDBHftKey(exchg, code, tick.trading_date, off)), raw_of(tick)));
		}
		query.commit();
	}

	WTSVariant* make_cfg(void)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", _root.string().c_str());
		return cfg;
	}

	void init_reader(bool with_cfg)
	{
		_reader.reset(new WtBtDtReaderAD());
		if(with_cfg)
		{
			WTSVariant* cfg = make_cfg();
			_reader->init(cfg, &_sink);
			cfg->release();
		}
		else
		{
			_reader->init(NULL, &_sink);
		}
	}

	std::filesystem::path _root;
	P05BtSink				_sink;
	std::unique_ptr<WtBtDtReaderAD> _reader;
};

}
//////////////////////////////////////////////////////////////////////////
// 状态：未配置数据目录
//////////////////////////////////////////////////////////////////////////

TEST_F(P05BtReaderADTest, NullCfgMakesEveryReadFail)
{
	init_reader(false);

	std::string buffer;
	EXPECT_FALSE(_reader->read_raw_bars("SHFE", "rb2409", KP_DAY, buffer));
	EXPECT_TRUE(buffer.empty());
	EXPECT_FALSE(_reader->read_raw_bars("SHFE", "rb2409", KP_Minute1, buffer));
	EXPECT_FALSE(_reader->read_raw_ticks("SHFE", "rb2409", 20240102, buffer));
	EXPECT_TRUE(buffer.empty());

	// init 里 cfg==NULL 直接 return，不会打 “initialized” 日志
	EXPECT_EQ(_sink.count_of(LL_INFO), 0u);
}

TEST_F(P05BtReaderADTest, MissingExchangeDirReturnsFalse)
{
	init_reader(true);

	std::string buffer;
	// day/DCE 目录根本不存在 → get_k_db 打不开 → false
	EXPECT_FALSE(_reader->read_raw_bars("DCE", "m2409", KP_DAY, buffer));
	EXPECT_TRUE(buffer.empty());
	// ticks/SHFE/xxx 目录不存在
	EXPECT_FALSE(_reader->read_raw_ticks("SHFE", "rb2409", 20240102, buffer));
	// 打开失败会走 LL_ERROR 分支
	EXPECT_GE(_sink.total(), 0u);
}

//////////////////////////////////////////////////////////////////////////
// 日线：日期作为键尾，与 pipeToDayBars 对齐
//////////////////////////////////////////////////////////////////////////

TEST_F(P05BtReaderADTest, DayBarRoundTripFieldByField)
{
	std::vector<WTSBarStruct> bars;
	for(uint32_t i = 0; i < 12; i++)
		bars.push_back(make_bar(20240102 + i, 20240102 + i, 3500.0 + i));
	// 故意逆序入库，读回必须按 LMDB 键序（= 日期升序）
	std::reverse(bars.begin(), bars.end());
	put_bars(_root / "day" / "SHFE", "SHFE", "rb2409", bars, true);
	std::reverse(bars.begin(), bars.end());

	init_reader(true);

	std::string buffer;
	ASSERT_TRUE(_reader->read_raw_bars("SHFE", "rb2409", KP_DAY, buffer));
	ASSERT_EQ(buffer.size(), bars.size() * sizeof(WTSBarStruct));
	const WTSBarStruct* out = (const WTSBarStruct*)buffer.data();
	for(size_t i = 0; i < bars.size(); i++)
	{
		EXPECT_EQ(out[i].date, bars[i].date) << i;
		// 卡3 3c=C（用户裁定 2026-11-04，R4-BUG-13）：库内为旧 writer 写法 time=交易日，
		// LMDB 值无逐条版本，读端一律把日线 time 归一为 0；其余字段逐项保留。
		ASSERT_NE(bars[i].time, 0u) << i;
		EXPECT_EQ(out[i].time, 0u) << i;
		EXPECT_DOUBLE_EQ(out[i].open, bars[i].open) << i;
		EXPECT_DOUBLE_EQ(out[i].high, bars[i].high) << i;
		EXPECT_DOUBLE_EQ(out[i].low, bars[i].low) << i;
		EXPECT_DOUBLE_EQ(out[i].close, bars[i].close) << i;
		EXPECT_DOUBLE_EQ(out[i].settle, bars[i].settle) << i;
		EXPECT_DOUBLE_EQ(out[i].money, bars[i].money) << i;
		EXPECT_DOUBLE_EQ(out[i].vol, bars[i].vol) << i;
		EXPECT_DOUBLE_EQ(out[i].hold, bars[i].hold) << i;
		EXPECT_DOUBLE_EQ(out[i].add, bars[i].add) << i;
	}
	EXPECT_EQ(_sink.count_of(LL_INFO), 1u);
}

TEST_F(P05BtReaderADTest, RepeatedDayBarReadsReturnStableBytes)
{
	std::vector<WTSBarStruct> bars;
	for(uint32_t i = 0; i < 5; i++)
		bars.push_back(make_bar(20240205 + i, 20240205 + i, 100.0 + i));
	put_bars(_root / "day" / "DCE", "DCE", "m2409", bars, true);

	init_reader(true);

	const std::size_t expect_sz = bars.size() * sizeof(WTSBarStruct);
	std::string first, second, third;
	ASSERT_TRUE(_reader->read_raw_bars("DCE", "m2409", KP_DAY, first));
	ASSERT_EQ(first.size(), expect_sz);

	auto expect_same_bytes = [&first](const std::string& actual) {
		EXPECT_EQ(actual.size(), first.size());
		if (actual.size() == first.size())
			EXPECT_TRUE(std::equal(actual.begin(), actual.end(), first.begin()));
	};

	// 每次访问同一库都应成功，并返回与首读完全一致的原始字节。
	EXPECT_TRUE(_reader->read_raw_bars("DCE", "m2409", KP_DAY, second));
	expect_same_bytes(second);
	EXPECT_TRUE(_reader->read_raw_bars("DCE", "m2409", KP_DAY, third));
	expect_same_bytes(third);
}

TEST_F(P05BtReaderADTest, Min1AndMin5UseSeparateDatabases)
{
	std::vector<WTSBarStruct> m1, m5;
	for(uint32_t i = 0; i < 9; i++)
		m1.push_back(make_bar(20240301, TimeUtils::timeToMinBar(20240301, 931 + i), 10.0 + i));
	for(uint32_t i = 0; i < 3; i++)
		m5.push_back(make_bar(20240301, TimeUtils::timeToMinBar(20240301, 935 + i * 5), 20.0 + i));
	put_bars(_root / "min1" / "CZCE", "CZCE", "SR409", m1, false);
	put_bars(_root / "min5" / "CZCE", "CZCE", "SR409", m5, false);

	init_reader(true);

	std::string buf1, buf5;
	ASSERT_TRUE(_reader->read_raw_bars("CZCE", "SR409", KP_Minute1, buf1));
	ASSERT_EQ(buf1.size(), m1.size() * sizeof(WTSBarStruct));
	const WTSBarStruct* out1 = (const WTSBarStruct*)buf1.data();
	for(size_t i = 0; i < m1.size(); i++)
	{
		EXPECT_EQ(out1[i].time, m1[i].time) << i;
		EXPECT_DOUBLE_EQ(out1[i].close, m1[i].close) << i;
	}

	ASSERT_TRUE(_reader->read_raw_bars("CZCE", "SR409", KP_Minute5, buf5));
	ASSERT_EQ(buf5.size(), m5.size() * sizeof(WTSBarStruct));
	const WTSBarStruct* out5 = (const WTSBarStruct*)buf5.data();
	for(size_t i = 0; i < m5.size(); i++)
	{
		EXPECT_EQ(out5[i].time, m5[i].time) << i;
		EXPECT_DOUBLE_EQ(out5[i].open, m5[i].open) << i;
	}

	// min1 的键尾是 bar.time（(date-19900000)*10000+hhmm），必须显著大于 date
	EXPECT_GT(m1.front().time, 20240301u);
}

TEST_F(P05BtReaderADTest, OnlyThreePeriodsAreRoutable)
{
	init_reader(true);

	std::string buffer;
	// get_k_db 只认 min1/min5/day，其它周期返回空指针（此时不会越界访问 PERIOD_NAME）
	EXPECT_FALSE(_reader->read_raw_bars("SHFE", "rb2409", (WTSKlinePeriod)200, buffer));
	EXPECT_TRUE(buffer.empty());
}

TEST_F(P05BtReaderADTest, CodePrefixFilterKeepsOtherCodesOut)
{
	std::vector<WTSBarStruct> rb, hc;
	for(uint32_t i = 0; i < 6; i++)
		rb.push_back(make_bar(20240401 + i, 20240401 + i, 300.0 + i));
	for(uint32_t i = 0; i < 4; i++)
		hc.push_back(make_bar(20240401 + i, 20240401 + i, 500.0 + i));
	{
		// 同一 db 内两个合约
		std::filesystem::path dir = _root / "day" / "SHFE";
		std::filesystem::create_directories(dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(dir.string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for(const WTSBarStruct& bar : rb)
			ASSERT_TRUE(query.put(key_str(LMDBBarKey("SHFE", "rb2409", (uint32_t)bar.date)), raw_of(bar)));
		for(const WTSBarStruct& bar : hc)
			ASSERT_TRUE(query.put(key_str(LMDBBarKey("SHFE", "hc2409", (uint32_t)bar.date)), raw_of(bar)));
		query.commit();
	}

	init_reader(true);

	std::string buffer;
	ASSERT_TRUE(_reader->read_raw_bars("SHFE", "rb2409", KP_DAY, buffer));
	EXPECT_EQ(buffer.size(), rb.size() * sizeof(WTSBarStruct));
	const WTSBarStruct* out = (const WTSBarStruct*)buffer.data();
	for(size_t i = 0; i < rb.size(); i++)
		EXPECT_DOUBLE_EQ(out[i].open, rb[i].open) << i;

	// 第二个合约共享同一交易所数据库句柄，但只能读到自身记录。
	buffer.clear();
	ASSERT_TRUE(_reader->read_raw_bars("SHFE", "hc2409", KP_DAY, buffer));
	ASSERT_EQ(buffer.size(), hc.size() * sizeof(WTSBarStruct));
	out = (const WTSBarStruct*)buffer.data();
	for(size_t i = 0; i < hc.size(); i++)
		EXPECT_DOUBLE_EQ(out[i].open, hc[i].open) << i;

	// 切换合约后再次读取首个合约，缓存句柄仍应有效且结果稳定。
	buffer.clear();
	ASSERT_TRUE(_reader->read_raw_bars("SHFE", "rb2409", KP_DAY, buffer));
	ASSERT_EQ(buffer.size(), rb.size() * sizeof(WTSBarStruct));
	out = (const WTSBarStruct*)buffer.data();
	for(size_t i = 0; i < rb.size(); i++)
		EXPECT_DOUBLE_EQ(out[i].open, rb[i].open) << i;
}

TEST_F(P05BtReaderADTest, EmptyDatabaseReturnsTrueWithNoBytes)
{
	// 建一个空的 day/INE 库（没有任何 key），目录存在 → 环境能打开
	std::filesystem::path dir = _root / "day" / "INE";
	std::filesystem::create_directories(dir);
	{
		WtLMDB db(false);
		ASSERT_TRUE(db.open(dir.string().c_str(), 4 * 1024 * 1024));
		WtLMDBQuery query(db);
		query.commit();
	}
	{
		const std::filesystem::path tick_dir = _root / "ticks" / "INE" / "sc2409";
		std::filesystem::create_directories(tick_dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(tick_dir.string().c_str(), 4 * 1024 * 1024));
		WtLMDBQuery query(db);
		query.commit();
	}

	init_reader(true);

	std::string buffer = "stale-bars";
	EXPECT_TRUE(_reader->read_raw_bars("INE", "sc2409", KP_DAY, buffer));
	EXPECT_TRUE(buffer.empty());
	buffer = "stale-ticks";
	EXPECT_TRUE(_reader->read_raw_ticks("INE", "sc2409", 20240601, buffer));
	EXPECT_TRUE(buffer.empty());
}

TEST_F(P05BtReaderADTest, NoMatchInPopulatedDatabaseClearsExistingOutput)
{
	const WTSBarStruct bar = make_bar(20240601, 20240601, 10.0);
	put_bars(_root / "day" / "DCE", "DCE", "rb2409", { bar }, true);
	put_ticks(_root / "ticks" / "DCE" / "rb2409", "DCE", "rb2409",
		{ make_tick("DCE", "rb2409", 20240601, 20240601, 93000000, 100.0) });

	init_reader(true);
	std::string buffer = "stale-bars";
	EXPECT_TRUE(_reader->read_raw_bars("DCE", "missing2409", KP_DAY, buffer));
	EXPECT_TRUE(buffer.empty());

	buffer = "stale-ticks";
	EXPECT_TRUE(_reader->read_raw_ticks("DCE", "rb2409", 20240602, buffer));
	EXPECT_TRUE(buffer.empty());
}

//////////////////////////////////////////////////////////////////////////
// Tick：按交易日分桶
//////////////////////////////////////////////////////////////////////////

TEST_F(P05BtReaderADTest, TickRoundTripFilteredByTradingDate)
{
	std::vector<WTSTickStruct> ticks;
	for(uint32_t i = 0; i < 6; i++)
		ticks.push_back(make_tick("SHFE", "rb2409", 20240102, 20240102, 93000000 + i * 1000, 3600.0 + i));
	for(uint32_t i = 0; i < 3; i++)
		ticks.push_back(make_tick("SHFE", "rb2409", 20240103, 20240103, 210000000 + i * 1000, 3700.0 + i));
	put_ticks(_root / "ticks" / "SHFE" / "rb2409", "SHFE", "rb2409", ticks);

	init_reader(true);

	std::string buffer;
	ASSERT_TRUE(_reader->read_raw_ticks("SHFE", "rb2409", 20240102, buffer));
	ASSERT_EQ(buffer.size(), 6u * sizeof(WTSTickStruct));
	const WTSTickStruct* out = (const WTSTickStruct*)buffer.data();
	for(uint32_t i = 0; i < 6; i++)
	{
		EXPECT_EQ(out[i].trading_date, 20240102u) << i;
		EXPECT_EQ(out[i].action_time, 93000000u + i * 1000u) << i;
		EXPECT_DOUBLE_EQ(out[i].price, 3600.0 + i) << i;
		EXPECT_DOUBLE_EQ(out[i].upper_limit, 3600.0 + i + 100.0) << i;
		EXPECT_DOUBLE_EQ(out[i].open_interest, (3600.0 + i) * 5.0) << i;
		EXPECT_STREQ(out[i].code, "rb2409") << i;
	}

	// 夜盘时段：offTime 由 210000000 拆成 2100*100000+00000，仍落在 [0, 240000000) 内
	buffer.clear();
	ASSERT_TRUE(_reader->read_raw_ticks("SHFE", "rb2409", 20240103, buffer));
	ASSERT_EQ(buffer.size(), 3u * sizeof(WTSTickStruct));
	out = (const WTSTickStruct*)buffer.data();
	for(uint32_t i = 0; i < 3; i++)
	{
		EXPECT_EQ(out[i].trading_date, 20240103u) << i;
		EXPECT_DOUBLE_EQ(out[i].price, 3700.0 + i) << i;
	}

	// 未入库的交易日：区间为空但目录已打开，返回 true 且不带数据
	buffer.clear();
	EXPECT_TRUE(_reader->read_raw_ticks("SHFE", "rb2409", 20240104, buffer));
	EXPECT_TRUE(buffer.empty());
}

TEST_F(P05BtReaderADTest, TickDbCacheReusesHandlePerExchangeAndContract)
{
	put_ticks(_root / "ticks" / "DCE" / "i2409", "DCE", "i2409",
		{ make_tick("DCE", "i2409", 20240506, 20240506, 103000000, 800.0) });
	put_ticks(_root / "ticks" / "DCE" / "j2409", "DCE", "j2409",
		{ make_tick("DCE", "j2409", 20240506, 20240506, 103000000, 900.0),
		  make_tick("DCE", "j2409", 20240506, 20240506, 103001000, 901.0) });

	init_reader(true);

	for(uint32_t round = 0; round < 3; round++)
	{
		std::string b1, b2;
		ASSERT_TRUE(_reader->read_raw_ticks("DCE", "i2409", 20240506, b1)) << round;
		EXPECT_EQ(b1.size(), sizeof(WTSTickStruct)) << round;
		const WTSTickStruct* t1 = (const WTSTickStruct*)b1.data();
		EXPECT_STREQ(t1[0].code, "i2409") << round;
		EXPECT_DOUBLE_EQ(t1[0].price, 800.0) << round;
		ASSERT_TRUE(_reader->read_raw_ticks("DCE", "j2409", 20240506, b2)) << round;
		ASSERT_EQ(b2.size(), 2u * sizeof(WTSTickStruct)) << round;
		const WTSTickStruct* t2 = (const WTSTickStruct*)b2.data();
		EXPECT_STREQ(t2[0].code, "j2409") << round;
		EXPECT_DOUBLE_EQ(t2[0].price, 900.0) << round;
		EXPECT_DOUBLE_EQ(t2[1].price, 901.0) << round;
	}

	// Each contract opens once, repeated reads hit its own shared handle.
	EXPECT_EQ(_sink.count_containing("Tick db of DCE.i2409 opened"), 1u);
	EXPECT_EQ(_sink.count_containing("Tick db of DCE.j2409 opened"), 1u);
}

//////////////////////////////////////////////////////////////////////////
// LMDB AD 历史 writer 写过 pre-12098 pack(1)、12098 pack(1) 和当前结构；读侧只接收
// 这三种明确 value ABI，拒绝其他长度，并在整组记录通过校验前不修改调用方 buffer。
//////////////////////////////////////////////////////////////////////////
TEST_F(P05BtReaderADTest, GetRangeCursorErrorDoesNotPublishPartialResults)
{
	const std::filesystem::path db_dir = _root / "range_error";
	std::filesystem::create_directories(db_dir);
	WtLMDB db(false);
	ASSERT_TRUE(db.open(db_dir.string().c_str(), 4 * 1024 * 1024));
	{
		WtLMDBQuery writer(db);
		ASSERT_TRUE(writer.put("a", "first"));
		ASSERT_TRUE(writer.put("b", "second"));
		writer.commit();
	}

	WtLMDBQuery query(db);
	int callback_calls = 0;
	const MDB_dbi valid_dbi = WtLMDBRangeTestAccess::replace_dbi(query, static_cast<MDB_dbi>(-1));
	EXPECT_EQ(-1, query.get_range_checked("a", "z", [&callback_calls](const ValueArray&, const ValueArray&) {
		++callback_calls;
	}));
	EXPECT_EQ(0, callback_calls);
	EXPECT_TRUE(db.has_error());
	WtLMDBRangeTestAccess::replace_dbi(query, valid_dbi);

	RangeGetFailureState failure;
	WtLMDBRangeTestAccess::set_cursor_get_override(query, &fail_second_range_get, &failure);
	EXPECT_EQ(-1, query.get_range_checked("a", "z", [&callback_calls](const ValueArray&, const ValueArray&) {
		++callback_calls;
	}));
	EXPECT_EQ(2, failure.calls);
	EXPECT_EQ(0, callback_calls);
	EXPECT_TRUE(db.has_error());
	EXPECT_STREQ(mdb_strerror(MDB_CORRUPTED), db.errmsg());

	// 错误退出必须关闭 cursor；清除注入后，同一事务仍可完成完整扫描。
	WtLMDBRangeTestAccess::set_cursor_get_override(query, NULL, NULL);
	ValueArray keys;
	ValueArray values;
	EXPECT_EQ(2, query.get_range_checked("a", "z", [&keys, &values](const ValueArray& found_keys, const ValueArray& found_values) {
		keys = found_keys;
		values = found_values;
	}));
	EXPECT_FALSE(db.has_error());
	ASSERT_EQ(2u, keys.size());
	ASSERT_EQ(2u, values.size());
	EXPECT_EQ("a", keys[0]);
	EXPECT_EQ("second", values[1]);
}

TEST_F(P05BtReaderADTest, MalformedLmdbValuesAreRejectedWithoutChangingOutput)
{
	const std::filesystem::path bar_dir = _root / "day" / "KEX";
	const std::vector<std::pair<std::string, std::string>> bad_bars = {
		{ "zero2409", std::string() },
		{ "short2409", std::string(sizeof(WTSBarStruct) - 1, 'x') },
		{ "long2409", std::string(sizeof(WTSBarStruct) + 1, 'y') }
	};
	std::filesystem::create_directories(bar_dir);
	{
		WtLMDB db(false);
		ASSERT_TRUE(db.open(bar_dir.string().c_str(), 4 * 1024 * 1024));
		WtLMDBQuery query(db);
		for (const auto& bad : bad_bars)
		{
			// 正常记录先于损坏记录，验证失败时前面的有效数据也不会部分输出。
			const WTSBarStruct full = make_bar(20240601, 20240601, 100.0);
			ASSERT_TRUE(query.put(key_str(LMDBBarKey("KEX", bad.first.c_str(), full.date)), raw_of(full)));
			ASSERT_TRUE(query.put(key_str(LMDBBarKey("KEX", bad.first.c_str(), 20240602)), bad.second));
		}
		WTSBarStruct valid = make_bar(20240601, 20240601, 200.0);
		valid.reserve_ = 0xA1B2C3D4u;
		ASSERT_TRUE(query.put(key_str(LMDBBarKey("KEX", "valid2409", valid.date)), raw_of(valid)));
		// 这里用当前 key 编码来隔离验证旧 value ABI 的字段转换；e75 历史 key 的
		// exchange 宽度及 tick 日期/时间问题另有记录，不把本 fixture 当作旧 key 兼容证据。
		WTSBarStructOld legacy;
		legacy.date = 20240603;
		legacy.time = 20240603;
		legacy.open = 150.0;
		legacy.high = 152.0;
		legacy.low = 149.0;
		legacy.close = 151.0;
		legacy.settle = 150.5;
		legacy.money = 123456.0;
		legacy.vol = 123;
		legacy.hold = 456;
		legacy.add = -7;
		ASSERT_TRUE(query.put(key_str(LMDBBarKey("KEX", "legacy2409", legacy.date)), raw_of(legacy)));
		WTSBarStructPackedV2Fixture packed_bar = {};
		packed_bar.date = 20240605;
		packed_bar.time = 20240605;
		packed_bar.open = 170.0;
		packed_bar.high = 172.0;
		packed_bar.low = 169.0;
		packed_bar.close = 171.0;
		packed_bar.settle = 170.5;
		packed_bar.money = 98765.0;
		packed_bar.vol = 321.5;
		packed_bar.hold = 654.5;
		packed_bar.add = -9.5;
		ASSERT_TRUE(query.put(key_str(LMDBBarKey("KEX", "packed2409", packed_bar.date)), raw_of(packed_bar)));

		const WTSBarStruct long_key_bar = make_bar(20240606, 20240606, 180.0);
		LMDBBarKey long_key_prefix("KEX", "longkey2409", long_key_bar.date);
		std::string long_key((const char*)&long_key_prefix, sizeof(long_key_prefix));
		long_key.append(440, 'k'); // 492-byte key: below LMDB's key limit, above reader's fixed ABI.
		ASSERT_TRUE(query.put(long_key, raw_of(long_key_bar)));

		// 保留固定字段前缀，仅让 time 第一个字节大于下界并截去剩余三字节。
		// 这种短 key 在 LMDB 字节序范围内，能被真实 range 查询取到。
		const std::string short_lower = key_str(LMDBBarKey("KEX", "shortkey2409", 0));
		const std::string short_upper = key_str(LMDBBarKey("KEX", "shortkey2409", 0xffffffffu));
		std::string short_key = short_lower;
		short_key.resize(sizeof(LMDBBarKey) - 3);
		short_key.back() = '\x01';
		ASSERT_GT(compare_lmdb_keys(short_key.data(), short_key.size(), short_lower.data(), short_lower.size()), 0);
		ASSERT_LT(compare_lmdb_keys(short_key.data(), short_key.size(), short_upper.data(), short_upper.size()), 0);
		const WTSBarStruct short_key_bar = make_bar(20240607, 20240607, 190.0);
		ASSERT_TRUE(query.put(short_key, raw_of(short_key_bar)));
		query.commit();
	}

	WTSTickStruct good_tick = make_tick("KEX", "valid2409", 20240601, 20240601, 93000000, 300.0);
	good_tick.reserve_ = 0xC0FFEE12u;
	std::size_t short_tick_key_size = 0;
	const std::vector<std::pair<std::string, std::string>> bad_ticks = {
		{ "zerotick", std::string() },
		{ "shorttick", std::string(sizeof(WTSTickStruct) - 1, 'x') },
		{ "longtick", std::string(sizeof(WTSTickStruct) + 1, 'y') }
	};
	for (const auto& bad : bad_ticks)
	{
		const std::filesystem::path tick_dir = _root / "ticks" / "KEX" / bad.first;
		std::filesystem::create_directories(tick_dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(tick_dir.string().c_str(), 4 * 1024 * 1024));
		WtLMDBQuery query(db);
		const WTSTickStruct first = make_tick("KEX", bad.first.c_str(), 20240601, 20240601, 93000000, 400.0);
		ASSERT_TRUE(query.put(key_str(LMDBHftKey("KEX", bad.first.c_str(), 20240601, 93000000)), raw_of(first)));
		ASSERT_TRUE(query.put(key_str(LMDBHftKey("KEX", bad.first.c_str(), 20240601, 93001000)), bad.second));
		query.commit();
	}
	{
		const std::filesystem::path tick_dir = _root / "ticks" / "KEX" / "valid2409";
		std::filesystem::create_directories(tick_dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(tick_dir.string().c_str(), 4 * 1024 * 1024));
		WtLMDBQuery query(db);
		ASSERT_TRUE(query.put(key_str(LMDBHftKey("KEX", "valid2409", 20240601, 93000000)), raw_of(good_tick)));
		query.commit();
	}
	{
		const std::filesystem::path tick_dir = _root / "ticks" / "KEX" / "legacy2409";
		std::filesystem::create_directories(tick_dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(tick_dir.string().c_str(), 4 * 1024 * 1024));
		WtLMDBQuery query(db);
		// 和 bars 对照一样，保留旧 pack(1) value，但以当前的正常 key 读取。
		WTSTickStructOld legacy;
		set_str(legacy.exchg, sizeof(legacy.exchg), "KEX");
		set_str(legacy.code, sizeof(legacy.code), "legacy2409");
		legacy.price = 250.5;
		legacy.open = 249.5;
		legacy.high = 252.0;
		legacy.low = 248.0;
		legacy.settle_price = 250.0;
		legacy.upper_limit = 350.0;
		legacy.lower_limit = 150.0;
		legacy.total_volume = 1000;
		legacy.volume = 50;
		legacy.total_turnover = 250500.0;
		legacy.turn_over = 12525.0;
		legacy.open_interest = 2000;
		legacy.diff_interest = -3;
		legacy.trading_date = 20240601;
		legacy.action_date = 20240601;
		legacy.action_time = 93000000;
		legacy.pre_close = 249.0;
		legacy.pre_settle = 249.5;
		legacy.pre_interest = 1990;
		legacy.bid_prices[0] = 250.0;
		legacy.ask_prices[0] = 251.0;
		legacy.bid_qty[0] = 12;
		legacy.ask_qty[0] = 13;
		for (std::size_t level = 1; level < 10; ++level)
		{
			legacy.bid_prices[level] = 250.0 + level;
			legacy.ask_prices[level] = 251.0 + level;
			legacy.bid_qty[level] = static_cast<uint32_t>(12 + level);
			legacy.ask_qty[level] = static_cast<uint32_t>(13 + level);
		}
		ASSERT_TRUE(query.put(key_str(LMDBHftKey("KEX", "legacy2409", 20240601, 93000000)), raw_of(legacy)));
		query.commit();
	}
	{
		const std::filesystem::path tick_dir = _root / "ticks" / "KEX" / "packed2409";
		std::filesystem::create_directories(tick_dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(tick_dir.string().c_str(), 4 * 1024 * 1024));
		WtLMDBQuery query(db);
		WTSTickStructPackedV2Fixture packed_tick = {};
		set_str(packed_tick.exchg, sizeof(packed_tick.exchg), "KEX");
		set_str(packed_tick.code, sizeof(packed_tick.code), "packed2409");
		packed_tick.price = 275.5;
		packed_tick.open = 274.5;
		packed_tick.high = 277.0;
		packed_tick.low = 273.0;
		packed_tick.settle_price = 275.0;
		packed_tick.upper_limit = 375.0;
		packed_tick.lower_limit = 175.0;
		packed_tick.total_volume = 1100.5;
		packed_tick.volume = 55.5;
		packed_tick.total_turnover = 303050.0;
		packed_tick.turn_over = 15152.5;
		packed_tick.open_interest = 2050.5;
		packed_tick.diff_interest = -3.5;
		packed_tick.trading_date = 20240601;
		packed_tick.action_date = 20240601;
		packed_tick.action_time = 93002000;
		packed_tick.pre_close = 274.0;
		packed_tick.pre_settle = 274.5;
		packed_tick.pre_interest = 1990.5;
		packed_tick.bid_prices[0] = 275.0;
		packed_tick.ask_prices[0] = 276.0;
		packed_tick.bid_qty[0] = 12.5;
		packed_tick.ask_qty[0] = 13.5;
		for (std::size_t level = 1; level < 10; ++level)
		{
			packed_tick.bid_prices[level] = 275.0 + level;
			packed_tick.ask_prices[level] = 276.0 + level;
			packed_tick.bid_qty[level] = 12.5 + level;
			packed_tick.ask_qty[level] = 13.5 + level;
		}
		ASSERT_TRUE(query.put(key_str(LMDBHftKey("KEX", "packed2409", 20240601, 93002000)), raw_of(packed_tick)));
		query.commit();
	}
	{
		const std::filesystem::path tick_dir = _root / "ticks" / "KEX" / "shortkey2409";
		std::filesystem::create_directories(tick_dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(tick_dir.string().c_str(), 4 * 1024 * 1024));
		WtLMDBQuery query(db);
		const std::string lower = key_str(LMDBHftKey("KEX", "shortkey2409", 20240601, 0));
		const std::string upper = key_str(LMDBHftKey("KEX", "shortkey2409", 20240601, 240000000));
		std::string short_key = lower;
		for (std::size_t index = offsetof(LMDBHftKey, _time); index < upper.size(); ++index)
		{
			if (static_cast<unsigned char>(upper[index]) != 0)
			{
				short_key.resize(index + 1);
				short_key[index] = '\x01';
				break;
			}
		}
		ASSERT_LT(short_key.size(), sizeof(LMDBHftKey));
		ASSERT_GT(compare_lmdb_keys(short_key.data(), short_key.size(), lower.data(), lower.size()), 0);
		ASSERT_LT(compare_lmdb_keys(short_key.data(), short_key.size(), upper.data(), upper.size()), 0);
		short_tick_key_size = short_key.size();
		const WTSTickStruct short_key_tick = make_tick("KEX", "shortkey2409", 20240601, 20240601, 93000000, 320.0);
		ASSERT_TRUE(query.put(short_key, raw_of(short_key_tick)));

		query.commit();
	}
	{
		const std::filesystem::path tick_dir = _root / "ticks" / "KEX" / "longkey2409";
		std::filesystem::create_directories(tick_dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(tick_dir.string().c_str(), 4 * 1024 * 1024));
		WtLMDBQuery query(db);
		const std::string lower = key_str(LMDBHftKey("KEX", "longkey2409", 20240601, 0));
		const std::string upper = key_str(LMDBHftKey("KEX", "longkey2409", 20240601, 240000000));
		std::string long_key = key_str(LMDBHftKey("KEX", "longkey2409", 20240601, 93001000));
		long_key.append("extra");
		ASSERT_GT(compare_lmdb_keys(long_key.data(), long_key.size(), lower.data(), lower.size()), 0);
		ASSERT_LT(compare_lmdb_keys(long_key.data(), long_key.size(), upper.data(), upper.size()), 0);
		const WTSTickStruct long_key_tick = make_tick("KEX", "longkey2409", 20240601, 20240601, 93001000, 330.0);
		ASSERT_TRUE(query.put(long_key, raw_of(long_key_tick)));
		query.commit();
	}

	init_reader(true);
	for (const auto& bad : bad_bars)
	{
		std::string buffer = "preserve-bars";
		EXPECT_FALSE(_reader->read_raw_bars("KEX", bad.first.c_str(), KP_DAY, buffer)) << bad.first;
		EXPECT_EQ("preserve-bars", buffer) << bad.first;
	}
	{
		std::string buffer = "preserve-long-key";
		EXPECT_FALSE(_reader->read_raw_bars("KEX", "longkey2409", KP_DAY, buffer));
		EXPECT_EQ("preserve-long-key", buffer);
		EXPECT_LT(_sink.last_error().size(), 512u);
		EXPECT_NE(std::string::npos, _sink.last_error().find("key bytes=492"));
		EXPECT_NE(std::string::npos, _sink.last_error().find("..."));
	}
	{
		std::string buffer = "preserve-short-key";
		EXPECT_FALSE(_reader->read_raw_bars("KEX", "shortkey2409", KP_DAY, buffer));
		EXPECT_EQ("preserve-short-key", buffer);
		EXPECT_NE(std::string::npos, _sink.last_error().find("key bytes=" + std::to_string(sizeof(LMDBBarKey) - 3)));
	}
	{
		std::string buffer = "preserve-bars";
		ASSERT_TRUE(_reader->read_raw_bars("KEX", "valid2409", KP_DAY, buffer));
		ASSERT_EQ(sizeof(WTSBarStruct), buffer.size());
		const WTSBarStruct* out = reinterpret_cast<const WTSBarStruct*>(buffer.data());
		EXPECT_EQ(20240601u, out->date);
		// 3c=C：读端日线 time 归一为 0（库内 time=20240601）
		EXPECT_EQ(0u, out->time);
		EXPECT_DOUBLE_EQ(200.0, out->open);
		EXPECT_EQ(0xA1B2C3D4u, out->reserve_);
	}
	{
		std::string buffer;
		ASSERT_TRUE(_reader->read_raw_bars("KEX", "legacy2409", KP_DAY, buffer));
		ASSERT_EQ(sizeof(WTSBarStruct), buffer.size());
		const WTSBarStruct* out = reinterpret_cast<const WTSBarStruct*>(buffer.data());
		EXPECT_EQ(20240603u, out->date);
		// 3c=C：旧 value ABI 转换后日线 time 同样归一为 0（库内 time=20240603）
		EXPECT_EQ(0u, out->time);
		EXPECT_DOUBLE_EQ(150.0, out->open);
		EXPECT_DOUBLE_EQ(152.0, out->high);
		EXPECT_DOUBLE_EQ(149.0, out->low);
		EXPECT_DOUBLE_EQ(151.0, out->close);
		EXPECT_DOUBLE_EQ(150.5, out->settle);
		EXPECT_DOUBLE_EQ(123456.0, out->money);
		EXPECT_DOUBLE_EQ(123.0, out->vol);
		EXPECT_DOUBLE_EQ(456.0, out->hold);
		EXPECT_DOUBLE_EQ(-7.0, out->add);
		EXPECT_EQ(0u, out->reserve_);
	}
	{
		std::string buffer;
		ASSERT_TRUE(_reader->read_raw_bars("KEX", "packed2409", KP_DAY, buffer));
		ASSERT_EQ(sizeof(WTSBarStruct), buffer.size());
		const WTSBarStruct* out = reinterpret_cast<const WTSBarStruct*>(buffer.data());
		EXPECT_EQ(20240605u, out->date);
		// 3c=C：packed v2 value 转换后日线 time 归一为 0（库内 time=20240605）
		EXPECT_EQ(0u, out->time);
		EXPECT_DOUBLE_EQ(170.0, out->open);
		EXPECT_DOUBLE_EQ(172.0, out->high);
		EXPECT_DOUBLE_EQ(169.0, out->low);
		EXPECT_DOUBLE_EQ(171.0, out->close);
		EXPECT_DOUBLE_EQ(170.5, out->settle);
		EXPECT_DOUBLE_EQ(98765.0, out->money);
		EXPECT_DOUBLE_EQ(321.5, out->vol);
		EXPECT_DOUBLE_EQ(654.5, out->hold);
		EXPECT_DOUBLE_EQ(-9.5, out->add);
		EXPECT_EQ(0u, out->reserve_);
	}

	for (const auto& bad : bad_ticks)
	{
		std::string buffer = "preserve-ticks";
		EXPECT_FALSE(_reader->read_raw_ticks("KEX", bad.first.c_str(), 20240601, buffer)) << bad.first;
		EXPECT_EQ("preserve-ticks", buffer) << bad.first;
	}
	{
		std::string buffer = "preserve-short-tick-key";
		EXPECT_FALSE(_reader->read_raw_ticks("KEX", "shortkey2409", 20240601, buffer));
		EXPECT_EQ("preserve-short-tick-key", buffer);
		EXPECT_NE(std::string::npos, _sink.last_error().find("key bytes=" + std::to_string(short_tick_key_size)));
	}
	{
		std::string buffer = "preserve-long-tick-key";
		EXPECT_FALSE(_reader->read_raw_ticks("KEX", "longkey2409", 20240601, buffer));
		EXPECT_EQ("preserve-long-tick-key", buffer);
		EXPECT_NE(std::string::npos, _sink.last_error().find("key bytes=" + std::to_string(sizeof(LMDBHftKey) + 5)));
	}
	{
		std::string buffer = "preserve-ticks";
		ASSERT_TRUE(_reader->read_raw_ticks("KEX", "valid2409", 20240601, buffer));
		ASSERT_EQ(sizeof(WTSTickStruct), buffer.size());
		const WTSTickStruct* out = reinterpret_cast<const WTSTickStruct*>(buffer.data());
		EXPECT_DOUBLE_EQ(300.0, out->price);
		EXPECT_EQ(0xC0FFEE12u, out->reserve_);
	}
	{
		std::string buffer;
		ASSERT_TRUE(_reader->read_raw_ticks("KEX", "legacy2409", 20240601, buffer));
		ASSERT_EQ(sizeof(WTSTickStruct), buffer.size());
		const WTSTickStruct* out = reinterpret_cast<const WTSTickStruct*>(buffer.data());
		EXPECT_STREQ("KEX", out->exchg);
		EXPECT_STREQ("legacy2409", out->code);
		EXPECT_DOUBLE_EQ(250.5, out->price);
		EXPECT_DOUBLE_EQ(249.5, out->open);
		EXPECT_DOUBLE_EQ(252.0, out->high);
		EXPECT_DOUBLE_EQ(248.0, out->low);
		EXPECT_DOUBLE_EQ(250.0, out->settle_price);
		EXPECT_DOUBLE_EQ(350.0, out->upper_limit);
		EXPECT_DOUBLE_EQ(150.0, out->lower_limit);
		EXPECT_DOUBLE_EQ(1000.0, out->total_volume);
		EXPECT_DOUBLE_EQ(50.0, out->volume);
		EXPECT_DOUBLE_EQ(250500.0, out->total_turnover);
		EXPECT_DOUBLE_EQ(12525.0, out->turn_over);
		EXPECT_DOUBLE_EQ(2000.0, out->open_interest);
		EXPECT_DOUBLE_EQ(-3.0, out->diff_interest);
		EXPECT_EQ(20240601u, out->trading_date);
		EXPECT_EQ(20240601u, out->action_date);
		EXPECT_EQ(93000000u, out->action_time);
		EXPECT_DOUBLE_EQ(249.0, out->pre_close);
		EXPECT_DOUBLE_EQ(249.5, out->pre_settle);
		EXPECT_DOUBLE_EQ(1990.0, out->pre_interest);
		EXPECT_DOUBLE_EQ(250.0, out->bid_prices[0]);
		EXPECT_EQ(12u, out->bid_qty[0]);
		EXPECT_DOUBLE_EQ(251.0, out->ask_prices[0]);
		EXPECT_EQ(13u, out->ask_qty[0]);
		EXPECT_EQ(0u, out->reserve_);
		for (std::size_t level = 0; level < 10; ++level)
		{
			EXPECT_DOUBLE_EQ(250.0 + level, out->bid_prices[level]) << level;
			EXPECT_DOUBLE_EQ(251.0 + level, out->ask_prices[level]) << level;
			EXPECT_DOUBLE_EQ(12.0 + level, out->bid_qty[level]) << level;
			EXPECT_DOUBLE_EQ(13.0 + level, out->ask_qty[level]) << level;
		}
	}
	{
		std::string buffer;
		ASSERT_TRUE(_reader->read_raw_ticks("KEX", "packed2409", 20240601, buffer));
		ASSERT_EQ(sizeof(WTSTickStruct), buffer.size());
		const WTSTickStruct* out = reinterpret_cast<const WTSTickStruct*>(buffer.data());
		EXPECT_STREQ("KEX", out->exchg);
		EXPECT_STREQ("packed2409", out->code);
		EXPECT_DOUBLE_EQ(275.5, out->price);
		EXPECT_DOUBLE_EQ(274.5, out->open);
		EXPECT_DOUBLE_EQ(277.0, out->high);
		EXPECT_DOUBLE_EQ(273.0, out->low);
		EXPECT_DOUBLE_EQ(275.0, out->settle_price);
		EXPECT_DOUBLE_EQ(375.0, out->upper_limit);
		EXPECT_DOUBLE_EQ(175.0, out->lower_limit);
		EXPECT_DOUBLE_EQ(1100.5, out->total_volume);
		EXPECT_DOUBLE_EQ(55.5, out->volume);
		EXPECT_DOUBLE_EQ(303050.0, out->total_turnover);
		EXPECT_DOUBLE_EQ(15152.5, out->turn_over);
		EXPECT_DOUBLE_EQ(2050.5, out->open_interest);
		EXPECT_DOUBLE_EQ(-3.5, out->diff_interest);
		EXPECT_EQ(20240601u, out->trading_date);
		EXPECT_EQ(20240601u, out->action_date);
		EXPECT_EQ(93002000u, out->action_time);
		EXPECT_DOUBLE_EQ(274.0, out->pre_close);
		EXPECT_DOUBLE_EQ(274.5, out->pre_settle);
		EXPECT_DOUBLE_EQ(1990.5, out->pre_interest);
		EXPECT_DOUBLE_EQ(275.0, out->bid_prices[0]);
		EXPECT_DOUBLE_EQ(276.0, out->ask_prices[0]);
		EXPECT_DOUBLE_EQ(12.5, out->bid_qty[0]);
		EXPECT_DOUBLE_EQ(13.5, out->ask_qty[0]);
		EXPECT_EQ(0u, out->reserve_);
		for (std::size_t level = 0; level < 10; ++level)
		{
			EXPECT_DOUBLE_EQ(275.0 + level, out->bid_prices[level]) << level;
			EXPECT_DOUBLE_EQ(276.0 + level, out->ask_prices[level]) << level;
			EXPECT_DOUBLE_EQ(12.5 + level, out->bid_qty[level]) << level;
			EXPECT_DOUBLE_EQ(13.5 + level, out->ask_qty[level]) << level;
		}
	}
}
