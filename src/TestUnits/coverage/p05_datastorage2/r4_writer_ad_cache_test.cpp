// r4 / WtDataStorageAD：WtDataWriterAD 的缓存块构造与 tick 缓存状态机
//
// 覆盖对象（src/WtDataStorageAD/WtDataWriterAD.cpp）：
//  - init（L73-104）：路径标准化/建目录、四个缓存文件名、groupsize、四个 disable 开关
//  - loadCache（L116-281）：新建块头；已有块头/版本/size/capacity/文件长度校验与索引重建
//  - resizeRTBlock（L283-325）：容量不足时扩文件 + 重新映射 + _capacity 回写
//  - writeTick/pushTask（L327-414）：NULL tick、空合约、会话拒收、同步分支
//  - updateTickCache（L862-1014）：交易日回退/未来日期/量递减三类拒收、
//    同毫秒 +200ms 修正、procFlag 0/1/2 三套字段补齐、跨交易日重建缓存项
//  - updateBarCache（L549-840）：非交易时段早退、小节末分钟回退、d1/m1/m5 追加与滚动
//  - getCurTick（L842-860）：空代码 / 未知合约 / 缓存未命中 / 命中
// 块内容一律通过回读自己临时目录下的 .dmb 文件来断言（不使用 private public）。
#include "r4_ad_fixture.h"

#include <limits>

#ifdef WT_DATAWRITERAD_TEST_SEAMS
extern "C" void wt_data_writer_ad_fail_next_remap_for_test();
extern "C" void wt_data_writer_ad_set_cache_pre_map_hook_for_test(void (*hook)(const char*));
#endif

namespace {

static_assert(sizeof(R4BlkHead) == sizeof(RTBlockHeader), "块头回读布局必须与生产一致");

#ifdef WT_DATAWRITERAD_TEST_SEAMS
bool g_cachePreMapTruncateSucceeded = false;

void truncateCacheAfterPreflightForTest(const char* filename)
{
	std::error_code ec;
	std::filesystem::resize_file(filename, sizeof(RTBlockHeader) - 1, ec);
	g_cachePreMapTruncateSucceeded = !ec;
}
#endif

// d1 缓存项的 time 字段被写成交易日本身（WtDataWriterAD.cpp:601/620）
static const uint32_t R4_TD1 = 20230103;
static const uint32_t R4_TD2 = 20230104;
// 09:30 的 tick → minutes=30 → barMins=31 → minuteToTime(31)：540+31=571 分 = 09:31
// TimeUtils::timeToMinBar(20230103, 931) = (20230103-19900000)*10000 + 931
static const uint32_t R4_M1_931 = 3301030931u;
// 09:32 的分钟 bar（barMins=32 → 572 分 = 09:32）
static const uint32_t R4_M1_932 = 3301030932u;
// m5：barMins=(30/5)*5+5=35 → 575 分 = 09:35
static const uint32_t R4_M5_935 = 3301030935u;
// 15:00 的 tick：minutes=360 回退成 359 → barMins=360 → minuteToTime(360)=1500
static const uint32_t R4_M1_CLOSE = 3301031500u;

static WTSVariant* make_writer_cfg_without_groupsize(const std::string& root)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", root.c_str());
	cfg->append("disabletick", true);
	cfg->append("disablemin1", true);
	cfg->append("disablemin5", true);
	cfg->append("disableday", true);
	return cfg;
}

//////////////////////////////////////////////////////////////////////////
// 1. 全新目录：四个块文件按 AD 块头规范落盘
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, FreshInitBuildsFourBlockFilesWithValidADHeader)
{
	const std::size_t tick_sz = sizeof(RTTickCache) + sizeof(TickCacheItem) * R4_INIT_CAP;
	const std::size_t bar_sz = sizeof(RTBarCache) + sizeof(BarCacheItem) * R4_INIT_CAP;

	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, false, false, false, false);
	init_writer(cfg);

	const char* names[] = {"cache_tick.dmb", "cache_m1.dmb", "cache_m5.dmb", "cache_d1.dmb"};
	for (const char* name : names)
	{
		const std::string p = path_of(name);
		ASSERT_TRUE(std::filesystem::exists(p)) << name;
		const uint64_t expect_sz = (std::strcmp(name, "cache_tick.dmb") == 0) ? tick_sz : bar_sz;
		EXPECT_EQ(file_size_or_zero(p), expect_sz) << name;

		R4BlkHead head = read_head(p);
		// L146/187/228/269：strcpy(_blk_flag, BLK_FLAG) 拷 7 字符 + 结尾 0，共 FLAG_SIZE=8 字节
		EXPECT_EQ(std::memcmp(head._blk_flag, BLK_FLAG, FLAG_SIZE), 0) << name;
		EXPECT_EQ(head._type, (uint16_t)BT_RT_Cache) << name;
		EXPECT_EQ(head._version, (uint16_t)1) << name;
		EXPECT_EQ(head._size, 0u) << name;
		EXPECT_EQ(head._capacity, R4_INIT_CAP) << name;
	}

	// 开关全部为 false 时不应提前建任何 LMDB 目录（只有 pipe* 会建）
	EXPECT_FALSE(std::filesystem::exists(_root / "ticks"));
	EXPECT_FALSE(std::filesystem::exists(_root / "min1"));
	EXPECT_FALSE(std::filesystem::exists(_root / "day"));
}

//////////////////////////////////////////////////////////////////////////
// 2. 四个 disable 开关：tick 缓存仍更新，但不建任何 LMDB、不动 K 线缓存
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, DisableFlagsKeepTickCacheButTouchNothingElse)
{
	init_writer_all_disabled();

	WTSCommodityInfo* comm = _bd->add_commodity("EXD2", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXD2", "rb2301", comm);

	WTSTickStruct s = make_tick("EXD2", "rb2301", R4_TD1, R4_TD1, 93000000, 3900.0, 10, 100, 5, 1);
	write_or_fail(tick_of(ct, s), 0);

	// tick 缓存路径不受 disabletick 之前的 updateTickCache 影响
	WTSTickData* cur = _writer->getCurTick("rb2301", "EXD2");
	ASSERT_TRUE(cur != NULL);
	EXPECT_DOUBLE_EQ(cur->price(), 3900.0);
	cur->release();

	EXPECT_EQ(_sink->bcast_tick(), 1u);
	// canSessionReceive 收到的是品种挂的时段 id（WtDataWriterAD.cpp:344）
	EXPECT_EQ(_sink->sid_seen(), std::string("R4_DAY"));

	// K 线缓存三段全部保持空（_size 为 0）
	const char* bars[] = {"cache_m1.dmb", "cache_m5.dmb", "cache_d1.dmb"};
	for (const char* name : bars)
		EXPECT_EQ(read_head(path_of(name))._size, 0u) << name;

	EXPECT_FALSE(std::filesystem::exists(_root / "ticks"));
	EXPECT_FALSE(std::filesystem::exists(_root / "min1"));
	EXPECT_FALSE(std::filesystem::exists(_root / "min5"));
	EXPECT_FALSE(std::filesystem::exists(_root / "day"));
}

//////////////////////////////////////////////////////////////////////////
// 3. 已有合法缓存块：按 item 的 exchg.code 重建索引
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, ReloadExistingCacheRebuildsIndex)
{
	std::vector<TickCacheItem> items;
	items.push_back(make_tick_item("EXR3", "rb2301", R4_TD1, 3811.0));
	items.push_back(make_tick_item("EXR3", "rb2302", R4_TD1, 3812.0));
	write_block_file(path_of("cache_tick.dmb"), BLK_FLAG, BT_RT_Cache, 1, 2, 2, tick_payload(items));
	const uint64_t tickFileSize = file_size_or_zero(path_of("cache_tick.dmb"));
	// A failed remap may leave a longer physical file with the old valid capacity.
	std::filesystem::resize_file(path_of("cache_tick.dmb"), tickFileSize + 17);

	// K 线三段同样给已有文件，走 L191/232/273 的索引重建分支
	std::vector<BarCacheItem> bar_items;
	bar_items.push_back(make_bar_item("EXR3", "rb2301", R4_TD1, R4_M1_931, 3811.0));
	write_block_file(path_of("cache_m1.dmb"), BLK_FLAG, BT_RT_Cache, 1, 1, 1, bar_payload(bar_items));

	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, true, true, true);
	init_writer(cfg);

	R4BlkHead head = read_head(path_of("cache_tick.dmb"));
	EXPECT_EQ(head._size, 2u);
	EXPECT_EQ(head._capacity, 2u);
	EXPECT_EQ(file_size_or_zero(path_of("cache_tick.dmb")), tickFileSize + 17);
	EXPECT_EQ(read_head(path_of("cache_m1.dmb"))._size, 1u);

	// 索引来自文件里的 item，而不是新写入：两条都能取到，且价格与落盘一致
	WTSCommodityInfo* comm = _bd->add_commodity("EXR3", "rb", ContractCategory::CC_Future);
	_bd->add_contract("EXR3", "rb2301", comm);
	_bd->add_contract("EXR3", "rb2302", comm);

	WTSTickData* c1 = _writer->getCurTick("rb2301", "EXR3");
	ASSERT_TRUE(c1 != NULL);
	EXPECT_DOUBLE_EQ(c1->price(), 3811.0);
	c1->release();

	WTSTickData* c2 = _writer->getCurTick("rb2302", "EXR3");
	ASSERT_TRUE(c2 != NULL);
	EXPECT_DOUBLE_EQ(c2->price(), 3812.0);
	EXPECT_EQ(c2->tradingdate(), R4_TD1);
	c2->release();

	// 重建时不会被后来的新合约挤掉：未入缓存的代码仍然 NULL
	WTSTickData* c3 = _writer->getCurTick("rb2399", "EXR3");
	EXPECT_TRUE(c3 == NULL);
}

