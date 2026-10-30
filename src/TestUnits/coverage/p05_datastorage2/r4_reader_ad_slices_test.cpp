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

namespace {

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
	uint32_t bar_calls() const { return _bar_calls; }
	uint32_t all_updated() const { return _all_updated; }
	uint32_t last_update_time() const { return _last_update_time; }
	const WTSBarStruct& last_bar() const { return _last_bar; }
	const std::string& last_bar_code() const { return _last_code; }
	WTSKlinePeriod last_period() const { return _last_period; }
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
	std::vector<std::string> _all;
	wt_hashmap<WTSLogLevel, uint32_t> _logs;
	wt_hashmap<WTSLogLevel, std::string> _last;
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
// 2. tick 首次读取走增量分支（reload_flag 被 L132-137 覆盖回 1）
//    全量 get_lowers 的 count 限制因此失效：区间内所有 tick 都被读进循环缓冲区，
//    最终只保留最后 count 条。见 R4-BUG-06。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, FirstTickReadTakesIncrementalRangeAndKeepsLastCount)
{
	add_stock("EXR1", "rb2301");
	seed_ticks("EXR1", "rb2301", {
		{R4_RD1, tick_at("EXR1", "rb2301", 93000000, 3900.0)},
		{R4_RD1, tick_at("EXR1", "rb2301", 93100000, 3910.0)},
		{R4_RD1, tick_at("EXR1", "rb2301", 93200000, 3920.0)}
		});
	init_reader();

	// etime = 20230103 10:00:00.000
	// 区间 [date0/time1, 20230103/100000000] 命中全部 3 笔 → 缓冲区容量 2 → 保留后 2 笔，
	// 并且因为回绕，切片要拆成两个连续块
	WTSTickSlice* slice = _reader->readTickSlice("EXR1.rb2301", 2, 20230103100000000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->get_block_counts(), (std::size_t)2);
	ASSERT_EQ(slice->get_block_size(0), 1u);
	EXPECT_DOUBLE_EQ(slice->get_block_addr(0)[0].price, 3910.0);
	EXPECT_EQ(slice->get_block_addr(0)[0].action_time, 93100000u);
	ASSERT_EQ(slice->get_block_size(1), 1u);
	EXPECT_DOUBLE_EQ(slice->get_block_addr(1)[0].price, 3920.0);
	slice->release();

	// tick 库缓存键写成了 exchg（L617），查的是 exchg.code（L599）→ 每次都重开环境。
	// 断言面（R32 修正）：本次读取的 DEBUG 有两条 —— 先"Tick db ... opened"（L614），
	// 后"3 ticks after ... append to cache"（L163，reload_flag 被 :135 覆盖成 1 的
	// R4-BUG-06 增量分支）；last_log 只剩后一条，所以逐条数走 count_containing。
	EXPECT_EQ(_sink->count_containing("Tick db of EXR1.rb2301 opened"), 1u);
	// 首次读取没有走 get_lowers 全量分支（L181 的 "loaded to cache for the first
	// time" 一条都没出现），而是走了增量分支 —— R4-BUG-06 的直接现象
	EXPECT_EQ(_sink->count_containing("append to cache"), 1u);
	EXPECT_EQ(_sink->count_containing("loaded to cache for the first time"), 0u);
}

//////////////////////////////////////////////////////////////////////////
// 3. 同一 etime 复读和时间推进均返回预期 tick 窗口
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
// 4. 空库读取：0 命中，切片区容量为 count 且块大小为 0
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
	const uint32_t badKeyTime = R4_M1_932 + 256u;
	const uint32_t laterKey1 = badKeyTime + 256u;
	const uint32_t laterKey2 = laterKey1 + 1u;
	add_stock(exchg, "rb2301");
	WTSBarStruct pending = bar_at("rb2301", R4_M1_933, 3920.0);
	WTSBarStruct badKeyValue = bar_at("rb2301", badKeyTime, 3930.0);
	WTSBarStruct cached1 = bar_at("rb2301", R4_M1_931, 3900.0);
	WTSBarStruct cached2 = bar_at("rb2301", R4_M1_932, 3910.0);
	// 反转端序后的时间键是字节序有序的：badKeyTime 的高 3 字节比游标
	// R4_M1_932 大，因此即使短 key 少了末字节，MDB_SET_RANGE 仍会返回它；
	// 后两根 key 更大，使首读只装载它们，随后增量查询确实经过坏 key。
	seed_raw_bar_values("min1", exchg, {
		{bar_key_str(exchg, "rb2301", R4_M1_932), std::string((const char*)&pending, sizeof(pending))},
		{raw_bar_key_str(exchg, "rb2301", badKeyTime, keyLength),
			std::string((const char*)&badKeyValue, sizeof(badKeyValue))},
		{bar_key_str(exchg, "rb2301", laterKey1), std::string((const char*)&cached1, sizeof(cached1))},
		{bar_key_str(exchg, "rb2301", laterKey2), std::string((const char*)&cached2, sizeof(cached2))}
		});
	init_reader();

	EXPECT_EQ(_reader->readKlineSlice(stdCode.c_str(), KP_Minute1, 2,
		20230103100000000ull), nullptr);
	EXPECT_NE(_sink->last_log(LL_ERROR).find("Malformed bar key/value in LMDB while updating"), std::string::npos);
	EXPECT_EQ(_sink->bar_calls(), 0u);

	// 以末 bar 时间为截止值重读，公开切片仍只能看到最初装入的两根缓存数据。
	WTSKlineSlice* cached = _reader->readKlineSlice(stdCode.c_str(), KP_Minute1, 2,
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

	// 再次请求同一窗口可从未污染缓存返回；更新错误依旧被隔离。
	WTSKlineSlice* retry = _reader->readKlineSlice(stdCode.c_str(), KP_Minute1, 1,
		20230103093200000ull);
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
		{R4_M1_931, bar_at("rb2301", R4_M1_931, 3900.0)},
		{R4_M1_932, bar_at("rb2301", R4_M1_932, 3910.0)},
		{R4_M1_933, bar_at("rb2301", R4_M1_933, 3920.0)},
		{R4_M1_940, bar_at("rb2301", R4_M1_940, 3930.0)}
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

	// 日线的 etime 只用自然日：20230103
	WTSKlineSlice* slice = _reader->readKlineSlice("EXD9.rb2301", KP_DAY, 2, 20230103100000000ull);
	ASSERT_TRUE(slice != NULL);
	ASSERT_EQ(slice->get_block_size(0), 2u);
	const WTSBarStruct* b = slice->get_block_addr(0);
	EXPECT_EQ(b[0].date, R4_RD1);
	EXPECT_DOUBLE_EQ(b[0].open, 3900.0);
	EXPECT_EQ(b[1].date, 20230104u);
	slice->release();
	EXPECT_TRUE(std::filesystem::exists(_root / "day" / "EXD9" / "data.mdb"));
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
	// 0 命中不产生 append 日志，最后一条 DEBUG 只会是"重开环境"
	EXPECT_EQ(_sink->last_log(LL_DEBUG).find("append to cache"), std::string::npos)
		<< _sink->last_log(LL_DEBUG);
	EXPECT_NE(_sink->last_log(LL_DEBUG).find("Tick db of EXC0.rb2301 opened"), std::string::npos);
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

}  // namespace
