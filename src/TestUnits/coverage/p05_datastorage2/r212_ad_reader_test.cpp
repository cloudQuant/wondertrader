// r212 / WtDataStorageAD 读端残余
//
// 覆盖（src/WtDataStorageAD/WtDataReaderAD.cpp）：
//  - readKlineSlice 发现 RT 缓存 bar 晚于 etime 后回源 LMDB、不再追加缓存 bar :444-456
// 覆盖（src/WtDataStorageAD/WtRdmDtReaderAD.cpp）：
//  - readTickSliceByRange 起点定位时跨 action_date 的 lower_bound 比较          :238-243
//
// 数据直接用 WtLMDB/WtLMDBQuery 落盘 + r4 夹具的块文件布局，不经过 WtDataWriterAD
// （其 _async_task 未初始化，见 bugs/R4_adstorage.md R4-BUG-01），因此没有异步线程。
#include "r4_ad_fixture.h"

#include "WtDataStorageAD/WtDataReaderAD.h"
#include "WtDataStorageAD/WtRdmDtReaderAD.h"

#include <memory>
#include <string>
#include <vector>

namespace r212ad {

using namespace r4fx;

namespace {

const uint32_t kDate = 20230103;
const uint32_t kExchg = 20240109;

inline std::string exchg_key(const char* exchg, const char* code, uint32_t date, uint32_t time)
{
	LMDBHftKey key(exchg, code, date, time);
	return std::string((const char*)&key, sizeof(key));
}

inline std::string bar_key(const char* exchg, const char* code, uint32_t tail)
{
	LMDBBarKey key(exchg, code, tail);
	return std::string((const char*)&key, sizeof(key));
}

inline uint32_t min_bar_time(uint32_t date, uint32_t hhmm)
{
	return (date - 19900000u) * 10000u + hhmm;
}

class R212ReaderSink : public IDataReaderSink
{
public:
	virtual ~R212ReaderSink() {}
	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return NULL; }
	void on_bar(const char*, WTSKlinePeriod, WTSBarStruct*) override { _bars++; }
	void on_all_bar_updated(uint32_t) override {}
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		_logs[ll]++;
		_total++;
		if (message != NULL)
			_all.push_back(message);
	}
	uint32_t get_date() override { return kDate; }
	uint32_t get_min_time() override { return 1500u; }
	uint32_t get_secs() override { return 0u; }

	uint32_t count_containing(const char* sub) const
	{
		uint32_t ret = 0;
		const std::string s = sub == NULL ? "" : sub;
		for (const std::string& item : _all)
		{
			if (item.find(s) != std::string::npos)
				ret++;
		}
		return ret;
	}
	uint32_t bars() const { return _bars; }

	explicit R212ReaderSink(IBaseDataMgr* bd) : _bd(bd) {}

private:
	IBaseDataMgr*	_bd;
	uint32_t		_bars = 0;
	uint32_t		_total = 0;
	std::vector<std::string>	_all;
	wt_hashmap<WTSLogLevel, uint32_t>	_logs;
};

class R212RdmSink : public IRdmDtReaderSink
{
public:
	virtual ~R212RdmSink() {}
	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return NULL; }
	void reader_log(WTSLogLevel ll, const char*) override { _logs[ll]++; }

	explicit R212RdmSink(IBaseDataMgr* bd) : _bd(bd) {}

private:
	IBaseDataMgr*	_bd;
	wt_hashmap<WTSLogLevel, uint32_t>	_logs;
};

} // namespace

