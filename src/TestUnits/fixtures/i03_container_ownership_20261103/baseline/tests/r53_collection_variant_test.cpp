// Lane R53 - src/Includes/WTSCollection.hpp + src/Includes/WTSVariant.hpp
// 覆盖：WTSArray resize/排序/迭代器/idxOf/grab、WTSMap 边界与 last()、
//       WTSHashMap find/remove/clear、WTSQueue front/back 与迭代器、WTSStack 空栈路径；
//       WTSVariant Int64/UInt64、容器与缺失键转换、嵌套子对象/数组、release 计数。
// 说明：全部为内存容器，不读写外部资源；WTSMap::erase 不释放元素的缺陷用 DISABLED 记录。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Includes/WTSCollection.hpp"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSMarcos.h"

using namespace wtp;

namespace
{
class R53TagObj : public WTSObject
{
public:
	explicit R53TagObj(int tag) : _tag(tag) {}
	int tag() const { return _tag; }

private:
	int _tag;
};

bool has_name(const std::vector<std::string>& names, const char* want)
{
	for (const std::string& n : names)
	{
		if (n == want)
			return true;
	}
	return false;
}
}  // namespace

// WTSCollection.hpp:58-210、242-296：resize 先释放再填 NULL、排序、正向/反向迭代器
TEST(r53_array, resize_sort_and_iterators)
{
	WTSArray* arr = WTSArray::create();
	ASSERT_NE(arr, nullptr);
	EXPECT_TRUE(arr->empty());
	EXPECT_EQ(arr->size(), 0u);

	R53TagObj* a = new R53TagObj(1);
	R53TagObj* b = new R53TagObj(2);
	R53TagObj* c = new R53TagObj(3);
	arr->append(a);
	arr->append(b);
	EXPECT_EQ(a->retainCount(), 2u);
	EXPECT_EQ(arr->size(), 2u);

	// resize 先 clear（逐个 release）再按新长度填 NULL
	arr->resize(4);
	EXPECT_EQ(a->retainCount(), 1u);
	EXPECT_EQ(b->retainCount(), 1u);
	EXPECT_EQ(arr->size(), 4u);
	EXPECT_EQ(arr->at(0), nullptr);
	EXPECT_EQ(arr->at(3), nullptr);
	EXPECT_EQ(arr->at(99), nullptr);
	arr->resize(0);
	EXPECT_TRUE(arr->empty());

	// 重新追加后按 tag 降序排序
	arr->append(a);
	arr->append(b);
	arr->append(c);
	arr->sort([](WTSObject* l, WTSObject* r) {
		return static_cast<R53TagObj*>(l)->tag() > static_cast<R53TagObj*>(r)->tag();
	});
	ASSERT_EQ(arr->size(), 3u);
	EXPECT_EQ(arr->at<R53TagObj>(0)->tag(), 3);
	EXPECT_EQ(arr->at<R53TagObj>(1)->tag(), 2);
	EXPECT_EQ(arr->at<R53TagObj>(2)->tag(), 1);
	EXPECT_EQ(arr->operator[](0), c);

	// idxOf：NULL 与不在容器内的对象都返回 INVALID_UINT32
	EXPECT_EQ(arr->idxOf(nullptr), INVALID_UINT32);
	R53TagObj* missing = new R53TagObj(9);
	EXPECT_EQ(arr->idxOf(missing), INVALID_UINT32);
	EXPECT_EQ(arr->idxOf(c), 0u);
	missing->release();

	// grab 增加引用计数，at/operator[] 不增加
	WTSObject* grabbed = arr->grab(0);
	ASSERT_EQ(grabbed, c);
	EXPECT_EQ(c->retainCount(), 3u);
	grabbed->release();
	EXPECT_EQ(c->retainCount(), 2u);

	// 正向/反向、const 迭代器
	uint32_t forward = 0;
	for (WTSArray::Iterator it = arr->begin(); it != arr->end(); ++it)
		forward++;
	EXPECT_EQ(forward, 3u);
	uint32_t reverse = 0;
	for (WTSArray::ReverseIterator it = arr->rbegin(); it != arr->rend(); ++it)
		reverse++;
	EXPECT_EQ(reverse, 3u);

	const WTSArray* carr = arr;
	uint32_t cforward = 0;
	for (WTSArray::ConstIterator it = carr->begin(); it != carr->end(); ++it)
		cforward++;
	EXPECT_EQ(cforward, 3u);
	uint32_t creverse = 0;
	for (WTSArray::ConstReverseIterator it = carr->rbegin(); it != carr->rend(); ++it)
		creverse++;
	EXPECT_EQ(creverse, 3u);
	EXPECT_EQ(static_cast<R53TagObj*>(*carr->rbegin())->tag(), 1);
	EXPECT_EQ(arr->at(2), a);

	arr->release();
	a->release();
	b->release();
	c->release();
}

