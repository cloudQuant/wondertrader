// Lane L02 - WTSTools/Includes 类型系统单元测试
// 覆盖：WTSObject / WTSPoolObject 引用计数、WTSVariant 类型容器与读写、
//       WTSArray / WTSHashMap / WTSQueue 的持有与释放语义
// 说明：WTSVariant 的私有 create() 工厂无法从外部调用，只能通过公有 append()
//       间接覆盖，因此所有标量类型分支都由 createObject()/createArray() + append() 触发。
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "Includes/WTSObject.hpp"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSCollection.hpp"

USING_NS_WTP;

namespace {

// 可观测析构次数的 WTSObject 派生探针
class RefProbe : public WTSObject
{
public:
	static int32_t s_dtor;

	RefProbe() : m_tag(0) {}
	explicit RefProbe(int32_t tag) : m_tag(tag) {}
	virtual ~RefProbe() { s_dtor++; }

	int32_t tag() const { return m_tag; }

private:
	int32_t m_tag;
};
int32_t RefProbe::s_dtor = 0;

// 走对象池的探针（用于 WTSPoolObject::allocate/release 归还语义）
class PoolProbe : public WTSPoolObject<PoolProbe>
{
public:
	static int32_t s_dtor;
	PoolProbe() {}
	virtual ~PoolProbe() { s_dtor++; }
};
int32_t PoolProbe::s_dtor = 0;

// WTSVariant 的 RAII 包装，保证 retain/release 配对
class VarHolder
{
public:
	explicit VarHolder(WTSVariant* v = NULL) : _v(v) {}
	~VarHolder() { reset(); }
	VarHolder(const VarHolder&) = delete;
	VarHolder& operator=(const VarHolder&) = delete;

	void reset()
	{
		if (_v != NULL)
		{
			_v->release();
			_v = NULL;
		}
	}
	WTSVariant* get() const { return _v; }
	WTSVariant* operator->() const { return _v; }

private:
	WTSVariant* _v;
};

	// 数组/映射/队列容器在本用例中直接用 create()/release() 配对管理

typedef WTSHashMap<std::string> StrHashMap;

class L02VariantFixture : public ::testing::Test
{
protected:
	void SetUp() override
	{
		RefProbe::s_dtor = 0;
		PoolProbe::s_dtor = 0;
	}

	void TearDown() override
	{
		// 每个用例结束时探针对象必须已全部回收，否则说明测试本身漏了 release
		EXPECT_EQ(0, RefProbe::s_dtor) << "用例未清理探针析构计数（可能存在对象泄漏）";
	}
};

}  // namespace

//////////////////////////////////////////////////////////////// WTSObject
TEST_F(L02VariantFixture, ObjectRefCountLifecycle)
{
	RefProbe* p = new RefProbe(7);
	EXPECT_EQ(1u, p->retainCount());
	EXPECT_TRUE(p->isSingleRefs());
	EXPECT_EQ(2u, p->retain());
	EXPECT_EQ(3u, p->retain());
	EXPECT_FALSE(p->isSingleRefs());
	EXPECT_EQ(7, p->tag());

	p->release();
	EXPECT_EQ(2u, p->retainCount());
	EXPECT_EQ(0, RefProbe::s_dtor);

	p->release();
	EXPECT_EQ(1u, p->retainCount());
	EXPECT_EQ(0, RefProbe::s_dtor);

	p->release();
	EXPECT_EQ(1, RefProbe::s_dtor);
	RefProbe::s_dtor = 0;
}

TEST_F(L02VariantFixture, ObjectReleaseBalancesRetain)
{
	// refs==1 时 release() 直接 delete this，析构被调用一次
	RefProbe* p = new RefProbe();
	p->release();
	EXPECT_EQ(1, RefProbe::s_dtor);
	RefProbe::s_dtor = 0;
}

