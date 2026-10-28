// r4 / WtDataStorageAD：WtDataWriterAD 的 LMDB 落库链路（tick / m1 / m5 / d1 四条 pipe）
//
// 覆盖对象（src/WtDataStorageAD/WtDataWriterAD.cpp）：
//  - pipeToTicks（L416-448）：交易日做 date、偏移时间做 time 的 LMDBHftKey 构造 + 扩展 dumper
//  - pipeToDayBars / pipeToM1Bars / pipeToM5Bars（L450-547）：LMDBBarKey 与 "d1/m1/m5" 转储
//  - get_k_db（L1016-1053）：min1/min5/day 三个子目录映射 + 交易所级缓存 + create_directories
//  - get_t_db（L1055-1073）：每合约一库（ticks/{exchg}/{code}/）
//  - init 的 tickmapsize / klinemapsize 分支（L95-99）
// 说明：AD 与 WtDataStorage 有一组同名 extern "C" 工厂，本文件只用 AD 侧类型，
// 且所有校验都在临时目录里完成（写入器析构、LMDB 环境关闭以后才回读，避免同进程重复开环）。
#include "r4_ad_fixture.h"

namespace {

struct R4DbEntry
{
	std::string	_key;
	std::string	_val;
};

// 回读一个 LMDB 目录（必须在写入器已析构、环境已关闭之后调用）
// 注意：WtLMDBQuery::get_all（src/WTSUtils/WtLMDB.hpp:371-390）的循环在
// mdb_cursor_get 返回 MDB_NOTFOUND 后仍会把"当前游标位置"多追加一条 —— NOTFOUND
// 不改动 bKey/mData，所以尾行永远与上一行逐字节相同。LMDB 键唯一 ⇒ 相邻两条
// 完全相同的行只可能是这条幻尾行，这里把它剥掉，rows.size() 才等于真实入库条数。
[[maybe_unused]] static std::vector<R4DbEntry> r4_read_db(const std::string& dir)
{
	std::vector<R4DbEntry> ret;
	if (!std::filesystem::exists(dir)) return ret;

	WtLMDB db(false);
	if (!db.open(dir.c_str(), 8 * 1024 * 1024)) return ret;
	WtLMDBQuery query(db);
	query.get_all([&ret](const ValueArray& ayKeys, const ValueArray& ayVals) {
		const std::size_t n = std::min(ayKeys.size(), ayVals.size());
		for (std::size_t i = 0; i < n; i++)
		{
			R4DbEntry e;
			e._key = ayKeys[i];
			e._val = ayVals[i];
			ret.push_back(e);
		}
	});
	if (ret.size() >= 2
		&& ret.back()._key == ret[ret.size() - 2]._key
		&& ret.back()._val == ret[ret.size() - 2]._val)
		ret.pop_back();
	return ret;
}

[[maybe_unused]] static uint32_t be32(uint32_t v)
{
	// 与 LMDBKeys.h 的 reverseEndian 等价：键里的 date/time 都是大端存放
	return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8)
		| ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

//////////////////////////////////////////////////////////////////////////
// 扩展转储替身：记录每条被 pipe 出去的 bar / tick
//////////////////////////////////////////////////////////////////////////
class R4RecordingDumper : public IHisDataDumper
{
public:
	bool dumpHisBars(const char* /*stdCode*/, const char* period, WTSBarStruct* bars, uint32_t count) override
	{
		_bar_calls++;
		_periods[period] += count;
		if (count > 0) _bars.push_back(*bars);
		return _bars_ok;
	}

	bool dumpHisTicks(const char* stdCode, uint32_t uDate, WTSTickStruct* ticks, uint32_t count) override
	{
		_tick_calls++;
		_tick_dates.push_back(uDate);
		if (count > 0) _ticks.push_back(*ticks);
		_last_std_code = stdCode;
		return _ticks_ok;
	}

