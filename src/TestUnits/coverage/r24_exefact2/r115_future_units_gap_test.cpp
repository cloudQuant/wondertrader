// R115 lane：WtExeFact 期货执行单元 + WtOrdMon 的残余行
// 目标文件与行段（本轮起点为 reports/coverage-20261028T091357Z.txt 之后的 llvm-cov show 实读）：
//   src/WtExeFact/WtDiffMinImpactExeUnit.cpp: L137（全成交重置 cancel_times）、L217/L228
//       （on_tick 空/异码、首笔非交易时段过滤）、L443/L446（非 mode2 的零价回退）、L463-467（跌停钳位）
//   src/WtExeFact/WtMinImpactExeUnit.cpp: L174（全成交重置）、L254/L265（空/异码、首笔非交易时段）、
//       L434（byrate 至少 1 手）、L471-475/L482/L485（自动价下侧与零价回退）、
//       L533-537（跌停钳位）、L579（set_position(DBL_MAX) 的清仓日志）
//   src/WtExeFact/WtVWapExeUnit.cpp: L74-84（calTmStamp 的午休区间/午休钳位/下午区间；
//       L68-69 的 hour<9:30 分支因 to_string 丢前导 0 实际不可达，见 BUG-R115-001）、
//       L473-477（撤单重发时的涨停钳位）
//   src/WtExeFact/WtOrdMon.cpp: L86-88（enumOrder 回调体）
//
// 与 R7/R17/R24 的分工：R7 已覆盖四类单元的主链、n05 覆盖 MinImpact 主链；R17 覆盖 Diff 主链与
// 上下限组合、SimpData/Porter 拒绝面；R24 覆盖 StockVWap/StockMinImpact 的通道就绪、超时扫描与
// fire_at_once 卖侧。本文件只打上表仍为 0 的行，不重复它们的用例。
//
// 纪律：单线程、无墙钟断言（超时判定只用注入的 getCurTime 或 32 位乘法回绕的确定性输入）、
// 临时文件只落在系统临时目录并在析构清理；不加载任何插件 DLL。
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
#include "WtExeFact/WtOrdMon.h"
#include "WtExeFact/WtDiffMinImpactExeUnit.h"
#include "WtExeFact/WtMinImpactExeUnit.h"
#include "WtExeFact/WtVWapExeUnit.h"

using namespace wtp;

namespace
{
const char* const kCode = "SHFE.rb.2405";

//////////////////////////////////////////////////////////////////////////
// 执行环境替身：只记录公开 API 的调用，不建线程、不碰网络/共享内存
//////////////////////////////////////////////////////////////////////////
class FakeCtx : public ExecuteContext
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

	double getPosition(const char* /*stdCode*/, bool validOnly = true, int32_t flag = 3) override
	{
		if (flag == 1)
			return long_pos;			// 多仓（Stock 单元清仓分支用；本文件不触达）
		return validOnly ? pos_valid : pos_total;
	}

	OrderMap* getOrders(const char*) override { return NULL; }
	double getUndoneQty(const char*) override { return undone; }

	OrderIDs buy(const char* stdCode, double price, double qty, bool bForceClose) override
	{
		buys.push_back({ stdCode, price, qty, bForceClose });
		OrderIDs ids{ nextId++ };
		lastIds = ids;
		lastId = ids.empty() ? 0 : ids[0];
		return ids;
	}

	OrderIDs sell(const char* stdCode, double price, double qty, bool bForceClose) override
	{
		sells.push_back({ stdCode, price, qty, bForceClose });
		OrderIDs ids{ nextId++ };
		lastIds = ids;
		lastId = ids.empty() ? 0 : ids[0];
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
		lastIds = ids;
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
	double pos_valid = 0;	// InterfaceExecuteDefs.h 的真实语义：true=可用（昨仓）
	double pos_total = 0;	// false=总仓位
	double long_pos = 0;	// flag=1 的多仓
	double undone = 0;
	uint64_t curTime = 20240102093000000ull;
	uint32_t nextId = 9500;
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
	OrderIDs lastIds;
};

WTSCommodityInfo* makeFutComm(const char* pid = "rb", const char* name = "r115螺纹")
{
	WTSCommodityInfo* c = WTSCommodityInfo::create(pid, name, "SHFE", "R115DAY", "CHN");
	c->setVolScale(10);
	c->setPriceTick(1.0);
	c->setLotsTick(1.0);
	c->setMinLots(1.0);
	c->setCategory(CC_Future);
	c->setCoverMode(CM_OpenCover);
	c->setPriceMode(PM_Both);
	c->setTradingMode(TM_Both);
	return c;
}

WTSSessionInfo* makeDaySession(uint32_t first = 930, uint32_t last = 1130)
{
	WTSSessionInfo* s = WTSSessionInfo::create("R115DAY", "r115 日盘", 0);
	s->addTradingSection(first, last);
	return s;
}

WTSTickData* makeTick(const char* code, uint32_t date, uint32_t actTime, double lastPx,
	double preClose, double bidPx, double bidQty, double askPx, double askQty,
	double upper = 0, double lower = 0)
{
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	snprintf(ts.code, sizeof(ts.code), "%s", code);
	ts.action_date = date;
	ts.trading_date = date;
	ts.action_time = actTime;
	ts.price = lastPx;
	ts.pre_close = preClose;
	ts.bid_prices[0] = bidPx;
	ts.bid_qty[0] = bidQty;
	ts.ask_prices[0] = askPx;
	ts.ask_qty[0] = askQty;
	ts.upper_limit = upper;
	ts.lower_limit = lower;
	return WTSTickData::create(ts);
}

// 一个用例一个临时 CWD + 一个分布文件；返回系统临时目录里的绝对路径，析构时恢复 CWD 再删目录
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

