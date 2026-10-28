/*!
 * \file q04_trader_risk_limits_test.cpp
 * \brief TraderAdapter 流量风控参数装配与限额判定(迭代01 第五波 q04)
 *
 * 覆盖: init() 的 riskmon 解析(激活/未激活/无配置/无 default 告警)、
 * getRiskParams 的品种键推导、checkCancelLimits / checkOrderLimits 的
 *
 * 说明: 两个 check* 函数的“时间窗频率”分支在当前实现里是死代码
 * (写入 _order_time_cache/_cancel_time_cache 的语句全部被注释掉了), 因此不可覆盖,
 * 见 q04_uftcore3.md BUG-Q04-004。
 */
#include "q04_trader_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"

#include <string>
#include <vector>

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

//default 组的总量限额由参数决定, ag 组刻意设得更严
std::string riskJson(uint32_t defaultCancelLimit, uint32_t defaultOrderLimit,
	uint32_t agCancelLimit = 1)
{
	return std::string("{\"riskmon\":{\"active\":true,\"policy\":{")
		+ "\"default\":{\"cancel_total_limits\":" + std::to_string(defaultCancelLimit)
		+ ",\"cancel_times_boundary\":3,\"cancel_stat_timespan\":60,"
		+ "\"order_total_limits\":" + std::to_string(defaultOrderLimit)
		+ ",\"order_times_boundary\":5,\"order_stat_timespan\":30},"
		+ "\"ag\":{\"cancel_total_limits\":" + std::to_string(agCancelLimit)
		+ ",\"cancel_times_boundary\":3,\"cancel_stat_timespan\":60,"
		+ "\"order_total_limits\":1000,\"order_times_boundary\":5,\"order_stat_timespan\":30}"
		+ "}}}";
}

struct RiskEnv
{
	q04::TempRoot			_tmp{ "risk" };
	q04::FakeTraderApi		_api;
	q04::FakeBaseDataMgr	_bd;
	ActionPolicyMgr			_policy;
	q04::FakeSink			_sink;
	WTSVariant*				_cfg = nullptr;
	TraderAdapter			_adapter;

	~RiskEnv()
	{
		if (_cfg != nullptr)
			_cfg->release();
	}

	RiskEnv() = default;

	RiskEnv(const RiskEnv&) = delete;
	RiskEnv& operator=(const RiskEnv&) = delete;

	//装配一份风控配置; 没有 module 字段所以 init 返回 false, 但风控表已经建好
	bool loadRisk(const std::string& json)
	{
		_cfg = writeRiskCfg(_tmp, "trader.json", json);
		if (_cfg == nullptr)
			return false;
		//没有 module 字段, init 解析完风控参数后返回 false, 但 _cfg/风控表已留下
		_adapter.init("uft1", _cfg, &_bd, &_policy);
		return true;
	}

