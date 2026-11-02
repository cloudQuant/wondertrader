// R115 lane：WtExeFact 股票执行单元（WtStockVWap / WtStockMinImpact）的残余行
// 目标文件与行段（llvm-cov show 对本目标实读的 0 计数行）：
//   src/WtExeFact/WtStockVWapExeUnit.cpp: L115-129（calTmStamp 的午休区间/午休钳位/下午区间；
//       L107-108 的 hour<9:30 分支因 to_string 丢前导 0 实际不可达，同 BUG-R115-001）、L175（session retain）、
//       L271-274（撤单完成回减 cancelcnt）、L300-302（未撤单但 cancelcnt 非 0）、L382/L396（异码、
//       首笔非交易时段）、L426-436（超时 fail-closed 回调体）、L487（CalcFlag 重入保护）、
//       L505-508/L516（可卖仓位修正、diff==0）、L531-546（反向未完成单先撤/同向等待）、
//       L586-589/L597-598/L633-634（末次打板与整手取整、打板 5 跳）、L624-631（mode1/mode2 价格）、
//       L703-711/L721-726/L744（fire_at_once 的价格模式与涨停钳位）、L795-797（set_position 抓最新 tick）
//   src/WtExeFact/WtStockVWapExeUnit.h: L376（可转债最小 10 股）
//   src/WtExeFact/WtStockMinImpactExeUnit.cpp: L329-347（enumOrder + 超时 fail-closed 回调）、
//       L351-366（撤单次数超限强制删除）、L402-404（委托失败删除）、L459-462（抓最新 tick）、
//       L638（取整后 0 股不发单）；.h L337/L341（可转债 10 股、科创板 200 股）
//
// 与 R7/R17/R24 的分工：R7 已覆盖两单元的 init 钳位/通道就绪/主发单链；R24 已覆盖分桶、超时扫描主链、
// fire_at_once 卖侧与涨跌停组合。本文件只补它们留下的 0 行，并纠正两处 fake 语义：
//   1) fake getPosition 遵循 ExecuteDefs.h 的语义：true=可用仓、false=总仓；StockVWap 的目标调整
//      用例分别设置总仓与可卖多仓，检查减仓目标上调后卖量不超过可卖仓。
//   2) 超时回归通过可控 context clock 推进正常 1 秒阈值；只验证 fail-closed 和监控记录保留。
//
// 纪律：不启线程、无真实插件；Vwap_*.txt 写在系统临时目录并把 CWD 临时切过去，析构恢复并删除。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "Includes/ExecuteDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "WtExeFact/WtStockVWapExeUnit.h"
#include "WtExeFact/WtStockMinImpactExeUnit.h"

using namespace wtp;

namespace
{
const char* const kStockCode = "SHSE.STK.600000";

class StockCtx : public ExecuteContext
{
public:
	struct Rec
	{
		std::string code;
		double price;
		double qty;
		bool force;
	};

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return NULL; }
	WTSTickData* grabLastTick(const char*) override
	{
		if (_grabTick != NULL)
			_grabTick->retain();
		return _grabTick;
	}

	double getPosition(const char*, bool validOnly = true, int32_t flag = 3) override
	{
		if (flag == 1)
			return long_pos;
		return validOnly ? avail_pos : total_pos;
	}

	OrderMap* getOrders(const char*) override { return OrderMap::create(); }
	double getUndoneQty(const char*) override { return undone; }

	OrderIDs buy(const char* stdCode, double price, double qty, bool bForceClose) override
	{
		buys.push_back({ stdCode, price, qty, bForceClose });
		OrderIDs ids{ nextId++ };
		lastId = ids[0];
		if (onAfterBuy)
			onAfterBuy();
		return ids;
	}

	OrderIDs sell(const char* stdCode, double price, double qty, bool bForceClose) override
	{
		sells.push_back({ stdCode, price, qty, bForceClose });
		OrderIDs ids{ nextId++ };
		lastId = ids[0];
		return ids;
	}

	bool cancel(uint32_t localid) override
	{
		byidCancels.push_back(localid);
		return cancelByIdRet;
	}