	std::string dir() const { return _dir.string(); }

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

// 与单元析构顺序配对：unit 先析构（release 自己 retain 的 comm/sess），再释放夹具的引用
struct Env
{
	FakeCtx ctx;
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

WTSVariant* makeCfg(const char* priceKey, int32_t offset, uint32_t expire, int32_t priceMode,
	uint32_t span, bool byRate, double lots, double rate, double minOpenLots)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("offset", offset);
	cfg->append("expire", expire);
	cfg->append(priceKey, priceMode);
	cfg->append("span", span);
	cfg->append("byrate", byRate);
	cfg->append("lots", lots);
	cfg->append("rate", rate);
	cfg->append("minopenlots", minOpenLots);
	return cfg;
}
}	// namespace

//////////////////////////////////////////////////////////////////////////
// DiffMinImpact：空/异码行情、非 mode2 的零价回退、跌停钳位、全成交回报
// 生产文件: src/WtExeFact/WtDiffMinImpactExeUnit.cpp:217,443,446,462-467,137
//////////////////////////////////////////////////////////////////////////
TEST(r115_exefact, diff_minimpact_tick_fallbacks_lower_limit_and_fill_report)
{
	Env env;
	env.comm = makeFutComm();
	env.sess = makeDaySession(900, 1500);
	env.ctx.comm = env.comm;
	env.ctx.sess = env.sess;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = makeCfg("pricemode", 0, 1, -1, 0, false, 2.0, 0.0, 1.0);
		unit.init(&env.ctx, kCode, cfg);
		cfg->release();

		// 目标先于行情给出：do_calc 在 L346-350 等待 tick
		unit.set_position(kCode, -3.0);
		EXPECT_TRUE(env.ctx.sells.empty());

		// L217：空指针 + 异码，都不应改变内部状态
		unit.on_tick(NULL);
		WTSTickData* foreign = makeTick("SHFE.rb.2410", 20240102, 93000000, 3500, 3400, 3499, 10, 3501, 10);
		unit.on_tick(foreign);
		foreign->release();

		// L443/L446：mode=-1 取买一/卖一，二者都为 0 时回退到最新价；
		// L462-467：卖价 3500 低于跌停 3600，被钳到 3600 且不可撤
		WTSTickData* tick = makeTick(kCode, 20240102, 93000000, 3500, 3400, 0, 100, 0, 100, 0, 3600);
		unit.on_tick(tick);
		ASSERT_EQ(env.ctx.sells.size(), 1u);
		EXPECT_DOUBLE_EQ(env.ctx.sells[0].price, 3600.0);
		EXPECT_DOUBLE_EQ(env.ctx.sells[0].qty, 2.0);

		// L136-137：leftover==0 且非撤单 -> 重置 cancel_times；换一笔更新的行情再算一轮
		unit.on_order(env.ctx.lastId, kCode, false, 0.0, 3600.0, false);
		WTSTickData* tick2 = makeTick(kCode, 20240102, 93100000, 3502, 3400, 0, 100, 0, 100, 0, 3600);
		unit.on_tick(tick2);
		EXPECT_EQ(env.ctx.sells.size(), 2u);
		tick->release();
		tick2->release();
	}
}

