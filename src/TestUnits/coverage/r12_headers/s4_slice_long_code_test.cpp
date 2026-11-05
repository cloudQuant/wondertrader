// I03-NEW-083 (iteration 03, 2026-11-05): WTSKlineSlice::create / WTSTickSlice::create
// copied the caller's stdCode into the fixed _code[MAX_INSTRUMENT_LENGTH] buffer
// without a bound. They now follow the existing WTSTickData::create contract: a
// code that does not fit (length >= MAX_INSTRUMENT_LENGTH) or a NULL code is
// rejected with NULL, and the legacy void setCode keeps the old value.
#include <gtest/gtest.h>

#include <string>

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"

USING_NS_WTP;

namespace {

WTSBarStruct makeBar(uint32_t date, double close)
{
	WTSBarStruct bar;
	bar.date = date;
	bar.time = 0;
	bar.open = close;
	bar.high = close;
	bar.low = close;
	bar.close = close;
	return bar;
}

} // namespace

TEST(S4SliceLongCode, KlineSliceKeepsLongestCodeThatFits)
{
	const std::string code(MAX_INSTRUMENT_LENGTH - 1, 'K');
	WTSBarStruct bars[2] = { makeBar(20240102, 1.0), makeBar(20240103, 2.0) };
	WTSKlineSlice* slice = WTSKlineSlice::create(code.c_str(), KP_DAY, 1, bars, 2);
	ASSERT_NE(nullptr, slice);
	EXPECT_EQ(code, std::string(slice->code()));
	EXPECT_EQ(2u, static_cast<uint32_t>(slice->size()));
	EXPECT_DOUBLE_EQ(2.0, slice->at(1)->close);
	slice->release();
}

TEST(S4SliceLongCode, KlineSliceRejectsCodeThatDoesNotFit)
{
	WTSBarStruct bars[1] = { makeBar(20240102, 1.0) };
	const std::string exact(MAX_INSTRUMENT_LENGTH, 'L');
	EXPECT_EQ(nullptr, WTSKlineSlice::create(exact.c_str(), KP_DAY, 1, bars, 1));
	const std::string legalButLong(63, 'X');
	EXPECT_EQ(nullptr, WTSKlineSlice::create(legalButLong.c_str(), KP_DAY, 1, bars, 1));
	EXPECT_EQ(nullptr, WTSKlineSlice::create(legalButLong.c_str(), KP_Minute1, 1, NULL, 0));
	EXPECT_EQ(nullptr, WTSKlineSlice::create(NULL, KP_DAY, 1, bars, 1));
}

TEST(S4SliceLongCode, KlineSliceLegacySetCodeKeepsOldValueOnOverlongInput)
{
	WTSKlineSlice* slice = WTSKlineSlice::create("SHFE.ag.2512", KP_Minute1, 1, NULL, 0);
	ASSERT_NE(nullptr, slice);
	const std::string tooLong(MAX_INSTRUMENT_LENGTH * 2, 'Z');
	slice->setCode(tooLong.c_str());
	EXPECT_STREQ("SHFE.ag.2512", slice->code());
	slice->setCode(NULL);
	EXPECT_STREQ("SHFE.ag.2512", slice->code());
	const std::string fits(MAX_INSTRUMENT_LENGTH - 1, 'F');
	slice->setCode(fits.c_str());
	EXPECT_EQ(fits, std::string(slice->code()));
	slice->release();
}

TEST(S4SliceLongCode, TickSliceKeepsLongestCodeThatFits)
{
	const std::string code(MAX_INSTRUMENT_LENGTH - 1, 'T');
	WTSTickStruct ticks[2];
	ticks[0].price = 1.0;
	ticks[1].price = 2.0;
	WTSTickSlice* slice = WTSTickSlice::create(code.c_str(), ticks, 2);
	ASSERT_NE(nullptr, slice);
	EXPECT_EQ(2u, static_cast<uint32_t>(slice->size()));
	EXPECT_DOUBLE_EQ(2.0, slice->at(1)->price);
	slice->release();

	WTSTickSlice* empty = WTSTickSlice::create(code.c_str());
	ASSERT_NE(nullptr, empty);
	EXPECT_EQ(0u, static_cast<uint32_t>(empty->size()));
	empty->release();
}

TEST(S4SliceLongCode, TickSliceRejectsCodeThatDoesNotFit)
{
	WTSTickStruct ticks[1];
	const std::string exact(MAX_INSTRUMENT_LENGTH, 'L');
	EXPECT_EQ(nullptr, WTSTickSlice::create(exact.c_str(), ticks, 1));
	const std::string legalButLong(63, 'X');
	EXPECT_EQ(nullptr, WTSTickSlice::create(legalButLong.c_str(), ticks, 1));
	EXPECT_EQ(nullptr, WTSTickSlice::create(legalButLong.c_str()));
	EXPECT_EQ(nullptr, WTSTickSlice::create(NULL, ticks, 1));
}