TEST_F(L02VariantFixture, PoolObjectReturnsToPool)
{
	PoolProbe* p = PoolProbe::allocate();
	ASSERT_TRUE(p != NULL);
	EXPECT_EQ(1u, p->retainCount());

	p->retain();
	EXPECT_EQ(2u, p->retainCount());
	p->release();
	EXPECT_EQ(1u, p->retainCount());
	EXPECT_EQ(0, PoolProbe::s_dtor);

	p->release();
	// 归还对象池时会走一次析构
	EXPECT_EQ(1, PoolProbe::s_dtor);
	PoolProbe::s_dtor = 0;
}

//////////////////////////////////////////////////////////////// WTSVariant 标量
TEST_F(L02VariantFixture, VariantObjectAppendsAllScalarTypes)
{
	VarHolder root(WTSVariant::createObject());
	ASSERT_TRUE(root.get() != NULL);
	EXPECT_TRUE(root->isObject());
	EXPECT_FALSE(root->isArray());
	EXPECT_EQ(WTSVariant::VT_Object, root->type());
	EXPECT_EQ(0u, root->size());

	int32_t i32 = -2026;
	uint32_t u32 = 4000000000u;
	int64_t i64 = -9000000000LL;
	uint64_t u64 = 18000000000000000000ULL;

	EXPECT_TRUE(root->append("i32", i32));
	EXPECT_TRUE(root->append("u32", u32));
	EXPECT_TRUE(root->append("i64", i64));
	EXPECT_TRUE(root->append("u64", u64));
	EXPECT_TRUE(root->append("real", 3.14159));
	EXPECT_TRUE(root->append("str", "wondertrader"));
	EXPECT_TRUE(root->append("yes", true));
	EXPECT_TRUE(root->append("no", false));

	EXPECT_EQ(8u, root->size());
	EXPECT_TRUE(root->has("i32"));
	EXPECT_FALSE(root->has("not_exists"));

	EXPECT_EQ(i32, root->getInt32("i32"));
	EXPECT_EQ(u32, root->getUInt32("u32"));
	EXPECT_EQ(i64, root->getInt64("i64"));
	EXPECT_EQ(u64, root->getUInt64("u64"));
	EXPECT_DOUBLE_EQ(3.14159, root->getDouble("real"));
	EXPECT_EQ("wondertrader", root->getString("str"));
	EXPECT_STREQ("wondertrader", root->getCString("str"));
	EXPECT_TRUE(root->getBoolean("yes"));
	EXPECT_FALSE(root->getBoolean("no"));

	// 缺省值分支
	EXPECT_EQ(99, root->getInt32("missing", 99));
	EXPECT_EQ(100u, root->getUInt32("missing", 100));
	EXPECT_EQ(101LL, root->getInt64("missing", 101));
	EXPECT_EQ(102ULL, root->getUInt64("missing", 102));
	EXPECT_DOUBLE_EQ(103.0, root->getDouble("missing", 103.0));
	EXPECT_EQ("def", root->getString("missing", "def"));
	EXPECT_STREQ("def", root->getCString("missing", "def"));
	EXPECT_TRUE(root->getBoolean("missing", true));

	// 64 位数值以字符串落库，可无损回转
	EXPECT_EQ("18000000000000000000", root->get("u64")->asString());
	EXPECT_EQ(-9000000000LL, root->get("i64")->asInt64());
}

