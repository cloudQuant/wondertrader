// r21 / WtDataStorageAD：LMDBKeys.h 键语义 + WtBtDtReaderAD 缺口 + AD 族 extern "C" 入口
//
// 覆盖对象：
//  - src/WtDataStorageAD/LMDBKeys.h:39-46（reverseEndian(uint32)）、:89-96（_LMDBHftKey 构造）、
//    :131-137（_LMDBBarKey 构造）、:24-29（reverseEndian(uint16)，AD 模块内无调用点，见 R21-BUG-08）
//    —— 注意：本轮报表把 LMDBKeys.h 归到 CoverageTests_adstorage（生产目录 WtDataStorageAD），
//    真正计入增量的键构造发生在前两个文件的产品 TU 里
//    （WtRdmDtReaderAD.cpp:122-123/152-153/184-185/300-301/322-323 与
//     WtBtDtReaderAD.cpp:73-74/103-104）；本文件只做键语义的直接断言。
//  - src/WtDataStorageAD/WtBtDtReaderAD.cpp:13-24（pipe_btreader_log 的 sink==NULL 早退）、
//    :51-62（init）、:64-122（read_raw_bars / read_raw_ticks）、
//    :124-193（get_k_db / get_t_db 的存在性检查与 open 失败分支）
//  - src/WtDataStorageAD/WtBtDtReaderAD.cpp:26-39 与 WtRdmDtReaderAD.cpp:33-46
//    （AD 这一族的 extern "C" 工厂；WtDataStorage 的同名入口不在本目标，绝不引用）
#include "r21_ad_fixture.h"

static const char* const K3_EXCHG = "XEX";
static const char* const K3_CODE = "rb2409";

namespace {

// 把键对象摊成字节，便于逐字节断言大端布局
[[maybe_unused]] static std::vector<uint8_t> key_bytes(const void* key, size_t len)
{
	const uint8_t* p = (const uint8_t*)key;
	return std::vector<uint8_t>(p, p + len);
}

[[maybe_unused]] static std::vector<uint8_t> be32(uint32_t v)
{
	std::vector<uint8_t> ret(4, 0);
	ret[0] = (uint8_t)((v >> 24) & 0xFF);
	ret[1] = (uint8_t)((v >> 16) & 0xFF);
	ret[2] = (uint8_t)((v >> 8) & 0xFF);
	ret[3] = (uint8_t)(v & 0xFF);
	return ret;
}

//////////////////////////////////////////////////////////////////////////
// AD 这一族的 extern "C" 入口（WtDataStorage 的同名工厂不进本目标，绝不引用）
//////////////////////////////////////////////////////////////////////////
extern "C" {
	IRdmDtReader* createRdmDtReader(void);
	void deleteRdmDtReader(IRdmDtReader* reader);
	IBtDtReader* createBtDtReader(void);
	void deleteBtDtReader(IBtDtReader* reader);
}

[[maybe_unused]] static WTSVariant* root_cfg(const std::filesystem::path& root)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", root.string().c_str());
	return cfg;
}

// 带根目录的 init（cfg 用完即 release，不在测试里留悬挂的引用计数对象）
[[maybe_unused]] static void bt_init_rooted(IBtDtReader& bt, const std::filesystem::path& root,
	IBtDtReaderSink* sink)
{
	WTSVariant* cfg = root_cfg(root);
	bt.init(cfg, sink);
	cfg->release();
}

[[maybe_unused]] static void rdm_init_rooted(IRdmDtReader* reader, const std::filesystem::path& root,
	IRdmDtReaderSink* sink)
{
	WTSVariant* cfg = root_cfg(root);
	reader->init(cfg, sink);
	cfg->release();
}

[[maybe_unused]] static void write_regular_file(const std::filesystem::path& p)
{
	std::filesystem::create_directories(p.parent_path());
	std::ofstream fout(p);
	fout << "not a directory";
	fout.close();
}

}

