// 迭代03 第五会话主线程：决策 K34=A（2026-11-04 用户裁定“先 C 再 A”，BUG-R122-002）
// AD 读端 HOT/custom 规则代码按规则分段从物理合约拼接：
//   - 每段只取该物理合约在本段交易日内的数据（换约后旧合约继续交易的数据、新合约成为主力前的数据均不取）；
//   - 不读预合并的品种键历史；
//   - 前复权以最新段为基准按段因子换算；
//   - K 线缓存跟随最新段物理合约，规则切换时在 onMinuteEnd 中重建并回调新增 K 线；
//   - 分段边界无法换算为分钟时间时拒绝。
// 改前（K34=C）：规则代码一律拒绝。
#include "r100_ad_fixture.h"

#include <map>
#include <utility>

using namespace r100fx;
using namespace r4fx;

namespace {

const uint32_t kOld = 20230104;	// last trading date of rb2401 as HOT
const uint32_t kNew = 20230105;	// rb2405 becomes HOT

uint64_t k34MinBar(uint32_t date, uint32_t hhmm)
{
	return (uint64_t)(date - 19900000) * 10000 + hhmm;
}

WTSBarStruct k34Bar(uint32_t tdate, uint64_t time, double px)
{
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	bar.date = tdate;
	bar.time = time;
	bar.open = px;
	bar.high = px + 1;
	bar.low = px - 1;
	bar.close = px;
	bar.vol = 10;
	return bar;
}

// Session boundaries in the production YYYYMMDDHHMM form; trading date == natural date.
class K34BaseDataMgr : public R4BaseDataMgr
{
public:
	bool usable = true;
	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool, bool isStart) override
	{
		if (!usable)
			return R4BaseDataMgr::getBoundaryTime(NULL, tDate, false, isStart);
		return (uint64_t)tDate * 10000 + (isStart ? 900 : 1500);
	}
};

// Switch schedule: from each date on, the listed physical contract is the rule's code.
class K34HotMgr : public R100HotMgr
{
public:
	struct Switch { uint32_t date; std::string code; double factor; };
	std::vector<Switch> schedule;

	const char* getCustomRawCode(const char*, const char*, uint32_t dt) override
	{
		const char* code = "";
		for (const Switch& item : schedule)
		{
			if (item.date <= dt)
				code = item.code.c_str();
		}
		return code;
	}

	bool splitCustomSections(const char*, const char*, uint32_t sDt, uint32_t eDt, HotSections& sections) override
	{
		for (std::size_t i = 0; i < schedule.size(); ++i)
		{
			if (schedule[i].date > eDt)
				break;
			const uint32_t left = (std::max)(schedule[i].date, sDt);
			const uint32_t right = (i + 1 < schedule.size() && schedule[i + 1].date <= eDt)
				? TimeUtils::getNextDate(schedule[i + 1].date, -1) : eDt;
			if (left <= right)
				sections.emplace_back(HotSection(schedule[i].code.c_str(), left, right, schedule[i].factor));
		}
		return !sections.empty();
	}
};

class S5MainK34aPhysicalSegments : public R100ADReaderTest
{
protected:
	K34BaseDataMgr* k34bd = nullptr;
	K34HotMgr* k34hot = nullptr;

	void SetUp() override
	{
		R100ADReaderTest::SetUp();
		delete _sink;
		delete _hot;
		delete _bd;
		k34bd = new K34BaseDataMgr();
		k34hot = new K34HotMgr();
		_bd = k34bd;
		_hot = k34hot;
		_sink = new R100ReaderSink(_bd, _hot);
		k34hot->set_tag("HOT");
		k34hot->schedule = { { 19900102, "rb2401", 0.8 }, { kNew, "rb2405", 1.0 } };
		add_future("EXK", "rb", "rb2401");
		add_future("EXK", "rb", "rb2405");
	}

	void seedMinuteBars()
	{
		std::vector<WTSBarStruct> old;
		old.push_back(k34Bar(kOld, k34MinBar(kOld, 931), 3800.0));
		old.push_back(k34Bar(kOld, k34MinBar(kOld, 932), 3801.0));
		old.push_back(k34Bar(kNew, k34MinBar(kNew, 931), 3700.0));	// old contract after the switch: excluded
		seed_bars("min1", "EXK", "rb2401", old, false);
		std::vector<WTSBarStruct> fresh;
		fresh.push_back(k34Bar(kOld, k34MinBar(kOld, 933), 3600.0));	// before rb2405 became HOT: excluded
		fresh.push_back(k34Bar(kNew, k34MinBar(kNew, 931), 3900.0));
		fresh.push_back(k34Bar(kNew, k34MinBar(kNew, 932), 3901.0));
		seed_bars("min1", "EXK", "rb2405", fresh, false);
		std::vector<WTSBarStruct> merged;
		merged.push_back(k34Bar(kNew, k34MinBar(kNew, 933), 9999.0));	// pre-merged product key: never used
		seed_bars("min1", "EXK", "rb", merged, false);
	}

	static std::vector<double> closes(WTSKlineSlice* slice)
	{
		std::vector<double> out;
		for (int32_t i = 0; slice != NULL && i < (int32_t)slice->size(); ++i)
			out.push_back(slice->at(i)->close);
		return out;
	}
};

} // namespace