	OrderIDs cancel(const char* stdCode, bool isBuy, double /*qty*/ = 0) override
	{
		bydirCancels.push_back(std::make_pair(std::string(stdCode), isBuy));
		OrderIDs ids;
		for (uint32_t i = 0; i < cancelBydirRetCnt; i++)
			ids.push_back(nextId++);
		return ids;
	}

	void writeLog(const char* message) override { logs.push_back(message ? message : ""); }
	WTSCommodityInfo* getCommodityInfo(const char*) override { return comm; }
	WTSSessionInfo* getSessionInfo(const char*) override { return sess; }
	uint64_t getCurTime() override { return curTime; }

	bool hasLog(const std::string& sub) const
	{
		for (const std::string& l : logs)
		{
			if (l.find(sub) != std::string::npos)
				return true;
		}
		return false;
	}

public:
	double avail_pos = 0;	// true = 可用（昨仓）
	double total_pos = 0;	// false = 总仓位
	double long_pos = 0;
	double undone = 0;
	uint64_t curTime = 20240103093000000ull;
	uint32_t nextId = 6000;
	uint32_t lastId = 0;
	bool cancelByIdRet = true;
	uint32_t cancelBydirRetCnt = 1;
	WTSCommodityInfo* comm = NULL;
	WTSSessionInfo* sess = NULL;
	WTSTickData* _grabTick = NULL;
	std::vector<Rec> buys;
	std::vector<Rec> sells;
	std::vector<uint32_t> byidCancels;
	std::vector<std::pair<std::string, bool> > bydirCancels;
	std::vector<std::string> logs;
	std::function<void()> onAfterBuy;
};

// 股票/可转债品种：pid 必须放第 1 个参数，get_minOrderQty 用 getProduct() 比较 "STK"/"CBOND"
WTSCommodityInfo* makeStockComm(const char* pid, const char* name, const char* exchg)
{
	WTSCommodityInfo* c = WTSCommodityInfo::create(pid, name, exchg, "R115S", "CHN");
	c->retain();	// 抵消 get_minOrderQty 内无条件的 release（见 BUG-R115-003）
	c->setVolScale(1);
	c->setPriceTick(0.01);
	c->setLotsTick(1.0);
	c->setMinLots(1.0);
	c->setCategory(CC_Stock);
	c->setCoverMode(CM_OpenCover);
	c->setPriceMode(PM_Both);
	c->setTradingMode(TM_LongT1);
	return c;
}

WTSSessionInfo* makeSession()
{
	WTSSessionInfo* s = WTSSessionInfo::create("R115S", "r115 连续竞价", 0);
	s->addTradingSection(900, 1500);
	return s;
}

WTSTickData* makeStockTick(const char* code, uint32_t actTime, double lastPx,
	double bidPx, double askPx, double upper = 0, double lower = 0,
	double bidQty = 100, double askQty = 100)
{
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	snprintf(ts.code, sizeof(ts.code), "%s", code);
	ts.action_date = 20240103;
	ts.trading_date = 20240103;
	ts.action_time = actTime;
	ts.price = lastPx;
	ts.pre_close = lastPx - 0.1;
	ts.bid_prices[0] = bidPx;
	ts.bid_qty[0] = bidQty;
	ts.ask_prices[0] = askPx;
	ts.ask_qty[0] = askQty;
	ts.upper_limit = upper;
	ts.lower_limit = lower;
	return WTSTickData::create(ts);
}

class TempCwd
{
public:
	explicit TempCwd(const char* tag)
	{
		static uint32_t seq = 0;
		_dir = std::filesystem::temp_directory_path() /
			("r115_" + std::string(tag) + "_" + std::to_string((long long)::getpid()) + "_" + std::to_string(++seq));
		std::error_code ec;
		std::filesystem::create_directories(_dir, ec);
		_old = std::filesystem::current_path();
		std::filesystem::current_path(_dir, ec);
	}

	void write(const std::string& name, const std::string& body) const
	{
		std::ofstream ofs((_dir / name).string().c_str());
		ofs << body;
		ofs.close();
	}

	~TempCwd()
	{
		std::error_code ec;
		std::filesystem::current_path(_old, ec);
		std::filesystem::remove_all(_dir, ec);
	}

private:
	std::filesystem::path _dir;
	std::filesystem::path _old;
};

