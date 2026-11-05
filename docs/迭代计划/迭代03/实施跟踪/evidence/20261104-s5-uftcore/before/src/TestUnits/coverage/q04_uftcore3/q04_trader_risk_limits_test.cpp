/*!
 * \file q04_trader_risk_limits_test.cpp
 * \brief TraderAdapter 流量风控参数装配与限额判定(迭代01 第五波 q04)
 *
 * 覆盖: riskmon 初始化、check* 总量/频率判定，以及真实 adapter 下单/撤单入口。
 * 频率回归通过动态假 API 记录 SDK 调用，覆盖成功/失败返回、窗口边界和 inactive 配置。
 */
#include "q04_trader_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtUftCore/WtHelper.h"

#include <algorithm>
#include <filesystem>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifndef WT_UFT_LIFECYCLE_FAKE_PLUGIN_PATH
#error "CoverageTests_uftcore must wire the UFT lifecycle fake plugin"
#endif

using namespace wtp;

namespace {

const char* kAg = "SHFE.ag2406";

//把 riskmon 配置写成 json 文件, 返回 WTSVariant 根对象(引用由调用方释放)
WTSVariant* writeRiskCfg(q04::TempRoot& tmp, const char* name, const std::string& json)
{
	const std::string path = tmp.file(name);
	tmp.write(path, json);
	return WTSCfgLoader::load_from_file(path.c_str());
}

//default 组的总量限额由参数决定, ag24 组刻意设得更严
std::string riskJson(uint32_t defaultCancelLimit, uint32_t defaultOrderLimit,
	uint32_t agCancelLimit = 1)
{
	return std::string("{\"module\":\"N03RiskFakeTrader\",\"riskmon\":{\"active\":true,\"policy\":{")
		+ "\"default\":{\"cancel_total_limits\":" + std::to_string(defaultCancelLimit)
		+ ",\"cancel_times_boundary\":3,\"cancel_stat_timespan\":60,"
		+ "\"order_total_limits\":" + std::to_string(defaultOrderLimit)
		+ ",\"order_times_boundary\":5,\"order_stat_timespan\":30},"
		+ "\"ag24\":{\"cancel_total_limits\":" + std::to_string(agCancelLimit)
		+ ",\"cancel_times_boundary\":3,\"cancel_stat_timespan\":60,"
		+ "\"order_total_limits\":1000,\"order_times_boundary\":5,\"order_stat_timespan\":30}"
		+ "}}}";
}

std::string customOnlyRiskJson(uint32_t agCancelLimit, uint32_t agOrderLimit)
{
	return std::string("{\"module\":\"N03RiskFakeTrader\",\"riskmon\":{\"active\":true,\"policy\":{")
		+ "\"ag24\":{\"cancel_total_limits\":" + std::to_string(agCancelLimit)
		+ ",\"cancel_times_boundary\":3,\"cancel_stat_timespan\":60,"
		+ "\"order_total_limits\":" + std::to_string(agOrderLimit)
		+ ",\"order_times_boundary\":5,\"order_stat_timespan\":30}"
		+ "}}}";
}

std::string frequencyRiskJson(bool active, uint32_t orderBoundary, uint32_t orderWindow,
	uint32_t cancelBoundary, uint32_t cancelWindow, int insertRet = 0, int actionRet = 0)
{
	return std::string("{\"module\":\"N03RiskFakeTrader\",\"fake_order_insert_ret\":")
		+ std::to_string(insertRet) + ",\"fake_order_action_ret\":" + std::to_string(actionRet)
		+ ",\"riskmon\":{\"active\":" + (active ? "true" : "false") + ",\"policy\":{\"default\":{"
		+ "\"cancel_total_limits\":0,\"cancel_times_boundary\":" + std::to_string(cancelBoundary)
		+ ",\"cancel_stat_timespan\":" + std::to_string(cancelWindow)
		+ ",\"order_total_limits\":0,\"order_times_boundary\":" + std::to_string(orderBoundary)
		+ ",\"order_stat_timespan\":" + std::to_string(orderWindow) + "}}}}";
}

void setTestEnvironment(const char* name, const char* value)
{
#if defined(_WIN32)
	_putenv_s(name, value == nullptr ? "" : value);
#else
	if (value == nullptr)
		unsetenv(name);
	else
		setenv(name, value, 1);
#endif
}

class ScopedRiskPluginLog
{
public:
	explicit ScopedRiskPluginLog(const std::filesystem::path& path)
		: _hadPrevious(std::getenv("WT_UFT_LIFECYCLE_LOG") != nullptr)
		, _previous(_hadPrevious ? std::getenv("WT_UFT_LIFECYCLE_LOG") : "")
		, _path(path)
	{
		std::filesystem::remove(_path);
		setTestEnvironment("WT_UFT_LIFECYCLE_LOG", _path.string().c_str());
	}