TEST_F(S5MainK34aPhysicalSegments, HotBarsAreStitchedFromPhysicalSectionsOnly)
{
	seedMinuteBars();
	init_reader();
	WTSKlineSlice* all = _reader->readKlineSlice("EXK.rb.HOT", KP_Minute1, 10, 202301051000ull);
	ASSERT_NE(all, nullptr);
	EXPECT_EQ((std::vector<double>{ 3800.0, 3801.0, 3900.0, 3901.0 }), closes(all));
	all->release();

	WTSKlineSlice* last3 = _reader->readKlineSlice("EXK.rb.HOT", KP_Minute1, 3, 202301051000ull);
	ASSERT_NE(last3, nullptr);
	EXPECT_EQ((std::vector<double>{ 3801.0, 3900.0, 3901.0 }), closes(last3));
	last3->release();
}

TEST_F(S5MainK34aPhysicalSegments, ForwardAdjustedHotScalesOlderSectionsByTheirFactor)
{
	seedMinuteBars();
	init_reader();
	WTSKlineSlice* adj = _reader->readKlineSlice("EXK.rb.HOT-", KP_Minute1, 10, 202301051000ull);
	ASSERT_NE(adj, nullptr);
	EXPECT_EQ((std::vector<double>{ 3800.0 * 0.8, 3801.0 * 0.8, 3900.0, 3901.0 }), closes(adj));
	adj->release();
}

TEST_F(S5MainK34aPhysicalSegments, HotTicksAreStitchedByTradingDate)
{
	std::vector<WTSTickStruct> old;
	old.push_back(r4fx::make_tick("EXK", "rb2401", kOld, kOld, 93000000, 3800.0, 1, 10, 5, 0, 100, 1000));
	old.push_back(r4fx::make_tick("EXK", "rb2401", kOld, kOld, 93100000, 3801.0, 1, 10, 5, 0, 100, 1000));
	old.push_back(r4fx::make_tick("EXK", "rb2401", kNew, kNew, 93000000, 3700.0, 1, 10, 5, 0, 100, 1000));
	seed_ticks("EXK", "rb2401", old);
	std::vector<WTSTickStruct> fresh;
	fresh.push_back(r4fx::make_tick("EXK", "rb2405", kOld, kOld, 93200000, 3600.0, 1, 10, 5, 0, 100, 1000));
	fresh.push_back(r4fx::make_tick("EXK", "rb2405", kNew, kNew, 93000000, 3900.0, 1, 10, 5, 0, 100, 1000));
	seed_ticks("EXK", "rb2405", fresh);
	std::vector<WTSTickStruct> merged;
	merged.push_back(r4fx::make_tick("EXK", "rb", kNew, kNew, 93100000, 9999.0, 1, 10, 5, 0, 100, 1000));
	seed_ticks("EXK", "rb", merged);
	init_reader();

	WTSTickSlice* ticks = _reader->readTickSlice("EXK.rb.HOT", 10, 20230105100000000ull);
	ASSERT_NE(ticks, nullptr);
	std::vector<double> prices;
	for (int32_t i = 0; i < (int32_t)ticks->size(); ++i)
		prices.push_back(ticks->at(i)->price);
	EXPECT_EQ((std::vector<double>{ 3800.0, 3801.0, 3900.0 }), prices);
	ticks->release();
}

TEST_F(S5MainK34aPhysicalSegments, CacheFollowsTheRuleSwitchAtMinuteEnd)
{
	seedMinuteBars();
	init_reader();
	// Before the switch the rule maps to rb2401 only.
	WTSKlineSlice* before = _reader->readKlineSlice("EXK.rb.HOT", KP_Minute1, 10, 202301041500ull);
	ASSERT_NE(before, nullptr);
	EXPECT_EQ((std::vector<double>{ 3800.0, 3801.0 }), closes(before));
	before->release();

	const uint32_t callsBefore = _sink->bar_calls();
	_reader->onMinuteEnd(kNew, 932, kNew);
	EXPECT_EQ(callsBefore + 2, _sink->bar_calls()) << "the two new-section bars are reported once";
	EXPECT_EQ("EXK.rb.HOT", _sink->last_bar_code());
	EXPECT_DOUBLE_EQ(3901.0, _sink->last_bar().close);

	WTSKlineSlice* after = _reader->readKlineSlice("EXK.rb.HOT", KP_Minute1, 10, 202301051000ull);
	ASSERT_NE(after, nullptr);
	EXPECT_EQ((std::vector<double>{ 3800.0, 3801.0, 3900.0, 3901.0 }), closes(after));
	after->release();
}

TEST_F(S5MainK34aPhysicalSegments, UnusableSectionBoundariesFailClosed)
{
	seedMinuteBars();
	k34bd->usable = false;
	init_reader();
	const uint32_t errorsBefore = _sink->log_count(LL_ERROR);
	WTSKlineSlice* bars = _reader->readKlineSlice("EXK.rb.HOT", KP_Minute1, 10, 202301051000ull);
	EXPECT_EQ(nullptr, bars);
	if (bars != NULL)
		bars->release();
	EXPECT_GT(_sink->log_count(LL_ERROR), errorsBefore);
	// The mapping client rejects an unusable legacy boundary; the section read is refused.
	EXPECT_GE(_sink->count_containing("Mapping boundaries of EXK.rb2405 section"), 1u);
}
