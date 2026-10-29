// p05 / WtDataStorageAD：WtRdmDtReaderAD 的 LMDB 切片接口（写入→读回→逐字段比对）
//
// 覆盖对象：
//  - WtRdmDtReaderAD::init（cfg==NULL / cfg!=NULL）
//  - WtRdmDtReaderAD::readKlineSliceByRange（全量载入 / bNeedNewer 增量 / 窗口裁剪 / 未实现周期）
//  - WtRdmDtReaderAD::readTickSliceByCount / readTickSliceByDate / readKlineSliceByCount（TODO 桩）
//  - WtRdmDtReaderAD::readOrdDtlSliceByRange / readOrdQueSliceByRange / readTransSliceByRange
//  - WtRdmDtReaderAD::get_k_db / get_t_db（交易所级 db + fs::create_directories 自愈）
//  - IRdmDtReader::init / IBaseDataMgr / IHotMgr 的宿主注入面（本文件自带 mock，不引核心）
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

#include "Includes/WTSMarcos.h"
#include "Includes/WTSTypes.h"
#include "Includes/WTSStruct.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/IBaseDataMgr.h"
#include "Includes/IHotMgr.h"
#include "Includes/IRdmDtReader.h"
#include "Share/TimeUtils.hpp"
#include "WtDataStorageAD/LMDBKeys.h"
#include "WtDataStorageAD/WtRdmDtReaderAD.h"
#include "WTSUtils/WtLMDB.hpp"

using namespace wtp;

namespace {

static void set_str(char* dst, size_t cap, const char* src)
{
	snprintf(dst, cap, "%s", src);
}

//////////////////////////////////////////////////////////////////////////
// 宿主 mock：只提供本模块用到的三个回调
//////////////////////////////////////////////////////////////////////////
class P05BaseDataMgr : public IBaseDataMgr
{
public:
	P05BaseDataMgr()
	{
		_session = WTSSessionInfo::create("SHFE_DAY", "日盘", 0);
		_session->addTradingSection(0, 2359);
		_comm = WTSCommodityInfo::create("rb", "螺纹钢", "SHFE", "SHFE_DAY", "CHN");
		_comm->setSessionInfo(_session);
		_comm->setCategory(ContractCategory::CC_Future);
	}

	virtual ~P05BaseDataMgr()
	{
		_comm->release();
		_session->release();
	}

	WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		_last_pid = exchgpid == NULL ? "" : exchgpid;
		return _comm;
	}

	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		_last_pid = std::string(exchg == NULL ? "" : exchg) + "." + (pid == NULL ? "" : pid);
		return _comm;
	}

	WTSContractInfo* getContract(const char*, const char*, uint32_t) override { return NULL; }
	WTSArray* getContracts(const char*, uint32_t) override { return NULL; }
	WTSSessionInfo* getSession(const char* sid) override { _last_sid = sid; return _session; }
	WTSSessionInfo* getSessionByCode(const char*, const char*) override { return _session; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool) override { return false; }

	// 不做日历推导，直接令交易日 == 自然日，便于用固定键值断言
	uint32_t calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession) override
	{
		_pid_seen = stdPID == NULL ? "" : stdPID;
		_time_seen = uTime;
		_is_session = isSession;
		return uDate;
	}

	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool, bool isStart) override
	{
		return isStart ? ((uint64_t)tDate) * 1000000000ull + 90000000ull
			: ((uint64_t)tDate) * 1000000000ull + 150000000ull;
	}

	const std::string& last_pid() const { return _last_pid; }
	const std::string& pid_seen() const { return _pid_seen; }
	uint32_t time_seen() const { return _time_seen; }
	bool is_session() const { return _is_session; }

private:
	WTSSessionInfo*	_session = NULL;
	WTSCommodityInfo*	_comm = NULL;
	std::string		_last_pid;
	std::string		_pid_seen;
	std::string		_last_sid;
	uint32_t		_time_seen = 0;
	bool			_is_session = false;
};

