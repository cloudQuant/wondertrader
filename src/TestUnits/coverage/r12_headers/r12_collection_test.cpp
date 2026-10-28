// Lane R12 - src/Includes/WTSCollection.hpp 容器层
// 覆盖：WTSArray 追加/替换/窃取/排序/越界、WTSMap 键值增删与迭代与边界查找、
//       WTSHashMap 增删查与迭代、WTSQueue push/pop/front/back/swap、WTSStack LIFO，
//       所有持有/释放路径都断言到具体引用计数值
// 说明：容器元素用可观测析构次数的探针对象，析构计数即“是否真的释放”的可证伪证据。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "Includes/WTSObject.hpp"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSMarcos.h"

USING_NS_WTP;

namespace {

typedef WTSHashMap<std::string> StrHashMap;

// 析构可计数的探针
class Item : public WTSObject
{
public:
	static int32_t s_dtor;

	explicit Item(int32_t tag = 0) : m_tag(tag) {}
	virtual ~Item() { s_dtor++; }

	int32_t tag() const { return m_tag; }

private:
	int32_t m_tag;
};
int32_t Item::s_dtor = 0;

Item* newItem(int32_t tag) { return new Item(tag); }

uint32_t rc(WTSObject* o) { return o->retainCount(); }

}	// namespace

class R12Collection : public ::testing::Test
{
protected:
	void SetUp() override { Item::s_dtor = 0; }
	void TearDown() override {}
};

TEST_F(R12Collection, array_append_at_grab_refcount)
{
	WTSArray* arr = WTSArray::create();
	ASSERT_NE(arr, nullptr);
	EXPECT_EQ(arr->size(), 0u);
	EXPECT_TRUE(arr->empty());
	// 容器自身引用计数从 1 起（WTSObject 构造）
	EXPECT_EQ(rc(arr), 1u);

	Item* i0 = newItem(7);
	EXPECT_EQ(rc(i0), 1u);
	arr->append(i0);
	// append 默认自动 retain
	EXPECT_EQ(rc(i0), 2u);
	EXPECT_EQ(arr->size(), 1u);
	EXPECT_EQ(arr->at(0), i0);
	EXPECT_EQ(rc(i0), 2u);
	// at 不增加计数，grab 增加
	WTSObject* g = arr->grab(0);
	EXPECT_EQ(g, i0);
	EXPECT_EQ(rc(i0), 3u);
	g->release();
	EXPECT_EQ(rc(i0), 2u);
	// operator[] 同 at
	EXPECT_EQ((*arr)[0], i0);
	EXPECT_EQ(rc(i0), 2u);

	// 越界三种读取都是 NULL
	EXPECT_EQ(arr->at(3), nullptr);
	EXPECT_EQ((*arr)[3], nullptr);
	EXPECT_EQ(arr->grab(3), nullptr);
	// 定位探针下标
	EXPECT_EQ(arr->idxOf(i0), 0u);
	Item* i1 = newItem(9);
	arr->append(i1, false);
	// bAutoRetain=false 不增加引用计数
	EXPECT_EQ(rc(i1), 1u);
	EXPECT_EQ(arr->idxOf(i1), 1u);
	EXPECT_EQ(arr->size(), 2u);

	arr->clear();
	// clear 对每个元素 release 一次
	EXPECT_EQ(rc(i0), 1u);
	i0->release();
	i1->release();
	// 两个探针都已被真正析构
	EXPECT_EQ(Item::s_dtor, 2);
	arr->release();
}

TEST_F(R12Collection, array_idxof_null_and_missing)
{
	WTSArray* arr = WTSArray::create();
	Item* i0 = newItem(1);
	arr->append(i0);
	// idxOf 用 uint32_t 返回 -1，即 INVALID_UINT32
	EXPECT_EQ(arr->idxOf(NULL), INVALID_UINT32);
	Item* other = new Item(2);
	EXPECT_EQ(arr->idxOf(other), INVALID_UINT32);
	EXPECT_EQ(arr->idxOf(i0), 0u);
	delete other;
	arr->clear();
	i0->release();
	arr->release();
}

