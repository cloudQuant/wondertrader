/**
 * r80_replayer_config_tick_test.cpp  (lane R80)
 *
 * 靶子：src/WtBtCore/HisDataReplayer.cpp
 *   - L251-L265  basefiles.contract 的数组装载
 *   - L2946-L2962 loadFees 的「文件存在但解析失败」
 *   - L362       loadStkAdjFactorsFromFile 跳过非数组条目
 *   - L518-L555  dump_btstate 的 m1 / m5 周期命名
 *   - L436-L517  clear_cache/reset 与 tick 游标复位
 *   - L1626-L1768 / L1795-L1898 Level2 空缓存循环
 *   - L3089-L3165 sub_tick/sub_order_* 的 Q/H 后缀剥离
 *
 * 落盘只在系统临时目录；tick loader 纯内存。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSSessionInfo.hpp>
#include <Includes/WTSVariant.hpp>
#include <Share/CodeHelper.hpp>
#include <WtBtCore/HisDataReplayer.h>

#include "r80_common.h"

namespace
{
	std::string read_text(const std::filesystem::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		if (!in.is_open())
			return std::string();
		return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	std::vector<WTSBarStruct> tiny_bars()
	{
		return {
			r80::mk_bar(20200102u, 930, 10.0, 10.2, 9.8, 10.1, 100.0),
			r80::mk_bar(20200102u, 931, 10.1, 10.3, 9.9, 10.2, 110.0),
			r80::mk_bar(20200102u, 932, 10.2, 10.4, 10.0, 10.3, 120.0)
		};
	}

	class RecordingTickLoader : public r80::Loader
	{
	public:
		struct Request
		{
			std::string code;
			uint32_t date;
			size_t available;
		};

		std::vector<Request> requests;

		bool loadRawHisTicks(void* obj, const char* stdCode, uint32_t uDate,
			FuncReadTicks cb) override
		{
			auto it = ticks.find(std::string(stdCode) + "#" + std::to_string(uDate));
			requests.push_back({ stdCode, uDate, it == ticks.end() ? 0u : it->second.size() });
			return r80::Loader::loadRawHisTicks(obj, stdCode, uDate, cb);
		}
	};
}

class R80ConfigTick : public r80::Fixture
{
};

/*
 * 合约文件数组形态 + 坏 fees JSON（解析失败不阻断 init）+ 除权因子里的非数组条目。
 */
TEST_F(R80ConfigTick, ContractArrayMalformedFeesAndAdjEntries)
{
	const std::filesystem::path badFees = _base / "bad_fees.json";
	ASSERT_TRUE(r80::write_text(badFees, "{ this is not json"));
	const std::filesystem::path adj = _base / "adj_partial.json";
	ASSERT_TRUE(r80::write_text(adj,
		"{\n  \"SSE\": {\n"
		"    \"600000\": [{\"date\": 20200106, \"factor\": 2.0}],\n"
		"    \"BROKEN\": 42\n"
		"  }\n}\n"));

	r80::CfgOpts opts;
	opts.fees = badFees.string();
	opts.adjfactor = adj.string();
	opts.contracts.push_back((_root / "dist/common/contracts.json").string());
	opts.contracts.push_back((_root / "dist/common/stocks.json").string());

	HisDataReplayer rp;
	WTSVariant* cfg = r80::make_cfg(_root, _base, 202001010930ull, 202001071500ull, opts);
	ASSERT_TRUE(rp.init(cfg));
	cfg->release();

	EXPECT_NE(nullptr, rp.get_commodity_info("SSE.STK.000001"));
	EXPECT_NE(nullptr, rp.get_session_info("SD0930"));
	// 普通合约没有自定义规则 -> 原始代码为空（L2921-L2930）
	EXPECT_EQ(std::string(""), rp.get_rawcode("SSE.STK.000001"));
}

/*
 * dump_btstate 的周期命名：m1 -> "m1"，m5 -> "m5"（L527-L535）。
 * 两条回放各自带独立 sink 名，输出到独立目录。
 */
