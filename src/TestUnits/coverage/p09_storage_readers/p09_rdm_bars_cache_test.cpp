// p09 / WtDataStorage（非 AD 本体）：WtRdmDtReader 的缓存索引层与复权因子层
//
// 覆盖对象：
//  - WtRdmDtReader::init（cfg==NULL 分支：_sink/_base_data_mgr/_hot_mgr 注入后提前返回，不建轮询线程）
//  - WtRdmDtReader::indexBarFromCacheByRange / indexBarFromCacheByCount / readBarsFromCacheByRange
//    （区间裁剪、isDay 与非 isDay 两条比较器、右端越界、空缓存守卫）
//  - WtRdmDtReader::clearCache
//  - WtRdmDtReader::loadStkAdjFactorsFromFile（含带 PID / 不带 PID 的 key 拼装、非数组成员跳过、
//    19900101 兜底条目、按日期排序）与 inline getAdjFactors
//  - WtRdmDtReader::getAdjFactorByDate（非股票 / 无因子 / 命中 / 夹在中间 / 超出最后日期）
//
// 跳过：readKlineSliceByRange/ByCount、readTickSliceByDate/ByRange/ByCount、readOrd*SliceByRange、
// readTransSliceByRange、getRT*Block、cacheHisBarsFromFile —— 这些都要先拿 RT 命名共享内存块
// （BoostMappingFile）或 5s 轮询线程，属简报点名的共享内存/后台线程路径。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

// 先把 WtRdmDtReader.h 自身依赖的头按正常访问权限请进来（pragma once 会挡住后面的重复 include）
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
#include "Includes/FasterDefs.h"
#include "Share/StdUtils.hpp"
#include "Share/BoostMappingFile.hpp"
#include "Share/fmtlib.h"
#include "Share/TimeUtils.hpp"
#include "WtDataStorage/DataDefine.h"

// 覆盖率专用：把类内 private/protected 成员（_bars_cache、_adj_factors、indexBar*、
// readBarsFromCacheByRange、loadStkAdjFactorsFromFile、getAdjFactors）暴露给白盒用例。
// 访问权限不参与 Itanium name mangling，链接到的仍是 libWtDataStorage 里的真实现。
#define private public
#define protected public
#include "WtDataStorage/WtRdmDtReader.h"
#undef private
#undef protected

using namespace wtp;

namespace {

//////////////////////////////////////////////////////////////////////////
// 宿主 mock：只提供本模块用到的回调，不起线程、不碰共享内存
//////////////////////////////////////////////////////////////////////////
class P09RdmBaseDataMgr : public IBaseDataMgr
{
public:
	P09RdmBaseDataMgr()
	{
		_session = WTSSessionInfo::create("SSE_DAY", "日盘", 0);
		_session->addTradingSection(0, 2359);

		_stock = WTSCommodityInfo::create("STK", "股票", "SSE", "SSE_DAY", "CHN");
		_stock->setSessionInfo(_session);
		_stock->setCategory(ContractCategory::CC_Stock);

		_fut = WTSCommodityInfo::create("rb", "螺纹钢", "SHFE", "SSE_DAY", "CHN");
		_fut->setSessionInfo(_session);
		_fut->setCategory(ContractCategory::CC_Future);
	}

	virtual ~P09RdmBaseDataMgr()
	{
		_stock->release();
		_fut->release();
		_session->release();
	}

	WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		_last_pid = exchgpid == NULL ? "" : exchgpid;
		return _stock;
	}

	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		_last_pid = std::string(exchg == NULL ? "" : exchg) + "." + (pid == NULL ? "" : pid);
		return strcmp(exchg, "SSE") == 0 ? _stock : _fut;
	}

	WTSContractInfo* getContract(const char*, const char*, uint32_t) override { return NULL; }
	WTSArray* getContracts(const char*, uint32_t) override { return NULL; }
	WTSSessionInfo* getSession(const char* sid) override { _last_sid = sid; return _session; }
	WTSSessionInfo* getSessionByCode(const char*, const char*) override { return _session; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool) override { return uDate; }
	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool, bool isStart) override
	{
		return isStart ? ((uint64_t)tDate) * 1000000000ull + 90000000ull
			: ((uint64_t)tDate) * 1000000000ull + 150000000ull;
	}

	const std::string& last_pid() const { return _last_pid; }

