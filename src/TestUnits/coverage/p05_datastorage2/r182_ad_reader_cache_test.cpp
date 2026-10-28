// r182 / WtDataStorageAD：读端缓存复用与错误路径
//
// 覆盖（src/WtDataStorageAD/WtRdmDtReaderAD.cpp）：
//  - readTickSliceByRange：非空 tick 缓存、首笔不晚于起点时走第二次 lower_bound :236-247
//  - readKlineSliceByRange：第二次请求时间前移时从 LMDB 增量装载          :317-338
// 覆盖（src/WtDataStorageAD/WtDataReaderAD.cpp）：
//  - readTickSlice 打开 tick 库失败返回 NULL                            :143-145
//
// 数据由 WtDataWriterAD 公开接口（writeTick 的分钟线转储）落 LMDB，
// 不使用直写缓存文件的布局镜像（r172 的 RT 缓存直写用例会 SIGSEGV，本 lane 避开）。
#include "r162_ad_fixture.h"

#include "Share/TimeUtils.hpp"

#include <sys/stat.h>

namespace r182ad {

using namespace r162ad;
using std::string;

class R182AdReaderTest : public AdFixture
{
};

// WtRdmDtReaderAD.cpp:236-247；第一次读建立缓存，第二次起点晚于缓存首笔
TEST_F(R182AdReaderTest, R182RdmADTickRangeReusesEarlierCache)
{
	const uint32_t date = 20240110;
	WtDataWriterAD* w = make_writer();
	ASSERT_TRUE(w != NULL);
	push_tick(w, date, 93000000u, 30.0, 1.0);
	push_tick(w, date, 93100000u, 31.0, 2.0);
	push_tick(w, date, 93200000u, 32.0, 3.0);

	WtRdmDtReaderAD* rdm = make_rdm_reader();
	ASSERT_TRUE(rdm != NULL);

	WTSTickSlice* first = rdm->readTickSliceByRange("SSE.600000",
		(uint64_t)date * 1000000000ULL + 93000000ULL,
		(uint64_t)date * 1000000000ULL + 93200000ULL);
	ASSERT_TRUE(first != NULL);
	EXPECT_EQ(tick_count(first), 3u);
	first->release();

	// 缓存首笔 09:30 早于新起点 09:35 -> else 分支的第二次 lower_bound
	WTSTickSlice* second = rdm->readTickSliceByRange("SSE.600000",
		(uint64_t)date * 1000000000ULL + 93500000ULL,
		(uint64_t)date * 1000000000ULL + 93600000ULL);
	ASSERT_TRUE(second != NULL);
	ASSERT_EQ(tick_count(second), 1u);
	ASSERT_TRUE(second->at(0) != NULL);
	EXPECT_EQ(second->at(0)->action_time, 93200000u);
	second->release();
}

// WtRdmDtReaderAD.cpp:317-338；第二次请求截止时间前移，LMDB 增量补 09:32 分钟线
TEST_F(R182AdReaderTest, R182RdmADKlineRangeLoadsNewerBars)
{
	const uint32_t date = 20240110;
	WtDataWriterAD* w = make_writer();
	ASSERT_TRUE(w != NULL);
	push_tick(w, date, 93100000u, 31.0, 1.0);
	push_tick(w, date, 93200000u, 32.0, 2.0);

	WtRdmDtReaderAD* rdm = make_rdm_reader();
	ASSERT_TRUE(rdm != NULL);

	WTSKlineSlice* k1 = rdm->readKlineSliceByRange("SSE.600000", KP_Minute1,
		(uint64_t)date * 10000ULL + 931ULL, (uint64_t)date * 10000ULL + 932ULL);
	ASSERT_TRUE(k1 != NULL);
	EXPECT_EQ(bar_count(k1), 1u);
	k1->release();

	push_tick(w, date, 93300000u, 33.0, 3.0);	// 09:32 分钟线转储

	WTSKlineSlice* k2 = rdm->readKlineSliceByRange("SSE.600000", KP_Minute1,
		(uint64_t)date * 10000ULL + 931ULL, (uint64_t)date * 10000ULL + 933ULL);
	ASSERT_TRUE(k2 != NULL);
	ASSERT_EQ(bar_count(k2), 2u);
	EXPECT_EQ(k2->at(1)->time, (uint64_t)TimeUtils::timeToMinBar(date, 932));
	k2->release();
}

// WtDataReaderAD.cpp:143-145；tick 库目录不可读 -> get_t_db 失败 -> readTickSlice 返回 NULL
TEST_F(R182AdReaderTest, R182ReaderADUnreadableTickDbReturnsNull)
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

} // namespace r182ad