// ---------------------------------------------------------------------------
// reverseEndian(uint32)：4 个字节各自归位，且自反
// ---------------------------------------------------------------------------
TEST(R21LmdbKeys, ReverseEndian32IsSelfInverseAndMovesLowByteToTop)
{
	EXPECT_EQ(reverseEndian(0u), 0u);
	EXPECT_EQ(reverseEndian(1u), 0x01000000u);
	EXPECT_EQ(reverseEndian(0x000000FFu), 0xFF000000u);
	// uint32_t 字面量实参选定的是 4 字节重载（LMDBKeys.h:39-46）：
	// rev(0x0000FF00) = (0x00<<24)+(0xFF<<16)+(0x00<<8)+0x00 = 0x00FF0000，
	// 与下行 rev(0x00FF0000)=0x0000FF00 互为自反。R32 修正：期望从 0x000000FF
	// 改为 0x00FF0000（此前误按 2 字节重载 LMDBKeys.h:24-29 的换位去写）。
	EXPECT_EQ(reverseEndian(0x0000FF00u), 0x00FF0000u);
	EXPECT_EQ(reverseEndian(0x00FF0000u), 0x0000FF00u);
	EXPECT_EQ(reverseEndian(0xFF000000u), 0x000000FFu);
	EXPECT_EQ(reverseEndian(0xFFFFFFFFu), 0xFFFFFFFFu);
	const uint32_t probes[5] = { 20240102u, 93015500u, 3301030931u, 0u, 0xffffffffu };
	for (uint32_t i = 0; i < 5; i++)
		EXPECT_EQ(reverseEndian(reverseEndian(probes[i])), probes[i]) << "probe " << i;
}

// ---------------------------------------------------------------------------
// reverseEndian(uint16)：AD 模块（WtDataStorageAD 四个 .cpp）没有任何调用点，
// 是 LMDBKeys.h 里唯一无法由生产路径带进来的函数（见 R21-BUG-08）
// ---------------------------------------------------------------------------
TEST(R21LmdbKeys, ReverseEndian16SwapsTheTwoBytes)
{
	EXPECT_EQ(reverseEndian((uint16_t)0x0000), (uint16_t)0x0000);
	EXPECT_EQ(reverseEndian((uint16_t)0x0001), (uint16_t)0x0100);
	EXPECT_EQ(reverseEndian((uint16_t)0x1234), (uint16_t)0x3412);
	EXPECT_EQ(reverseEndian((uint16_t)0xFF00), (uint16_t)0x00FF);
	EXPECT_EQ(reverseEndian(reverseEndian((uint16_t)0x9A87)), (uint16_t)0x9A87);
}

// ---------------------------------------------------------------------------
// 键的二进制布局：packed 后 = 交易所(16) + 合约(32) [+ 交易日(4)] + 大端尾部，
// 未用满的字符数组以 0 填充（定长比较靠这些 0 对齐）
// ---------------------------------------------------------------------------
TEST(R21LmdbKeys, HftAndBarKeyLayoutArePackedWithBigEndianTails)
{
	LMDBHftKey hft(K3_EXCHG, K3_CODE, 20240102, 93015500);
	const size_t code_off = MAX_EXCHANGE_LENGTH;
	const size_t date_off = MAX_EXCHANGE_LENGTH + MAX_INSTRUMENT_LENGTH;
	const size_t time_off = date_off + 4;
	EXPECT_EQ(sizeof(LMDBHftKey), time_off + 4);
	EXPECT_EQ(hft._date, reverseEndian(20240102u));
	EXPECT_EQ(hft._time, reverseEndian(93015500u));

	std::vector<uint8_t> raw = key_bytes(&hft, sizeof(hft));
	for (size_t i = 0; i < 3; i++)
		EXPECT_EQ(raw[i], (uint8_t)K3_EXCHG[i]) << "exchg byte " << i;
	for (size_t i = 3; i < (size_t)MAX_EXCHANGE_LENGTH; i++)
		EXPECT_EQ(raw[i], 0) << "exchg pad byte " << i;
	for (size_t i = 0; i < 6; i++)
		EXPECT_EQ(raw[code_off + i], (uint8_t)K3_CODE[i]) << "code byte " << i;
	for (size_t i = 6; i < (size_t)MAX_INSTRUMENT_LENGTH; i++)
		EXPECT_EQ(raw[code_off + i], 0) << "code pad byte " << i;
	const std::vector<uint8_t> want_date = be32(20240102);
	const std::vector<uint8_t> want_time = be32(93015500);
	for (size_t i = 0; i < 4; i++)
	{
		EXPECT_EQ(raw[date_off + i], want_date[i]) << "date byte " << i;
		EXPECT_EQ(raw[time_off + i], want_time[i]) << "time byte " << i;
	}

	LMDBBarKey bar(K3_EXCHG, K3_CODE, 3301030931u);
	const size_t tail_off = MAX_EXCHANGE_LENGTH + MAX_INSTRUMENT_LENGTH;
	EXPECT_EQ(sizeof(LMDBBarKey), tail_off + 4);
	EXPECT_EQ(bar._bartime, reverseEndian(3301030931u));
	const std::vector<uint8_t> bar_raw = key_bytes(&bar, sizeof(bar));
	const std::vector<uint8_t> want_tail = be32(3301030931u);
	for (size_t i = 0; i < 4; i++)
		EXPECT_EQ(bar_raw[tail_off + i], want_tail[i]) << "bartime byte " << i;
}

