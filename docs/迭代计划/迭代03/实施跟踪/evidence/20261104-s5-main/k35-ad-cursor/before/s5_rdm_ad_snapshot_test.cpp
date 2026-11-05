// 迭代03 s5 storage-read / WtDataStorageAD：WtRdmDtReaderAD 读端。
//
//  - R21-BUG-07（可确定部分）：同窗口复读在 LMDB 环境没有任何新提交时不再全量重扫；
//    有提交（含同键修订）时仍全量重扫，修订可见性与改动前一致。
//  - BUG-R100-003：readKlineSliceByRange 的分钟边界按 64 位计算，2033 年的右端点不回绕。
//  - BUG-L04-004：各入口在建目录/开库前拒绝不可表示身份并给出 ERROR 诊断。
#include "r21_ad_fixture.h"

namespace {

const char* const S5_EXCHG = "XEX";
const char* const S5_CODE = "rb2409";
const char* const S5_STD = "XEX.rb.rb2409";

std::vector<WTSBarStruct> s5_day_bars(double base)
{
	std::vector<WTSBarStruct> bars;
	for (uint32_t i = 0; i < 4; i++)
	{
		const uint32_t dt = 20240102 + i;
		bars.push_back(r21_make_bar(dt, 0, base + i));
	}
	return bars;
}

} // namespace

TEST_F(R21ReaderADTest, S5SameWindowWithoutCommitReusesSnapshot)
{
	init_reader();
	put_bar_db("day", S5_EXCHG, S5_CODE, s5_day_bars(100.0), true);

	for (int round = 0; round < 3; ++round)
	{
		WTSKlineSlice* slice = reader()->readKlineSliceByRange(S5_STD, KP_DAY, 202401020930ull, 202401041500ull);
		ASSERT_TRUE(slice != NULL);
		ASSERT_EQ(slice->get_block_size(0), 3u);
		EXPECT_NEAR(slice->get_block_addr(0)[2].close, 103.0, 1e-9);
		slice->release();
	}
	// Three identical reads of an unchanged environment scan LMDB once.
	EXPECT_EQ(sink()->count_containing("Reading back"), 1u);
}

TEST_F(R21ReaderADTest, S5CommitAfterSnapshotKeepsFullRescanAndRevisionVisibility)
{
	init_reader();
	put_bar_db("day", S5_EXCHG, S5_CODE, s5_day_bars(100.0), true);
	WTSKlineSlice* first = reader()->readKlineSliceByRange(S5_STD, KP_DAY, 202401020930ull, 202401051500ull);
	ASSERT_TRUE(first != NULL);
	EXPECT_NEAR(first->get_block_addr(0)[0].close, 101.0, 1e-9);

	// Same-key revision of the OLDEST bar (before any incremental cursor would start).
	std::vector<WTSBarStruct> revised{ r21_make_bar(20240102, 0, 500.0) };
	put_bar_db("day", S5_EXCHG, S5_CODE, revised, true);
	WTSKlineSlice* second = reader()->readKlineSliceByRange(S5_STD, KP_DAY, 202401020930ull, 202401051500ull);
	ASSERT_TRUE(second != NULL);
	EXPECT_NEAR(second->get_block_addr(0)[0].close, 501.0, 1e-9) << "same-key revision must stay visible";
	// The slice handed out earlier keeps its own snapshot (card 11 shared owner).
	EXPECT_NEAR(first->get_block_addr(0)[0].close, 101.0, 1e-9);
	first->release();
	second->release();

	// A new bar committed later is visible as well.
	std::vector<WTSBarStruct> extra{ r21_make_bar(20240108, 0, 200.0) };
	put_bar_db("day", S5_EXCHG, S5_CODE, extra, true);
	WTSKlineSlice* third = reader()->readKlineSliceByRange(S5_STD, KP_DAY, 202401020930ull, 202401081500ull);
	ASSERT_TRUE(third != NULL);
	EXPECT_EQ(third->get_block_size(0), 5u);
	EXPECT_EQ(third->get_block_addr(0)[4].date, 20240108u);
	third->release();
	EXPECT_EQ(sink()->count_containing("Reading back"), 3u);
}

TEST_F(R21ReaderADTest, S5MinuteRangeEndingIn2033DoesNotWrap)
{
	init_reader();
	std::vector<WTSBarStruct> bars;
	bars.push_back(r21_make_bar(20321231, r21_min_bar_time(20321231, 930), 10.0));
	bars.push_back(r21_make_bar(20321231, r21_min_bar_time(20321231, 1459), 11.0));
	put_bar_db("min1", S5_EXCHG, S5_CODE, bars, false);

	WTSKlineSlice* slice = reader()->readKlineSliceByRange(S5_STD, KP_Minute1, 203212310900ull, 203301041500ull);
	ASSERT_TRUE(slice != NULL) << "a 2033 right endpoint wrapped below the 2032 bars";
	ASSERT_EQ(slice->get_block_size(0), 2u);
	EXPECT_EQ(slice->get_block_addr(0)[1].time, r21_min_bar_time(20321231, 1459));
	slice->release();
}

