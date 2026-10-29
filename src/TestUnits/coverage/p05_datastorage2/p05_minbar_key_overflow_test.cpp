// p05 / WtDataStorageAD：AD 落库键尾的整型截断与区间上界哨兵
//
// 背景：WtDataWriterAD 落库时
//   pipeToDayBars → LMDBBarKey(exchg, code, bar.date)
//   pipeToM1Bars / pipeToM5Bars → LMDBBarKey(exchg, code, (uint32_t)bar.time)
// 而所有读侧（WtBtDtReaderAD::read_raw_bars、WtRdmDtReaderAD::readKlineSliceByRange、
// WtDataReaderAD::read_bars_to_buffer）的右哨兵固定是 LMDBBarKey(exchg, code, 0xffffffff)。
// 分钟线的 bar.time = (date - 19900000) * 10000 + hhmm（uint64），
// 这里把它压成 uint32 做键尾。本文件用隔离 LMDB 环境手工构造键值并检查键尾截断与
// 哨兵扫描范围；这不是 WtDataWriterAD 的端到端写入证据。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

#include "Includes/WTSMarcos.h"
#include "Includes/WTSStruct.h"
#include "Share/TimeUtils.hpp"
#include "WtDataStorageAD/LMDBKeys.h"
#include "WTSUtils/WtLMDB.hpp"

using namespace wtp;

namespace {

class P05KeyTailTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		_root = std::filesystem::temp_directory_path()
			/ ("p05_keytail_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
		std::filesystem::remove_all(_root);
		std::filesystem::create_directories(_root);
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	static std::string key_str(const LMDBBarKey& key)
	{
		return std::string((const char*)&key, sizeof(key));
	}

	static std::string raw_of(const WTSBarStruct& bar)
	{
		return std::string((const char*)&bar, sizeof(bar));
	}

	static WTSBarStruct make_bar(uint32_t date, uint64_t time, double base)
	{
		WTSBarStruct bar;
		bar.date = date;
		bar.time = time;
		bar.open = base;
		bar.high = base + 1.0;
		bar.low = base - 1.0;
		bar.close = base + 0.5;
		bar.settle = base;
		bar.money = base * 10.0;
		bar.vol = base;
		return bar;
	}

	// 手工按 uint32 键尾构造 LMDB 记录，再检查读侧 (lKey=0, rKey=0xffffffff) 的扫描范围；
	// 不经过 WtDataWriterAD，不能作为 writer 端到端证据。
	uint32_t scan_with_sentinel(const char* exchg, const char* code,
		const std::vector<WTSBarStruct>& seeds, const std::vector<uint64_t>& key_times)
	{
		std::filesystem::create_directories(_root);
		WtLMDB db(false);
		if(!db.open(_root.string().c_str(), 16 * 1024 * 1024))
			return 0u;

		{
			WtLMDBQuery w(db);
			for(size_t i = 0; i < seeds.size(); i++)
				w.put(key_str(LMDBBarKey(exchg, code, (uint32_t)key_times[i])), raw_of(seeds[i]));
			w.commit();
		}

		const LMDBBarKey lKey(exchg, code, 0);
		const LMDBBarKey rKey(exchg, code, 0xffffffff);
		uint32_t hits = 0;
		WtLMDBQuery q(db);
		q.get_range(std::string((const char*)&lKey, sizeof(lKey)),
			std::string((const char*)&rKey, sizeof(rKey)),
			[&hits](const ValueArray& /*ayKeys*/, const ValueArray& ayVals) {
				hits += (uint32_t)ayVals.size();
			});
		return hits;
	}

	std::filesystem::path _root;
};

}
//////////////////////////////////////////////////////////////////////////

TEST_F(P05KeyTailTest, MinBarTimeGrowsMonotonicallyUpTo2032)
{
	// (date-19900000)*10000 + hhmm，与 TimeUtils::timeToMinBar 口径一致
	const uint64_t t2024 = TimeUtils::timeToMinBar(20240102, 931);
	const uint64_t t2032 = TimeUtils::timeToMinBar(20321231, 2359);
	EXPECT_GT(t2032, t2024);
	// 这两个值都必须还能塞进 uint32 键尾
	EXPECT_LE(t2024, 0xffffffffu);
	EXPECT_LE(t2032, 0xffffffffu);
	EXPECT_EQ((uint32_t)t2032, (uint64_t)t2032);
}

