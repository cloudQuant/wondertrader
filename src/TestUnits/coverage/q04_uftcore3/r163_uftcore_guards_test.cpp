/*!
 * \file r163_uftcore_guards_test.cpp
 * \brief 第二十四波 R163 - WtUftCore 剩余公开面（守卫/生命周期/风控限额）
 *
 * 覆盖（只读生产代码）:
 *  - WtUftEngine.cpp:179-183   get_tick_slice 在 return NULL 之后仍留有数据层调用（死代码）
 *  - WtUftEngine.cpp:209-243   get_kline_slice 同上，订阅表 _bar_sub_map 的唯一写入点不可达
 *  - WtUftEngine.cpp:584-603   on_bar 按 key 查订阅表；无订阅时只走空循环
 *  - WtUftEngine.cpp:118-168   品种/合约/时段查询的 NULL 早退
 *  - EventNotifier.cpp:58-112  init 的 inactive / 模块缺失 / 正常挂载三分支
 *  - EventNotifier.cpp:114-228 五个 notify 入口的 `_mq_sid == 0` 与空载荷早退
 *  - TraderAdapter.cpp:103-206 init/initExt 的 NULL 参数、无 module 早退
 *  - TraderAdapter.cpp:1857-1968 checkCancelLimits/checkOrderLimits 总量限额与排除队列
 *
 * 说明（死代码与产品缺陷）:
 *  - EventNotifier 的 worker 在首轮 run_one 时因为没有 outstanding work 会把
 *    io_context 永久 stop（BUG-R163-001），因此 notify 的任务体（L121-227）与
 *    tradeToJson/orderToJson（L230-303）在本构建里不可达；本文件的 DISABLED_
 *    用例保留“应当投递”的期望，不把失败断言转绿。
 *  - TraderAdapter 的撤单/下单时间窗分支（L1878-1902/L1940-1964）依赖
 *    _order_time_cache/_cancel_time_cache，而两处写入在 L340/L392/L420 已注释，
 *    属既有死分支（q04 台账 BUG-Q04-004），本轮不重复登记。
 *
 * 边界: 不连柜台、不建 socket/shm；EventNotifier 的 url 固定 inproc://，
 *       析构 join 其 2ms worker；临时目录只落系统 temp 并在 TearDown 清理。
 */
#include "r123_uft_common.h"

#include "WtUftCore/EventNotifier.h"
#include "WtUftCore/WtUftDtMgr.h"
#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/WtHelper.h"

#include "WTSUtils/WTSCfgLoader.h"

#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"

#include <gtest/gtest.h>

#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
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
	return "inproc://r163_uft_notifier_" + std::to_string((long)::getpid())
		+ "_" + std::to_string(++seq);
}

WTSVariant* makeNotifierCfg(bool active)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("active", active);
	cfg->append("url", nextUrl().c_str());
	return cfg;
}

void writeText(const std::string& path, const std::string& body)
{
	std::ofstream ofs(path.c_str(), std::ios::binary | std::ios::trunc);
	ofs << body;
}

} // namespace

// WtUftEngine.cpp:118-168 + :179-183 + :209-243 + :584-603
TEST(R163UftEngine, DeadSliceAccessorsAndEmptyBarDispatch)
{
	r123::SessionBaseDataMgr bd;
	WTSSessionInfo* session = WTSSessionInfo::create("R163ALL", "r163 allday", 0);
	ASSERT_NE(nullptr, session);
	session->addTradingSection(0, 2400);
	bd._session = session;

	{
		WtUftDtMgr dtMgr;
		WtUftEngine engine;
		engine.init(nullptr, &bd, &dtMgr, nullptr);
		engine.set_date_time(20240102, 930, 1500, 931);
		engine.set_trading_date(20240102);

		EXPECT_EQ(20240102u, engine.get_date());
		EXPECT_EQ(930u, engine.get_min_time());
		EXPECT_EQ(931u, engine.get_raw_time());
		EXPECT_EQ(1500u, engine.get_secs());
		EXPECT_EQ(20240102u, engine.get_trading_date());

		// 179-183: 两个切片接口在首行 return NULL，后面的数据层调用是死代码
		EXPECT_EQ(nullptr, engine.get_tick_slice(1, "SHFE.ag2406", 10));
		EXPECT_EQ(nullptr, engine.get_kline_slice(1, "SHFE.ag2406", "m", 10, 5));
		EXPECT_EQ(nullptr, engine.get_kline_slice(1, "SHFE.ag2406", "m", 10, 2));
		EXPECT_EQ(nullptr, engine.get_kline_slice(1, "SHFE.ag2406", "d", 10, 1));
		EXPECT_EQ(nullptr, engine.get_last_tick(1, "SHFE.ag2406"));

		// 121-129/140-167: 合约不存在时品种/合约/按时段查询均返回 NULL
		EXPECT_EQ(nullptr, engine.get_commodity_info("SHFE.ag2406"));
		EXPECT_EQ(nullptr, engine.get_contract_info("SHFE.ag2406"));
		EXPECT_EQ(nullptr, engine.get_session_info("SHFE.ag2406", true));
		EXPECT_EQ(session, engine.get_session_info("R163ALL", false));

		// 584-603: _bar_sub_map 的写入点在 209-243 的死代码里，on_bar 只能走空循环
		engine.sub_tick(1, "SHFE.ag2406");
		engine.on_tick("SHFE.ag2406", nullptr);
		engine.on_bar("SHFE.ag2406", "m", 5, nullptr);
		engine.on_bar("SHFE.ag2406", "d", 1, nullptr);
		EXPECT_EQ(nullptr, engine.getContext(1));
	}

	session->release();
}