	~ScopedRiskPluginLog()
	{
		setTestEnvironment("WT_UFT_LIFECYCLE_LOG", _hadPrevious ? _previous.c_str() : nullptr);
	}

	std::vector<std::string> events() const
	{
		std::vector<std::string> result;
		std::ifstream input(_path);
		std::string line;
		while (std::getline(input, line))
			result.push_back(line);
		return result;
	}

private:
	bool _hadPrevious;
	std::string _previous;
	std::filesystem::path _path;
};

std::size_t countPluginEvents(const ScopedRiskPluginLog& log, const char* event)
{
	const std::vector<std::string> events = log.events();
	return static_cast<std::size_t>(std::count(events.begin(), events.end(), event));
}

std::string riskFakeLibraryName()
{
#if defined(_WIN32)
	return "N03RiskFakeTrader.dll";
#elif defined(__APPLE__)
	return "libN03RiskFakeTrader.dylib";
#else
	return "libN03RiskFakeTrader.so";
#endif
}

bool stageRiskFakeTrader(const q04::TempRoot& tmp)
{
	const std::filesystem::path source(WT_UFT_LIFECYCLE_FAKE_PLUGIN_PATH);
	const std::filesystem::path tradersDir = tmp.dir() / "traders";
	std::error_code ec;
	std::filesystem::create_directories(tradersDir, ec);
	if (ec)
		return false;
	std::filesystem::copy_file(source, tradersDir / riskFakeLibraryName(),
		std::filesystem::copy_options::overwrite_existing, ec);
	return !ec;
}

class ScopedRiskInstDir
{
public:
	explicit ScopedRiskInstDir(const std::filesystem::path& path)
		: _old(WtHelper::getInstDir())
	{
		std::string instDir = path.string();
		if (!instDir.empty() && instDir.back() != '/' && instDir.back() != '\\')
			instDir.push_back('/');
		WtHelper::setInstDir(instDir.c_str());
	}

	~ScopedRiskInstDir()
	{
		WtHelper::setInstDir(_old.c_str());
	}

	ScopedRiskInstDir(const ScopedRiskInstDir&) = delete;
	ScopedRiskInstDir& operator=(const ScopedRiskInstDir&) = delete;

private:
	std::string _old;
};

struct RiskEnv
{
	q04::TempRoot			_tmp{ "risk" };
	ScopedRiskInstDir		_instDir{ _tmp.dir() };
	q04::FakeBaseDataMgr	_bd;
	ActionPolicyMgr			_policy;
	q04::FakeSink			_sink;
	WTSVariant*				_cfg = nullptr;
	TraderAdapter			_adapter;
	bool					_pluginStaged = false;

	~RiskEnv()
	{
		if (_cfg != nullptr)
			_cfg->release();
	}

	RiskEnv() { _pluginStaged = stageRiskFakeTrader(_tmp); }

	RiskEnv(const RiskEnv&) = delete;
	RiskEnv& operator=(const RiskEnv&) = delete;

	// 通过真实动态模块入口成功初始化，避免在失败 init 回滚后误测残留风控表。
	bool loadRisk(const std::string& json)
	{
		if (!_pluginStaged)
			return false;
		_cfg = writeRiskCfg(_tmp, "trader.json", json);
		if (_cfg == nullptr)
			return false;
		return _adapter.init("uft1", _cfg, &_bd, &_policy);
	}

	void finish()
	{
		const std::string path = _tmp.file("policy.json");
		_tmp.write(path, q04::policyJsonOpenCloseLimited());
		EXPECT_TRUE(_policy.init(path.c_str()));
		_bd.add("SHFE", "ag", "ag2406");
		_adapter.addSink(&_sink);
		EXPECT_TRUE(_adapter.run());
		_adapter.onLoginResult(true, "ok", 20240315);
	}

