// r23_storage4 / WtDataStorage：WtDataWriter 的收盘作业（transHisData → proc_loop → dump_*）
//
// 覆盖对象（src/WtDataStorage/WtDataWriter.cpp，公开入口 transHisData/isSessionProceeded/getCurTick）：
//  - transHisData     :1569-1614  getSessionComms==NULL 早退、按品种展开合约、MARK.<sid> 入队、
//                                 首次调用起 proc_loop 线程 / 后续 notify_all
//  - proc_loop        :2216-2700  逐合约把 rt/{ticks,trans,orders,queue} 块压成 his/<type>/<exchg>/<date>/<code>.dsb
//                                 （BlockHeaderV2 + CMP_V2 + compress_data）、_size 归零、
//                                 MARK 分支写 marker.ini、_disable_his 跳过分支、dumper 失败日志
//  - dump_bars_via_dumper :1698-1816  由 protected _dumpers 经薄派生注入替身，覆盖 d1/m1/m5 三段
//  - dump_bars_to_file    :2020-2214  日线快照（经 dump_day_data）+ min1/min5 迁移与二次追加
//
// 同步做法（不要用 release()，理由见夹具 abandon_writer 注释）：
//  transHisData 先 push 全部合约、**最后** push "MARK.<sid>"，proc_loop 按 FIFO 处理并在 MARK 分支
//  写 marker.ini，所以「marker.ini 里出现该 sid」就是队列排空的可靠信号；用例用带上限的轮询等它。
#include "r23_writer_fixture.h"

#include "WTSUtils/WTSCmpHelper.hpp"

namespace r23 {

namespace {

// 读历史 .dsb（压缩形态）并解压成 item 数组；head 是 BlockHeaderV2 或 BlockHeader
template<typename T>
vector<T> load_dsb(const string& file, uint16_t expect_type, uint16_t expect_version)
{
	vector<T> items;
	const string raw = read_file_bytes(file);
	if (raw.size() < sizeof(BlockHeader))
		return items;

	BlockHeaderV2 head;
	std::memset(&head, 0, sizeof(head));
	std::memcpy(&head, raw.data(), sizeof(head));
	if (head._type != expect_type || head._version != expect_version)
		return items;

	string payload;
	if (expect_version == BLOCK_VERSION_CMP_V2)
	{
		const char* body = raw.data() + sizeof(BlockHeaderV2);
		const size_t body_len = raw.size() - sizeof(BlockHeaderV2);
		if (head._size != body_len)
			return items;                  // _size 与实际载荷不符，交给调用方判空
		payload = WTSCmpHelper::uncompress_data(body, body_len);
	}
	else
	{
		payload = raw.substr(sizeof(BlockHeader));
	}

	if (payload.size() % sizeof(T) != 0)
		return items;

	items.resize(payload.size() / sizeof(T));
	if (!items.empty())
		std::memcpy(items.data(), payload.data(), payload.size());
	return items;
}

inline uintmax_t dsb_size(const string& file) { return file_size(file); }

} // namespace

// ------------------------------------------------------------------ 收盘迁移主链
TEST_F(R23WriterTest, TransHisDataMovesRtTicksToCompressedDsb)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec t1;                    // 09:30 3600
	t1.total_volume = 1000.0;
	write_tick(w, t1, 0);

	TickSpec t2;                    // 09:31 3700
	t2.action_time = 93100000;
	t2.price = 3700.0;
	t2.total_volume = 1100.0;
	t2.total_turnover = 3970000.0;
	write_tick(w, t2, 0);

	const string rt = rt_tick_file("rb2210");
	ASSERT_EQ(read_block_header<RTDayBlockHeader>(rt)._size, 2u);

	abandon_writer(w);              // proc_loop 起线程后不再 join（见夹具注释）
	w->transHisData("R23_DAY");
	ASSERT_TRUE(wait_marker("R23_DAY")) << "收盘作业没在 10s 内排空队列";