//////////////////////////////////////////////////////////////////////////
// 3a. 损坏/越界块拒绝初始化，并且在所有文件预检前不改写任何缓存文件
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, RejectsMalformedCacheFilesWithoutModifyingAnyCache)
{
	enum class Corruption
	{
		ShortHeader,
		BadFlag,
		BadType,
		BadVersion,
		SizeExceedsCapacity,
		PayloadShorterThanCapacity,
		ExtremeCapacity
	};
	struct CacheCase
	{
		const char* _name;
		const char* _target;
		Corruption _corruption;
	};
	const CacheCase cases[] = {
		{"short_header", "cache_tick.dmb", Corruption::ShortHeader},
		{"bad_flag", "cache_m1.dmb", Corruption::BadFlag},
		{"bad_type", "cache_m5.dmb", Corruption::BadType},
		{"bad_version", "cache_d1.dmb", Corruption::BadVersion},
		{"size_over_capacity", "cache_tick.dmb", Corruption::SizeExceedsCapacity},
		{"payload_short", "cache_m1.dmb", Corruption::PayloadShorterThanCapacity},
		{"extreme_capacity", "cache_m5.dmb", Corruption::ExtremeCapacity}
	};
	const char* cacheNames[] = {"cache_tick.dmb", "cache_m1.dmb", "cache_m5.dmb", "cache_d1.dmb"};

	for (const CacheCase& cacheCase : cases)
	{
		const std::filesystem::path caseRoot = _root / cacheCase._name;
		std::filesystem::create_directories(caseRoot);
		std::vector<TickCacheItem> tickItems;
		tickItems.push_back(make_tick_item("EXR4", "rb2301", R4_TD1, 3901.0));
		std::vector<BarCacheItem> barItems;
		barItems.push_back(make_bar_item("EXR4", "rb2301", R4_TD1, R4_M1_931, 3901.0));
		write_block_file((caseRoot / cacheNames[0]).string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1,
			tick_payload(tickItems));
		for (size_t i = 1; i < 4; ++i)
			write_block_file((caseRoot / cacheNames[i]).string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1,
				bar_payload(barItems));

		const std::filesystem::path target = caseRoot / cacheCase._target;
		if (cacheCase._corruption == Corruption::ShortHeader)
		{
			std::ofstream truncated(target, std::ios::binary | std::ios::trunc);
			truncated.write("short", 5);
			ASSERT_TRUE(truncated.good()) << target.string();
		}
		else
		{
			std::fstream file(target, std::ios::binary | std::ios::in | std::ios::out);
			ASSERT_TRUE(file.good()) << target.string();
			R4BlkHead header = {};
			file.read((char*)&header, sizeof(header));
			ASSERT_EQ(file.gcount(), static_cast<std::streamsize>(sizeof(header))) << target.string();
			switch (cacheCase._corruption)
			{
			case Corruption::BadFlag:
				header._blk_flag[0] = 'X';
				break;
			case Corruption::BadType:
				header._type = 0;
				break;
			case Corruption::BadVersion:
				header._version = 2;
				break;
			case Corruption::SizeExceedsCapacity:
				header._size = 2;
				break;
			case Corruption::PayloadShorterThanCapacity:
				header._capacity = 2;
				break;
			case Corruption::ExtremeCapacity:
				header._capacity = (std::numeric_limits<uint32_t>::max)();
				header._size = 0;
				break;
			case Corruption::ShortHeader:
				break;
			}
			file.seekp(0, std::ios::beg);
			file.write((const char*)&header, sizeof(header));
			file.flush();
			ASSERT_TRUE(file.good()) << target.string();
		}

		auto readBytes = [](const std::filesystem::path& path) {
			std::ifstream input(path, std::ios::binary);
			if (!input)
				return std::string();
			input.seekg(0, std::ios::end);
			const std::streamoff length = input.tellg();
			if (length < 0)
				return std::string();
			std::string bytes(static_cast<size_t>(length), '\0');
			input.seekg(0, std::ios::beg);
			if (!bytes.empty())
				input.read(&bytes[0], static_cast<std::streamsize>(bytes.size()));
			return input || bytes.empty() ? bytes : std::string();
		};
		std::vector<std::string> before;
		for (const char* cacheName : cacheNames)
		{
			before.push_back(readBytes(caseRoot / cacheName));
			ASSERT_FALSE(before.back().empty()) << cacheName;
		}

		const uint32_t invalidLogCount = _sink->count_containing("Invalid");
		WtDataWriterAD candidate;
		WTSVariant* cfg = make_writer_cfg(caseRoot.string(), 1000, true, true, true, true);
		const bool initialized = candidate.init(cfg, _sink);
		cfg->release();
		EXPECT_FALSE(initialized) << cacheCase._name;
		EXPECT_GT(_sink->count_containing("Invalid"), invalidLogCount) << cacheCase._name;

		for (size_t i = 0; i < 4; ++i)
			EXPECT_EQ(readBytes(caseRoot / cacheNames[i]), before[i])
				<< cacheNames[i] << " / " << cacheCase._name;
	}
}

//////////////////////////////////////////////////////////////////////////
// 宏专用竞态注入：预检通过后、map 前缩短 tick 文件，验证先检查映射长度再读头。
//////////////////////////////////////////////////////////////////////////
#ifdef WT_DATAWRITERAD_TEST_SEAMS
TEST_F(R4WriterADTest, RejectsCacheShortenedBetweenPreflightAndMap)
{
	std::vector<TickCacheItem> tickItems;
	tickItems.push_back(make_tick_item("EXR5", "rb2301", R4_TD1, 3901.0));
	write_block_file(path_of("cache_tick.dmb"), BLK_FLAG, BT_RT_Cache, 1, 1, 1, tick_payload(tickItems));
	std::vector<BarCacheItem> barItems;
	barItems.push_back(make_bar_item("EXR5", "rb2301", R4_TD1, R4_M1_931, 3901.0));
	for (const char* cacheName : {"cache_m1.dmb", "cache_m5.dmb", "cache_d1.dmb"})
		write_block_file(path_of(cacheName), BLK_FLAG, BT_RT_Cache, 1, 1, 1, bar_payload(barItems));

	g_cachePreMapTruncateSucceeded = false;
	const uint32_t shortMapLogCount = _sink->count_containing("shorter than its RT header");
	wt_data_writer_ad_set_cache_pre_map_hook_for_test(truncateCacheAfterPreflightForTest);
	WtDataWriterAD candidate;
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, true, true, true);
	const bool initialized = candidate.init(cfg, _sink);
	cfg->release();
	wt_data_writer_ad_set_cache_pre_map_hook_for_test(nullptr);

	EXPECT_FALSE(initialized);
	EXPECT_TRUE(g_cachePreMapTruncateSucceeded);
	EXPECT_EQ(file_size_or_zero(path_of("cache_tick.dmb")), sizeof(RTBlockHeader) - 1);
	EXPECT_GT(_sink->count_containing("shorter than its RT header"), shortMapLogCount);
}
#endif