	//推一张已撤销的普通买单, 让 total_cancels()/total_orders() 前进
	void pushCanceledBuyOrder(const char* orderid)
	{
		WTSOrderInfo* ord = WTSOrderInfo::create();
		ord->setCode("ag2406");
		ord->setExchange("SHFE");
		ord->setDirection(WDT_LONG);
		ord->setOffsetType(WOT_OPEN);
		ord->setOrderFlag(WOF_NOR);
		ord->setVolume(1);
		ord->setVolLeft(1);
		ord->setOrderState(WOS_Canceled);
		ord->setOrderID(orderid);
		_adapter.onPushOrder(ord);
		ord->release();
	}

	void pushAliveOrder(uint32_t localid, const char* orderid, const char* code = "ag2406",
		const char* exchg = "SHFE", const char* product = "ag")
	{
		if (_bd.getContract(code, exchg) == nullptr)
			_bd.add(exchg, product, code);
		WTSOrderInfo* ord = WTSOrderInfo::create();
		ord->setCode(code);
		ord->setExchange(exchg);
		ord->setDirection(WDT_LONG);
		ord->setOffsetType(WOT_OPEN);
		ord->setOrderFlag(WOF_NOR);
		ord->setVolume(1);
		ord->setVolLeft(1);
		ord->setOrderState(WOS_NotTraded_Queuing);
		ord->setOrderID(orderid);
		const std::string userTag = std::string("wtp.uft1.") + std::to_string(localid);
		ord->setUserTag(userTag.c_str());
		WTSArray* orders = WTSArray::create();
		orders->append(ord);
		_adapter.onRspOrders(orders);
		orders->release();
		ord->release();
	}
};

}	// namespace

TEST(q04_trader_risk, riskmon_parsing_switches_the_monitor_on_and_off)
{
	RiskEnv on;
	//default 撤单限额 1 / 下单限额 1000000
	ASSERT_TRUE(on.loadRisk(riskJson(1, 1000000)));
	on.finish();
	//风控激活后, 未被排除且无统计数据的合约仍然通过限额检查
	EXPECT_TRUE(on._adapter.checkCancelLimits(kAg));
	EXPECT_TRUE(on._adapter.checkOrderLimits(kAg));

	RiskEnv off;
	//active=false 只记日志
	ASSERT_TRUE(off.loadRisk(
		R"({"module":"N03RiskFakeTrader","riskmon":{"active":false,"policy":{"default":{"cancel_total_limits":1}}}})"));
	off.finish();
	EXPECT_TRUE(off._adapter.checkCancelLimits(kAg));
	EXPECT_TRUE(off._adapter.checkOrderLimits(kAg));

	//完全没有 riskmon 配置
	RiskEnv none;
	ASSERT_TRUE(none.loadRisk(R"({"module":"N03RiskFakeTrader","user":"u"})"));
	none.finish();
	EXPECT_TRUE(none._adapter.checkCancelLimits(kAg));
	EXPECT_TRUE(none._adapter.checkOrderLimits(kAg));
}

TEST(q04_trader_risk, cancel_total_limit_moves_the_code_into_the_excluding_list)
{
	RiskEnv env;
	//default 的撤单总量限额 = 1
	ASSERT_TRUE(env.loadRisk(riskJson(1, 1000000)));
	env.finish();

	//尚无统计数据: 限额判定通过
	EXPECT_TRUE(env._adapter.checkCancelLimits(kAg));

	//一张已撤销的普通买单
	env.pushCanceledBuyOrder("RK-1");
	EXPECT_EQ(2u, env._adapter.getInfos(kAg)) << "撤单统计 +1, 首次推送信息量 +1";

	//撤单总量达到 1 -> 进入排除队列
	EXPECT_FALSE(env._adapter.checkCancelLimits(kAg));
	//排除后所有检查直接拒绝, 不再读统计
	EXPECT_FALSE(env._adapter.checkCancelLimits(kAg));
	EXPECT_FALSE(env._adapter.checkOrderLimits(kAg));
	//交易开关也一并关闭(isTradeEnabled 因接口不可外部引用而未断言, 见 BUG-Q04-006)
}

