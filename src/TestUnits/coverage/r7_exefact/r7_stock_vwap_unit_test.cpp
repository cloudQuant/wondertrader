// R7 lane：WtStockVWapExeUnit（股票/科创板 VWAP 分片执行单元）
// 目标文件: src/WtExeFact/WtStockVWapExeUnit.cpp / WtStockVWapExeUnit.h
// 覆盖面: init（产品分类及数量场景、可选键 "offset"、分布文件解析）、
//         do_calc 的持仓不足目标修正、碎股完成判定、成交量分片被丢弃的路径、价格模式与涨跌停、
//         on_tick/on_order/on_channel_ready/on_entrust/on_trade/clear_all_position/set_position 各分支。
//
// 关键量（L601-609）：isBuy 在 L530 由 **undone** 决定，而能走到这里说明 undone==0（L542 已拦），
// 所以恒为 false -> 永远走「卖」分支，且 L609 无条件 `curQty = min(vailyPos, _min_open_lots)`
// 覆盖了 L601/L607 的结果。分桶用例用两个低于目标的累计量断言较大分桶应计划较大买量；
// 当前覆写会让这条正确性断言失败。减仓目标也会因正的 `curQty` 被发成买单，见 bugs/R7_exefact.md。
//
// 时序：tail_secs 一律取 0 —— init 在 L192 计算 _fire_span 时 _total_secs 仍是 0（L198 才赋值），
// tail_secs=0 时 _fire_span=0，on_tick 非首笔分支的间隔判定恒真（同 BUG-R7-001）。
// 驱动顺序：init -> set_position -> on_tick -> on_channel_ready（do_calc L517 在 L547 的
// 空指针检查之前就解引用 _last_tick，没有 tick 会崩，见 BUG-R7-005）。
#include <gtest/gtest.h>

#include <cmath>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "Includes/ExecuteDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "WtExeFact/WtStockVWapExeUnit.h"
#include "r7_init_probe.h"

using namespace wtp;

namespace
{
// stdCode 需要 3 段：init L201 与 get_minOrderQty 都用 split(stdCode,".")[2] 取代码号
const char* const kStk = "SHSE.STK.600000";      // 普通股（< 688000）
const char* const kKcb = "SHSE.STK.688001";      // 科创板（>= 688000）
const char* const kBond = "SHSE.CBOND.113001";   // 可转债

class SFakeCtx : public ExecuteContext
{
public:
	struct Rec { std::string code; double price; double qty; };

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return NULL; }
	WTSTickData* grabLastTick(const char*) override { return NULL; }
	// ExecuteContext: validOnly=true (including the default) returns available
	// holdings; false returns total holdings. The strategy flag selects sides,
	// which this single-sided fixture does not model separately.
	double getPosition(const char*, bool validOnly = true, int32_t = 3) override
	{
		return validOnly ? avail : pos;
	}
	OrderMap* getOrders(const char*) override { return NULL; }
	double getUndoneQty(const char*) override { return undone; }
	OrderIDs buy(const char* c, double p, double q, bool) override
	{
		buys.push_back({ c, p, q });
		OrderIDs ids{ nextId++ };
		return ids;
	}
	OrderIDs sell(const char* c, double p, double q, bool) override
	{
		sells.push_back({ c, p, q });
		OrderIDs ids{ nextId++ };
		return ids;
	}
	bool cancel(uint32_t id) override { byidCancels.push_back(id); return true; }
	OrderIDs cancel(const char* c, bool isBuy, double = 0) override
	{
		bydirCancels.push_back({ c, isBuy });
		OrderIDs ids;
		for (uint32_t i = 0; i < cancelBydirRetCnt; i++)
			ids.push_back(nextId++);
		return ids;
	}
	void writeLog(const char* l) override { logs.push_back(l ? l : ""); }
	WTSCommodityInfo* getCommodityInfo(const char*) override { return comm; }
	WTSSessionInfo* getSessionInfo(const char*) override { return sess; }
	uint64_t getCurTime() override { return 20240103093000000ull; }