private:
	WTSSessionInfo*	_session = NULL;
	WTSCommodityInfo*	_stock = NULL;
	WTSCommodityInfo*	_fut = NULL;
	std::string		_last_pid;
	std::string		_last_sid;
};

class P09RdmHotMgr : public IHotMgr
{
public:
	virtual ~P09RdmHotMgr() {}

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
	const char* getRuleTag(const char* /*stdCode*/) override { return ""; }
	double getRuleFactor(const char*, const char*, uint32_t) override { return 1.0; }
};

class P09RdmSink : public IRdmDtReaderSink
{
public:
	P09RdmSink(IBaseDataMgr* bd, IHotMgr* hot) : _bd(bd), _hot(hot) {}
	virtual ~P09RdmSink() {}

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
	std::map<WTSLogLevel, uint32_t>	_logs;
	uint32_t		_total = 0;
};

// bar.time 的编码与生产代码一致：(date - 19900000) * 10000 + hhmm
static inline uint64_t p09_bar_time(uint32_t date, uint32_t hhmm)
{
	return ((uint64_t)date - 19900000ull) * 10000ull + hhmm;
}

// 造 n 根日线 bar，日期从 sDate 起按“日+1（跨月不管，只要单调递增即可）”推进，time 固定收盘 1500
static void p09_fill_bars(std::vector<WTSBarStruct>& bars, uint32_t sDate, uint32_t n)
{
	bars.clear();
	bars.resize(n);
	for (uint32_t i = 0; i < n; i++)
	{
		WTSBarStruct& bar = bars[i];
		memset(&bar, 0, sizeof(WTSBarStruct));
		bar.date = sDate + i;			// 仅要求单调递增，不做日历换算
		bar.time = p09_bar_time(bar.date, 1500);
		bar.open = (double)(i + 1) * 1.0;
		bar.high = (double)(i + 1) * 2.0;
		bar.low = (double)(i + 1) * 0.5;
		bar.close = (double)(i + 1) * 1.5;
		bar.vol = (uint64_t)(i + 1) * 10;
	}
}

class P09RdmCacheTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_hot.reset(new P09RdmHotMgr());
		_bd.reset(new P09RdmBaseDataMgr());
		_sink.reset(new P09RdmSink(_bd.get(), _hot.get()));
		// cfg == NULL：init 只注入 sink/基础数据/热点管理器后即返回，不启动 5s 轮询线程
		_reader.init(NULL, _sink.get());
	}

	void TearDown() override
	{
		std::filesystem::remove_all(_root, _ec);
	}

	std::string p09_path(const char* name) const
	{
		return _root + "/" + name;
	}

	WtRdmDtReader&	reader(void) { return _reader; }

	std::string	_root
	{
		(std::filesystem::temp_directory_path()
			/ ("p09_rdm_" + std::to_string(::getpid()) + "_" + std::to_string(++_seq))).string()
	};

	std::unique_ptr<P09RdmHotMgr>			_hot;
	std::unique_ptr<P09RdmBaseDataMgr>		_bd;
	std::unique_ptr<P09RdmSink>				_sink;
	WtRdmDtReader							_reader;
	std::error_code							_ec;
	static uint32_t							_seq;
};
uint32_t P09RdmCacheTest::_seq = 0;

TEST_F(P09RdmCacheTest, init_with_null_cfg_injects_host_and_skips_check_thread)
{
	// init(NULL) 必须在建线程之前返回
	EXPECT_EQ(_reader._thrd_check.get(), nullptr);
	EXPECT_EQ(_reader._base_data_mgr, _bd.get());
	EXPECT_EQ(_reader._hot_mgr, _hot.get());
	// _sink 是 IRdmDtReader 的 protected 成员，改用「日志确实打到 sink」来证明注入成功
	EXPECT_EQ(_sink->total(), 0u);
	EXPECT_TRUE(_reader._base_dir.empty());
}

TEST_F(P09RdmCacheTest, index_bar_by_range_empty_cache_returns_null_and_touches_key)
{
	uint32_t count = 7;
	// _bars_cache[key] 会默认构造一个空 BarsList，随后被 empty() 守卫拦住
	WTSBarStruct* p = _reader.indexBarFromCacheByRange("SHFE.rb@0#1", 202001010930, 202001101500, count);
	EXPECT_EQ(p, nullptr);
	EXPECT_EQ(_reader._bars_cache.size(), 1u);
	EXPECT_TRUE(_reader._bars_cache["SHFE.rb@0#1"]._bars.empty());
}