// WTSCollection.hpp:330-585：WTSMap 边界查询、last()、const 迭代器
TEST(r53_map, bounds_last_and_const_iterators)
{
	WTSMap<int>* map = WTSMap<int>::create();
	ASSERT_NE(map, nullptr);
	EXPECT_EQ(map->size(), 0u);
	EXPECT_EQ(map->last(), nullptr);

	R53TagObj* o1 = new R53TagObj(1);
	R53TagObj* o2 = new R53TagObj(2);
	R53TagObj* o3 = new R53TagObj(3);
	map->add(2, o2);
	map->add(1, o1);
	map->add(3, o3);
	EXPECT_EQ(map->size(), 3u);
	EXPECT_EQ(map->get(2), o2);
	EXPECT_EQ(map->operator[](3), o3);
	EXPECT_EQ(map->get(9), nullptr);
	EXPECT_EQ(map->grab(9), nullptr);
	EXPECT_EQ(static_cast<R53TagObj*>(map->last())->tag(), 3);

	ASSERT_NE(map->lower_bound(2), map->end());
	EXPECT_EQ(map->lower_bound(2)->first, 2);
	ASSERT_NE(map->upper_bound(2), map->end());
	EXPECT_EQ(map->upper_bound(2)->first, 3);
	EXPECT_EQ(map->find(4), map->end());

	const WTSMap<int>* cmap = map;
	EXPECT_EQ(cmap->size(), 3u);
	ASSERT_NE(cmap->lower_bound(1), cmap->end());
	EXPECT_EQ(cmap->lower_bound(1)->first, 1);
	ASSERT_NE(cmap->upper_bound(1), cmap->end());
	EXPECT_EQ(cmap->upper_bound(1)->first, 2);
	EXPECT_NE(cmap->find(3), cmap->end());
	EXPECT_EQ(cmap->find(8), cmap->end());
	uint32_t citems = 0;
	for (WTSMap<int>::ConstIterator it = cmap->begin(); it != cmap->end(); ++it)
		citems++;
	EXPECT_EQ(citems, 3u);
	uint32_t crev = 0;
	for (WTSMap<int>::ConstReverseIterator it = cmap->rbegin(); it != cmap->rend(); ++it)
		crev++;
	EXPECT_EQ(crev, 3u);

	// remove 释放一次；随后 clear 释放剩余两个
	map->remove(2);
	EXPECT_EQ(o2->retainCount(), 1u);
	EXPECT_EQ(map->size(), 2u);
	map->clear();
	EXPECT_EQ(map->size(), 0u);
	EXPECT_EQ(o1->retainCount(), 1u);
	EXPECT_EQ(o3->retainCount(), 1u);
	map->release();
	o1->release();
	o2->release();
	o3->release();
}