TEST_F(R80ConfigTick, DumpStateUsesMinutePeriodNames)
{
	const std::vector<WTSBarStruct> bars = tiny_bars();
	ASSERT_TRUE(r80::write_bars_csv(_base, "SSE.STK.000001_m1", bars, true));
	ASSERT_TRUE(r80::write_bars_csv(_base, "SSE.STK.000001_m5", bars, true));

	{
		HisDataReplayer rp;
		r80::CfgOpts opts;
		WTSVariant* cfg = r80::make_cfg(_root, _base, 202001020930ull, 202001021500ull, opts);
		ASSERT_TRUE(rp.init(cfg));
		cfg->release();

		r80::Sink sink;
		rp.register_sink(&sink, "r80_dump_m1");
		sink.onInit = [&rp, &sink]() {
			sink.slice = rp.get_kline_slice("SSE.STK.000001", "m", 1, 1, true);
		};
		ASSERT_TRUE(rp.prepare());
		ASSERT_NE(nullptr, sink.slice);
		rp.run(true);
		EXPECT_EQ(1u, sink.doneCalls);
		sink.slice->release();
		sink.slice = NULL;
	}

	{
		HisDataReplayer rp;
		r80::CfgOpts opts;
		WTSVariant* cfg = r80::make_cfg(_root, _base, 202001020930ull, 202001021500ull, opts);
		ASSERT_TRUE(rp.init(cfg));
		cfg->release();

		r80::Sink sink;
		rp.register_sink(&sink, "r80_dump_m5");
		sink.onInit = [&rp, &sink]() {
			sink.slice = rp.get_kline_slice("SSE.STK.000001", "m", 5, 1, true);
		};
		ASSERT_TRUE(rp.prepare());
		ASSERT_NE(nullptr, sink.slice);
		rp.run(true);
		EXPECT_EQ(1u, sink.doneCalls);
		sink.slice->release();
		sink.slice = NULL;
	}

	const std::string m1 = read_text(_out / "r80_dump_m1" / "btenv.json");
	const std::string m5 = read_text(_out / "r80_dump_m5" / "btenv.json");
	ASSERT_FALSE(m1.empty());
	ASSERT_FALSE(m5.empty());
	EXPECT_NE(std::string::npos, m1.find("\"m1\""));
	EXPECT_NE(std::string::npos, m5.find("\"m5\""));
}

/*
 * 逐 tick 回放 + Level2 订阅（csv 模式无 Level2 数据）：
 * getNext*Time 在 checkOrder* 处早退，replayHftDatasByDay 对空缓存循环 continue；
 * 第二轮 prepare 触发 reset 的 tick 游标复位（L462-L466），重新订阅后可再次回放。
 * 同时覆盖 sub_tick、sub_order_* 与 sub_transaction 的真实复权后缀 -/+ 剥离。
 */