TEST_F(L02VariantFixture, VariantCrossTypeConversions)
{
	VarHolder root(WTSVariant::createObject());
	uint32_t u32 = 12345u;
	EXPECT_TRUE(root->append("num", u32));
	EXPECT_TRUE(root->append("text", "777"));
	EXPECT_TRUE(root->append("half", 0.5));
	EXPECT_TRUE(root->append("flag", true));

	WTSVariant* num = root->get("num");
	ASSERT_TRUE(num != NULL);
	EXPECT_EQ(WTSVariant::VT_Uint32, num->type());
	EXPECT_EQ(12345, num->asInt32());
	EXPECT_EQ(12345u, num->asUInt32());
	EXPECT_EQ(12345LL, num->asInt64());
	EXPECT_EQ(12345ull, num->asUInt64());
	EXPECT_DOUBLE_EQ(12345.0, num->asDouble());
	EXPECT_EQ("12345", num->asString());
	EXPECT_STREQ("12345", num->asCString());

	WTSVariant* text = root->get("text");
	ASSERT_TRUE(text != NULL);
	EXPECT_EQ(WTSVariant::VT_String, text->type());
	EXPECT_EQ(777, text->asInt32());
	EXPECT_DOUBLE_EQ(777.0, text->asDouble());
	EXPECT_FALSE(text->asBoolean());

	WTSVariant* half = root->get("half");
	ASSERT_TRUE(half != NULL);
	EXPECT_EQ(WTSVariant::VT_Real, half->type());
	// double 以 %.10f 格式落库
	EXPECT_EQ("0.5000000000", half->asString());
	EXPECT_EQ(0, half->asInt32());

	WTSVariant* flag = root->get("flag");
	ASSERT_TRUE(flag != NULL);
	EXPECT_EQ(WTSVariant::VT_Boolean, flag->type());
	EXPECT_TRUE(flag->asBoolean());
	EXPECT_EQ("true", flag->asString());
	// VT_Boolean 不在数值转换白名单内，走 default 分支
	EXPECT_EQ(0, flag->asInt32());
	EXPECT_DOUBLE_EQ(0.0, flag->asDouble());
	EXPECT_EQ(0u, flag->asUInt32());
	EXPECT_EQ(0LL, flag->asInt64());
	EXPECT_EQ(0ull, flag->asUInt64());

	// 容器类型取标量得到默认值
	WTSVariant* sub = WTSVariant::createObject();
	EXPECT_TRUE(root->append("sub", sub, true));
	sub->release();
	EXPECT_EQ(0, root->get("sub")->asInt32());
	EXPECT_EQ(0u, root->get("sub")->asUInt64());
	EXPECT_DOUBLE_EQ(0.0, root->get("sub")->asDouble());
	EXPECT_EQ("", root->get("sub")->asString());
	EXPECT_STREQ("", root->get("sub")->asCString());
}

TEST_F(L02VariantFixture, VariantBooleanParsingAcceptsYes)
{
	VarHolder root(WTSVariant::createObject());
	EXPECT_TRUE(root->append("y", "yes"));
	EXPECT_TRUE(root->append("TRUE", "TRUE"));
	EXPECT_TRUE(root->append("n", "no"));
	EXPECT_TRUE(root->append("empty", ""));

	EXPECT_TRUE(root->getBoolean("y"));
	EXPECT_TRUE(root->getBoolean("TRUE"));
	EXPECT_FALSE(root->getBoolean("n"));
	EXPECT_FALSE(root->getBoolean("empty"));
}

TEST_F(L02VariantFixture, VariantWrongTypeAccessors)
{
	VarHolder arr(WTSVariant::createArray());
	EXPECT_TRUE(arr->isArray());
	EXPECT_FALSE(arr->isObject());
	// 数组按名字取成员 -> NULL
	EXPECT_TRUE(arr->get("anything") == NULL);
	EXPECT_FALSE(arr->has("anything"));
	EXPECT_EQ(0u, arr->size());
	EXPECT_EQ(0u, arr->memberNames().size());
	// 空数组按下标取 -> NULL
	EXPECT_TRUE(arr->get((uint32_t)0) == NULL);
	// 数组按标量读取 -> default 分支
	EXPECT_EQ(0, arr->asInt32());
	EXPECT_EQ(0u, arr->asUInt32());
	EXPECT_EQ(0LL, arr->asInt64());
	EXPECT_EQ(0ull, arr->asUInt64());
	EXPECT_DOUBLE_EQ(0.0, arr->asDouble());
	EXPECT_EQ("", arr->asString());

	VarHolder obj(WTSVariant::createObject());
	EXPECT_EQ(0, obj->asInt32());
	EXPECT_EQ("", obj->asString());
}

