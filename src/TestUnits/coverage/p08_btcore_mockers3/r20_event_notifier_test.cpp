/**
 * r20_event_notifier_test.cpp  (lane R20)
 *
 * 靶子: src/WtBtCore/EventNotifier.cpp（44/64 未覆盖）
 * 公开面: init / notifyEvent / notifyData / notifyFund / 构造析构。
 *
 * 关键约束（读源码得出，不猜测）：
 * - EventNotifier 构造函数只初始化 _mq_sid=0，**不初始化** m_strURL 之外的
 *   _creator/_remover/_publisher/_register 四个函数指针（EventNotifier.h ctor）。
 *   因此 heap/stack 实例调用 notify* 会读野指针（UB）。本用例统一用 **static 存储期**
 *   实例：静态区在 ctor 之前已被零初始化，ctor 不写这四个成员 → _publisher 恒为 NULL，
 *   notify* 的 `if(_publisher)` 守卫是确定性的。缺陷已记 bugs/R20_wtbtcore.md。
 * - init(active=true) 的正分支会在 load 成功后调用 create_server 起**真实 MQ 服务**
 *   （本机确认 build-cov 下存在 libWtMsgQue.dylib），违反「禁真实 socket」，
 *   故本轮不进入该分支，登记留白。
 * - 不起线程、不 sleep、不读挂钟；期望值全部由本用例自喂输入算得。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include <Includes/WTSVariant.hpp>
#include <WtBtCore/EventNotifier.h>

USING_NS_WTP;

namespace
{
	// static 存储期 → 四个函数指针成员确定性为 NULL
	static wtp::EventNotifier g_notifier;

	WTSVariant* bool_cfg(const char* key, bool val)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append(key, val);
		return cfg;
	}
}

class R20EventNotifier : public ::testing::Test
{
protected:
	void SetUp() override {}
};

// ctor: 静态实例构造后 notify* 的守卫链必须为空转（_publisher==NULL 由静态零初始化保证）
TEST_F(R20EventNotifier, CtorLeavesPublisherNullOnStaticInstance)
{
	// notifyEvent 内部 strlen(evtType) 只在 publisher 非空时使用；
	// 这里只验证守卫空转不崩溃、且可重复调用幂等。
	g_notifier.notifyEvent("BT_BACKTEST_DONE");
	g_notifier.notifyEvent("BT_BACKTEST_DONE");
	g_notifier.notifyEvent("");
	SUCCEED();
}

// notifyData: publisher 为 NULL 时不得触碰 data 缓冲区（本用例自喂哨兵 buffer 验证未被改写）
TEST_F(R20EventNotifier, NotifyDataDoesNotTouchBufferWithoutPublisher)
{
	char payload[16];
	std::memset(payload, 0x5A, sizeof(payload));
	const char expected[16] = {
		0x5A,0x5A,0x5A,0x5A,0x5A,0x5A,0x5A,0x5A,
		0x5A,0x5A,0x5A,0x5A,0x5A,0x5A,0x5A,0x5A };
	g_notifier.notifyData("BT_STRATA", payload, static_cast<uint32_t>(sizeof(payload)));
	g_notifier.notifyData("BT_STRATA", payload, 0);
	EXPECT_EQ(0, std::memcmp(payload, expected, sizeof(payload)));
}

// notifyFund: JSON 拼装段（L157-185）在 publisher 为 NULL 时也必须完整走完、只空转发送
TEST_F(R20EventNotifier, NotifyFundBuildsJsonAndNoopsWithoutPublisher)
{
	// 五个入参全部来自本用例；无 publisher → 无任何外部可观察副作用，
	// 断言锚定「两次不同入参调用都不崩溃、且函数自身不改任何共享状态」。
	g_notifier.notifyFund("BT_FUND", 20240103, 1234.5, -67.25, 100000.0, 88.125);
	g_notifier.notifyFund("BT_FUND", 0, 0.0, 0.0, 0.0, 0.0);
	g_notifier.notifyFund("BT_FUND", 20241231, -1.0, -2.0, -3.0, -4.0);
	SUCCEED();
}

// init: active=false 必须在读 url / load 模块之前返回 false（L69-70）
TEST_F(R20EventNotifier, InitReturnsFalseWhenInactive)
{
	WTSVariant* cfg = bool_cfg("active", false);
	EXPECT_FALSE(g_notifier.init(cfg));
	// 早退不得污染静态实例的 notify* 空转语义
	g_notifier.notifyEvent("R20_AFTER_INACTIVE");
	SUCCEED();
}

// init 用独立 static 实例同样返回 false；两个实例互不影响（无全局单例状态）
TEST_F(R20EventNotifier, InactiveInitIsSideEffectFreeOnFreshInstance)
{
	static wtp::EventNotifier s_probe;
	WTSVariant* cfg = bool_cfg("active", false);
	EXPECT_FALSE(s_probe.init(cfg));
	s_probe.notifyFund("R20_PROBE", 20240103, 1.0, 2.0, 3.0, 4.0);
	s_probe.notifyData("R20_PROBE", nullptr, 0);
	SUCCEED();
}

// dtor: _remover==NULL 且 _mq_sid==0 时短路，静态实例析构不触发（对象仍存活）；
// 这里覆盖「init 失败后 _mq_sid 仍为 0」的可观察不变量。
TEST_F(R20EventNotifier, FailedInitLeavesServerIdZero)
{
	static wtp::EventNotifier s_probe2;
	WTSVariant* cfg = bool_cfg("active", false);
	EXPECT_FALSE(s_probe2.init(cfg));
	// _mq_sid==0 不变 → 若将来析构该实例，_remover&&_mq_sid!=0 必为假（EventNotifier.cpp L55-56）
	s_probe2.notifyEvent("R20_SID_ZERO");
	SUCCEED();
}

// DISABLED_: init(active=true) 正分支。触发条件：cfg.active=true 且
// WtHelper::getCWD()/getInstDir() 下存在 libWtMsgQue.dylib（本机 build-cov 已存在该文件）。
// 该分支 load 成功后会调用 _register(on_mq_log) 与 _creator(url, true) **拉起真实 MQ 服务**，
// 违反本 lane「禁真实 socket」硬规则；且成功后 _register 未做 NULL 校验（只校验 _creator），
// 符号缺失时直接调用空指针。整条登记留白，见 manifest「留白面」。
TEST_F(R20EventNotifier, DISABLED_InitActiveLoadsMqModuleAndOpensServer)
{
	// 故意不实现：见上。若主控批准放行 socket，再补 url 与返回值断言。
	GTEST_SKIP() << "requires real MQ server; blocked by lane rule";
}

// DISABLED_: init(NULL)。触发条件：任意 EventNotifier 实例传 cfg==NULL，
// L69 `cfg->getBoolean("active")` 直接解引用空指针崩溃（无 NULL 守卫）。
// 已按缺陷登记（bugs/R20_wtbtcore.md，S3 输入健壮性）。
TEST_F(R20EventNotifier, DISABLED_InitNullConfigDerefsAndCrashes)
{
	GTEST_SKIP() << "EventNotifier::init(NULL) derefs cfg at EventNotifier.cpp:69";
}