// WTSCollection.hpp:614-750、761-938：HashMap find/remove/clear、Queue front/back、
// Stack 空栈 top/pop 与 clear
TEST(r53_containers, hashmap_queue_stack_lifecycle)
{
	WTSHashMap<std::string>* hm = WTSHashMap<std::string>::create();
	ASSERT_NE(hm, nullptr);
	R53TagObj* a = new R53TagObj(1);
	R53TagObj* b = new R53TagObj(2);
	R53TagObj* c = new R53TagObj(3);
	hm->add("a", a);
	hm->add("b", b);
	hm->add("c", c);
	EXPECT_EQ(hm->size(), 3u);
	EXPECT_EQ(hm->get("a"), a);
	EXPECT_EQ(hm->get("zz"), nullptr);
	WTSObject* grabbed = hm->grab("a");
	ASSERT_EQ(grabbed, a);
	EXPECT_EQ(a->retainCount(), 3u);
	grabbed->release();

	const WTSHashMap<std::string>* chm = hm;
	EXPECT_NE(chm->find("b"), chm->end());
	EXPECT_EQ(chm->find("zz"), chm->end());
	hm->remove("b");
	EXPECT_EQ(b->retainCount(), 1u);
	EXPECT_EQ(hm->size(), 2u);
	hm->clear();
	EXPECT_EQ(hm->size(), 0u);
	EXPECT_EQ(a->retainCount(), 1u);
	EXPECT_EQ(c->retainCount(), 1u);
	hm->release();

	WTSQueue* q = WTSQueue::create();
	ASSERT_NE(q, nullptr);
	EXPECT_TRUE(q->empty());
	EXPECT_EQ(q->front(), nullptr);
	EXPECT_EQ(q->back(), nullptr);
	q->push(a);
	q->push(b);
	EXPECT_EQ(q->size(), 2u);
	EXPECT_EQ(q->front(false), a);
	EXPECT_EQ(q->back(false), b);
	EXPECT_EQ(a->retainCount(), 2u);
	WTSObject* qf = q->front();
	ASSERT_EQ(qf, a);
	EXPECT_EQ(a->retainCount(), 3u);
	qf->release();
	WTSObject* qb = q->back();
	ASSERT_EQ(qb, b);
	EXPECT_EQ(b->retainCount(), 3u);
	qb->release();
	uint32_t qitems = 0;
	for (WTSQueue::Iterator it = q->begin(); it != q->end(); ++it)
		qitems++;
	EXPECT_EQ(qitems, 2u);
	const WTSQueue* cq = q;
	uint32_t cqitems = 0;
	for (WTSQueue::ConstIterator it = cq->begin(); it != cq->end(); ++it)
		cqitems++;
	EXPECT_EQ(cqitems, 2u);
	q->clear();
	EXPECT_EQ(a->retainCount(), 1u);
	EXPECT_EQ(b->retainCount(), 1u);
	EXPECT_TRUE(q->empty());
	q->release();

	WTSStack* stack = WTSStack::create();
	ASSERT_NE(stack, nullptr);
	EXPECT_TRUE(stack->empty());
	EXPECT_EQ(stack->top(), nullptr);
	EXPECT_EQ(stack->pop(), nullptr);
	stack->push(a);
	EXPECT_EQ(stack->size(), 1u);
	EXPECT_EQ(stack->top(), a);
	EXPECT_EQ(stack->pop(), a);
	EXPECT_TRUE(stack->empty());
	EXPECT_EQ(a->retainCount(), 1u);
	stack->push(b);
	stack->clear();
	EXPECT_EQ(b->retainCount(), 1u);
	EXPECT_TRUE(stack->empty());
	stack->release();

	a->release();
	b->release();
	c->release();
}

// WTSVariant.hpp:80-94、165-235、292-326：Int64/UInt64 与缺失键默认值
TEST(r53_variant, int64_uint64_and_scalar_getters)
{
	WTSVariant* v = WTSVariant::createObject();
	ASSERT_NE(v, nullptr);
	EXPECT_FALSE(v->has("missing"));
	EXPECT_TRUE(v->append("i64", (int64_t)-1234567890123LL));
	EXPECT_TRUE(v->append("u64", (uint64_t)12345678901234ULL));
	EXPECT_TRUE(v->append("i32", (int32_t)-7));
	EXPECT_TRUE(v->append("u32", (uint32_t)7));
	EXPECT_TRUE(v->append("real", 1.25));
	EXPECT_TRUE(v->append("flag", true));
	EXPECT_TRUE(v->append("text", "hello"));

	EXPECT_EQ(v->size(), 7u);
	EXPECT_TRUE(v->has("i64"));
	EXPECT_EQ(v->getInt64("i64"), -1234567890123LL);
	EXPECT_EQ(v->getUInt64("u64"), 12345678901234ULL);
	EXPECT_EQ(v->getInt32("i32"), -7);
	EXPECT_EQ(v->getUInt32("u32"), 7u);
	EXPECT_DOUBLE_EQ(v->getDouble("real"), 1.25);
	EXPECT_TRUE(v->getBoolean("flag"));
	EXPECT_STREQ(v->getCString("text"), "hello");
	EXPECT_EQ(v->getString("text"), "hello");

	EXPECT_EQ(v->getInt64("missing", -9), -9);
	EXPECT_EQ(v->getUInt64("missing", 9u), 9u);
	EXPECT_STREQ(v->getCString("missing", "dft"), "dft");
	EXPECT_EQ(v->getString("missing", "dft"), "dft");
	EXPECT_FALSE(v->getBoolean("missing", false));

	EXPECT_EQ(v->type(), WTSVariant::VT_Object);
	EXPECT_TRUE(v->isObject());
	EXPECT_FALSE(v->isArray());
	EXPECT_EQ(v->get(9u), nullptr);
	// 容器类型走 as* 的 default 分支
	EXPECT_EQ(v->asInt32(), 0);
	EXPECT_EQ(v->asInt64(), 0);
	EXPECT_EQ(v->asUInt64(), 0u);
	EXPECT_DOUBLE_EQ(v->asDouble(), 0.0);
	EXPECT_EQ(v->asString(), "");
	EXPECT_STREQ(v->asCString(), "");

	WTSVariant* arr = WTSVariant::createArray();
	ASSERT_NE(arr, nullptr);
	EXPECT_EQ(arr->type(), WTSVariant::VT_Array);
	EXPECT_TRUE(arr->isArray());
	EXPECT_FALSE(arr->isObject());
	EXPECT_EQ(arr->size(), 0u);
	EXPECT_EQ(arr->get(0u), nullptr);
	EXPECT_FALSE(arr->has("x"));
	// 对象型 append 在数组上必须早退
	EXPECT_FALSE(arr->append("oops", 1));
	EXPECT_TRUE(arr->append((int64_t)-5));
	EXPECT_TRUE(arr->append((uint64_t)6));
	EXPECT_TRUE(arr->append(2.5));
	EXPECT_TRUE(arr->append(false));
	EXPECT_TRUE(arr->append("s"));
	EXPECT_EQ(arr->size(), 5u);
	EXPECT_EQ(arr->get(0u)->asInt64(), -5);
	EXPECT_EQ(arr->get(1u)->asUInt64(), 6u);
	EXPECT_DOUBLE_EQ(arr->get(2u)->asDouble(), 2.5);
	EXPECT_FALSE(arr->get(3u)->asBoolean());
	EXPECT_STREQ(arr->get(4u)->asCString(), "s");
	EXPECT_EQ(arr->get(5u), nullptr);
	arr->release();
	v->release();
}