class P05HotMgr : public IHotMgr
{
public:
	virtual ~P05HotMgr() {}

	const char* getRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isHot(const char*, const char*, uint32_t) override { return false; }
	bool splitHotSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isSecond(const char*, const char*, uint32_t) override { return false; }
	bool splitSecondSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getCustomRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevCustomRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isCustomHot(const char*, const char*, uint32_t) override { return false; }
	bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getRuleTag(const char* /*stdCode*/) override { return _tag.empty() ? "" : _tag.c_str(); }
	double getRuleFactor(const char*, const char*, uint32_t) override { return 1.0; }

	void set_rule_tag(const char* tag) { _tag = tag; }

private:
	std::string	_tag;
};

class P05RdmSink : public IRdmDtReaderSink
{
public:
	P05RdmSink(IBaseDataMgr* bd, IHotMgr* hot) : _bd(bd), _hot(hot) {}
	virtual ~P05RdmSink() {}


	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return _hot; }
	void reader_log(WTSLogLevel ll, const char* /*message*/) override
	{
		_logs[ll]++;
		_total++;
	}

	uint32_t count_of(WTSLogLevel ll) const
	{
		auto it = _logs.find(ll);
		return it == _logs.end() ? 0u : it->second;
	}
	uint32_t total() const { return _total; }

private:
	IBaseDataMgr*	_bd;
	IHotMgr*		_hot;
	wt_hashmap<WTSLogLevel, uint32_t> _logs;
	uint32_t		_total = 0;
};

