// p09 / WtDataStorage（非 AD 本体）：WtRdmDtReader 缓存区间索引的窗口语义
//
// 本文件只碰 _bars_cache 的纯计算层（无共享内存、无线程、无落盘），把「日期区间与窗口裁剪 /
// 跨块拼接 / 增量与全量 / 切片访问器边界」这些分支一次跑清楚：
//  - indexBarFromCacheByRange：左闭右闭、右端越界（eit==end）、isDay 比较器；左端整体越界为 BUG-P09-005 禁用候选
//  - readBarsFromCacheByRange：同一区间的拷贝版语义（含空区间不 resize）
//  - indexBarFromCacheByCount：count 收敛 / count==1 / count 超过可用条数；count==0 保留为 BUG-P09-005 禁用候选
//  - BarsList::_rt_bars 不参与索引（只有 _bars 被扫）
//
// 时间编码沿用生产代码的 bar.time = (date - 19900000) * 10000 + hhmm（uint32，2033 年起溢出，
// 已由 BUG-P05-004 记录），所以本文件所有日期都取 2020~2021，避免把断言建在回绕值上。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

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

// 覆盖率专用白盒开关（访问权限不参与 Itanium name mangling）
#define private public
#define protected public
#include "WtDataStorage/WtRdmDtReader.h"
#undef private
#undef protected

using namespace wtp;

namespace {

class P09WBaseDataMgr : public IBaseDataMgr
{
public:
	P09WBaseDataMgr()
	{
		_session = WTSSessionInfo::create("SSE_DAY", "日盘", 0);
		_session->addTradingSection(0, 2359);
		_comm = WTSCommodityInfo::create("STK", "股票", "SSE", "SSE_DAY", "CHN");
		_comm->setSessionInfo(_session);
		_comm->setCategory(ContractCategory::CC_Stock);
	}

	virtual ~P09WBaseDataMgr()
	{
		_comm->release();
		_session->release();
	}

	WTSCommodityInfo* getCommodity(const char*) override { return _comm; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return _comm; }
	WTSContractInfo* getContract(const char*, const char*, uint32_t) override { return NULL; }
	WTSArray* getContracts(const char*, uint32_t) override { return NULL; }
	WTSSessionInfo* getSession(const char*) override { return _session; }
	WTSSessionInfo* getSessionByCode(const char*, const char*) override { return _session; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool) override { return uDate; }
	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool, bool isStart) override
	{
		return isStart ? ((uint64_t)tDate) * 1000000000ull + 90000000ull
			: ((uint64_t)tDate) * 1000000000ull + 150000000ull;
	}

private:
	WTSSessionInfo*	_session = NULL;
	WTSCommodityInfo*	_comm = NULL;
};

class P09WHotMgr : public IHotMgr
{
public:
	virtual ~P09WHotMgr() {}

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
	const char* getRuleTag(const char*) override { return ""; }
	double getRuleFactor(const char*, const char*, uint32_t) override { return 1.0; }
};

class P09WSink : public IRdmDtReaderSink
{
public:
	P09WSink(IBaseDataMgr* bd, IHotMgr* hot) : _bd(bd), _hot(hot) {}
	virtual ~P09WSink() {}

	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return _hot; }
	void reader_log(WTSLogLevel ll, const char*) override { _logs[ll]++; _total++; }
	uint32_t total() const { return _total; }

private:
	IBaseDataMgr*	_bd;
	IHotMgr*		_hot;
	std::map<WTSLogLevel, uint32_t>	_logs;
	uint32_t		_total = 0;
};

static inline uint64_t bar_time_of(uint32_t date, uint32_t hhmm)
{
	return ((uint64_t)date - 19900000ull) * 10000ull + hhmm;
}

// 按 (date, hhmm) 造一根 bar；open 用作“这条来自哪一段”的指纹
static WTSBarStruct make_bar(uint32_t date, uint32_t hhmm, double tag)
{
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(WTSBarStruct));
	bar.date = date;
	bar.time = bar_time_of(date, hhmm);
	bar.open = tag;
	bar.high = tag + 1;
	bar.low = tag - 1;
	bar.close = tag + 0.5;
	bar.vol = (uint64_t)(tag * 100);
	return bar;
}

