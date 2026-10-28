// r23_storage4 / WtDataStorage：WtDataWriter 走通 init 之后的「同步 tick 落盘链」
//
// 覆盖对象（src/WtDataStorage/WtDataWriter.cpp，全部经公开入口 init/writeTick/getCurTick 驱动，
// 不使用 #define private public）：
//  - init               :117-175  参数解析（savelog/cache/async/groupsize/disable*/minbar_price_mode）
//                                  + marker.ini 读回 :154-164 + loadCache + 收尾日志
//  - loadCache          :267-310  新建 cache.dmb（容量=getContractSize）与「已映射直接返回」与
//                                  「已存在文件重建索引」两条分支
//  - resizeRTBlock      :312-364  由 cache.dmb 首次越界触发（append + 重新映射 + 回写 capacity）
//  - writeTick/procTick :366-414  NULL 拒绝、async=false 走同步 procTick、canSessionReceive 过滤、
//                                  updateCache 失败即 break、_disable_tick 开关、groupsize 计数日志
//  - updateCache        :1438-1567 全部 8 条分支（新交易日/procFlag 派生/交易日倒退/
//                                  时段偏移日期超前/总量回退/同时间戳进位/新高新低/procFlag=1 增量）
//  - getCurTick         :1418-1436  空代码、未知合约、未入缓存三种拒绝 + 命中读回
//  - pipeToTicks        :624-666  含 _save_tick_log 的 csv 逐笔落地分支（p10 只走了 savelog=false）
//
// 落盘：std::filesystem/temp_directory_path()/r23_w_<pid>_<seq>，TearDown release()+remove_all。
// 线程：init 起的 check_loop 不可 join，故 writer 只 release 不 delete（见夹具头注释）。
#include "r23_writer_fixture.h"

namespace r23 {

// ------------------------------------------------------------------ init/loadCache
TEST_F(R23WriterTest, InitCreatesCacheDmbSizedByContractCount)
{
	_bd->set_contract_size(3);
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	const string cf = cache_file();
	ASSERT_TRUE(file_exists(cf)) << cf << " 未生成";
	EXPECT_EQ(file_size(cf), (uintmax_t)(sizeof(RTTickCache) + 3 * sizeof(TickCacheItem)));

	const RTBlockHeader head = read_block_header<RTBlockHeader>(cf);
	EXPECT_EQ(std::memcmp(head._blk_flag, BLK_FLAG, sizeof(head._blk_flag)), 0);
	EXPECT_EQ(head._type, (uint32_t)BT_RT_Cache);
	EXPECT_EQ(head._version, 1u);
	EXPECT_EQ(head._size, 0u);
	EXPECT_EQ(head._capacity, 3u);

	// loadCache 只在第一次 init 里问一次品种总数
	EXPECT_EQ(_bd->size_calls(), 1u);

	// init 收尾日志（WtDataWriter.cpp:170）逐项核对参数回显
	EXPECT_TRUE(_sink->has_log("WtDataWriter initialized", LL_INFO));
	EXPECT_TRUE(_sink->has_log("async_mode: false", LL_INFO));
	EXPECT_TRUE(_sink->has_log("log_group_size: 1", LL_INFO));
	EXPECT_TRUE(_sink->has_log("save_csv_tick: false", LL_INFO));
}

TEST_F(R23WriterTest, InitReadsMarkerSectionAndReportsProceededByDate)
{
	// 收盘标记由 proc_loop 写、由 init 读回（:154-164）；这里手工造两份标记：一个未来日期、一个过去日期
	const string ini = _base + "marker.ini";
	{
		std::ofstream ofs(ini);
		ofs << "[markers]\nR23FUTURE=20991231\nR23PAST=20200101\n";
		ofs.close();
	}

	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	// _proc_date 命中且 >= 今日 → true；命中但在过去 → false；未命中 → false
	EXPECT_TRUE(w->isSessionProceeded("R23FUTURE"));
	EXPECT_FALSE(w->isSessionProceeded("R23PAST"));
	EXPECT_FALSE(w->isSessionProceeded("R23ABSENT"));
}

TEST_F(R23WriterTest, DISABLED_SecondInitOnSameWriterTerminatesProcess)
{
	// 复现（不要启用）：第二次 init 走到 :168 `_proc_chk.reset(new StdThread(...))`，
	// 旧 StdThread(=std::shared_ptr<std::thread>) 引用归零 → ~std::thread 对一个仍 joinable
	// （正睡在 :1621 sleep_for(10s) 里）的线程执行 → 按标准直接 std::terminate()，进程当场死。
	// 生产里 init 只被 WtDtServo 调一次，所以这条路径没人踩过；本车道把它收进 DISABLED_，
	// 启用集里 loadCache 的「已映射即返回」短路（:269-270）因此留白。
	_bd->set_contract_size(2);
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);
	const uintmax_t sz0 = file_size(cache_file());
	const uint32_t calls0 = _bd->size_calls();