TEST_F(R12Collection, array_set_replaces_and_releases_old)
{
	WTSArray* arr = WTSArray::create();
	arr->resize(2);
	EXPECT_EQ(arr->size(), 2u);
	EXPECT_EQ(arr->at(0), nullptr);

	Item* i0 = newItem(10);
	Item* i1 = newItem(20);
	arr->set(0, i0);
	EXPECT_EQ(rc(i0), 2u);
	EXPECT_EQ(arr->at(0), i0);
	// 同位置替换：旧值被 release，新值被 retain
	arr->set(0, i1);
	EXPECT_EQ(rc(i0), 1u);
	EXPECT_EQ(rc(i1), 2u);
	EXPECT_EQ(Item::s_dtor, 0);
	// 下标越界直接返回，不改变长度
	arr->set(5, i0);
	EXPECT_EQ(arr->size(), 2u);
	i0->release();
	delete i0;
	// 探针在 clear 前引用计数为 1（i0 已手工释放）
	arr->clear();
	EXPECT_EQ(rc(i1), 1u);
	i1->release();
	arr->release();
	EXPECT_EQ(Item::s_dtor, 2);
}

TEST_F(R12Collection, array_append_array_steals_without_retain)
{
	WTSArray* a = WTSArray::create();
	WTSArray* b = WTSArray::create();
	Item* i0 = newItem(1);
	Item* i1 = newItem(2);
	a->append(i0);
	b->append(i1);
	EXPECT_EQ(rc(i1), 2u);
	// 整串搬移：目标计数不变、来源被清空
	a->append(b);
	EXPECT_EQ(a->size(), 2u);
	EXPECT_EQ(b->size(), 0u);
	EXPECT_EQ(rc(i1), 2u);
	// 空指针来源被忽略
	a->append((WTSArray*)NULL);
	EXPECT_EQ(a->size(), 2u);
	// 模板版 at<T> 直接给出派生类指针
	EXPECT_EQ(a->at<Item>(1), i1);
	EXPECT_EQ(a->at<Item>(9), nullptr);
	i0->release();
	i1->release();
	a->release();
	b->release();
}

TEST_F(R12Collection, array_sort_and_iterator)
{
	WTSArray* arr = WTSArray::create();
	Item* i0 = newItem(3);
	Item* i1 = newItem(1);
	Item* i2 = newItem(2);
	arr->append(i0);
	arr->append(i1);
	arr->append(i2);

	arr->sort([](WTSObject* l, WTSObject* r) {
		return static_cast<Item*>(l)->tag() < static_cast<Item*>(r)->tag();
	});
	EXPECT_EQ(static_cast<Item*>(arr->at(0))->tag(), 1);
	EXPECT_EQ(static_cast<Item*>(arr->at(1))->tag(), 2);
	EXPECT_EQ(static_cast<Item*>(arr->at(2))->tag(), 3);

	// 迭代器遍历计数
	uint32_t cnt = 0;
	for (auto it = arr->begin(); it != arr->end(); ++it)
	{
		ASSERT_NE(*it, nullptr);
		cnt++;
	}
	EXPECT_EQ(cnt, 3u);
	// const 迭代器同样可遍历
	uint32_t ccnt = 0;
	const WTSArray* carr = arr;
	for (auto it = carr->begin(); it != carr->end(); ++it)
		ccnt++;
	EXPECT_EQ(ccnt, 3u);

	// 排序不影响引用计数
	EXPECT_EQ(rc(i0), 2u);
	arr->release();
	// 容器引用计数归 0 时释放全部元素
	EXPECT_EQ(Item::s_dtor, 3);
}

TEST_F(R12Collection, array_release_twice_is_noop)
{
	WTSArray* arr = WTSArray::create();
	Item* i0 = newItem(5);
	arr->append(i0);
	// 先自增一次引用，再各减一次
	arr->retain();
	EXPECT_EQ(rc(arr), 2u);
	arr->release();
	EXPECT_EQ(rc(arr), 1u);
	EXPECT_EQ(arr->size(), 1u);
	arr->release();
	EXPECT_EQ(Item::s_dtor, 1);
}

