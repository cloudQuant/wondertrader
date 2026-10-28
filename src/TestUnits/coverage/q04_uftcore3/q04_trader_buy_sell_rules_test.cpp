/*!
 * \file q04_trader_buy_sell_rules_test.cpp
 * \brief TraderAdapter buy/sell 的动作规则推进(开平限额/平今平昨/单笔限量)(迭代01 第五波 q04)
 *
 * 覆盖 TraderAdapter::buy / sell 的规则循环全部分支:
 * AT_Open(_limit_l/_limit_s/_limit 限额)、AT_CloseToday(_pure + CoverMode)、
 * AT_CloseYestoday(_pure)、AT_Close(区分/不区分平昨平今)、单笔最大委托量拆分、
 * 剩余量未触发的告警支、qty==0 早退。
 * 统计限额(l_openvol/s_openvol)由 onRspTrades 推进, 用后续 buy 的可开量反证。
 */
#include "q04_trader_fixture.h"

#include <string>
#include <vector>

using namespace wtp;

namespace {

//按 pid 绑定不同规则组, 便于在同一份基础数据上走不同分支
const char* kPolicy = R"JSON({
	"default": {
		"order": [ {"action":"open", "limit":0, "limit_s":0, "limit_l":0, "pure":false} ],
		"filters": []
	},
	"closefirst": {
		"order": [
			{"action":"closeyestoday","limit":0,"limit_s":0,"limit_l":0,"pure":false},
			{"action":"closetoday",   "limit":0,"limit_s":0,"limit_l":0,"pure":false},
			{"action":"open",         "limit":0,"limit_s":0,"limit_l":0,"pure":false}
		],
		"filters": ["SHFE.cu"]
	},
	"closeonly": {
		"order": [
			{"action":"close", "limit":0, "limit_s":0, "limit_l":0, "pure":false},
			{"action":"open",  "limit":0, "limit_s":0, "limit_l":0, "pure":false}
		],
		"filters": ["SHFE.ni"]
	},
	"purecheck": {
		"order": [
			{"action":"closetoday",   "limit":0,"limit_s":0,"limit_l":0,"pure":true},
			{"action":"closeyestoday","limit":0,"limit_s":0,"limit_l":0,"pure":true},
			{"action":"open",         "limit":0,"limit_s":0,"limit_l":0,"pure":false}
		],
		"filters": ["SHFE.al"]
	},
	"limitlong": {
		"order": [ {"action":"open", "limit":0, "limit_s":0, "limit_l":10, "pure":false} ],
		"filters": ["SHFE.ag"]
	},
	"limitsshort": {
		"order": [ {"action":"open", "limit":0, "limit_s":7, "limit_l":0, "pure":false} ],
		"filters": ["SHFE.au"]
	},
	"limittotal": {
		"order": [ {"action":"open", "limit":12, "limit_s":0, "limit_l":0, "pure":false} ],
		"filters": ["SHFE.rb"]
	}
})JSON";

const char* kAg = "SHFE.ag2406";
const char* kCu = "SHFE.cu2406";
const char* kNi = "SHFE.ni2406";
const char* kAl = "SHFE.al2406";
const char* kAu = "SHFE.au2406";
const char* kRb = "SHFE.rb2410";

struct BuyEnv
{
	q04::TempRoot			_tmp{ "buysell" };
	q04::FakeTraderApi		_api;
	q04::FakeBaseDataMgr	_bd;
	ActionPolicyMgr			_policy;
	q04::FakeSink			_sink;
	TraderAdapter			_adapter;