TEST(q04_trader_risk, order_total_limit_is_counted_from_order_reports)
{
	RiskEnv env;
	//default 的下单总量限额 = 2
	ASSERT_TRUE(env.loadRisk(
		R"({"module":"N03RiskFakeTrader","riskmon":{"active":true,"policy":{"default":{
		"cancel_total_limits":1000000,"cancel_times_boundary":3,"cancel_stat_timespan":60,
		"order_total_limits":2,"order_times_boundary":5,"order_stat_timespan":30}}}})"));
	env.finish();

	EXPECT_TRUE(env._adapter.checkOrderLimits(kAg));

	//两张委托回报(未撤销), total_orders = 2
	WTSOrderInfo* o1 = WTSOrderInfo::create();
	o1->setCode("ag2406");
	o1->setExchange("SHFE");
	o1->setDirection(WDT_LONG);
	o1->setOffsetType(WOT_OPEN);
	o1->setVolume(1);
	o1->setVolLeft(1);
	o1->setOrderState(WOS_NotTraded_Queuing);
	o1->setOrderID("RQ-1");
	WTSOrderInfo* o2 = WTSOrderInfo::create();
	o2->setCode("ag2406");
	o2->setExchange("SHFE");
	o2->setDirection(WDT_SHORT);
	o2->setOffsetType(WOT_CLOSE);
	o2->setVolume(1);
	o2->setVolLeft(1);
	o2->setOrderState(WOS_NotTraded_Queuing);
	o2->setOrderID("RQ-2");
	WTSArray* ay = WTSArray::create();
	ay->append(o1);
	ay->append(o2);
	env._adapter.onRspOrders(ay);
	ay->release();

	//多头开 + 空头平都算 buy 侧, total_orders = 2 >= 2 -> 排除
	EXPECT_FALSE(env._adapter.checkOrderLimits(kAg));
	//排除后撤单限额同样直接拒绝
	EXPECT_FALSE(env._adapter.checkCancelLimits(kAg));
	o1->release();
	o2->release();
}

TEST(q04_trader_risk, product_cancel_limit_overrides_permissive_default)
{
	RiskEnv env;
	//ag24 组限额 1, default 组放宽到 1000000
	ASSERT_TRUE(env.loadRisk(riskJson(1000000, 1000000, 1)));
	env.finish();
	ASSERT_TRUE(env._adapter.checkCancelLimits(kAg));

	env.pushCanceledBuyOrder("PK-1");
	ASSERT_EQ(2u, env._adapter.getInfos(kAg)) << "一张普通撤单回报应计入订单与撤单信息量";

	//标准码 SHFE.ag2406 应命中 ag24 组；已计入的一次普通撤单达到限额 1。
	EXPECT_FALSE(env._adapter.checkCancelLimits(kAg));
}

TEST(q04_trader_risk, risk_params_default_group_is_used_for_unlisted_products)
{
	RiskEnv env;
	//只有 default 组, 撤单总量限额 1
	ASSERT_TRUE(env.loadRisk(
		R"({"module":"N03RiskFakeTrader","riskmon":{"active":true,"policy":{"default":{
		"cancel_total_limits":1,"cancel_times_boundary":3,"cancel_stat_timespan":60,
		"order_total_limits":1,"order_times_boundary":5,"order_stat_timespan":30}}}})"));
	env.finish();

	//未列出的品种回落到 default: 一张撤单就命中限额
	env.pushCanceledBuyOrder("OD-1");
	EXPECT_FALSE(env._adapter.checkCancelLimits(kAg));
	//同一个被排除的代码在任何限额检查前就会被挡掉
	EXPECT_FALSE(env._adapter.checkOrderLimits(kAg));
}