	void set_bars_ok(bool v) { _bars_ok = v; }
	void set_ticks_ok(bool v) { _ticks_ok = v; }
	uint32_t bar_calls() const { return _bar_calls; }
	uint32_t tick_calls() const { return _tick_calls; }
	uint32_t period_count(const char* p) const
	{
		auto it = _periods.find(p);
		return it == _periods.end() ? 0u : it->second;
	}
	const std::vector<WTSBarStruct>& bars() const { return _bars; }
	const std::vector<WTSTickStruct>& ticks() const { return _ticks; }
	uint32_t tick_date(std::size_t i) const { return i < _tick_dates.size() ? _tick_dates[i] : 0u; }

private:
	uint32_t	_bar_calls = 0;
	uint32_t	_tick_calls = 0;
	bool		_bars_ok = true;
	bool		_ticks_ok = true;
	std::string	_last_std_code;
	wt_hashmap<std::string, uint32_t> _periods;
	std::vector<WTSBarStruct>	_bars;
	std::vector<WTSTickStruct>	_ticks;
	std::vector<uint32_t>		_tick_dates;
};

static const uint32_t R4_D1 = 20230103;
static const uint32_t R4_D2 = 20230104;
static const uint32_t R4_M1_931 = 3301030931u;
static const uint32_t R4_M1_932 = 3301030932u;

//////////////////////////////////////////////////////////////////////////
// 1. tick 库：键 =（交易日, 偏移时间），值 = 完整 WTSTickStruct
//    现实现每合约最多写进 2 笔（get_t_db 命中时 std::move 掏空缓存，
//    见 R4-BUG-02）——本用例固化"第 3 笔起静默丢失"的现象。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, TickDbStoresTradingDateKeyedRecordsUntilHandleMoveOut)
{
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, false, true, true, true);
	init_writer(cfg);

	WTSCommodityInfo* comm = _bd->add_commodity("EXT1", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXT1", "rb2301", comm);

	// 4 笔，交易日相同、动作时间递增、总量递增（不会被 updateTickCache 丢弃）
	const double px[4] = {3900.0, 3910.0, 3920.0, 3930.0};
	const uint32_t at[4] = {93000000, 93100000, 93200000, 93300000};
	for (int i = 0; i < 4; i++)
	{
		WTSTickStruct s = make_tick("EXT1", "rb2301", R4_D1, R4_D1, at[i], px[i], 1, 10, 5, 0, 100 + i, 1000 + i);
		write_or_fail(tick_of(ct, s), 0);
	}
	EXPECT_EQ(_sink->bcast_tick(), 4u);
	// 库句柄为空时既不报错也不日志：丢失是静默的
	EXPECT_EQ(_sink->log_count(LL_ERROR), 0u);

	_writer.reset();
	auto rows = r4_read_db((_root / "ticks" / "EXT1" / "rb2301").string());
	ASSERT_EQ(rows.size(), (std::size_t)2) << "现实现只写进前两笔（get_t_db 的 move 语义）";

	// 键必须是 LMDBHftKey{exchg, code, 大端交易日, 大端偏移时间}
	const LMDBHftKey* k0 = (const LMDBHftKey*)rows[0]._key.data();
	EXPECT_EQ(rows[0]._key.size(), sizeof(LMDBHftKey));
	EXPECT_STREQ(k0->_exchg, "EXT1");
	EXPECT_STREQ(k0->_code, "rb2301");
	EXPECT_EQ(k0->_date, be32(R4_D1));
	EXPECT_EQ(k0->_time, be32(93000000u));
	const LMDBHftKey* k1 = (const LMDBHftKey*)rows[1]._key.data();
	EXPECT_EQ(k1->_time, be32(93100000u));

	ASSERT_EQ(rows[0]._val.size(), sizeof(WTSTickStruct));
	const WTSTickStruct* t0 = (const WTSTickStruct*)rows[0]._val.data();
	EXPECT_DOUBLE_EQ(t0->price, 3900.0);
	EXPECT_EQ(t0->trading_date, R4_D1);
	const WTSTickStruct* t1 = (const WTSTickStruct*)rows[1]._val.data();
	EXPECT_DOUBLE_EQ(t1->price, 3910.0);
}

//////////////////////////////////////////////////////////////////////////
// 2. HFT 键序：date 在前、time 在后且大端 → LMDB 字典序 == 时间序
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, HftKeysSortChronologicallyAcrossTradingDates)
{
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, false, true, true, true);
	init_writer(cfg);

	WTSCommodityInfo* comm = _bd->add_commodity("EXT2", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXT2", "rb2301", comm);

	// 第一笔 20230103 14:59，第二笔跨到 20230104 09:30（键序必须仍然按日优先）
	WTSTickStruct s1 = make_tick("EXT2", "rb2301", R4_D1, R4_D1, 145900000, 3900.0, 1, 10, 5, 0, 100, 1000);
	write_or_fail(tick_of(ct, s1), 0);
	WTSTickStruct s2 = make_tick("EXT2", "rb2301", R4_D2, R4_D2, 93000000, 3910.0, 1, 10, 5, 0, 101, 1010);
	write_or_fail(tick_of(ct, s2), 0);

	_writer.reset();
	auto rows = r4_read_db((_root / "ticks" / "EXT2" / "rb2301").string());
	ASSERT_EQ(rows.size(), (std::size_t)2);
	// 字典序：20230103/1459 必须排在 20230104/0930 之前
	EXPECT_LT(std::memcmp(rows[0]._key.data(), rows[1]._key.data(), sizeof(LMDBHftKey)), 0);
	const LMDBHftKey* ka = (const LMDBHftKey*)rows[0]._key.data();
	const LMDBHftKey* kb = (const LMDBHftKey*)rows[1]._key.data();
	EXPECT_EQ(ka->_date, be32(R4_D1));
	EXPECT_EQ(ka->_time, be32(145900000u));
	EXPECT_EQ(kb->_date, be32(R4_D2));
	EXPECT_EQ(kb->_time, be32(93000000u));
	// 小端原值直接比较会反序：证明确实做了字节序转换，而不是原样写入
	EXPECT_NE(ka->_date, R4_D1);
	EXPECT_NE(ka->_time, 145900000u);
}

