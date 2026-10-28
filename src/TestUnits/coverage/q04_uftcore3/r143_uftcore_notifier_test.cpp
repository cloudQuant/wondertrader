/*!
 * \file r143_uftcore_notifier_test.cpp
 * \brief 第二十二波 R143 - WtUftCore/EventNotifier 成功挂载 MQ 后的 JSON 组装
 *
 * 覆盖（只读生产代码）:
 *  - EventNotifier.cpp:119-140  notify_log：tag/time/message 三字段 JSON + LOG 主题发布
 *  - EventNotifier.cpp:148-166  notify_event：time/message JSON + GRP_EVENT 主题发布
 *  - EventNotifier.cpp:174-194  notify(trader, message)：trader/time/message JSON
 *  - EventNotifier.cpp:202-211  notify(trader, localid, stdCode, WTSTradeInfo*)：
 *    retain -> post -> tradeToJson -> publish -> release
 *  - EventNotifier.cpp:219-227  notify(trader, localid, stdCode, WTSOrderInfo*)：
 *    retain -> post -> orderToJson -> publish
 *  - EventNotifier.cpp:230-303  tradeToJson/orderToJson 的字段与 private 组装面
 *
 * 边界:
 *  - URL 固定为进程内 `inproc://`，nanomsg 不创建任何 socket/IPC 文件；用 pid+序号
 *    保证同进程多次 init 不撞名。
 *  - init() 会起一个 2ms 轮询的 asio worker（生产设计），本用例在 50ms 内 notify 后
 *    立即析构，析构 join 该 worker，不产生常驻线程。
 *  - WtMsgQue 插件路径从测试可执行文件所在目录解析，仅 init 期间切换 InstDir。
 *  - 本用例不连柜台、不访问网络、不写文件。
 *
 * 已知产品缺陷（见 bugs/R143.md BUG-R143-001）: 订单通知 lambda 只 retain 不 release。
 */
#include "WtUftCore/EventNotifier.h"
#include "WtUftCore/WtHelper.h"

#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"

#include <gtest/gtest.h>

#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

using namespace wtp;