struct Env
{
	StockCtx ctx;
	WTSCommodityInfo* comm = NULL;
	WTSSessionInfo* sess = NULL;

	~Env()
	{
		if (sess != NULL)
			sess->release();
		if (comm != NULL)
			comm->release();
		if (ctx._grabTick != NULL)
			ctx._grabTick->release();
	}
};

std::string r115V2VwapBody(double value)
{
	std::string body = "{\"version\":2,\"profiles\":[{\"session_id\":\"R115S\",\"offset_minutes\":0,"
		"\"sections\":[{\"from\":900,\"to\":1500}],\"bucket_unit\":\"trading_minute\","
		"\"value_mode\":\"cumulative\",\"close_inclusive\":true,\"values\":[";
	for (uint32_t i = 0; i <= 360; ++i)
	{
		if (i != 0)
			body += ",";
		body += std::to_string((long long)value);
	}
	body += "]}]}";
	return body;
}

std::string r115BucketV2VwapBody()
{
	std::string body = "{\"version\":2,\"profiles\":[{\"session_id\":\"R115S\",\"offset_minutes\":0,"
		"\"sections\":[{\"from\":900,\"to\":1500}],\"bucket_unit\":\"trading_minute\","
		"\"value_mode\":\"cumulative\",\"close_inclusive\":true,\"values\":[";
	for (uint32_t i = 0; i <= 360; ++i)
	{
		if (i != 0)
			body += ",";
		const uint32_t value = i < 60 ? 0 : (i < 180 ? 1000 : (i < 300 ? 2000 : 3000));
		body += std::to_string(value);
	}
	body += "]}]}";
	return body;
}

WTSVariant* makeVwapCfg(uint32_t totalTimes, uint32_t priceMode, uint32_t ordSticky,
	double lots, double minOpenLots, uint32_t offset = 0,
	uint32_t beginTime = 900, uint32_t endTime = 1500)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("begin_time", beginTime);
	cfg->append("end_time", endTime);
	cfg->append("ord_sticky", ordSticky);
	cfg->append("tail_secs", (uint32_t)0);
	cfg->append("total_times", totalTimes);
	cfg->append("price_mode", priceMode);
	cfg->append("offset", offset);
	cfg->append("lots", lots);
	cfg->append("minopenlots", minOpenLots);
	return cfg;
}

WTSVariant* makeStockMinCfg(uint32_t expire, int32_t priceMode, uint32_t span,
	double lots, double minOrder, int32_t maxCancel, double rate = 0.0, bool byRate = false)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("offset", (int32_t)0);
	cfg->append("expire", expire);
	cfg->append("pricemode", priceMode);
	cfg->append("span", span);
	cfg->append("byrate", byRate);
	cfg->append("lots", lots);
	cfg->append("rate", rate);
	cfg->append("min_order", minOrder);
	cfg->append("max_cancel_time", maxCancel);
	return cfg;
}
}	// namespace