	double pos = 0;
	double avail = 0;
	double undone = 0;
	uint32_t nextId = 7000;
	uint32_t cancelBydirRetCnt = 2;
	WTSCommodityInfo* comm = NULL;
	WTSSessionInfo* sess = NULL;
	std::vector<Rec> buys;
	std::vector<Rec> sells;
	std::vector<uint32_t> byidCancels;
	std::vector<std::pair<std::string, bool>> bydirCancels;
	std::vector<std::string> logs;

	bool hasLog(const std::string& sub) const { return countLog(sub) > 0; }
	uint32_t countLog(const std::string& sub) const
	{
		uint32_t n = 0;
		for (const std::string& l : logs)
			if (l.find(sub) != std::string::npos)
				n++;
		return n;
	}
};

inline std::string sCommName()
{
	return "R7SVWAP" + std::to_string((long long)getpid());
}

class SVwapFile
{
public:
	explicit SVwapFile(double bucketVal, uint32_t rows = 241)
	{
		_path = std::string("Vwap_") + sCommName() + ".txt";
		std::ofstream f(_path.c_str());
		for (uint32_t i = 0; i < rows; i++)
		{
			if (i)
				f << ",";
			f << bucketVal;
		}
		f.close();
	}
	~SVwapFile() { std::remove(_path.c_str()); }

private:
	std::string _path;
};

class ScopedProbeDir
{
public:
	ScopedProbeDir()
	{
		std::error_code ec;
		const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(ec);
		if (ec)
			return;
		const std::string pattern = (tempRoot / "WT_R7_005_XXXXXX").string();
		std::vector<char> writable(pattern.begin(), pattern.end());
		writable.push_back('\0');
		char* created = ::mkdtemp(writable.data());
		if (created == nullptr)
			return;

		_path = created;
		_owns = true;
		_ready = true;
	}

	~ScopedProbeDir() { cleanup(); }

	bool ready() const { return _ready; }

	bool cleanup()
	{
		if (!_owns)
			return true;

		std::error_code ec;
		std::filesystem::remove_all(_path, ec);
		if (!ec)
			_owns = false;
		return !ec;
	}

	const std::string& path() const { return _path; }

private:
	std::string _path;
	bool _owns{ false };
	bool _ready{ false };
};

// WTSCommodityInfo::create 的第 1 个参数是 product， 第 5 个参数是交易模板。
// 额外 retain() 一次是为了抵消 get_minOrderQty（WtStockVWapExeUnit.h:386）对
// ctx->getCommodityInfo() 借来指针的那次 release —— 否则引用计数净少 1，
// 单元析构就会把对象打回 0，用例收尾的 release 会踩已销毁对象。每个 commodity
// 只供一个执行单元使用，因为 helper 对借来的指针执行一次 release。
WTSCommodityInfo* mkSComm(const char* product)
{
	WTSCommodityInfo* c = WTSCommodityInfo::create(product, sCommName().c_str(), "SHSE", "NIGHT_DAY", "NIGHT_DAY");
	c->retain();
	c->setVolScale(1);
	c->setPriceTick(0.01);
	c->setLotsTick(1.0);
	c->setMinLots(1.0);
	c->setCategory(CC_Stock);
	c->setCoverMode(CM_OpenCover);
	c->setPriceMode(PM_Both);
	c->setTradingMode(TM_Long);          // 股票只可做多 -> init L221 置 _is_t0
	return c;
}