	const string dsb = his_tick_file("rb2210", 20240108);
	ASSERT_TRUE(file_exists(dsb)) << dsb << " 未生成";
	const BlockHeaderV2 head = read_block_header<BlockHeaderV2>(dsb);
	EXPECT_EQ(std::memcmp(head._blk_flag, BLK_FLAG, sizeof(head._blk_flag)), 0);
	EXPECT_EQ(head._type, (uint16_t)BT_HIS_Ticks);
	EXPECT_EQ(head._version, (uint16_t)BLOCK_VERSION_CMP_V2);
	EXPECT_EQ(dsb_size(dsb), (uintmax_t)(sizeof(BlockHeaderV2) + head._size));

	const vector<WTSTickStruct> ticks = load_dsb<WTSTickStruct>(dsb, BT_HIS_Ticks, BLOCK_VERSION_CMP_V2);
	ASSERT_EQ(ticks.size(), 2u);
	EXPECT_DOUBLE_EQ(ticks[0].price, 3600.0);
	EXPECT_DOUBLE_EQ(ticks[0].total_volume, 1000.0);
	EXPECT_EQ(ticks[0].action_time, 93000000u);
	EXPECT_DOUBLE_EQ(ticks[1].price, 3700.0);
	EXPECT_DOUBLE_EQ(ticks[1].total_volume, 1100.0);
	EXPECT_DOUBLE_EQ(ticks[1].total_turnover, 3970000.0);
	EXPECT_EQ(ticks[1].trading_date, 20240108u);

	// 迁移完必须把 RT 块条数归零（:2477），否则下一条会重复落地
	const RTDayBlockHeader rt_head = read_block_header<RTDayBlockHeader>(rt);
	EXPECT_EQ(rt_head._size, 0u);
	EXPECT_TRUE(_sink->has_log("Transfering tick data of SHFE.rb2210...", LL_INFO));
}

TEST_F(R23WriterTest, TransHisDataWritesMarkerThenNextInitSeesSessionProceeded)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);
	abandon_writer(w);

	TickSpec t1;
	write_tick(w, t1, 0);

	w->transHisData("R23_SESS_A");
	ASSERT_TRUE(wait_marker("R23_SESS_A"));

	const string ini = _base + "marker.ini";
	const string content = read_file_bytes(ini);
	EXPECT_NE(content.find("[markers]"), string::npos) << content;
	EXPECT_NE(content.find("R23_SESS_A"), string::npos) << content;
	EXPECT_TRUE(_sink->has_log("ClosingTask mark of Trading session [R23_SESS_A] updated", LL_INFO));

	// 原 writer 的 _proc_date 是 init 时读的，不会自己更新
	EXPECT_FALSE(w->isSessionProceeded("R23_SESS_A"));

	// 第二个 writer 从同一个 marker.ini 恢复（init:154-164）
	R23Writer* w2 = make_writer();
	ASSERT_TRUE(w2 != NULL);
	abandon_writer(w2);
	EXPECT_TRUE(w2->isSessionProceeded("R23_SESS_A"));
	EXPECT_FALSE(w2->isSessionProceeded("R23_SESS_B"));
}

TEST_F(R23WriterTest, TransHisDataWithUnknownSessionStaysSilent)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);
	abandon_writer(w);

	TickSpec t1;
	write_tick(w, t1, 0);

	// getSessionComms 返回 NULL → :1575 直接 return，既不建线程也不写任何文件
	_sink->set_comms_null(true);
	w->transHisData("R23_NOPE");
	usleep(200000);                 // 给（错误地）被启动的线程一点时间
	EXPECT_STREQ(_sink->last_sid().c_str(), "R23_NOPE");
	EXPECT_FALSE(file_exists(_base + "marker.ini"));
	EXPECT_FALSE(file_exists(his_tick_file("rb2210", 20240108)));

	// 对照组：同样的 writer 只要品种表能查到，MARK 就会落地
	_sink->set_comms_null(false);
	w->transHisData("R23_OK");
	ASSERT_TRUE(wait_marker("R23_OK"));
	const string content = read_file_bytes(_base + "marker.ini");
	EXPECT_NE(content.find("R23_OK"), string::npos);
	EXPECT_EQ(content.find("R23_NOPE"), string::npos) << "早退分支也写了标记";
}

