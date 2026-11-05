// BUG-R12-13 call chain (S5): WtRdmDtReader / WtDataReader trading-date mapping.
// A failed mapping must stop the read before any slice is built: no call reaches
// the legacy virtual with an invalid date and no slice is returned.
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "fakes.hpp"
#include "Includes/IDataReader.h"
#include "Includes/IRdmDtReader.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "WtDataStorage/WtDataReader.h"
#include "WtDataStorage/WtRdmDtReader.h"

using namespace wtp;
namespace fs = std::filesystem;

namespace {
class RdmSink : public IRdmDtReaderSink
{
public:
	RdmSink(IBaseDataMgr* bd, IHotMgr* hot) : _bd(bd), _hot(hot) {}
	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return _hot; }
	void reader_log(WTSLogLevel ll, const char* message) override { if (ll >= LL_ERROR) errors.push_back(message); }
	std::vector<std::string> errors;
private:
	IBaseDataMgr* _bd; IHotMgr* _hot;
};

class LiveSink : public IDataReaderSink
{
public:
	LiveSink(IBaseDataMgr* bd, IHotMgr* hot) : _bd(bd), _hot(hot) {}
	void on_bar(const char*, WTSKlinePeriod, WTSBarStruct*) override {}
	void on_all_bar_updated(uint32_t) override {}
	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return _hot; }
	uint32_t get_date() override { return 20240227; }
	uint32_t get_min_time() override { return 1000; }
	uint32_t get_secs() override { return 0; }
	void reader_log(WTSLogLevel ll, const char* message) override { if (ll >= LL_ERROR) errors.push_back(message); }
	std::vector<std::string> errors;
private:
	IBaseDataMgr* _bd; IHotMgr* _hot;
};

class ReaderChain : public ::testing::Test
{
protected:
	fs::path root;
	FakeHotMgr hot;
	void SetUp() override
	{
		setenv("TZ", "UTC", 1); tzset(); FakeClock::reset();
		char name[] = "/tmp/wt-i03-s5-reader-XXXXXX";
		char* dir = mkdtemp(name);
		ASSERT_NE(nullptr, dir);
		root = dir;
	}
	void TearDown() override { FakeClock::reset(); std::error_code ec; fs::remove_all(root, ec); }

	WTSVariant* cfg()
	{
		WTSVariant* c = WTSVariant::createObject();
		c->append("path", (root.string() + "/").c_str());
		return c;
	}
	static bool anyInvalidDate(const FakeLegacyMgr& mgr)
	{
		for (const auto& call : mgr.calcCalls)
		{
			uint32_t weekDay = 0;
			// Date 0 is the legacy "manager's current date" and stays legal.
			if ((call.date != 0 && !TimeUtils::tryGetWeekDay(call.date, weekDay)) || !WTSSessionInfo::isValidSessionTime(call.time))
				return true;
		}
		return false;
	}
};
}

TEST_F(ReaderChain, RdmValidRangeMapsEndpointsAndManagerCurrentDate)
{
	FakeLegacyMgr mgr;
	RdmSink sink(&mgr, &hot);
	WtRdmDtReader reader;
	WTSVariant* c = cfg();
	reader.init(c, &sink);
	WTSTickSlice* slice = reader.readTickSliceByRange("EX.P.2406", 20240226093000000ULL, 20240227100000000ULL);
	if (slice) slice->release();
	ASSERT_EQ(3u, mgr.calcCalls.size());
	EXPECT_EQ(20240227u, mgr.calcCalls[0].date);
	EXPECT_EQ(20240226u, mgr.calcCalls[1].date);
	EXPECT_EQ(0u, mgr.calcCalls[2].date); // manager's current date (legacy meaning kept)
	EXPECT_FALSE(anyInvalidDate(mgr));
	c->release();
}