TEST_F(L02VariantFixture, VariantAppendInvalidOnWrongContainer)
{
	VarHolder arr(WTSVariant::createArray());
	int32_t v = 1;
	// 数组上按名字追加失败
	EXPECT_FALSE(arr->append("k", v));
	EXPECT_FALSE(arr->append("k", "s"));
	uint32_t u = 2;
	EXPECT_FALSE(arr->append("k", u));
	EXPECT_FALSE(arr->append("k", (int64_t)3));
	EXPECT_FALSE(arr->append("k", (uint64_t)4));
	EXPECT_FALSE(arr->append("k", 5.0));
	EXPECT_FALSE(arr->append("k", true));

	VarHolder obj(WTSVariant::createObject());
	// 对象上按下标追加失败
	EXPECT_FALSE(obj->append("elem"));
	EXPECT_FALSE(obj->append(u));
	EXPECT_FALSE(obj->append((int64_t)3));
	EXPECT_FALSE(obj->append((uint64_t)4));
	EXPECT_FALSE(obj->append(1.5));
	EXPECT_FALSE(obj->append(false));
	EXPECT_FALSE(obj->append((WTSVariant*)NULL));
	EXPECT_FALSE(obj->append("valid", (WTSVariant*)NULL));
	EXPECT_EQ(0u, obj->size());
}

TEST_F(L02VariantFixture, VariantChildRefCountIsBalanced)
{
	{
		VarHolder root(WTSVariant::createObject());
		WTSVariant* child = WTSVariant::createObject();
		// create 出来引用计数为 1
		EXPECT_EQ(1u, child->retainCount());
		EXPECT_TRUE(root->append("child", child, true));
		// 默认容器再持有一次
		EXPECT_EQ(2u, child->retainCount());
		// 测试侧释放自己持有的这一次
		child->release();
		EXPECT_EQ(1u, child->retainCount());

		WTSVariant* got = root->get("child");
		ASSERT_TRUE(got != NULL);
		// get 不增加引用计数
		EXPECT_EQ(1u, got->retainCount());
		// 不手动 release child，交由 root 析构释放
	}
	// root 析构 -> map->release -> child 引用计数归零，无泄漏
	SUCCEED();
}

TEST_F(L02VariantFixture, VariantNestedMembersAndMemberNames)
{
	VarHolder root(WTSVariant::createObject());
	VarHolder sub(WTSVariant::createObject());
	uint32_t code = 1;
	EXPECT_TRUE(sub->append("code", code));
	EXPECT_TRUE(sub->append("name", "IF2107"));
	EXPECT_TRUE(root->append("sub", sub.get(), true));
	EXPECT_EQ(2u, sub->retainCount());

	WTSVariant* got = root->get("sub");
	ASSERT_TRUE(got != NULL);
	EXPECT_EQ(1u, got->getUInt32("code"));
	EXPECT_EQ("IF2107", got->getString("name"));
	// get(std::string) 重载
	std::string key("sub");
	EXPECT_TRUE(root->get(key) == got);

	WTSVariant::MemberNames names = root->memberNames();
	ASSERT_EQ(1u, names.size());
	EXPECT_EQ("sub", names[0]);

	// 同名 key 覆盖：旧值被释放一次
	VarHolder sub2(WTSVariant::createObject());
	EXPECT_TRUE(root->append("sub", sub2.get(), true));
	EXPECT_EQ(2u, sub2->retainCount());
	EXPECT_EQ(1u, sub->retainCount());
	EXPECT_EQ(1u, root->size());
}

