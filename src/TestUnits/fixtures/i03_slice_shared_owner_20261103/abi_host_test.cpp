// Host side (new header). Loads the old-header plugin with dlopen(RTLD_LOCAL) and checks
// that append-only owner members keep the old inline accessors working in both directions.
#include <gtest/gtest.h>
#include <dlfcn.h>
#include <memory>
#include <vector>

#include "Includes/WTSDataDef.hpp"

USING_NS_WTP;

namespace {
struct OldPlugin
{
	void* h = nullptr;
	template <typename F> F sym(const char* n) { return reinterpret_cast<F>(dlsym(h, n)); }
	OldPlugin() { h = dlopen(I03_OLD_PLUGIN_PATH, RTLD_NOW | RTLD_LOCAL); }
	~OldPlugin() { if (h) dlclose(h); }
};
}

TEST(I03SliceOwnerAbi, OldHeaderPluginReadsHostSlicesAndReleasesOwners)
{
	OldPlugin p;
	ASSERT_NE(p.h, nullptr) << dlerror();
	auto tickSize = p.sym<uint32_t (*)(WTSTickSlice*)>("old_tick_size");
	auto tickPrice = p.sym<double (*)(WTSTickSlice*, int32_t)>("old_tick_price");
	auto tickBlocks = p.sym<std::size_t (*)(WTSTickSlice*)>("old_tick_blocks");
	auto klSize = p.sym<int32_t (*)(WTSKlineSlice*)>("old_kline_size");
	auto klClose = p.sym<double (*)(WTSKlineSlice*, int32_t)>("old_kline_close");
	auto klMax = p.sym<double (*)(WTSKlineSlice*)>("old_kline_max");
	auto klCode = p.sym<const char* (*)(WTSKlineSlice*)>("old_kline_code");
	auto trSize = p.sym<uint32_t (*)(WTSTransSlice*)>("old_trans_size");
	auto trPrice = p.sym<double (*)(WTSTransSlice*, int32_t)>("old_trans_price");
	auto rel = p.sym<void (*)(WTSObject*)>("old_release");
	auto ret = p.sym<void (*)(WTSObject*)>("old_retain");
	ASSERT_TRUE(tickSize && tickPrice && tickBlocks && klSize && klClose && klMax && klCode && trSize && trPrice && rel && ret);

	auto ticks = std::make_shared<std::vector<WTSTickStruct>>(3);
	auto more = std::make_shared<std::vector<WTSTickStruct>>(2);
	for (int i = 0; i < 3; i++) (*ticks)[i].price = 1.0 + i;
	for (int i = 0; i < 2; i++) (*more)[i].price = 10.0 + i;
	std::weak_ptr<std::vector<WTSTickStruct>> w1 = ticks, w2 = more;
	WTSTickSlice* ts = WTSTickSlice::create("NEW", ticks->data(), 3);
	ts->appendBlock(more->data(), 2);
	ts->addOwner(ticks);
	ts->addOwner(more);
	ticks.reset();
	more.reset();
	EXPECT_EQ(tickSize(ts), 5u);
	EXPECT_EQ(tickBlocks(ts), 2u);
	EXPECT_DOUBLE_EQ(tickPrice(ts, 0), 1.0);
	EXPECT_DOUBLE_EQ(tickPrice(ts, 4), 11.0);
	EXPECT_DOUBLE_EQ(tickPrice(ts, -1), 11.0);
	ret(ts);
	rel(ts);
	EXPECT_FALSE(w1.expired());
	rel(ts);	// final release from old plugin code -> host virtual destructor releases owners
	EXPECT_TRUE(w1.expired());
	EXPECT_TRUE(w2.expired());

	auto bars = std::make_shared<std::vector<WTSBarStruct>>(4);
	for (int i = 0; i < 4; i++) { (*bars)[i].close = 100.0 + i; (*bars)[i].high = 200.0 + i; }
	std::weak_ptr<std::vector<WTSBarStruct>> wb = bars;
	WTSKlineSlice* ks = WTSKlineSlice::create("KCODE", KP_DAY, 1, bars->data(), 4);
	ks->addOwner(bars);
	bars.reset();
	EXPECT_EQ(klSize(ks), 4);
	EXPECT_DOUBLE_EQ(klClose(ks, -1), 103.0);
	EXPECT_DOUBLE_EQ(klMax(ks), 203.0);
	EXPECT_STREQ(klCode(ks), "KCODE");
	rel(ks);
	EXPECT_TRUE(wb.expired());

	auto trans = std::make_shared<std::vector<WTSTransStruct>>(2);
	(*trans)[1].price = 7.5;
	WTSTransSlice* tr = WTSTransSlice::create("T", trans->data(), 2);
	tr->addOwner(trans);
	std::weak_ptr<std::vector<WTSTransStruct>> wt = trans;
	trans.reset();
	EXPECT_EQ(trSize(tr), 2u);
	EXPECT_DOUBLE_EQ(trPrice(tr, 1), 7.5);
	rel(tr);
	EXPECT_TRUE(wt.expired());
}

TEST(I03SliceOwnerAbi, HostReadsAndReleasesOldLayoutSlices)
{
	OldPlugin p;
	ASSERT_NE(p.h, nullptr) << dlerror();
	auto mk = p.sym<WTSTickSlice* (*)(WTSTickStruct*, uint32_t)>("old_create_tick");
	auto oldTickSize = p.sym<std::size_t (*)()>("old_sizeof_tick_slice");
	auto oldKlineSize = p.sym<std::size_t (*)()>("old_sizeof_kline_slice");
	ASSERT_TRUE(mk && oldTickSize && oldKlineSize);
	// Layout is append-only: the new object is strictly larger, prefix offsets unchanged.
	EXPECT_GT(sizeof(WTSTickSlice), oldTickSize());
	EXPECT_GT(sizeof(WTSKlineSlice), oldKlineSize());
	std::printf("[abi] sizeof WTSTickSlice old=%zu new=%zu, WTSKlineSlice old=%zu new=%zu\n",
		oldTickSize(), sizeof(WTSTickSlice), oldKlineSize(), sizeof(WTSKlineSlice));

	std::vector<WTSTickStruct> ticks(2);
	ticks[1].price = 42.0;
	WTSTickSlice* s = mk(ticks.data(), 2);
	ASSERT_NE(s, nullptr);
	EXPECT_EQ(s->size(), 2u);	// host inline accessor on old-layout object
	EXPECT_DOUBLE_EQ(s->at(1)->price, 42.0);
	EXPECT_EQ(s->get_block_counts(), 1u);
	// NOTE: addOwner()/ownerCount() must NOT be called on an old-layout object (member
	// beyond the old object's size); only host-created slices carry owners.
	s->release();	// virtual destructor from the plugin's vtable
}