	const string cfg_file = write_params();
	WTSVariant* cfg = WTSCfgLoader::load_from_file(cfg_file.c_str());
	ASSERT_TRUE(cfg != NULL);
	EXPECT_TRUE(w->init(cfg, _sink.get()));   // ← 这里 std::terminate
	cfg->release();

	// 理想行为：loadCache 短路，不再问第二遍品种数、不重建文件
	EXPECT_EQ(_bd->size_calls(), calls0);
	EXPECT_EQ(file_size(cache_file()), sz0);
}

TEST_F(R23WriterTest, LoadCacheReindexesItemsFromExistingFile)
{
	_bd->set_contract_size(4);

	TickSpec spec;
	spec.price = 3611.0;
	spec.total_volume = 1234.0;
	R23Writer* w1 = make_writer();
	ASSERT_TRUE(w1 != NULL);
	write_tick(w1, spec, 0);
	close_writer(w1);          // 解除映射，让第二个 writer 走「文件已存在」分支

	R23Writer* w2 = make_writer();
	ASSERT_TRUE(w2 != NULL);
	EXPECT_EQ(_bd->size_calls(), 2u);

	WTSTickData* back = w2->getCurTick("rb2210", "SHFE");
	ASSERT_TRUE(back != NULL) << "loadCache 没有把 cache.dmb 里的既有条目重建进 _tick_cache_idx";
	EXPECT_DOUBLE_EQ(back->price(), 3611.0);
	EXPECT_DOUBLE_EQ(back->totalvolume(), 1234.0);
	back->release();

	// 索引重建后新合约应该接在既有 _size 之后（第二条槽位，而不是覆盖第 0 条）
	TickSpec spec2;
	spec2.code = "rb2211";
	spec2.price = 3612.0;
	write_tick(w2, spec2, 0);
	const RTBlockHeader head = read_block_header<RTBlockHeader>(cache_file());
	EXPECT_EQ(head._size, 2u);

	WTSTickData* b2 = w2->getCurTick("rb2211", "SHFE");
	ASSERT_TRUE(b2 != NULL);
	EXPECT_DOUBLE_EQ(b2->price(), 3612.0);
	b2->release();
}

// ------------------------------------------------------------------ getCurTick 拒绝面
TEST_F(R23WriterTest, GetCurTickRejectsEmptyCodeAndUnknownAndUncached)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	// strlen(code)==0（:1420）
	EXPECT_TRUE(w->getCurTick("") == NULL);

	// getContract 查不到（:1424）
	EXPECT_TRUE(w->getCurTick("rb9999", "SHFE") == NULL);

	// 合约存在但从未进过缓存（:1430 _tick_cache_idx 未命中）
	EXPECT_TRUE(w->getCurTick("rb2211", "SHFE") == NULL);

	TickSpec spec;
	spec.price = 3620.0;
	write_tick(w, spec, 0);

	WTSTickData* hit = w->getCurTick("rb2210", "SHFE");
	ASSERT_TRUE(hit != NULL);
	EXPECT_DOUBLE_EQ(hit->price(), 3620.0);
	hit->release();
}