TEST_F(P05KeyTailTest, BarsBeyond2033EscapeTheUpperSentinel)
{
	// 公开编码语义：(date-19900000)*10000+hhmm，结果应保留完整 uint64。
	// 20330101 的真值为 4,301,011,000，超过 uint32；独立提升后再计算，避免复现
	// BUG-R32-003 中 helper 先在 uint32 域乘法回绕的问题。
	const uint64_t t_ok_2032 = TimeUtils::timeToMinBar(20321231, 1000);
	const uint64_t t_true_2033 =
		(static_cast<uint64_t>(20330101u) - 19900000ull) * 10000ull + 1000ull;
	const uint64_t t_helper_2033 = TimeUtils::timeToMinBar(20330101, 1000);
	EXPECT_GT(t_true_2033, 0xffffffffull);
	// 正确 helper 应返回完整真值。用非致命断言，helper 回绕失败时仍继续验证下方
	// 独立手工构造的 LMDB uint32 键尾与哨兵格式事实。
	EXPECT_EQ(t_helper_2033, t_true_2033) << "BUG-R32-003: timeToMinBar must preserve uint64 time";

	// 独立的存储格式事实：下面由 scan_with_sentinel 手工 put，键尾被 LMDBBarKey 的
	// uint32 参数截断。4,301,011,000 - 2^32 = 6,043,704，落在固定右哨兵内；
	// 此检查没有经过 WtDataWriterAD，不能推断 writer 端到端行为。
	const uint32_t truncated_tail_2033 = static_cast<uint32_t>(t_true_2033);
	EXPECT_EQ(truncated_tail_2033, 6043704u);
	std::vector<WTSBarStruct> seeds;
	std::vector<uint64_t> keys;
	seeds.push_back(make_bar(20240102, t_ok_2032, 100.0));	keys.push_back(t_ok_2032);
	seeds.push_back(make_bar(20240103, t_ok_2032 + 1, 200.0));	keys.push_back(t_ok_2032 + 1);
	seeds.push_back(make_bar(20330101, t_true_2033, 300.0));	keys.push_back(t_true_2033);
	EXPECT_NE(static_cast<uint64_t>(truncated_tail_2033), t_true_2033);

	const uint32_t hits = scan_with_sentinel("SHFE", "rb2409", seeds, keys);
	// 三条手工写入的记录都在扫描结果中；这里仅钉住 32 位键尾与哨兵边界行为。
	EXPECT_EQ(hits, 3u);
}

TEST_F(P05KeyTailTest, TruncatedKeyTailCollidesWithOlderBar)
{
	// R32 修正：hi 用 uint64 算式直出真值（不能再用 TimeUtils::timeToMinBar ——
	// 它在 uint32 里已经先回绕，见 TimeUtils.hpp:339 与 BUG-R32-003），
	// wrapped = hi - 2^32 才是"截断键尾"的落点。
	const uint64_t hi = 430101ull * 10000ull + 1000ull;		// 20330101 01:00 的语义真值
	const uint64_t wrapped = hi - 0x100000000ull;					// 回绕值 6,043,704
	ASSERT_GT(hi, 0xffffffffu);
	ASSERT_LT(wrapped, 0xffffffffu);

	std::vector<WTSBarStruct> seeds;
	std::vector<uint64_t> keys;
	seeds.push_back(make_bar(19900101, wrapped, 1.0));			keys.push_back(wrapped);
	seeds.push_back(make_bar(20330101, hi, 2.0));					keys.push_back(hi);
	// 第二条的键尾 == 第一条的键尾 → LMDB 覆盖写，只剩一条
	const uint32_t hits = scan_with_sentinel("DCE", "m2409", seeds, keys);
	EXPECT_EQ(hits, 1u);
	EXPECT_EQ((uint32_t)hi, (uint32_t)wrapped);
}

TEST_F(P05KeyTailTest, DayBarsKeyedByDateNeverOverflowSentinel)
{
	// 日线键尾是 bar.date（8 位 YYYYMMDD），远小于 0xffffffff，直到 9999 年都安全
	const uint32_t d1 = 20240102, d2 = 20330101, d3 = 99991231;
	EXPECT_LT(d1, d2);
	EXPECT_LT(d2, d3);
	EXPECT_LT(d3, 0xffffffffu);

	std::vector<WTSBarStruct> seeds;
	std::vector<uint64_t> keys;
	seeds.push_back(make_bar(d1, d1, 10.0));		keys.push_back(d1);
	seeds.push_back(make_bar(d2, d2, 20.0));		keys.push_back(d2);
	seeds.push_back(make_bar(d3, d3, 30.0));		keys.push_back(d3);
	EXPECT_EQ(scan_with_sentinel("CZCE", "SR409", seeds, keys), 3u);
}

TEST_F(P05KeyTailTest, KeyTailOrderMatchesNumericOrderInsideUint32)
{
	// 读侧依赖“字典序 == 数值序”：键尾必须是 packed 大端布局的最后一个字段
	std::vector<uint32_t> tails = {0u, 1u, 20240102u, 0xfffffffeu, 0xffffffffu};
	for(size_t i = 1; i < tails.size(); i++)
	{
		const std::string prev = key_str(LMDBBarKey("SHFE", "rb2409", tails[i - 1]));
		const std::string cur = key_str(LMDBBarKey("SHFE", "rb2409", tails[i]));
		EXPECT_LT(prev.compare(cur), 0) << "tail " << tails[i];
	}
	// 合约代码前缀必须把键空间分开：同一交易所不同 code，即使 code 的键尾更大，
	// 字典序也只由 code 段先决定（读侧 (lKey=0 .. rKey=0xffffffff) 的取数范围依赖它）
	const std::string aa = key_str(LMDBBarKey("SHFE", "aa0101", 0xffffffffu));
	const std::string zz = key_str(LMDBBarKey("SHFE", "zz1231", 0));
	EXPECT_LT(aa.compare(zz), 0);
	const std::string other_exchg = key_str(LMDBBarKey("DCE", "aa0101", 0));
	EXPECT_LT(other_exchg.compare(aa), 0);
}
