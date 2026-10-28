// r100 / WtDataWriterAD：已有 K 线缓存重载/扩容、init 建目录、procFlag=2 零值补齐、
// 日线滚动时的扩展 dumper（含 NULL 项）
//
// 覆盖（src/WtDataStorageAD/WtDataWriterAD.cpp）：
//  - init 目录不存在 -> create_directories      :79-81
//  - loadCache 已有 m5/d1 缓存 -> 重建索引       :230-238 / :271-279
//  - updateBarCache 的 d1/m1/m5 缓存扩容         :580-584 / :669-673 / :772-776
//  - updateTickCache 新交易日 procFlag==2 的零值补齐 :919-942
//  - pipeToDayBars 的 dumper 循环（NULL continue） :468-480
#include "r4_ad_fixture.h"

namespace r100fx {

// 独立 suite 名：R4WriterADTest 已被 r4_*.cpp 使用，跨 namespace 同名 suite
// 在 gtest 运行期仍会被判成「同一 suite 不同 fixture」而整族拒绝。
class R100WriterADTest : public R4WriterADTest
{
};

namespace {

class RecDumper : public IHisDataDumper
{
public:
	virtual ~RecDumper() {}

	bool dumpHisBars(const char* stdCode, const char* period, WTSBarStruct* bars, uint32_t count) override
	{
		_bars++;
		_last_code = stdCode == NULL ? "" : stdCode;
		_periods[period] += 1;
		if (bars != NULL && count > 0)
			_last_bar = *bars;
		return true;
	}

	bool dumpHisTicks(const char*, uint32_t, WTSTickStruct*, uint32_t) override { return true; }