//////////////////////////////////////////////////////////////////////////
// 3. 每合约一库：目录按 ticks/{exchg}/{code}/ 切开
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, EachCodeGetsItsOwnTickDbDirectory)
{
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, false, true, true, true);
	init_writer(cfg);

	WTSCommodityInfo* comm = _bd->add_commodity("EXT3", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct1 = _bd->add_contract("EXT3", "rb2301", comm);
	WTSContractInfo* ct2 = _bd->add_contract("EXT3", "rb2302", comm);

	WTSTickStruct s1 = make_tick("EXT3", "rb2301", R4_D1, R4_D1, 93000000, 3901.0, 1, 10, 5, 0);
	write_or_fail(tick_of(ct1, s1), 0);
	WTSTickStruct s2 = make_tick("EXT3", "rb2302", R4_D1, R4_D1, 93000000, 3902.0, 1, 10, 6, 0);
	write_or_fail(tick_of(ct2, s2), 0);

	// 同一交易所下两个合约各建一库
	const auto d1 = _root / "ticks" / "EXT3" / "rb2301";
	const auto d2 = _root / "ticks" / "EXT3" / "rb2302";
	EXPECT_TRUE(std::filesystem::exists(d1));
	EXPECT_TRUE(std::filesystem::exists(d2));
	EXPECT_TRUE(std::filesystem::exists(d1 / "data.mdb"));

	_writer.reset();
	auto r1 = r4_read_db(d1.string());
	auto r2 = r4_read_db(d2.string());
	ASSERT_EQ(r1.size(), (std::size_t)1);
	ASSERT_EQ(r2.size(), (std::size_t)1);
	EXPECT_DOUBLE_EQ(((const WTSTickStruct*)r1[0]._val.data())->price, 3901.0);
	EXPECT_DOUBLE_EQ(((const WTSTickStruct*)r2[0]._val.data())->price, 3902.0);
	EXPECT_STREQ(((const LMDBHftKey*)r2[0]._key.data())->_code, "rb2302");
}