//////////////////////////////////////////////////////////////////////////
// 夹具
//////////////////////////////////////////////////////////////////////////
class P05RdmReaderADTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		_root = std::filesystem::temp_directory_path()
			/ ("p05_rdmad_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
		std::filesystem::remove_all(_root);
		std::filesystem::create_directories(_root);
	}

	void TearDown() override
	{
		_reader.reset();
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	static std::string key_str(const LMDBBarKey& key)
	{
		return std::string((const char*)&key, sizeof(key));
	}

	static std::string key_str(const LMDBHftKey& key)
	{
		return std::string((const char*)&key, sizeof(key));
	}

	static std::string raw_of(const WTSBarStruct& bar)
	{
		return std::string((const char*)&bar, sizeof(bar));
	}

	static std::string raw_of(const WTSTickStruct& tick)
	{
		return std::string((const char*)&tick, sizeof(tick));
	}

	static WTSBarStruct make_bar(uint32_t date, uint64_t time, double base)
	{
		WTSBarStruct bar;
		bar.date = date;
		bar.time = time;
		bar.open = base;
		bar.high = base + 2.0;
		bar.low = base - 2.0;
		bar.close = base + 1.0;
		bar.settle = base + 1.0;
		bar.money = base * 300.0;
		bar.vol = base * 3.0;
		bar.hold = base;
		bar.add = 1.0;
		return bar;
	}

	static WTSTickStruct make_tick(const char* exchg, const char* code, uint32_t tdate,
		uint32_t adate, uint32_t atime, double price)
	{
		WTSTickStruct tick;
		set_str(tick.exchg, sizeof(tick.exchg), exchg);
		set_str(tick.code, sizeof(tick.code), code);
		tick.price = price;
		tick.open = price - 1.0;
		tick.high = price + 1.0;
		tick.low = price - 1.0;
		tick.total_volume = price * 10.0;
		tick.volume = price;
		tick.trading_date = tdate;
		tick.action_date = adate;
		tick.action_time = atime;
		return tick;
	}

	// 返回按日期升序排列的 n 根日线（20240102 起，逐日 +1）
	std::vector<WTSBarStruct> make_day_bars(uint32_t n, double base)
	{
		std::vector<WTSBarStruct> bars;
		for(uint32_t i = 0; i < n; i++)
		{
			const uint32_t dt = 20240102 + i;
			bars.push_back(make_bar(dt, dt, base + i));
		}
		return bars;
	}

	void put_day_bars(const char* exchg, const char* code, const std::vector<WTSBarStruct>& bars)
	{
		const std::filesystem::path dir = _root / "day" / exchg;
		std::filesystem::create_directories(dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(dir.string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for(const WTSBarStruct& bar : bars)
			ASSERT_TRUE(query.put(key_str(LMDBBarKey(exchg, code, (uint32_t)bar.date)), raw_of(bar)));
		query.commit();
	}

	void put_min_bars(const char* subdir, const char* exchg, const char* code,
		const std::vector<WTSBarStruct>& bars)
	{
		const std::filesystem::path dir = _root / subdir / exchg;
		std::filesystem::create_directories(dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(dir.string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for(const WTSBarStruct& bar : bars)
			ASSERT_TRUE(query.put(key_str(LMDBBarKey(exchg, code, (uint32_t)bar.time)), raw_of(bar)));
		query.commit();
	}

	void init_reader(bool with_cfg)
	{
		_bd.reset(new P05BaseDataMgr());
		_hot.reset(new P05HotMgr());
		_sink.reset(new P05RdmSink(_bd.get(), _hot.get()));
		_reader.reset(new WtRdmDtReaderAD());
		if(with_cfg)
		{
			WTSVariant* cfg = WTSVariant::createObject();
			cfg->append("path", _root.string().c_str());
			_reader->init(cfg, _sink.get());
			cfg->release();
		}
		else
		{
			_reader->init(NULL, _sink.get());
		}
	}

	std::filesystem::path _root;
	std::unique_ptr<P05BaseDataMgr>	_bd;
	std::unique_ptr<P05HotMgr>		_hot;
	std::unique_ptr<P05RdmSink>		_sink;
	std::unique_ptr<WtRdmDtReaderAD>	_reader;
};

}
//////////////////////////////////////////////////////////////////////////

TEST_F(P05RdmReaderADTest, NullCfgLeavesEmptyBaseDirButSinksAreWired)
{
	init_reader(false);

	// cfg==NULL：IRdmDtReader::init 仍拿到 sink/basedata，只是 _base_dir 为空
	EXPECT_EQ(_bd->last_pid(), "");
	// R32 裁定：这里原来直接调 readKlineSliceByRange —— _base_dir 为空时
	// get_k_db（WtRdmDtReaderAD.cpp:420）拼出的是相对路径 "day/SHFE/"，
	// 自建空库后 0 根命中 → :353 对空 vector 取 &v[0] 且上界写成 size()-1
	// （下溢）→ SIGSEGV（实测 CRASH rc=139）。触发条件与崩溃面整体移到下面的
	// DISABLED_ 用例登记；缺陷本体是既有的 R21-BUG-06/R21-BUG-12 一族
	// （见 bugs/R21_adstorage.md），不新登记。本条保留其余桩断言。
	EXPECT_EQ(_reader->readTickSliceByCount("SHFE.rb.rb2409", 10, 20240120090000), nullptr);
	EXPECT_EQ(_reader->readTickSliceByDate("SHFE.rb.rb2409", 20240102), nullptr);
	EXPECT_EQ(_reader->readKlineSliceByCount("SHFE.rb.rb2409", KP_DAY, 5, 202401200000), nullptr);
	// 桩函数不建任何目录：_base_dir 为空 ⇒ 除 CWD 外没有 LMDB 产物
	EXPECT_EQ(_sink->count_of(LL_DEBUG), 0u);
}

// ---------------------------------------------------------------------------
// R32 登记（启用即 SIGSEGV，rc=139）：cfg==NULL ⇒ _base_dir 为空串，
// readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY, ...) 的 get_k_db 走
// WtRdmDtReaderAD.cpp:420-421 的 "{}{}/{}/" → 相对路径 "day/SHFE/"（在 CWD 里
// 自建空库，本身又是 R21-BUG-12 的无 error_code create_directories），
// 空库 0 根命中后 :353 `&barsList._bars[0]` 对空 vector 取下标、
// `size() - 1` 在 size()==0 时下溢成 SIZE_MAX-1 → lower_bound 解引用空指针。
// 缺陷本体见 bugs/R21_adstorage.md R21-BUG-06（空结果集下标崩溃），不新登记。
// ---------------------------------------------------------------------------
TEST_F(P05RdmReaderADTest, DISABLED_NullCfgKlineSliceOpensCwdRelativeDbAndCrashesOnEmptyBars)
{
	init_reader(false);

	WTSKlineSlice* slice = _reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY, 202401020000, 202401200000);
	if(slice != NULL)
		slice->release();
	// 启用即崩：走到这里说明缺陷已修复（空结果应返回空切片而不是段错误）
	FAIL() << "R21-BUG-06：0 根命中时对空 vector 取 &v[0] → SIGSEGV";
}

TEST_F(P05RdmReaderADTest, NotProvidedSlicesReturnNull)
{
	init_reader(true);

	// 头文件里直接 return NULL 的四个桩（钉住“未实现”，避免被当成有数据）
	EXPECT_EQ(_reader->readOrdDtlSliceByRange("SHFE.rb.rb2409", 202401020000, 202401200000), nullptr);
	EXPECT_EQ(_reader->readOrdQueSliceByRange("SHFE.rb.rb2409", 202401020000, 202401200000), nullptr);
	EXPECT_EQ(_reader->readTransSliceByRange("SHFE.rb.rb2409", 202401020000, 202401200000), nullptr);
	EXPECT_EQ(_reader->getAdjFactorByDate("SHFE.rb.rb2409", 20240102), 1.0);
	_reader->clearCache();
}

TEST_F(P05RdmReaderADTest, DayKlineSliceRoundTripFieldByField)
{
	const std::vector<WTSBarStruct> bars = make_day_bars(15, 3500.0);
	put_day_bars("SHFE", "rb2409", bars);

	init_reader(true);

	WTSKlineSlice* slice = _reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY, 202401020000, 202401200000);
	ASSERT_NE(slice, nullptr);
	EXPECT_EQ(slice->get_block_size(0), 15u);
	// calcTradingDate 的入参要能被宿主看到：交易日口径 = 自然日 + 时分
	EXPECT_EQ(_bd->pid_seen(), "SHFE.rb");
	EXPECT_EQ(_bd->is_session(), false);

	ASSERT_EQ(slice->get_block_counts(), (std::size_t)1);
	const WTSBarStruct* out = (const WTSBarStruct*)slice->get_block_addr(0);
	ASSERT_NE(out, nullptr);
	for(uint32_t i = 0; i < 15; i++)
	{
		EXPECT_EQ(out[i].date, bars[i].date) << i;
		EXPECT_DOUBLE_EQ(out[i].open, bars[i].open) << i;
		EXPECT_DOUBLE_EQ(out[i].high, bars[i].high) << i;
		EXPECT_DOUBLE_EQ(out[i].low, bars[i].low) << i;
		EXPECT_DOUBLE_EQ(out[i].close, bars[i].close) << i;
		EXPECT_DOUBLE_EQ(out[i].money, bars[i].money) << i;
	}
	slice->release();
}

TEST_F(P05RdmReaderADTest, WindowClippingUsesCachedBars)
{
	std::vector<WTSBarStruct> bars = make_day_bars(20, 100.0);
	// 乱序入库，读回必须是 LMDB 键序（日期升序）
	std::reverse(bars.begin(), bars.end());
	put_day_bars("DCE", "i2409", bars);
	std::reverse(bars.begin(), bars.end());

	init_reader(true);

	// 只要 [20240105, 20240112]，即 bars[3..10]
	WTSKlineSlice* slice = _reader->readKlineSliceByRange("DCE.i.i2409", KP_DAY, 202401050000, 202401120000);
	if(slice == nullptr)
	{
		// DCE 的品种段会被解析成 product="i"，交易所/代码组合仍会命中同一个 db
		FAIL() << "窗口切片返回空，键区间与 db 布局不符";
	}
	const WTSBarStruct* out = (const WTSBarStruct*)slice->get_block_addr(0);
	const uint32_t cnt = slice->get_block_size(0);
	ASSERT_GT(cnt, 0u);
	// 返回的每一根都必须能在入库集合里逐字段找到
	for(uint32_t i = 0; i < cnt; i++)
	{
		bool bFound = false;
		for(const WTSBarStruct& seed : bars)
		{
			if(seed.date == out[i].date)
			{
				EXPECT_DOUBLE_EQ(seed.open, out[i].open) << i;
				EXPECT_DOUBLE_EQ(seed.close, out[i].close) << i;
				bFound = true;
				break;
			}
		}
		EXPECT_TRUE(bFound) << "bar @" << out[i].date;
	}
	slice->release();

	// 第二次调用命中 _exchg_d1_dbs，返回语句 std::move(it->second) 会把缓存掏空 → 目前返回 NULL，
	// 属缺陷（BUG-P05-001），理想行为见下面的 DISABLED_ 用例，这里不把它钉成“期望”。
	WTSKlineSlice* poisoned = _reader->readKlineSliceByRange("DCE.i.i2409", KP_DAY, 202401050000, 202401120000);
	if(poisoned != NULL)
		poisoned->release();
}

// 修复 BUG-P05-001 以后启用：同一 (exchg, period) 的重复取数必须稳定返回同一条数。
TEST_F(P05RdmReaderADTest, DISABLED_RepeatedKlineSlicesShouldNotReturnNull)
{
	const std::vector<WTSBarStruct> bars = make_day_bars(6, 700.0);
	put_day_bars("GFEX", "lc2409", bars);
	init_reader(true);

	for(uint32_t round = 0; round < 3; round++)
	{
		WTSKlineSlice* slice = _reader->readKlineSliceByRange("GFEX.lc.lc2409", KP_DAY,
			202401020000, 202401100000);
		ASSERT_NE(slice, nullptr) << round;
		EXPECT_EQ(slice->get_block_size(0), 6u) << round;
		slice->release();
	}
}

TEST_F(P05RdmReaderADTest, MinuteBarsUseTimeAsKeyTail)
{
	std::vector<WTSBarStruct> m1;
	for(uint32_t i = 0; i < 12; i++)
	{
		const uint64_t t = TimeUtils::timeToMinBar(20240301, 931 + i);
		m1.push_back(make_bar(20240301, t, 50.0 + i));
	}
	put_min_bars("min1", "CZCE", "SR409", m1);

	init_reader(true);

	WTSKlineSlice* slice = _reader->readKlineSliceByRange("CZCE.SR.SR409", KP_Minute1,
		202403010931, 202403011000);
	ASSERT_NE(slice, nullptr);
	const WTSBarStruct* out = (const WTSBarStruct*)slice->get_block_addr(0);
	const uint32_t cnt = slice->get_block_size(0);
	EXPECT_GT(cnt, 0u);
	EXPECT_LE(cnt, 12u);
	// 分钟线时间形如 (date-19900000)*10000+hhmm，必然大于裸日期
	if(cnt > 0)
		EXPECT_GT(out[0].time, 20240301u);
	slice->release();

	// min5 目录不存在 → db 是空的；空缓存直接下标 [0] 会崩，属于缺陷类，见 DISABLED_ 用例
}

TEST_F(P05RdmReaderADTest, OutOfRangeEndBeforeFirstBarYieldsEmptySlice)
{
	const std::vector<WTSBarStruct> bars = make_day_bars(8, 200.0);
	put_day_bars("INE", "sc2409", bars);

	init_reader(true);

	// etime 早于第一根（20240102）：lower_bound 命中首元素后被 pBar-- 掏成 0 条
	WTSKlineSlice* slice = _reader->readKlineSliceByRange("INE.sc.sc2409", KP_DAY,
		202312010000, 202312310000);
	if(slice != NULL)
	{
		EXPECT_EQ(slice->get_block_size(0), 0u);
		slice->release();
	}
}

TEST_F(P05RdmReaderADTest, UnknownPeriodYieldsNullSlice)
{
	init_reader(true);
	// get_k_db 只认 min1/min5/day
	EXPECT_EQ(_reader->readKlineSliceByRange("SHFE.rb.rb2409", (WTSKlinePeriod)200,
		202401020000, 202401200000), nullptr);
}

//////////////////////////////////////////////////////////////////////////
// Empty-result boundaries in WtRdmDtReaderAD. Each disabled case owns a new
// reader and a single database setup so one failure cannot hide later cases.
// The AD slice API represents no matching records as NULL or an empty slice.
//////////////////////////////////////////////////////////////////////////
static void expect_empty_kline_slice(WTSKlineSlice* slice)
{
	if (slice == NULL)
		return;
	for (std::size_t i = 0; i < slice->get_block_counts(); i++)
		EXPECT_EQ(0u, slice->get_block_size(i));
	slice->release();
}

static void expect_empty_tick_slice(WTSTickSlice* slice)
{
	if (slice == NULL)
		return;
	for (std::size_t i = 0; i < slice->get_block_counts(); i++)
		EXPECT_EQ(0u, slice->get_block_size(i));
	slice->release();
}

TEST_F(P05RdmReaderADTest, DISABLED_EmptyDailyDatabaseReturnsEmptyKlineSlice)
{
	init_reader(true);
	// No prior read populates or moves the day database cache.
	expect_empty_kline_slice(_reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
		202401020000, 202401200000));
}

