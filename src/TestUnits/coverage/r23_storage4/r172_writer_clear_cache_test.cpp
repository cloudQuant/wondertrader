// r172 / WtDataWriter：收盘缓存清理分支（CMD_CLEAR_CACHE）
//
// 覆盖（src/WtDataStorage/WtDataWriter.cpp）：
//  - transHisData 的 CMD_CLEAR_CACHE 入队分支 :1601-1604
//  - proc_loop 的清理分支 :2240-2391：
//      快照表头/逐合约快照 :2245-2276、过期合约删除 rt tick :2278-2285、
//      缓存重建（索引替换、重写 cache.dmb、重新映射）:2289-2338、
//      当日快照落盘 :2341-2354、六个 rt 目录删除 :2356-2380
//
// 线程纪律沿用 r23 夹具：transHisData 之后只等文件信号，不 release（proc_loop
// 的 wait 无谓词，见 bugs/p10_storage_writer.md BUG-P10-005），由一用例一进程兜底。
// 三个用例分别覆盖：有代码过期（重建）/ 全部有效（不重建）/ 缓存为空。
#include "r23_writer_fixture.h"

namespace r23 {

class R172WriterClearCacheTest : public R23WriterTest
{
protected:
	string find_snapshot_csv() const
	{
		const string dir = _base + "his/snapshot";
		std::error_code ec;
		if (!std::filesystem::exists(dir, ec))
			return "";
		for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
		{
			if (entry.path().extension() == ".csv")
				return entry.path().string();
		}
		return "";
	}

	// 清理线程没有 marker 可等，按「快照 CSV 已出现 + rt/ticks 已被删除 + 可选日志/内容」
	// 三个信号一起判定，上限 5s，绝不无界等待。
	bool wait_clear(uint32_t max_ms, const string& needle1, const string& needle2,
		bool want_rebuild_log)
	{
		uint32_t waited = 0;
		while (waited < max_ms)
		{
			const string csv = find_snapshot_csv();
			if (!csv.empty() && !file_exists(_base + "rt/ticks")
				&& (!want_rebuild_log || _sink->has_log("expired cache cleared totally", LL_INFO)))
			{
				const string content = read_file_bytes(csv);
				const bool hit1 = needle1.empty() || content.find(needle1) != string::npos;
				const bool hit2 = needle2.empty() || content.find(needle2) != string::npos;
				if (hit1 && hit2)
					return true;
			}
			usleep(5000);
			waited += 5;
		}
		return false;
	}

	// 写一个 tick 并确认缓存里已有该合约（writeTick 同步路径会立即更新 _tick_cache_idx）
	void seed_code(const char* code, double price)
	{
		TickSpec spec;
		spec.code = code;
		spec.price = price;
		spec.open = price;
		spec.high = price;
		spec.low = price;
		spec.settle = price;
		write_tick(unclosed_writer(), spec, 0);
	}
};

// WtDataWriter.cpp:1601-1604、2240-2391（过期合约 -> 重建分支）
TEST_F(R172WriterClearCacheTest, R172WriterClearCacheRebuildsExpiredCode)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	seed_code("rb2210", 3600.0);
	seed_code("rb2211", 3700.0);

	// 让 rb2210 在 _bd_mgr 中消失：proc_loop 走 :2278 的「过期代码」分支，
	// setCodes 只剩 rb2211，于是 :2289 的 size 不等成立，触发缓存重建。
	_bd->forget_contract("rb2210");

	abandon_writer(w);
	w->transHisData("CMD_CLEAR_CACHE");

	ASSERT_TRUE(wait_clear(5000, "rb2211", "", true)) << "cleanup thread did not finish";

	// 快照行只保留有效合约
	const string csv = find_snapshot_csv();
	ASSERT_FALSE(csv.empty());
	const string content = read_file_bytes(csv);
	EXPECT_NE(content.find("rb2211"), string::npos);
	EXPECT_EQ(content.find("rb2210"), string::npos);

	// cache.dmb 被重写为只含 rb2211 一项的新块
	const RTBlockHeader head = read_block_header<RTBlockHeader>(cache_file());
	EXPECT_EQ(head._size, 1u);
	TickCacheItem item;
	ASSERT_TRUE((read_block_item<RTBlockHeader, TickCacheItem>(cache_file(), 0, item)));
	EXPECT_STREQ(item._tick.code, "rb2211");

	// 整个 rt/ticks 目录已被删除
	EXPECT_FALSE(file_exists(_base + "rt/ticks"));
	EXPECT_TRUE(file_exists(_base + "his/snapshot"));
}

// WtDataWriter.cpp:2289-2338（全部代码有效 -> 不重建缓存，但仍出快照、清 rt）
TEST_F(R172WriterClearCacheTest, R172WriterClearCacheKeepsAllLiveCodes)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	seed_code("rb2210", 3600.0);
	seed_code("rb2211", 3700.0);

	abandon_writer(w);
	w->transHisData("CMD_CLEAR_CACHE");

	ASSERT_TRUE(wait_clear(5000, "rb2210", "rb2211", false)) << "cleanup thread did not finish";

	const string csv = find_snapshot_csv();
	ASSERT_FALSE(csv.empty());
	const string content = read_file_bytes(csv);
	EXPECT_NE(content.find("rb2210"), string::npos);
	EXPECT_NE(content.find("rb2211"), string::npos);

	// 没有过期代码 -> 不走重建，cache.dmb 仍是原来的两项
	const RTBlockHeader head = read_block_header<RTBlockHeader>(cache_file());
	EXPECT_EQ(head._size, 2u);
	EXPECT_FALSE(_sink->has_log("expired cache cleared totally", LL_INFO));

	EXPECT_FALSE(file_exists(_base + "rt/ticks"));
	EXPECT_TRUE(file_exists(_base + "his/snapshot"));
}

// WtDataWriter.cpp:2245-2276、2289、2341-2380（缓存为空：只有表头快照 + 清 rt）
TEST_F(R172WriterClearCacheTest, R172WriterClearCacheWithEmptyCache)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	abandon_writer(w);
	w->transHisData("CMD_CLEAR_CACHE");

	ASSERT_TRUE(wait_clear(5000, "date,exchg,code", "", false)) << "cleanup thread did not finish";

	const string csv = find_snapshot_csv();
	ASSERT_FALSE(csv.empty());
	const string content = read_file_bytes(csv);
	EXPECT_NE(content.find("date,exchg,code"), string::npos);
	// 空缓存没有任何数据行（表头行以 std::endl 结尾）
	EXPECT_EQ(content.find("SHFE"), string::npos);

	const RTBlockHeader head = read_block_header<RTBlockHeader>(cache_file());
	EXPECT_EQ(head._size, 0u);
	EXPECT_FALSE(file_exists(_base + "rt/ticks"));
}

} // namespace r23