//////////////////////////////////////////////////////////////////////////
// 4. m1 bar 滚动入库：键 = bar.time，值 = 完成的那根 bar
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, MinuteBarRolloverPipesIntoMin1Db)
{
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, false, true, true);
	init_writer(cfg);

	WTSCommodityInfo* comm = _bd->add_commodity("EXM1", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXM1", "rb2301", comm);

	// 5 个不同分钟 → 4 次 bar 滚动 → pipeToM1Bars 被调 4 次
	const uint32_t at[5] = {93000000, 93100000, 93200000, 93300000, 93400000};
	const double px[5] = {3900.0, 3910.0, 3920.0, 3930.0, 3940.0};
	for (int i = 0; i < 5; i++)
	{
		WTSTickStruct s = make_tick("EXM1", "rb2301", R4_D1, R4_D1, at[i], px[i], 1, 10, 5, 0, 100 + i, 1000 + i);
		write_or_fail(tick_of(ct, s), 0);
	}

	_writer.reset();
	auto rows = r4_read_db((_root / "min1" / "EXM1").string());
	// get_k_db 的 move 语义同样把句柄从缓存里掏走：第 3 次起不再入库（R4-BUG-02）
	ASSERT_EQ(rows.size(), (std::size_t)2);
	const LMDBBarKey* k0 = (const LMDBBarKey*)rows[0]._key.data();
	EXPECT_EQ(rows[0]._key.size(), sizeof(LMDBBarKey));
	EXPECT_STREQ(k0->_exchg, "EXM1");
	EXPECT_STREQ(k0->_code, "rb2301");
	EXPECT_EQ(k0->_bartime, be32(R4_M1_931));
	const LMDBBarKey* k1 = (const LMDBBarKey*)rows[1]._key.data();
	EXPECT_EQ(k1->_bartime, be32(R4_M1_932));

	const WTSBarStruct* b0 = (const WTSBarStruct*)rows[0]._val.data();
	EXPECT_EQ(b0->date, R4_D1);
	EXPECT_DOUBLE_EQ(b0->open, 3900.0);
	EXPECT_DOUBLE_EQ(b0->close, 3900.0);
	const WTSBarStruct* b1 = (const WTSBarStruct*)rows[1]._val.data();
	EXPECT_DOUBLE_EQ(b1->close, 3910.0);
}

//////////////////////////////////////////////////////////////////////////
// 5. d1 bar 滚动入库：键 = bar.date；同时固化日线 vol/money 重复累加（R4-BUG-03）
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, DayBarRolloverPipesIntoDayDbWithDuplicatedVolume)
{
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, true, true, false);
	init_writer(cfg);

	WTSCommodityInfo* comm = _bd->add_commodity("EXD5", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXD5", "rb2301", comm);

	WTSTickStruct s1 = make_tick("EXD5", "rb2301", R4_D1, R4_D1, 93000000, 3900.0, 10, 100, 50, 1, 100, 1000);
	write_or_fail(tick_of(ct, s1), 0);
	WTSTickStruct s2 = make_tick("EXD5", "rb2301", R4_D1, R4_D1, 93100000, 3910.0, 10, 100, 60, 1, 110, 1100);
	write_or_fail(tick_of(ct, s2), 0);
	// 跨交易日 → 旧日线出管
	WTSTickStruct s3 = make_tick("EXD5", "rb2301", R4_D2, R4_D2, 93000000, 4000.0, 10, 100, 70, 1, 120, 1200);
	write_or_fail(tick_of(ct, s3), 0);

	_writer.reset();
	auto rows = r4_read_db((_root / "day" / "EXD5").string());
	ASSERT_EQ(rows.size(), (std::size_t)1);
	const LMDBBarKey* k = (const LMDBBarKey*)rows[0]._key.data();
	EXPECT_EQ(k->_bartime, be32(R4_D1));

	const WTSBarStruct* bar = (const WTSBarStruct*)rows[0]._val.data();
	EXPECT_EQ(bar->date, R4_D1);
	EXPECT_DOUBLE_EQ(bar->open, 3900.0);
	EXPECT_DOUBLE_EQ(bar->high, 3910.0);
	EXPECT_DOUBLE_EQ(bar->low, 3900.0);
	EXPECT_DOUBLE_EQ(bar->close, 3910.0);
	// 两笔各 10 手：语义上应为 20，L649-652 各累加两次 → 30
	EXPECT_DOUBLE_EQ(bar->vol, 30.0);
	EXPECT_DOUBLE_EQ(bar->money, 300.0);
	EXPECT_DOUBLE_EQ(bar->hold, 60.0);
}

//////////////////////////////////////////////////////////////////////////
// 6. m5 入库键同样是 bar.time
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, MinuteFiveBarPipesIntoMin5Db)
{
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, true, false, true);
	init_writer(cfg);

	WTSCommodityInfo* comm = _bd->add_commodity("EXM5", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXM5", "rb2301", comm);

	// 09:30 属于 09:35 这根 5 分钟线；09:35 起换成 09:40
	WTSTickStruct s1 = make_tick("EXM5", "rb2301", R4_D1, R4_D1, 93000000, 3900.0, 10, 100, 50, 1, 100, 1000);
	write_or_fail(tick_of(ct, s1), 0);
	WTSTickStruct s2 = make_tick("EXM5", "rb2301", R4_D1, R4_D1, 93500000, 3950.0, 10, 100, 55, 1, 110, 1100);
	write_or_fail(tick_of(ct, s2), 0);

	_writer.reset();
	auto rows = r4_read_db((_root / "min5" / "EXM5").string());
	ASSERT_EQ(rows.size(), (std::size_t)1);
	const LMDBBarKey* k = (const LMDBBarKey*)rows[0]._key.data();
	// (30/5)*5+5 = 35 → 540+35=575 分 → 09:35
	EXPECT_EQ(k->_bartime, be32(3301030935u));
	const WTSBarStruct* bar = (const WTSBarStruct*)rows[0]._val.data();
	EXPECT_DOUBLE_EQ(bar->close, 3900.0);
	EXPECT_DOUBLE_EQ(bar->vol, 10.0);
}