	BuyEnv()
	{
		//cu/al 区分平昨平今, ni/rb/au/ag 不区分
		_bd.add("SHFE", "cu", "cu2406", 0, 0, CM_CoverToday);
		_bd.add("SHFE", "ni", "ni2406", 0, 0, CM_OpenCover);
		_bd.add("SHFE", "al", "al2406", 0, 0, CM_CoverToday);
		_bd.add("SHFE", "ag", "ag2406", 0, 0, CM_OpenCover);
		_bd.add("SHFE", "au", "au2406", 0, 0, CM_OpenCover);
		_bd.add("SHFE", "rb", "rb2410", 0, 0, CM_OpenCover);
		const std::string path = _tmp.file("policy.json");
		_tmp.write(path, kPolicy);
		EXPECT_TRUE(_policy.init(path.c_str()));
		EXPECT_TRUE(_adapter.initExt("uft1", &_api, &_bd, &_policy));
		_adapter.addSink(&_sink);
		EXPECT_TRUE(_adapter.run());
		_adapter.onLoginResult(true, "ok", 20240315);
	}

	//柜台持仓回报
	void loadPosition(const char* exchg, const char* code,
		double lPre, double lPreAvail, double lNew, double lNewAvail,
		double sPre, double sPreAvail, double sNew, double sNewAvail)
	{
		std::vector<WTSObject*> items{
			q04::posItem(exchg, code, WDT_LONG, lPre, lPreAvail, lNew, lNewAvail),
			q04::posItem(exchg, code, WDT_SHORT, sPre, sPreAvail, sNew, sNewAvail),
		};
		WTSArray* ay = WTSArray::create();
		for (auto one : items)
			ay->append(one);
		_adapter.onRspPosition(ay);
		ay->release();
		for (auto one : items)
			one->release();
	}

	//成交回报推进当日开仓统计
	void feedTrade(const char* exchg, const char* code, WTSDirectionType dir,
		WTSOffsetType offset, double vol)
	{
		WTSTradeInfo* t = WTSTradeInfo::create(code, exchg);
		t->setDirection(dir);
		t->setOffsetType(offset);
		t->setVolume(vol);
		WTSArray* ay = WTSArray::create();
		ay->append(t);
		_adapter.onRspTrades(ay);
		ay->release();
		t->release();
	}
};

}	// namespace

TEST(q04_trader_buysell, zero_quantity_returns_empty_without_touching_channel)
{
	BuyEnv env;

	EXPECT_TRUE(env._adapter.buy(kAg, 5000.0, 0.0, 0, false).empty());
	EXPECT_TRUE(env._adapter.sell(kAg, 5000.0, 0.0, 0, false).empty());
	EXPECT_EQ(0u, env._api._entrusts.size());
}

TEST(q04_trader_buysell, open_rule_respects_max_limit_volume_per_order)
{
	BuyEnv env;

	//单笔最大委托量 2, 限价单 -> 拆成 2+2+1
	q04::FakeBaseDataMgr& bd = env._bd;
	WTSContractInfo* ag = bd.getContract("ag2406", "SHFE");
	ag->setVolumeLimits(0, 2);
	OrderIDs ids = env._adapter.buy(kAg, 5000.0, 5.0, 0, false);
	ASSERT_EQ(3u, ids.size());
	ASSERT_EQ(3u, env._api._entrusts.size());
	EXPECT_DOUBLE_EQ(2.0, env._api._entrusts[0]._vol);
	EXPECT_DOUBLE_EQ(2.0, env._api._entrusts[1]._vol);
	EXPECT_DOUBLE_EQ(1.0, env._api._entrusts[2]._vol);
	for (auto& snap : env._api._entrusts)
	{
		EXPECT_EQ((int)WDT_LONG, snap._dir);
		EXPECT_EQ((int)WOT_OPEN, snap._offset);
	}
	//未完成量按总量累加
	EXPECT_DOUBLE_EQ(5.0, env._adapter.getUndoneQty(kAg));

	//市价单取 maxMktVol, 为 0 时按 DBL_MAX 处理 -> 一笔下完
	env._api._entrusts.clear();
	OrderIDs mkt = env._adapter.buy(kAg, 0.0, 7.0, 0, false);
	ASSERT_EQ(1u, mkt.size());
	EXPECT_DOUBLE_EQ(7.0, env._api._entrusts[0]._vol);
	EXPECT_EQ((int)WPT_ANYPRICE, env._api._entrusts[0]._ptype);
}

