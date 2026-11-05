// Iteration 03 card 11 = B -- WtDtServo/WtDataManager cache ownership
// (BUG-R102-002 clear_cache, BUG-R123-003 destructor order).
// Uses only members that exist in both baseline and working tree, so it builds
// for the red control as well.
#include "q03_storage3_fixture.h"

#define private public
#include "WtDtServo/WtDataManager.h"
#undef private

#include <memory>

using namespace q03;

namespace {

int g_reader_alive = 0;
int g_removed = 0;

std::vector<WTSBarStruct> minute_bars(uint32_t hhmmStart, uint32_t count, uint32_t seqBase)
{
	std::vector<WTSBarStruct> ret;
	uint32_t h = hhmmStart / 100, m = hhmmStart % 100;
	for (uint32_t i = 0; i < count; i++)
	{
		const uint64_t t = (uint64_t)(20240102 - 19900000) * 10000 + h * 100 + m;
		ret.push_back(make_bar(20240102, t, seqBase + i));
		if (++m == 60) { m = 0; h++; }
	}
	return ret;
}

class FakeReader : public IRdmDtReader
{
public:
	FakeReader() { g_reader_alive++; }
	~FakeReader() override { g_reader_alive--; }

	WTSOrdDtlSlice* readOrdDtlSliceByRange(const char*, uint64_t, uint64_t) override { calls++; return NULL; }
	WTSOrdQueSlice* readOrdQueSliceByRange(const char*, uint64_t, uint64_t) override { calls++; return NULL; }
	WTSTransSlice* readTransSliceByRange(const char*, uint64_t, uint64_t) override { calls++; return NULL; }
	WTSTickSlice* readTickSliceByDate(const char*, uint32_t) override { calls++; return NULL; }
	WTSTickSlice* readTickSliceByRange(const char*, uint64_t, uint64_t) override { calls++; return NULL; }
	WTSTickSlice* readTickSliceByCount(const char*, uint32_t, uint64_t) override { calls++; return NULL; }
	WTSKlineSlice* readKlineSliceByRange(const char* code, WTSKlinePeriod period, uint64_t, uint64_t) override
	{
		calls++;
		if (more.empty())
			return NULL;
		return WTSKlineSlice::create(code, period, 1, more.data(), (int32_t)more.size());
	}
	WTSKlineSlice* readKlineSliceByCount(const char* code, WTSKlinePeriod period, uint32_t, uint64_t) override
	{
		calls++;
		return WTSKlineSlice::create(code, period, 1, base.data(), (int32_t)base.size());
	}
	void clearCache() override { clears++; }

	std::vector<WTSBarStruct> base;
	std::vector<WTSBarStruct> more;
	int calls = 0;
	int clears = 0;
};

void remove_reader(IRdmDtReader* r)
{
	g_removed++;
	delete r;
}

class I03SliceOwnerServo : public ::testing::Test
{
protected:
	void SetUp() override
	{
		g_removed = 0;
		dm.reset(new WtDataManager());
		reader = new FakeReader();
		reader->base = minute_bars(931, 4, 1);
		dm->_reader = reader;
		dm->_remover = remove_reader;
		dm->_bd_mgr = &bd;
		dm->_hot_mgr = &hot;
		dm->_align_by_section = false;
	}

	WTSKlineSlice* query()
	{
		return dm->get_kline_slice_by_range("SSE.STK.600000", KP_Minute1, 2, 202401020900ull, 202401021500ull);
	}

	BaseDataMgr bd;
	HotMgr hot;
	FakeReader* reader = nullptr;
	std::unique_ptr<WtDataManager> dm;
};

}

// R102: clear_cache releases the manager's own resample cache and still clears the reader.
TEST_F(I03SliceOwnerServo, ClearCacheDropsOwnCacheButHeldSliceStaysValid)
{
	WTSKlineSlice* held = query();
	ASSERT_NE(held, nullptr);
	ASSERT_GT(held->size(), 0);
	const double close0 = held->at(0)->close;
	ASSERT_EQ(dm->_bars_cache.size(), 1u);

	dm->clear_cache();
	EXPECT_EQ(reader->clears, 1);
	EXPECT_TRUE(dm->_bars_cache.empty());
	EXPECT_DOUBLE_EQ(held->at(0)->close, close0);

	// the next query rebuilds from the reader instead of hitting the old cache
	const int before = reader->calls;
	WTSKlineSlice* again = query();
	ASSERT_NE(again, nullptr);
	EXPECT_GT(reader->calls, before);
	again->release();
	held->release();
}

// R102: an incremental update of the cached series must not invalidate a held slice.
TEST_F(I03SliceOwnerServo, IncrementalUpdateDoesNotInvalidateHeldSlice)
{
	WTSKlineSlice* held = query();
	ASSERT_NE(held, nullptr);
	ASSERT_GT(held->size(), 0);
	std::vector<WTSBarStruct> snapshot;
	for (int32_t i = 0; i < held->size(); i++)
		snapshot.push_back(*held->at(i));

	reader->more = minute_bars(935, 115, 100);	// grows the cached vector well past its capacity
	WTSKlineSlice* grown = query();
	ASSERT_NE(grown, nullptr);
	EXPECT_GT(grown->size(), held->size());

	ASSERT_EQ((std::size_t)held->size(), snapshot.size());
	for (int32_t i = 0; i < held->size(); i++)
	{
		EXPECT_EQ(held->at(i)->time, snapshot[i].time);
		EXPECT_DOUBLE_EQ(held->at(i)->close, snapshot[i].close);
	}
	grown->release();
	held->release();
}

// R123: destruction destroys the reader through the module remover exactly once,
// and a slice held by the caller outlives the manager.
TEST_F(I03SliceOwnerServo, DestructorRemovesReaderOnceAndHeldSliceOutlivesManager)
{
	WTSKlineSlice* held = query();
	ASSERT_NE(held, nullptr);
	const double close0 = held->at(0)->close;

	dm.reset();
	EXPECT_EQ(g_removed, 1);
	EXPECT_EQ(g_reader_alive, 0);
	EXPECT_DOUBLE_EQ(held->at(0)->close, close0);
	held->release();
}

// R123: partial initialization (no reader) must not call any remover.
TEST_F(I03SliceOwnerServo, PartialInitDestructorDoesNotCallRemover)
{
	delete reader;
	reader = nullptr;
	dm->_reader = NULL;
	dm->_remover = remove_reader;
	dm.reset();
	EXPECT_EQ(g_removed, 0);
	EXPECT_EQ(g_reader_alive, 0);
}

// R102: clear_cache also clears the subscribed realtime bar map.
TEST_F(I03SliceOwnerServo, ClearCacheClearsRealtimeBars)
{
	dm->subscribe_bar("SSE.STK.600000", KP_Minute1, 1);
	ASSERT_NE(dm->_rt_bars, nullptr);
	ASSERT_EQ(dm->_rt_bars->size(), 1u);
	dm->clear_cache();
	EXPECT_EQ(dm->_rt_bars->size(), 0u);
	EXPECT_EQ(reader->clears, 1);
}
