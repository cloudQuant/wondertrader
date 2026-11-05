// 迭代03 卡3 3c=C（R4-BUG-13）：AD 存储日线 day.time=0、date 保留；cache_d1.dmb 版本标记 2；
// 旧版本 1 日线缓存与旧 LMDB day 记录（time=交易日）兼容读取。
// 真实产品 TU：WtDataWriterAD / WtDataReaderAD / WtRdmDtReaderAD / WtBtDtReaderAD；数据目录为临时目录。
#include "r100_ad_fixture.h"

#include "Includes/IBtDtReader.h"
#include "Includes/IRdmDtReader.h"
#include "WtDataStorageAD/WtBtDtReaderAD.h"
#include "WtDataStorageAD/WtRdmDtReaderAD.h"

using namespace r100fx;

namespace {

const uint32_t kTD = 20261105;

WTSBarStruct legacy_day_bar(uint32_t date, double px)
{
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	bar.date = date;
	bar.time = date;	// 旧 AD writer 写法：time = 交易日
	bar.open = px;
	bar.high = px + 1;
	bar.low = px - 1;
	bar.close = px + 0.5;
	bar.vol = 10;
	bar.money = px * 10;
	return bar;
}

class C3BtSink : public IBtDtReaderSink
{
public:
	void reader_log(WTSLogLevel, const char*) override {}
};

class C3RdmSink : public IRdmDtReaderSink
{
public:
	C3RdmSink(IBaseDataMgr* bd, IHotMgr* hot) : _bd(bd), _hot(hot) {}
	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return _hot; }
	void reader_log(WTSLogLevel, const char*) override {}
	IBaseDataMgr* _bd;
	IHotMgr* _hot;
};

class C3AdDayBar : public R100ADReaderTest
{
protected:
	void TearDown() override
	{
		if (_writer)
			_writer->release();
		_writer.reset();
		R100ADReaderTest::TearDown();
	}

	void init_writer()
	{
		_wsink.reset(new r4fx::R4WriterSink(_bd));
		WTSVariant* cfg = r4fx::make_writer_cfg(_root.string(), 1000, true, true, true, false);
		_writer.reset(new WtDataWriterAD());
		ASSERT_TRUE(_writer->init(cfg, _wsink.get()));
		cfg->release();
	}

	void write_tick(WTSContractInfo* ct, uint32_t tdate, uint32_t atime, double px)
	{
		WTSTickStruct s = r4fx::make_tick(ct->getExchg(), ct->getCode(), tdate, tdate, atime, px, 10, 100, 50, 1, 100, 1000);
		WTSTickData* t = WTSTickData::create(s);
		t->setContractInfo(ct);
		ASSERT_TRUE(_writer->writeTick(t, 0));
		t->release();
	}

	std::unique_ptr<r4fx::R4WriterSink> _wsink;
	std::unique_ptr<WtDataWriterAD> _writer;
};

} // namespace

// 新数据：日线缓存条目 time=0、date=交易日；新建 cache_d1.dmb 头版本 2；m1/m5/tick 缓存版本仍为 1
TEST_F(C3AdDayBar, NewDayBarWritesTimeZeroAndVersionMarker)
{
	WTSContractInfo* ct = add_future("SHFE", "rb", "rb2601");
	init_writer();
	write_tick(ct, kTD, 93000000, 3500.0);

	BarCacheItem d1;
	ASSERT_TRUE(r4fx::read_bar_item(path_of("cache_d1.dmb"), "SHFE", "rb2601", d1));
	EXPECT_EQ(d1._bar.date, kTD);
	EXPECT_EQ(d1._bar.time, 0u);
	EXPECT_EQ(r4fx::read_head(path_of("cache_d1.dmb"))._version, 2u);
	EXPECT_EQ(r4fx::read_head(path_of("cache_tick.dmb"))._version, 1u);
}