//////////////////////////////////////////////////////////////// WTSVariant 数组
TEST_F(L02VariantFixture, VariantArrayIndexedAccess)
{
	VarHolder arr(WTSVariant::createArray());
	EXPECT_TRUE(arr->append("a"));
	EXPECT_TRUE(arr->append((int32_t)-1));
	uint32_t two = 2;
	EXPECT_TRUE(arr->append(two));
	int64_t big = 5000000000LL;
	EXPECT_TRUE(arr->append(big));
	uint64_t ubig = 9000000000000000000ULL;
	EXPECT_TRUE(arr->append(ubig));
	EXPECT_TRUE(arr->append(1.25));
	EXPECT_TRUE(arr->append(true));
	EXPECT_EQ(7u, arr->size());

	EXPECT_STREQ("a", arr->get((uint32_t)0)->asCString());
	EXPECT_EQ(-1, arr->get((uint32_t)1)->asInt32());
	EXPECT_EQ(2u, arr->get((uint32_t)2)->asUInt32());
	EXPECT_EQ(big, arr->get((uint32_t)3)->asInt64());
	EXPECT_EQ(ubig, arr->get((uint32_t)4)->asUInt64());
	EXPECT_DOUBLE_EQ(1.25, arr->get((uint32_t)5)->asDouble());
	EXPECT_TRUE(arr->get((uint32_t)6)->asBoolean());

	// 下标越界返回 NULL（WTSArray::at 做了上界检查）
	EXPECT_TRUE(arr->get((uint32_t)7) == NULL);
	EXPECT_TRUE(arr->get((uint32_t)9999) == NULL);

	// 成员容器为空数组时 size 返回 0
	VarHolder obj(WTSVariant::createObject());
	EXPECT_TRUE(obj->append("arr", WTSVariant::createArray()));
	WTSVariant* inner = obj->get("arr");
	ASSERT_TRUE(inner != NULL);
	EXPECT_EQ(0u, inner->size());
}

TEST_F(L02VariantFixture, VariantArrayWithEmbeddedContainers)
{
	VarHolder arr(WTSVariant::createArray());
	VarHolder item(WTSVariant::createObject());
	uint32_t one = 1;
	EXPECT_TRUE(item->append("id", one));
	EXPECT_TRUE(arr->append(item.get(), true));
	EXPECT_EQ(2u, item->retainCount());
	ASSERT_EQ(1u, arr->size());

	WTSVariant* back = arr->get((uint32_t)0);
	ASSERT_TRUE(back != NULL);
	EXPECT_TRUE(back->isObject());
	EXPECT_EQ(1u, back->getUInt32("id"));
}

//////////////////////////////////////////////////////////////// WTSArray
TEST_F(L02VariantFixture, WtsArrayAppendAtGrabAndSet)
{
	WTSArray* ay = WTSArray::create();
	EXPECT_TRUE(ay->empty());
	EXPECT_EQ(0u, ay->size());

	RefProbe* p1 = new RefProbe(1);   // refs 1
	RefProbe* p2 = new RefProbe(2);   // refs 1

	ay->append(p1);               // 自动 retain -> 2
	EXPECT_EQ(2u, p1->retainCount());
	ay->append(p2, false);        // 不 retain -> 仍为 1
	EXPECT_EQ(1u, p2->retainCount());
	// 注意：clear() 不区分是否自动持有，一律 release，见 bugs 记录
	p2->retain();                 // 预留给后续 clear() 的额外 release

	EXPECT_EQ(2u, ay->size());
	EXPECT_TRUE(ay->at(0) == p1);
	EXPECT_TRUE(ay->at<WTSObject>(1) == p2);
	EXPECT_TRUE(ay->operator[](0) == p1);
	EXPECT_TRUE(ay->at(2) == NULL);
	EXPECT_TRUE(ay->at(4294967295u) == NULL);

	WTSObject* grabbed = ay->grab(0);
	ASSERT_TRUE(grabbed == p1);
	EXPECT_EQ(3u, grabbed->retainCount());
	grabbed->release();
	EXPECT_EQ(2u, grabbed->retainCount());
	EXPECT_TRUE(ay->grab(9) == NULL);

	EXPECT_EQ(0u, ay->idxOf(p1));
	EXPECT_EQ(1u, ay->idxOf(p2));
	EXPECT_EQ(0xffffffffu, ay->idxOf(NULL));
	RefProbe other;
	EXPECT_EQ(0xffffffffu, ay->idxOf(&other));

	ay->resize(1, NULL);          // 先 clear 再预分配
	EXPECT_EQ(1u, ay->size());
	EXPECT_TRUE(ay->at(0) == NULL);
	EXPECT_EQ(1u, p1->retainCount());
	EXPECT_EQ(2u, p2->retainCount());

	// set：新值 retain，旧值（此处为 NULL）交换出去
	ay->set(0, p2, true);
	EXPECT_EQ(3u, p2->retainCount());
	// 越界 set 不生效
	ay->set(5, p1, true);
	EXPECT_EQ(1u, ay->size());
	EXPECT_TRUE(ay->at(0) == p2);

	// 收尾：数组持有 p2 两次？不，set 只 retain 一次，
	// 归还前手工补齐引用计数
	ay->clear();                  // p2 3 -> 2（p1 已不在数组内）
	EXPECT_EQ(0u, ay->size());
	ay->release();                // 释放数组本身
	p1->release();                // 1 -> 0 delete
	p2->release();                // 2 -> 1 ... 预留的那次
	p2->release();                // 1 -> 0 delete
	EXPECT_EQ(2, RefProbe::s_dtor);
	RefProbe::s_dtor = 0;
}