TEST_F(R23WriterTest, WriteTickNullReturnsFalse)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	// writeTick 开头 curTick==NULL（:368）
	EXPECT_FALSE(w->writeTick(NULL, 0));
	// procTick 里 ct==NULL 直接 break（:384）：不崩、仍然返回 true，且不产生任何落盘
	WTSTickStruct ts;
	std::memset(&ts, 0, sizeof(ts));
	snprintf(ts.exchg, sizeof(ts.exchg), "%s", "SHFE");
	snprintf(ts.code, sizeof(ts.code), "%s", "rb2210");
	ts.price = 3600.0;
	ts.trading_date = 20240108;
	ts.action_date = 20240108;
	ts.action_time = 93000000;
	WTSTickData* naked = WTSTickData::create(ts);
	naked->setContractInfo(NULL);
	EXPECT_TRUE(w->writeTick(naked, 0));
	naked->release();

	EXPECT_EQ(_sink->ticks(), 0u);
	EXPECT_FALSE(file_exists(rt_tick_file("rb2210")));
}

// ------------------------------------------------------------------ updateCache 分支矩阵
TEST_F(R23WriterTest, FirstTickOfTradingdaySeedsCacheAndRaisesNewHighLow)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec spec;
	spec.price = 3611.5;
	spec.open = 3600.0;
	spec.high = 3620.0;
	spec.low = 3590.0;
	spec.total_volume = 1000.0;
	spec.total_turnover = 3611500.0;
	spec.open_interest = 90000.0;
	spec.pre_interest = 89500.0;
	WTSTickData* d = write_tick(w, spec, 0);
	ASSERT_TRUE(d != NULL);

	// 新交易日：整日新高新低被置位（:1498 setLimitFlag(1|2)）
	EXPECT_TRUE(d->isNewHigh());
	EXPECT_TRUE(d->isNewLow());
	// procFlag==0 → 不派生增量，volume 仍是原值
	EXPECT_DOUBLE_EQ(d->volume(), 10.0);
	EXPECT_TRUE(_sink->has_log("First tick of new tradingday", LL_INFO));

	WTSTickData* cached = w->getCurTick("rb2210", "SHFE");
	ASSERT_TRUE(cached != NULL);
	EXPECT_DOUBLE_EQ(cached->price(), 3611.5);
	EXPECT_DOUBLE_EQ(cached->open(), 3600.0);
	EXPECT_DOUBLE_EQ(cached->high(), 3620.0);
	EXPECT_DOUBLE_EQ(cached->low(), 3590.0);
	EXPECT_DOUBLE_EQ(cached->totalvolume(), 1000.0);
	EXPECT_DOUBLE_EQ(cached->openinterest(), 90000.0);
	// getCurTick 是新建对象，limit flag 不会随缓存项带出来
	EXPECT_EQ(cached->getLimitFlag(), 0u);
	cached->release();

	// 缓存项里的日期槽位
	TickCacheItem item;
	{
		const bool got_item = read_block_item<RTBlockHeader, TickCacheItem>(cache_file(), 0, item);
		ASSERT_TRUE(got_item);
	}
	EXPECT_EQ(item._date, 20240108u);
	EXPECT_DOUBLE_EQ(item._tick.price, 3611.5);
	EXPECT_DOUBLE_EQ(item._tick.settle_price, spec.settle);
	EXPECT_DOUBLE_EQ(item._tick.upper_limit, 3800.0);
	EXPECT_DOUBLE_EQ(item._tick.pre_close, 3585.0);
	EXPECT_EQ(item._tick.action_time, 93000000u);

	// RT tick 文件块同步落一条
	const RTDayBlockHeader blk = read_block_header<RTDayBlockHeader>(rt_tick_file("rb2210"));
	EXPECT_EQ(blk._size, 1u);
	EXPECT_EQ(blk._capacity, 2500u);
	EXPECT_EQ(blk._type, (uint32_t)BT_RT_Ticks);
	EXPECT_EQ(blk._date, 20240108u);
	WTSTickStruct from_file;
	{
		const bool got_ts = read_block_item<RTDayBlockHeader, WTSTickStruct>(rt_tick_file("rb2210"), 0, from_file);
		ASSERT_TRUE(got_ts);
	}
	EXPECT_DOUBLE_EQ(from_file.price, 3611.5);
	EXPECT_DOUBLE_EQ(from_file.total_volume, 1000.0);
	EXPECT_STREQ(from_file.code, "rb2210");
}