//////////////////////////////////////////////////////////////////////////
// 4. 容量已满：扩文件 + 重映射 + capacity 回写，且原有条目仍在
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, TickCacheAtCapacityResizesFileAndKeepsOldItems)
{
	std::vector<TickCacheItem> items;
	items.push_back(make_tick_item("EXS4", "rb2301", R4_TD1, 3901.0));
	items.push_back(make_tick_item("EXS4", "rb2302", R4_TD1, 3902.0));
	write_block_file(path_of("cache_tick.dmb"), BLK_FLAG, BT_RT_Cache, 1, 2, 2, tick_payload(items));

	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, true, true, true);
	init_writer(cfg);

	const uint64_t old_sz = file_size_or_zero(path_of("cache_tick.dmb"));
	EXPECT_EQ(old_sz, sizeof(RTTickCache) + sizeof(TickCacheItem) * 2);

	WTSCommodityInfo* comm = _bd->add_commodity("EXS4", "rb", ContractCategory::CC_Future);
	// getCurTick（WtDataWriterAD.cpp:847-849）先查基础数据：rb2301/rb2302 的缓存项是
	// 手写的块文件，但合约登记也要补齐，否则 R32 之前的夹具漏前置 → 回查恒 NULL
	_bd->add_contract("EXS4", "rb2301", comm);
	_bd->add_contract("EXS4", "rb2302", comm);
	WTSContractInfo* ct3 = _bd->add_contract("EXS4", "rb2303", comm);

	// 第 3 条：_size 由 2 变 3 >= capacity 2 → 扩容到 capacity + CACHE_SIZE_STEP_AD = 402
	WTSTickStruct s = make_tick("EXS4", "rb2303", R4_TD1, R4_TD1, 93000000, 3903.0, 1, 10, 0, 0);
	write_or_fail(tick_of(ct3, s), 0);

	R4BlkHead head = read_head(path_of("cache_tick.dmb"));
	// 目标容量 = 原 capacity(2) + CACHE_SIZE_STEP_AD(400) = 402
	EXPECT_EQ(head._capacity, 2u + R4_STEP);
	EXPECT_EQ(head._size, 3u);
	EXPECT_EQ(file_size_or_zero(path_of("cache_tick.dmb")),
		sizeof(RTTickCache) + sizeof(TickCacheItem) * (2u + R4_STEP));

	// 扩容后索引仍然有效（旧下标指向同一条目）
	TickCacheItem it0;
	ASSERT_TRUE(read_tick_item(path_of("cache_tick.dmb"), 0, it0));
	EXPECT_STREQ(it0._tick.code, "rb2301");
	EXPECT_DOUBLE_EQ(it0._tick.price, 3901.0);

	WTSTickData* c1 = _writer->getCurTick("rb2301", "EXS4");
	ASSERT_TRUE(c1 != NULL);
	EXPECT_DOUBLE_EQ(c1->price(), 3901.0);
	c1->release();

	WTSTickData* c3 = _writer->getCurTick("rb2303", "EXS4");
	ASSERT_TRUE(c3 != NULL);
	EXPECT_DOUBLE_EQ(c3->price(), 3903.0);
	c3->release();

	// L881 的扩容日志。R32 修正：第 3 笔是 EXS4.rb2303 的首笔（item._date 从 0 前进到
	// 20230103），updateTickCache L946-948 随后还会打一条 "First tick of new tradingday"
	// 的 INFO，把 last_log(LL_INFO) 覆盖掉 ⇒ 一次性语句改用 count_containing 断言。
	EXPECT_EQ(_sink->count_containing("Tick Cache resized to 402 items"), 1u)
		<< _sink->last_log(LL_INFO);
}

//////////////////////////////////////////////////////////////////////////
// 默认构建保留 rename 后 open_existing_file 失败的回归及文件恢复路径。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, BarCacheExpansionFailureKeepsStateAndRetriesAfterFileRestore)
{
	struct CacheCase
	{
		const char* _name;
		const char* _filename;
		const char* _exchg;
		const char* _failure_log;
	};
	const CacheCase cases[] = {
		{"day", "cache_d1.dmb", "EXR4D", "day bar cache expansion failed"},
		{"m1", "cache_m1.dmb", "EXR4M1", "m1 bar cache expansion failed"},
		{"m5", "cache_m5.dmb", "EXR4M5", "m5 bar cache expansion failed"}
	};
	const char* barFiles[] = {"cache_d1.dmb", "cache_m1.dmb", "cache_m5.dmb"};

	for (const CacheCase& cacheCase : cases)
	{
		if (_writer)
		{
			_writer->release();
			_writer.reset();
		}

		const std::filesystem::path caseRoot = _root / cacheCase._name;
		std::filesystem::create_directories(caseRoot);
		const std::filesystem::path cachePath = caseRoot / cacheCase._filename;
		const std::filesystem::path movedPath = cachePath.string() + ".offline";

		std::vector<BarCacheItem> oldItems;
		oldItems.push_back(make_bar_item(cacheCase._exchg, "rb_old", R4_TD1, R4_M1_931, 1111.0));
		write_block_file(cachePath.string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1, bar_payload(oldItems));

		WTSVariant* cfg = make_writer_cfg(caseRoot.string(), 1000, true, false, false, false);
		init_writer(cfg);

		WTSCommodityInfo* comm = _bd->add_commodity(cacheCase._exchg, "rb", ContractCategory::CC_Future);
		WTSContractInfo* oldCt = _bd->add_contract(cacheCase._exchg, "rb_old", comm);
		WTSContractInfo* ct = _bd->add_contract(cacheCase._exchg, "rb_new", comm);
		ASSERT_NE(nullptr, oldCt);
		ASSERT_NE(nullptr, ct);

		std::error_code ec;
		std::filesystem::rename(cachePath, movedPath, ec);
		ASSERT_FALSE(ec) << ec.message();

		const uint32_t broadcastsBefore = _sink->bcast_tick();
		WTSTickStruct first = make_tick(cacheCase._exchg, "rb_new", R4_TD1, R4_TD1,
			93000000, 3901.0, 1, 10, 50, 0, 1, 10);
		write_or_fail(tick_of(ct, first), 0);
		EXPECT_EQ(_sink->bcast_tick(), broadcastsBefore + 1);
		EXPECT_GE(_sink->count_containing(cacheCase._failure_log), 1u);

		// The missing original path fails before truncate; preserve the old mapped state.
		R4BlkHead failedHead = read_head(movedPath.string());
		EXPECT_EQ(failedHead._size, 1u);
		EXPECT_EQ(failedHead._capacity, 1u);
		BarCacheItem preserved;
		ASSERT_TRUE(read_bar_item(movedPath.string(), cacheCase._exchg, "rb_old", preserved));
		EXPECT_DOUBLE_EQ(preserved._bar.close, 1111.0);
		BarCacheItem missing;
		EXPECT_FALSE(read_bar_item(movedPath.string(), cacheCase._exchg, "rb_new", missing));

		// Model the disk residue from the separate truncate-success/remap-failure case.
		const std::uint64_t expandedSize = sizeof(RTBarCache) +
			sizeof(BarCacheItem) * (1u + R4_STEP);
		std::filesystem::resize_file(movedPath, expandedSize, ec);
		ASSERT_FALSE(ec) << ec.message();
		EXPECT_EQ(file_size_or_zero(movedPath.string()), expandedSize);
		EXPECT_EQ(read_head(movedPath.string())._capacity, 1u);
		WTSTickStruct oldIdentityTick = make_tick(cacheCase._exchg, "rb_old", R4_TD1, R4_TD1,
			93030000, 3911.0, 1, 10, 51, 0, 2, 20);
		write_or_fail(tick_of(oldCt, oldIdentityTick), 0);
		EXPECT_EQ(_sink->bcast_tick(), broadcastsBefore + 2);
		ASSERT_TRUE(read_bar_item(movedPath.string(), cacheCase._exchg, "rb_old", preserved));
		EXPECT_DOUBLE_EQ(preserved._bar.close, 3911.0);

		// Failure in one bar stream does not suppress updates to the other two streams.
		for (const char* barFile : barFiles)
		{
			if (std::strcmp(barFile, cacheCase._filename) == 0)
				continue;
			BarCacheItem other;
			ASSERT_TRUE(read_bar_item((caseRoot / barFile).string(), cacheCase._exchg, "rb_new", other))
				<< barFile;
		}

		// Restore the same mapped file at its original path, then retry the same identity.
		std::filesystem::rename(movedPath, cachePath, ec);
		ASSERT_FALSE(ec) << ec.message();
		WTSTickStruct retry = make_tick(cacheCase._exchg, "rb_new", R4_TD1, R4_TD1,
			93030000, 3902.0, 1, 10, 51, 0, 2, 20);
		write_or_fail(tick_of(ct, retry), 0);
		EXPECT_EQ(_sink->bcast_tick(), broadcastsBefore + 3);

		R4BlkHead recoveredHead = read_head(cachePath.string());
		EXPECT_EQ(recoveredHead._size, 2u);
		EXPECT_EQ(recoveredHead._capacity, 1u + R4_STEP);
		EXPECT_EQ(file_size_or_zero(cachePath.string()), expandedSize);
		ASSERT_TRUE(read_bar_item(cachePath.string(), cacheCase._exchg, "rb_old", preserved));
		EXPECT_DOUBLE_EQ(preserved._bar.close, 3911.0);
		ASSERT_TRUE(read_bar_item(cachePath.string(), cacheCase._exchg, "rb_new", missing));
		EXPECT_DOUBLE_EQ(missing._bar.close, 3902.0);
	}
}