TEST_F(R23WriterTest, SecondClosingRunAppendsMinBarsAndReplacesDayBar)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);
	abandon_writer(w);

	TickSpec t1;                    // 09:30 → min1 标签 931
	t1.price = 3600.0;
	t1.total_volume = 1000.0;
	t1.total_turnover = 3600000.0;
	write_tick(w, t1, 0);

	TickSpec t2;                    // 09:31 → 932
	t2.action_time = 93100000;
	t2.price = 3620.0;
	t2.total_volume = 1100.0;
	t2.total_turnover = 3982000.0;
	write_tick(w, t2, 0);

	w->transHisData("R23_ROUND1");
	ASSERT_TRUE(wait_marker("R23_ROUND1"));

	vector<WTSBarStruct> m1 = load_dsb<WTSBarStruct>(his_min1_file("rb2210"),
		BT_HIS_Minute1, BLOCK_VERSION_CMP_V2);
	ASSERT_EQ(m1.size(), 2u);
	EXPECT_EQ(m1[0].time % 10000, 931u);
	EXPECT_EQ(m1[1].time % 10000, 932u);

	vector<WTSBarStruct> day = load_dsb<WTSBarStruct>(his_day_file("rb2210"),
		BT_HIS_Day, BLOCK_VERSION_RAW_V2);
	ASSERT_EQ(day.size(), 1u);
	// 日线来自缓存里最后一条 tick（dump_bars_to_file:2040-2053）
	EXPECT_DOUBLE_EQ(day[0].open, 3590.0);
	EXPECT_DOUBLE_EQ(day[0].close, 3620.0);
	EXPECT_DOUBLE_EQ(day[0].high, 3610.0);
	EXPECT_DOUBLE_EQ(day[0].low, 3580.0);
	EXPECT_DOUBLE_EQ(day[0].settle, 3595.0);
	EXPECT_DOUBLE_EQ(day[0].vol, 1100.0);
	EXPECT_DOUBLE_EQ(day[0].money, 3982000.0);
	EXPECT_DOUBLE_EQ(day[0].hold, 90000.0);
	EXPECT_DOUBLE_EQ(day[0].add, 10.0);
	EXPECT_EQ(day[0].date, 20240108u);
	EXPECT_EQ(day[0].time, 0u);

	// 第二轮：09:32 一根新 bar + 同一交易日的新快照
	TickSpec t3;
	t3.action_time = 93200000;
	t3.price = 3630.0;
	t3.total_volume = 1200.0;
	t3.total_turnover = 4356000.0;
	write_tick(w, t3, 0);

	w->transHisData("R23_ROUND2");
	ASSERT_TRUE(wait_marker("R23_ROUND2"));

	m1 = load_dsb<WTSBarStruct>(his_min1_file("rb2210"), BT_HIS_Minute1, BLOCK_VERSION_CMP_V2);
	ASSERT_EQ(m1.size(), 3u) << "min1 历史文件没有把新一轮的 bar 追加进去";
	EXPECT_EQ(m1[0].time % 10000, 931u);
	EXPECT_EQ(m1[1].time % 10000, 932u);
	EXPECT_EQ(m1[2].time % 10000, 933u);
	EXPECT_DOUBLE_EQ(m1[2].open, 3630.0);

	const vector<WTSBarStruct> day2 = load_dsb<WTSBarStruct>(his_day_file("rb2210"),
		BT_HIS_Day, BLOCK_VERSION_RAW_V2);
	// 同日期不同值 → 替换最后一条而不是追加（dump_day_data:1963-1967）
	ASSERT_EQ(day2.size(), 1u);
	EXPECT_DOUBLE_EQ(day2[0].close, 3630.0);
	EXPECT_DOUBLE_EQ(day2[0].vol, 1200.0);
	EXPECT_DOUBLE_EQ(day2[0].money, 4356000.0);
}