// EventNotifier.cpp:58-112 + :114-228
TEST(R163UftNotifier, InactiveMissingModuleAndGuardLifecycle)
{
	// 58-61: active=false 直接返回 false，不碰 URL/模块
	{
		EventNotifier notifier;
		WTSVariant* cfg = makeNotifierCfg(false);
		EXPECT_FALSE(notifier.init(cfg));
		cfg->release();
	}

	// 63-76: 模块目录里没有 WtMsgQue 时 load_library 失败
	{
		q04::TempRoot tmp("r163_no_mq");
		InstDirGuard instdir(tmp.dir().string());
		EventNotifier notifier;
		WTSVariant* cfg = makeNotifierCfg(true);
		EXPECT_FALSE(notifier.init(cfg));
		cfg->release();
	}

	// 114-228: 未 init 的对象 _mq_sid == 0，五个入口都必须在触碰载荷前返回
	{
		WTSTradeInfo* trd = WTSTradeInfo::create("ag2406", "SHFE");
		ASSERT_NE(nullptr, trd);
		WTSOrderInfo* ord = WTSOrderInfo::create();
		ASSERT_NE(nullptr, ord);
		const uint32_t trdRefs = trd->retainCount();
		const uint32_t ordRefs = ord->retainCount();

		{
			EventNotifier notifier;
			notifier.notify_log("R163", "not inited");
			notifier.notify_event("not inited");
			notifier.notify("r163", "not inited");
			notifier.notify("r163", 1, "SHFE.ag2406", trd);
			notifier.notify("r163", 2, "SHFE.ag2406", ord);
		}

		EXPECT_EQ(trdRefs, trd->retainCount());
		EXPECT_EQ(ordRefs, ord->retainCount());
		trd->release();
		ord->release();
	}

	// 78-111: 模块可加载时 init 成功；空载荷必须在 retain 之前返回
	const std::string bin = binDir();
	if (bin.empty() || !std::filesystem::exists(bin + "libWtMsgQue.dylib"))
		GTEST_SKIP() << "libWtMsgQue.dylib not found in this build";

	InstDirGuard instdir(bin);
	EventNotifier notifier;
	WTSVariant* cfg = makeNotifierCfg(true);
	ASSERT_TRUE(notifier.init(cfg));
	cfg->release();

	WTSTradeInfo* trd = WTSTradeInfo::create("ag2406", "SHFE");
	ASSERT_NE(nullptr, trd);
	WTSOrderInfo* ord = WTSOrderInfo::create();
	ASSERT_NE(nullptr, ord);
	const uint32_t trdRefs = trd->retainCount();
	const uint32_t ordRefs = ord->retainCount();

	notifier.notify_log("R163", "guard line");
	notifier.notify_event("guard line");
	notifier.notify("r163", "guard line");
	notifier.notify("r163", 3, "SHFE.ag2406", (WTSTradeInfo*)nullptr);
	notifier.notify("r163", 4, "SHFE.ag2406", (WTSOrderInfo*)nullptr);

	EXPECT_EQ(trdRefs, trd->retainCount());
	EXPECT_EQ(ordRefs, ord->retainCount());
	trd->release();
	ord->release();
	// 析构会 join worker；worker 首轮已 stop io_context（BUG-R163-001）
}

/* 产品缺陷 BUG-R163-001 的可执行证据：worker 在无 outstanding work 时调用
 * run_one()，scheduler 会把 io_context 永久 stop，此后 notify 投递的任务永不执行。
 * 生产正确行为应是 init 后 500ms 内把 retain 降回 1；当前恒为 2，因此按硬约束
 * 保留 DISABLED_，不把失败断言改成成功断言。 */