// ---------------------------------------------------------------------------
// 字节序即排序序：memcmp 顺序 == (交易所, 合约, 日期, 时间) 的数值顺序；
// 哨兵 0 / 0xffffffff 框住同一合约的全部键；短合约名因 0 填充排在同名长码之前
// ---------------------------------------------------------------------------
TEST(R21LmdbKeys, KeyByteOrderSortsByExchgCodeThenTimeAndSentinelsBoundEverything)
{
	const LMDBBarKey k0(K3_EXCHG, K3_CODE, 0);
	const LMDBBarKey kA(K3_EXCHG, K3_CODE, 20240102);
	const LMDBBarKey kB(K3_EXCHG, K3_CODE, 20240103);
	const LMDBBarKey kMax(K3_EXCHG, K3_CODE, 0xffffffff);
	const LMDBBarKey other(K3_EXCHG, "rb2410", 20240102);
	const LMDBBarKey shortMax(K3_EXCHG, "rb", 0xffffffff);

	EXPECT_LT(r21_key_str(kA).compare(r21_key_str(kB)), 0);
	EXPECT_LT(r21_key_str(k0).compare(r21_key_str(kA)), 0);
	EXPECT_LT(r21_key_str(kB).compare(r21_key_str(kMax)), 0);
	// "rb2409" 与 "rb2410" 在第 5 个字节分别是 '0' / '1'
	EXPECT_LT(r21_key_str(kA).compare(r21_key_str(other)), 0);
	// "rb" 的上界仍在 "rb2409" 的下界之前
	EXPECT_LT(r21_key_str(shortMax).compare(r21_key_str(k0)), 0);

	const LMDBHftKey t0(K3_EXCHG, K3_CODE, 20240102, 0);
	const LMDBHftKey t1(K3_EXCHG, K3_CODE, 20240102, 93015500);
	const LMDBHftKey t2(K3_EXCHG, K3_CODE, 20240102, 150000000);
	const LMDBHftKey t3(K3_EXCHG, K3_CODE, 20240103, 93000000);
	EXPECT_LT(r21_key_str(t0).compare(r21_key_str(t1)), 0);
	EXPECT_LT(r21_key_str(t1).compare(r21_key_str(t2)), 0);
	// 交易日优先于时间：跨日之后时间不再可比
	EXPECT_LT(r21_key_str(t2).compare(r21_key_str(t3)), 0);
}