// 跨日落库：上一交易日日线以 time=0 写入 LMDB day 库
TEST_F(C3AdDayBar, CrossDayPipedDayBarHasTimeZero)
{
	WTSContractInfo* ct = add_future("SHFE", "rb", "rb2601");
	init_writer();
	write_tick(ct, kTD, 93000000, 3500.0);
	write_tick(ct, kTD + 1, 93000000, 3600.0);

	WtLMDB db(false);
	ASSERT_TRUE(db.open(path_of("day/SHFE").c_str(), 8 * 1024 * 1024));
	WtLMDBQuery q(db);
	LMDBBarKey key("SHFE", "rb2601", kTD);
	const std::string val = q.get(std::string((const char*)&key, sizeof(key)));
	ASSERT_EQ(val.size(), sizeof(WTSBarStruct));
	WTSBarStruct bar;
	memcpy(&bar, val.data(), sizeof(bar));
	EXPECT_EQ(bar.date, kTD);
	EXPECT_EQ(bar.time, 0u);
	EXPECT_DOUBLE_EQ(bar.open, 3500.0);
}

// 旧缓存迁移：版本 1、time=交易日的日线缓存在 writer 打开时迁移为 time=0 并标记版本 2，OHLCV 不变
TEST_F(C3AdDayBar, LegacyV1DayCacheMigratedOnWriterOpen)
{
	WTSContractInfo* ct = add_future("SHFE", "rb", "rb2601");
	(void)ct;
	vector<BarCacheItem> dc;
	dc.push_back(r4fx::make_bar_item("SHFE", "rb2601", kTD, kTD, 3500.0));
	write_cache("cache_d1.dmb", dc, 1, 4);
	ASSERT_EQ(r4fx::read_head(path_of("cache_d1.dmb"))._version, 1u);

	init_writer();

	BarCacheItem d1;
	ASSERT_TRUE(r4fx::read_bar_item(path_of("cache_d1.dmb"), "SHFE", "rb2601", d1));
	EXPECT_EQ(d1._bar.date, kTD);
	EXPECT_EQ(d1._bar.time, 0u);
	EXPECT_DOUBLE_EQ(d1._bar.open, 3500.0);
	EXPECT_EQ(r4fx::read_head(path_of("cache_d1.dmb"))._version, 2u);
}

// fail-closed 正控：未知版本（3）的日线缓存仍被拒绝，不改写
TEST_F(C3AdDayBar, UnknownDayCacheVersionStillRejected)
{
	vector<BarCacheItem> slots(4);
	slots[0] = r4fx::make_bar_item("SHFE", "rb2601", kTD, kTD, 3500.0);
	r4fx::write_block_file(path_of("cache_d1.dmb"), BLK_FLAG, BT_RT_Cache, 3, 1, 4, r4fx::bar_payload(slots));
	_wsink.reset(new r4fx::R4WriterSink(_bd));
	WTSVariant* cfg = r4fx::make_writer_cfg(_root.string(), 1000, true, true, true, false);
	_writer.reset(new WtDataWriterAD());
	EXPECT_FALSE(_writer->init(cfg, _wsink.get()));
	cfg->release();
	EXPECT_EQ(r4fx::read_head(path_of("cache_d1.dmb"))._version, 3u);
}

// 读端兼容：WtDataReaderAD 接受版本 2 日线缓存，并把旧 LMDB day 记录 time 归一为 0
TEST_F(C3AdDayBar, ReaderAcceptsV2CacheAndNormalizesLegacyLmdbDay)
{
	add_stock("EXDD", "rb2301");
	vector<WTSBarStruct> d1;
	d1.push_back(legacy_day_bar(20230102, 4000.0));
	seed_bars("day", "EXDD", "rb2301", d1, true);

	vector<BarCacheItem> slots(4);
	slots[0] = r4fx::make_bar_item("EXDD", "rb2301", 20230103, 0, 4040.0);
	r4fx::write_block_file(path_of("cache_d1.dmb"), BLK_FLAG, BT_RT_Cache, 2, 1, 4, r4fx::bar_payload(slots));

	init_reader();
	WTSKlineSlice* dk = _reader->readKlineSlice("EXDD.rb2301", KP_DAY, 2, 20230102100000000ull);
	ASSERT_TRUE(dk != NULL);
	ASSERT_EQ(dk->size(), 1u);
	EXPECT_EQ(dk->get_block_addr(0)[0].date, 20230102u);
	EXPECT_EQ(dk->get_block_addr(0)[0].time, 0u);
	dk->release();

	_reader->onMinuteEnd(20230103, 1000, 20230103);
	EXPECT_EQ(_sink->bar_calls(), 1u);
	EXPECT_EQ(_sink->last_bar().date, 20230103u);
	EXPECT_EQ(_sink->last_bar().time, 0u);
	EXPECT_DOUBLE_EQ(_sink->last_bar().close, 4040.0);
}