class P09WindowTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		// cfg==NULL：注入宿主后即返回，不启动 5s 轮询线程
		_reader.init(NULL, _sink.get());
	}

	std::string fresh_key(const char* tag)
	{
		return std::string(tag) + "#" + std::to_string(++_seq) + "#0";
	}

	P09WindowTest()
		: _hot(new P09WHotMgr()), _bd(new P09WBaseDataMgr()), _sink(new P09WSink(_bd.get(), _hot.get()))
	{
	}

	std::unique_ptr<P09WHotMgr>			_hot;
	std::unique_ptr<P09WBaseDataMgr>		_bd;
	std::unique_ptr<P09WSink>				_sink;
	WtRdmDtReader							_reader;
	uint32_t								_seq = 0;
};

TEST_F(P09WindowTest, window_bounds_are_inclusive_on_both_sides)
{
	const std::string key = fresh_key("SSE.STK.000001");
	WtRdmDtReader::BarsList& lst = _reader._bars_cache[key];
	for (uint32_t d = 20200401; d <= 20200406; d++)
		lst._bars.push_back(make_bar(d, 1500, (double)(d - 20200400)));

	uint32_t count = 0;
	// 左端 == 首条、右端 == 末条
	WTSBarStruct* p = _reader.indexBarFromCacheByRange(key, 202004011500, 202004061500, count);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(count, 6u);
	EXPECT_DOUBLE_EQ(p[0].open, 1.0);
	EXPECT_DOUBLE_EQ(p[5].open, 6.0);

	// 单点区间（左闭右闭，同一条命中两次）
	count = 0;
	p = _reader.indexBarFromCacheByRange(key, 202004031500, 202004031500, count);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(count, 1u);
	EXPECT_DOUBLE_EQ(p[0].open, 3.0);

	// 左端落在两根 bar 之间的“空隙”（1500 之后、次日 1500 之前）也仍按 time 比较取下一条
	count = 0;
	p = _reader.indexBarFromCacheByRange(key, 202004031530, 202004051500, count);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(count, 2u);
	EXPECT_DOUBLE_EQ(p[0].open, 4.0);

	// 拷贝版给出同一区间
	std::vector<WTSBarStruct> out;
	uint32_t n = _reader.readBarsFromCacheByRange(key, 202004031530, 202004051500, out);
	EXPECT_EQ(n, 2u);
	ASSERT_EQ(out.size(), 2u);
	EXPECT_DOUBLE_EQ(out[0].open, 4.0);
	EXPECT_DOUBLE_EQ(out[1].open, 5.0);
}

TEST_F(P09WindowTest, range_right_edge_beyond_last_bar_clamps_to_latest)
{
	const std::string key = fresh_key("SSE.STK.000002");
	WtRdmDtReader::BarsList& lst = _reader._bars_cache[key];
	for (uint32_t d = 20200501; d <= 20200504; d++)
		lst._bars.push_back(make_bar(d, 1500, (double)(d - 20200500)));

	// 右端越界：eit == end() -> eIdx = size-1（截到最新一条）
	uint32_t count = 0;
	WTSBarStruct* p = _reader.indexBarFromCacheByRange(key, 202005031500, 202012311500, count);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(count, 2u);
	EXPECT_DOUBLE_EQ(p[count - 1].open, 4.0);

	// 复制版对空区间安全；指针版对应的 BUG-P09-005 候选见下方禁用测试。
	std::vector<WTSBarStruct> out;
	out.reserve(8);
	uint32_t n = _reader.readBarsFromCacheByRange(key, 202012311500, 202101011500, out);
	EXPECT_EQ(n, 0u);
	EXPECT_TRUE(out.empty());

	// stime == 0：uint32 回绕后的 eBar.time 远小于 2020 年的 bar -> sIdx 归零，等于全量起点
	count = 0;
	p = _reader.indexBarFromCacheByRange(key, 0, 202005041500, count);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(count, 4u);
	EXPECT_EQ(p, &lst._bars[0]);
}

// BUG-P09-005 regression candidate: the active right-edge test above is a non-empty positive control.
// The current helper derives count==0 for a left edge after all cached bars, then still indexes _bars[size()].
// Intended post-fix contract: count==0 and p==nullptr. The helper's declaration does not currently promise
// nullptr for empty results, so keep this disabled until the contract is decided and the one-past access is fixed.
TEST_F(P09WindowTest, DISABLED_range_left_edge_beyond_all_returns_null_for_empty_result)
{
	const std::string key = fresh_key("SSE.STK.000002_EMPTY_RANGE");
	WtRdmDtReader::BarsList& lst = _reader._bars_cache[key];
	for (uint32_t d = 20200501; d <= 20200504; d++)
		lst._bars.push_back(make_bar(d, 1500, (double)(d - 20200500)));

	uint32_t count = 9;
	WTSBarStruct* p = _reader.indexBarFromCacheByRange(key, 202012311500, 202101011500, count);
	EXPECT_EQ(count, 0u);
	EXPECT_EQ(p, nullptr);
}