	void finish()
	{
		const std::string path = _tmp.file("policy.json");
		_tmp.write(path, q04::policyJsonOpenCloseLimited());
		EXPECT_TRUE(_policy.init(path.c_str()));
		_bd.add("SHFE", "ag", "ag2406");
		EXPECT_TRUE(_adapter.initExt("uft1", &_api, &_bd, &_policy));
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
		ord->setVolume(1);
		ord->setVolLeft(1);
		ord->setOrderState(WOS_Canceled);
		ord->setOrderID(orderid);
		_adapter.onPushOrder(ord);
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
	WTSVariant* cfg = writeRiskCfg(off._tmp, "inactive.json",
		R"({"riskmon":{"active":false,"policy":{"default":{"cancel_total_limits":1}}}})");
	ASSERT_NE(nullptr, cfg);
	EXPECT_FALSE(off._adapter.init("uft1", cfg, &off._bd, &off._policy));
	cfg->release();
	off._bd.add("SHFE", "ag", "ag2406");
	ASSERT_TRUE(off._adapter.initExt("uft1", &off._api, &off._bd, &off._policy));
	off._adapter.run();
	//风控未激活时 _risk_params_map 是空的, 限额判定会走到 getRiskParams 的
	//end() 解引用支(见 DISABLED 用例与 BUG-Q04-005), 这里只验证装配本身
	EXPECT_STREQ("uft1", off._adapter.id());

	//完全没有 riskmon 配置
	RiskEnv none;
	WTSVariant* noneCfg = writeRiskCfg(none._tmp, "none.json", R"({"user":"u"})");
	ASSERT_NE(nullptr, noneCfg);
	EXPECT_FALSE(none._adapter.init("uft1", noneCfg, &none._bd, &none._policy));
	noneCfg->release();
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
	ASSERT_TRUE(env.loadRisk(riskJson(1000000, 2)));
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

TEST(q04_trader_risk, product_level_rule_is_looked_up_with_a_wrong_key)
{
	RiskEnv env;
	//ag 组限额 1, default 组放宽到 1000000
	ASSERT_TRUE(env.loadRisk(riskJson(1000000, 1000000, 1)));
	env.finish();

	env.pushCanceledBuyOrder("PK-1");

	//按约定: getRiskParams("SHFE.ag2406") 应取到 ag 组 -> 应返回 false
	//实测: 品种键被截成 "ag24"(多切了两位), 命中不到 ag 组, 回落到 default -> true
	//(实现如此, 语义待裁定, 见 q04_uftcore3.md BUG-Q04-003)
	EXPECT_TRUE(env._adapter.checkCancelLimits(kAg));
	//default 限额 1000000 没被打满, 交易仍然可用
}

TEST(q04_trader_risk, risk_params_default_group_is_used_for_unlisted_products)
{
	RiskEnv env;
	//只有 default 组, 撤单总量限额 1
	WTSVariant* cfg = writeRiskCfg(env._tmp, "onlydefault.json",
		R"({"riskmon":{"active":true,"policy":{"default":{
		"cancel_total_limits":1,"cancel_times_boundary":3,"cancel_stat_timespan":60,
		"order_total_limits":1,"order_times_boundary":5,"order_stat_timespan":30}}}})");
	ASSERT_NE(nullptr, cfg);
	EXPECT_FALSE(env._adapter.init("uft1", cfg, &env._bd, &env._policy));
	cfg->release();
	env.finish();

	//未列出的品种回落到 default: 一张撤单就命中限额
	env.pushCanceledBuyOrder("OD-1");
	EXPECT_FALSE(env._adapter.checkCancelLimits(kAg));
	//排除队列里的代码在任何限额检查前就被挡掉
	EXPECT_FALSE(env._adapter.checkOrderLimits("SHFE.cu2406"));
}

//崩溃/未定义行为类输入整类收在这里
TEST(DISABLED_q04_trader_risk, no_default_group_and_null_statmap_class)
{
	q04::TempRoot tmp("riskcls");
	q04::FakeTraderApi api;
	q04::FakeBaseDataMgr bd;
	ActionPolicyMgr policy;

	//1) 只有非 default 组: getRiskParams 回落时 `return &it->second;` 解引用 end()
	//   (与 BUG-N03-002 getActionRules 同一类)
	WTSVariant* cfg = writeRiskCfg(tmp, "nodefault.json",
		R"({"riskmon":{"active":true,"policy":{"ag":{"cancel_total_limits":1}}}})");
	TraderAdapter adapter;
	adapter.init("uft1", cfg, &bd, &policy);
	cfg->release();
	adapter.initExt("uft1", &api, &bd, &policy);
	adapter.checkCancelLimits("SHFE.ag2406");

	//2) 未 run(): _stat_map 为 NULL, 限额判定直接解引用
	TraderAdapter noStat;
	noStat.initExt("uft1", &api, &bd, &policy);
	noStat.checkOrderLimits("SHFE.ag2406");

	//3) 风控未激活(或 policy 为空): _risk_params_map 为空,
	//   getRiskParams 回落时对 end() 取地址(BUG-Q04-005)
	TraderAdapter noRiskCfg;
	noRiskCfg.initExt("uft1", &api, &bd, &policy);
	noRiskCfg.run();
	noRiskCfg.checkCancelLimits("SHFE.ag2406");
	noRiskCfg.checkOrderLimits("SHFE.ag2406");
}