// WTSVariant.hpp:137-151、519-532、653-696：嵌套对象/数组、memberNames、递归 release
TEST(r53_variant, nested_children_and_member_names)
{
	WTSVariant* root = WTSVariant::createObject();
	WTSVariant* child = WTSVariant::createObject();
	WTSVariant* list = WTSVariant::createArray();
	ASSERT_NE(root, nullptr);
	ASSERT_NE(child, nullptr);
	ASSERT_NE(list, nullptr);

	EXPECT_TRUE(child->append("k", "v"));
	EXPECT_TRUE(list->append("x"));
	EXPECT_TRUE(list->append((int32_t)5));
	// bAutoRetain=false：所有权转给父容器
	EXPECT_TRUE(root->append("child", child, false));
	EXPECT_TRUE(root->append("list", list, false));
	EXPECT_FALSE(root->append("null", (WTSVariant*)nullptr));

	EXPECT_EQ(root->size(), 2u);
	EXPECT_EQ(root->get("child"), child);
	EXPECT_EQ(root->get(std::string("list")), list);
	EXPECT_STREQ(root->getCString("child"), "");	// 容器不是字符串
	const WTSVariant::MemberNames names = root->memberNames();
	ASSERT_EQ(names.size(), 2u);
	EXPECT_TRUE(has_name(names, "child"));
	EXPECT_TRUE(has_name(names, "list"));

	EXPECT_EQ(child->getString("k"), "v");
	EXPECT_EQ(list->size(), 2u);
	EXPECT_STREQ(list->get(0u)->asCString(), "x");
	EXPECT_EQ(list->get(1u)->asInt32(), 5);
	EXPECT_EQ(list->get(2u), nullptr);
	root->release();	// child/list 引用计数归零由父容器释放
}

// WTSCollection.hpp:494-510（BUG-R53-002）：erase(key) 只从 std::map 摘除，不 release。
// 期望：erase 与 remove 一样把元素引用计数减一；实际：引用计数保持 2，元素泄漏。
TEST(DISABLED_r53_map, erase_by_key_does_not_release_element)
{
	WTSMap<int>* map = WTSMap<int>::create();
	ASSERT_NE(map, nullptr);
	R53TagObj* obj = new R53TagObj(7);
	map->add(1, obj, true);
	ASSERT_EQ(obj->retainCount(), 2u);

	map->erase(1);
	EXPECT_EQ(map->size(), 0u);
	EXPECT_EQ(obj->retainCount(), 1u);
	map->release();
	obj->release();
}