//////////////////////////////////////////////////////////////////////////
// 7. 扩展 dumper：每条 pipe 出去的 tick/bar 都要转储，NULL dumper 被跳过
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, ExtendedDumperReceivesPipedTicksAndBars)
{
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, false, false, false, false);
	init_writer(cfg);

	R4RecordingDumper ok_dumper;
	R4RecordingDumper bad_dumper;
	bad_dumper.set_ticks_ok(false);
	bad_dumper.set_bars_ok(false);
	_writer->add_ext_dumper("rec", &ok_dumper);
	_writer->add_ext_dumper("bad", &bad_dumper);
	_writer->add_ext_dumper("null", NULL);   // L439/L472/L505/L538 的 continue 分支

	WTSCommodityInfo* comm = _bd->add_commodity("EXDP", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXDP", "rb2301", comm);

	WTSTickStruct s1 = make_tick("EXDP", "rb2301", R4_D1, R4_D1, 93000000, 3900.0, 10, 100, 50, 1, 100, 1000);
	write_or_fail(tick_of(ct, s1), 0);
	// 09:30→09:35：m1（31→36 分钟）和 m5（35→40 分钟）各滚一次，d1 仍在本交易日
	WTSTickStruct s2 = make_tick("EXDP", "rb2301", R4_D1, R4_D1, 93500000, 3910.0, 10, 100, 55, 1, 110, 1100);
	write_or_fail(tick_of(ct, s2), 0);

	// 转储次数按 pipe 调用计，与 db 句柄是否失效无关
	EXPECT_EQ(ok_dumper.tick_calls(), 2u);
	EXPECT_STREQ(ok_dumper.ticks()[0].code, "rb2301");
	// dumpHisTicks 收到的是交易日（tradingdate），不是动作日期
	EXPECT_EQ(ok_dumper.tick_date(0), R4_D1);
	EXPECT_EQ(ok_dumper.tick_date(1), R4_D1);
	EXPECT_EQ(ok_dumper.period_count("m1"), 1u);
	EXPECT_EQ(ok_dumper.period_count("m5"), 1u);
	EXPECT_EQ(ok_dumper.period_count("d1"), 0u);
	// 失败转储只记错误日志，不抛不改控制流
	EXPECT_EQ(_sink->log_count(LL_ERROR), 4u) << _sink->last_log(LL_ERROR);
	EXPECT_NE(_sink->last_log(LL_ERROR).find("via extended dumper"), std::string::npos);

	_writer.reset();
}

//////////////////////////////////////////////////////////////////////////
// 8. tickmapsize / klinemapsize 显式配置后仍能正常入库
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, ExplicitMapSizesStillPipeData)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", _root.string().c_str());
	cfg->append("groupsize", (uint32_t)1000);
	cfg->append("disabletick", false);
	cfg->append("disablemin1", true);
	cfg->append("disablemin5", true);
	cfg->append("disableday", true);
	cfg->append("tickmapsize", (uint32_t)(2 * 1024 * 1024));
	cfg->append("klinemapsize", (uint32_t)(2 * 1024 * 1024));
	init_writer(cfg);

	WTSCommodityInfo* comm = _bd->add_commodity("EXMS", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXMS", "rb2301", comm);

	WTSTickStruct s1 = make_tick("EXMS", "rb2301", R4_D1, R4_D1, 93000000, 3900.0, 1, 10, 5, 0);
	write_or_fail(tick_of(ct, s1), 0);
	// K 线全禁用 → 不应出现 min1/min5/day 目录
	EXPECT_FALSE(std::filesystem::exists(_root / "min1"));
	EXPECT_FALSE(std::filesystem::exists(_root / "day"));

	_writer.reset();
	auto rows = r4_read_db((_root / "ticks" / "EXMS" / "rb2301").string());
	ASSERT_EQ(rows.size(), (std::size_t)1);
	EXPECT_DOUBLE_EQ(((const WTSTickStruct*)rows[0]._val.data())->price, 3900.0);
}