//////////////////////////////////////////////////////////////////////////
// Diff / MinImpact：首笔行情不在交易时段被过滤 + MinImpact 清仓目标日志
// 生产文件: WtDiffMinImpactExeUnit.cpp:228；WtMinImpactExeUnit.cpp:254,265,579
//////////////////////////////////////////////////////////////////////////
TEST(r115_exefact, first_tick_outside_session_is_filtered_and_clear_logs)
{
	Env denv;
	denv.comm = makeFutComm();
	denv.sess = makeDaySession(900, 1000);
	denv.ctx.comm = denv.comm;
	denv.ctx.sess = denv.sess;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = makeCfg("pricemode", 0, 1, 0, 0, false, 1.0, 0.0, 1.0);
		unit.init(&denv.ctx, kCode, cfg);
		cfg->release();
		unit.set_position(kCode, 5.0);
		WTSTickData* tick = makeTick(kCode, 20240102, 200000000, 3500, 3400, 3499, 10, 3501, 10);
		unit.on_tick(tick);					// L227-228：20:00 不在 09:00-10:00
		EXPECT_TRUE(denv.ctx.buys.empty());
		tick->release();
	}

	Env menv;
	menv.comm = makeFutComm();
	menv.sess = makeDaySession(900, 1000);
	menv.ctx.comm = menv.comm;
	menv.ctx.sess = menv.sess;
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = makeCfg("pricemode", 0, 1, 0, 0, false, 1.0, 0.0, 1.0);
		unit.init(&menv.ctx, kCode, cfg);
		cfg->release();

		// L254：空/异码早退
		unit.on_tick(NULL);
		WTSTickData* foreign = makeTick("SHFE.rb.2410", 20240102, 93000000, 3500, 3400, 3499, 10, 3501, 10);
		unit.on_tick(foreign);
		foreign->release();

		unit.set_position(kCode, 5.0);
		WTSTickData* tick = makeTick(kCode, 20240102, 200000000, 3500, 3400, 3499, 10, 3501, 10);
		unit.on_tick(tick);					// L264-265：非交易时段过滤
		EXPECT_TRUE(menv.ctx.buys.empty());

		// L578-579：进入清仓处理（DBL_MAX）的日志分支；无 tick 时 do_calc 在 L386-390 退出
		unit.set_position(kCode, DBL_MAX);
		EXPECT_TRUE(menv.ctx.hasLog("clearing processing"));
		tick->release();
	}
}

//////////////////////////////////////////////////////////////////////////
// MinImpact：自动价下侧 + 零价回退 + 跌停钳位 + 全成交回报；byrate 至少 1 手
// 生产文件: src/WtExeFact/WtMinImpactExeUnit.cpp:471-475,482,485,533-537,173-174,429-434
//////////////////////////////////////////////////////////////////////////
TEST(r115_exefact, minimpact_auto_down_lower_limit_fill_and_rate_floor)
{
	Env env;
	env.comm = makeFutComm();
	env.sess = makeDaySession(900, 1500);
	env.ctx.comm = env.comm;
	env.ctx.sess = env.sess;
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = makeCfg("pricemode", 0, 1, 2, 0, false, 5.0, 0.0, 1.0);
		unit.init(&env.ctx, kCode, cfg);
		cfg->release();
		unit.set_position(kCode, -5.0);
		// bidqty<askqty -> L471-475 走下侧；bid 价为 0 -> L481-485 回退到最新价；
		// 跌停 3600 高于卖价 -> L532-537 钳位
		WTSTickData* tick = makeTick(kCode, 20240102, 93000000, 3500, 3400, 0, 100, 3600, 200, 0, 3600);
		unit.on_tick(tick);
		ASSERT_EQ(env.ctx.sells.size(), 1u);
		EXPECT_DOUBLE_EQ(env.ctx.sells[0].price, 3600.0);
		EXPECT_DOUBLE_EQ(env.ctx.sells[0].qty, 5.0);

		// L163-174：leftover==0 且非撤单 -> 从监控器删除并重置 cancel_times
		unit.on_order(env.ctx.lastId, kCode, false, 0.0, 3600.0, false);
		tick->release();
	}

	Env renv;
	renv.comm = makeFutComm();
	renv.sess = makeDaySession(900, 1500);
	renv.ctx.comm = renv.comm;
	renv.ctx.sess = renv.sess;
	{
		WtMinImpactExeUnit unit;
		// byrate=true, rate=0.1, 对手只有 1 手 -> round(0.1)=0, L433-434 抬到 1 手
		WTSVariant* cfg = makeCfg("pricemode", 0, 0, 0, 0, true, 100.0, 0.1, 1.0);
		unit.init(&renv.ctx, kCode, cfg);
		cfg->release();
		unit.set_position(kCode, 5.0);
		WTSTickData* tick = makeTick(kCode, 20240102, 93000000, 3500, 3400, 3499, 1, 3501, 1);
		unit.on_tick(tick);
		ASSERT_EQ(renv.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(renv.ctx.buys[0].qty, 1.0);
		tick->release();
	}
}