//////////////////////////////////////////////////////////////////////////
// StockVWap：时段分桶四分支 + 末次打板 + 打板 5 跳 + 首笔过滤/异码
// 生产文件: src/WtExeFact/WtStockVWapExeUnit.cpp:107-108,115-116,119-120,128-129,
//           175,382,396,586-589,597-598,633-634
//////////////////////////////////////////////////////////////////////////
TEST(r115_exefact, stock_vwap_time_buckets_showhand_and_guards)
{
	TempCwd cwd("svwap");
	cwd.write("Vwap_R115SV.txt", r115BucketV2VwapBody());

	Env env;
	env.comm = makeStockComm("STK", "R115SV", "SHSE");
	env.sess = makeSession();
	env.ctx.comm = env.comm;
	env.ctx.sess = env.sess;
	{
		WtStockVWapExeUnit unit;
		// 10:00-14:00 window with a two-hour fire span keeps all three clock points eligible.
		WTSVariant* cfg = makeVwapCfg(2, 0, 0, 100.0, 100.0, 0, 1000, 1400);
		unit.init(&env.ctx, kStockCode, cfg);
		cfg->release();
		ASSERT_TRUE(env.ctx.hasLog("初始化完成"));

		// L381-382：空/异码；L395-396：首笔 08:00 盘前行情被过滤，后续 10:00 行情时间递增
		unit.on_tick(NULL);
		WTSTickData* foreign = makeStockTick("SHSE.STK.600001", 200000000, 10.0, 9.99, 10.01);
		unit.on_tick(foreign);
		foreign->release();
		WTSTickData* outside = makeStockTick(kStockCode, 80000000, 10.0, 9.99, 10.01);
		unit.on_tick(outside);
		EXPECT_TRUE(env.ctx.buys.empty());
		outside->release();

		// 先给第一笔有效行情（目标为 0，不触发计算），再置通道就绪
		WTSTickData* t1 = makeStockTick(kStockCode, 100000000, 10.0, 9.99, 10.01);
		unit.on_tick(t1);
		unit.on_channel_ready();
		EXPECT_TRUE(env.ctx.buys.empty());

		// L795-797 在同文件另一用例；这里 grabLastTick 返回空，_start_price 仍为 0
		// 10:00 首次执行：1000 股按最小 100 股整手
		unit.set_position(kStockCode, 3000.0);
		ASSERT_EQ(env.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(env.ctx.buys[0].price, 10.0);
		EXPECT_DOUBLE_EQ(env.ctx.buys[0].qty, 1000.0);

		// 12:00 是下一笔符合两小时间隔的行情；仍按分布切片价执行。
		env.ctx.curTime += 7200000;
		env.ctx.avail_pos = 1000;
		env.ctx.total_pos = 1000;
		env.ctx.long_pos = 1000;
		WTSTickData* t2 = makeStockTick(kStockCode, 120000000, 10.5, 10.49, 10.51);
		unit.on_tick(t2);
		ASSERT_EQ(env.ctx.buys.size(), 2u);
		EXPECT_DOUBLE_EQ(env.ctx.buys[1].price, 10.5);
		EXPECT_DOUBLE_EQ(env.ctx.buys[1].qty, 1000.0);

		// 14:00 再过两小时后进入最后一轮：剩余目标整手加价 5 跳。
		// 顺带在买单回调里重入 clear_all_position：CalcFlag 占用 -> do_calc 在 L485-487 直接返回，
		// 不影响外层这笔已算好的单（覆盖重入保护）。
		env.ctx.onAfterBuy = [&unit]() { unit.clear_all_position(kStockCode); };
		env.ctx.curTime += 7200000;
		env.ctx.avail_pos = 2000;
		env.ctx.total_pos = 2000;
		env.ctx.long_pos = 2000;
		WTSTickData* t3 = makeStockTick(kStockCode, 140000000, 10.8, 10.79, 10.81);
		unit.on_tick(t3);
		ASSERT_EQ(env.ctx.buys.size(), 3u);
		EXPECT_NEAR(env.ctx.buys[2].price, 10.85, 1e-9);

		t1->release();
		t2->release();
		t3->release();
	}
}

//////////////////////////////////////////////////////////////////////////
// StockVWap：可卖仓位修正 L505-508、diff==0 早退 L516、mode2 发单价 L628-631
// 生产文件: src/WtExeFact/WtStockVWapExeUnit.cpp:505-508,516,601-609,628-631,795-797
//////////////////////////////////////////////////////////////////////////
TEST(r115_exefact, stock_vwap_target_adjust_diff_zero_and_mode2_price)
{
	TempCwd cwd("svwap2");
	cwd.write("Vwap_R115SV2.txt", r115V2VwapBody(1000));

	Env env;
	env.comm = makeStockComm("STK", "R115SV2", "SHSE");
	env.sess = makeSession();
	env.ctx.comm = env.comm;
	env.ctx.sess = env.sess;
	{
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = makeVwapCfg(10, 2, 0, 100.0, 100.0);
		unit.init(&env.ctx, kStockCode, cfg);
		cfg->release();
		ASSERT_TRUE(env.ctx.hasLog("初始化完成"));

		WTSTickData* t1 = makeStockTick(kStockCode, 100000000, 10.0, 9.99, 10.01);
		unit.on_tick(t1);
		unit.on_channel_ready();

		// 先建立基准买单；随后更新目标并送入新行情，验证可卖仓保护和 mode2 卖价路径。
		unit.set_position(kStockCode, 1000.0);
		ASSERT_EQ(env.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(env.ctx.buys[0].price, 10.01); // mode2 买入取卖一价（ask）

		// 总仓 1000、可卖多仓 400；目标清到 0 时保护性上调目标到 600，差额 -400。
		// 同一笔行情时间已被上一轮消费，目标更新不应复用它下单。
		env.ctx.avail_pos = 400;	// 单边多仓时 flag=3 净可用仓与 flag=1 可卖多仓一致
		env.ctx.total_pos = 1000;
		env.ctx.long_pos = 400;
		unit.set_position(kStockCode, 0.0);
		EXPECT_EQ(env.ctx.buys.size(), 1u);

		// 36 分钟 fire_span 到期后，减仓必须卖出且不得超过可卖仓；mode2 卖出取买一价。
		env.ctx.curTime += 2160000;
		WTSTickData* t2 = makeStockTick(kStockCode, 103600000, 10.2, 10.19, 10.21);
		unit.on_tick(t2);
		ASSERT_EQ(env.ctx.sells.size(), 1u);
		EXPECT_EQ(env.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(env.ctx.sells[0].price, 10.19);
		EXPECT_DOUBLE_EQ(env.ctx.sells[0].qty, 100.0);
		EXPECT_LE(env.ctx.sells[0].qty, env.ctx.long_pos);
		EXPECT_TRUE(env.ctx.hasLog("target adjust"));

		// 目标与总仓均为 1000；新鲜行情确认 diff==0 早退，不再发单。
		env.ctx.avail_pos = 400;
		env.ctx.total_pos = 1000;
		unit.set_position(kStockCode, 1000.0);
		EXPECT_EQ(env.ctx.buys.size(), 1u);
		EXPECT_EQ(env.ctx.sells.size(), 1u);
		env.ctx.curTime += 2160000;
		WTSTickData* t3 = makeStockTick(kStockCode, 111200000, 10.22, 10.21, 10.23);
		unit.on_tick(t3);
		EXPECT_EQ(env.ctx.buys.size(), 1u);
		EXPECT_EQ(env.ctx.sells.size(), 1u);
		EXPECT_TRUE(env.ctx.hasLog("reached exactly"));

		// L793-797：grabLastTick 返回最新 tick -> _start_price 被更新；新目标不能复用已消费的行情。
		env.ctx._grabTick = makeStockTick(kStockCode, 111200000, 10.25, 10.24, 10.26);
		env.ctx.avail_pos = 0;
		env.ctx.total_pos = 0;
		env.ctx.long_pos = 0;
		unit.set_position(kStockCode, 2000.0);
		// 同一笔行情时间已被 do_calc 消费拦下，但 set_position 仍抓 tick 并写 _start_price。
		EXPECT_EQ(env.ctx.buys.size(), 1u);
		EXPECT_EQ(env.ctx.sells.size(), 1u);

		t1->release();
		t2->release();
		t3->release();
	}
}

//////////////////////////////////////////////////////////////////////////
// StockVWap：身份未验证的超时 fail-closed；普通订单回报和 fire_at_once 由其他用例覆盖。
// 生产文件: src/WtExeFact/WtStockVWapExeUnit.cpp:271-274,300-302,426-436,
//           703-708,721-726,744
//////////////////////////////////////////////////////////////////////////
TEST(r115_exefact, stock_vwap_unverified_timeout_fails_closed)
{
	TempCwd cwd("svwap3");
	cwd.write("Vwap_R115SV3.txt", r115V2VwapBody(1000));

	Env env;
	env.comm = makeStockComm("STK", "R115SV3", "SHSE");
	env.sess = makeSession();
	env.ctx.comm = env.comm;
	env.ctx.sess = env.sess;
	{
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = makeVwapCfg(10, 1, 1u, 100.0, 100.0);
		unit.init(&env.ctx, kStockCode, cfg);
		cfg->release();
		ASSERT_TRUE(env.ctx.hasLog("初始化完成"));

		unit.set_position(kStockCode, 1000.0);
		unit.on_channel_ready();
		WTSTickData* t1 = makeStockTick(kStockCode, 100000000, 10.0, 9.99, 10.01);
		unit.on_tick(t1);
		ASSERT_EQ(env.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(env.ctx.buys[0].price, 9.99);	// mode1：买用买一价
		const uint32_t id1 = env.ctx.lastId;
		env.ctx.undone = env.ctx.buys[0].qty;	// fake 回填活单未完成量

		// 第二笔行情触发超时扫描；没有可验证身份时不调用 provider cancel。
		env.ctx.curTime += 1000;
		WTSTickData* t2 = makeStockTick(kStockCode, 100100000, 10.4, 10.39, 10.41, 9.5, 0.5);
		unit.on_tick(t2);
		EXPECT_TRUE(env.ctx.byidCancels.empty());
		EXPECT_TRUE(env.ctx.hasLog(std::string("Expired order ") + std::to_string(id1)));
		EXPECT_EQ(env.ctx.buys.size(), 1u);

		env.ctx.curTime += 1000;
		WTSTickData* t3 = makeStockTick(kStockCode, 100200000, 10.5, 10.49, 10.51);
		unit.on_tick(t3);
		EXPECT_TRUE(env.ctx.byidCancels.empty());
		EXPECT_EQ(env.ctx.buys.size(), 1u);
		unit.on_order(id1, kStockCode, true, 0.0, env.ctx.buys[0].price, true);
		EXPECT_TRUE(env.ctx.hasLog(std::string("Order ") + std::to_string(id1) + " of " + kStockCode + " canceled"));

		t1->release();
		t2->release();
	}
}

//////////////////////////////////////////////////////////////////////////
// StockMinImpact：超时 fail-closed + enumOrder；超时不增加撤单次数，也不强制删除监控项。
//                 + 委托失败删除（L402-404）+ set_position 抓 tick（L459-462）
//                 + 取整为 0 股不发单（L637-638）
//////////////////////////////////////////////////////////////////////////
TEST(r115_exefact, stock_minimpact_expire_enum_error_order_entrust_and_zero_qty)
{
	Env env;
	env.comm = makeStockComm("STK", "R115SMI", "SHSE");
	env.sess = makeSession();
	env.ctx.comm = env.comm;
	env.ctx.sess = env.sess;
	{
		WtStockMinImpactExeUnit unit;
		WTSVariant* cfg = makeStockMinCfg(1u, 0, 0, 100.0, 100.0, 1);
		unit.init(&env.ctx, kStockCode, cfg);
		cfg->release();

		unit.on_channel_ready();
		WTSTickData* t1 = makeStockTick(kStockCode, 100000000, 10.0, 9.99, 10.01);
		unit.on_tick(t1);
		unit.set_position(kStockCode, 200.0);
		ASSERT_EQ(env.ctx.buys.size(), 1u);
		env.ctx.undone = env.ctx.buys[0].qty;	// fake 回填活单未完成量
		const std::string monitoredId = std::to_string(env.ctx.lastId);

		// 第二笔 tick：枚举后报告身份不可验证的超时；不调用 provider cancel。
		env.ctx.curTime += 1000;
		WTSTickData* t2 = makeStockTick(kStockCode, 100100000, 10.05, 10.04, 10.06);
		unit.on_tick(t2);
		EXPECT_TRUE(env.ctx.byidCancels.empty());
		EXPECT_TRUE(env.ctx.hasLog("Expired order"));
		EXPECT_TRUE(env.ctx.hasLog(monitoredId + " entertime:"));
		EXPECT_EQ(env.ctx.buys.size(), 1u);

		// 第三笔 tick 仍能枚举监控项，不会重复自动撤单或强制删除。
		env.ctx.curTime += 1000;
		WTSTickData* t3 = makeStockTick(kStockCode, 100200000, 10.08, 10.07, 10.09);
		unit.on_tick(t3);
		EXPECT_TRUE(env.ctx.byidCancels.empty());
		EXPECT_FALSE(env.ctx.hasLog("erase forcely"));
		EXPECT_TRUE(env.ctx.hasLog(monitoredId + " entertime:"));
		EXPECT_EQ(env.ctx.buys.size(), 1u);

		t1->release();
		t2->release();
		t3->release();
	}

	Env eenv;
	eenv.comm = makeStockComm("STK", "R115SMI2", "SHSE");
	eenv.sess = makeSession();
	eenv.ctx.comm = eenv.comm;
	eenv.ctx.sess = eenv.sess;
	{
		WtStockMinImpactExeUnit unit;
		WTSVariant* cfg = makeStockMinCfg(0, 0, 0, 100.0, 100.0, 1);
		unit.init(&eenv.ctx, kStockCode, cfg);
		cfg->release();

		eenv.ctx._grabTick = makeStockTick(kStockCode, 100000000, 10.0, 9.99, 10.01);
		unit.on_channel_ready();
		WTSTickData* tick = makeStockTick(kStockCode, 100000000, 10.0, 9.99, 10.01);
		unit.on_tick(tick);
		// L457-462：grabLastTick 非空 -> 更新 _start_price
		unit.set_position(kStockCode, 200.0);
		ASSERT_EQ(eenv.ctx.buys.size(), 1u);
		eenv.ctx._grabTick->release();
		eenv.ctx._grabTick = NULL;

		// L395-404：委托失败 -> 从监控器删除并写日志，随后 do_calc 重新发单
		unit.on_entrust(eenv.ctx.lastId, kStockCode, false, "r115 reject");
		EXPECT_TRUE(eenv.ctx.hasLog("entrust failed"));
		tick->release();
	}

	Env zenv;
	zenv.comm = makeStockComm("STK", "R115SMI3", "SHSE");
	zenv.sess = makeSession();
	zenv.ctx.comm = zenv.comm;
	zenv.ctx.sess = zenv.sess;
	{
		WtStockMinImpactExeUnit unit;
		// lots=10, 目标 150, min_order=100：round_hands(10,100)=0 -> L637-638 直接返回
		WTSVariant* cfg = makeStockMinCfg(0, 0, 0, 10.0, 100.0, 1);
		unit.init(&zenv.ctx, kStockCode, cfg);
		cfg->release();
		unit.on_channel_ready();
		WTSTickData* tick = makeStockTick(kStockCode, 100000000, 10.0, 9.99, 10.01);
		unit.on_tick(tick);
		unit.set_position(kStockCode, 150.0);
		EXPECT_TRUE(zenv.ctx.buys.empty());
		tick->release();
	}
}

//////////////////////////////////////////////////////////////////////////
// 品种最小单位：可转债 10 股（StockVWap.h L376 / StockMinImpact.h L337）
//              + 科创板 200 股（StockMinImpact.h L341）
//////////////////////////////////////////////////////////////////////////
TEST(r115_exefact, stock_units_min_order_by_product)
{
	// StockMinImpact 可转债：min_order=min(20,10)=10；目标 12 -> 发 10 股
	{
		Env env;
		env.comm = makeStockComm("CBOND", "R115CB", "SHSE");
		env.sess = makeSession();
		env.ctx.comm = env.comm;
		env.ctx.sess = env.sess;
		WtStockMinImpactExeUnit unit;
		WTSVariant* cfg = makeStockMinCfg(0, 0, 0, 12.0, 20.0, 1);
		unit.init(&env.ctx, "SHSE.CBOND.113001", cfg);
		cfg->release();
		unit.on_channel_ready();
		WTSTickData* tick = makeStockTick("SHSE.CBOND.113001", 100000000, 100.0, 99.99, 100.01);
		unit.on_tick(tick);
		unit.set_position("SHSE.CBOND.113001", 12.0);
		ASSERT_EQ(env.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(env.ctx.buys[0].qty, 10.0);
		tick->release();
	}

	// StockMinImpact 科创板：min_order=max(100,200)=200；目标 250 -> 发 200 股
	{
		Env env;
		env.comm = makeStockComm("STK", "R115KC", "SHSE");
		env.sess = makeSession();
		env.ctx.comm = env.comm;
		env.ctx.sess = env.sess;
		WtStockMinImpactExeUnit unit;
		WTSVariant* cfg = makeStockMinCfg(0, 0, 0, 250.0, 100.0, 1);
		unit.init(&env.ctx, "SHSE.STK.688001", cfg);
		cfg->release();
		unit.on_channel_ready();
		WTSTickData* tick = makeStockTick("SHSE.STK.688001", 100000000, 10.0, 9.99, 10.01);
		unit.on_tick(tick);
		unit.set_position("SHSE.STK.688001", 250.0);
		ASSERT_EQ(env.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(env.ctx.buys[0].qty, 200.0);
		tick->release();
	}

	// StockVWap 可转债：min_open_lots=min(20,10)=10；分桶值 12 -> 买 10 股
	{
		TempCwd cwd("svwapcb");
		cwd.write("Vwap_R115SVB.txt", r115V2VwapBody(12));
		Env env;
		env.comm = makeStockComm("CBOND", "R115SVB", "SHSE");
		env.sess = makeSession();
		env.ctx.comm = env.comm;
		env.ctx.sess = env.sess;
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = makeVwapCfg(10, 0, 0, 12.0, 20.0);
		unit.init(&env.ctx, "SHSE.CBOND.113002", cfg);
		cfg->release();
		ASSERT_TRUE(env.ctx.hasLog("初始化完成"));
		WTSTickData* tick = makeStockTick("SHSE.CBOND.113002", 100000000, 100.0, 99.99, 100.01);
		unit.on_tick(tick);
		unit.on_channel_ready();
		unit.set_position("SHSE.CBOND.113002", 12.0);
		ASSERT_EQ(env.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(env.ctx.buys[0].qty, 10.0);
		tick->release();
	}
}

//////////////////////////////////////////////////////////////////////////
// StockVWap：未完成单与目标方向相反 -> 先撤（L531-541）；同向 -> 本轮不发（L542-546）
// 生产文件: src/WtExeFact/WtStockVWapExeUnit.cpp:530-546
//////////////////////////////////////////////////////////////////////////
TEST(r115_exefact, stock_vwap_live_order_opposite_and_same_direction_guards)
{
	TempCwd cwd("svwap4");
	cwd.write("Vwap_R115SV4.txt", r115V2VwapBody(1000));
	cwd.write("Vwap_R115SV5.txt", r115V2VwapBody(1000));

	// 目标买 1000，未完成 -100（卖单）-> diff*undone<0 -> 按 undone 方向撤卖单（isBuy=false）
	{
		Env env;
		env.comm = makeStockComm("STK", "R115SV4", "SHSE");
		env.sess = makeSession();
		env.ctx.comm = env.comm;
		env.ctx.sess = env.sess;
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = makeVwapCfg(10, 0, 0, 100.0, 100.0);
		unit.init(&env.ctx, kStockCode, cfg);
		cfg->release();
		ASSERT_TRUE(env.ctx.hasLog("初始化完成"));

		WTSTickData* tick = makeStockTick(kStockCode, 100000000, 10.0, 9.99, 10.01);
		unit.on_tick(tick);
		unit.on_channel_ready();
		env.ctx.undone = -100;
		unit.set_position(kStockCode, 1000.0);
		ASSERT_EQ(env.ctx.bydirCancels.size(), 1u);
		EXPECT_EQ(env.ctx.bydirCancels[0].first, std::string(kStockCode));
		EXPECT_FALSE(env.ctx.bydirCancels[0].second);
		EXPECT_TRUE(env.ctx.buys.empty());
		tick->release();
	}

	// 目标买 1000，未完成 +100（同向）-> L542-546 记“上一轮有挂单未完成”后返回
	{
		Env env;
		env.comm = makeStockComm("STK", "R115SV5", "SHSE");
		env.sess = makeSession();
		env.ctx.comm = env.comm;
		env.ctx.sess = env.sess;
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = makeVwapCfg(10, 0, 0, 100.0, 100.0);
		unit.init(&env.ctx, kStockCode, cfg);
		cfg->release();
		ASSERT_TRUE(env.ctx.hasLog("初始化完成"));

		WTSTickData* tick = makeStockTick(kStockCode, 100000000, 10.0, 9.99, 10.01);
		unit.on_tick(tick);
		unit.on_channel_ready();
		env.ctx.undone = 100;
		unit.set_position(kStockCode, 1000.0);
		EXPECT_TRUE(env.ctx.buys.empty());
		EXPECT_TRUE(env.ctx.hasLog("上一轮有挂单未完成"));
		tick->release();
	}
}