TEST_F(R23WriterTest, ProcFlagOneDerivesIncrementalVolumeTurnoverInterest)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec t1;
	t1.total_volume = 1000.0;
	t1.total_turnover = 3600000.0;
	t1.open_interest = 90000.0;
	t1.pre_interest = 89990.0;
	WTSTickData* d1 = write_tick(w, t1, 1);
	// 新交易日 + procFlag==1：当日首笔的 volume/turn_over 直接取总量（:1481-1490）
	EXPECT_DOUBLE_EQ(d1->volume(), 1000.0);
	EXPECT_DOUBLE_EQ(d1->turnover(), 3600000.0);
	EXPECT_DOUBLE_EQ(d1->additional(), 10.0);

	TickSpec t2;
	t2.action_time = 93100000;
	t2.price = 3650.0;
	t2.high = 3655.0;          // 比缓存 3610 高 → 新高
	t2.low = 3585.0;           // 比缓存 3580 高 → 不是新低
	t2.total_volume = 1200.0;
	t2.total_turnover = 4320000.0;
	t2.open_interest = 90050.0;
	WTSTickData* d2 = write_tick(w, t2, 1);

	// procFlag==1 的增量派生（:1556-1558）
	EXPECT_DOUBLE_EQ(d2->volume(), 200.0);
	EXPECT_DOUBLE_EQ(d2->turnover(), 720000.0);
	EXPECT_DOUBLE_EQ(d2->additional(), 50.0);
	// 新高=1、新低=0（:1542-1547）
	EXPECT_EQ(d2->getLimitFlag(), 1u);
	EXPECT_TRUE(d2->isNewHigh());
	EXPECT_FALSE(d2->isNewLow());

	WTSTickData* cached = w->getCurTick("rb2210", "SHFE");
	ASSERT_TRUE(cached != NULL);
	EXPECT_DOUBLE_EQ(cached->totalvolume(), 1200.0);
	EXPECT_DOUBLE_EQ(cached->volume(), 200.0);
	EXPECT_DOUBLE_EQ(cached->openinterest(), 90050.0);
	cached->release();
}

TEST_F(R23WriterTest, TickWithOlderTradingdayIsDroppedBeforeAnyWrite)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec t1;
	t1.price = 3600.0;
	write_tick(w, t1, 0);
	const uint32_t logs_before = _sink->total();
	ASSERT_TRUE(logs_before > 0);

	TickSpec t2;
	t2.trading_date = 20240107;     // 比缓存的 20240108 早
	t2.action_date = 20240107;
	t2.price = 3700.0;
	WTSTickData* d2 = write_tick(w, t2, 0);
	ASSERT_TRUE(d2 != NULL);

	// updateCache 返回 false → procTick 直接 break：不 broadcast、不写 RT 块
	EXPECT_TRUE(_sink->has_log("is less than cached tradingday", LL_INFO));
	EXPECT_EQ(_sink->ticks(), 1u) << "被丢弃的 tick 仍然走了 broadcastTick";
	const RTDayBlockHeader blk = read_block_header<RTDayBlockHeader>(rt_tick_file("rb2210"));
	EXPECT_EQ(blk._size, 1u);

	WTSTickData* cached = w->getCurTick("rb2210", "SHFE");
	ASSERT_TRUE(cached != NULL);
	EXPECT_DOUBLE_EQ(cached->price(), 3600.0);
	cached->release();
}