// ------------------------------------------------------------------ 扩展 dumper
TEST_F(R23WriterTest, ExtendedDumperReceivesDayMin1Min5BarsAndTicks)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	FakeDumper dumper;
	w->add_dumper("r23", &dumper);

	TickSpec t1;
	t1.price = 3600.0;
	t1.total_volume = 1000.0;
	t1.total_turnover = 3600000.0;
	write_tick(w, t1, 0);

	TickSpec t2;
	t2.action_time = 93100000;
	t2.price = 3700.0;
	t2.total_volume = 1100.0;
	t2.total_turnover = 3970000.0;
	write_tick(w, t2, 0);

	// 逐笔成交也来一条，验证 trans 走 dumper
	WTSTransData* trns = make_trans_data(93000000, 3650.0, 7);
	ASSERT_TRUE(w->writeTransaction(trns));

	abandon_writer(w);
	w->transHisData("R23_DUMP");
	ASSERT_TRUE(wait_marker("R23_DUMP"));

	// proc_loop 的 tick 分支把整块交给 dumper（:2436）
	EXPECT_EQ(dumper.tick_calls(), 1u);
	EXPECT_STREQ(dumper.tick_key().c_str(), "SHFE.rb2210");
	EXPECT_EQ(dumper.tick_date(), 20240108u);
	EXPECT_EQ(dumper.tick_count(), 2u);
	EXPECT_DOUBLE_EQ(dumper.first_tick().price, 3600.0);

	// 逐笔成交分支（:2504）
	EXPECT_EQ(dumper.trns_calls(), 1u);
	EXPECT_EQ(dumper.trns_count(), 1u);

	// dump_bars_via_dumper 三段：d1 一条、m1 两条、m5 一条
	EXPECT_EQ(dumper.bar_count_of("d1"), 1u);
	EXPECT_EQ(dumper.bar_count_of("m1"), 2u);
	EXPECT_EQ(dumper.bar_count_of("m5"), 1u);
	ASSERT_EQ(dumper.last_bars().size(), 3u);
	EXPECT_DOUBLE_EQ(dumper.last_bars()[0].close, 3700.0);      // d1 用最后一条 tick
	EXPECT_EQ(dumper.last_bars()[0].time, 0u);
	EXPECT_EQ(dumper.last_bars()[1].time % 10000, 931u);        // m1 首根

	// dumper 收完以后，文件落地路径不受影响
	EXPECT_TRUE(file_exists(his_tick_file("rb2210", 20240108)));
	EXPECT_TRUE(file_exists(his_min1_file("rb2210")));
	EXPECT_TRUE(file_exists(his_day_file("rb2210")));
}

TEST_F(R23WriterTest, ExtendedDumperFailureOnlyLogsError)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	FakeDumper dumper;
	dumper.set_ret(false);
	w->add_dumper("r23bad", &dumper);

	TickSpec t1;
	write_tick(w, t1, 0);

	abandon_writer(w);
	w->transHisData("R23_DUMP_FAIL");
	ASSERT_TRUE(wait_marker("R23_DUMP_FAIL"));

	EXPECT_TRUE(_sink->has_log("ClosingTask of tick of SHFE.rb2210 on 20240108 via extended dumper r23bad failed", LL_ERROR));
	EXPECT_TRUE(_sink->has_log("Closing Task of day bar of SHFE.rb2210 failed via extended dumper r23bad", LL_ERROR));
	EXPECT_TRUE(_sink->has_log("Closing Task of m1 bar of SHFE.rb2210 failed via extended dumper r23bad", LL_ERROR));

	// 失败不阻断文件落地
	EXPECT_TRUE(file_exists(his_tick_file("rb2210", 20240108)));
	const vector<WTSTickStruct> ticks = load_dsb<WTSTickStruct>(
		his_tick_file("rb2210", 20240108), BT_HIS_Ticks, BLOCK_VERSION_CMP_V2);
	EXPECT_EQ(ticks.size(), 1u);
}