//////////////////////////////////////////////////////////////////////////
// VWap：calTmStamp 的四个时段分支（09:00 / 12:00 午休钳位 / 14:00）+ 撤单重发涨停钳位
// 生产文件: src/WtExeFact/WtVWapExeUnit.cpp:68-84,468-477
//////////////////////////////////////////////////////////////////////////
TEST(r115_exefact, vwap_time_buckets_and_cancel_refire_upper_limit)
{
	TempCwd cwd("vwap");
	{
		std::string body;
		for (uint32_t i = 0; i < 400; i++)
		{
			if (i)
				body += ",";
			body += "1000";
		}
		cwd.write("Vwap_R115VW.txt", body);
	}

	Env env;
	env.comm = makeFutComm("rb", "R115VW");
	env.ctx.comm = env.comm;
	{
		WtVWapExeUnit unit;
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("begin_time", (uint32_t)900);
		cfg->append("end_time", (uint32_t)1500);
		cfg->append("ord_sticky", (uint32_t)0);
		cfg->append("tail_secs", (uint32_t)0);
		cfg->append("total_times", (uint32_t)10);
		cfg->append("price_mode", (uint32_t)0);
		cfg->append("price_offset", (uint32_t)0);
		cfg->append("lots", 100.0);
		cfg->append("minopenlots", 100.0);
		unit.init(&env.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 1000.0);	// 无 tick：L341-345
		unit.on_channel_ready();			// 无 tick：L341-345
		EXPECT_TRUE(env.ctx.buys.empty());

		// L70-71：10:00 落上午区间（L68-69 的 hour<9:30 分支见文件头 BUG-R115-001，实际不可达）；
		// 首笔 tick 触发 do_calc
		WTSTickData* t1 = makeTick(kCode, 20240102, 100000000, 3500, 3400, 3499, 100, 3501, 100, 3900, 3000);
		unit.on_tick(t1);
		ASSERT_EQ(env.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(env.ctx.buys[0].price, 3500.0);

		// L74-75 + L82-84：12:00 先落午休区间，再被 113000000..130000000 钳到第 120 分钟
		WTSTickData* t2 = makeTick(kCode, 20240102, 120000000, 3600, 3400, 3599, 100, 3601, 100, 3900, 3000);
		unit.on_tick(t2);
		ASSERT_EQ(env.ctx.buys.size(), 2u);
		EXPECT_DOUBLE_EQ(env.ctx.buys[1].price, 3600.0);

		// L77-78：14:00 走下午区间；最新价 4000 高于涨停 3900 -> 主链先钳到 3900
		WTSTickData* t3 = makeTick(kCode, 20240102, 140000000, 4000, 3400, 3999, 100, 4001, 100, 3900, 3000);
		unit.on_tick(t3);
		ASSERT_EQ(env.ctx.buys.size(), 3u);
		EXPECT_DOUBLE_EQ(env.ctx.buys[2].price, 3900.0);

		// 撤单回报 -> fire_at_once；cancel_times=1 使价格 4001，仍高于涨停 -> L473-477
		unit.on_order(env.ctx.lastId, kCode, true, 0.0, 3900.0, true);
		ASSERT_EQ(env.ctx.buys.size(), 4u);
		EXPECT_DOUBLE_EQ(env.ctx.buys[3].price, 3900.0);

		t1->release();
		t2->release();
		t3->release();
	}
}

//////////////////////////////////////////////////////////////////////////
// WtOrdMon：enumOrder 的完整回调体（L86-88）
// 生产文件: src/WtExeFact/WtOrdMon.cpp:77-91
//////////////////////////////////////////////////////////////////////////
TEST(r115_exefact, ordmon_enum_reports_id_time_and_cancel_flag)
{
	WtOrdMon mon;
	uint32_t ids[2] = { 11, 22 };
	mon.push_order(ids, 2, 777, true);
	uint32_t single = 33;
	mon.push_order(&single, 1, 888, false);

	std::vector<std::pair<uint32_t, uint64_t> > seen;
	uint32_t nonCancel = 0;
	mon.enumOrder([&](uint32_t localid, uint64_t entertime, bool cancancel) {
		seen.push_back(std::make_pair(localid, entertime));
		if (!cancancel)
			nonCancel++;
	});
	ASSERT_EQ(seen.size(), 3u);
	EXPECT_EQ(nonCancel, 1u);
	bool has11 = false, has22 = false, has33 = false;
	for (size_t i = 0; i < seen.size(); i++)
	{
		if (seen[i].first == 11 && seen[i].second == 777)
			has11 = true;
		if (seen[i].first == 22 && seen[i].second == 777)
			has22 = true;
		if (seen[i].first == 33 && seen[i].second == 888)
			has33 = true;
	}
	EXPECT_TRUE(has11);
	EXPECT_TRUE(has22);
	EXPECT_TRUE(has33);

	mon.clear_orders();
	seen.clear();
	mon.enumOrder([&](uint32_t localid, uint64_t, bool) { seen.push_back(std::make_pair(localid, 0)); });
	EXPECT_TRUE(seen.empty());
}