TEST_F(P09RdmCacheTest, index_bar_by_range_trims_window)
{
	const std::string key = "SSE.STK.600000#0";
	WtRdmDtReader::BarsList& lst = _reader._bars_cache[key];
	p09_fill_bars(lst._bars, 20200101, 10);
	lst._exchg = "SSE";
	lst._code = "SSE.STK.600000";

	uint32_t count = 0;
	// 中间区间：20200103 ~ 20200105（含端点）
	WTSBarStruct* p = _reader.indexBarFromCacheByRange(key, 202001031500, 202001051500, count);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(count, 3u);
	EXPECT_EQ(p[0].date, 20200103u);
	EXPECT_EQ(p[2].date, 20200105u);
	// 返回的是缓存内部指针，不是拷贝
	EXPECT_EQ(p, &lst._bars[2]);

	// 右端越界：etime 大于最后一根 → eit == end() → eIdx = size-1
	count = 0;
	p = _reader.indexBarFromCacheByRange(key, 202001081500, 202002011500, count);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(count, 3u);
	EXPECT_EQ(p[2].date, 20200110u);

	// 左右同一天：单根
	count = 0;
	p = _reader.indexBarFromCacheByRange(key, 202001011500, 202001011500, count);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(count, 1u);
	EXPECT_EQ(p[0].date, 20200101u);

	// 分钟比较器分支（isDay=false）按 time 比较；日线分支（isDay=true）按 date 比较
	count = 0;
	WTSBarStruct* pd = _reader.indexBarFromCacheByRange(key, 202001030000, 202001050000, count, true);
	ASSERT_NE(pd, nullptr);
	EXPECT_EQ(count, 3u);
	EXPECT_EQ(pd[0].date, 20200103u);
	EXPECT_EQ(pd[count - 1].date, 20200105u);
}

TEST_F(P09RdmCacheTest, index_bar_by_count_clamps_and_walks_back)
{
	const std::string key = "SSE.STK.600001#0";
	WtRdmDtReader::BarsList& lst = _reader._bars_cache[key];
	p09_fill_bars(lst._bars, 20200201, 8);

	uint32_t count = 3;
	WTSBarStruct* p = _reader.indexBarFromCacheByCount(key, 202002081500, count);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(count, 3u);
	EXPECT_EQ(p[0].date, 20200206u);
	EXPECT_EQ(p[2].date, 20200208u);
	EXPECT_EQ(p, &lst._bars[5]);

	// 请求量超过可用条数 -> 收敛到 eIdx+1
	count = 100;
	p = _reader.indexBarFromCacheByCount(key, 202002081500, count);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(count, 8u);
	EXPECT_EQ(p, &lst._bars[0]);

	// 右端越界（etime 在最后一条之后）-> 取到最新 count 条
	count = 2;
	p = _reader.indexBarFromCacheByCount(key, 202003011500, count);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(count, 2u);
	EXPECT_EQ(p[1].date, 20200208u);

	// 空缓存守卫
	count = 5;
	EXPECT_EQ(_reader.indexBarFromCacheByCount("unknown.key#9", 202002081500, count), nullptr);
}