TEST_F(P09WindowTest, seam_between_two_sections_is_returned_whole_without_dedup)
{
	// 模拟「跨文件/跨块拼接」：前一段末尾与后一段开头有同一天的重复 bar
	const std::string key = fresh_key("SSE.STK.000003");
	WtRdmDtReader::BarsList& lst = _reader._bars_cache[key];
	std::vector<WTSBarStruct> segA;
	std::vector<WTSBarStruct> segB;
	for (uint32_t d = 20200601; d <= 20200604; d++) segA.push_back(make_bar(d, 1500, 10.0 + (d - 20200600)));
	for (uint32_t d = 20200604; d <= 20200606; d++) segB.push_back(make_bar(d, 1500, 20.0 + (d - 20200600)));
	lst._bars.insert(lst._bars.end(), segA.begin(), segA.end());
	lst._bars.insert(lst._bars.end(), segB.begin(), segB.end());
	ASSERT_EQ(lst._bars.size(), 7u);

	std::vector<WTSBarStruct> out;
	uint32_t n = _reader.readBarsFromCacheByRange(key, 202006031500, 202006051500, out);
	// 拼接处的重复 bar 原样返回，索引层不去重（去重是上层 replayer 的责任）
	EXPECT_EQ(n, 4u);
	ASSERT_EQ(out.size(), 4u);
	EXPECT_DOUBLE_EQ(out[0].open, 13.0);			// 0603 (segA)
	EXPECT_DOUBLE_EQ(out[1].open, 14.0);			// 0604 (segA)
	EXPECT_DOUBLE_EQ(out[2].open, 24.0);			// 0604 (segB) 重复
	EXPECT_DOUBLE_EQ(out[3].open, 25.0);			// 0605 (segB)

	// 增量读取：从上一轮的末条时间继续，会再次拿到那条边界 bar（左闭）
	out.clear();
	n = _reader.readBarsFromCacheByRange(key, 202006051500, 202006061500, out);
	EXPECT_EQ(n, 2u);
	ASSERT_EQ(out.size(), 2u);
	EXPECT_DOUBLE_EQ(out[0].open, 25.0);
	EXPECT_DOUBLE_EQ(out[1].open, 26.0);
}

TEST_F(P09WindowTest, isday_comparator_collapses_same_day_minute_bars)
{
	const std::string key = fresh_key("SSE.STK.000004");
	WtRdmDtReader::BarsList& lst = _reader._bars_cache[key];
	for (uint32_t t = 931; t <= 934; t++)
		lst._bars.push_back(make_bar(20200701, t, (double)t));

	// 非 isDay：按 time 逐分钟区分
	uint32_t count = 0;
	WTSBarStruct* p = _reader.indexBarFromCacheByRange(key, 202007010931, 202007010933, count);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(count, 3u);
	EXPECT_DOUBLE_EQ(p[0].open, 931.0);
	EXPECT_DOUBLE_EQ(p[2].open, 933.0);

	// isDay：比较器只看 date，全部同日 -> 右端 lower_bound 命中首条，区间塌成 1 条
	std::vector<WTSBarStruct> out;
	uint32_t n = _reader.readBarsFromCacheByRange(key, 202007010931, 202007010933, out, true);
	EXPECT_EQ(n, 1u);
	if (n == 1) EXPECT_DOUBLE_EQ(out[0].open, 931.0);

	// 同一天在 isDay 下整段可取（右端日期严格大于 bar 日期时才不塌）
	out.clear();
	n = _reader.readBarsFromCacheByRange(key, 202007010931, 202007020000, out, true);
	EXPECT_EQ(n, 4u);
	EXPECT_DOUBLE_EQ(out[3].open, 934.0);
}

TEST_F(P09WindowTest, rt_bars_are_never_scanned_by_the_indexers)
{
	const std::string key = fresh_key("SSE.STK.000005");
	WtRdmDtReader::BarsList& lst = _reader._bars_cache[key];
	for (uint32_t d = 20200801; d <= 20200803; d++) lst._bars.push_back(make_bar(d, 1500, (double)d));
	// _rt_bars 里放更“新”的数据，索引层不应看见它们
	for (uint32_t d = 20200901; d <= 20200905; d++) lst._rt_bars.push_back(make_bar(d, 1500, (double)d));

	uint32_t count = 100;
	WTSBarStruct* p = _reader.indexBarFromCacheByCount(key, 202012311500, count);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(count, 3u);					// 只有 _bars 的 3 条
	EXPECT_EQ(p, &lst._bars[0]);

	std::vector<WTSBarStruct> out;
	uint32_t n = _reader.readBarsFromCacheByRange(key, 202008011500, 202012311500, out);
	EXPECT_EQ(n, 3u);
	EXPECT_EQ(lst._rt_bars.size(), 5u);		// rt 缓存保持原样，未被消费
}