TEST(q04_trader_risk, custom_only_policy_matches_configured_products_and_leaves_unknown_products_unmonitored)
{
	RiskEnv unknown;
	ASSERT_TRUE(unknown.loadRisk(customOnlyRiskJson(1, 1000)));
	unknown.finish();
	// Existing warning documents that a custom-only policy does not monitor codes
	// without an explicit rule; a miss must return safely without inheriting garbage.
	EXPECT_TRUE(unknown._adapter.checkCancelLimits("SSE.600000"));
	EXPECT_TRUE(unknown._adapter.checkOrderLimits("SSE.600000"));

	RiskEnv customCancel;
	ASSERT_TRUE(customCancel.loadRisk(customOnlyRiskJson(1, 1000)));
	customCancel.finish();
	EXPECT_TRUE(customCancel._adapter.checkCancelLimits(kAg));
	customCancel.pushCanceledBuyOrder("NC-1");
	EXPECT_FALSE(customCancel._adapter.checkCancelLimits(kAg));

	RiskEnv customOrder;
	ASSERT_TRUE(customOrder.loadRisk(customOnlyRiskJson(1000, 1)));
	customOrder.finish();
	EXPECT_TRUE(customOrder._adapter.checkOrderLimits(kAg));
	WTSOrderInfo* order = WTSOrderInfo::create();
	ASSERT_NE(nullptr, order);
	order->setCode("ag2406");
	order->setExchange("SHFE");
	order->setDirection(WDT_LONG);
	order->setOffsetType(WOT_OPEN);
	order->setOrderFlag(WOF_NOR);
	order->setVolume(1);
	order->setVolLeft(1);
	order->setOrderState(WOS_NotTraded_Queuing);
	order->setOrderID("NO-1");
	WTSArray* orders = WTSArray::create();
	ASSERT_NE(nullptr, orders);
	orders->append(order);
	customOrder._adapter.onRspOrders(orders);
	orders->release();
	EXPECT_FALSE(customOrder._adapter.checkOrderLimits(kAg));
	order->release();
}

TEST(q04_trader_risk, order_frequency_limits_allow_the_configured_maximum_then_exclude_only_that_contract)
{
	RiskEnv env;
	ScopedRiskPluginLog log(env._tmp.file("plugin.log"));
	ASSERT_TRUE(env.loadRisk(frequencyRiskJson(true, 2, 60, 2, 60)));
	// No reports or timestamp entries yet; checks remain safe before run() initializes statistics.
	EXPECT_TRUE(env._adapter.checkOrderLimits(kAg));
	EXPECT_TRUE(env._adapter.checkCancelLimits(kAg));
	env._bd.add("SHFE", "cu", "cu2406");
	env.finish();

	EXPECT_NE(UINT_MAX, env._adapter.openLong(kAg, 5000, 1, 0));
	EXPECT_NE(UINT_MAX, env._adapter.openLong("SHFE.cu2406", 70000, 1, 0));
	EXPECT_NE(UINT_MAX, env._adapter.openLong(kAg, 5000, 1, 0));
	EXPECT_EQ(UINT_MAX, env._adapter.openLong(kAg, 5000, 5, 0));

	EXPECT_EQ(3u, countPluginEvents(log, "api.orderInsert"));
	EXPECT_DOUBLE_EQ(2.0, env._adapter.getUndoneQty(kAg)) << "a rate-limited order must release its pre-reserved quantity";
	EXPECT_FALSE(env._adapter.checkOrderLimits(kAg));
	EXPECT_TRUE(env._adapter.checkOrderLimits("SHFE.cu2406"));
}

TEST(q04_trader_risk, order_frequency_window_expires_before_the_next_eligible_order)
{
	RiskEnv env;
	ScopedRiskPluginLog log(env._tmp.file("plugin.log"));
	ASSERT_TRUE(env.loadRisk(frequencyRiskJson(true, 1, 1, 1, 1)));
	env.finish();

	EXPECT_NE(UINT_MAX, env._adapter.openLong(kAg, 5000, 1, 0));
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	EXPECT_NE(UINT_MAX, env._adapter.openLong(kAg, 5000, 1, 0));
	EXPECT_EQ(2u, countPluginEvents(log, "api.orderInsert"));
	EXPECT_FALSE(env._adapter.checkOrderLimits(kAg)) << "the second successful request fills the one-request window";
}