WTSTickData* mkSTick(const char* code, uint32_t actTime, double lastPx,
	double bidPx, double askPx, double upper = 0, double lower = 0)
{
	WTSTickStruct ts;
	snprintf(ts.code, sizeof(ts.code), "%s", code);
	ts.action_date = 20240103;
	ts.trading_date = 20240103;
	ts.action_time = actTime;
	ts.price = lastPx;
	ts.pre_close = 10.0;
	ts.bid_prices[0] = bidPx;
	ts.bid_qty[0] = 1000;
	ts.ask_prices[0] = askPx;
	ts.ask_qty[0] = 900;
	ts.upper_limit = upper;
	ts.lower_limit = lower;
	return WTSTickData::create(ts);
}

// L178-189 读取的键（注意价格偏移的键名是 "offset"）
WTSVariant* mkSCfg(uint32_t totalTimes = 5, uint32_t priceMode = 0, uint32_t offset = 0,
	double minOpenLots = 100.0, double lots = 1000.0, uint32_t ordSticky = 0,
	uint32_t tailSecs = 0)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("begin_time", (uint32_t)930);
	cfg->append("end_time", (uint32_t)1500);
	cfg->append("ord_sticky", ordSticky);
	cfg->append("tail_secs", tailSecs);
	cfg->append("total_times", totalTimes);
	cfg->append("price_mode", priceMode);
	cfg->append("offset", offset);
	cfg->append("lots", lots);
	cfg->append("minopenlots", minOpenLots);
	return cfg;
}

// 唯一安全顺序：init -> 落目标 -> 先给 tick -> 再就绪
void bootS(WtStockVWapExeUnit& unit, SFakeCtx& ctx, const char* code, double target,
	WTSVariant* cfg, uint32_t actTime = 100000000, double lastPx = 10.50,
	double bidPx = 10.49, double askPx = 10.51)
{
	unit.init(&ctx, code, cfg);
	cfg->release();
	unit.set_position(code, target);
	unit.on_tick(mkSTick(code, actTime, lastPx, bidPx, askPx));
	unit.on_channel_ready();
}

const char* channel_ready_probe_marker(bool provideTick)
{
	return provideTick
		? "BUG-R7-005 probe: before on_channel_ready, tick=present\n"
		: "BUG-R7-005 probe: before on_channel_ready, tick=absent\n";
}

int run_channel_ready_probe(bool provideTick, double target, int markerFd)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 0;
	ctx.avail = 0;
	int result = 0;
	{
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = mkSCfg();
		unit.init(&ctx, kStk, cfg);
		cfg->release();
		unit.set_position(kStk, target);
		if (provideTick)
		{
			WTSTickData* tick = mkSTick(kStk, 100000000, 10.50, 10.49, 10.51);
			unit.on_tick(tick);
			tick->release(); // on_tick retains the accepted tick; leave that reference to the unit.
		}
		const char* marker = channel_ready_probe_marker(provideTick);
		size_t markerBytes = 0;
		const size_t markerSize = std::strlen(marker);
		while (markerBytes < markerSize)
		{
			ssize_t written = ::write(markerFd, marker + markerBytes, markerSize - markerBytes);
			if (written > 0)
				markerBytes += (size_t)written;
			else if (written < 0 && errno == EINTR)
				continue;
			else
				return 4;
		}
		unit.on_channel_ready();
		if (!ctx.buys.empty() || !ctx.sells.empty())
			result = 2;
	}
	ctx.comm->release();
	return result;
}

struct ProbeChildResult
{
	bool started{ false };
	bool waited{ false };
	bool markerReceived{ false };
	int waitStatus{ 0 };
	int error{ 0 };
	std::string output;
};