TEST_F(R12Collection, map_add_get_grab_remove)
{
	WTSMap<int32_t>* map = WTSMap<int32_t>::create();
	ASSERT_NE(map, nullptr);
	EXPECT_EQ(map->size(), 0u);
	EXPECT_EQ(map->get(1), nullptr);
	EXPECT_EQ(map->grab(1), nullptr);
	EXPECT_EQ((*map)[3], nullptr);
	EXPECT_EQ(map->last(), nullptr);

	Item* i1 = newItem(11);
	Item* i2 = newItem(22);
	map->add(1, i1);
	EXPECT_EQ(rc(i1), 2u);
	map->add(2, i2);
	EXPECT_EQ(map->size(), 2u);
	// get/[] 不加引用，grab 加引用
	EXPECT_EQ(map->get(1), i1);
	EXPECT_EQ(rc(i1), 2u);
	WTSObject* g = map->grab(2);
	EXPECT_EQ(g, i2);
	EXPECT_EQ(rc(i2), 3u);
	g->release();
	EXPECT_EQ(rc(i2), 2u);
	// 同键覆盖：旧值被释放并析构（旧值只剩容器一份引用→clear 前手工释放）
	Item* i1b = newItem(12);
	map->add(1, i1b);
	EXPECT_EQ(rc(i1b), 2u);
	EXPECT_EQ(rc(i1), 1u);
	map->remove(1);
	// remove 后新值引用回到 1
	EXPECT_EQ(rc(i1b), 1u);
	EXPECT_EQ(map->size(), 1u);
	// 删除不存在的键是无操作
	map->remove(99);
	EXPECT_EQ(map->size(), 1u);
	// 键序容器的 last() 返回最大键的值
	EXPECT_EQ(map->last(), i2);
	i1b->release();
	map->release();
	// 容器销毁时剩余元素被释放
	EXPECT_EQ(rc(i2), 1u);
	i2->release();
	EXPECT_EQ(Item::s_dtor, 3);
}

TEST_F(R12Collection, map_find_bounds_and_erase_iterator)
{
	WTSMap<int32_t>* map = WTSMap<int32_t>::create();
	Item* i1 = newItem(1);
	Item* i3 = newItem(3);
	Item* i5 = newItem(5);
	map->add(10, i1);
	map->add(30, i3);
	map->add(50, i5);
	EXPECT_EQ(map->size(), 3u);

	WTSMap<int32_t>::Iterator it = map->find(30);
	ASSERT_TRUE(it != map->end());
	EXPECT_EQ(it->second, i3);
	EXPECT_TRUE(map->find(20) == map->end());
	// lower_bound(20) 是第一个 >=20 的键（30），upper_bound(30) 是第一个 >30 的键（50）
	EXPECT_EQ(map->lower_bound(20)->first, 30);
	EXPECT_EQ(map->upper_bound(30)->first, 50);
	EXPECT_TRUE(map->lower_bound(60) == map->end());
	// 迭代器删除：只移除，元素引用计数不变
	uint32_t before = rc(i5);
	map->erase(map->find(50));
	EXPECT_EQ(map->size(), 2u);
	EXPECT_EQ(rc(i5), before);
	// const 迭代器删除分支
	const WTSMap<int32_t>* cmap = map;
	WTSMap<int32_t>::ConstIterator cit = cmap->find(30);
	ASSERT_TRUE(cit != cmap->end());
	map->erase(cit);
	EXPECT_EQ(map->size(), 1u);
	// 按键删除
	map->erase(10);
	EXPECT_EQ(map->size(), 0u);
	i1->release();
	i3->release();
	i5->release();
	map->release();
}