TEST(q04_trader_risk, failed_sdk_order_is_not_counted_in_the_frequency_window)
{
	RiskEnv env;
	ScopedRiskPluginLog log(env._tmp.file("plugin.log"));
	ASSERT_TRUE(env.loadRisk(frequencyRiskJson(true, 1, 60, 1, 60, -1, -1)));
	env.finish();

	EXPECT_EQ(UINT_MAX, env._adapter.openLong(kAg, 5000, 1, 0));
	EXPECT_EQ(UINT_MAX, env._adapter.openLong(kAg, 5000, 1, 0));
	EXPECT_EQ(2u, countPluginEvents(log, "api.orderInsert"));
	EXPECT_TRUE(env._adapter.checkOrderLimits(kAg));
}

TEST(q04_trader_risk, cancel_frequency_limits_record_only_successful_sdk_actions)
{
	RiskEnv env;
	ScopedRiskPluginLog log(env._tmp.file("plugin.log"));
	ASSERT_TRUE(env.loadRisk(frequencyRiskJson(true, 5, 60, 1, 60, 0, 0)));
	env.finish();
	env.pushAliveOrder(1001, "RF-CANCEL-1");
	env.pushAliveOrder(1002, "RF-CANCEL-2");

	EXPECT_TRUE(env._adapter.cancel(1001));
	EXPECT_FALSE(env._adapter.cancel(1002));
	EXPECT_EQ(1u, countPluginEvents(log, "api.orderAction"));
	EXPECT_FALSE(env._adapter.checkCancelLimits(kAg));
}

TEST(q04_trader_risk, failed_sdk_cancel_is_not_counted_and_cancel_all_uses_the_same_guard)
{
	RiskEnv failed;
	ScopedRiskPluginLog failedLog(failed._tmp.file("plugin.log"));
	ASSERT_TRUE(failed.loadRisk(frequencyRiskJson(true, 5, 60, 1, 60, 0, -1)));
	failed.finish();
	failed.pushAliveOrder(1101, "RF-CANCEL-FAIL-1");
	failed.pushAliveOrder(1102, "RF-CANCEL-FAIL-2");
	EXPECT_FALSE(failed._adapter.cancel(1101));
	EXPECT_FALSE(failed._adapter.cancel(1102));
	EXPECT_EQ(2u, countPluginEvents(failedLog, "api.orderAction"));
	EXPECT_TRUE(failed._adapter.checkCancelLimits(kAg));

	RiskEnv cancelAll;
	ScopedRiskPluginLog cancelAllLog(cancelAll._tmp.file("plugin.log"));
	ASSERT_TRUE(cancelAll.loadRisk(frequencyRiskJson(true, 5, 60, 1, 60, 0, 0)));
	cancelAll.finish();
	cancelAll.pushAliveOrder(1201, "RF-CANCEL-ALL-1");
	OrderIDs accepted = cancelAll._adapter.cancelAll(kAg);
	ASSERT_EQ((OrderIDs{ 1201 }), accepted);
	OrderIDs overLimit = cancelAll._adapter.cancelAll(kAg);
	EXPECT_TRUE(overLimit.empty());
	EXPECT_EQ(1u, countPluginEvents(cancelAllLog, "api.orderAction"));
	EXPECT_FALSE(cancelAll._adapter.checkCancelLimits(kAg));
}

TEST(q04_trader_risk, inactive_frequency_policy_does_not_change_order_or_cancel_paths)
{
	RiskEnv env;
	ScopedRiskPluginLog log(env._tmp.file("plugin.log"));
	ASSERT_TRUE(env.loadRisk(frequencyRiskJson(false, 1, 60, 1, 60, 0, 0)));
	env.finish();

	EXPECT_NE(UINT_MAX, env._adapter.openLong(kAg, 5000, 1, 0));
	EXPECT_NE(UINT_MAX, env._adapter.openLong(kAg, 5000, 1, 0));
	env.pushAliveOrder(1301, "RF-INACTIVE-CANCEL-1");
	env.pushAliveOrder(1302, "RF-INACTIVE-CANCEL-2");
	EXPECT_TRUE(env._adapter.cancel(1301));
	EXPECT_TRUE(env._adapter.cancel(1302));

	EXPECT_EQ(2u, countPluginEvents(log, "api.orderInsert"));
	EXPECT_EQ(2u, countPluginEvents(log, "api.orderAction"));
	EXPECT_TRUE(env._adapter.checkOrderLimits(kAg));
	EXPECT_TRUE(env._adapter.checkCancelLimits(kAg));
}