ProbeChildResult run_channel_ready_probe_in_child(const std::string& tempDir,
	bool provideTick, double target)
{
	ProbeChildResult result;
	int markerPipe[2];
	if (::pipe(markerPipe) != 0)
	{
		result.error = errno;
		return result;
	}

	// The child executes C++ setup after fork. POSIX only guarantees async-signal-safe
	// calls before exec when the parent is multithreaded; keep this probe in a
	// single-threaded test process and do not reuse it from a threaded host.
	pid_t child = ::fork();
	if (child < 0)
	{
		result.error = errno;
		::close(markerPipe[0]);
		::close(markerPipe[1]);
		return result;
	}
	if (child == 0)
	{
		::close(markerPipe[0]);
		if (::chdir(tempDir.c_str()) != 0)
			::_exit(125);
		::alarm(5);
		int exitCode = run_channel_ready_probe(provideTick, target, markerPipe[1]);
		::close(markerPipe[1]);
		::_exit(exitCode);
	}

	result.started = true;
	::close(markerPipe[1]);
	char buffer[128];
	for (;;)
	{
		ssize_t count = ::read(markerPipe[0], buffer, sizeof(buffer));
		if (count > 0)
			result.output.append(buffer, (size_t)count);
		else if (count == 0)
			break;
		else if (errno != EINTR)
		{
			result.error = errno;
			break;
		}
	}
	::close(markerPipe[0]);
	result.markerReceived = result.output.find(channel_ready_probe_marker(provideTick)) != std::string::npos;

	pid_t waited;
	do
	{
		waited = ::waitpid(child, &result.waitStatus, 0);
	}
	while (waited < 0 && errno == EINTR);
	if (waited == child)
		result.waited = true;
	else if (result.error == 0)
		result.error = errno;
	return result;
}

void expect_channel_ready_probe_success(const ProbeChildResult& result)
{
	EXPECT_TRUE(result.started) << "fork/pipe failed with errno " << result.error;
	EXPECT_TRUE(result.markerReceived) << "child did not print the pre-call marker; output: " << result.output;
	EXPECT_TRUE(result.waited) << "waitpid failed with errno " << result.error;
	if (!result.waited)
		return;
	if (WIFEXITED(result.waitStatus))
		EXPECT_EQ(WEXITSTATUS(result.waitStatus), 0) << "child output: " << result.output;
	else if (WIFSIGNALED(result.waitStatus))
		ADD_FAILURE() << "child terminated by signal " << WTERMSIG(result.waitStatus)
			<< "; output: " << result.output;
	else
		ADD_FAILURE() << "child did not exit normally; status=" << result.waitStatus;
}


int run_stock_vwap_init_probe(uint32_t totalTimes, int markerFd)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	int exitCode = 0;
	{
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = mkSCfg(totalTimes, 0, 0, 100.0, 1000.0, 0, 0);
		if (!r7_test::write_marker(markerFd))
			exitCode = 4;
		else
			unit.init(&ctx, kStk, cfg);
		cfg->release();
	}
	ctx.comm->release();
	return exitCode;
}

}

//////////////////////////////////////////////////////////////////////////
// 名字契约
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, names_and_fact_binding)
{
	WtStockVWapExeUnit unit;
	EXPECT_STREQ(unit.getName(), "WtStockVWapExeUnit");   // L150
	EXPECT_STREQ(unit.getFactName(), "WtExeFact");        // L141
}

//////////////////////////////////////////////////////////////////////////
// SFakeCtx follows ExecuteContext's public validOnly contract.
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, fake_position_matches_execute_context_contract)
{
	SFakeCtx ctx;
	ctx.pos = 1000;
	ctx.avail = 200;

	EXPECT_DOUBLE_EQ(ctx.getPosition(kStk), 200.0);        // default validOnly=true
	EXPECT_DOUBLE_EQ(ctx.getPosition(kStk, true), 200.0);
	EXPECT_DOUBLE_EQ(ctx.getPosition(kStk, false), 1000.0);
}