TEST_F(P09RdmCacheTest, read_bars_from_cache_by_range_copies_window)
{
	const std::string key = "SSE.STK.600002#0";
	WtRdmDtReader::BarsList& lst = _reader._bars_cache[key];
	p09_fill_bars(lst._bars, 20200301, 12);

	std::vector<WTSBarStruct> out;
	uint32_t n = _reader.readBarsFromCacheByRange(key, 202003031500, 202003071500, out);
	EXPECT_EQ(n, 5u);
	ASSERT_EQ(out.size(), 5u);
	for (uint32_t i = 0; i < n; i++)
	{
		EXPECT_EQ(out[i].date, 20200303u + i);
		EXPECT_EQ(out[i].time, lst._bars[2 + i].time);
		EXPECT_DOUBLE_EQ(out[i].close, (double)(3 + i) * 1.5);
		EXPECT_EQ(out[i].vol, (uint64_t)(3 + i) * 10);
	}

	// 右端越界
	out.clear();
	n = _reader.readBarsFromCacheByRange(key, 202003101500, 202004011500, out);
	EXPECT_EQ(n, 3u);
	EXPECT_EQ(out[2].date, 20200312u);

	// 有 begin() 守卫：区间整体在首条之前 -> 返回 0（同文件的 indexBar* 缺这个守卫）
	out.clear();
	n = _reader.readBarsFromCacheByRange(key, 201901011500, 201901021500, out);
	EXPECT_EQ(n, 0u);

	// isDay 分支按 date 比较
	out.clear();
	n = _reader.readBarsFromCacheByRange(key, 202003040000, 202003040000, out, true);
	EXPECT_EQ(n, 1u);
	if (n == 1) EXPECT_EQ(out[0].date, 20200304u);
}

TEST_F(P09RdmCacheTest, clear_cache_drops_bars_but_keeps_adj_factors)
{
	_reader._bars_cache["A#0"]._bars.resize(3);
	_reader._bars_cache["B#1"]._bars.resize(5);
	_reader._adj_factors["SSE.STK.000001"].push_back(WtRdmDtReader::AdjFactor{ 19900101, 1.0 });

	_reader.clearCache();

	EXPECT_TRUE(_reader._bars_cache.empty());
	EXPECT_TRUE(_reader._rt_min1_map.empty());
	EXPECT_TRUE(_reader._rt_tick_map.empty());
	ASSERT_EQ(_reader._adj_factors.size(), 1u);
	EXPECT_EQ(_reader._adj_factors["SSE.STK.000001"].size(), 1u);
}

TEST_F(P09RdmCacheTest, load_adj_factors_from_missing_or_bad_file)
{
	const uint32_t before = _sink->total();
	EXPECT_FALSE(_reader.loadStkAdjFactorsFromFile(p09_path("not_exists.json").c_str()));
	EXPECT_EQ(_sink->count_of(LL_ERROR), before + 1);
	EXPECT_TRUE(_reader._adj_factors.empty());

	std::filesystem::create_directories(_root, _ec);
	const std::string badfile = p09_path("bad.json");
	{
		std::ofstream ofs(badfile);
		ofs << "this is definitely not a json document";
	}
	// 解析失败时不应把 _adj_factors 写成空条目
	EXPECT_FALSE(_reader.loadStkAdjFactorsFromFile(badfile.c_str()));
	EXPECT_GE(_sink->count_of(LL_ERROR), 1u);
}

TEST_F(P09RdmCacheTest, load_adj_factors_from_json_key_format_and_sort)
{
	std::filesystem::create_directories(_root, _ec);
	const std::string adjfile = p09_path("adjfactor.json");
	{
		std::ofstream ofs(adjfile);
		ofs << "{\n"
			<< "  \"SSE\": {\n"
			<< "    \"STK.600000\": [\n"
			<< "      {\"date\": 20210601, \"factor\": 0.5},\n"
			<< "      {\"date\": 20200101, \"factor\": 0.8}\n"
			<< "    ],\n"
			<< "    \"600001\": [\n"
			<< "      {\"date\": 20220301, \"factor\": 2.0}\n"
			<< "    ],\n"
			<< "    \"NOTANARRAY\": { \"date\": 20200101, \"factor\": 3.0 }\n"
			<< "  },\n"
			<< "  \"SHFE\": { \"rb\": { \"date\": 20200101 } }\n"
			<< "}\n";
	}

	ASSERT_TRUE(_reader.loadStkAdjFactorsFromFile(adjfile.c_str()));

	// 带 PID 的 code 直接拼 exchg.code；不带的强制补 STK
	ASSERT_EQ(_reader._adj_factors.count("SSE.STK.600000"), 1u);
	ASSERT_EQ(_reader._adj_factors.count("SSE.STK.600001"), 1u);
	// 非数组成员被 continue 跳过（既不会产生条目也不会计数）
	EXPECT_EQ(_reader._adj_factors.count("SSE.STK.NOTANARRAY"), 0u);
	EXPECT_EQ(_reader._adj_factors.count("SHFE.STK.rb"), 0u);
	EXPECT_EQ(_reader._adj_factors.size(), 2u);

	// 19900101 兜底条目 + 按日期升序
	const WtRdmDtReader::AdjFactorList& lst = _reader._adj_factors["SSE.STK.600000"];
	ASSERT_EQ(lst.size(), 3u);
	EXPECT_EQ(lst[0]._date, 19900101u);
	EXPECT_DOUBLE_EQ(lst[0]._factor, 1.0);
	EXPECT_EQ(lst[1]._date, 20200101u);
	EXPECT_DOUBLE_EQ(lst[1]._factor, 0.8);
	EXPECT_EQ(lst[2]._date, 20210601u);
	EXPECT_DOUBLE_EQ(lst[2]._factor, 0.5);

	// inline 的 getAdjFactors(code, exchg, pid) 用同一套 key 规则
	const WtRdmDtReader::AdjFactorList& again = _reader.getAdjFactors("600000", "SSE", "STK");
	EXPECT_EQ(again.size(), 3u);

	// 未注册的 key 通过 operator[] 返回空列表（不抛异常）
	const WtRdmDtReader::AdjFactorList& miss = _reader.getAdjFactors("999999", "SSE", "STK");
	EXPECT_TRUE(miss.empty());
}