TEST(q04_trader_buysell, buy_closes_short_by_yestoday_then_today_then_opens)
{
	BuyEnv env;
	//cu: 区分平昨平今, 规则组 closefirst(平昨 -> 平今 -> 开)
	env.loadPosition("SHFE", "cu2406", 0, 0, 0, 0, 3, 2, 2, 2);

	OrderIDs ids = env._adapter.buy(kCu, 5100.0, 10.0, 0, false);
	ASSERT_EQ(3u, ids.size());
	ASSERT_EQ(3u, env._api._entrusts.size());
	//平昨: min(left=10, s_preavail=2) = 2
	EXPECT_EQ((int)WDT_SHORT, env._api._entrusts[0]._dir);
	EXPECT_EQ((int)WOT_CLOSE, env._api._entrusts[0]._offset);
	EXPECT_DOUBLE_EQ(2.0, env._api._entrusts[0]._vol);
	//平今: covermode==CM_CoverToday -> min(left=8, s_newavail=2) = 2
	EXPECT_EQ((int)WOT_CLOSETODAY, env._api._entrusts[1]._offset);
	EXPECT_DOUBLE_EQ(2.0, env._api._entrusts[1]._vol);
	//剩余全部开多
	EXPECT_EQ((int)WDT_LONG, env._api._entrusts[2]._dir);
	EXPECT_EQ((int)WOT_OPEN, env._api._entrusts[2]._offset);
	EXPECT_DOUBLE_EQ(6.0, env._api._entrusts[2]._vol);
}

TEST(q04_trader_buysell, sell_closes_long_with_covermode_branches)
{
	BuyEnv env;

	//ni: 不区分平昨平今, 规则组 closeonly(close -> open)
	env.loadPosition("SHFE", "ni2406", 5, 5, 3, 3, 0, 0, 0, 0);
	OrderIDs ids = env._adapter.sell(kNi, 6000.0, 10.0, 0, false);
	ASSERT_EQ(2u, ids.size());
	//AT_Close 且不区分平昨平今: 用全部可平多头 avail_pos(true)=8
	EXPECT_EQ((int)WDT_LONG, env._api._entrusts[0]._dir);
	EXPECT_EQ((int)WOT_CLOSE, env._api._entrusts[0]._offset);
	EXPECT_DOUBLE_EQ(8.0, env._api._entrusts[0]._vol);
	//剩余 2 开空
	EXPECT_EQ((int)WDT_SHORT, env._api._entrusts[1]._dir);
	EXPECT_EQ((int)WOT_OPEN, env._api._entrusts[1]._offset);
	EXPECT_DOUBLE_EQ(2.0, env._api._entrusts[1]._vol);

	//cu: 区分平昨平今的 AT_Close 要先平昨再平今
	env._api._entrusts.clear();
	env.loadPosition("SHFE", "cu2406", 6, 3, 4, 4, 0, 0, 0, 0);
	//cu 的规则组是 closefirst(closetoday 走 l_newavail, closeyestoday 走 l_preavail)
	OrderIDs ids2 = env._adapter.sell(kCu, 6000.0, 12.0, 0, false);
	//平昨 3 -> 平今 4 -> 开空 5
	ASSERT_EQ(3u, ids2.size());
	EXPECT_DOUBLE_EQ(3.0, env._api._entrusts[0]._vol);
	EXPECT_EQ((int)WOT_CLOSE, env._api._entrusts[0]._offset);
	EXPECT_DOUBLE_EQ(4.0, env._api._entrusts[1]._vol);
	EXPECT_EQ((int)WOT_CLOSETODAY, env._api._entrusts[1]._offset);
	EXPECT_EQ((int)WDT_SHORT, env._api._entrusts[2]._dir);
	EXPECT_DOUBLE_EQ(5.0, env._api._entrusts[2]._vol);
}