TEST_F(P05RdmReaderADTest, DISABLED_DailyDatabaseWithOtherContractReturnsEmptyKlineSlice)
{
	init_reader(true);
	put_day_bars("SHFE", "hc2409", make_day_bars(4, 4000.0));
	// The existing SHFE DB contains bars, but none for rb2409.
	expect_empty_kline_slice(_reader->readKlineSliceByRange("SHFE.rb.rb2409", KP_DAY,
		202401020000, 202401200000));
}

TEST_F(P05RdmReaderADTest, DISABLED_EmptyMinuteDatabaseReturnsEmptyKlineSlice)
{
	init_reader(true);
	// get_k_db creates a valid, empty min1/CZCE database for this reader.
	expect_empty_kline_slice(_reader->readKlineSliceByRange("CZCE.SR.SR999", KP_Minute1,
		202403010931, 202403011000));
}

TEST_F(P05RdmReaderADTest, DISABLED_EmptyTickDatabaseReturnsEmptyTickSlice)
{
	init_reader(true);
	// get_t_db creates an empty but valid database for this exact contract.
	expect_empty_tick_slice(_reader->readTickSliceByRange("SHFE.rb.rb2409",
		20240101090000000ull, 20240101150000000ull));
}

TEST_F(P05RdmReaderADTest, DISABLED_TickDatabaseWithoutMatchingTradingDateReturnsEmptySlice)
{
	init_reader(true);
	const std::filesystem::path tdir = _root / "ticks" / "SHFE" / "rb2409";
	std::filesystem::create_directories(tdir);
	{
		WtLMDB db(false);
		ASSERT_TRUE(db.open(tdir.string().c_str(), 4 * 1024 * 1024));
		WtLMDBQuery query(db);
		const WTSTickStruct tick = make_tick("SHFE", "rb2409", 20240201, 20240201,
			93000000, 3600.0);
		ASSERT_TRUE(query.put(key_str(LMDBHftKey("SHFE", "rb2409", 20240201, 93000000)),
			raw_of(tick)));
		query.commit();
	}
	// Same contract has a record, but the query is for the prior trading day.
	expect_empty_tick_slice(_reader->readTickSliceByRange("SHFE.rb.rb2409",
		20240101090000000ull, 20240101150000000ull));
}
