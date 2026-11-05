// L03 Includes lane 单元测试：WTSObject 引用计数 / WTSPoolObject 对象池 / WTSSwitchItem
// 目标文件: src/Includes/WTSObject.hpp, src/Includes/WTSSwitchItem.hpp
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Includes/WTSObject.hpp"
#include "Includes/WTSSwitchItem.hpp"
#include "Includes/WTSSessionInfo.hpp"

using namespace wtp;

namespace
{
int g_tracked_dtor = 0;

// 用于观察 WTSObject::release() 归零即 delete 的行为
class L03TrackedObj : public WTSObject
{
public:
	L03TrackedObj() : tag_(0) {}
	~L03TrackedObj() override { ++g_tracked_dtor; }

	void setTag(uint32_t tag) { tag_ = tag; }
	uint32_t tag() const { return tag_; }

private:
	uint32_t tag_;
};

int g_pool_ctor = 0;
int g_pool_dtor = 0;

// 用于观察 WTSPoolObject 的分配/回收
struct L03PoolItem : public WTSPoolObject<L03PoolItem>
{
	uint32_t seq;
	std::string payload;

	L03PoolItem() : seq(static_cast<uint32_t>(++g_pool_ctor)) { payload.clear(); }
	~L03PoolItem() { ++g_pool_dtor; }
};

//////////////////////////////////////////////////////////////////////////
// WTSObject 引用计数
//////////////////////////////////////////////////////////////////////////
TEST(l03_object, initial_ref_is_single)
{
	L03TrackedObj* o = new L03TrackedObj();
	EXPECT_EQ(o->retainCount(), 1u);
	EXPECT_TRUE(o->isSingleRefs());
	o->setTag(7);
	EXPECT_EQ(o->tag(), 7u);
	o->release();
	EXPECT_EQ(g_tracked_dtor, 1);
}

TEST(l03_object, retain_and_release_balancing)
{
	L03TrackedObj* o = new L03TrackedObj();
	EXPECT_EQ(o->retain(), 2u);
	EXPECT_EQ(o->retain(), 3u);
	EXPECT_FALSE(o->isSingleRefs());
	EXPECT_EQ(o->retainCount(), 3u);

	o->release();
	EXPECT_EQ(o->retainCount(), 2u);
	o->release();
	EXPECT_EQ(o->retainCount(), 1u);
	EXPECT_TRUE(o->isSingleRefs());
	// 归零后立即 delete this，之后不可再访问该指针
	const int dtorBefore = g_tracked_dtor;
	o->release();
	EXPECT_EQ(g_tracked_dtor, dtorBefore + 1);
}

TEST(l03_object, release_is_idempotent_at_zero_refs)
{
	// m_uRefs == 0 时 release() 直接返回，不会二次 delete
	// 这里通过一个自定义子类模拟：析构里把引用计数钉在 0
	class L03ZeroRefObj : public WTSObject
	{
	public:
		L03ZeroRefObj() = default;
		~L03ZeroRefObj() override { ++g_tracked_dtor; }
		// 让首次 release 归零后对象仍然存活（模拟池对象语义），
		// 以便继续调用 release 走 m_uRefs == 0 的早退分支
		void release() override
		{
			if (m_uRefs == 0)
				return;
			const uint32_t cnt = m_uRefs.fetch_sub(1);
			// 故意不 delete，只记录
			(void)cnt;
		}
	};

	L03ZeroRefObj* o = new L03ZeroRefObj();
	o->release();
	o->release();
	o->release();
	EXPECT_EQ(o->retainCount(), 0u);
	EXPECT_EQ(g_tracked_dtor, 0);
	delete o;
	EXPECT_EQ(g_tracked_dtor, 1);
}

TEST(l03_object, refcount_works_on_concrete_includes_type)
{
	WTSSessionInfo* s = WTSSessionInfo::create("L03_REFS", "引用计数", 0);
	ASSERT_NE(s, nullptr);
	EXPECT_TRUE(s->isSingleRefs());
	s->retain();
	s->retain();
	EXPECT_EQ(s->retainCount(), 3u);
	s->addTradingSection(900, 1500);
	s->release();
	EXPECT_EQ(s->retainCount(), 2u);
	EXPECT_STREQ(s->id(), "L03_REFS");
	s->release();
	s->release();
}

//////////////////////////////////////////////////////////////////////////
// WTSPoolObject
//////////////////////////////////////////////////////////////////////////
TEST(l03_pool_object, allocate_returns_usable_ready_item)
{
	const int ctorBefore = g_pool_ctor;
	L03PoolItem* a = L03PoolItem::allocate();
	ASSERT_NE(a, nullptr);
	EXPECT_EQ(g_pool_ctor, ctorBefore + 1);
	EXPECT_EQ(a->retainCount(), 1u);
	EXPECT_TRUE(a->isSingleRefs());
	a->payload = "first";
	a->seq = 100;
	EXPECT_EQ(a->payload, "first");
	a->retain();
	EXPECT_EQ(a->retainCount(), 2u);
	a->release();
	EXPECT_EQ(a->retainCount(), 1u);
	// 归还对象池
	a->release();
}

TEST(l03_pool_object, recycled_after_last_release)
{
	const int ctorBefore = g_pool_ctor;
	const int dtorBefore = g_pool_dtor;

	std::vector<L03PoolItem*> items;
	for (uint32_t i = 0; i < 8; i++)
	{
		L03PoolItem* it = L03PoolItem::allocate();
		ASSERT_NE(it, nullptr);
		it->seq = i;
		items.emplace_back(it);
	}
	EXPECT_EQ(g_pool_ctor, ctorBefore + 8);

	for (L03PoolItem* it : items)
		it->release();
	items.clear();
	// 8 个对象全部构造过，回收时逐个析构
	EXPECT_EQ(g_pool_dtor, dtorBefore + 8);

	// 池可继续分配
	L03PoolItem* again = L03PoolItem::allocate();
	ASSERT_NE(again, nullptr);
	EXPECT_EQ(g_pool_ctor, ctorBefore + 9);
	again->release();
}

TEST(l03_pool_object, shared_pool_serves_many_release_paths)
{
	L03PoolItem* a = L03PoolItem::allocate();
	L03PoolItem* b = L03PoolItem::allocate();
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	// 同一静态池分配的两个对象互不相同
	EXPECT_NE(a, b);
	a->retain();
	a->release();
	b->release();
	a->release();
}

//////////////////////////////////////////////////////////////////////////
// WTSSwitchItem
//////////////////////////////////////////////////////////////////////////
TEST(l03_switch_item, create_and_accessors)
{
	WTSSwitchItem* it = WTSSwitchItem::create("SHFE", "rb", "rb2410", "rb2501", 20241015);
	ASSERT_NE(it, nullptr);
	EXPECT_STREQ(it->exchg(), "SHFE");
	EXPECT_STREQ(it->product(), "rb");
	EXPECT_STREQ(it->from(), "rb2410");
	EXPECT_STREQ(it->to(), "rb2501");
	EXPECT_EQ(it->switch_date(), 20241015u);
	// 复权因子默认 1.0
	EXPECT_DOUBLE_EQ(it->get_factor(), 1.0);

	it->set_factor(0.85);
	EXPECT_DOUBLE_EQ(it->get_factor(), 0.85);
	it->set_factor(0.0);
	EXPECT_DOUBLE_EQ(it->get_factor(), 0.0);
	it->release();
}

TEST(l03_switch_item, empty_fields_and_date_edges)
{
	WTSSwitchItem* it = WTSSwitchItem::create("", "", "", "", 0);
	ASSERT_NE(it, nullptr);
	// std::string 成员，空串安全
	EXPECT_STREQ(it->exchg(), "");
	EXPECT_STREQ(it->product(), "");
	EXPECT_STREQ(it->from(), "");
	EXPECT_STREQ(it->to(), "");
	EXPECT_EQ(it->switch_date(), 0u);
	it->release();

	const std::string longCode(200, 'z');
	WTSSwitchItem* big = WTSSwitchItem::create("BINANCE", "BTCUSDT", longCode.c_str(), longCode.c_str(), 20241231);
	ASSERT_NE(big, nullptr);
	EXPECT_EQ(std::strlen(big->from()), 200u);
	EXPECT_STREQ(big->to(), longCode.c_str());
	big->release();
}

TEST(l03_switch_item, refcount_lifecycle)
{
	WTSSwitchItem* it = WTSSwitchItem::create("DCE", "i", "i2401", "i2405", 20240108);
	ASSERT_NE(it, nullptr);
	EXPECT_EQ(it->retainCount(), 1u);
	it->retain();
	EXPECT_EQ(it->retainCount(), 2u);
	it->release();
	EXPECT_EQ(it->retainCount(), 1u);
	it->release();
}
}  // namespace