//////////////////////////////////////////////////////////////////////////
// 普通股增仓：方向为买，数量规则不由本用例定合同（当前 L609 会覆盖分桶计算）。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, normal_stock_increase_emits_positive_buy)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 1000;
	ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 1500, mkSCfg(5, 0, 0, 200.0, 1000.0));
	// 增仓目标 1000->1500；L609 会覆盖分桶计算，订单数量不作为业务合同。
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_GT(ctx.buys[0].qty, 0.0);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.50);      // price_mode=0 -> 最新价
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 不同累计 VWAP 目标应产生不同的正向切片；不固定精确数量。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, vwap_bucket_distribution_changes_planned_buy_qty)
{
	SFakeCtx c1, c2;
	c1.comm = mkSComm("STK");
	c2.comm = mkSComm("STK");
	c1.pos = c2.pos = 1000;
	c1.avail = c2.avail = 1000;
	WtStockVWapExeUnit u1, u2;
	{
		SVwapFile vfA(1300.0);
		bootS(u1, c1, kStk, 4000, mkSCfg());          // diffQty=3000，累计分桶目标 1300
	}
	{
		SVwapFile vfB(2500.0);
		bootS(u2, c2, kStk, 4000, mkSCfg());          // 同一目标，累计分桶目标 2500
	}
	// 头文件定义 VwapAim 为按分钟的目标总报单量；在 10:00 的 bucket 30，增量分别为 300 和 1500。
	ASSERT_EQ(c1.buys.size(), 1u);
	ASSERT_EQ(c2.buys.size(), 1u);
	EXPECT_GT(c1.buys[0].qty, 0.0);
	EXPECT_GT(c2.buys[0].qty, c1.buys[0].qty);
	c1.comm->release();
	c2.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 科创板增仓方向；具体最小手数/切片量仍待业务合同确认。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, kcb_stock_increase_emits_positive_buy)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 5000;
	ctx.avail = 5000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kKcb, 6000, mkSCfg(5, 0, 0, 50.0, 1000.0));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_GT(ctx.buys[0].qty, 0.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 可转债增仓方向；具体最小手数/切片量仍待业务合同确认。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, convertible_bond_increase_emits_positive_buy)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("CBOND");
	ctx.pos = 5000;
	ctx.avail = 5000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kBond, 6000, mkSCfg(5, 0, 0, 50.0, 1000.0));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_GT(ctx.buys[0].qty, 0.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 空仓建仓目标应买入；当前产品错误地落入 0 量卖单分支（BUG-R7-007）。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, flat_position_buy_target_requires_a_buy)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 0;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 1000, mkSCfg(5, 0, 0, 100.0, 1000.0));
	// 目标从空仓增至 1000，应产生正向买入；数量规则没有在接口中约定。
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_GT(ctx.buys[0].qty, 0.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 碎股清仓：方向应为卖出，数量不得超过可用仓；产品当前会反向买入（BUG-R7-007）。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, odd_lot_clear_target_sells_within_available_position)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 40;
	ctx.avail = 40;
	WtStockVWapExeUnit unit;
	// 先落一个非 0 目标，再用 clear_all_position 把目标打到 0（set_position(0) 会被判等丢弃）
	unit.init(&ctx, kStk, mkSCfg(5, 0, 0, 100.0, 1000.0));
	unit.set_position(kStk, 400);
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit.on_channel_ready();
	uint32_t sellsBefore = (uint32_t)ctx.sells.size();
	unit.clear_all_position(kStk);
	ASSERT_EQ(ctx.sells.size(), sellsBefore + 1u);
	EXPECT_GT(ctx.sells.back().qty, 0.0);
	EXPECT_LE(ctx.sells.back().qty, ctx.avail);
	EXPECT_EQ(ctx.buys.size(), 1u); // only the initial increase may buy
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 完成判定：按整手取整后目标与实仓相等即置 _is_finish（L521-526）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, rounding_to_whole_lots_marks_finish_and_stops)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 950;
	ctx.avail = 950;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 1000, mkSCfg(5, 0, 0, 100.0, 1000.0));
	// round_hands(1000,100)=(int)((1000+50)/100)*100=1000
	// round_hands(950,100)=(int)((950+50)/100)*100=1000 -> 相等 -> _is_finish
	EXPECT_TRUE(ctx.hasLog("target position"));
	EXPECT_TRUE(ctx.hasLog("set finish"));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 非 T+0 应使用 total-available 保留不可卖持仓；正确目标应上抬到 800。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, insufficient_sellable_position_adjusts_target_up)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	WTSCommodityInfo* c = mkSComm("STK");
	c->setTradingMode(TM_Both);          // 非 T+0；接口规定 false 返回总仓，产品当前将其用作 vailyPos
	ctx.comm = c;
	ctx.pos = 1000;
	ctx.avail = 200;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 0, mkSCfg(5, 0, 0, 100.0, 1000.0));
	EXPECT_TRUE(ctx.hasLog("can sell hold pos not enough"));
	EXPECT_TRUE(ctx.hasLog("target adjust"));
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_GT(ctx.sells[0].qty, 0.0);
	EXPECT_LE(ctx.sells[0].qty, ctx.avail);
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 价格模式与偏移：增仓路径覆盖 mode1 和 "offset" 键（不是 "price_offset"）。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, price_mode_and_offset_key)
{
	SVwapFile vf(25.0);
	SFakeCtx c1, c2;
	c1.comm = mkSComm("STK");
	c1.pos = c1.avail = 1000;
	c2.comm = mkSComm("STK");
	c2.pos = c2.avail = 1000;
	WtStockVWapExeUnit u1, u2;
	bootS(u1, c1, kStk, 1500, mkSCfg(5, 1, 0, 100.0, 1000.0));
	// 增仓目标：当前实现的 mode1 买价取 bidprice(0)。
	ASSERT_EQ(c1.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(c1.buys[0].price, 10.49);
	bootS(u2, c2, kStk, 1500, mkSCfg(5, 1, 4, 100.0, 1000.0));
	// 非打板走 L635-638：targetPx = 10.49 + 0.01*4 = 10.53
	ASSERT_EQ(c2.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(c2.buys[0].price, 10.53);
	c1.comm->release();
	c2.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 涨停收敛：买入方向的委托价被压到涨停价且订单不可撤（L646-651）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, buy_price_capped_to_upper_limit)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 1000;
	ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg(5, 0, 300, 100.0, 1000.0));
	unit.set_position(kStk, 5000);          // diffQty=4000>0 -> isBuy=true
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51, 10.60, 0));
	unit.on_channel_ready();
	// targetPx = 10.50 + 0.01*300 = 13.50 > 10.60 -> 收敛
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.60);
	EXPECT_TRUE(ctx.hasLog("modified to upper limit price"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 减仓应卖出且不能超过可用仓；价格分支命中下限收敛，产品当前却发买单（BUG-R7-007）。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, sell_price_floored_to_lower_limit)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 1000;
	ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg(5, 0, 300, 100.0, 1000.0));
	unit.set_position(kStk, 100);           // diffQty=-900 -> isBuy=false
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51, 0, 10.20));
	unit.on_channel_ready();
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_GT(ctx.sells[0].qty, 0.0);
	EXPECT_LE(ctx.sells[0].qty, ctx.avail);
	EXPECT_TRUE(ctx.hasLog("modified to lower limit price"));
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 零价回退（L641-642）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, zero_last_price_falls_back_to_preclose)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 1000;
	ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg());
	unit.set_position(kStk, 1500);
	unit.on_tick(mkSTick(kStk, 100000000, 0.0, 0.0, 0.0));
	unit.on_channel_ready();
	// price()==0 -> 用 preclose()=10.0
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_channel_ready 三分支 + 撤单计数与重发（L322-345、L348-358、L361-366、L284-296）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, channel_ready_branches)
{
	SVwapFile vf(25.0);
	// 分支1：外部未完成单 -> 全撤并抬起 _cancel_cnt
	{
		SFakeCtx ctx;
		ctx.comm = mkSComm("STK");
		ctx.undone = 300.0;
		ctx.cancelBydirRetCnt = 2;
		WtStockVWapExeUnit unit;
		unit.init(&ctx, kStk, mkSCfg());
		unit.set_position(kStk, 100);
		unit.on_channel_ready();
		EXPECT_TRUE(ctx.hasLog("unmanaged orders of"));
		EXPECT_TRUE(ctx.hasLog("cancelcnt -> 2"));
		EXPECT_TRUE(ctx.hasLog("尚有未完成的撤单指令"));   // L512（随后的 do_calc）
		EXPECT_TRUE(ctx.buys.empty());
		ctx.comm->release();
	}
	// 分支2：undone=0 且 OMS 有单 -> 清本地错单
	{
		SFakeCtx ctx;
		ctx.comm = mkSComm("STK");
		ctx.pos = 1000;
		ctx.avail = 1000;
		WtStockVWapExeUnit unit;
		bootS(unit, ctx, kStk, 1500, mkSCfg());
		ASSERT_EQ(ctx.buys.size(), 1u);
		unit.on_channel_ready();
		EXPECT_TRUE(ctx.hasLog("not confirmed in trading channel, clear all"));
		ctx.comm->release();
	}
}