TEST_F(R23WriterTest, NightSessionOffsetDateAheadOfTradingdayAbandonsTick)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	// fn2201 挂在偏移 180 分钟的夜盘时段上：21:00 + 180min == 1440 → 偏移日期跨到次日
	TickSpec t1;
	t1.code = "fn2201";
	t1.trading_date = 20240108;
	t1.action_date = 20240108;
	t1.action_time = 210000000;
	t1.total_volume = 1000.0;
	WTSTickData* d1 = write_tick(w, t1, 0);
	ASSERT_TRUE(d1 != NULL);
	// 首条走「新交易日」分支，不做偏移日期校验
	EXPECT_TRUE(w->getCurTick("fn2201", "SHFE") != NULL);

	TickSpec t2;
	t2.code = "fn2201";
	t2.trading_date = 20240108;         // 仍声称是 08 日的交易日
	t2.action_date = 20240108;
	t2.action_time = 210000000;
	t2.total_volume = 1100.0;
	t2.price = 3700.0;
	WTSTickData* d2 = write_tick(w, t2, 0);
	ASSERT_TRUE(d2 != NULL);

	// getOffsetDate(20240108, 2100) == 20240109 > trading_date → 丢弃（:1512-1516）
	EXPECT_TRUE(_sink->has_log("has an exception, abandoned", LL_WARN));
	const RTDayBlockHeader blk = read_block_header<RTDayBlockHeader>(rt_tick_file("fn2201"));
	EXPECT_EQ(blk._size, 1u);

	WTSTickData* cached = w->getCurTick("fn2201", "SHFE");
	ASSERT_TRUE(cached != NULL);
	EXPECT_DOUBLE_EQ(cached->price(), 3600.0);
	cached->release();
}

TEST_F(R23WriterTest, TotalVolumeRollbackAbandonsTick)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec t1;
	t1.total_volume = 1000.0;
	t1.price = 3600.0;
	write_tick(w, t1, 0);

	TickSpec t2;
	t2.action_time = 93100000;
	t2.total_volume = 900.0;        // 比缓存小 → 认为是回退
	t2.price = 3700.0;
	write_tick(w, t2, 0);

	EXPECT_TRUE(_sink->has_log("is less than cached volume", LL_WARN));
	const RTDayBlockHeader blk = read_block_header<RTDayBlockHeader>(rt_tick_file("rb2210"));
	EXPECT_EQ(blk._size, 1u);

	WTSTickData* cached = w->getCurTick("rb2210", "SHFE");
	ASSERT_TRUE(cached != NULL);
	EXPECT_DOUBLE_EQ(cached->totalvolume(), 1000.0);
	EXPECT_DOUBLE_EQ(cached->price(), 3600.0);
	cached->release();
}

TEST_F(R23WriterTest, SameOrEarlierTimestampGetsCentisecondBumpChain)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	// 缓存槽位先落在 x500 → 同时间戳再来一笔（总量更大）→ +min((999-500)/50*50, 200) = +200
	TickSpec a;
	a.action_time = 93000500;
	a.total_volume = 1000.0;
	write_tick(w, a, 0);

	TickSpec b = a;
	b.action_time = 93000500;
	b.total_volume = 1100.0;
	WTSTickData* d_b = write_tick(w, b, 0);
	EXPECT_EQ(d_b->actiontime(), 93000700u) << "毫秒余量 450 应被 200 上限夹住";

	// 缓存现在在 x700：余量 (999-700)/50*50 = 250 → 仍夹到 200
	TickSpec c = a;
	c.action_time = 93000700;
	c.total_volume = 1200.0;
	WTSTickData* d_c = write_tick(w, c, 0);
	EXPECT_EQ(d_c->actiontime(), 93000900u);

	// 缓存 x900：余量 (999-900)/50*50 = 50 → 只加 50
	TickSpec e = a;
	e.action_time = 93000900;
	e.total_volume = 1300.0;
	WTSTickData* d_e = write_tick(w, e, 0);
	EXPECT_EQ(d_e->actiontime(), 93000950u);

	// 缓存 x950：余量 (999-950)/50*50 = 0 → 不进位
	TickSpec f = a;
	f.action_time = 93000950;
	f.total_volume = 1400.0;
	WTSTickData* d_f = write_tick(w, f, 0);
	EXPECT_EQ(d_f->actiontime(), 93000950u);

	WTSTickData* cached = w->getCurTick("rb2210", "SHFE");
	ASSERT_TRUE(cached != NULL);
	EXPECT_EQ(cached->actiontime(), 93000950u);
	EXPECT_DOUBLE_EQ(cached->totalvolume(), 1400.0);
	cached->release();

	// 5 笔都进了 RT tick 块（丢弃路径不会增加）
	const RTDayBlockHeader blk = read_block_header<RTDayBlockHeader>(rt_tick_file("rb2210"));
	EXPECT_EQ(blk._size, 5u);
}

