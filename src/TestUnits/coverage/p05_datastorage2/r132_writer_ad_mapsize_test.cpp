// r132 / WtDataWriterAD：LMDB mapsize 耗尽时的落库失败日志（分 tick / K 线两路）。
//
// 覆盖（src/WtDataStorageAD/WtDataWriterAD.cpp）：
//  - pipeToTicks 的 put_and_commit 失败 :425-432
//  - pipeToDayBars 失败 :456-461
//  - pipeToM1Bars 失败 :487-494
//  - pipeToM5Bars 失败 :521-527
//
// 手法：先用公开的 WtLMDB/WtLMDBQuery 把小 mapsize 的库填满，再让 writer 用同一个
// 小 mapsize 打开同一路径；put 返回 MDB_MAP_FULL 后只写 LL_ERROR 日志，不会崩溃。
// 两个用例分别只压 tick 库与只压 K 线库，避免一次失败影响另一路。不启动异步线程
// （沿用 r4 夹具把 _async_task 残值压成 0）；临时目录由夹具清理。
#include "r4_ad_fixture.h"

#include <filesystem>

#include "WtDataStorageAD/WtDataWriterAD.h"

namespace r132 {

using std::string;
using std::vector;

class R132WriterADMapsizeTest : public r4fx::R4WriterADTest
{
protected:
	static const uint32_t kMapSize = 16 * 1024;

	void fill_db(const string& path)
	{
		std::error_code ec;
		std::filesystem::create_directories(path, ec);

		WtLMDB db(false);
		ASSERT_TRUE(db.open(path.c_str(), kMapSize));

		bool full = false;
		uint32_t seq = 0;
		// 小批量提交：撞上 MDB_MAP_FULL 之前已经写入的批次必须真正落盘，
		// 这样 writer 打开时库已接近满页，后续 put 一定会失败。
		for (uint32_t batch = 0; batch < 10000 && !full; batch++)
		{
			WtLMDBQuery q(db);
			for (uint32_t j = 0; j < 8; j++)
			{
				string key(16, '\0');
				memcpy(key.data(), &seq, sizeof(seq));
				string val(512, (char)('a' + (seq % 26)));
				seq++;
				if (!q.put(key, val))
				{
					full = true;
					break;
				}
			}
			q.commit();
		}
		ASSERT_TRUE(full) << "无法把小 mapsize 的 LMDB 填满：" << path;
	}

	WTSContractInfo* prepare_contract()
	{
		WTSCommodityInfo* comm = _bd->add_commodity("EXQ", "rb", ContractCategory::CC_Future);
		EXPECT_TRUE(comm != NULL);
		WTSContractInfo* ct = _bd->add_contract("EXQ", "rb2301", comm);
		EXPECT_TRUE(ct != NULL);
		return ct;
	}

	void write_ticks(WTSContractInfo* ct)
	{
		// 30 笔 tick 跨 15 个交易日、30 个分钟：覆盖 tick/m1/m5/day 四类落库；
		// 交易日足够多，保证小 mapsize 下 day 落库也一定撞上 MDB_MAP_FULL
		for (uint32_t i = 0; i < 30; i++)
		{
			const uint32_t tdate = 20230103 + i / 2;
			WTSTickStruct ts = r4fx::make_tick("EXQ", "rb2301", tdate, tdate,
				(930 + i) * 100000, 100.0 + i, 1.0, 1.0, 1000.0, 0.0,
				(double)(i + 1), (double)(i + 1));
			WTSTickData* tick = tick_of(ct, ts);
			write_or_fail(tick, 0);
		}
	}
};

// WtDataWriterAD.cpp: L416-448（tick 库 put 失败）
TEST_F(R132WriterADMapsizeTest, R132WriterADTickMapFullLogsPipeFailure)
{
	WTSContractInfo* ct = prepare_contract();
	ASSERT_TRUE(ct != NULL);

	fill_db(path_of("ticks/EXQ/rb2301"));

	WTSVariant* cfg = r4fx::make_writer_cfg(_root.string(), 1000,
		false, false, false, false);
	cfg->append("tickmapsize", kMapSize);
	init_writer(cfg);

	write_ticks(ct);

	EXPECT_GE(_sink->count_containing("pipe tick"), 1u);
}

// WtDataWriterAD.cpp: L450-547（m1/m5/day 库 put 失败；关掉 tick 落库以隔离）
TEST_F(R132WriterADMapsizeTest, R132WriterADKlineMapFullLogsBarFailures)
{
	WTSContractInfo* ct = prepare_contract();
	ASSERT_TRUE(ct != NULL);

	fill_db(path_of("min1/EXQ"));
	fill_db(path_of("min5/EXQ"));
	fill_db(path_of("day/EXQ"));

	// disabletick=true：本用例只观察 K 线三路的失败日志
	WTSVariant* cfg = r4fx::make_writer_cfg(_root.string(), 1000,
		true, false, false, false);
	cfg->append("klinemapsize", kMapSize);
	init_writer(cfg);

	write_ticks(ct);

	EXPECT_GE(_sink->count_containing("pipe m1 bar"), 1u);
	EXPECT_GE(_sink->count_containing("pipe m5 bar"), 1u);
	EXPECT_GE(_sink->count_containing("pipe day bar"), 1u);
}

} // namespace r132