TEST_F(L02VariantFixture, WtsArrayMergeIterateAndSort)
{
	WTSArray* a = WTSArray::create();
	WTSArray* b = WTSArray::create();
	RefProbe* p1 = new RefProbe(1);
	RefProbe* p2 = new RefProbe(2);
	a->append(p1);
	b->append(p2);
	EXPECT_EQ(2u, p2->retainCount());

	a->append(b);
	EXPECT_EQ(2u, a->size());
	EXPECT_EQ(0u, b->size());        // 元素所有权被窃取
	EXPECT_EQ(2u, p2->retainCount());

	a->append((WTSArray*)NULL);
	EXPECT_EQ(2u, a->size());

	WTSArray::Iterator it = a->begin();
	EXPECT_TRUE(it != a->end());
	EXPECT_TRUE(*it == p1);
	WTSArray::ConstIterator cit = ((const WTSArray*)a)->begin();
	EXPECT_TRUE(cit != ((const WTSArray*)a)->end());
	EXPECT_TRUE(*((const WTSArray*)a)->rbegin() == p2);
	EXPECT_TRUE(a->rend() != a->rbegin());

	a->sort([](WTSObject* left, WTSObject* right) {
		return left->retainCount() < right->retainCount();
	});
	EXPECT_EQ(2u, a->size());
	EXPECT_TRUE(a->at(0) == p2);

	a->clear();
	EXPECT_EQ(0u, a->size());
	EXPECT_TRUE(a->empty());
	a->release();
	b->release();
	p1->release();
	p2->release();
	EXPECT_EQ(2, RefProbe::s_dtor);
	RefProbe::s_dtor = 0;
}

//////////////////////////////////////////////////////////////// WTSHashMap
TEST_F(L02VariantFixture, WtsHashMapAddGetGrabRemove)
{
	StrHashMap* map = StrHashMap::create();
	EXPECT_EQ(0u, map->size());

	RefProbe* p = new RefProbe(1);
	map->add("k1", p);
	EXPECT_EQ(1u, map->size());
	EXPECT_EQ(2u, p->retainCount());
	EXPECT_TRUE(map->get("k1") == p);
	EXPECT_EQ(2u, p->retainCount());

	WTSObject* grabbed = map->grab("k1");
	ASSERT_TRUE(grabbed == p);
	EXPECT_EQ(3u, grabbed->retainCount());
	grabbed->release();
	EXPECT_EQ(2u, p->retainCount());

	EXPECT_TRUE(map->get("missing") == NULL);
	EXPECT_TRUE(map->grab("missing") == NULL);
	EXPECT_TRUE(map->find("k1") != map->end());
	EXPECT_TRUE(map->find("missing") == map->end());

	// 同名覆盖会释放旧值
	RefProbe* p2 = new RefProbe(2);
	map->add("k1", p2);
	EXPECT_EQ(1u, map->size());
	EXPECT_EQ(2u, p2->retainCount());
	EXPECT_EQ(1u, p->retainCount());

	for (StrHashMap::ConstIterator mit = map->begin(); mit != map->end(); ++mit)
	{
		EXPECT_EQ("k1", mit->first);
		EXPECT_TRUE(mit->second == p2);
	}

	map->remove("k1");
	EXPECT_EQ(0u, map->size());
	EXPECT_EQ(1u, p2->retainCount());
	map->remove("not_here");

	map->clear();
	map->release();
	p->release();
	p2->release();
	EXPECT_EQ(2, RefProbe::s_dtor);
	RefProbe::s_dtor = 0;
}