// ------------------------------------------------------------------ resizeRTBlock
TEST_F(R23WriterTest, CacheGrowsByStepWhenCapacityExhausted)
{
	_bd->set_contract_size(1);      // 只给 1 个槽位，第一条 tick 就会触发扩容
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec t1;
	t1.code = "rb2210";
	t1.price = 3600.0;
	write_tick(w, t1, 0);

	// CACHE_SIZE_STEP=200 → 1+200=201（:1457 + resizeRTBlock:313-364）
	const RTBlockHeader head = read_block_header<RTBlockHeader>(cache_file());
	EXPECT_EQ(head._capacity, 201u);
	EXPECT_EQ(head._size, 1u);
	EXPECT_EQ(file_size(cache_file()),
		(uintmax_t)(sizeof(RTTickCache) + 201 * sizeof(TickCacheItem)));
	EXPECT_TRUE(_sink->has_log("Tick Cache resized to 201 items", LL_INFO));

	// 第二个合约不再触发扩容，且落在第 1 号槽位
	TickSpec t2;
	t2.code = "rb2211";
	t2.price = 3601.0;
	write_tick(w, t2, 0);

	const RTBlockHeader head2 = read_block_header<RTBlockHeader>(cache_file());
	EXPECT_EQ(head2._capacity, 201u);
	EXPECT_EQ(head2._size, 2u);

	TickCacheItem slot1;
	{
		const bool got_slot = read_block_item<RTBlockHeader, TickCacheItem>(cache_file(), 1, slot1);
		ASSERT_TRUE(got_slot);
	}
	EXPECT_DOUBLE_EQ(slot1._tick.price, 3601.0);

	WTSTickData* a = w->getCurTick("rb2210", "SHFE");
	WTSTickData* b = w->getCurTick("rb2211", "SHFE");
	ASSERT_TRUE(a != NULL);
	ASSERT_TRUE(b != NULL);
	EXPECT_DOUBLE_EQ(a->price(), 3600.0);
	EXPECT_DOUBLE_EQ(b->price(), 3601.0);
	a->release();
	b->release();
}

// ------------------------------------------------------------------ savelog / 开关
TEST_F(R23WriterTest, SaveTickLogWritesCommaSeparatedRows)
{
	R23Writer* w = make_writer("\"savelog\":true");
	ASSERT_TRUE(w != NULL);
	EXPECT_TRUE(_sink->has_log("save_csv_tick: true", LL_INFO));

	TickSpec spec;
	spec.price = 3611.5;
	spec.total_volume = 1000.0;
	spec.open_interest = 90000.0;
	spec.total_turnover = 3611500.0;
	spec.volume = 12.0;
	spec.diff_interest = 7.0;
	write_tick(w, spec, 0);
	write_tick(w, spec, 0);
	// csv 是 ofstream（app 模式）写的，带缓冲；release() 会 delete 块对象从而关掉流
	close_writer(w);

	// 文件名用的是 getTickBlock 的 curDate 参数 = tick.tradingdate()
	const string csv = _base + "rt/ticks/SHFE/rb2210.20240108.csv";
	ASSERT_TRUE(file_exists(csv)) << csv << " 未生成";
	const string content = read_file_bytes(csv);

	uint32_t lines = 0;
	for (size_t i = 0; i < content.size(); i++)
		if (content[i] == '\n')
			lines++;
	EXPECT_EQ(lines, 2u);

	const StringVector ay = StrUtil::split(content, "\n");
	ASSERT_GE(ay.size(), 1u);
	// code,tradingdate,actiondate,actiontime,localtime,price,totalvolume,openinterest,
	// totalturnover,volume,additional,turnover
	const StringVector cols = StrUtil::split(ay[0], ",");
	ASSERT_EQ(cols.size(), 12u);
	EXPECT_STREQ(cols[0].c_str(), "rb2210");
	EXPECT_STREQ(cols[1].c_str(), "20240108");
	EXPECT_STREQ(cols[2].c_str(), "20240108");
	EXPECT_STREQ(cols[3].c_str(), "93000000");
	EXPECT_STREQ(cols[5].c_str(), "3611.5");
	EXPECT_STREQ(cols[6].c_str(), "1000");
	EXPECT_STREQ(cols[7].c_str(), "90000");
	EXPECT_STREQ(cols[8].c_str(), "3611500");
	EXPECT_STREQ(cols[9].c_str(), "12");
	EXPECT_STREQ(cols[10].c_str(), "7");
}