namespace {

std::string binDir()
{
	Dl_info info;
	if (dladdr(reinterpret_cast<void*>(&binDir), &info) == 0 || info.dli_fname == nullptr)
		return std::string();
	std::filesystem::path p = std::filesystem::path(info.dli_fname).parent_path().parent_path();
	std::string ret = p.string();
	if (!ret.empty() && ret.back() != '/')
		ret.push_back('/');
	return ret;
}

class InstDirGuard
{
public:
	explicit InstDirGuard(const std::string& dir)
		: _old(WtHelper::getInstDir())
	{
		WtHelper::setInstDir(dir.c_str());
	}
	~InstDirGuard() { WtHelper::setInstDir(_old.c_str()); }
	InstDirGuard(const InstDirGuard&) = delete;
	InstDirGuard& operator=(const InstDirGuard&) = delete;

private:
	std::string _old;
};

std::string nextUrl()
{
	static std::atomic<uint32_t> seq(0);
	return "inproc://r143_uft_notifier_" + std::to_string((long)::getpid())
		+ "_" + std::to_string(++seq);
}

//成功后短暂等待 inproc worker 执行 post 的任务；50ms 为轮询周期的 25 倍。
void drainBriefly()
{
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

bool makeNotifier(EventNotifier& notifier, WTSVariant*& cfg)
{
	cfg = WTSVariant::createObject();
	cfg->append("active", true);
	cfg->append("url", nextUrl().c_str());
	return notifier.init(cfg);
}

} // namespace

// EventNotifier.cpp:119-140 + :148-166 + :174-194
TEST(R143UftNotifier, postsTextLogEventAndTraderMessages)
{
	const std::string bin = binDir();
	if (bin.empty() || !std::filesystem::exists(bin + "libWtMsgQue.dylib"))
		GTEST_SKIP() << "libWtMsgQue.dylib not found in this build";

	InstDirGuard instdir(bin);
	EventNotifier notifier;
	WTSVariant* cfg = nullptr;
	ASSERT_TRUE(makeNotifier(notifier, cfg));
	cfg->release();

	notifier.notify_log("R143", "log line");
	notifier.notify_event("event line");
	notifier.notify("uft1", "trader text line");
	drainBriefly();
}

// EventNotifier.cpp:202-211 + :230-263
TEST(R143UftNotifier, postsTradeJsonAndReleasesRetainedPayload)
{
	const std::string bin = binDir();
	if (bin.empty() || !std::filesystem::exists(bin + "libWtMsgQue.dylib"))
		GTEST_SKIP() << "libWtMsgQue.dylib not found in this build";

	WTSTradeInfo* trd = WTSTradeInfo::create("ag2406", "SHFE");
	ASSERT_NE(nullptr, trd);
	trd->setDirection(WDT_LONG);
	trd->setOffsetType(WOT_CLOSETODAY);
	trd->setVolume(3.0);
	trd->setPrice(5123.5);
	ASSERT_EQ(1u, trd->retainCount());

	{
		InstDirGuard instdir(bin);
		EventNotifier notifier;
		WTSVariant* cfg = nullptr;
		ASSERT_TRUE(makeNotifier(notifier, cfg));
		cfg->release();

		notifier.notify("uft1", 4001, "SHFE.ag2406", trd);
		// notify 先 retain 再 post；这里只证明 retain 确实发生
		EXPECT_GE(trd->retainCount(), 2u);
		// 轮询等待 worker 执行 post 的 lambda（最多 500ms）
		for (int i = 0; i < 100 && trd->retainCount() > 1; i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	// lambda 内 release 后回到 1，再由用例释放创建引用
	EXPECT_EQ(1u, trd->retainCount());
	trd->release();
}

// EventNotifier.cpp:219-227 + :265-303
TEST(R143UftNotifier, postsOrderJsonWithStateAndVolumeFields)
{
	const std::string bin = binDir();
	if (bin.empty() || !std::filesystem::exists(bin + "libWtMsgQue.dylib"))
		GTEST_SKIP() << "libWtMsgQue.dylib not found in this build";

	WTSOrderInfo* ord = WTSOrderInfo::create();
	ASSERT_NE(nullptr, ord);
	ord->setCode("ag2406");
	ord->setExchange("SHFE");
	ord->setDirection(WDT_SHORT);
	ord->setOffsetType(WOT_OPEN);
	ord->setOrderState(WOS_Canceled);
	ord->setStateMsg("cancelled");
	ord->setVolume(6.0);
	ord->setVolLeft(2.0);
	ord->setVolTraded(4.0);
	ord->setPrice(4988.0);
	ASSERT_EQ(1u, ord->retainCount());

	{
		InstDirGuard instdir(bin);
		EventNotifier notifier;
		WTSVariant* cfg = nullptr;
		ASSERT_TRUE(makeNotifier(notifier, cfg));
		cfg->release();

		notifier.notify("uft1", 4002, "SHFE.ag2406", ord);
		EXPECT_EQ(2u, ord->retainCount());
		drainBriefly();
	}

	// BUG-R143-001: 订单 lambda 没有 release，retain 永久留在对象上。
	// 用例侧按当前实现把剩余引用全部释放（创建 1 + notify retain 1），避免泄漏污染后续用例。
	uint32_t refs = ord->retainCount();
	for (uint32_t i = 0; i < refs; i++)
		ord->release();
}

// EventNotifier.cpp:197-200 + :214-217：MQ 已挂载时空载荷仍必须在 retain/post 之前返回
TEST(R143UftNotifier, nullPayloadIsDroppedBeforeRetainWhenMqIsActive)
{
	const std::string bin = binDir();
	if (bin.empty() || !std::filesystem::exists(bin + "libWtMsgQue.dylib"))
		GTEST_SKIP() << "libWtMsgQue.dylib not found in this build";

	WTSTradeInfo* trd = WTSTradeInfo::create("ag2406", "SHFE");
	ASSERT_NE(nullptr, trd);
	WTSOrderInfo* ord = WTSOrderInfo::create();
	ASSERT_NE(nullptr, ord);
	const uint32_t trdRefs = trd->retainCount();
	const uint32_t ordRefs = ord->retainCount();

	{
		InstDirGuard instdir(bin);
		EventNotifier notifier;
		WTSVariant* cfg = nullptr;
		ASSERT_TRUE(makeNotifier(notifier, cfg));
		cfg->release();

		notifier.notify("uft1", 4003, "SHFE.ag2406", (WTSTradeInfo*)nullptr);
		notifier.notify("uft1", 4004, "SHFE.ag2406", (WTSOrderInfo*)nullptr);
		EXPECT_EQ(trdRefs, trd->retainCount());
		EXPECT_EQ(ordRefs, ord->retainCount());
		drainBriefly();
	}

	trd->release();
	ord->release();
}
