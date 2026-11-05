// Iteration 03 card 11 = B: header-level owner/refcount/copy-on-write tests (green only).
#include <gtest/gtest.h>

#include <boost/circular_buffer.hpp>
#include <memory>
#include <vector>

#include "Includes/WTSDataDef.hpp"

USING_NS_WTP;

namespace {

struct CountedObject : public WTSObject
{
	static int alive;
	CountedObject() { alive++; }
	~CountedObject() override { alive--; }
};
int CountedObject::alive = 0;

template <typename Slice, typename Item>
void check_final_release(Slice* (*make)(Item*, uint32_t))
{
	std::weak_ptr<std::vector<Item>> weak;
	Slice* slice = nullptr;
	{
		auto buf = std::make_shared<std::vector<Item>>(4);
		weak = buf;
		slice = make(buf->data(), 4);
		ASSERT_NE(slice, nullptr);
		EXPECT_TRUE(slice->addOwner(buf));
		EXPECT_TRUE(slice->addOwner(buf));	// duplicates are ignored
		EXPECT_EQ(slice->ownerCount(), 1u);
	}
	EXPECT_FALSE(weak.expired());
	slice->retain();
	slice->release();
	EXPECT_FALSE(weak.expired());
	slice->release();
	EXPECT_TRUE(weak.expired());
}

}

TEST(I03SliceOwnerUnit, OwnersAreReleasedWithLastSliceReferenceForAllSliceTypes)
{
	check_final_release<WTSTickSlice, WTSTickStruct>([](WTSTickStruct* p, uint32_t n) { return WTSTickSlice::create("A", p, n); });
	check_final_release<WTSOrdDtlSlice, WTSOrdDtlStruct>([](WTSOrdDtlStruct* p, uint32_t n) { return WTSOrdDtlSlice::create("A", p, n); });
	check_final_release<WTSOrdQueSlice, WTSOrdQueStruct>([](WTSOrdQueStruct* p, uint32_t n) { return WTSOrdQueSlice::create("A", p, n); });
	check_final_release<WTSTransSlice, WTSTransStruct>([](WTSTransStruct* p, uint32_t n) { return WTSTransSlice::create("A", p, n); });
	check_final_release<WTSKlineSlice, WTSBarStruct>([](WTSBarStruct* p, uint32_t n) { return WTSKlineSlice::create("A", KP_DAY, 1, p, (int32_t)n); });
}

TEST(I03SliceOwnerUnit, MultipleOwnersUseInlineThenOverflowStorage)
{
	WTSTickSlice* slice = WTSTickSlice::create("A");
	std::vector<std::weak_ptr<int>> weak;
	for (int i = 0; i < 5; i++)
	{
		auto p = std::make_shared<int>(i);
		weak.push_back(p);
		EXPECT_TRUE(slice->addOwner(p));
	}
	EXPECT_FALSE(slice->addOwner(WTSSliceOwnerPtr()));
	EXPECT_EQ(slice->ownerCount(), 5u);
	for (auto& w : weak)
		EXPECT_FALSE(w.expired());
	slice->release();
	for (auto& w : weak)
		EXPECT_TRUE(w.expired());
}

TEST(I03SliceOwnerUnit, WTSObjectOwnerRetainsAndReleases)
{
	CountedObject* obj = new CountedObject();
	ASSERT_EQ(CountedObject::alive, 1);
	WTSKlineSlice* slice = WTSKlineSlice::create("A", KP_DAY, 1);
	slice->addOwner(makeSliceOwner(obj));
	EXPECT_EQ(obj->retainCount(), 2u);
	obj->release();	// cache drops its reference
	EXPECT_EQ(CountedObject::alive, 1);
	slice->release();
	EXPECT_EQ(CountedObject::alive, 0);
	EXPECT_FALSE(makeSliceOwner(nullptr));
}

TEST(I03SliceOwnerUnit, SharedBufferDetachesOnStructuralChangeWhileBorrowed)
{
	WTSSharedBuffer<std::vector<int>> buf;
	EXPECT_TRUE(buf.empty());
	EXPECT_FALSE(buf.owner());
	buf = std::vector<int>{ 1, 2, 3 };
	const int* oldData = buf.data();
	EXPECT_FALSE(buf.shared());

	WTSSliceOwnerPtr borrowed = buf.owner();
	EXPECT_TRUE(buf.shared());

	buf.push_back(4);	// detach + append
	EXPECT_NE(buf.data(), oldData);
	EXPECT_EQ(oldData[0], 1);
	EXPECT_EQ(oldData[2], 3);
	EXPECT_EQ(buf.size(), 4u);

	WTSSliceOwnerPtr borrowed2 = buf.owner();
	const int* data2 = buf.data();
	buf.clear();	// renew without copying
	EXPECT_TRUE(buf.empty());
	EXPECT_EQ(data2[3], 4);

	buf.resize(2);
	buf[0] = 9;
	WTSSliceOwnerPtr borrowed3 = buf.owner();
	const int* data3 = buf.data();
	buf.resize(1);	// shared: keeps prefix in a new buffer
	EXPECT_EQ(data3[1], 0);
	EXPECT_EQ(buf[0], 9);

	std::vector<int> other{ 7, 8 };
	WTSSliceOwnerPtr borrowed4 = buf.owner();
	const int* data4 = buf.data();
	buf.swap(other);
	EXPECT_EQ(buf.size(), 2u);
	EXPECT_EQ(buf[1], 8);
	EXPECT_EQ(other.size(), 1u);
	EXPECT_EQ(other[0], 9);
	EXPECT_EQ(data4[0], 9);

	buf.assign(other.begin(), other.end());
	EXPECT_EQ(buf.size(), 1u);
}

TEST(I03SliceOwnerUnit, SharedBufferMutatesInPlaceWhenNotBorrowed)
{
	WTSSharedBuffer<std::vector<int>> buf;
	buf.reserve(16);
	buf.push_back(1);
	const int* p = buf.data();
	buf.push_back(2);
	EXPECT_EQ(buf.data(), p);
	WTSSharedBuffer<std::vector<int>> copy(buf);	// copies are deep
	copy.push_back(3);
	EXPECT_EQ(buf.size(), 2u);
	EXPECT_EQ(copy.size(), 3u);
	WTSSharedBuffer<std::vector<int>> moved(std::move(copy));
	EXPECT_EQ(moved.size(), 3u);
	EXPECT_TRUE(copy.empty());
	const std::vector<int>& view = moved;
	EXPECT_EQ(view.back(), 3);
}

TEST(I03SliceOwnerUnit, SharedCircularBufferDetachesBeforeOverwrite)
{
	WTSSharedBuffer<boost::circular_buffer<int>> buf;
	buf.rset_capacity(3);
	buf.push_back(1);
	buf.push_back(2);
	buf.push_back(3);
	auto one = buf.array_one();
	WTSSliceOwnerPtr borrowed = buf.owner();
	buf.push_back(4);	// would overwrite the oldest element in place
	EXPECT_EQ(one.first[0], 1);
	EXPECT_EQ(one.first[2], 3);
	EXPECT_EQ(buf.back(), 4);
	EXPECT_EQ(buf.size(), 3u);
	buf.mutate().back() = 5;
	EXPECT_EQ(one.first[2], 3);
}
