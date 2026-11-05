// I03-NEW-039（迭代03 最终缺口，slice2）：WtDtServo WtDataManager 的 K 线缓存键原以
// fmtutil::format<64> 拼接调用方 stdCode。CodeHelper::extractStdCode 接受的合法 stdCode 可达
// 63 字节，键超出 64 字节固定缓冲；在 fmt 11 下 format_to(char[64]) 越界即抛 format_error，
// 而 fmtutil::format 为 noexcept，进程直接 terminate（改前红控）。改后以 std::string 拼接，
// 键与完整代码一致。fake reader 不返回数据，路径不创建切片（与 I03-NEW-083 无关）。
#include <gtest/gtest.h>

#include <string>

#include <Includes/IBaseDataMgr.h>
#include <Includes/IRdmDtReader.h>
#include <Includes/WTSVariant.hpp>
#include <Share/fmtlib.h>
#define private public
#include <WtDtServo/WtDataManager.h>
#undef private

USING_NS_WTP;

namespace i03s2n039 {

class EmptyReader : public IRdmDtReader
{
public:
	WTSOrdDtlSlice* readOrdDtlSliceByRange(const char*, uint64_t, uint64_t) override { return NULL; }
	WTSOrdQueSlice* readOrdQueSliceByRange(const char*, uint64_t, uint64_t) override { return NULL; }
	WTSTransSlice* readTransSliceByRange(const char*, uint64_t, uint64_t) override { return NULL; }
	WTSTickSlice* readTickSliceByDate(const char*, uint32_t) override { ++tickReads; return NULL; }
	WTSTickSlice* readTickSliceByRange(const char*, uint64_t, uint64_t) override { return NULL; }
	WTSKlineSlice* readKlineSliceByRange(const char*, WTSKlinePeriod, uint64_t, uint64_t) override { ++barReads; return NULL; }
	WTSTickSlice* readTickSliceByCount(const char*, uint32_t, uint64_t) override { return NULL; }
	WTSKlineSlice* readKlineSliceByCount(const char*, WTSKlinePeriod, uint32_t, uint64_t) override { ++barReads; return NULL; }
	int tickReads = 0;
	int barReads = 0;
};

class NoCommodityBdMgr : public IBaseDataMgr
{
public:
	WTSCommodityInfo* getCommodity(const char*) override { return NULL; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return NULL; }
	WTSContractInfo* getContract(const char*, const char*, uint32_t) override { return NULL; }
	WTSArray* getContracts(const char*, uint32_t) override { return NULL; }
	WTSSessionInfo* getSession(const char*) override { return NULL; }
	WTSSessionInfo* getSessionByCode(const char*, const char*) override { return NULL; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t d, uint32_t, bool) override { return d; }
	uint64_t getBoundaryTime(const char*, uint32_t d, bool, bool) override { return d; }
};

// 63 字节的合法标准代码：交易所 + 31 字节品种 + 27 字节合约
std::string longest_std_code()
{
	return std::string("SSE.") + std::string(31, 'A') + "." + std::string(27, 'B');
}

} // namespace i03s2n039

using namespace i03s2n039;

TEST(I03S2New039ServoCacheKey, LongestLegalStdCodeKeepsExactCacheKeys)
{
	const std::string code = longest_std_code();
	ASSERT_EQ(63u, code.size());
	EmptyReader reader;
	NoCommodityBdMgr bd;
	WtDataManager dm;
	dm._reader = &reader;
	dm._bd_mgr = &bd;

	EXPECT_EQ(nullptr, dm.get_skline_slice_by_date(code.c_str(), 5, 20240102));
	EXPECT_EQ(1, reader.tickReads);
	EXPECT_EQ(1u, dm._bars_cache.count(fmt::format("{}-{}-s{}", code, 20240102, 5)))
		<< "second-bar cache key must contain the full standard code";

	EXPECT_EQ(nullptr, dm.get_kline_slice_by_range(code.c_str(), KP_Minute1, 5, 202401020930ull, 202401021500ull));
	EXPECT_EQ(1u, dm._bars_cache.count(fmt::format("{}-{}-{}", code, (uint32_t)KP_Minute1, 5)))
		<< "resampled-bar cache key must contain the full standard code";

	dm._reader = NULL;	// 不经 remover 销毁测试自有的 reader
	dm._bd_mgr = NULL;
}
