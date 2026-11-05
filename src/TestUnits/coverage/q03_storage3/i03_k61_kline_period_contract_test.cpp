// 迭代03 K61（BUG-I02-NEW-060，用户裁定 2026-11-06）：K 线读端只支持 m1/m5/day 三种基础周期。
//
// 合同：
//  - WtDataReader::readKlineSlice、WtRdmDtReader::readKlineSliceByRange/ByCount 收到其他周期
//    （KP_Tick、KP_Hour、KP_Half 或越界值）时返回 NULL，打一条 ERROR“Unsupported kline period”，
//    不读文件、不调用外部历史加载器。
//  - 基础周期照常走加载器，并把调用方请求的周期原样传给加载器。
// 小时线、半小时线等由上层（WtDtMgr / WTSDataFactory）从基础周期重采样，不经过读端。
#include "r52_storage_fixture.h"

#include "WtDataStorage/WtDataReader.h"
#include "WtDataStorage/WtRdmDtReader.h"

#include <vector>

namespace i03k61 {

using namespace q03;
using std::string;
using std::vector;

class CountingLoader : public r52::Loader
{
public:
	bool loadFinalHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override
	{
		_final_periods.push_back(period);
		return r52::Loader::loadFinalHisBars(obj, stdCode, period, cb);
	}
	bool loadRawHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override
	{
		_raw_periods.push_back(period);
		return r52::Loader::loadRawHisBars(obj, stdCode, period, cb);
	}
	size_t calls() const { return _final_periods.size() + _raw_periods.size(); }

	vector<WTSKlinePeriod> _final_periods;
	vector<WTSKlinePeriod> _raw_periods;
};

class I03K61KlinePeriodTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("i03k61"));
		_root = _sandbox->root_abs();
		_bd.set_cur_tdate(20240110);
		_rdm.init(NULL, &_qsink);
	}

	void TearDown() override
	{
		_rdm.clearCache();
		_sandbox.reset();
	}

	void init_reader(r52::Loader* loader)
	{
		const string cfgfile = _root + "reader.json";
		const string json = fmtutil::format("{{\"path\":\"{}\",\"adjust_flag\":0}}", _root);
		ASSERT_TRUE(write_bytes(cfgfile, json));
		WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
		ASSERT_TRUE(cfg != NULL);
		_reader.init(cfg, &_sink, loader);
		cfg->release();
	}

	static vector<WTSBarStruct> one_bar()
	{
		vector<WTSBarStruct> bars;
		WTSBarStruct bar;
		bar.date = 20240108;
		bar.time = TimeUtils::timeToMinBar(20240108, 1500);
		bar.open = bar.high = bar.low = bar.close = 1.0;
		bar.vol = 1;
		bars.push_back(bar);
		return bars;
	}

	std::unique_ptr<Sandbox>	_sandbox;
	string						_root;
	r52::BDMgr					_bd;
	HotMgr						_hot;
	r52::ReaderSink				_sink{&_bd, &_hot};
	RdmSink						_qsink{&_bd, &_hot};
	WtDataReader				_reader;
	WtRdmDtReader				_rdm;
};

TEST_F(I03K61KlinePeriodTest, ReaderRejectsNonBasePeriodsWithoutTouchingLoader)
{
	CountingLoader loader;
	loader.add_final("SSE.STK.600030", one_bar());
	loader.add_raw("SSE.STK.600030", one_bar());
	init_reader(&loader);

	const WTSKlinePeriod bad[] = { KP_Tick, KP_Hour, KP_Half, (WTSKlinePeriod)99 };
	for (WTSKlinePeriod period : bad)
	{
		const uint32_t errorsBefore = _sink.count_of(LL_ERROR);
		WTSKlineSlice* slice = _reader.readKlineSlice("SSE.STK.600030", period, 10, 202401081500ull);
		EXPECT_EQ(nullptr, slice) << "period " << (uint32_t)period;
		if (slice != NULL)
			slice->release();
		EXPECT_EQ(errorsBefore + 1, _sink.count_of(LL_ERROR)) << "period " << (uint32_t)period;
		EXPECT_NE(string::npos, _sink.last_log().find("Unsupported kline period")) << _sink.last_log();
	}
	EXPECT_EQ(0u, loader.calls());
}

TEST_F(I03K61KlinePeriodTest, ReaderPassesBasePeriodsToLoaderUnchanged)
{
	CountingLoader loader;
	init_reader(&loader);

	const WTSKlinePeriod good[] = { KP_Minute1, KP_Minute5, KP_DAY };
	for (WTSKlinePeriod period : good)
	{
		WTSKlineSlice* slice = _reader.readKlineSlice("SSE.STK.600030", period, 10, 202401081500ull);
		if (slice != NULL)
			slice->release();
		EXPECT_EQ(string::npos, _sink.last_log().find("Unsupported kline period")) << "period " << (uint32_t)period;
	}
	ASSERT_EQ(3u, loader._final_periods.size());
	EXPECT_EQ(KP_Minute1, loader._final_periods[0]);
	EXPECT_EQ(KP_Minute5, loader._final_periods[1]);
	EXPECT_EQ(KP_DAY, loader._final_periods[2]);
}

TEST_F(I03K61KlinePeriodTest, RdmReaderRejectsNonBasePeriodsByRangeAndByCount)
{
	const WTSKlinePeriod bad[] = { KP_Tick, KP_Hour, KP_Half, (WTSKlinePeriod)99 };
	for (WTSKlinePeriod period : bad)
	{
		uint32_t errorsBefore = _qsink.count_of(LL_ERROR);
		WTSKlineSlice* slice = _rdm.readKlineSliceByRange("SSE.STK.600030", period, 202401080930ull, 202401081500ull);
		EXPECT_EQ(nullptr, slice) << "range period " << (uint32_t)period;
		if (slice != NULL)
			slice->release();
		EXPECT_EQ(errorsBefore + 1, _qsink.count_of(LL_ERROR)) << "range period " << (uint32_t)period;
		EXPECT_NE(string::npos, _qsink.last_log().find("Unsupported kline period")) << _qsink.last_log();

		errorsBefore = _qsink.count_of(LL_ERROR);
		slice = _rdm.readKlineSliceByCount("SSE.STK.600030", period, 10, 202401081500ull);
		EXPECT_EQ(nullptr, slice) << "count period " << (uint32_t)period;
		if (slice != NULL)
			slice->release();
		EXPECT_EQ(errorsBefore + 1, _qsink.count_of(LL_ERROR)) << "count period " << (uint32_t)period;
		EXPECT_NE(string::npos, _qsink.last_log().find("Unsupported kline period")) << _qsink.last_log();
	}
}

TEST_F(I03K61KlinePeriodTest, RdmReaderBasePeriodsAreNotRejected)
{
	const WTSKlinePeriod good[] = { KP_Minute1, KP_Minute5, KP_DAY };
	for (WTSKlinePeriod period : good)
	{
		WTSKlineSlice* slice = _rdm.readKlineSliceByRange("SSE.STK.600030", period, 202401080930ull, 202401081500ull);
		if (slice != NULL)
			slice->release();
		EXPECT_EQ(string::npos, _qsink.last_log().find("Unsupported kline period")) << "range period " << (uint32_t)period;
		slice = _rdm.readKlineSliceByCount("SSE.STK.600030", period, 10, 202401081500ull);
		if (slice != NULL)
			slice->release();
		EXPECT_EQ(string::npos, _qsink.last_log().find("Unsupported kline period")) << "count period " << (uint32_t)period;
	}
}

} // namespace i03k61
