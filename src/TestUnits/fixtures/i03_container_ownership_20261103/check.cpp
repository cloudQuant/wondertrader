// 迭代03 卡12（BUG-R53-002 / BUG-R12-14）容器所有权合同定向检查。
// 合同：12a=A WTSMap::erase 三重载释放容器持有的引用；12b=C WTSStack 显式
// pushOwned/pushTransferred/pushBorrowed，旧 push(obj,false) 按转移兼容，pop 返回拥有引用。
// 用法：check list | check all | check <case>；每个 case 结束时断言构造数==析构数（无泄漏），
// 并逐步断言精确引用计数；双重释放/UAF 由 ASan 捕获。
// 同一源文件既用当前头编译（green），也用 baseline/Includes 下 HEAD 头编译（red 对照）；
// 新增显式 API 的 case 只在定义了 WTS_COLLECTION_OWNERSHIP_CONTRACT 时编译。
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "Includes/WTSCollection.hpp"

USING_NS_WTP;

namespace {

int g_ctor = 0;
int g_dtor = 0;
int g_fail = 0;

class Probe : public WTSObject
{
public:
	explicit Probe(int tag) : _tag(tag) { g_ctor++; }
	virtual ~Probe() { g_dtor++; }
	int tag() const { return _tag; }

private:
	int _tag;
};

#define CHECK_EQ(a, b) do { \
	long long _va = (long long)(a); long long _vb = (long long)(b); \
	if (_va != _vb) { g_fail++; std::printf("  FAIL %s:%d  %s == %s  (got %lld, want %lld)\n", __FILE__, __LINE__, #a, #b, _va, _vb); } \
} while (0)

uint32_t rc(WTSObject* o) { return o->retainCount(); }

typedef WTSMap<int> IntMap;

//////////////////////////////////////////////////////////////////////// WTSMap
void map_erase_iterator_releases()
{
	IntMap* m = IntMap::create();
	Probe* p = new Probe(1);
	m->add(1, p);
	CHECK_EQ(rc(p), 2);
	m->erase(m->find(1));
	CHECK_EQ(m->size(), 0);
	CHECK_EQ(rc(p), 1);	// 只剩调用方的引用
	p->release();
	CHECK_EQ(g_dtor, 1);
	m->release();
}

void map_erase_const_iterator_releases()
{
	IntMap* m = IntMap::create();
	Probe* p = new Probe(1);
	m->add(1, p);
	const IntMap* cm = m;
	IntMap::ConstIterator cit = cm->find(1);
	m->erase(cit);
	CHECK_EQ(m->size(), 0);
	CHECK_EQ(rc(p), 1);
	p->release();
	CHECK_EQ(g_dtor, 1);
	m->release();
}

void map_erase_key_releases()
{
	IntMap* m = IntMap::create();
	Probe* p = new Probe(1);
	m->add(1, p);
	m->erase(1);
	CHECK_EQ(m->size(), 0);
	CHECK_EQ(rc(p), 1);
	p->release();
	CHECK_EQ(g_dtor, 1);
	m->release();
}

void map_erase_key_missing_and_null_value()
{
	IntMap* m = IntMap::create();
	Probe* p = new Probe(1);
	m->add(1, p);
	m->erase(99);	// 不存在的 key：无操作
	CHECK_EQ(m->size(), 1);
	CHECK_EQ(rc(p), 2);
	m->add(2, nullptr);	// 允许 NULL 值
	m->erase(2);
	CHECK_EQ(m->size(), 1);
	m->add(3, nullptr);
	m->erase(m->find(3));
	CHECK_EQ(m->size(), 1);
	CHECK_EQ(rc(p), 2);
	m->release();	// 析构路径释放 key=1 的容器引用
	CHECK_EQ(rc(p), 1);
	p->release();
	CHECK_EQ(g_dtor, 1);
}

void map_erase_last_reference_destroys_once()
{
	IntMap* m = IntMap::create();
	// bAutoRetain=false：调用方把唯一引用交给容器；三个重载各自是最后一份引用
	m->add(1, new Probe(1), false);
	m->add(2, new Probe(2), false);
	m->add(3, new Probe(3), false);
	m->erase(m->find(1));
	CHECK_EQ(g_dtor, 1);
	const IntMap* cm = m;
	m->erase(cm->find(2));
	CHECK_EQ(g_dtor, 2);
	m->erase(3);
	CHECK_EQ(g_dtor, 3);
	CHECK_EQ(m->size(), 0);
	m->release();
	CHECK_EQ(g_dtor, 3);
}

void map_duplicate_value_under_two_keys()
{
	IntMap* m = IntMap::create();
	Probe* p = new Probe(1);
	m->add(1, p);
	m->add(2, p);
	CHECK_EQ(rc(p), 3);
	m->erase(1);
	CHECK_EQ(rc(p), 2);	// 只释放被摘除节点那一份
	CHECK_EQ(m->size(), 1);
	m->erase(m->find(2));
	CHECK_EQ(rc(p), 1);
	// 覆盖同一 key 同一对象：add 先 retain 再 release 旧值，计数不变
	m->add(5, p);
	m->add(5, p);
	CHECK_EQ(rc(p), 2);
	m->erase(5);
	CHECK_EQ(rc(p), 1);
	p->release();
	CHECK_EQ(g_dtor, 1);
	m->release();
}

void map_remove_clear_destructor_matrix()
{
	IntMap* m = IntMap::create();
	Probe* a = new Probe(1);
	Probe* b = new Probe(2);
	Probe* c = new Probe(3);
	m->add(1, a);
	m->add(2, b);
	m->add(3, c);
	m->remove(1);
	CHECK_EQ(rc(a), 1);
	m->erase(2);
	CHECK_EQ(rc(b), 1);	// erase 与 remove 对称
	m->clear();
	CHECK_EQ(rc(c), 1);
	m->add(4, c);
	m->release();	// 析构 clear
	CHECK_EQ(rc(c), 1);
	a->release();
	b->release();
	c->release();
	CHECK_EQ(g_dtor, 3);
}

//////////////////////////////////////////////////////////////////////// WTSStack
void stack_empty()
{
	WTSStack* s = WTSStack::create();
	CHECK_EQ(s->empty(), 1);
	CHECK_EQ(s->top() == nullptr, 1);
	CHECK_EQ(s->pop() == nullptr, 1);
	s->clear();
	CHECK_EQ(s->size(), 0);
	s->push(nullptr);
	s->push(nullptr, false);
	CHECK_EQ(s->size(), 2);
	CHECK_EQ(s->pop() == nullptr, 1);
	CHECK_EQ(s->pop() == nullptr, 1);
	CHECK_EQ(s->pop() == nullptr, 1);
	s->release();
}

void stack_push_default_pop_owning()
{
	WTSStack* s = WTSStack::create();
	Probe* p = new Probe(1);
	s->push(p);
	CHECK_EQ(rc(p), 2);
	WTSObject* got = s->pop();
	CHECK_EQ(got == p, 1);
	CHECK_EQ(rc(p), 2);	// pop 交出栈的引用，不在返回前 release
	got->release();
	CHECK_EQ(rc(p), 1);
	p->release();
	CHECK_EQ(g_dtor, 1);
	s->release();
}

void stack_push_true_pop_owning()
{
	WTSStack* s = WTSStack::create();
	Probe* p = new Probe(1);
	s->push(p, true);
	CHECK_EQ(rc(p), 2);
	WTSObject* got = s->pop();
	CHECK_EQ(rc(p), 2);
	got->release();
	p->release();
	CHECK_EQ(g_dtor, 1);
	s->release();
}

void stack_push_false_pop_transfer()
{
	WTSStack* s = WTSStack::create();
	Probe* p = new Probe(1);	// 唯一引用
	s->push(p, false);	// 转交给栈
	CHECK_EQ(rc(p), 1);
	WTSObject* got = s->pop();	// 旧实现：返回前 release → 对象已析构，以下读取为 UAF
	CHECK_EQ(g_dtor, 0);
	CHECK_EQ(got == p, 1);
	CHECK_EQ(rc(got), 1);
	CHECK_EQ(static_cast<Probe*>(got)->tag(), 1);
	got->release();
	CHECK_EQ(g_dtor, 1);
	s->release();
	CHECK_EQ(g_dtor, 1);
}

void stack_push_false_clear_and_destructor()
{
	WTSStack* s = WTSStack::create();
	s->push(new Probe(1), false);
	s->clear();
	CHECK_EQ(g_dtor, 1);
	s->push(new Probe(2), false);
	s->release();
	CHECK_EQ(g_dtor, 2);
}

void stack_duplicate_elements_legacy()
{
	WTSStack* s = WTSStack::create();
	Probe* p = new Probe(1);
	s->push(p);
	s->push(p);
	CHECK_EQ(rc(p), 3);
	WTSObject* a = s->pop();
	CHECK_EQ(rc(p), 3);
	a->release();
	CHECK_EQ(rc(p), 2);
	s->clear();	// 剩余一份由 clear 释放
	CHECK_EQ(rc(p), 1);
	p->release();
	CHECK_EQ(g_dtor, 1);
	s->release();
}

#ifdef WTS_COLLECTION_OWNERSHIP_CONTRACT
void stack_explicit_owned()
{
	WTSStack* s = WTSStack::create();
	Probe* p = new Probe(1);
	s->pushOwned(p);
	CHECK_EQ(rc(p), 2);
	CHECK_EQ(s->isTopBorrowed(), 0);
	WTSObject* got = s->pop();
	CHECK_EQ(rc(p), 2);
	got->release();
	p->release();
	CHECK_EQ(g_dtor, 1);
	s->release();
}

void stack_explicit_transferred()
{
	WTSStack* s = WTSStack::create();
	Probe* p = new Probe(1);
	s->pushTransferred(p);
	CHECK_EQ(rc(p), 1);
	CHECK_EQ(s->isTopBorrowed(), 0);
	WTSObject* got = s->pop();
	CHECK_EQ(g_dtor, 0);
	CHECK_EQ(rc(got), 1);
	got->release();
	CHECK_EQ(g_dtor, 1);
	s->release();
}

void stack_explicit_borrowed()
{
	WTSStack* s = WTSStack::create();
	Probe* p = new Probe(1);	// 外部 owner 持有唯一引用
	s->pushBorrowed(p);
	CHECK_EQ(rc(p), 1);
	CHECK_EQ(s->isTopBorrowed(), 1);
	CHECK_EQ(s->top() == p, 1);
	CHECK_EQ(rc(p), 1);
	WTSObject* got = s->pop();	// 借用元素 pop 时 retain，仍返回拥有引用
	CHECK_EQ(rc(p), 2);
	got->release();
	CHECK_EQ(rc(p), 1);
	// clear/析构不消费外部 owner 的引用
	s->pushBorrowed(p);
	s->pushBorrowed(p);
	s->clear();
	CHECK_EQ(rc(p), 1);
	s->pushBorrowed(p);
	s->release();
	CHECK_EQ(rc(p), 1);
	CHECK_EQ(g_dtor, 0);
	p->release();
	CHECK_EQ(g_dtor, 1);
}

void stack_explicit_null_entries()
{
	WTSStack* s = WTSStack::create();
	s->pushOwned(nullptr);
	s->pushTransferred(nullptr);
	s->pushBorrowed(nullptr);
	CHECK_EQ(s->size(), 3);
	CHECK_EQ(s->isTopBorrowed(), 1);
	CHECK_EQ(s->pop() == nullptr, 1);
	CHECK_EQ(s->pop() == nullptr, 1);
	CHECK_EQ(s->pop() == nullptr, 1);
	CHECK_EQ(s->isTopBorrowed(), 0);
	s->pushBorrowed(nullptr);
	s->pushOwned(nullptr);
	s->release();
}

void stack_duplicate_mixed_modes()
{
	WTSStack* s = WTSStack::create();
	Probe* p = new Probe(1);
	p->retain();	// 调用方 2 份，其中 1 份将转交
	s->pushOwned(p);	// 3
	s->pushTransferred(p);	// 3（转交）
	s->pushBorrowed(p);	// 3
	CHECK_EQ(rc(p), 3);
	CHECK_EQ(s->size(), 3);
	WTSObject* b = s->pop();	// 借用 → retain
	CHECK_EQ(rc(p), 4);
	b->release();
	CHECK_EQ(rc(p), 3);
	WTSObject* t = s->pop();	// 转交的那份交还
	CHECK_EQ(rc(p), 3);
	t->release();
	CHECK_EQ(rc(p), 2);
	s->clear();	// 释放 pushOwned 那份
	CHECK_EQ(rc(p), 1);
	p->release();
	CHECK_EQ(g_dtor, 1);
	s->release();
}

void stack_destructor_matrix()
{
	WTSStack* s = WTSStack::create();
	Probe* owned = new Probe(1);
	Probe* borrowed = new Probe(2);
	s->pushOwned(owned);	// owned: 2
	s->pushTransferred(new Probe(3));	// 唯一引用在栈内
	s->pushBorrowed(borrowed);	// borrowed: 1
	s->push(nullptr);
	s->push(new Probe(4), false);	// 旧入口转移
	s->release();	// 析构 → clear
	CHECK_EQ(g_dtor, 2);	// Probe3、Probe4
	CHECK_EQ(rc(owned), 1);
	CHECK_EQ(rc(borrowed), 1);
	owned->release();
	borrowed->release();
	CHECK_EQ(g_dtor, 4);
}

void stack_clear_matrix_then_reuse()
{
	WTSStack* s = WTSStack::create();
	Probe* owned = new Probe(1);
	Probe* borrowed = new Probe(2);
	s->pushOwned(owned);
	s->pushBorrowed(borrowed);
	s->pushTransferred(new Probe(3));
	s->clear();
	CHECK_EQ(s->empty(), 1);
	CHECK_EQ(g_dtor, 1);
	CHECK_EQ(rc(owned), 1);
	CHECK_EQ(rc(borrowed), 1);
	s->clear();	// 重复 clear 无操作
	s->pushOwned(owned);
	WTSObject* got = s->pop();
	got->release();
	CHECK_EQ(rc(owned), 1);
	owned->release();
	borrowed->release();
	CHECK_EQ(g_dtor, 3);
	s->release();
}
#endif

struct Case { const char* name; void (*fn)(); };

const Case kCases[] = {
	{ "map_erase_iterator_releases", map_erase_iterator_releases },
	{ "map_erase_const_iterator_releases", map_erase_const_iterator_releases },
	{ "map_erase_key_releases", map_erase_key_releases },
	{ "map_erase_key_missing_and_null_value", map_erase_key_missing_and_null_value },
	{ "map_erase_last_reference_destroys_once", map_erase_last_reference_destroys_once },
	{ "map_duplicate_value_under_two_keys", map_duplicate_value_under_two_keys },
	{ "map_remove_clear_destructor_matrix", map_remove_clear_destructor_matrix },
	{ "stack_empty", stack_empty },
	{ "stack_push_default_pop_owning", stack_push_default_pop_owning },
	{ "stack_push_true_pop_owning", stack_push_true_pop_owning },
	{ "stack_push_false_pop_transfer", stack_push_false_pop_transfer },
	{ "stack_push_false_clear_and_destructor", stack_push_false_clear_and_destructor },
	{ "stack_duplicate_elements_legacy", stack_duplicate_elements_legacy },
#ifdef WTS_COLLECTION_OWNERSHIP_CONTRACT
	{ "stack_explicit_owned", stack_explicit_owned },
	{ "stack_explicit_transferred", stack_explicit_transferred },
	{ "stack_explicit_borrowed", stack_explicit_borrowed },
	{ "stack_explicit_null_entries", stack_explicit_null_entries },
	{ "stack_duplicate_mixed_modes", stack_duplicate_mixed_modes },
	{ "stack_destructor_matrix", stack_destructor_matrix },
	{ "stack_clear_matrix_then_reuse", stack_clear_matrix_then_reuse },
#endif
};

int runOne(const Case& c)
{
	g_ctor = g_dtor = g_fail = 0;
	c.fn();
	if (g_ctor != g_dtor)
	{
		g_fail++;
		std::printf("  FAIL leak/over-release: ctor=%d dtor=%d\n", g_ctor, g_dtor);
	}
	std::printf("[%s] %s\n", g_fail == 0 ? "PASS" : "FAIL", c.name);
	std::fflush(stdout);
	return g_fail == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
#ifdef WTS_COLLECTION_OWNERSHIP_CONTRACT
	std::printf("header contract: %ld\n", (long)WTS_COLLECTION_OWNERSHIP_CONTRACT);
#else
	std::printf("header contract: none (legacy header)\n");
#endif
	const char* want = argc > 1 ? argv[1] : "all";
	int failed = 0, ran = 0;
	for (const Case& c : kCases)
	{
		if (std::strcmp(want, "list") == 0) { std::printf("%s\n", c.name); continue; }
		if (std::strcmp(want, "all") != 0 && std::strcmp(want, c.name) != 0) continue;
		ran++;
		failed += runOne(c);
	}
	if (std::strcmp(want, "list") == 0) return 0;
	if (ran == 0) { std::printf("unknown case: %s\n", want); return 2; }
	std::printf("summary: ran=%d failed=%d\n", ran, failed);
	return failed == 0 ? 0 : 1;
}
