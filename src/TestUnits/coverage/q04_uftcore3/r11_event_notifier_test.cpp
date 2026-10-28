/*!
 * \file r11_event_notifier_test.cpp
 * \brief 覆盖率迭代01 第七波 R11(WtUftCore) - EventNotifier 未挂载 MQ 时的静默分支
 *
 * 覆盖: 构造/析构、init() 的 active=false 分支、init() 的"模块装载失败"分支,
 * 以及 _mq_sid==0 时 5 个 notify 入口的提前返回。
 *
 * 刻意不启动任何线程: init() 成功时才会创建 _worker(内部 sleep 2ms 轮询 + io_context),
 * 而本用例让 init() 一定失败, 因此 _asyncio 不会被 run_one, 也不会真的建立 MQ server。
 * tradeToJson/orderToJson 是 private 且只能由 _mq_sid!=0 的 post lambda 调用,
 * 属于需要接缝的面, 已登记到 bugs/R11_wtuftcore.md。
 */
#include <boost/noncopyable.hpp>

#include "WtUftCore/EventNotifier.h"
#include "WtUftCore/WtHelper.h"

#include "Includes/WTSMarcos.h"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "Share/DLLHelper.hpp"
#include "Share/StdUtils.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace wtp;

namespace {

//把 init() 里"能否真的装载 WtMsgQue"的判断原样复刻一遍, 用来决定本用例是否可做
//只要返回 true, 就证明生产代码里 load_library 必然返回 NULL, 不会建立真实 MQ server
bool mqModuleAbsent()
{
	std::string module = DLLHelper::wrap_module("WtMsgQue", "lib");
	std::string dllpath = WtHelper::getCWD() + module;
	if (StdFile::exists(dllpath.c_str()))
		return false;
	dllpath = WtHelper::getInstDir() + module;
	return !StdFile::exists(dllpath.c_str());
}

//////////////////////////////////////////////////////////////////////////
TEST(R11EventNotifier, initDisabledByInactiveFlag)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("active", false);
	cfg->append("url", "udp://127.0.0.1:39999");

	EventNotifier notifier;
	//active=false 必须在读 url 之前就返回 false
	EXPECT_FALSE(notifier.init(cfg));

	//此时 _mq_sid 仍是 0(构造函数初始化的), 五个入口都必须静默返回
	notifier.notify_log("R11", "log line");
	notifier.notify_event("event line");
	notifier.notify("uft1", "trader message");
	notifier.notify("uft1", 1001, "SHFE.ag2406", (WTSTradeInfo*)nullptr);
	notifier.notify("uft1", 1002, "SHFE.ag2406", (WTSOrderInfo*)nullptr);
	//走到这里没有崩溃即为通过; 下面的断言是可证伪的: init 失败后再次 init 仍失败
	EXPECT_FALSE(notifier.init(cfg));

	cfg->release();
}

//////////////////////////////////////////////////////////////////////////
TEST(R11EventNotifier, initFailsWhenMqModuleMissing)
{
	if (!mqModuleAbsent())
		GTEST_SKIP() << "WtMsgQue is loadable here, skip to avoid creating a real MQ server";

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("active", true);
	cfg->append("url", "udp://127.0.0.1:39998");

	EventNotifier notifier;
	//装载失败 -> 返回 false, 且不会创建 worker 线程
	EXPECT_FALSE(notifier.init(cfg));
	EXPECT_TRUE(mqModuleAbsent());

	notifier.notify_log("R11", "still silent");
	notifier.notify_event("still silent");
	cfg->release();
}

//////////////////////////////////////////////////////////////////////////
//_mq_sid==0 时, 传入有效指针也必须被第一条 guard 拦下(不会 retain/release 悬空对象)
TEST(R11EventNotifier, notifyGuardsBeforeTouchingPayload)
{
	WTSTradeInfo* trd = WTSTradeInfo::create("ag2406", "SHFE");
	ASSERT_NE(nullptr, trd);
	trd->setDirection(WDT_LONG);
	trd->setOffsetType(WOT_OPEN);
	trd->setVolume(5.0);
	trd->setPrice(6000.25);

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("active", false);
	EventNotifier notifier;
	ASSERT_FALSE(notifier.init(cfg));

	//payload 非空但 _mq_sid==0 -> 依然不发送; 引用计数不应被 notify 改变
	uint32_t before = trd->retainCount();
	notifier.notify("uft1", 2001, "SHFE.ag2406", trd);
	EXPECT_EQ(before, trd->retainCount());

	WTSOrderInfo* ord = WTSOrderInfo::create();
	ASSERT_NE(nullptr, ord);
	before = ord->retainCount();
	notifier.notify("uft1", 2002, "SHFE.ag2406", ord);
	EXPECT_EQ(before, ord->retainCount());

	trd->release();
	ord->release();
	cfg->release();
}

} // namespace