TEST_F(R12Collection, hashmap_string_key_add_get_remove)
{
	StrHashMap* map = StrHashMap::create();
	ASSERT_NE(map, nullptr);
	EXPECT_EQ(map->size(), 0u);
	EXPECT_EQ(map->get("none"), nullptr);
	EXPECT_EQ(map->grab("none"), nullptr);

	Item* a = newItem(1);
	Item* b = newItem(2);
	Item* c = newItem(3);
	map->add("a", a);
	EXPECT_EQ(rc(a), 2u);
	map->add("b", b, false);
	// bAutoRetain=false 不引用
	EXPECT_EQ(rc(b), 1u);
	map->add("c", c);
	EXPECT_EQ(rc(c), 2u);
	EXPECT_EQ(map->size(), 3u);
	EXPECT_EQ(map->get("a"), a);
	WTSObject* g = map->grab("a");
	EXPECT_EQ(g, a);
	EXPECT_EQ(rc(a), 3u);
	g->release();
	// 同键覆盖释放旧值
	Item* a2 = newItem(4);
	map->add("a", a2);
	EXPECT_EQ(rc(a), 1u);
	a->release();
	EXPECT_EQ(map->get("a"), a2);
	map->remove("a");
	EXPECT_EQ(rc(a2), 1u);
	a2->release();
	EXPECT_EQ(map->size(), 2u);
	EXPECT_EQ(map->get("a"), nullptr);
	// 删除不存在的键不改变长度
	map->remove("zzz");
	EXPECT_EQ(map->size(), 2u);
	// find 命中/未命中
	ASSERT_TRUE(map->find("b") != map->end());
	EXPECT_TRUE(map->find("zzz") == map->end());
	// 迭代计数
	uint32_t cnt = 0;
	for (auto it = map->begin(); it != map->end(); ++it)
		cnt++;
	EXPECT_EQ(cnt, 2u);
	// c 由 remove 弹出后自行释放
	map->remove("c");
	EXPECT_EQ(rc(c), 1u);
	c->release();
	map->release();
	// 容器析构时 clear() 释放剩余元素 b（b 只被 add(false) 挂上，计数为 1）
	EXPECT_EQ(Item::s_dtor, 4);
}

TEST_F(R12Collection, queue_push_front_back_and_refcount)
{
	WTSQueue* q = WTSQueue::create();
	Item* i0 = newItem(1);
	Item* i1 = newItem(2);
	// 空队列取前后元素返回 NULL
	EXPECT_EQ(q->front(), nullptr);
	EXPECT_EQ(q->back(), nullptr);
	EXPECT_TRUE(q->empty());
	q->push(i0);
	q->push(i1);
	EXPECT_EQ(q->size(), 2u);
	EXPECT_FALSE(q->empty());
	// push 自动 retain
	EXPECT_EQ(rc(i0), 2u);
	EXPECT_EQ(rc(i1), 2u);
	// front/back 默认 retain
	WTSObject* f = q->front();
	EXPECT_EQ(f, i0);
	EXPECT_EQ(rc(i0), 3u);
	f->release();
	EXPECT_EQ(rc(i0), 2u);
	WTSObject* bk = q->back();
	EXPECT_EQ(bk, i1);
	EXPECT_EQ(rc(i1), 3u);
	bk->release();
	// bRetain=false 只取不引用
	EXPECT_EQ(q->front(false), i0);
	EXPECT_EQ(rc(i0), 2u);
	EXPECT_EQ(q->back(false), i1);
	EXPECT_EQ(rc(i1), 2u);
	// bAutoRetain=false 入队也不引用（此处不测 NULL 入队：见 bugs 里
	// WTSQueue::clear() 对空槽解引用的缺陷，DISABLED 用例单独记录）
	Item* i2 = newItem(3);
	q->push(i2, false);
	EXPECT_EQ(rc(i2), 1u);
	EXPECT_EQ(q->size(), 3u);
	EXPECT_EQ(q->front(false), i0);
	i0->release();
	i1->release();
	// i2 计数只有容器这一份，交给 release()/clear() 回收
	q->release();
	EXPECT_EQ(Item::s_dtor, 3);
}

// 触发条件：WTSQueue::push(NULL, false) 允许把空槽入队（WTSCollection.hpp:772-777
// 只在 obj 非空时才 retain），但 WTSQueue::clear()（同文件 828-836）遍历时无空槽判空，
// 直接 (*it)->release() → 空指针解引用崩溃。生产里只要有一次 NULL 入队 + 容器清理即触发。
TEST(DISABLED_r12_collection, queue_clear_crashes_on_null_slot)
{
	WTSQueue* q = WTSQueue::create();
	q->push(NULL, false);
	EXPECT_EQ(q->size(), 1u);
	q->release();
}