	uint32_t bars() const { return _bars; }
	uint32_t period_count(const char* period) const
	{
		auto it = _periods.find(period);
		return it == _periods.end() ? 0u : it->second;
	}
	const std::string& last_code() const { return _last_code; }
	const WTSBarStruct& last_bar() const { return _last_bar; }

private:
	uint32_t	_bars = 0;
	std::string	_last_code;
	WTSBarStruct	_last_bar;
	wt_hashmap<std::string, uint32_t> _periods;
};

} // namespace

// WtDataWriterAD.cpp: L79-81 / L230-238 / L271-279 / L580-584 / L669-673 / L772-776
TEST_F(R100WriterADTest, R100WriterADReloadCachesThenResizeAllPeriods)
{
	WTSCommodityInfo* comm = _bd->add_commodity("EXC1", "rb", ContractCategory::CC_Future);
	_bd->add_contract("EXC1", "rb01", comm);
	_bd->add_contract("EXC1", "rb02", comm);
	_bd->add_contract("EXC1", "rb03", comm);

	const std::string base = _root.string() + "/nested/cache_root";
	ASSERT_FALSE(std::filesystem::exists(base));

	// 手写三个已有缓存块：capacity=2、size=2，先放 rb01/rb02
	vector<BarCacheItem> items;
	items.push_back(make_bar_item("EXC1", "rb01", 20230103, 3301030930, 3900.0));
	items.push_back(make_bar_item("EXC1", "rb02", 20230103, 3301030930, 3900.0));
	write_block_file(base + "/cache_m1.dmb", BLK_FLAG, BT_RT_Cache, 1, 2, 2, bar_payload(items));
	write_block_file(base + "/cache_m5.dmb", BLK_FLAG, BT_RT_Cache, 1, 2, 2, bar_payload(items));
	write_block_file(base + "/cache_d1.dmb", BLK_FLAG, BT_RT_Cache, 1, 2, 2, bar_payload(items));

	WTSVariant* cfg = make_writer_cfg(base, 1000, true, false, false, false);
	init_writer(cfg);
	EXPECT_TRUE(std::filesystem::exists(base));

	// 新合约 rb03 进入三个缓存 -> size 3 >= capacity 2，各扩容 400
	WTSTickData* t = tick_of(_bd->getContract("rb03", "EXC1"),
		make_tick("EXC1", "rb03", 20230103, 20230103, 93100000, 3900.0, 1, 10, 5, 0, 100, 1000));
	write_or_fail(t, 0);

	R4BlkHead m1 = read_head(base + "/cache_m1.dmb");
	R4BlkHead m5 = read_head(base + "/cache_m5.dmb");
	R4BlkHead d1 = read_head(base + "/cache_d1.dmb");
	EXPECT_EQ(m1._capacity, 402u);
	EXPECT_EQ(m5._capacity, 402u);
	EXPECT_EQ(d1._capacity, 402u);
	EXPECT_EQ(m1._size, 3u);
	EXPECT_EQ(m5._size, 3u);
	EXPECT_EQ(d1._size, 3u);

	EXPECT_EQ(_sink->count_containing("m1 cache resized to 402 items"), 1u);
	EXPECT_EQ(_sink->count_containing("m5 cache resized to 402 items"), 1u);
	EXPECT_EQ(_sink->count_containing("day cache resized to 402 items"), 1u);

	// 扩容后旧索引与旧条目仍然有效
	BarCacheItem old;
	ASSERT_TRUE(read_bar_item(base + "/cache_m1.dmb", "EXC1", "rb01", old));
	EXPECT_EQ(old._bar.close, 3900.0);
}

// WtDataWriterAD.cpp: L79-81 / L468-480 / L919-942
TEST_F(R100WriterADTest, R100WriterADProcFlag2ZeroFillAndDayDumper)
{
	WTSCommodityInfo* comm = _bd->add_commodity("EXC2", "rb", ContractCategory::CC_Future);
	_bd->add_contract("EXC2", "rb01", comm);
	_bd->add_contract("EXC2", "rb02", comm);

	const std::string base = _root.string() + "/fresh/dir";
	WTSVariant* cfg = make_writer_cfg(base, 1000, true, true, true, false);
	init_writer(cfg);
	EXPECT_TRUE(std::filesystem::exists(base));

	RecDumper rec;
	_writer->add_ext_dumper("rec", &rec);
	_writer->add_ext_dumper("null", NULL);

	// 同一合约跨交易日 -> 旧日线出管，dumper 循环里 NULL 项被跳过
	WTSTickData* t1 = tick_of(_bd->getContract("rb01", "EXC2"),
		make_tick("EXC2", "rb01", 20230103, 20230103, 93100000, 3900.0, 1, 10, 5, 0, 100, 1000));
	write_or_fail(t1, 0);
	WTSTickData* t2 = tick_of(_bd->getContract("rb01", "EXC2"),
		make_tick("EXC2", "rb01", 20230104, 20230104, 93100000, 3910.0, 1, 10, 6, 0, 101, 1010));
	write_or_fail(t2, 0);

	EXPECT_EQ(rec.period_count("d1"), 1u);
	EXPECT_EQ(rec.last_code(), std::string("EXC2.rb01"));
	EXPECT_EQ(rec.last_bar().date, 20230103u);

	// procFlag=2 的新交易日首笔：总量/总额/开高低全 0，用本笔数据补齐
	WTSTickStruct zero = make_tick("EXC2", "rb02", 20230105, 20230105, 93100000, 3920.0,
		5, 50, 7, 0, 0, 0);
	zero.open = 0;
	zero.high = 0;
	zero.low = 0;
	zero.total_volume = 0;
	zero.total_turnover = 0;
	WTSTickData* t3 = tick_of(_bd->getContract("rb02", "EXC2"), zero);
	write_or_fail(t3, 2);

	BarCacheItem item;
	ASSERT_TRUE(read_bar_item(path_of("cache_d1.dmb"), "EXC2", "rb02", item));
	EXPECT_EQ(item._bar.date, 20230105u);
	EXPECT_DOUBLE_EQ(item._bar.close, 3920.0);
	EXPECT_DOUBLE_EQ(item._bar.open, 3920.0);
	EXPECT_DOUBLE_EQ(item._bar.high, 3920.0);
	EXPECT_DOUBLE_EQ(item._bar.low, 3920.0);

	WTSTickData* cur = _writer->getCurTick("rb02", "EXC2");
	ASSERT_TRUE(cur != NULL);
	EXPECT_DOUBLE_EQ(cur->totalvolume(), 5.0);
	EXPECT_DOUBLE_EQ(cur->open(), 3920.0);
	EXPECT_DOUBLE_EQ(cur->high(), 3920.0);
	EXPECT_DOUBLE_EQ(cur->low(), 3920.0);
	cur->release();
}

} // namespace r100fx