// ---------------------------------------------------------------------------
// _sink == NULL 时 pipe_btreader_log 必须早退（WtBtDtReaderAD.cpp:16-17），
// 读库本身照常返回字节：
//   day 库 2 根（键尾 = date）→ buffer = 2*sizeof(WTSBarStruct)
//   ticks 库 2 笔（交易日 20240102）→ buffer = 2*sizeof(WTSTickStruct)，
//   键区间 [ (20240102,0), (20240102,240000000) ]（:103-104）
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, BtRawBarsAndTicksWithNullSinkFillBuffersAndLogNothing)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(r21_make_bar(20240102, 20240102, 900));
	bars.push_back(r21_make_bar(20240103, 20240103, 901));
	put_bar_db("day", K3_EXCHG, K3_CODE, bars, true);

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r21_make_tick(K3_EXCHG, K3_CODE, 20240102, 20240102, 93015500, 3901));
	ticks.push_back(r21_make_tick(K3_EXCHG, K3_CODE, 20240102, 20240102, 100015500, 3902));
	put_tick_db(K3_EXCHG, K3_CODE, ticks);

	WtBtDtReaderAD bt;
	bt_init_rooted(bt, _root, NULL);

	std::string buffer;
	ASSERT_TRUE(bt.read_raw_bars(K3_EXCHG, K3_CODE, KP_DAY, buffer));
	EXPECT_EQ(buffer.size(), sizeof(WTSBarStruct) * 2);
	const WTSBarStruct* got = (const WTSBarStruct*)buffer.data();
	ASSERT_TRUE(got != NULL);
	// 键序即日期序：20240102 在前
	EXPECT_EQ(got[0].date, 20240102u);
	EXPECT_EQ(got[1].date, 20240103u);
	// make_bar: close = base+1、vol = base*3、money = base*300
	EXPECT_NEAR(got[0].close, 901.0, 1e-9);
	EXPECT_NEAR(got[1].vol, 2703.0, 1e-9);
	EXPECT_NEAR(got[1].money, 270300.0, 1e-9);

	std::string tick_buffer;
	ASSERT_TRUE(bt.read_raw_ticks(K3_EXCHG, K3_CODE, 20240102, tick_buffer));
	EXPECT_EQ(tick_buffer.size(), sizeof(WTSTickStruct) * 2);
	const WTSTickStruct* gt = (const WTSTickStruct*)tick_buffer.data();
	ASSERT_TRUE(gt != NULL);
	EXPECT_EQ(gt[0].action_time, 93015500u);
	EXPECT_EQ(gt[1].price, 3902.0);
	EXPECT_EQ(gt[1].trading_date, 20240102u);
	// 全程没有 sink，任何日志都到不了别处（早退即返回，不解引用 NULL）
	EXPECT_EQ(sink()->total(), 0u);
}

// ---------------------------------------------------------------------------
// 指定交易日没有任何键：get_range 回调收到空数组 → :107-108 早退 →
// 函数仍然返回 true、buffer 一个字节都没有（调用方分不清「没数据」与「读失败」，
// 见 R21-BUG-09）
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, BtReadRawTicksOnDateWithoutRecordsReturnsTrueWithEmptyBuffer)
{
	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r21_make_tick(K3_EXCHG, K3_CODE, 20240102, 20240102, 93015500, 3901));
	put_tick_db(K3_EXCHG, K3_CODE, ticks);

	R21BtSink btsink;
	WtBtDtReaderAD bt;
	bt_init_rooted(bt, _root, &btsink);

	std::string buffer;
	ASSERT_TRUE(bt.read_raw_ticks(K3_EXCHG, K3_CODE, 20240103, buffer));
	EXPECT_TRUE(buffer.empty());
	// 两条 DEBUG：先 "Tick db of ... opened"（get_t_db），再 "Reading back ticks on ..."
	EXPECT_EQ(btsink.count_of(LL_DEBUG), 2u);
	EXPECT_EQ(btsink.last_log(LL_DEBUG), "Reading back ticks on 20240103 of XEX.rb2409...");
}