//////////////////////////////////////////////////////////////////////////
// 宏专用故障注入：实际 truncate 成功后让 remap 返回失败，再原位重试。
//////////////////////////////////////////////////////////////////////////
#ifdef WT_DATAWRITERAD_TEST_SEAMS
TEST_F(R4WriterADTest, BarCacheRemapFailureKeepsStateAndRetriesInPlace)
{
	struct CacheCase
	{
		const char* _name;
		const char* _filename;
		const char* _exchg;
		const char* _failure_log;
	};
	const CacheCase cases[] = {
		{"day", "cache_d1.dmb", "EXR4D", "day bar cache expansion failed"},
		{"m1", "cache_m1.dmb", "EXR4M1", "m1 bar cache expansion failed"},
		{"m5", "cache_m5.dmb", "EXR4M5", "m5 bar cache expansion failed"}
	};
	const char* barFiles[] = {"cache_d1.dmb", "cache_m1.dmb", "cache_m5.dmb"};

	for (const CacheCase& cacheCase : cases)
	{
		if (_writer)
		{
			_writer->release();
			_writer.reset();
		}

		const std::filesystem::path caseRoot = _root / cacheCase._name;
		std::filesystem::create_directories(caseRoot);
		const std::filesystem::path cachePath = caseRoot / cacheCase._filename;

		std::vector<BarCacheItem> oldItems;
		oldItems.push_back(make_bar_item(cacheCase._exchg, "rb_old", R4_TD1, R4_M1_931, 1111.0));
		write_block_file(cachePath.string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1, bar_payload(oldItems));

		WTSVariant* cfg = make_writer_cfg(caseRoot.string(), 1000, true, false, false, false);
		init_writer(cfg);

		WTSCommodityInfo* comm = _bd->add_commodity(cacheCase._exchg, "rb", ContractCategory::CC_Future);
		WTSContractInfo* oldCt = _bd->add_contract(cacheCase._exchg, "rb_old", comm);
		WTSContractInfo* ct = _bd->add_contract(cacheCase._exchg, "rb_new", comm);
		ASSERT_NE(nullptr, oldCt);
		ASSERT_NE(nullptr, ct);

		const uint32_t broadcastsBefore = _sink->bcast_tick();
		WTSTickStruct first = make_tick(cacheCase._exchg, "rb_new", R4_TD1, R4_TD1,
			93000000, 3901.0, 1, 10, 50, 0, 1, 10);
#ifdef WT_DATAWRITERAD_TEST_SEAMS
		wt_data_writer_ad_fail_next_remap_for_test();
#endif
		write_or_fail(tick_of(ct, first), 0);
		EXPECT_EQ(_sink->bcast_tick(), broadcastsBefore + 1);
		EXPECT_GE(_sink->count_containing(cacheCase._failure_log), 1u);

		// truncate succeeded before the injected remap failure: physical extent grew,
		// while the old mapped header, logical size, and index remain unchanged.
		const std::uint64_t expandedSize = sizeof(RTBarCache) +
			sizeof(BarCacheItem) * (1u + R4_STEP);
		EXPECT_EQ(file_size_or_zero(cachePath.string()), expandedSize);
		EXPECT_GE(_sink->count_containing("Failed to remap expanded RT cache file"), 1u);
		R4BlkHead failedHead = read_head(cachePath.string());
		EXPECT_EQ(failedHead._size, 1u);
		EXPECT_EQ(failedHead._capacity, 1u);
		BarCacheItem preserved;
		ASSERT_TRUE(read_bar_item(cachePath.string(), cacheCase._exchg, "rb_old", preserved));
		EXPECT_DOUBLE_EQ(preserved._bar.close, 1111.0);
		BarCacheItem missing;
		EXPECT_FALSE(read_bar_item(cachePath.string(), cacheCase._exchg, "rb_new", missing));
		WTSTickStruct oldIdentityTick = make_tick(cacheCase._exchg, "rb_old", R4_TD1, R4_TD1,
			93030000, 3911.0, 1, 10, 51, 0, 2, 20);
		write_or_fail(tick_of(oldCt, oldIdentityTick), 0);
		EXPECT_EQ(_sink->bcast_tick(), broadcastsBefore + 2);
		ASSERT_TRUE(read_bar_item(cachePath.string(), cacheCase._exchg, "rb_old", preserved));
		EXPECT_DOUBLE_EQ(preserved._bar.close, 3911.0);

		// Failure in one bar stream does not suppress updates to the other two streams.
		for (const char* barFile : barFiles)
		{
			if (std::strcmp(barFile, cacheCase._filename) == 0)
				continue;
			BarCacheItem other;
			ASSERT_TRUE(read_bar_item((caseRoot / barFile).string(), cacheCase._exchg, "rb_new", other))
				<< barFile;
		}

		// Retry the same identity at the unchanged path. The previously extended file
		// should now remap successfully and commit the new cache slot.
		WTSTickStruct retry = make_tick(cacheCase._exchg, "rb_new", R4_TD1, R4_TD1,
			93030000, 3902.0, 1, 10, 51, 0, 2, 20);
		write_or_fail(tick_of(ct, retry), 0);
		EXPECT_EQ(_sink->bcast_tick(), broadcastsBefore + 3);

		R4BlkHead recoveredHead = read_head(cachePath.string());
		EXPECT_EQ(recoveredHead._size, 2u);
		EXPECT_EQ(recoveredHead._capacity, 1u + R4_STEP);
		EXPECT_EQ(file_size_or_zero(cachePath.string()),
			expandedSize);
		ASSERT_TRUE(read_bar_item(cachePath.string(), cacheCase._exchg, "rb_old", preserved));
		EXPECT_DOUBLE_EQ(preserved._bar.close, 3911.0);
		ASSERT_TRUE(read_bar_item(cachePath.string(), cacheCase._exchg, "rb_new", missing));
		EXPECT_DOUBLE_EQ(missing._bar.close, 3902.0);
	}
}
#endif