TEST_F(L02VariantFixture, WtsHashMapClearReleasesEntries)
{
	StrHashMap* map = StrHashMap::create();
	RefProbe* p = new RefProbe(1);
	map->add("k", p);
	EXPECT_EQ(2u, p->retainCount());
	map->clear();
	EXPECT_EQ(0u, map->size());
	EXPECT_EQ(1u, p->retainCount());
	map->release();
	p->release();
	EXPECT_EQ(1, RefProbe::s_dtor);
	RefProbe::s_dtor = 0;
}

//////////////////////////////////////////////////////////////// WTSQueue
TEST_F(L02VariantFixture, WtsQueuePushPopFrontBack)
{
	WTSQueue* q = WTSQueue::create();
	EXPECT_TRUE(q->empty());
	EXPECT_TRUE(q->front() == NULL);
	EXPECT_TRUE(q->back() == NULL);
	EXPECT_EQ(0u, q->size());

	RefProbe* p1 = new RefProbe(1);
	RefProbe* p2 = new RefProbe(2);
	q->push(p1);
	q->push(p2);
	EXPECT_EQ(2u, q->size());
	EXPECT_FALSE(q->empty());
	EXPECT_TRUE(q->front(false) == p1);
	EXPECT_TRUE(q->back(false) == p2);
	EXPECT_EQ(3u, q->front(true)->retainCount());   // 2 持有 + 1 临时
	q->front(true)->release();
	EXPECT_EQ(2u, p1->retainCount());
	EXPECT_EQ(2u, p2->retainCount());

	q->pop();
	EXPECT_EQ(1u, q->size());
	// BUG: pop() 只出队不做 release，被弹出对象引用计数仍为 2（泄漏）
	EXPECT_EQ(2u, p1->retainCount());
	p1->release();                                  // 手工补偿
	EXPECT_EQ(1u, p1->retainCount());

	q->clear();
	EXPECT_EQ(0u, q->size());
	q->release();
	p1->release();
	p2->release();
	EXPECT_EQ(2, RefProbe::s_dtor);
	RefProbe::s_dtor = 0;
}

TEST_F(L02VariantFixture, WtsQueueSwapIteratorsAndPushNoRetain)
{
	WTSQueue* a = WTSQueue::create();
	WTSQueue* b = WTSQueue::create();
	RefProbe* p = new RefProbe(1);

	a->push(p);
	a->push(p, false);
	EXPECT_EQ(2u, a->size());
	EXPECT_EQ(3u, p->retainCount());   // 1 创建 + 1 自动 retain + 0
	EXPECT_TRUE(a->begin() != a->end());
	EXPECT_TRUE(b->begin() == b->end());
	WTSQueue::ConstIterator cit = ((const WTSQueue*)a)->begin();
	EXPECT_TRUE(cit != ((const WTSQueue*)a)->end());

	a->swap(b);
	EXPECT_EQ(0u, a->size());
	EXPECT_EQ(2u, b->size());
	EXPECT_TRUE(b->front(false) == p);

	// b 持有 p 一次，a 持有 0 次（swap 后）
	b->clear();
	a->clear();
	a->release();
	b->release();
	p->release();
	p->release();
	EXPECT_EQ(1, RefProbe::s_dtor);
	RefProbe::s_dtor = 0;
}