// ---------------------------------------------------------------------------
// init 的 cfg 开关（:55-61）：cfg==NULL 一条日志都不打、_base_dir 留空
// → 后续读取的路径退化成 "ticks/XEX/rb2409"（相对 cwd），本用例只验证
// 它「不存在即返回 false」，不创建任何目录；带 cfg 时打 INFO 且路径带结尾斜杠。
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, BtInitCfgGateControlsRootDirInfoLogAndReadReachability)
{
	R21BtSink bare_sink;
	WtBtDtReaderAD bare;
	bare.init(NULL, &bare_sink);
	// cfg==NULL 的早退（:55-56）在 INFO 之前，一条日志都不该有
	EXPECT_EQ(bare_sink.count_of(LL_INFO), 0u);
	std::string buffer;
	// _base_dir 为空 → 路径退化成 "ticks/XEX/rb2409"（相对 cwd），本用例不建任何目录
	EXPECT_FALSE(bare.read_raw_ticks(K3_EXCHG, K3_CODE, 20240102, buffer));
	EXPECT_TRUE(buffer.empty());
	EXPECT_FALSE(bare.read_raw_bars(K3_EXCHG, K3_CODE, KP_DAY, buffer));
	EXPECT_TRUE(buffer.empty());
	// INFO 永远只可能来自带 cfg 的 init，这里必须还是 0
	EXPECT_EQ(bare_sink.count_of(LL_INFO), 0u);

	R21BtSink cfg_sink;
	WtBtDtReaderAD with_cfg;
	bt_init_rooted(with_cfg, _root, &cfg_sink);
	EXPECT_EQ(cfg_sink.count_of(LL_INFO), 1u);
	EXPECT_EQ(cfg_sink.last_log(LL_INFO),
		"WtBtDtReaderAD initialized, root data dir is " + _root.string() + "/");
	// 临时目录里没有 day/XEX → 存在性检查（:152-153）在 open 之前就把门关了
	EXPECT_FALSE(with_cfg.read_raw_bars(K3_EXCHG, K3_CODE, KP_DAY, buffer));
	EXPECT_TRUE(buffer.empty());
	EXPECT_EQ(cfg_sink.count_of(LL_ERROR), 0u);
	EXPECT_FALSE(std::filesystem::exists(_root / "day"));
}

// ---------------------------------------------------------------------------
// 路径被同名普通文件占用时，两个读取分支的命运完全不同：
//  - get_k_db 的 :151 拼出的路径**带结尾斜杠**（"{base}day/XEX/"），
//    POSIX 对「普通文件 + 尾斜杠」的 stat 必失败 → StdFile::exists(:152) 为假
//    → :153 直接返回空句柄，连 mdb_env_open 都不做 → 一条 LL_ERROR 都没有；
//  - get_t_db 的 :177 拼出的路径**不带**结尾斜杠 → exists 为真 →
//    mdb_env_open 对普通文件路径只能失败 → :181-185 的 LL_ERROR 分支被触达。
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, BtDbPathOccupiedByRegularFileTakesExistenceAndOpenFailures)
{
	write_regular_file(_root / "day" / K3_EXCHG);
	write_regular_file(_root / "ticks" / K3_EXCHG / K3_CODE);

	R21BtSink btsink;
	WtBtDtReaderAD bt;
	bt_init_rooted(bt, _root, &btsink);

	std::string buffer;
	// 带尾斜杠：存在性检查直接拦下（无日志、无 open）
	EXPECT_FALSE(bt.read_raw_bars(K3_EXCHG, K3_CODE, KP_DAY, buffer));
	EXPECT_TRUE(buffer.empty());
	EXPECT_EQ(btsink.count_of(LL_ERROR), 0u);

	// 不带尾斜杠：走到 open 失败分支
	EXPECT_FALSE(bt.read_raw_ticks(K3_EXCHG, K3_CODE, 20240102, buffer));
	EXPECT_TRUE(buffer.empty());
	EXPECT_EQ(btsink.count_of(LL_ERROR), 1u);
	EXPECT_NE(btsink.last_log(LL_ERROR).find("Opening tick db of XEX.rb2409 failed:"), std::string::npos);
}

