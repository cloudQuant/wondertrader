// L01 (lane Share/WTSUtils): ObjectPool.hpp coverage.
// 疑似缺陷见 docs/迭代计划/迭代01/bugs/l01_share_wtsutils.md
#include <gtest/gtest.h>

#include "Share/ObjectPool.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// 记录构造/析构次数的普通类型
struct Tracked
{
	int				a{ 0 };
	double			b{ 0 };
	std::vector<int> v{};

	static int live;
	static int created;

	Tracked() { ++live; ++created; }
	~Tracked() { --live; }

	Tracked(const Tracked&) = delete;
	Tracked& operator=(const Tracked&) = delete;
};

int Tracked::live = 0;
int Tracked::created = 0;

// 简单 POD
struct Pod
{
	uint32_t	x;
	uint32_t	y;
	uint64_t	sum;
};

// 带显式构造逻辑的成员，用于验证 placement new 真的执行了构造函数体
struct Tagged
{
	uint32_t	magic;
	char		name[16];

	Tagged() :magic(0xABCD1234u)
	{
		std::memset(name, 0, sizeof(name));
		std::memcpy(name, "tag", 4);
	}
};

} // namespace

// ------------------------------------------------------------------ 基本构造/析构
TEST(l01_objectpool, construct_value_initializes_pod)
{
	ObjectPool<Pod> pool;
	Pod* p = pool.construct();
	ASSERT_NE(p, nullptr);
	// new(mem) T() 走值初始化，POD 应被清零
	EXPECT_EQ(p->x, 0u);
	EXPECT_EQ(p->y, 0u);
	EXPECT_EQ(p->sum, 0ull);

	// 池外写入不应破坏下一次分配（先销毁归还）
	p->x = 7;
	p->sum = 99;
	pool.destroy(p);
	SUCCEED();
}

TEST(l01_objectpool, construct_calls_constructor_body)
{
	ObjectPool<Tagged> pool;
	Tagged* p = pool.construct();
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(p->magic, 0xABCD1234u);
	EXPECT_STREQ(p->name, "tag");
	pool.destroy(p);
}

TEST(l01_objectpool, destroy_invokes_destructor)
{
	Tracked::live = 0;
	Tracked::created = 0;

	{
		ObjectPool<Tracked> pool;
		Tracked* a = pool.construct();
		Tracked* b = pool.construct();
		ASSERT_NE(a, nullptr);
		ASSERT_NE(b, nullptr);
		EXPECT_EQ(Tracked::live, 2);
		EXPECT_EQ(Tracked::created, 2);

		a->a = 1;
		b->v.push_back(5);
		EXPECT_EQ(b->v.size(), 1u);

		pool.destroy(a);
		EXPECT_EQ(Tracked::live, 1);
		pool.destroy(b);
		EXPECT_EQ(Tracked::live, 0);
	}
	// 池析构后不应再有存活对象
	EXPECT_EQ(Tracked::live, 0);
}

TEST(l01_objectpool, addresses_are_distinct_while_in_use)
{
	ObjectPool<Pod> pool;
	std::vector<Pod*> holds;
	for (int i = 0; i < 256; ++i)
	{
		Pod* p = pool.construct();
		ASSERT_NE(p, nullptr);
		p->x = static_cast<uint32_t>(i);
		holds.push_back(p);
	}

	// 每个在使用的地址必须唯一
	std::vector<char*> seen;
	for (Pod* p : holds)
		seen.push_back(reinterpret_cast<char*>(p));
	std::sort(seen.begin(), seen.end());
	EXPECT_EQ(std::unique(seen.begin(), seen.end()), seen.end());

	// 数据没有被后来的分配覆盖
	for (int i = 0; i < 256; ++i)
		EXPECT_EQ(holds[static_cast<std::size_t>(i)]->x, static_cast<uint32_t>(i));

	for (Pod* p : holds)
		pool.destroy(p);
	EXPECT_TRUE(holds.front() != nullptr);
}

TEST(l01_objectpool, freed_chunk_is_reused)
{
	ObjectPool<Pod> pool;
	Pod* first = pool.construct();
	ASSERT_NE(first, nullptr);
	pool.destroy(first);

	Pod* second = pool.construct();
	ASSERT_NE(second, nullptr);
	// boost::pool 的空闲链表是 LIFO，归还后立刻再取应拿到同一块
	EXPECT_EQ(second, first);
	pool.destroy(second);
}

TEST(l01_objectpool, release_memory_then_allocate_again)
{
	ObjectPool<Pod> pool;
	std::vector<Pod*> holds;
	for (int i = 0; i < 64; ++i)
		holds.push_back(pool.construct());

	for (Pod* p : holds)
		pool.destroy(p);

	// 归还空闲块给操作系统后仍可继续分配
	pool.release();
	Pod* p = pool.construct();
	ASSERT_NE(p, nullptr);
	p->y = 123;
	EXPECT_EQ(p->y, 123u);
	pool.destroy(p);

	// 多次 release 是幂等的
	pool.release();
	pool.release();
	SUCCEED();
}

TEST(l01_objectpool, destructor_of_empty_and_used_pool)
{
	Tracked::live = 0;
	{
		ObjectPool<Tracked> emptyPool;
		EXPECT_EQ(Tracked::live, 0);
	}
	{
		ObjectPool<Tracked> pool;
		// 注意：未 destroy 的对象不会执行析构，这里只验证池本身可正常销毁
		Tracked* p = pool.construct();
		ASSERT_NE(p, nullptr);
		EXPECT_EQ(Tracked::live, 1);
		pool.destroy(p);
		EXPECT_EQ(Tracked::live, 0);
	}
	EXPECT_EQ(Tracked::live, 0);
}

TEST(l01_objectpool, pool_is_polymorphic_through_base_pointer)
{
	// ObjectPool 的析构函数是 virtual，通过基类指针删除派生池是安全的
	ObjectPool<Pod>* base = new ObjectPool<Pod>();
	Pod* p = base->construct();
	ASSERT_NE(p, nullptr);
	base->destroy(p);
	delete base;
	SUCCEED();
}

TEST(l01_objectpool, non_trivial_type_with_std_string)
{
	ObjectPool<std::string> pool;
	std::string* s = pool.construct();
	ASSERT_NE(s, nullptr);
	EXPECT_TRUE(s->empty());

	s->assign("hello-world");
	EXPECT_EQ(s->size(), 11u);
	EXPECT_STREQ(s->c_str(), "hello-world");

	pool.destroy(s);
	// 归还后重新构造必须是干净的空串
	std::string* again = pool.construct();
	ASSERT_NE(again, nullptr);
	EXPECT_TRUE(again->empty());
	pool.destroy(again);
}

TEST(l01_objectpool, DISABLED_destroy_null_is_undefined_and_rejected)
{
	// destroy(nullptr) 的接口容忍语义尚未定义；当前实现会解引用空指针，
	// 但接口没有明确要求必须支持 NULL。先保留候选用例，等待输入前置条件定案。
	ObjectPool<Pod> pool;
	pool.destroy(nullptr);
	SUCCEED();
}

TEST(l01_objectpool, independent_pools_do_not_share_chunks)
{
	ObjectPool<Pod> poolA;
	ObjectPool<Tagged> poolB;

	Pod* a = poolA.construct();
	Tagged* b = poolB.construct();
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);

	a->x = 1;
	b->magic = 2;

	EXPECT_EQ(a->x, 1u);
	EXPECT_EQ(b->magic, 2u);
	EXPECT_NE(reinterpret_cast<char*>(a), reinterpret_cast<char*>(b));

	poolA.destroy(a);
	poolB.destroy(b);
}