TEST_F(R80ConfigTick, TickReplayResetsCursorAndSkipsEmptyLevel2)
{
	RecordingTickLoader loader;
	loader.ticks[std::string("SSE.STK.000001#20200102")] = {
		r80::mk_tick(20200102u, 930, 10.0, 100.0),
		r80::mk_tick(20200102u, 931, 11.0, 110.0),
		r80::mk_tick(20200102u, 932, 12.0, 120.0)
	};

	HisDataReplayer rp;
	r80::CfgOpts opts;
	opts.tick = true;
	WTSVariant* cfg = r80::make_cfg(_root, _base, 202001020930ull, 202001021500ull, opts);
	ASSERT_TRUE(rp.init(cfg, NULL, &loader));
	cfg->release();

	r80::Sink sink;
	rp.register_sink(&sink, "r80_tick_reset");

	ASSERT_TRUE(rp.prepare());
	const std::string baseCode = "SSE.STK.000001";
	const std::string qfqCode = baseCode + SUFFIX_QFQ;
	const std::string hfqCode = baseCode + SUFFIX_HFQ;
	// prepare 内部的 reset 会清空订阅表，必须在 prepare 之后再订阅
	rp.sub_tick(1, qfqCode.c_str());
	rp.sub_order_detail(2, qfqCode.c_str());
	rp.sub_order_queue(3, hfqCode.c_str());
	rp.sub_transaction(4, hfqCode.c_str());
	ASSERT_EQ(20200102u, rp.get_trading_date());
	ASSERT_NE(nullptr, rp.get_commodity_info(baseCode.c_str()));
	WTSSessionInfo* stockSession = rp.get_session_info("SSE.STK.000001", true);
	ASSERT_NE(nullptr, stockSession);
	EXPECT_TRUE(stockSession->isInTradingTime(930));
	EXPECT_TRUE(stockSession->isInTradingTime(932));

	// csv 模式拒绝 Level2：三个切片入口直接返回 NULL
	EXPECT_EQ(nullptr, rp.get_order_detail_slice("SSE.STK.000001", 2));
	EXPECT_EQ(nullptr, rp.get_order_queue_slice("SSE.STK.000001", 2));
	EXPECT_EQ(nullptr, rp.get_transaction_slice("SSE.STK.000001", 2));

	rp.run(false);
	ASSERT_EQ(1u, loader.requests.size());
	ASSERT_EQ(baseCode, loader.requests[0].code);
	ASSERT_EQ(20200102u, loader.requests[0].date);
	ASSERT_EQ(3u, loader.requests[0].available);
	// 三笔 tick 均在配置的回放窗口内。
	const size_t firstRoundCount = sink.ticks.size();
	EXPECT_EQ(3u, firstRoundCount);
	if (firstRoundCount >= 3u)
	{
		EXPECT_DOUBLE_EQ(10.0, sink.ticks[0].price);
		EXPECT_EQ(20200102u, sink.ticks[0].date);
		EXPECT_EQ(930u, sink.ticks[0].time);
		EXPECT_DOUBLE_EQ(11.0, sink.ticks[1].price);
		EXPECT_EQ(20200102u, sink.ticks[1].date);
		EXPECT_EQ(931u, sink.ticks[1].time);
		EXPECT_DOUBLE_EQ(12.0, sink.ticks[2].price);
		EXPECT_EQ(20200102u, sink.ticks[2].date);
		EXPECT_EQ(932u, sink.ticks[2].time);
	}
	EXPECT_EQ(1u, sink.beginCalls);
	EXPECT_EQ(1u, sink.endCalls);
	EXPECT_EQ(1u, sink.doneCalls);

	// 第二轮：prepare -> reset 把 tick 游标还原到 UINT_MAX，重新订阅后再次回放
	const size_t beforeSecondRound = sink.ticks.size();
	ASSERT_TRUE(rp.prepare());
	rp.sub_tick(1, qfqCode.c_str());
	rp.run(false);
	const size_t secondRoundCount = sink.ticks.size() - beforeSecondRound;
	EXPECT_EQ(3u, secondRoundCount);
	EXPECT_EQ(6u, sink.ticks.size());
	if (sink.ticks.size() >= 6u)
	{
		EXPECT_DOUBLE_EQ(10.0, sink.ticks[3].price);
		EXPECT_EQ(20200102u, sink.ticks[3].date);
		EXPECT_EQ(930u, sink.ticks[3].time);
		EXPECT_DOUBLE_EQ(11.0, sink.ticks[4].price);
		EXPECT_EQ(20200102u, sink.ticks[4].date);
		EXPECT_EQ(931u, sink.ticks[4].time);
		EXPECT_DOUBLE_EQ(12.0, sink.ticks[5].price);
		EXPECT_EQ(20200102u, sink.ticks[5].date);
		EXPECT_EQ(932u, sink.ticks[5].time);
	}
	EXPECT_EQ(2u, sink.beginCalls);
	EXPECT_EQ(2u, sink.endCalls);
	EXPECT_EQ(2u, sink.doneCalls);
}