class R212AdReaderTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		_seq = seq++;
		_root = std::filesystem::temp_directory_path()
			/ ("r212_ad_" + std::to_string((uint32_t)::getpid()) + "_" + std::to_string(_seq));
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
		std::filesystem::create_directories(_root, ec);
		ASSERT_FALSE(ec) << ec.message();

		_bd.reset(new R4BaseDataMgr());
		_sink.reset(new R212ReaderSink(_bd.get()));
		_comm = _bd->add_commodity("EXQ", "rb2301", ContractCategory::CC_Stock);
		_bd->add_contract("EXQ", "rb2301", _comm);
	}

	void TearDown() override
	{
		_rdm.reset();
		_reader.reset();
		_rsink.reset();
		_sink.reset();
		_bd.reset();
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	void init_reader()
	{
		_reader.reset(new WtDataReaderAD());
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", _root.string().c_str());
		_reader->init(cfg, _sink.get(), NULL);
		cfg->release();
	}

	void init_rdm()
	{
		_rdm.reset(new WtRdmDtReaderAD());
		_rsink.reset(new R212RdmSink(_bd.get()));
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", _root.string().c_str());
		_rdm->init(cfg, _rsink.get());
		cfg->release();
	}

	void seed_bars(const char* code, const std::vector<std::pair<uint32_t, WTSBarStruct>>& rows)
	{
		std::filesystem::create_directories(_root / "min1" / "EXQ");
		WtLMDB db(false);
		ASSERT_TRUE(db.open((_root / "min1" / "EXQ").string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for (const auto& row : rows)
			ASSERT_TRUE(query.put(bar_key("EXQ", code, row.first),
				std::string((const char*)&row.second, sizeof(WTSBarStruct))));
		query.commit();
	}

	void seed_ticks(const char* code, const std::vector<WTSTickStruct>& ticks)
	{
		std::filesystem::create_directories(_root / "ticks" / "EXQ" / code);
		WtLMDB db(false);
		ASSERT_TRUE(db.open((_root / "ticks" / "EXQ" / code).string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for (const WTSTickStruct& t : ticks)
			ASSERT_TRUE(query.put(exchg_key("EXQ", code, t.action_date, t.action_time),
				std::string((const char*)&t, sizeof(t))));
		query.commit();
	}

	static WTSBarStruct make_min_bar(uint32_t hhmm, double px)
	{
		WTSBarStruct bar;
		memset(&bar, 0, sizeof(bar));
		bar.date = kDate;
		bar.time = min_bar_time(kDate, hhmm);
		bar.open = px;
		bar.high = px + 1;
		bar.low = px - 1;
		bar.close = px + 0.5;
		bar.vol = 10;
		bar.money = px * 10;
		return bar;
	}

	std::filesystem::path	_root;
	uint32_t		_seq = 0;
	std::unique_ptr<R4BaseDataMgr>	_bd;
	std::unique_ptr<R212ReaderSink>	_sink;
	std::unique_ptr<WtDataReaderAD>	_reader;
	std::unique_ptr<WtRdmDtReaderAD>	_rdm;
	std::unique_ptr<R212RdmSink>	_rsink;
	WTSCommodityInfo*	_comm = NULL;
};

// WtDataReaderAD.cpp:444-456
// 产品缺陷（BUG-R212-001）：RT 缓存 bar 晚于 etime 时，444-455 会第二次调用
// update_cache_from_lmdb -> get_k_db。WtDataReaderAD::get_k_db 在缓存命中分支
// `return std::move(it->second)` 把 map 里的 shared_ptr 掏空，第二次命中返回空指针，
// update_cache_from_lmdb 随即 *db 解引用崩溃。故本用例保持 DISABLED_，不得以放宽断言换绿。
TEST_F(R212AdReaderTest, DISABLED_R212AdReaderRtBarAheadOfEtimeRefreshesLmdbOnly)
{
	seed_bars("rb2301", {
		{min_bar_time(kDate, 931), make_min_bar(931, 3900.0)},
		{min_bar_time(kDate, 932), make_min_bar(932, 3910.0)},
		{min_bar_time(kDate, 933), make_min_bar(933, 3920.0)}
		});

	// RT 缓存里是 09:40，晚于请求截止 09:35
	std::vector<BarCacheItem> items;
	items.push_back(make_bar_item("EXQ", "rb2301", kDate, min_bar_time(kDate, 940), 3930.0));
	write_block_file((_root / "cache_m1.dmb").string(), BLK_FLAG, BT_RT_Cache,
		1, 1, 1, bar_payload(items));

	init_reader();
	WTSKlineSlice* slice = _reader->readKlineSlice("EXQ.rb2301", KP_Minute1, 3,
		(uint64_t)20230103 * 1000000000ull + 93500000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->get_block_counts(), (std::size_t)1);
	ASSERT_EQ(slice->get_block_size(0), 3u);
	const WTSBarStruct* bars = slice->get_block_addr(0);
	EXPECT_EQ(bars[0].time, (uint64_t)min_bar_time(kDate, 931));
	EXPECT_EQ(bars[1].time, (uint64_t)min_bar_time(kDate, 932));
	EXPECT_EQ(bars[2].time, (uint64_t)min_bar_time(kDate, 933));
	slice->release();

	// 缓存 bar 晚于 etime：只回源 LMDB，不应把 09:40 追加进来
	EXPECT_EQ(_sink->count_containing("updated from cache instead of lmdb"), 0u);
}

// WtRdmDtReaderAD.cpp:238-243
TEST_F(R212AdReaderTest, R212AdRdmCrossDateTickRangeStartComparator)
{
	std::vector<WTSTickStruct> ticks;
	ticks.push_back(make_tick("EXQ", "rb2301", 20240109, 20240109, 100000000u,
		30.0, 1.0, 30.0, 1000.0, 1.0));
	ticks.push_back(make_tick("EXQ", "rb2301", 20240110, 20240110, 90100000u,
		31.0, 1.0, 31.0, 1001.0, 1.0));
	ticks.push_back(make_tick("EXQ", "rb2301", 20240110, 20240110, 90200000u,
		32.0, 1.0, 32.0, 1002.0, 1.0));
	seed_ticks("rb2301", ticks);

	init_rdm();
	WTSTickSlice* slice = _rdm->readTickSliceByRange("EXQ.rb2301",
		(uint64_t)kExchg * 1000000000ull + 100000000ull,
		(uint64_t)20240110 * 1000000000ull + 100000000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 3u);
	EXPECT_EQ(slice->at(0)->action_date, 20240109u);
	EXPECT_EQ(slice->at(1)->action_date, 20240110u);
	EXPECT_EQ(slice->at(1)->action_time, 90100000u);
	slice->release();
}

} // namespace r212ad
