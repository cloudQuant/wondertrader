// r162 / WtDataStorageAD 读取端补充：
//  - WtDataReaderAD::update_cache_from_lmdb 的「LMDB 新增 bar 追加」分支 :292-297
//  - WtDataReaderAD::get_t_db 的缓存命中 :601-602
//  - WtRdmDtReaderAD::readTickSliceByRange 的起始 lower_bound 分支 :236-247
//  - WtRdmDtReaderAD::get_k_db / get_t_db 打开失败的返回 :423-426 / :447-450
//  - WtDataReaderAD::readTickSlice 的 tick db 打开失败返回 :143-145
// LMDB fixture 直接按 writer 使用的 key/value schema 构造，避免 reader 用例依赖
// WtDataWriterAD 未初始化的 _async_task 执行模式。
#include "r162_ad_fixture.h"

#include "WtDataStorageAD/LMDBKeys.h"
#include "WTSUtils/WtLMDB.hpp"

#include <sys/stat.h>

namespace r162ad {

namespace {

template <typename T>
struct ReleaseSlice
{
	void operator()(T* slice) const
	{
		if (slice != nullptr)
			slice->release();
	}
};

std::string bar_key(const char* exchg, const char* code, uint32_t bartime)
{
	LMDBBarKey key(exchg, code, bartime);
	return std::string((const char*)&key, sizeof(key));
}

std::string tick_key(const char* exchg, const char* code, uint32_t date, uint32_t time)
{
	LMDBHftKey key(exchg, code, date, time);
	return std::string((const char*)&key, sizeof(key));
}

WTSBarStruct make_bar(uint32_t date, uint32_t hhmm, double price)
{
	WTSBarStruct bar;
	bar.date = date;
	bar.time = (uint64_t)(date - 19900000u) * 10000u + hhmm;
	bar.open = price;
	bar.high = price + 1.0;
	bar.low = price - 1.0;
	bar.close = price + 0.5;
	bar.vol = 1.0;
	bar.money = price;
	return bar;
}

WTSTickStruct make_tick_struct(uint32_t date, uint32_t time, double price, double volume)
{
	WTSTickStruct tick;
	strcpy(tick.exchg, "SSE");
	strcpy(tick.code, "600000");
	tick.trading_date = date;
	tick.action_date = date;
	tick.action_time = time;
	tick.price = price;
	tick.open = price;
	tick.high = price;
	tick.low = price;
	tick.total_volume = volume;
	tick.volume = 1.0;
	tick.total_turnover = price * volume;
	tick.turn_over = price;
	return tick;
}

bool put_bar(const std::filesystem::path& dir, const WTSBarStruct& bar)
{
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	if (ec)
		return false;

	WtLMDB db(false);
	if (!db.open(dir.string().c_str(), 8 * 1024 * 1024))
		return false;
	WtLMDBQuery query(db);
	const std::string value((const char*)&bar, sizeof(bar));
	if (!query.put(bar_key("SSE", "600000", (uint32_t)bar.time), value))
		return false;
	query.commit();
	return !db.has_error();
}

bool seed_ticks(const std::filesystem::path& dir, const std::vector<WTSTickStruct>& ticks)
{
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	if (ec)
		return false;

	WtLMDB db(false);
	if (!db.open(dir.string().c_str(), 8 * 1024 * 1024))
		return false;
	WtLMDBQuery query(db);
	for (const WTSTickStruct& tick : ticks)
	{
		const std::string value((const char*)&tick, sizeof(tick));
		if (!query.put(tick_key("SSE", "600000", tick.trading_date, tick.action_time), value))
			return false;
	}
	query.commit();
	return !db.has_error();
}

} // namespace

class R162AdReaderTest : public AdFixture
{
};

// WtDataReaderAD.cpp:292-297, 601-602
TEST_F(R162AdReaderTest, R162ReaderADMinuteBarsAppendFromLmdb)
{
	const std::filesystem::path kline_dir = _dir / "min1" / "SSE";
	const std::filesystem::path tick_dir = _dir / "ticks" / "SSE" / "600000";
	const WTSBarStruct first_bar = make_bar(20240110u, 931u, 11.0);
	ASSERT_TRUE(put_bar(kline_dir, first_bar));
	ASSERT_TRUE(seed_ticks(tick_dir, std::vector<WTSTickStruct>{
		make_tick_struct(20240110u, 93000000u, 10.0, 1.0),
		make_tick_struct(20240110u, 93100000u, 11.0, 2.0),
		make_tick_struct(20240110u, 93200000u, 12.0, 3.0),
	}));

	WtDataReaderAD* reader = make_reader();
	ASSERT_TRUE(reader != NULL);
	std::unique_ptr<WtDataReaderAD> reader_guard(reader);

	uint64_t etime = (uint64_t)20240110 * 1000000000ULL + 93100000ULL;
	std::unique_ptr<WTSKlineSlice, ReleaseSlice<WTSKlineSlice>> first(
		reader->readKlineSlice("SSE.600000", KP_Minute1, 10, etime));
	ASSERT_TRUE(first != NULL);
	ASSERT_EQ(bar_count(first.get()), 1u);
	ASSERT_NE(first->at(0), nullptr);
	EXPECT_EQ(first->at(0)->time, first_bar.time);
	EXPECT_DOUBLE_EQ(first->at(0)->close, first_bar.close);

	// 09:31 首批数据已读入 reader cache 后，再直接追加 09:32 LMDB 记录。
	const WTSBarStruct second_bar = make_bar(20240110u, 932u, 12.0);
	ASSERT_TRUE(put_bar(kline_dir, second_bar));

	std::unique_ptr<WTSKlineSlice, ReleaseSlice<WTSKlineSlice>> second(
		reader->readKlineSlice("SSE.600000", KP_Minute1, 10,
			(uint64_t)20240110 * 1000000000ULL + 93200000ULL));
	ASSERT_TRUE(second != NULL);
	EXPECT_EQ(bar_count(second.get()), 2u);
	const WTSBarStruct* returned_first = second->at(0);
	ASSERT_NE(returned_first, nullptr);
	EXPECT_EQ(returned_first->time, first_bar.time);
	EXPECT_DOUBLE_EQ(returned_first->close, first_bar.close);
	if (bar_count(second.get()) >= 2)
	{
		const WTSBarStruct* returned_second = second->at(1);
		ASSERT_NE(returned_second, nullptr);
		EXPECT_EQ(returned_second->time, second_bar.time);
		EXPECT_DOUBLE_EQ(returned_second->close, second_bar.close);
	}
	EXPECT_EQ(_rsink->bars(), 1u);

	// 两次读 tick：第二次命中 get_t_db 的 _tick_dbs 缓存。
	std::unique_ptr<WTSTickSlice, ReleaseSlice<WTSTickSlice>> ticks1(
		reader->readTickSlice("SSE.600000", 10,
			(uint64_t)20240110 * 1000000000ULL + 93300000ULL));
	ASSERT_TRUE(ticks1 != NULL);
	EXPECT_EQ(tick_count(ticks1.get()), 3u);

	std::unique_ptr<WTSTickSlice, ReleaseSlice<WTSTickSlice>> ticks2(
		reader->readTickSlice("SSE.600000", 10,
			(uint64_t)20240110 * 1000000000ULL + 93300000ULL));
	ASSERT_TRUE(ticks2 != NULL);
	EXPECT_EQ(tick_count(ticks2.get()), 3u);
}

// WtDataReaderAD.cpp:292-297（日线周期同一追加分支）
TEST_F(R162AdReaderTest, R162ReaderADDayBarAppendsAfterTradingDayRollover)
{
	WtDataWriterAD* w = make_writer();
	ASSERT_TRUE(w != NULL);

	push_tick(w, 20240110, 93000000u, 20.0, 1.0);
	push_tick(w, 20240111, 93000000u, 21.0, 2.0);	// 收盘上一交易日 -> 落 20240110 日线

	WtDataReaderAD* reader = make_reader();
	ASSERT_TRUE(reader != NULL);

	WTSKlineSlice* first = reader->readKlineSlice("SSE.600000", KP_DAY, 10,
		(uint64_t)20240110 * 1000000000ULL);
	ASSERT_TRUE(first != NULL);
	EXPECT_EQ(bar_count(first), 1u);
	first->release();

	push_tick(w, 20240112, 93000000u, 22.0, 3.0);	// 落 20240111 日线

	WTSKlineSlice* second = reader->readKlineSlice("SSE.600000", KP_DAY, 10,
		(uint64_t)20240111 * 1000000000ULL);
	ASSERT_TRUE(second != NULL);
	EXPECT_EQ(bar_count(second), 2u);
	second->release();
	EXPECT_GE(_rsink->bars(), 1u);
}

// WtRdmDtReaderAD.cpp:236-247, 441
TEST_F(R162AdReaderTest, R162RdmReaderADTickRangeStartsAtFirstTickAndKlineCacheReused)
{
	WtDataWriterAD* w = make_writer();
	ASSERT_TRUE(w != NULL);

	push_tick(w, 20240110, 93000000u, 30.0, 1.0);
	push_tick(w, 20240110, 93100000u, 31.0, 2.0);

	WtRdmDtReaderAD* rdm = make_rdm_reader();
	ASSERT_TRUE(rdm != NULL);

	// stime 恰好等于第一条 tick 的时间 -> 走 else 分支里的第二次 lower_bound
	const uint64_t stime = (uint64_t)20240110 * 1000000000ULL + 93000000ULL;
	const uint64_t etime = (uint64_t)20240110 * 1000000000ULL + 93200000ULL;
	WTSTickSlice* ticks = rdm->readTickSliceByRange("SSE.600000", stime, etime);
	ASSERT_TRUE(ticks != NULL);
	EXPECT_EQ(tick_count(ticks), 2u);
	ticks->release();

	// 第一次读建立 min1 库连接，第二次读命中 _exchg_m1_dbs
	WTSKlineSlice* k1 = rdm->readKlineSliceByRange("SSE.600000", KP_Minute1,
		(uint64_t)20240110 * 10000ULL + 931ULL, (uint64_t)20240110 * 10000ULL + 932ULL);
	ASSERT_TRUE(k1 != NULL);
	EXPECT_EQ(bar_count(k1), 1u);
	k1->release();

	push_tick(w, 20240110, 93200000u, 32.0, 3.0);	// 落 09:32 分钟线

	WTSKlineSlice* k2 = rdm->readKlineSliceByRange("SSE.600000", KP_Minute1,
		(uint64_t)20240110 * 10000ULL + 931ULL, (uint64_t)20240110 * 10000ULL + 933ULL);
	ASSERT_TRUE(k2 != NULL);
	EXPECT_EQ(bar_count(k2), 2u);
	k2->release();
}

// WtDataReaderAD.cpp:143-145
TEST_F(R162AdReaderTest, R162ReaderADTickDbOpenFailureReturnsNull)
{
	const string dir = path_of("ticks/SSE/600000");
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	ASSERT_FALSE(ec) << ec.message();
	ASSERT_EQ(::chmod(dir.c_str(), 0000), 0);

	WtDataReaderAD* reader = make_reader();
	ASSERT_TRUE(reader != NULL);
	WTSTickSlice* slice = reader->readTickSlice("SSE.600000", 10,
		(uint64_t)20240110 * 1000000000ULL + 93200000ULL);

	ASSERT_EQ(::chmod(dir.c_str(), 0755), 0);
	EXPECT_TRUE(slice == NULL);
	if (slice != NULL)
		slice->release();
	EXPECT_GE(_rsink->count_of(LL_ERROR), 1u);
	EXPECT_NE(_rsink->last_log().find("Opening tick db of SSE.600000 failed"), string::npos);
}

// WtRdmDtReaderAD.cpp:447-450
TEST_F(R162AdReaderTest, R162RdmReaderADTickDbOpenFailureReturnsNull)
{
	const string dir = path_of("ticks/SSE/600000");
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	ASSERT_FALSE(ec) << ec.message();
	ASSERT_EQ(::chmod(dir.c_str(), 0000), 0);

	WtRdmDtReaderAD* rdm = make_rdm_reader();
	ASSERT_TRUE(rdm != NULL);
	WTSTickSlice* slice = rdm->readTickSliceByRange("SSE.600000",
		(uint64_t)20240110 * 1000000000ULL + 93000000ULL,
		(uint64_t)20240110 * 1000000000ULL + 93200000ULL);

	ASSERT_EQ(::chmod(dir.c_str(), 0755), 0);
	EXPECT_TRUE(slice == NULL);
	if (slice != NULL)
		slice->release();
	EXPECT_GE(_qsink->count_of(LL_ERROR), 1u);
	EXPECT_NE(_qsink->last_log().find("Opening tick db of SSE.600000 failed"), string::npos);
}

// WtRdmDtReaderAD.cpp:423-426
TEST_F(R162AdReaderTest, R162RdmReaderADKlineDbOpenFailureReturnsNull)
{
	const string dir = path_of("min1/SSE");
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	ASSERT_FALSE(ec) << ec.message();
	ASSERT_EQ(::chmod(dir.c_str(), 0000), 0);

	WtRdmDtReaderAD* rdm = make_rdm_reader();
	ASSERT_TRUE(rdm != NULL);
	WTSKlineSlice* slice = rdm->readKlineSliceByRange("SSE.600000", KP_Minute1,
		(uint64_t)20240110 * 10000ULL + 930ULL, (uint64_t)20240110 * 10000ULL + 932ULL);

	ASSERT_EQ(::chmod(dir.c_str(), 0755), 0);
	EXPECT_TRUE(slice == NULL);
	if (slice != NULL)
		slice->release();
	EXPECT_GE(_qsink->count_of(LL_ERROR), 1u);
	const string error_log = _qsink->last_log();
	EXPECT_NE(error_log.find("Opening min1 db "), string::npos);
	EXPECT_NE(error_log.find("SSE failed"), string::npos);
}

// WtDataWriterAD.cpp:649-652：同一交易日第二笔 tick 合并日线时 vol/money 被累加两次。
// 产品缺陷 BUG-R162-002，按纪律整条 DISABLED_，断言保留正确口径（1+1=2）。
TEST_F(R162AdReaderTest, DISABLED_R162WriterADMergedDayBarVolumeIsDoubleCounted)
{
	WtDataWriterAD* w = make_writer();
	ASSERT_TRUE(w != NULL);

	push_tick(w, 20240110, 93000000u, 10.0, 1.0);		// 新日线，vol=1
	push_tick(w, 20240110, 93100000u, 11.0, 1.0);		// 同日合并，正确应 vol=2，实际 3
	push_tick(w, 20240111, 93000000u, 12.0, 1.0);		// 触发 20240110 日线落库

	WtDataReaderAD* reader = make_reader();
	ASSERT_TRUE(reader != NULL);

	WTSKlineSlice* slice = reader->readKlineSlice("SSE.600000", KP_DAY, 10,
		(uint64_t)20240110 * 1000000000ULL);
	ASSERT_TRUE(slice != NULL);
	ASSERT_EQ(bar_count(slice), 1u);
	ASSERT_GE(slice->get_block_counts(), 1u);
	EXPECT_DOUBLE_EQ(slice->get_block_addr(0)[0].vol, 2.0);
	slice->release();
}

} // namespace r162ad