TEST_F(R21ReaderADTest, S5OverlongIdentityRejectedBeforeOpeningDatabases)
{
	init_reader();
	const std::string longEx(MAX_EXCHANGE_LENGTH, 'X');
	const std::string stdCode = longEx + ".rb.rb2409";
	EXPECT_TRUE(reader()->readKlineSliceByRange(stdCode.c_str(), KP_DAY, 202401020930ull, 202401041500ull) == NULL);
	EXPECT_TRUE(reader()->readKlineSliceByCount(stdCode.c_str(), KP_DAY, 3, 202401041500ull) == NULL);
	EXPECT_TRUE(reader()->readTickSliceByRange(stdCode.c_str(), 20240102093000000ull, 20240102100000000ull) == NULL);
	EXPECT_TRUE(reader()->readTickSliceByCount(stdCode.c_str(), 3, 20240102100000000ull) == NULL);
	EXPECT_TRUE(reader()->readTickSliceByDate(stdCode.c_str(), 20240102) == NULL);
	EXPECT_FALSE(std::filesystem::exists(_root / "day"));
	EXPECT_FALSE(std::filesystem::exists(_root / "ticks"));
#ifndef WT_S5_BASELINE
	EXPECT_EQ(sink()->count_containing("rejected storage identity"), 5u);
#endif
}

//////////////////////////////////////////////////////////////////////////
// 第二轮：I03-NEW-051（夜盘 tick 区间二次偏移）与 I03-NEW-052（未知品种不解引用）
//////////////////////////////////////////////////////////////////////////
namespace {

// Futures night session with a +300 minute offset: 21:00-02:30 natural time. Ticks
// after 21:00 on 2023-01-03 belong to trading date 2023-01-04, and the AD writer
// keys them by (trading date, offset time), as WtDataWriterAD::commitTickToDb does.
class S5NightBaseDataMgr : public IBaseDataMgr
{
public:
	S5NightBaseDataMgr()
	{
		_session = WTSSessionInfo::create("S5_NIGHT", "s5 night", 300);
		_session->addTradingSection(2100, 230);
		_comm = WTSCommodityInfo::create("rb", "rb", "XEX", "S5_NIGHT", "CHN");
		_comm->setSessionInfo(_session);
		_comm->setCategory(ContractCategory::CC_Future);
	}
	virtual ~S5NightBaseDataMgr()
	{
		_comm->release();
		_session->release();
	}
	WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		return exchgpid != NULL && std::string(exchgpid) == "XEX.rb" ? _comm : NULL;
	}
	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		return exchg != NULL && pid != NULL && std::string(exchg) == "XEX" && std::string(pid) == "rb" ? _comm : NULL;
	}
	WTSContractInfo* getContract(const char*, const char*, uint32_t) override { return NULL; }
	WTSArray* getContracts(const char*, uint32_t) override { return NULL; }
	WTSSessionInfo* getSession(const char*) override { return _session; }
	WTSSessionInfo* getSessionByCode(const char*, const char*) override { return _session; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool) override { return false; }
	// Natural HHMM in, trading date out: night trading from 21:00 rolls to the next day.
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t uTime, bool) override
	{
		if (uDate == 20230103 && uTime >= 2100)
			return 20230104;
		return uDate;
	}
	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool, bool isStart) override
	{
		return isStart ? (uint64_t)tDate * 10000 + 2100 : (uint64_t)tDate * 10000 + 230;
	}
	WTSSessionInfo* session() { return _session; }

private:
	WTSSessionInfo* _session;
	WTSCommodityInfo* _comm;
};

} // namespace

TEST_F(R21ReaderADTest, S5NightSessionTickRangeUsesNaturalTimeForTradingDate)
{
	S5NightBaseDataMgr night;
	R21RdmSink nightSink(&night, NULL);
	WTSTickStruct tick = r21_make_tick("XEX", "rb2309", 20230104, 20230103, 220000000u, 4000.0);
	const uint32_t offTime = night.session()->offsetTime(2200, true) * 100000 + 0;
	{
		const std::filesystem::path dir = _root / "ticks" / "XEX" / "rb2309";
		std::filesystem::create_directories(dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(dir.string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		ASSERT_TRUE(query.put(r21_key_str(LMDBHftKey("XEX", "rb2309", 20230104, offTime)), r21_raw_of(tick)));
		query.commit();
	}
	WtRdmDtReaderAD reader;
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", _root.string().c_str());
	reader.init(cfg, &nightSink);
	cfg->release();

	WTSTickSlice* slice = reader.readTickSliceByRange("XEX.rb.rb2309",
		20230103213000000ull, 20230103223000000ull);
	ASSERT_TRUE(slice != NULL) << "night tick keyed by (next trading date, offset time) was missed";
	ASSERT_EQ(slice->size(), 1u);
	EXPECT_EQ(slice->at(0)->action_time, 220000000u);
	EXPECT_DOUBLE_EQ(slice->at(0)->price, 4000.0);
	slice->release();
}

TEST_F(R21ReaderADTest, S5UnknownCommodityTickRangeIsRefusedWithoutDereference)
{
	S5NightBaseDataMgr night;	// knows only XEX.rb
	R21RdmSink nightSink(&night, NULL);
	WtRdmDtReaderAD reader;
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", _root.string().c_str());
	reader.init(cfg, &nightSink);
	cfg->release();
	EXPECT_TRUE(reader.readTickSliceByRange("XEX.nope.np2309", 20230103213000000ull, 20230103223000000ull) == NULL);
	EXPECT_FALSE(std::filesystem::exists(_root / "ticks"));
#ifndef WT_S5_BASELINE
	EXPECT_EQ(nightSink.count_containing("not found"), 1u);
#endif
}