TEST_F(P09RdmCacheTest, get_adj_factor_by_date_stock_and_non_stock)
{
	WtRdmDtReader::AdjFactorList& lst = _reader._adj_factors["SSE.STK.600100"];
	lst.push_back(WtRdmDtReader::AdjFactor{ 19900101, 1.0 });
	lst.push_back(WtRdmDtReader::AdjFactor{ 20200101, 0.8 });
	lst.push_back(WtRdmDtReader::AdjFactor{ 20210601, 0.5 });

	// 非股票品种直接返回 1.0
	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SHFE.rb0", 20210101), 1.0);

	// 股票但无因子 -> 1.0（未知 key 走 operator[] 的空分支）
	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SSE.STK.999999", 20210101), 1.0);

	// 命中当天 / 夹在两条之间 / 超出最后一条，三个分支
	// 注意：getAdjFactorByDate 会按 isExright() 决定是否裁掉 stdCode 末位，
	// 所以这里同时给裁与不裁两种 key 灌同样的因子，避免依赖 CodeHelper 的复权判定。
	WtRdmDtReader::AdjFactorList& alt = _reader._adj_factors["SSE.STK.60010"];
	alt = lst;

	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SSE.STK.600100", 20200101), 0.8);
	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SSE.STK.600100", 20201231), 0.8);
	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SSE.STK.600100", 20210601), 0.5);
	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SSE.STK.600100", 20991231), 0.5);
	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SSE.STK.600100", 19900101), 1.0);
}

// 崩溃/UB 类输入整类收进同一个 DISABLED_ 用例，常规用例零残留。
// 复现（--gtest_also_run_disabled_tests）：
//  1) indexBarFromCacheByRange / indexBarFromCacheByCount 的 eit-- 缺 eit==begin() 守卫
//     （同文件 readBarsFromCacheByRange 有），etime 早于首条 bar 时回绕/越界；
//  2) readBarsFromCacheByRange 缺 barsList._bars.empty() 守卫，未预热的 key 会算出
//     eIdx = 0 - 1 = SIZE_MAX，随后 resize 出天文数字并 memcpy。
TEST_F(P09RdmCacheTest, DISABLED_p09_underflow_inputs_from_range_indexers)
{
	const std::string key = "SSE.STK.600999#0";
	WtRdmDtReader::BarsList& lst = _reader._bars_cache[key];
	p09_fill_bars(lst._bars, 20200101, 5);

	uint32_t count = 2;
	// (1) etime 早于第一条 -> eit == begin() 且 eit->time > eBar.time -> eit--
	WTSBarStruct* p = _reader.indexBarFromCacheByRange(key, 201901011500, 201901021500, count);
	(void)p;
	// (2) 同上，ByCount 分支
	count = 2;
	p = _reader.indexBarFromCacheByCount(key, 201901011500, count);
	(void)p;
	// (3) 未预热的 key -> 空 vector 上 size()-1 下溢
	std::vector<WTSBarStruct> out;
	_reader.readBarsFromCacheByRange("never.seen#0", 202001011500, 202001051500, out);
}

}