TEST_F(R23WriterTest, DisabledHistorySkipsClosingWorkButStillMarks)
{
	R23Writer* w = make_writer("\"disablehis\":true");
	ASSERT_TRUE(w != NULL);
	EXPECT_TRUE(_sink->has_log("disable_history: true", LL_INFO));

	TickSpec t1;
	write_tick(w, t1, 0);
	ASSERT_TRUE(file_exists(rt_tick_file("rb2210")));

	abandon_writer(w);
	w->transHisData("R23_NOHIS");
	ASSERT_TRUE(wait_marker("R23_NOHIS"));

	EXPECT_TRUE(_sink->has_log("skipped due to history data disabled", LL_INFO));
	EXPECT_FALSE(file_exists(his_tick_file("rb2210", 20240108)));
	EXPECT_FALSE(file_exists(his_day_file("rb2210")));
	EXPECT_FALSE(file_exists(his_min1_file("rb2210")));
	// RT 块内容也没被动过
	EXPECT_EQ(read_block_header<RTDayBlockHeader>(rt_tick_file("rb2210"))._size, 1u);
	// 标记分支在 _disable_his 判断之外，仍然写
	EXPECT_TRUE(file_exists(_base + "marker.ini"));
}

TEST_F(R23WriterTest, GetCurTickSeesCacheWrittenBeforeClosing)
{
	// getCurTick 的命中面已在 r23_writer_tick_cache_test.cpp 覆盖；
	// 这里补的是「收盘作业把 RT tick 块清零之后，缓存项依然保留」这条事实
	// （_tick_cache_idx / cache.dmb 不参与 transHisData 的清理）。
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec t1;
	t1.price = 3600.0;
	write_tick(w, t1, 0);

	abandon_writer(w);
	w->transHisData("R23_CACHE");
	ASSERT_TRUE(wait_marker("R23_CACHE"));

	WTSTickData* cached = w->getCurTick("rb2210", "SHFE");
	ASSERT_TRUE(cached != NULL);
	EXPECT_DOUBLE_EQ(cached->price(), 3600.0);
	cached->release();

	const RTBlockHeader head = read_block_header<RTBlockHeader>(cache_file());
	EXPECT_EQ(head._size, 1u);
}

// ------------------------------------------------------------------ 崩溃/挂死收口
TEST_F(R23WriterTest, DISABLED_ClearCacheCommandMaySleepThreeHundredSeconds)
{
	// 复现（不要启用）：transHisData(CMD_CLEAR_CACHE) 进 proc_loop 的清理分支（:2240-2390）。
	// 该分支会 remove_all 六个 rt/* 目录，且一旦抛异常就 `sleep_for(300s)` 重试（:2385），
	// 最坏情况 5 次重试 = 1500s，直接撞 60s 的 HANG 门禁；
	// 另外 :2323-2335 先 `_tick_cache_file->close()` 把 `_tick_cache_block` 置 NULL，
	// 再 map 新文件，两次 map 之间主线程若并发调 updateCache/getCurTick 就是裸指针解引用。
	// 需要「可控失败的目录」才能安全覆盖，本车道预算内不做。
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec t1;
	write_tick(w, t1, 0);
	_bd->forget_contract("rb2210");     // 让缓存里的代码变成「已过期」，走 diff 重建分支

	abandon_writer(w);
	w->transHisData("CMD_CLEAR_CACHE");
	usleep(500000);

	EXPECT_TRUE(file_exists(_base + "his/snapshot/"));
	EXPECT_FALSE(file_exists(rt_tick_file("rb2210")));
}

} // namespace r23