TEST_F(R23WriterTest, DisableTickStillFeedsCacheAndKlines)
{
	R23Writer* w = make_writer("\"disabletick\":true");
	ASSERT_TRUE(w != NULL);
	EXPECT_TRUE(_sink->has_log("disable_tick: true", LL_INFO));

	TickSpec spec;
	spec.price = 3600.0;
	write_tick(w, spec, 0);

	// pipeToTicks 被开关挡掉 → 不建 rt/ticks 文件（:398-399）
	EXPECT_FALSE(file_exists(rt_tick_file("rb2210")));
	// 但缓存与分钟线仍然照常
	const RTBlockHeader head = read_block_header<RTBlockHeader>(cache_file());
	EXPECT_EQ(head._size, 1u);
	EXPECT_TRUE(file_exists(rt_min1_file("rb2210")));
	WTSTickData* cached = w->getCurTick("rb2210", "SHFE");
	ASSERT_TRUE(cached != NULL);
	EXPECT_DOUBLE_EQ(cached->price(), 3600.0);
	cached->release();
}

TEST_F(R23WriterTest, SessionNotReceivingStopsEverythingBeforeCache)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);
	_sink->set_can_receive(false);

	TickSpec spec;
	write_tick(w, spec, 0);

	// procTick 的 canSessionReceive 过滤（:390）→ 缓存、RT 块、broadcast 全部不动
	EXPECT_EQ(_sink->ticks(), 0u);
	const RTBlockHeader head = read_block_header<RTBlockHeader>(cache_file());
	EXPECT_EQ(head._size, 0u);
	EXPECT_FALSE(file_exists(rt_tick_file("rb2210")));
	EXPECT_FALSE(file_exists(rt_min1_file("rb2210")));
	EXPECT_TRUE(w->getCurTick("rb2210", "SHFE") == NULL);
}

TEST_F(R23WriterTest, LogGroupSizeControlsReceiptCounterLogging)
{
	// groupsize=3 → 只有第 3 笔打日志（:409 cnt % _log_group_size == 0）
	R23Writer* w = make_writer("\"groupsize\":3");
	ASSERT_TRUE(w != NULL);

	const uint32_t info_before = _sink->count_of(LL_INFO);
	TickSpec spec;
	for (uint32_t i = 0; i < 2; i++)
	{
		spec.total_volume = 1000.0 + i;
		spec.action_time = 93000000 + i * 100000;
		write_tick(w, spec, 0);
	}
	// 前两条会打「建库/首笔」日志，但绝不能打「收到 N 笔」的分组汇总（:409）
	EXPECT_NE(_sink->count_of(LL_INFO), info_before);
	EXPECT_FALSE(_sink->has_log("ticks received from exchange", LL_INFO))
		<< "未满一组（2/3）就打出了分组汇总日志";

	spec.total_volume = 1002.0;
	spec.action_time = 93200000;
	write_tick(w, spec, 0);
	EXPECT_TRUE(_sink->has_log("ticks received from exchange SHFE", LL_INFO));
	EXPECT_TRUE(_sink->has_log("3 ticks received", LL_INFO));
}

} // namespace r23