//////////////////////////////////////////////////////////////////////////
// 9. 缓存里的时间修正（+200ms）会一并落进 LMDB
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, BumpedActionTimeIsPersistedToTickDb)
{
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, false, true, true, true);
	init_writer(cfg);

	WTSCommodityInfo* comm = _bd->add_commodity("EXBM", "MA", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXBM", "MA301", comm);

	WTSTickStruct s1 = make_tick("EXBM", "MA301", R4_D1, R4_D1, 93000000, 2500.0, 1, 10, 5, 0, 100, 1000);
	write_or_fail(tick_of(ct, s1), 0);
	// 同毫秒且总量不减 → 动作时间被 +200，pipe 时用的是修正后的结构体
	WTSTickStruct s2 = make_tick("EXBM", "MA301", R4_D1, R4_D1, 93000000, 2501.0, 2, 20, 5, 0, 101, 1020);
	write_or_fail(tick_of(ct, s2), 0);

	_writer.reset();
	auto rows = r4_read_db((_root / "ticks" / "EXBM" / "MA301").string());
	ASSERT_EQ(rows.size(), (std::size_t)2);
	const LMDBHftKey* k1 = (const LMDBHftKey*)rows[1]._key.data();
	// 键仍按原始偏移时间生成（L425 在修正之后执行，取的是修正后的 action_time）
	EXPECT_EQ(k1->_time, be32(93000200u));
	const WTSTickStruct* t1 = (const WTSTickStruct*)rows[1]._val.data();
	EXPECT_EQ(t1->action_time, 93000200u);
}

//////////////////////////////////////////////////////////////////////////
// 10. groupsize 为 0（配置缺失）→ 取模崩溃，无法安全启用
//     触发条件：cfg 不带 groupsize → getUInt32 返回 0 → writeTick 任务体内
//     `_tcnt_map[exchg] % _log_group_size`（WtDataWriterAD.cpp:362）整数除零 → SIGFPE。
//     本用例只登记不启用，详见 bugs/R4_adstorage.md R4-BUG-01。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, DISABLED_GroupSizeZeroDividesByZeroOnFirstTick)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", _root.string().c_str());
	cfg->append("disabletick", true);
	cfg->append("disablemin1", true);
	cfg->append("disablemin5", true);
	cfg->append("disableday", true);
	init_writer(cfg);

	WTSCommodityInfo* comm = _bd->add_commodity("EXGS", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXGS", "rb2301", comm);
	WTSTickStruct s1 = make_tick("EXGS", "rb2301", R4_D1, R4_D1, 93000000, 3900.0, 1, 10, 5, 0);
	write_or_fail(tick_of(ct, s1), 0);
	// 启用即 SIGFPE
	FAIL() << "groupsize 缺省为 0 时应触发除零";
}

//////////////////////////////////////////////////////////////////////////
// 11. db 路径被同名普通文件占用 → boost::filesystem::create_directories 抛异常
//     触发条件：预先在 {base}ticks/{exchg}/{code} 放一个普通文件，再写该合约的 tick。
//     WtDataWriterAD.cpp:1064 未带 error_code，异常会从 writeTick 的任务体里逃出
//     → std::terminate。故整条登记为 DISABLED_，见 R4-BUG-05。
//////////////////////////////////////////////////////////////////////////
TEST_F(R4WriterADTest, DISABLED_TickDbPathOccupiedByRegularFileThrowsOutOfWriteTick)
{
	std::filesystem::create_directories(_root / "ticks" / "EXBLK");
	std::ofstream blocker((_root / "ticks" / "EXBLK" / "rb2301").string());
	blocker << "not a directory";
	blocker.close();

	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, false, true, true, true);
	init_writer(cfg);

	WTSCommodityInfo* comm = _bd->add_commodity("EXBLK", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXBLK", "rb2301", comm);
	WTSTickStruct s1 = make_tick("EXBLK", "rb2301", R4_D1, R4_D1, 93000000, 3900.0, 1, 10, 5, 0);
	// 启用即 terminate（未捕获的 boost::filesystem::filesystem_error）
	write_or_fail(tick_of(ct, s1), 0);
}

}  // namespace