TEST(q04_trader_buysell, pure_rules_are_skipped_when_opposite_side_is_not_clean)
{
	BuyEnv env;
	//al: 规则组 purecheck(closetoday pure + closeyestoday pure + open), 区分平昨平今
	//买入方向看空头: s_prevol!=0 让 pure 平今跳过, s_newvol!=0 让 pure 平昨跳过
	env.loadPosition("SHFE", "al2406", 0, 0, 0, 0, 3, 3, 2, 2);

	OrderIDs ids = env._adapter.buy(kAl, 19000.0, 5.0, 0, false);
	//两条 pure 规则都被 continue, 只剩开多 5
	ASSERT_EQ(1u, ids.size());
	ASSERT_EQ(1u, env._api._entrusts.size());
	EXPECT_EQ((int)WDT_LONG, env._api._entrusts[0]._dir);
	EXPECT_EQ((int)WOT_OPEN, env._api._entrusts[0]._offset);
	EXPECT_DOUBLE_EQ(5.0, env._api._entrusts[0]._vol);

	//bForceClose=true 时 pure 检查失效, 按规则顺序先平今 2 再平昨 3
	env._api._entrusts.clear();
	OrderIDs forced = env._adapter.buy(kAl, 19000.0, 5.0, 0, true);
	ASSERT_EQ(2u, forced.size());
	EXPECT_EQ((int)WDT_SHORT, env._api._entrusts[0]._dir);
	EXPECT_EQ((int)WOT_CLOSETODAY, env._api._entrusts[0]._offset);
	EXPECT_DOUBLE_EQ(2.0, env._api._entrusts[0]._vol);
	EXPECT_EQ((int)WOT_CLOSE, env._api._entrusts[1]._offset);
	EXPECT_DOUBLE_EQ(3.0, env._api._entrusts[1]._vol);
	//开仓规则在 bForceClose 下被整体跳过, 5 手全部用于平空
	//未完成量: 第一次 buy 开多 5 + 这次平空 2+3
	EXPECT_DOUBLE_EQ(10.0, env._adapter.getUndoneQty(kAl));
}

TEST(q04_trader_buysell, long_open_limit_is_consumed_by_reported_trades)
{
	BuyEnv env;
	//ag: 规则组 limitlong(_limit_l = 10)
	env.loadPosition("SHFE", "ag2406", 0, 0, 0, 0, 0, 0, 0, 0);

	OrderIDs first = env._adapter.buy(kAg, 5000.0, 4.0, 0, false);
	ASSERT_EQ(1u, first.size());
	EXPECT_DOUBLE_EQ(4.0, env._api._entrusts.back()._vol);

	//成交回报把当日多头开仓量推到 4
	env.feedTrade("SHFE", "ag2406", WDT_LONG, WOT_OPEN, 4);
	env._api._entrusts.clear();

	//还想买 8, 只剩 10-4=6 的可开量, 余下 2 触发不了(告警支)
	OrderIDs second = env._adapter.buy(kAg, 5000.0, 8.0, 0, false);
	ASSERT_EQ(1u, second.size());
	EXPECT_DOUBLE_EQ(6.0, env._api._entrusts[0]._vol);

	env.feedTrade("SHFE", "ag2406", WDT_LONG, WOT_OPEN, 6);
	env._api._entrusts.clear();

	//额度已用完 -> 规则被 continue, 一张单都不发
	OrderIDs third = env._adapter.buy(kAg, 5000.0, 3.0, 0, false);
	EXPECT_EQ(0u, third.size());
	EXPECT_EQ(0u, env._api._entrusts.size());
}

TEST(q04_trader_buysell, short_open_limit_only_constrains_the_sell_side)
{
	BuyEnv env;
	//au: 规则组 limitsshort(_limit_s = 7)
	env.loadPosition("SHFE", "au2406", 0, 0, 0, 0, 0, 0, 0, 0);

	//卖出开空 10 -> 只允许 7
	OrderIDs ids = env._adapter.sell(kAu, 550000.0, 10.0, 0, false);
	ASSERT_EQ(1u, ids.size());
	EXPECT_DOUBLE_EQ(7.0, env._api._entrusts.back()._vol);

	env.feedTrade("SHFE", "au2406", WDT_SHORT, WOT_OPEN, 7);
	env._api._entrusts.clear();

	//额度用完: sell 不再发单
	OrderIDs again = env._adapter.sell(kAu, 550000.0, 2.0, 0, false);
	EXPECT_EQ(0u, again.size());

	//买入不受 _limit_s 影响, 仍可开多
	env._api._entrusts.clear();
	OrderIDs longs = env._adapter.buy(kAu, 550000.0, 3.0, 0, false);
	ASSERT_EQ(1u, longs.size());
	EXPECT_EQ((int)WDT_LONG, env._api._entrusts[0]._dir);
}