TEST(R163UftNotifier, DISABLED_WorkerDeliversPostedTradeNotification)
{
	const std::string bin = binDir();
	if (bin.empty() || !std::filesystem::exists(bin + "libWtMsgQue.dylib"))
		GTEST_SKIP() << "libWtMsgQue.dylib not found in this build";

	InstDirGuard instdir(bin);
	EventNotifier notifier;
	WTSVariant* cfg = makeNotifierCfg(true);
	ASSERT_TRUE(notifier.init(cfg));
	cfg->release();

	WTSTradeInfo* trd = WTSTradeInfo::create("ag2406", "SHFE");
	ASSERT_NE(nullptr, trd);
	notifier.notify("r163", 5, "SHFE.ag2406", trd);
	ASSERT_EQ(2u, trd->retainCount());

	for (int i = 0; i < 100 && trd->retainCount() > 1; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	EXPECT_EQ(1u, trd->retainCount());

	while (trd->retainCount() > 0)
		trd->release();
}

// TraderAdapter.cpp:103-206 + :1857-1968
TEST(R163UftTrader, InitGuardsAndRiskTotalLimits){
	q04::TempRoot tmp("r163_risk");
	q04::FakeTraderApi api;
	q04::FakeBaseDataMgr bd;
	ActionPolicyMgr policy;
	q04::FakeSink sink;

	// 105-106: NULL 参数不得初始化成功。
	// initExt 的 NULL api 守卫缺失（BUG-R175-003），调用必崩，见下方 DISABLED_ 用例。
	{
		TraderAdapter guard;
		EXPECT_FALSE(guard.init("r163", nullptr, &bd, &policy));
	}

	const std::string riskPath = tmp.file("trader.json");
	writeText(riskPath,
		"{\"riskmon\":{\"active\":true,\"policy\":{\"default\":{"
		"\"cancel_total_limits\":1,\"cancel_times_boundary\":3,\"cancel_stat_timespan\":60,"
		"\"order_total_limits\":1000000,\"order_times_boundary\":5,\"order_stat_timespan\":30}}}}");
	WTSVariant* cfg = WTSCfgLoader::load_from_file(riskPath.c_str());
	ASSERT_NE(nullptr, cfg);

	TraderAdapter adapter;
	// 162-163: 无 module 字段，风控表装配完成后返回 false
	EXPECT_FALSE(adapter.init("r163", cfg, &bd, &policy));
	cfg->release();

	const std::string policyPath = tmp.file("policy.json");
	writeText(policyPath, q04::policyJsonOpenCloseLimited());
	ASSERT_TRUE(policy.init(policyPath.c_str()));
	bd.add("SHFE", "ag", "ag2406");
	ASSERT_TRUE(adapter.initExt("r163", &api, &bd, &policy));
	adapter.addSink(&sink);
	ASSERT_TRUE(adapter.run());
	adapter.onLoginResult(true, "ok", 20240315);

	EXPECT_TRUE(adapter.checkCancelLimits("SHFE.ag2406"));
	EXPECT_TRUE(adapter.checkOrderLimits("SHFE.ag2406"));

	// 1866-1873: 一张已撤销委托让 total_cancels 前进到限额 1
	WTSOrderInfo* ord = WTSOrderInfo::create();
	ASSERT_NE(nullptr, ord);
	ord->setCode("ag2406");
	ord->setExchange("SHFE");
	ord->setDirection(WDT_LONG);
	ord->setOffsetType(WOT_OPEN);
	ord->setVolume(1);
	ord->setVolLeft(1);
	ord->setOrderState(WOS_Canceled);
	// WTSOrderStruct 构造不初始化 orderFlag（WTSTradeDef.hpp:318-328），
	// 不显式设置会按非普通单计入 auto_cancels，total_cancels 恒为 0。
	ord->setOrderFlag(WOF_NOR);
	ord->setOrderID("R163-RK-1");
	adapter.onPushOrder(ord);
	ord->release();

	EXPECT_FALSE(adapter.checkCancelLimits("SHFE.ag2406"));
	// 排除队列优先于任何统计：下单检查同样被挡
	EXPECT_FALSE(adapter.checkOrderLimits("SHFE.ag2406"));
}

/* BUG-R175-003 的可执行证据：WtUftCore::TraderAdapter::initExt 在
 * TraderAdapter.cpp:98 直接 `api->init(NULL)`，没有像 init() 那样先判
 * params==NULL。按“NULL api 不得初始化成功”的契约，启用即 SIGSEGV，故 DISABLED_。 */
TEST(R163UftTrader, DISABLED_InitExtWithNullApiCrashesInsteadOfReturningFalse)
{
	q04::FakeBaseDataMgr bd;
	ActionPolicyMgr policy;
	TraderAdapter guard;
	EXPECT_FALSE(guard.initExt("r163", nullptr, &bd, &policy));
}