//////////////////////////////////////////////////////////////////////////
// 5. procFlag=0：同交易日连续 tick 直接整体覆盖缓存，价格/量原样落缓存
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, ProcFlagZeroOverwritesCacheVerbatim)
{
	init_writer_all_disabled();
	WTSCommodityInfo* comm = _bd->add_commodity("EXP0", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXP0", "rb2301", comm);

	WTSTickStruct s1 = make_tick("EXP0", "rb2301", R4_TD1, R4_TD1, 93000000, 3900.0, 10, 100, 5, 1, 100, 1000);
	write_or_fail(tick_of(ct, s1), 0);
	WTSTickStruct s2 = make_tick("EXP0", "rb2301", R4_TD1, R4_TD1, 93100000, 3910.0, 20, 200, 6, 1, 120, 1200);
	write_or_fail(tick_of(ct, s2), 0);

	WTSTickData* cur = _writer->getCurTick("rb2301", "EXP0");
	ASSERT_TRUE(cur != NULL);
	EXPECT_DOUBLE_EQ(cur->price(), 3910.0);
	EXPECT_DOUBLE_EQ(cur->volume(), 20.0);
	EXPECT_DOUBLE_EQ(cur->totalvolume(), 120.0);
	// procFlag==0 → L980 只做 memcpy，不做增量换算：diff_interest 保持原始值
	EXPECT_DOUBLE_EQ(cur->additional(), 1.0);
	cur->release();
}

//////////////////////////////////////////////////////////////////////////
// 6. procFlag=1：同日内 volume/turn_over/diff_interest 改写为与上一笔的差额
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, ProcFlagOneTurnsTotalsIntoDeltas)
{
	init_writer_all_disabled();
	WTSCommodityInfo* comm = _bd->add_commodity("EXP1", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXP1", "rb2301", comm);

	WTSTickStruct s1 = make_tick("EXP1", "rb2301", R4_TD1, R4_TD1, 93000000, 3900.0, 10, 100, 50, 0, 100, 1000);
	write_or_fail(tick_of(ct, s1), 1);
	WTSTickStruct s2 = make_tick("EXP1", "rb2301", R4_TD1, R4_TD1, 93100000, 3910.0, 0, 0, 60, 0, 130, 1350);
	write_or_fail(tick_of(ct, s2), 1);

	WTSTickData* cur = _writer->getCurTick("rb2301", "EXP1");
	ASSERT_TRUE(cur != NULL);
	// L984-986：130-100 / 1350-1000 / 60-50
	EXPECT_DOUBLE_EQ(cur->volume(), 30.0);
	EXPECT_DOUBLE_EQ(cur->turnover(), 350.0);
	EXPECT_DOUBLE_EQ(cur->additional(), 10.0);
	// 总量字段保持原值
	EXPECT_DOUBLE_EQ(cur->totalvolume(), 130.0);
	cur->release();
}

//////////////////////////////////////////////////////////////////////////
// 7. procFlag=1 跨交易日：L907-918 用自身总量回填，并把 pre_interest 差分写入缓存
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, ProcFlagOneOnNewTradingDateRebaselinesCache)
{
	init_writer_all_disabled();
	WTSCommodityInfo* comm = _bd->add_commodity("EXN1", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXN1", "rb2301", comm);

	WTSTickStruct s1 = make_tick("EXN1", "rb2301", R4_TD1, R4_TD1, 93000000, 3900.0, 10, 100, 50, 0, 100, 1000);
	write_or_fail(tick_of(ct, s1), 1);

	// 新交易日完整快照：持仓 60、昨持 50 → diff_interest=10；同时 item._date 前进
	WTSTickStruct s2 = make_tick("EXN1", "rb2301", R4_TD2, R4_TD2, 93000000, 4000.0, 5, 50, 60, 0, 5, 50);
	s2.pre_interest = 50;
	write_or_fail(tick_of(ct, s2), 1);

	WTSTickData* cur = _writer->getCurTick("rb2301", "EXN1");
	ASSERT_TRUE(cur != NULL);
	EXPECT_EQ(cur->tradingdate(), R4_TD2);
	EXPECT_DOUBLE_EQ(cur->additional(), 10.0);
	EXPECT_DOUBLE_EQ(cur->volume(), 5.0);
	cur->release();

	TickCacheItem item;
	ASSERT_TRUE(read_tick_item(path_of("cache_tick.dmb"), 0, item));
	EXPECT_EQ(item._date, R4_TD2);
	EXPECT_EQ(item._tick.pre_interest, s2.pre_interest);

	// L946 的新交易日日志
	EXPECT_NE(_sink->last_log(LL_INFO).find("First tick of new tradingday"), std::string::npos)
		<< _sink->last_log(LL_INFO);
}

//////////////////////////////////////////////////////////////////////////
// 7a. procFlag=1 完整快照边界：pre_interest=0 仍表示昨持为 0，additional 应为 60。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, ProcFlagOneZeroPreInterestKeepsFullSnapshot)
{
	init_writer_all_disabled();
	WTSCommodityInfo* comm = _bd->add_commodity("EXN0", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXN0", "rb2301", comm);

	WTSTickStruct s1 = make_tick("EXN0", "rb2301", R4_TD1, R4_TD1, 93000000, 3900.0, 10, 100, 50, 0, 100, 1000);
	write_or_fail(tick_of(ct, s1), 1);

	WTSTickStruct s2 = make_tick("EXN0", "rb2301", R4_TD2, R4_TD2, 93000000, 4000.0, 5, 50, 60, 0, 5, 50);
	s2.open_interest = 60;
	s2.pre_interest = 0;
	write_or_fail(tick_of(ct, s2), 1);

	WTSTickData* cur = _writer->getCurTick("rb2301", "EXN0");
	ASSERT_TRUE(cur != NULL);
	EXPECT_EQ(cur->tradingdate(), R4_TD2);
	EXPECT_DOUBLE_EQ(cur->additional(), 60.0);
	cur->release();
}

//////////////////////////////////////////////////////////////////////////
// 8. procFlag=2：总量/总成交额为 0 时自动累加；最低价应保持 low 的最小值语义。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, ProcFlagTwoAccumulatesTotals)
{
	init_writer_all_disabled();
	WTSCommodityInfo* comm = _bd->add_commodity("EXP2", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXP2", "rb2301", comm);

	WTSTickStruct s1 = make_tick("EXP2", "rb2301", R4_TD1, R4_TD1, 93000000, 3900.0, 10, 100, 50, 0, 100, 1000);
	write_or_fail(tick_of(ct, s1), 2);

	WTSTickStruct s2 = make_tick("EXP2", "rb2301", R4_TD1, R4_TD1, 93100000, 3800.0, 20, 200, 55, 0, 0, 0);
	// 夹具 make_tick（r4_ad_fixture.h:244-245）会把 ≤0 的总量回填成本笔量，
	// 而本用例要打的正是 WtDataWriterAD.cpp:994-998「总量为 0 才累加」的分支，
	// 所以必须显式把两个总量字段压回 0（此前缺这一步 ⇒ 累加分支根本没被走到）。
	s2.total_volume = 0;
	s2.total_turnover = 0;
	s2.open = 0;
	s2.high = 0;
	s2.low = 0;
	write_or_fail(tick_of(ct, s2), 2);

	WTSTickData* cur = _writer->getCurTick("rb2301", "EXP2");
	ASSERT_TRUE(cur != NULL);
	// L994-998：总量累加
	EXPECT_DOUBLE_EQ(cur->totalvolume(), 120.0);
	EXPECT_DOUBLE_EQ(cur->totalturnover(), 1200.0);
	// L1000-1001：open 用最新价补；L1003-1004：high 用 max(本笔价, 缓存 high) 补，
	// 期望值依据该公式从 3800 改为 max(3800, 3900)=3900（上一笔 3900 的 high 已在缓存里）
	EXPECT_DOUBLE_EQ(cur->open(), 3800.0);
	EXPECT_DOUBLE_EQ(cur->high(), 3900.0);
	// 第二笔 price=3800、low=0 应补为 min(3800,3900)=3800。
	EXPECT_DOUBLE_EQ(cur->low(), 3800.0);
	cur->release();

	// 后续价格高于已知最低价且 low 仍缺失时，不得把累计 low 抬高。
	WTSTickStruct s3 = make_tick("EXP2", "rb2301", R4_TD1, R4_TD1, 93200000, 4000.0, 1, 10, 56, 0, 121, 1210);
	s3.low = 0;
	write_or_fail(tick_of(ct, s3), 2);
	cur = _writer->getCurTick("rb2301", "EXP2");
	ASSERT_TRUE(cur != NULL);
	EXPECT_DOUBLE_EQ(cur->low(), 3800.0);
	cur->release();
}

//////////////////////////////////////////////////////////////////////////
// 9. 同一动作时间且总量不减（郑商所场景）：缓存时间被 +200ms（L972-975）
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, IdenticalActionTimeIsBumpedBy200Ms)
{
	init_writer_all_disabled();
	WTSCommodityInfo* comm = _bd->add_commodity("EXZ9", "MA", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXZ9", "MA301", comm);

	WTSTickStruct s1 = make_tick("EXZ9", "MA301", R4_TD1, R4_TD1, 93000000, 2500.0, 1, 10, 0, 0, 100, 1000);
	write_or_fail(tick_of(ct, s1), 0);
	// 时间戳完全相同，但总量更大 → 走 +200 分支（同时避开"总量递减"拒收）
	WTSTickStruct s2 = make_tick("EXZ9", "MA301", R4_TD1, R4_TD1, 93000000, 2501.0, 2, 20, 0, 0, 101, 1020);
	write_or_fail(tick_of(ct, s2), 0);

	WTSTickData* cur = _writer->getCurTick("MA301", "EXZ9");
	ASSERT_TRUE(cur != NULL);
	EXPECT_EQ(cur->actiontime(), 93000200u);
	EXPECT_DOUBLE_EQ(cur->price(), 2501.0);
	cur->release();

	// 第三笔仍然同毫秒：WtDataWriterAD.cpp:972-975 的 +200 是加在「本笔 action_time」
	// （仍是 93000000）上，不是在缓存值上累加 ⇒ 期望从 93000400 改为 93000200
	//（依据 L974 `newTick.action_time += 200;`，newTick 每次都以原始输入起步）。
	WTSTickStruct s3 = make_tick("EXZ9", "MA301", R4_TD1, R4_TD1, 93000000, 2502.0, 3, 30, 0, 0, 102, 1050);
	write_or_fail(tick_of(ct, s3), 0);
	WTSTickData* cur2 = _writer->getCurTick("MA301", "EXZ9");
	ASSERT_TRUE(cur2 != NULL);
	EXPECT_EQ(cur2->actiontime(), 93000200u);
	cur2->release();
}

//////////////////////////////////////////////////////////////////////////
// 10. 交易日回退：L891-895 直接拒绝，不进缓存不广播
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, OlderTradingDateThanCacheIsRejected)
{
	init_writer_all_disabled();
	WTSCommodityInfo* comm = _bd->add_commodity("EXB1", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXB1", "rb2301", comm);

	WTSTickStruct s1 = make_tick("EXB1", "rb2301", R4_TD2, R4_TD2, 93000000, 4000.0, 10, 100, 5, 0);
	write_or_fail(tick_of(ct, s1), 0);
	ASSERT_EQ(_sink->bcast_tick(), 1u);

	WTSTickStruct s2 = make_tick("EXB1", "rb2301", R4_TD1, R4_TD1, 93000000, 3900.0, 10, 100, 5, 0);
	write_or_fail(tick_of(ct, s2), 0);

	// 回退笔被丢掉：缓存仍是 20230104/4000，且没有第二次广播
	EXPECT_EQ(_sink->bcast_tick(), 1u);
	WTSTickData* cur = _writer->getCurTick("rb2301", "EXB1");
	ASSERT_TRUE(cur != NULL);
	EXPECT_EQ(cur->tradingdate(), R4_TD2);
	EXPECT_DOUBLE_EQ(cur->price(), 4000.0);
	cur->release();
	EXPECT_NE(_sink->last_log(LL_INFO).find("is less than cached tradingday"), std::string::npos);

	// K 线缓存完全没被动过（L348 的 break 生效）
	const char* bars[] = {"cache_m1.dmb", "cache_m5.dmb", "cache_d1.dmb"};
	for (const char* name : bars)
		EXPECT_EQ(read_head(path_of(name))._size, 0u) << name;
}

//////////////////////////////////////////////////////////////////////////
// 11. 偏移日期跑到交易日前面（夜盘错配）：L954-960 判为异常丢弃
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, OffsetDateAheadOfTradingDateIsAbandoned)
{
	init_writer_all_disabled();
	WTSCommodityInfo* comm = _bd->add_commodity("EXOF", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXOF", "rb2301", comm);

	WTSTickStruct s1 = make_tick("EXOF", "rb2301", R4_TD1, R4_TD1, 93000000, 3900.0, 10, 100, 5, 0, 100, 1000);
	write_or_fail(tick_of(ct, s1), 0);

	// 同一交易日，但动作日期已经前进到 20230104（getOffsetDate 返回 20230104 > 20230103）
	WTSTickStruct s2 = make_tick("EXOF", "rb2301", R4_TD1, R4_TD2, 93100000, 3910.0, 20, 200, 6, 0, 120, 1200);
	write_or_fail(tick_of(ct, s2), 0);

	EXPECT_EQ(_sink->bcast_tick(), 1u);
	WTSTickData* cur = _writer->getCurTick("rb2301", "EXOF");
	ASSERT_TRUE(cur != NULL);
	EXPECT_DOUBLE_EQ(cur->price(), 3900.0);
	cur->release();
	EXPECT_NE(_sink->last_log(LL_ERROR).find("has an exception, abandoned"), std::string::npos)
		<< _sink->last_log(LL_ERROR);
}

//////////////////////////////////////////////////////////////////////////
// 12. 同交易日总量递减（且非 procFlag=2）：L961-966 丢弃并告警
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, DecreasingTotalVolumeIsAbandoned)
{
	init_writer_all_disabled();
	WTSCommodityInfo* comm = _bd->add_commodity("EXDV", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXDV", "rb2301", comm);

	WTSTickStruct s1 = make_tick("EXDV", "rb2301", R4_TD1, R4_TD1, 93000000, 3900.0, 10, 100, 5, 0, 100, 1000);
	write_or_fail(tick_of(ct, s1), 0);

	WTSTickStruct s2 = make_tick("EXDV", "rb2301", R4_TD1, R4_TD1, 93100000, 3890.0, 1, 10, 5, 0, 90, 900);
	write_or_fail(tick_of(ct, s2), 0);

	EXPECT_EQ(_sink->bcast_tick(), 1u);
	WTSTickData* cur = _writer->getCurTick("rb2301", "EXDV");
	ASSERT_TRUE(cur != NULL);
	EXPECT_DOUBLE_EQ(cur->price(), 3900.0);
	cur->release();
	EXPECT_NE(_sink->last_log(LL_ERROR).find("is less than cached volume"), std::string::npos)
		<< _sink->last_log(LL_ERROR);

	// procFlag==2 明确豁免该检查（L961 的 && procFlag != 2）
	WTSTickStruct s3 = make_tick("EXDV", "rb2301", R4_TD1, R4_TD1, 93200000, 3880.0, 1, 10, 5, 0, 0, 0);
	write_or_fail(tick_of(ct, s3), 2);
	EXPECT_EQ(_sink->bcast_tick(), 2u);
	WTSTickData* cur2 = _writer->getCurTick("rb2301", "EXDV");
	ASSERT_TRUE(cur2 != NULL);
	EXPECT_DOUBLE_EQ(cur2->price(), 3880.0);
	cur2->release();
}

//////////////////////////////////////////////////////////////////////////
// 13. updateBarCache 主干：d1/m1/m5 三个缓存项各自的 OHLCV 语义。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, BarCacheMergesTicksOfDayMinute5AndMinute1)
{
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, false, false, false);
	init_writer(cfg);

	WTSCommodityInfo* comm = _bd->add_commodity("EXBC", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXBC", "rb2301", comm);

	// 09:30:00 → minutes=30 → m1 覆盖到 09:31、m5 覆盖到 09:35
	WTSTickStruct s1 = make_tick("EXBC", "rb2301", R4_TD1, R4_TD1, 93000000, 3900.0, 10, 100, 50, 1, 100, 1000);
	write_or_fail(tick_of(ct, s1), 0);

	BarCacheItem d1, m1, m5;
	ASSERT_TRUE(read_bar_item(path_of("cache_d1.dmb"), "EXBC", "rb2301", d1));
	ASSERT_TRUE(read_bar_item(path_of("cache_m1.dmb"), "EXBC", "rb2301", m1));
	ASSERT_TRUE(read_bar_item(path_of("cache_m5.dmb"), "EXBC", "rb2301", m5));

	EXPECT_EQ(d1._bar.date, R4_TD1);
	// L601/620：day bar 的 time 被写成 barDate = tradingdate
	EXPECT_EQ(d1._bar.time, (uint64_t)R4_TD1);
	EXPECT_DOUBLE_EQ(d1._bar.open, 3900.0);
	EXPECT_DOUBLE_EQ(d1._bar.vol, 10.0);
	EXPECT_EQ(m1._bar.time, (uint64_t)R4_M1_931);
	EXPECT_EQ(m5._bar.time, (uint64_t)R4_M5_935);
	EXPECT_EQ(m1._bar.date, R4_TD1);

	// 同一分钟内的第二笔：走 else 分支
	WTSTickStruct s2 = make_tick("EXBC", "rb2301", R4_TD1, R4_TD1, 93030000, 3910.0, 10, 100, 60, 1, 110, 1100);
	write_or_fail(tick_of(ct, s2), 0);

	ASSERT_TRUE(read_bar_item(path_of("cache_m1.dmb"), "EXBC", "rb2301", m1));
	EXPECT_EQ(m1._bar.time, (uint64_t)R4_M1_931);
	EXPECT_DOUBLE_EQ(m1._bar.close, 3910.0);
	EXPECT_DOUBLE_EQ(m1._bar.high, 3910.0);
	EXPECT_DOUBLE_EQ(m1._bar.low, 3900.0);
	// m1 的 else 分支只累加一次（L754-757）
	EXPECT_DOUBLE_EQ(m1._bar.vol, 20.0);
	EXPECT_DOUBLE_EQ(m1._bar.money, 200.0);

	// 日线同日合并两笔各 10 手、成交额各 100，正确累计值应为 20 / 200。
	ASSERT_TRUE(read_bar_item(path_of("cache_d1.dmb"), "EXBC", "rb2301", d1));
	EXPECT_DOUBLE_EQ(d1._bar.vol, 20.0);
	EXPECT_DOUBLE_EQ(d1._bar.money, 200.0);
	EXPECT_DOUBLE_EQ(d1._bar.hold, 60.0);
	EXPECT_DOUBLE_EQ(d1._bar.add, 2.0);

	// 换分钟：m1 时间前进到 09:32（此时会触发 pipeToM1Bars → min1/EXBC 建库）
	WTSTickStruct s3 = make_tick("EXBC", "rb2301", R4_TD1, R4_TD1, 93130000, 3920.0, 10, 100, 61, 1, 120, 1200);
	write_or_fail(tick_of(ct, s3), 0);
	ASSERT_TRUE(read_bar_item(path_of("cache_m1.dmb"), "EXBC", "rb2301", m1));
	EXPECT_EQ(m1._bar.time, (uint64_t)R4_M1_932);
	EXPECT_DOUBLE_EQ(m1._bar.vol, 10.0);
	EXPECT_TRUE(std::filesystem::exists(_root / "min1" / "EXBC"));
}

//////////////////////////////////////////////////////////////////////////
// 14. 小节最后一分钟回退（L562-565）：15:00 的 tick 归到 1500 这根 bar
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, LastOfSectionTickRollsBackOneMinute)
{
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, false, false, true);
	init_writer(cfg);

	WTSCommodityInfo* comm = _bd->add_commodity("EXLS", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXLS", "rb2301", comm);

	WTSTickStruct s1 = make_tick("EXLS", "rb2301", R4_TD1, R4_TD1, 150000500, 3950.0, 10, 100, 50, 1, 100, 1000);
	write_or_fail(tick_of(ct, s1), 0);

	BarCacheItem m1;
	ASSERT_TRUE(read_bar_item(path_of("cache_m1.dmb"), "EXLS", "rb2301", m1));
	// minutes=360 回退为 359 → barMins=360 → minuteToTime(360)=1500
	EXPECT_EQ(m1._bar.time, (uint64_t)R4_M1_CLOSE);
	EXPECT_DOUBLE_EQ(m1._bar.close, 3950.0);
	// disableday=true → d1 缓存没建索引
	BarCacheItem d1;
	EXPECT_FALSE(read_bar_item(path_of("cache_d1.dmb"), "EXLS", "rb2301", d1));
}

//////////////////////////////////////////////////////////////////////////
// 15. 非交易时段 tick：K 线早退（L556-557），但 tick 缓存和广播照常
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, TickOutsideSessionUpdatesTickCacheOnly)
{
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, false, false, false);
	init_writer(cfg);

	WTSCommodityInfo* comm = _bd->add_commodity("EXOS", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXOS", "rb2301", comm);

	// 20:00 不在 0900-1500 时段内
	WTSTickStruct s1 = make_tick("EXOS", "rb2301", R4_TD1, R4_TD1, 200000000, 3900.0, 10, 100, 50, 1, 100, 1000);
	write_or_fail(tick_of(ct, s1), 0);

	EXPECT_EQ(_sink->bcast_tick(), 1u);
	WTSTickData* cur = _writer->getCurTick("rb2301", "EXOS");
	ASSERT_TRUE(cur != NULL);
	EXPECT_DOUBLE_EQ(cur->price(), 3900.0);
	cur->release();

	BarCacheItem item;
	EXPECT_FALSE(read_bar_item(path_of("cache_m1.dmb"), "EXOS", "rb2301", item));
	EXPECT_FALSE(read_bar_item(path_of("cache_d1.dmb"), "EXOS", "rb2301", item));
	const char* bars[] = {"cache_m1.dmb", "cache_m5.dmb", "cache_d1.dmb"};
	for (const char* name : bars)
		EXPECT_EQ(read_head(path_of(name))._size, 0u) << name;
}

//////////////////////////////////////////////////////////////////////////
// 16. writeTick 的三个入口守卫
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, WriteTickGuardsNullTickNullContractAndClosedSession)
{
	init_writer_all_disabled();

	// L329-330：NULL tick 直接 false
	EXPECT_FALSE(_writer->writeTick(NULL, 0));
	EXPECT_EQ(_sink->bcast_tick(), 0u);

	// L338-339：拿不到合约 → 任务体 break，不广播
	WTSTickStruct s0 = make_tick("EXGD", "rb2301", R4_TD1, R4_TD1, 93000000, 3900.0, 1, 10, 0, 0);
	WTSTickData* t0 = WTSTickData::create(s0);
	t0->setContractInfo(NULL);
	EXPECT_TRUE(_writer->writeTick(t0, 0));
	t0->release();
	EXPECT_EQ(_sink->bcast_tick(), 0u);
	EXPECT_TRUE(_writer->getCurTick("rb2301", "EXGD") == NULL);

	// L344-345：会话拒收
	WTSCommodityInfo* comm = _bd->add_commodity("EXGD", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXGD", "rb2301", comm);
	_sink->set_can_recv(false);
	WTSTickStruct s1 = make_tick("EXGD", "rb2301", R4_TD1, R4_TD1, 93000000, 3900.0, 1, 10, 0, 0);
	write_or_fail(tick_of(ct, s1), 0);
	EXPECT_EQ(_sink->bcast_tick(), 0u);
	EXPECT_TRUE(_writer->getCurTick("rb2301", "EXGD") == NULL);
	EXPECT_EQ(_sink->sid_seen(), std::string("R4_DAY"));

	// 放行后同一条 tick 正常入库
	_sink->set_can_recv(true);
	WTSTickStruct s2 = make_tick("EXGD", "rb2301", R4_TD1, R4_TD1, 93100000, 3910.0, 2, 20, 0, 0, 3, 30);
	write_or_fail(tick_of(ct, s2), 0);
	EXPECT_EQ(_sink->bcast_tick(), 1u);
	EXPECT_DOUBLE_EQ(_sink->last_bcast_price(), 3910.0);
}

//////////////////////////////////////////////////////////////////////////
// 17. getCurTick 的四种输入
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, GetCurTickCoversEmptyUnknownMissAndHit)
{
	init_writer_all_disabled();
	WTSCommodityInfo* comm = _bd->add_commodity("EXGT", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXGT", "rb2301", comm);

	// L844-845：空代码
	EXPECT_TRUE(_writer->getCurTick("") == NULL);
	EXPECT_TRUE(_writer->getCurTick("", "EXGT") == NULL);

	// L847-849：基础数据里没有的合约
	EXPECT_TRUE(_writer->getCurTick("rb9999", "EXGT") == NULL);

	// L854-855：合约有、缓存未命中
	EXPECT_TRUE(_writer->getCurTick("rb2301", "EXGT") == NULL);

	WTSTickStruct s1 = make_tick("EXGT", "rb2301", R4_TD1, R4_TD1, 93000000, 3988.0, 1, 10, 7, 2);
	write_or_fail(tick_of(ct, s1), 0);

	// L857-859：命中，返回的是缓存项的副本
	WTSTickData* hit = _writer->getCurTick("rb2301", "EXGT");
	ASSERT_TRUE(hit != NULL);
	EXPECT_DOUBLE_EQ(hit->price(), 3988.0);
	EXPECT_DOUBLE_EQ(hit->openinterest(), 7.0);
	EXPECT_EQ(hit->tradingdate(), R4_TD1);
	hit->release();
}

//////////////////////////////////////////////////////////////////////////
// 18. 无异步线程时 release 只置位（L108-113 的非线程分支）
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, ReleaseWithoutTaskThreadIsInert)
{
	init_writer_all_disabled();
	WTSCommodityInfo* comm = _bd->add_commodity("EXRL", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXRL", "rb2301", comm);

	WTSTickStruct s1 = make_tick("EXRL", "rb2301", R4_TD1, R4_TD1, 93000000, 3900.0, 1, 10, 0, 0);
	write_or_fail(tick_of(ct, s1), 0);
	ASSERT_EQ(_sink->bcast_tick(), 1u);

	// async 未开启时没有 worker；同步写入保持既有行为。
	_writer->release();
	WTSTickStruct s2 = make_tick("EXRL", "rb2301", R4_TD1, R4_TD1, 93100000, 3910.0, 2, 20, 0, 0, 3, 30);
	write_or_fail(tick_of(ct, s2), 0);
	EXPECT_EQ(_sink->bcast_tick(), 2u);

	WTSTickData* cur = _writer->getCurTick("rb2301", "EXRL");
	ASSERT_TRUE(cur != NULL);
	EXPECT_DOUBLE_EQ(cur->price(), 3910.0);
	cur->release();
}

//////////////////////////////////////////////////////////////////////////
// async 键缺省时 writeTick 在返回前完成同步副作用。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, WriterDefaultsToSynchronousWhenAsyncIsMissing)
{
	init_writer_all_disabled();
	WTSCommodityInfo* comm = _bd->add_commodity("EXSYN", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXSYN", "rb2301", comm);
	WTSTickStruct s = make_tick("EXSYN", "rb2301", R4_TD1, R4_TD1,
		93000000, 3900.0, 1, 10, 50, 0, 1, 10);
	WTSTickData* tick = tick_of(ct, s);
	const bool accepted = _writer->writeTick(tick, 0);
	tick->release();
	ASSERT_TRUE(accepted);
	EXPECT_EQ(_sink->bcast_tick(), 1u);
}

//////////////////////////////////////////////////////////////////////////
// async 显式开启后，release 必须 drain 所有已接受任务；release 后的新任务应拒绝。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, AsyncWriterDrainsAcceptedTicksOnReleaseAndRejectsNewTicks)
{
	init_writer_all_disabled(1000, true);
	WTSCommodityInfo* comm = _bd->add_commodity("EXASY", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXASY", "rb2301", comm);
	const uint32_t tick_count = 64;
	for (uint32_t i = 0; i < tick_count; ++i)
	{
		const uint32_t action_time = 93000000 + (i / 60) * 100000 + (i % 60) * 1000;
		WTSTickStruct tick = make_tick("EXASY", "rb2301", R4_TD1, R4_TD1,
			action_time, 3900.0 + i, 1, 10, 50, 0, 100 + i, 1000 + i);
		write_or_fail(tick_of(ct, tick), 0);
	}

	_writer->release();
	EXPECT_EQ(_sink->bcast_tick(), tick_count);

	WTSTickStruct rejected = make_tick("EXASY", "rb2301", R4_TD1, R4_TD1,
		94000000, 4000.0, 1, 10, 50, 0, 200, 2000);
	WTSTickData* rejected_tick = tick_of(ct, rejected);
	EXPECT_FALSE(_writer->writeTick(rejected_tick, 0));
	rejected_tick->release();
	EXPECT_EQ(_sink->bcast_tick(), tick_count);
}

//////////////////////////////////////////////////////////////////////////
// 两个独立 writer 的异步线程并发更新进程级交易所计数，计数和分组日志不应丢失。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, AsyncWriterTickCountersAreSafeAcrossInstances)
{
	init_writer_all_disabled(2, true);
	WTSCommodityInfo* comm1 = _bd->add_commodity("EXCONC", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct1 = _bd->add_contract("EXCONC", "rb2301", comm1);

	R4BaseDataMgr bd2;
	R4WriterSink sink2(&bd2);
	WtDataWriterAD writer2;
	struct ReleaseOnExit
	{
		WtDataWriterAD& _writer;
		~ReleaseOnExit() { _writer.release(); }
	} release_writer2{writer2};

	const std::filesystem::path root2 = _root / "writer2";
	std::filesystem::create_directories(root2);
	WTSVariant* cfg2 = make_writer_cfg(root2.string(), 2, true, true, true, true, true);
	const bool init2 = writer2.init(cfg2, &sink2);
	cfg2->release();
	ASSERT_TRUE(init2);
	WTSCommodityInfo* comm2 = bd2.add_commodity("EXCONC", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct2 = bd2.add_contract("EXCONC", "rb2301", comm2);

	const uint32_t tick_count_per_writer = 256;
	for (uint32_t i = 0; i < tick_count_per_writer; ++i)
	{
		const uint32_t action_time = 93000000 + (i / 60) * 100000 + (i % 60) * 1000;
		WTSTickStruct first = make_tick("EXCONC", "rb2301", R4_TD1, R4_TD1,
			action_time, 3900.0 + i, 1, 10, 50, 0, 100 + i, 1000 + i);
		write_or_fail(tick_of(ct1, first), 0);

		WTSTickStruct second = make_tick("EXCONC", "rb2301", R4_TD1, R4_TD1,
			action_time, 3900.0 + i, 1, 10, 50, 0, 100 + i, 1000 + i);
		WTSTickData* tick2 = tick_of(ct2, second);
		const bool accepted = writer2.writeTick(tick2, 0);
		tick2->release();
		EXPECT_TRUE(accepted) << i;
	}

	writer2.release();
	_writer->release();
	EXPECT_EQ(_sink->bcast_tick(), tick_count_per_writer);
	EXPECT_EQ(sink2.bcast_tick(), tick_count_per_writer);
	// 两个 writer 共 512 笔、同一交易所、groupsize=2，应恰有 256 条分组日志。
	EXPECT_EQ(_sink->count_containing("ticks received from exchange")
		+ sink2.count_containing("ticks received from exchange"), tick_count_per_writer);
}

//////////////////////////////////////////////////////////////////////////
// groupsize 缺省沿用构造合同中的 1000；真实 writeTick 路径到达取模仍安全。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, MissingGroupSizeUsesDefaultInterval)
{
	init_writer(make_writer_cfg_without_groupsize(_root.string()));
	WTSCommodityInfo* comm = _bd->add_commodity("EXGSM", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXGSM", "rb2301", comm);

	const uint32_t tick_count = 1000;
	for (uint32_t i = 0; i < tick_count; ++i)
	{
		const uint32_t action_time = 93000000 + (i / 60) * 100000 + (i % 60) * 1000;
		WTSTickStruct tick = make_tick("EXGSM", "rb2301", R4_TD1, R4_TD1,
			action_time, 3900.0 + i, 1, 10, 50, 0, i + 1, (i + 1) * 10);
		write_or_fail(tick_of(ct, tick), 0);
		if (i + 1 == tick_count - 1)
			EXPECT_EQ(_sink->count_containing("ticks received from exchange"), 0u);
	}

	EXPECT_EQ(_sink->bcast_tick(), tick_count);
	EXPECT_EQ(_sink->count_containing("ticks received from exchange"), 1u);
}

//////////////////////////////////////////////////////////////////////////
// groupsize=0 是配置错误：init 拒绝，未加载缓存/启动 async worker，之后 writeTick fail-closed。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, ZeroGroupSizeRejectsInitAndBlocksWrites)
{
	WTSVariant* cfg = make_writer_cfg(_root.string(), 0, true, true, true, true, true);
	_writer.reset(new_writer());
	const bool initialized = _writer->init(cfg, _sink);
	cfg->release();
	EXPECT_FALSE(initialized);
	EXPECT_NE(_sink->last_log(LL_ERROR).find("groupsize"), std::string::npos);
	EXPECT_FALSE(std::filesystem::exists(path_of("cache_tick.dmb")));

	WTSCommodityInfo* comm = _bd->add_commodity("EXGS0", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXGS0", "rb2301", comm);
	WTSTickStruct s = make_tick("EXGS0", "rb2301", R4_TD1, R4_TD1,
		93000000, 3900.0, 1, 10, 50, 0, 1, 10);
	WTSTickData* tick = tick_of(ct, s);
	const bool accepted = _writer->writeTick(tick, 0);
	tick->release();
	EXPECT_FALSE(accepted);
	EXPECT_EQ(_sink->bcast_tick(), 0u);
}

//////////////////////////////////////////////////////////////////////////
// 19. groupsize 控制"每交易所累计笔数"日志
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, LogGroupSizeEmitsCountEveryNthTick)
{
	WTSVariant* cfg = make_writer_cfg(_root.string(), 2, true, true, true, true);
	init_writer(cfg);

	WTSCommodityInfo* comm = _bd->add_commodity("EXLGS", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXLGS", "rb2301", comm);

	WTSTickStruct s1 = make_tick("EXLGS", "rb2301", R4_TD1, R4_TD1, 93000000, 3900.0, 1, 10, 0, 0, 1, 10);
	write_or_fail(tick_of(ct, s1), 0);
	// 第 1 笔：只有"First tick of new tradingday"，没有分组日志
	EXPECT_EQ(_sink->last_log(LL_INFO).find("ticks received from exchange"), std::string::npos);

	WTSTickStruct s2 = make_tick("EXLGS", "rb2301", R4_TD1, R4_TD1, 93100000, 3910.0, 1, 10, 0, 0, 2, 20);
	write_or_fail(tick_of(ct, s2), 0);
	EXPECT_NE(_sink->last_log(LL_INFO).find("ticks received from exchange"), std::string::npos)
		<< _sink->last_log(LL_INFO);
	EXPECT_NE(_sink->last_log(LL_INFO).find("EXLGS"), std::string::npos);
}

}  // namespace
