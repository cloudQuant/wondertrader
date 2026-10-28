// r193 / WtDataStorageAD：读端打不开 LMDB 的公开错误面
//
// 覆盖（src/WtDataStorageAD/WtDataReaderAD.cpp）：
//  - readTickSlice 的 get_t_db 失败早退 :143-145
//  - readKlineSlice 无数据时返回 NULL :419-420
// 覆盖（src/WtDataStorageAD/WtRdmDtReaderAD.cpp）：
//  - readTickSliceByRange 的 get_t_db 失败早退 :114-116
//
// 手法：沿用 r162 AD 夹具；用 chmod 0000 的空目录制造 EACCES，用例内恢复 0755。
// 所有临时目录都在系统临时目录。写端 resize 失败路径（:309-311）会落到
// BoostMappingFile::map 的未捕获异常，按纪律不驱动。
#include "r162_ad_fixture.h"

#include <sys/stat.h>

namespace r193ad {

using namespace r162ad;
using std::string;

class R193AdResidualTest : public AdFixture
{
};

// WtDataReaderAD.cpp:143-145；tick 库目录不可读时 readTickSlice 返回 NULL
TEST_F(R193AdResidualTest, R193ReaderUnreadableTickDbReturnsNull)
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
	delete reader;

	EXPECT_TRUE(slice == NULL);
	EXPECT_GE(_rsink->count_of(LL_ERROR), 1u);
}

// WtDataReaderAD.cpp:419-420；空 K 线库返回 NULL，不误返回空切片
TEST_F(R193AdResidualTest, R193ReaderEmptyKlineReturnsNull)
{
	WtDataReaderAD* reader = make_reader();
	ASSERT_TRUE(reader != NULL);
	WTSKlineSlice* slice = reader->readKlineSlice("SSE.600000", KP_Minute1, 10,
		(uint64_t)20240110 * 1000000000ULL + 93200000ULL);
	delete reader;
	EXPECT_TRUE(slice == NULL);
}

// WtRdmDtReaderAD.cpp:114-116；tick 库目录不可读时区间读返回 NULL
TEST_F(R193AdResidualTest, R193RdmUnreadableTickDbReturnsNull)
{
	const string dir = path_of("ticks/SSE/600001");
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	ASSERT_FALSE(ec) << ec.message();
	ASSERT_EQ(::chmod(dir.c_str(), 0000), 0);

	WtRdmDtReaderAD* reader = make_rdm_reader();
	ASSERT_TRUE(reader != NULL);
	WTSTickSlice* slice = reader->readTickSliceByRange("SSE.600001",
		(uint64_t)20240110 * 1000000000ULL + 93000000ULL,
		(uint64_t)20240110 * 1000000000ULL + 150000000ULL);

	ASSERT_EQ(::chmod(dir.c_str(), 0755), 0);
	delete reader;

	EXPECT_TRUE(slice == NULL);
	EXPECT_GE(_qsink->count_of(LL_ERROR), 1u);
}

} // namespace r193ad