TEST(q04_trader_buysell, total_limit_shares_the_quota_between_both_directions)
{
	BuyEnv env;
	//rb: 规则组 limittotal(_limit = 12, 不区分方向)
	env.loadPosition("SHFE", "rb2410", 0, 0, 0, 0, 0, 0, 0, 0);

	OrderIDs first = env._adapter.buy(kRb, 3500.0, 8.0, 0, false);
	ASSERT_EQ(1u, first.size());
	env.feedTrade("SHFE", "rb2410", WDT_LONG, WOT_OPEN, 8);

	//剩余额度 4
	env._api._entrusts.clear();
	OrderIDs second = env._adapter.sell(kRb, 3500.0, 6.0, 0, false);
	ASSERT_EQ(1u, second.size());
	EXPECT_DOUBLE_EQ(4.0, env._api._entrusts[0]._vol);
	env.feedTrade("SHFE", "rb2410", WDT_SHORT, WOT_OPEN, 4);

	//额度耗尽, 两边都下不去
	env._api._entrusts.clear();
	EXPECT_EQ(0u, env._adapter.buy(kRb, 3500.0, 1.0, 0, false).size());
	EXPECT_EQ(0u, env._adapter.sell(kRb, 3500.0, 1.0, 0, false).size());
}

TEST(q04_trader_buysell, unknown_stdcode_without_contract_info_is_rejected_before_rule_loop)
{
	BuyEnv env;

	//基础数据里没有的合约: getContract 返回 NULL, buy/sell 直接解引用
	//属于已判定崩溃支, 这里只验证 cInfo 显式传入时不查基础数据
	WTSContractInfo* cu = env._bd.getContract("cu2406", "SHFE");
	env.loadPosition("SHFE", "cu2406", 0, 0, 0, 0, 2, 2, 0, 0);
	OrderIDs ids = env._adapter.buy(kCu, 5100.0, 5.0, 0, false, cu);
	//规则组 closefirst: 平昨 min(5, s_preavail=2)=2, 平今 0(今仓不可平), 开 3
	ASSERT_EQ(2u, ids.size());
	EXPECT_DOUBLE_EQ(2.0, env._api._entrusts[0]._vol);
	EXPECT_DOUBLE_EQ(3.0, env._api._entrusts[1]._vol);
	EXPECT_EQ((int)WOT_OPEN, env._api._entrusts[1]._offset);
}

//已判定崩溃/越界的输入整类收在这里
TEST(DISABLED_q04_trader_buysell, null_contract_and_missing_default_class)
{
	BuyEnv env;

	//1) 合约不在基础数据里: buy 里 cInfo==NULL 后直接 cInfo->getCommInfo()
	env._adapter.buy("SHFE.unknown", 5000.0, 1.0, 0, false);

	//2) 品种没有任何动作规则且没有 default 组:
	//   ActionPolicyMgr::getActionRules 返回 _rules.end() 解引用(BUG-N03-002 同源)
	q04::TempRoot tmp2("cls3");
	q04::FakeTraderApi api2;
	q04::FakeBaseDataMgr bd2;
	bd2.add("CZCE", "TA", "ta501", 0, 0, CM_OpenCover);
	ActionPolicyMgr noDefault;
	const std::string path = tmp2.file("onlygroup.json");
	tmp2.write(path, R"JSON({ "other": { "order":[{"action":"open","limit":1}], "filters":["CZCE.c"] } })JSON");
	noDefault.init(path.c_str());
	TraderAdapter adapter2;
	adapter2.initExt("uft2", &api2, &bd2, &noDefault);
	adapter2.run();
	adapter2.buy("CZCE.ta501", 5000.0, 1.0, 0, false);
}