TEST_F(ReaderChain, RdmInvalidRangeEndpointsStopBeforeMappingAndSlicing)
{
	FakeLegacyMgr mgr;
	RdmSink sink(&mgr, &hot);
	WtRdmDtReader reader;
	WTSVariant* c = cfg();
	reader.init(c, &sink);
	const uint64_t invalidDate = 20240230093000000ULL;     // February 30
	const uint64_t invalidTime = 20240227096000000ULL;     // 09:60
	const uint64_t valid = 20240227100000000ULL;
	EXPECT_EQ(nullptr, reader.readTickSliceByRange("EX.P.2406", invalidDate, valid));
	EXPECT_EQ(nullptr, reader.readTickSliceByRange("EX.P.2406", valid, invalidTime));
	EXPECT_EQ(nullptr, reader.readTransSliceByRange("EX.P.2406", invalidDate, valid));
	EXPECT_EQ(nullptr, reader.readOrdDtlSliceByRange("EX.P.2406", invalidDate, valid));
	EXPECT_EQ(nullptr, reader.readOrdQueSliceByRange("EX.P.2406", invalidDate, valid));
	EXPECT_EQ(nullptr, reader.readTickSliceByCount("EX.P.2406", 10, invalidDate));
	EXPECT_FALSE(anyInvalidDate(mgr)) << "an invalid endpoint reached the legacy virtual";
	EXPECT_FALSE(sink.errors.empty());
	c->release();
}

TEST_F(ReaderChain, RdmMissingSessionAndClockFailureStopTheRead)
{
	{
		FakeLegacyMgr mgr("CHINA", false);
		RdmSink sink(&mgr, &hot);
		WtRdmDtReader reader;
		WTSVariant* c = cfg();
		reader.init(c, &sink);
		EXPECT_EQ(nullptr, reader.readTransSliceByRange("EX.P.2406", 20240226093000000ULL, 20240227100000000ULL));
		EXPECT_TRUE(mgr.calcCalls.empty());
		c->release();
	}
	{
		FakeLegacyMgr mgr;
		RdmSink sink(&mgr, &hot);
		WtRdmDtReader reader;
		WTSVariant* c = cfg();
		reader.init(c, &sink);
		FakeClock::armed = true;
		FakeClock::fail = true;
		WTSTickSlice* byDate = reader.readTickSliceByDate("EX.P.2406", 20240227);
		WTSTickSlice* byRange = reader.readTickSliceByRange("EX.P.2406", 20240226093000000ULL, 20240227100000000ULL);
		FakeClock::reset();
		EXPECT_EQ(nullptr, byDate);
		EXPECT_EQ(nullptr, byRange);
		EXPECT_FALSE(anyInvalidDate(mgr));
		c->release();
	}
}

TEST_F(ReaderChain, LiveReaderInvalidEndAndClockFailureStopTheRead)
{
	FakeLegacyMgr mgr;
	LiveSink sink(&mgr, &hot);
	WtDataReader reader;
	WTSVariant* c = cfg();
	reader.init(c, &sink);
	EXPECT_EQ(nullptr, reader.readTickSlice("EX.P.2406", 10, 20240230100000000ULL));
	EXPECT_EQ(nullptr, reader.readTransSlice("EX.P.2406", 10, 20240227096000000ULL));
	EXPECT_EQ(nullptr, reader.readKlineSlice("EX.P.2406", KP_Minute1, 10, 202402301000ULL));
	EXPECT_FALSE(anyInvalidDate(mgr));
	FakeClock::armed = true;
	FakeClock::fail = true;
	WTSTickSlice* failed = reader.readTickSlice("EX.P.2406", 10, 20240227100000000ULL);
	FakeClock::reset();
	EXPECT_EQ(nullptr, failed);
	EXPECT_FALSE(anyInvalidDate(mgr));
	// Positive control: a valid end maps both dates explicitly.
	mgr.calcCalls.clear();
	WTSTickSlice* ok = reader.readTickSlice("EX.P.2406", 10, 20240227100000000ULL);
	if (ok) ok->release();
	ASSERT_EQ(2u, mgr.calcCalls.size());
	EXPECT_EQ(20240227u, mgr.calcCalls[0].date);
	EXPECT_FALSE(anyInvalidDate(mgr));
	c->release();
}