//////////////////////////////////////////////////////////////////////////
// on_order：撤单回报收敛后重发；成交回报复位 _cancel_times（L261-302）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, on_order_reports_and_refire)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 1000;
	ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 1500, mkSCfg(5, 0));
	ASSERT_EQ(ctx.buys.size(), 1u);          // 初始正向委托，具体分片量不在本用例断言
	unit.on_order(4242, kStk, true, 0, 10.50, true);       // 未知 id -> L261
	unit.on_order(7000, kStk, true, 0, 10.50, false);      // 全额成交
	EXPECT_TRUE(ctx.hasLog("has filled"));   // L280
	unit.on_order(7000, kStk, true, 5, 10.50, false);      // 已摘除 -> 忽略
	EXPECT_EQ(ctx.countLog("has filled"), 1u);
	unit.on_tick(mkSTick(kStk, 100100000, 10.60, 10.59, 10.61));  // 换 tick
	unit.on_order(7001, kStk, true, 5, 10.60, true);       // 撤单回报（_cancel_cnt==0）
	// 取消确认触发重发；这里验证重发方向与价格，不锁定分片数量。
	EXPECT_TRUE(ctx.hasLog("re_fire will be done"));
	ASSERT_EQ(ctx.buys.size(), 3u);
	EXPECT_GT(ctx.buys[2].qty, 0.0);
	EXPECT_DOUBLE_EQ(ctx.buys[2].price, 10.61);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_trade 只写日志（L455-459）；on_entrust 拒单重算（L463-475）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, on_trade_logs_and_on_entrust_recalculates)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 1000;
	ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 1500, mkSCfg());
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.on_trade(7000, kStk, true, 100, 10.50);
	EXPECT_TRUE(ctx.hasLog("Order 7000 of SHSE.STK.600000 traded:"));   // L458; numeric formatting is not part of the log contract
	unit.on_entrust(9999, kStk, false, "not mine");        // 非本单元 -> L468
	unit.on_entrust(7000, kStk, false, "rejected");        // erase -> do_calc -> 同一 tick 被拦
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.hasLog("No tick of"));                 // L573
	unit.on_entrust(7000, kStk, true, "ok");
	EXPECT_EQ(ctx.buys.size(), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// set_position 的三道防御（L766-778）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, set_position_guards)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg());
	unit.on_channel_ready();                               // diffQty=0 -> L515 return
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit.set_position("other", 100);                       // 代码不符 -> L766
	unit.set_position(kStk, -5);                           // 负目标 -> L774
	EXPECT_TRUE(ctx.hasLog("is an error stock target position"));
	ctx.pos = 500;
	ctx.avail = 500;
	unit.set_position(kStk, 1000);
	unit.set_position(kStk, 1000);                         // 重复目标 -> L770
	EXPECT_EQ(ctx.countLog("第 1 次发单"), 1u);
	unit.on_channel_lost();                                // L811 空实现
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// clear_all_position 的合约过滤（L824-825）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, clear_all_position_filters_code)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 1000;
	ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 1500, mkSCfg());
	uint32_t n = (uint32_t)ctx.buys.size();
	unit.clear_all_position("SHSE.STK.000001");            // 非本合约 -> 直接 return
	EXPECT_EQ(ctx.buys.size(), n);
	EXPECT_TRUE(ctx.sells.empty());
	unit.clear_all_position(kStk);                         // _target_pos=0 -> do_calc
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_GT(ctx.sells[0].qty, 0.0);
	EXPECT_LE(ctx.sells[0].qty, ctx.avail);
	EXPECT_EQ(ctx.buys.size(), n);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// BUG-R7-005 候选回归：父进程隔离子进程并回收其临时目录；崩溃和 alarm 超时均失败。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, channel_ready_before_first_tick_returns_without_orders)
{
	ScopedProbeDir probeDir;
	ASSERT_TRUE(probeDir.ready());
	const ProbeChildResult result = run_channel_ready_probe_in_child(probeDir.path(), false, 1000.0);
	expect_channel_ready_probe_success(result);
	EXPECT_TRUE(probeDir.cleanup()) << "failed to remove private death-test directory";
}