TEST_F(P09WindowTest, index_bar_by_count_walks_back_from_right_edge)
{
	const std::string key = fresh_key("SSE.STK.000006");
	WtRdmDtReader::BarsList& lst = _reader._bars_cache[key];
	for (uint32_t d = 20201001; d <= 20201010; d++)
		lst._bars.push_back(make_bar(d, 1500, (double)(d - 20201000)));

	uint32_t count = 1;
	WTSBarStruct* p = _reader.indexBarFromCacheByCount(key, 202010101500, count);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(count, 1u);
	EXPECT_EQ(p, &lst._bars[9]);

	count = 4;
	p = _reader.indexBarFromCacheByCount(key, 202010061500, count);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(count, 4u);
	EXPECT_EQ(p, &lst._bars[2]);
	EXPECT_DOUBLE_EQ(p[3].open, 6.0);

	// 请求量大于可用条数 -> 收敛到 eIdx+1，起点回到 0
	count = 50;
	p = _reader.indexBarFromCacheByCount(key, 202010101500, count);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(count, 10u);
	EXPECT_EQ(p, &lst._bars[0]);

}

// BUG-P09-005 regression candidate: the active index_bar_by_count_walks_back_from_right_edge test
// keeps non-empty controls; count==0 computes sIdx==_bars.size(), then still indexes _bars[sIdx].
// Intended post-fix contract: count==0 and p==nullptr. The helper declaration does not currently promise
// nullptr for empty results; re-enable only after deciding that contract and fixing the one-past access.
TEST_F(P09WindowTest, DISABLED_index_bar_by_count_zero_returns_null_for_empty_result)
{
	const std::string key = fresh_key("SSE.STK.000006_ZERO_COUNT");
	WtRdmDtReader::BarsList& lst = _reader._bars_cache[key];
	for (uint32_t d = 20201001; d <= 20201010; d++)
		lst._bars.push_back(make_bar(d, 1500, (double)(d - 20201000)));

	uint32_t count = 0;
	WTSBarStruct* p = _reader.indexBarFromCacheByCount(key, 202010101500, count);
	EXPECT_EQ(count, 0u);
	EXPECT_EQ(p, nullptr);
}

// 崩溃/UB 类输入整类收进同一个 DISABLED_ 用例（常规用例零残留）。
// 复现（--gtest_also_run_disabled_tests）：
//  IRdmDtReader 的公开签名是 readKlineSliceByRange(code, period, stime, etime = 0)，
//  etime=0 会被算成 eBar.time = (uint32)(0 - 19900000) * 10000 = 2863462912，
//  它小于任何 2013 年以后的 bar.time，于是 lower_bound 命中 begin()，
//  而 indexBarFromCacheByRange / indexBarFromCacheByCount 的 eit-- 没有 eit==begin() 守卫
//  （同文件 readBarsFromCacheByRange 有，直接 return 0），随即回绕越界。
TEST_F(P09WindowTest, DISABLED_p09_default_etime_zero_underflows_the_range_indexers)
{
	const std::string key = fresh_key("SSE.STK.000007");
	WtRdmDtReader::BarsList& lst = _reader._bars_cache[key];
	for (uint32_t d = 20201101; d <= 20201105; d++)
		lst._bars.push_back(make_bar(d, 1500, (double)(d - 20201100)));

	uint32_t count = 5;
	WTSBarStruct* p = _reader.indexBarFromCacheByRange(key, 202011011500, 0, count);
	(void)p;
	count = 5;
	p = _reader.indexBarFromCacheByCount(key, 0, count);
	(void)p;

	// 这一条是安全的：readBarsFromCacheByRange 有 begin() 守卫，但结果是「静默返回 0 条」，
	// 也就是默认参数 etime=0 在拷贝版里等价于「什么都读不到」。
	std::vector<WTSBarStruct> out;
	EXPECT_EQ(_reader.readBarsFromCacheByRange(key, 202011011500, 0, out), 0u);
}

}