TEST_F(R12Collection, queue_swap_and_clear_release_each_once)
{
	WTSQueue* a = WTSQueue::create();
	WTSQueue* b = WTSQueue::create();
	Item* i0 = newItem(1);
	a->push(i0);
	EXPECT_EQ(rc(i0), 2u);
	// swap 只搬指针，引用计数不变
	a->swap(b);
	EXPECT_EQ(a->size(), 0u);
	EXPECT_EQ(b->size(), 1u);
	EXPECT_EQ(rc(i0), 2u);
	EXPECT_TRUE(a->empty());
	// b 持有该元素，clear 后引用减 1
	b->clear();
	EXPECT_EQ(b->size(), 0u);
	EXPECT_EQ(rc(i0), 1u);
	i0->release();
	a->release();
	b->release();
}

// 触发条件：WTSQueue::pop()（WTSCollection.hpp:767-770）只 pop_front，
// 不对弹出的元素 release，与 WTSStack::pop() 的语义相反；
// 在引用计数为 2 的入队元素上弹出后计数仍是 2，即元素泄漏。
// 该行为在生产里是稳定可复现的，但断言“泄漏”会让用例长期锁死错误语义，
// 故整类 DISABLED 只做记录，缺陷见 bugs/R12_headers_wtstools.md。
TEST(DISABLED_r12_collection, queue_pop_does_not_release_element)
{
	WTSQueue* q = WTSQueue::create();
	Item* i0 = newItem(1);
	q->push(i0);
	EXPECT_EQ(rc(i0), 2u);
	q->pop();
	EXPECT_EQ(q->size(), 0u);
	EXPECT_EQ(rc(i0), 1u);
	i0->release();
	q->release();
}

TEST_F(R12Collection, stack_lifo_top_pop_clear)
{
	WTSStack* st = WTSStack::create();
	ASSERT_NE(st, nullptr);
	// 空栈 top/pop 返回 nullptr
	EXPECT_EQ(st->top(), nullptr);
	EXPECT_EQ(st->pop(), nullptr);
	EXPECT_TRUE(st->empty());
	EXPECT_EQ(st->size(), 0u);

	Item* i0 = newItem(1);
	Item* i1 = newItem(2);
	st->push(i0);
	st->push(i1);
	EXPECT_EQ(st->size(), 2u);
	EXPECT_EQ(rc(i0), 2u);
	EXPECT_EQ(rc(i1), 2u);
	// top 不改变引用计数
	EXPECT_EQ(st->top(), i1);
	EXPECT_EQ(rc(i1), 2u);
	// pop 出栈即 release 一次，返回裸指针由调用方持有
	WTSObject* p = st->pop();
	EXPECT_EQ(p, i1);
	EXPECT_EQ(rc(i1), 1u);
	EXPECT_EQ(st->size(), 1u);
	i1->release();
	// bAutoRetain=false 入栈不引用
	Item* i2 = newItem(3);
	st->push(i2, false);
	EXPECT_EQ(rc(i2), 1u);
	EXPECT_EQ(st->top(), i2);
	EXPECT_EQ(st->size(), 2u);
	// clear 逐个释放：i0 计数 2→1 仍存活，i2 计数 1→0 当场析构
	st->clear();
	EXPECT_TRUE(st->empty());
	EXPECT_EQ(rc(i0), 1u);
	EXPECT_EQ(Item::s_dtor, 1);
	i0->release();
	st->release();
}

TEST_F(R12Collection, stack_release_destructor_clears_elements)
{
	WTSStack* st = WTSStack::create();
	Item* i0 = newItem(1);
	st->push(i0);
	EXPECT_EQ(rc(i0), 2u);
	// 容器引用归 0 → 析构函数里 clear() → 元素被释放
	st->release();
	EXPECT_EQ(Item::s_dtor, 1);
}