// ---------------------------------------------------------------------------
// AD 一族 extern "C" 工厂：createRdmDtReader / deleteRdmDtReader；
// delete(NULL) 必须是空操作（WtRdmDtReaderAD.cpp:41-45）
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, AdRdmFactoryBuildsUsableReaderAndToleratesNullDelete)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(r21_make_bar(20240102, 20240102, 1100));
	bars.push_back(r21_make_bar(20240103, 20240103, 1101));
	put_bar_db("day", K3_EXCHG, K3_CODE, bars, true);

	IRdmDtReader* reader = createRdmDtReader();
	ASSERT_TRUE(reader != NULL);
	// 工厂产物必须是 AD 这一族
	EXPECT_TRUE(dynamic_cast<WtRdmDtReaderAD*>(reader) != NULL);
	rdm_init_rooted(reader, _root, sink());

	// 2 根：eBar.date = 20240103 = 末根（不回退）；首根 date == sBar.date
	// → :376 分支 sIdx = 0 → cnt = 1 - 0 + 1 = 2
	WTSKlineSlice* slice = reader->readKlineSliceByRange("XEX.rb.rb2409", KP_DAY,
		202401020930ull, 202401031500ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->get_block_size(0), 2u);
	const WTSBarStruct* out = (const WTSBarStruct*)slice->get_block_addr(0);
	ASSERT_TRUE(out != NULL);
	EXPECT_NEAR(out[1].close, 1102.0, 1e-9);
	slice->release();

	EXPECT_EQ(sink()->count_containing("day db of XEX opened"), 1u);
	deleteRdmDtReader(reader);
	// NULL 入参只走 if 的假分支，不崩
	deleteRdmDtReader(NULL);
}

// ---------------------------------------------------------------------------
// createBtDtReader / deleteBtDtReader（WtBtDtReaderAD.cpp:28-38）
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, AdBtFactoryBuildsUsableReaderAndToleratesNullDelete)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(r21_make_bar(20240102, 20240102, 1200));
	put_bar_db("min1", K3_EXCHG, K3_CODE, bars, true);

	IBtDtReader* reader = createBtDtReader();
	ASSERT_TRUE(reader != NULL);
	EXPECT_TRUE(dynamic_cast<WtBtDtReaderAD*>(reader) != NULL);

	R21BtSink btsink;
	bt_init_rooted(*reader, _root, &btsink);

	std::string buffer;
	// m1 库里的键尾是 date（20240102），read_raw_bars 用 [0,0xffffffff] 全量取 → 1 根
	ASSERT_TRUE(reader->read_raw_bars(K3_EXCHG, K3_CODE, KP_Minute1, buffer));
	EXPECT_EQ(buffer.size(), sizeof(WTSBarStruct));
	const WTSBarStruct* got = (const WTSBarStruct*)buffer.data();
	ASSERT_TRUE(got != NULL);
	EXPECT_EQ(got[0].date, 20240102u);
	EXPECT_NEAR(got[0].close, 1201.0, 1e-9);
	// :71 的日志用 PERIOD_NAME[period]（WTSTypes.h:115-123），KP_Minute1 → "min1"
	// （R32 修正：期望从 "m1" 改为 "min1"，原文以产品格式串 "{} {} bars..." 为准）
	EXPECT_EQ(btsink.last_log(LL_DEBUG), "Reading back min1 bars of XEX.rb2409...");
	deleteBtDtReader(reader);
	deleteBtDtReader(NULL);
	EXPECT_TRUE(std::filesystem::exists(_root / "min1" / K3_EXCHG));
}

// ---------------------------------------------------------------------------
// NULL sink 的直接调用不作为产品缺陷复现：WtDataManager::init 正常通过 init(cfg, this)
// 提供 sink，且本文件的工厂正控覆盖了可用的非空 sink。接口未声明 NULL sink 合同。
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, DISABLED_RdmInitWithNullSinkContractPending)
{
	GTEST_SKIP() << "WtDataManager::init passes this as sink; NULL sink support is unspecified.";
}
