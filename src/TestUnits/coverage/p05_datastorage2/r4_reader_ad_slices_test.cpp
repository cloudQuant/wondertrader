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
// 3. 同一 etime 二次读取不再查库；换交易所/合约后一次读取又重开环境
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, RepeatReadAtSameEtimeSkipsQueryButReopensDb)
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

	// 第 2 次：容量够、etime 未前进 → reload_flag 保持 0，不做任何区间查询
	const uint32_t dbg_after_first = _sink->log_count(LL_DEBUG);
	WTSTickSlice* s2 = _reader->readTickSlice("EXR2.rb2301", 2, 20230103093130000ull);
	ASSERT_TRUE(s2 != NULL);
	ASSERT_EQ(s2->get_block_size(0), 2u);
	s2->release();
	// 唯一新增的 DEBUG 只能是"重开环境"，而不是"append to cache"
	EXPECT_GT(_sink->log_count(LL_DEBUG), dbg_after_first);
	EXPECT_EQ(_sink->last_log(LL_DEBUG).find("append to cache"), std::string::npos)
		<< _sink->last_log(LL_DEBUG);
	EXPECT_NE(_sink->last_log(LL_DEBUG).find("Tick db of EXR2.rb2301 opened"), std::string::npos);

	// 第 3 次：etime 前进 1 分钟 → 增量窗口只命中 932 那一笔 → 缓冲区挤出 3900 并换成回绕两段
	WTSTickSlice* s3 = _reader->readTickSlice("EXR2.rb2301", 2, 20230103093230000ull);
	ASSERT_TRUE(s3 != NULL);
	EXPECT_NE(_sink->last_log(LL_DEBUG).find("1 ticks after"), std::string::npos)
		<< _sink->last_log(LL_DEBUG);
	// 缓冲区已回绕：array_two 有 1 条，必须靠 appendBlock 拼出第二个块
	EXPECT_EQ(s3->get_block_counts(), (std::size_t)2);
	ASSERT_EQ(s3->get_block_size(0), 1u);
	EXPECT_DOUBLE_EQ(s3->get_block_addr(0)[0].price, 3910.0);
	ASSERT_EQ(s3->get_block_size(1), 1u);
	EXPECT_DOUBLE_EQ(s3->get_block_addr(1)[0].price, 3920.0);
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
// 6. 同一 etime 复读：不再回源；此时交易所级句柄缓存已被掏空，
//    换合约读取会拿到空指针 → cacheBarsFromStorage 返回 false → 切片为 NULL
//    （get_k_db 的 move 语义，R4-BUG-02 的读取侧表现）
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, SecondCodeOnSameExchangeFailsAfterCacheHandleMovedOut)
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
	ASSERT_EQ(s1->get_block_size(0), 2u);
	s1->release();

	// rb2302 第一次读：min1/EXK2 的缓存项已被上一次 move 掏空 → 拿不到库
	WTSKlineSlice* s2 = _reader->readKlineSlice("EXK2.rb2302", KP_Minute1, 1, etime);
	EXPECT_TRUE(s2 == NULL);

	// 同一合约再读（etime 不变 → 不触发 update_cache_from_lmdb）仍可复用上次的缓存
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
// 14. 登记：onMinuteEnd 会 back() 空的循环缓冲区
//     触发条件：先让某合约进入 _bars_cache 但一条 bar 都没读到（例如回源失败），
//     再调 onMinuteEnd → L511 barsList._bars.back() 在空缓冲区上是未定义行为。
//     见 bugs/R4_adstorage.md R4-BUG-08。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4ReaderADTest, DISABLED_OnMinuteEndOnBarlessCacheEntryReadsBackOfEmptyBuffer)
{
	add_stock("EXX2", "rb2301");
	init_reader();
	// min1 库不存在 → cacheBarsFromStorage 打开空库、0 条命中 → _bars_cache 有条目但为空
	_reader->readKlineSlice("EXX2.rb2301", KP_Minute1, 2, 20230103100000000ull);
	_reader->onMinuteEnd(R4_RD1, 1000, 0);
	FAIL() << "启用即在空 circular_buffer 上取 back()";
}

}  // namespace