//////////////////////////////////////////////////////////////////////////
// 正控：提供首笔 tick 后再就绪。10 股低于普通股 100 股最小手数，当前实现
// 会在安全读取 tick 后收敛到 finish，不进入存在引用缺陷的发单尾部。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, channel_ready_after_first_tick_returns_without_orders)
{
	ScopedProbeDir probeDir;
	ASSERT_TRUE(probeDir.ready());
	const ProbeChildResult result = run_channel_ready_probe_in_child(probeDir.path(), true, 10.0);
	expect_channel_ready_probe_success(result);
	EXPECT_TRUE(probeDir.cleanup()) << "failed to remove private death-test directory";
}

//////////////////////////////////////////////////////////////////////////
// 缺陷复现（越界/异常，不能启用）：标准股票代码只有 2 段
// init L201 与 get_minOrderQty 都取 split(stdCode,".")[2]
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, DISABLED_two_segment_stock_code_out_of_range)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	WtStockVWapExeUnit unit;
	unit.init(&ctx, "SHSE.600000", mkSCfg());              // 期望：越界/stoi 异常（BUG-R7-009）
	unit.on_channel_ready();
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// bounded child probe only: no fallback/default is asserted, and a normal ARM64
// return does not rule out C++ undefined behavior in the division.
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, init_zero_total_times_bounded_child_probe)
{
	r7_test::ScopedTempDir probeDir("WT_R7_002_STOCK_VWAP");
	ASSERT_TRUE(probeDir.ready());
	const r7_test::ProbeResult result = r7_test::run_bounded_child(probeDir.path(), 0, run_stock_vwap_init_probe);
	ASSERT_TRUE(probeDir.cleanup()) << "failed to remove private init-probe directory";
	if (!r7_test::expect_normal_return(result))
		return;
	GTEST_SKIP() << "The child returned normally on this build; without UBSan or an explicit invalid-config failure contract, this cannot prove safe rejection or absence of undefined behavior.";
}

TEST(r7_stkvw, init_positive_total_times_control_returns)
{
	r7_test::ScopedTempDir probeDir("WT_R7_002_STOCK_VWAP");
	ASSERT_TRUE(probeDir.ready());
	const r7_test::ProbeResult result = r7_test::run_bounded_child(probeDir.path(), 5, run_stock_vwap_init_probe);
	r7_test::expect_normal_return(result);
	EXPECT_TRUE(probeDir.cleanup()) << "failed to remove private init-probe directory";
}