// 读端兼容：版本 1 旧日线缓存（time=交易日）只读读取时 time 归一为 0
TEST_F(C3AdDayBar, ReaderNormalizesLegacyV1CacheBar)
{
	add_stock("EXDD", "rb2301");
	vector<WTSBarStruct> d1;
	d1.push_back(legacy_day_bar(20230102, 4000.0));
	seed_bars("day", "EXDD", "rb2301", d1, true);
	vector<BarCacheItem> dc;
	dc.push_back(r4fx::make_bar_item("EXDD", "rb2301", 20230103, 20230103, 4040.0));
	write_cache("cache_d1.dmb", dc, 1, 4);

	init_reader();
	WTSKlineSlice* dk = _reader->readKlineSlice("EXDD.rb2301", KP_DAY, 2, 20230102100000000ull);
	ASSERT_TRUE(dk != NULL);
	dk->release();
	_reader->onMinuteEnd(20230103, 1000, 20230103);
	ASSERT_EQ(_sink->bar_calls(), 1u);
	EXPECT_EQ(_sink->last_bar().date, 20230103u);
	EXPECT_EQ(_sink->last_bar().time, 0u);
	// 只读读取不改写旧缓存文件
	EXPECT_EQ(r4fx::read_head(path_of("cache_d1.dmb"))._version, 1u);
}

// 回测读端：WtBtDtReaderAD 旧 day 记录 time 归一为 0；分钟线 time 不变
TEST_F(C3AdDayBar, BtReaderNormalizesLegacyDayButNotMinute)
{
	vector<WTSBarStruct> d1;
	d1.push_back(legacy_day_bar(20230102, 4000.0));
	d1.push_back(legacy_day_bar(20230103, 4010.0));
	seed_bars("day", "EXDD", "rb2301", d1, true);
	WTSBarStruct m1 = legacy_day_bar(20230103, 4010.0);
	m1.time = 3301030931ull; // (date-19900000)*10000+HHMM
	vector<WTSBarStruct> mins(1, m1);
	seed_bars("min1", "EXDD", "rb2301", mins, false);

	C3BtSink sink;
	WtBtDtReaderAD reader;
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", _root.string().c_str());
	reader.init(cfg, &sink);
	cfg->release();

	std::string buf;
	ASSERT_TRUE(reader.read_raw_bars("EXDD", "rb2301", KP_DAY, buf));
	ASSERT_EQ(buf.size(), 2 * sizeof(WTSBarStruct));
	const WTSBarStruct* out = (const WTSBarStruct*)buf.data();
	EXPECT_EQ(out[0].date, 20230102u);
	EXPECT_EQ(out[0].time, 0u);
	EXPECT_EQ(out[1].time, 0u);
	EXPECT_DOUBLE_EQ(out[1].open, 4010.0);

	std::string mbuf;
	ASSERT_TRUE(reader.read_raw_bars("EXDD", "rb2301", KP_Minute1, mbuf));
	ASSERT_EQ(mbuf.size(), sizeof(WTSBarStruct));
	EXPECT_EQ(((const WTSBarStruct*)mbuf.data())[0].time, m1.time);
}

// 随机读端：WtRdmDtReaderAD 旧 day 记录 time 归一为 0
TEST_F(C3AdDayBar, RdmReaderNormalizesLegacyDay)
{
	add_stock("EXDD", "rb2301");
	vector<WTSBarStruct> d1;
	d1.push_back(legacy_day_bar(20230102, 4000.0));
	d1.push_back(legacy_day_bar(20230103, 4010.0));
	seed_bars("day", "EXDD", "rb2301", d1, true);

	C3RdmSink sink(_bd, _hot);
	WtRdmDtReaderAD reader;
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", _root.string().c_str());
	reader.init(cfg, &sink);
	cfg->release();

	WTSKlineSlice* s = reader.readKlineSliceByCount("EXDD.rb2301", KP_DAY, 5, 202301031500ull);
	ASSERT_TRUE(s != NULL);
	ASSERT_EQ(s->size(), 2u);
	EXPECT_EQ(s->at(0)->date, 20230102u);
	EXPECT_EQ(s->at(0)->time, 0u);
	EXPECT_EQ(s->at(1)->time, 0u);
	s->release();
}
