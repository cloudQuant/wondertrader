// R7 lane：WtStockVWapExeUnit（股票/科创板 VWAP 分片执行单元）
// 目标文件: src/WtExeFact/WtStockVWapExeUnit.cpp / WtStockVWapExeUnit.h
// 覆盖面: init（产品分类及数量场景、可选键 "offset"、动态 session/profile、
//         交易时间窗、无效配置失败）、
//         do_calc 的持仓不足目标修正、碎股完成判定、分桶数量与订单安全、价格模式与涨跌停、
//         on_tick/on_order/on_channel_ready/on_entrust/on_trade/clear_all_position/set_position 各分支。
//
// 回归点：方向从目标差值确定；VWAP 分桶切片保留符号并受剩余目标/可卖仓约束；
// ExecuteContext 的 validOnly=true 是可用仓、false 是总仓。
//
// 时间窗先映射到 session 的累计交易秒，再计算 duration 和 _fire_span；午休不计入间隔。
// 驱动顺序：init -> set_position -> on_tick -> on_channel_ready；无首 tick 的就绪回调应安全等待。
#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "Includes/ExecuteDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "WtExeFact/WtOrdMon.h"
#define private public
#include "WtExeFact/WtStockVWapExeUnit.h"
#undef private
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
	struct PositionQuery { bool validOnly; int32_t flag; };
	SFakeCtx()
	{
		sess = WTSSessionInfo::create("NIGHT_DAY", "NIGHT_DAY");
		sess->addTradingSection(930, 1130);
		sess->addTradingSection(1300, 1500);
	}
	~SFakeCtx() override
	{
		for (const auto& order : grossOrders)
			order.second->release();
		if (sess != nullptr)
			sess->release();
	}
	void setSession(WTSSessionInfo* value)
	{
		if (sess != nullptr)
			sess->release();
		sess = value;
	}

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return NULL; }
	WTSTickData* grabLastTick(const char*) override { return NULL; }
	// ExecuteContext: validOnly=true returns available holdings; false returns
	// total holdings. This stock fixture models long holdings only, as flag 1 does.
	double getPosition(const char*, bool validOnly = true, int32_t flag = 3) override
	{
		positionQueries.push_back({ validOnly, flag });
		if ((flag & 1) == 0)
			return 0;
		return validOnly ? avail : pos;
	}
	OrderMap* getOrders(const char* code) override
	{
		if (returnNullOrders)
			return NULL;
		OrderMap* snapshot = OrderMap::create();
		for (const auto& order : grossOrders)
		{
			if (code == nullptr || strcmp(code, order.second->getCode()) == 0)
				snapshot->add(order.first, order.second);
		}
		return snapshot;
	}
	double getUndoneQty(const char*) override { return undone; }
	OrderIDs buy(const char* c, double p, double q, bool) override
	{
		if (returnEmptyOrderIds)
		{
			emptyOrderIdAttempts++;
			return {};
		}
		buys.push_back({ c, p, q });
		OrderIDs ids{ nextId++ };
		return ids;
	}
	OrderIDs sell(const char* c, double p, double q, bool) override
	{
		if (returnEmptyOrderIds)
		{
			emptyOrderIdAttempts++;
			return {};
		}
		sells.push_back({ c, p, q });
		OrderIDs ids{ nextId++ };
		return ids;
	}
	bool cancel(uint32_t id) override
	{
		byidCancels.push_back(id);
		for (auto it = grossOrders.begin(); it != grossOrders.end(); ++it)
		{
			if (it->first == id)
			{
				it->second->release();
				grossOrders.erase(it);
				break;
			}
		}
		return true;
	}
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
	uint64_t getCurTime() override { return curTime; }

	double pos = 0;
	double avail = 0;
	double undone = 0;
	uint64_t curTime = 20240103093000000ull;
	uint32_t nextId = 7000;
	uint32_t cancelBydirRetCnt = 2;
	bool returnEmptyOrderIds{ false };
	uint32_t emptyOrderIdAttempts{ 0 };
	bool returnNullOrders{ false };
	WTSCommodityInfo* comm = NULL;
	WTSSessionInfo* sess = NULL;
	std::vector<Rec> buys;
	std::vector<Rec> sells;
	std::vector<PositionQuery> positionQueries;
	std::vector<uint32_t> byidCancels;
	std::vector<std::pair<std::string, bool>> bydirCancels;
	std::vector<std::pair<uint32_t, WTSOrderInfo*>> grossOrders;
	std::vector<std::string> logs;

	void addGrossOrder(uint32_t localid, const char* code, bool isBuy, double qty)
	{
		WTSOrderInfo* order = WTSOrderInfo::create();
		order->setCode(code);
		order->setDirection(isBuy ? WDT_LONG : WDT_SHORT);
		order->setOffsetType(WOT_OPEN);
		order->m_bIsNet = false;
		order->m_bIsBuy = isBuy;
		order->setVolume(qty);
		order->setVolLeft(qty);
		grossOrders.emplace_back(localid, order);
	}

	bool hasLog(const std::string& sub) const { return countLog(sub) > 0; }
	uint32_t countLog(const std::string& sub) const
	{
		uint32_t n = 0;
		for (const std::string& l : logs)
			if (l.find(sub) != std::string::npos)
				n++;
		return n;
	}
	bool queriedPosition(bool validOnly, int32_t flag) const
	{
		for (const PositionQuery& q : positionQueries)
			if (q.validOnly == validOnly && q.flag == flag)
				return true;
		return false;
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

class SVwapV2File
{
public:
	explicit SVwapV2File(WTSSessionInfo& session, size_t activeBucket, double activeValue)
	{
		_path = std::string("Vwap_") + sCommName() + ".txt";
		std::ofstream file(_path.c_str());
		file << "{\"version\":2,\"profiles\":[{\"session_id\":\"" << session.id()
			<< "\",\"offset_minutes\":" << session.getOffsetMins() << ",\"sections\":[";
		const auto& sections = session.getTradingSections();
		for (size_t i = 0; i < sections.size(); ++i)
		{
			if (i != 0)
				file << ',';
			file << "{\"from\":" << sections[i].first_raw
				<< ",\"to\":" << sections[i].second_raw << '}';
		}
		const size_t count = static_cast<size_t>(session.getTradingMins()) + 1;
		file << "],\"bucket_unit\":\"trading_minute\",\"value_mode\":\"cumulative\","
			<< "\"close_inclusive\":true,\"values\":[";
		for (size_t i = 0; i < count; ++i)
		{
			if (i != 0)
				file << ',';
			file << (i == activeBucket ? activeValue : 0.0);
		}
		file << "]}]}";
	}
	~SVwapV2File() { std::remove(_path.c_str()); }

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

// WTSCommodityInfo::create 的第 1 个参数是 product，第 5 个参数是交易模板。
WTSCommodityInfo* mkSComm(const char* product)
{
	WTSCommodityInfo* c = WTSCommodityInfo::create(product, sCommName().c_str(), "SHSE", "NIGHT_DAY", "NIGHT_DAY");
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

void releaseAllSCommReferences(WTSCommodityInfo* comm)
{
	if (comm == nullptr)
		return;
	const uint32_t refs = comm->retainCount();
	for (uint32_t i = 0; i < refs; ++i)
		comm->release();
}

WTSTickData* mkSTick(const char* code, uint32_t actTime, double lastPx,
	double bidPx, double askPx, double upper = 0, double lower = 0,
	uint32_t actionDate = 20240103, uint32_t tradingDate = 20240103)
{
	WTSTickStruct ts;
	snprintf(ts.code, sizeof(ts.code), "%s", code);
	ts.action_date = actionDate;
	ts.trading_date = tradingDate;
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
	uint32_t tailSecs = 0, bool includeMinOpenLots = true,
	uint32_t beginTime = 930, uint32_t endTime = 1500)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("begin_time", beginTime);
	cfg->append("end_time", endTime);
	cfg->append("ord_sticky", ordSticky);
	cfg->append("tail_secs", tailSecs);
	cfg->append("total_times", totalTimes);
	cfg->append("price_mode", priceMode);
	cfg->append("offset", offset);
	cfg->append("lots", lots);
	if (includeMinOpenLots)
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
			// Keep the creation reference through the single calculation to isolate the
			// separate member-reference defect (BUG-R7-015) from this first-tick probe.
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
		if (provideTick)
		{
			if (ctx.buys.size() != 1 || !ctx.sells.empty() ||
				ctx.buys[0].qty <= 0 || ctx.buys[0].qty > target || ctx.buys[0].price <= 0)
				result = 2;
		}
		else if (!ctx.buys.empty() || !ctx.sells.empty())
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
	EXPECT_DOUBLE_EQ(ctx.getPosition(kStk, true, 2), 0.0); // no short holdings in this fixture
}

TEST(r7_stkvw, net_zero_buy_sell_snapshot_fails_closed_without_bare_id_cancels)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.addGrossOrder(8100, kStk, true, 100.0);
	ctx.addGrossOrder(8101, kStk, false, 100.0);
	ctx.undone = 0.0; // +100 buy and -100 sell net to zero
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 0.0, mkSCfg());

	EXPECT_TRUE(ctx.byidCancels.empty());
	EXPECT_TRUE(ctx.bydirCancels.empty());
	EXPECT_FALSE(unit._orders_mon.has_order(8100));
	EXPECT_FALSE(unit._orders_mon.has_order(8101));
	EXPECT_EQ(unit.executionState(), WtStockVWapExeUnit::ExecutionState::active);
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_TRUE(ctx.hasLog("gross same-code orders exist while net undone"));
	EXPECT_TRUE(ctx.hasLog("found 2 gross same-code order(s) without verifiable owner"));
	EXPECT_TRUE(ctx.hasLog("no localid cancellation or ownership registration, execution held"));

	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.byidCancels.empty());
	EXPECT_TRUE(ctx.bydirCancels.empty());
	EXPECT_FALSE(unit._orders_mon.has_order(8100));
	EXPECT_FALSE(unit._orders_mon.has_order(8101));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

TEST(r7_stkvw, timeout_without_verified_identity_keeps_monitor_and_never_cancels)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	{
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = mkSCfg(5, 0, 0, 100.0, 1000.0, 1);
		unit.init(&ctx, kStk, cfg);
		cfg->release();
		unit.set_position(kStk, 0);

		const uint32_t id = 8151;
		unit._orders_mon.push_order(&id, 1, ctx.curTime);
		unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
		ctx.curTime += 999;
		unit.on_tick(mkSTick(kStk, 100001000, 10.50, 10.49, 10.51));
		EXPECT_EQ(ctx.countLog("Expired order 8151"), 0u);
		ctx.curTime += 1;
		unit.on_tick(mkSTick(kStk, 100002000, 10.50, 10.49, 10.51));
		EXPECT_TRUE(ctx.byidCancels.empty());
		EXPECT_TRUE(ctx.bydirCancels.empty());
		EXPECT_TRUE(unit._orders_mon.has_order(id));
		EXPECT_EQ(ctx.countLog("Expired order 8151"), 1u);

		unit.on_channel_ready(); // clears unconfirmed monitor rows, not the quarantine latch
		EXPECT_TRUE(unit._orders_mon.has_unverified_timeout());
		unit.set_position(kStk, 100.0);
		unit.fire_at_once(100.0);
		unit.on_tick(mkSTick(kStk, 100003000, 10.50, 10.49, 10.51));
		EXPECT_TRUE(ctx.byidCancels.empty());
		EXPECT_TRUE(ctx.bydirCancels.empty());
		EXPECT_FALSE(unit._orders_mon.has_order(id));
		EXPECT_TRUE(unit._orders_mon.has_unverified_timeout());
		EXPECT_TRUE(ctx.buys.empty());
		EXPECT_TRUE(ctx.sells.empty());
		EXPECT_EQ(ctx.countLog("Expired order 8151"), 1u);
	}
	ctx.comm->release();
}

TEST(r7_stkvw, missing_gross_order_snapshot_fails_closed_but_recovers_on_later_snapshot)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.returnNullOrders = true;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 100.0, mkSCfg());
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_TRUE(ctx.hasLog("gross same-code order state is unavailable, failing closed"));
	EXPECT_EQ(unit.executionState(), WtStockVWapExeUnit::ExecutionState::active);

	ctx.returnNullOrders = false;
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// minopenlots 是可选配置；缺省时使用品种最小手数，不读取未初始化值。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, minopenlots_optional_defaults_to_minimum_hand)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 1000, mkSCfg(5, 0, 0, 100.0, 1000.0, 0, 0, false));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);
	EXPECT_GT(ctx.buys[0].price, 0.0);
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 明确提供 0 的 minopenlots 非法：安全禁用执行单，绝不进入除零或下单路径。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, zero_minopenlots_disables_orders)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	WtStockVWapExeUnit unit;
	WTSVariant* cfg = mkSCfg(5, 0, 0, 0.0);
	unit.init(&ctx, kStk, cfg);
	cfg->release();
	unit.set_position(kStk, 1000);
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit.on_channel_ready();
	EXPECT_FALSE(unit.initializationSucceeded());
	EXPECT_TRUE(ctx.hasLog("minopenlots must be a finite positive number"));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 普通股增仓：方向为买，数量为正且不超过剩余目标。
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
	// 增仓目标 1000->1500。
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_GT(ctx.buys[0].qty, 0.0);
	EXPECT_LE(ctx.buys[0].qty, 500.0);
	EXPECT_GT(ctx.buys[0].price, 0.0);
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
	EXPECT_LE(c1.buys[0].qty, 3000.0);
	EXPECT_LE(c2.buys[0].qty, 3000.0);
	EXPECT_GT(c1.buys[0].price, 0.0);
	EXPECT_GT(c2.buys[0].price, 0.0);
	c1.comm->release();
	c2.comm->release();
}

TEST(r7_stkvw, vwap_bucket_distribution_changes_planned_sell_qty)
{
	SFakeCtx c1, c2;
	c1.comm = mkSComm("STK");
	c2.comm = mkSComm("STK");
	c1.pos = c2.pos = 1000;
	c1.avail = c2.avail = 1000;
	WtStockVWapExeUnit u1, u2;
	{
		SVwapFile vfA(1300.0);
		bootS(u1, c1, kStk, 500, mkSCfg());       // diffQty=-500; same current position
	}
	{
		SVwapFile vfB(1400.0);
		bootS(u2, c2, kStk, 500, mkSCfg());       // same target/position, different cumulative profile
	}
	ASSERT_EQ(c1.sells.size(), 1u);
	ASSERT_EQ(c2.sells.size(), 1u);
	EXPECT_GT(c1.sells[0].qty, 0.0);
	EXPECT_GT(c2.sells[0].qty, c1.sells[0].qty);
	EXPECT_LE(c1.sells[0].qty, 500.0);
	EXPECT_LE(c2.sells[0].qty, 500.0);
	EXPECT_LE(c1.sells[0].qty, c1.avail);
	EXPECT_LE(c2.sells[0].qty, c2.avail);
	EXPECT_TRUE(c1.buys.empty());
	EXPECT_TRUE(c2.buys.empty());
	c1.comm->release();
	c2.comm->release();
}

TEST(r7_stkvw, target_equal_to_position_has_zero_delta_and_sends_no_order)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 1000, mkSCfg());
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_EQ(unit.executionState(), WtStockVWapExeUnit::ExecutionState::complete);
	ctx.comm->release();
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
	EXPECT_LE(ctx.buys[0].qty, 1000.0);
	EXPECT_GT(ctx.buys[0].price, 0.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 可转债增仓方向；具体最小手数/切片量仍待业务合同确认。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, convertible_bond_increase_emits_positive_buy)
{
	SVwapFile vf(5100.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("CBOND");
	ctx.pos = 5000;
	ctx.avail = 5000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kBond, 5100, mkSCfg(1, 0, 0, 50.0, 1000.0, 0, 14400));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);
	EXPECT_LE(ctx.buys[0].qty, 100.0);
	ctx.comm->release();
}

TEST(r7_stkvw, convertible_bond_final_sell_keeps_existing_open_lot_cap)
{
	SVwapFile vf(5100.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("CBOND");
	ctx.pos = ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kBond, 900, mkSCfg(1, 0, 0, 50.0, 1000.0, 0, 14400));
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 10.0);
	ctx.pos = ctx.avail = 990; // first 10-unit slice filled
	unit.on_order(7000, kBond, false, 0, ctx.sells[0].price, false);
	unit.on_tick(mkSTick(kBond, 100100000, 10.50, 10.49, 10.51)); // final configured attempt
	ASSERT_EQ(ctx.sells.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.sells[1].qty, 10.0);
	EXPECT_LE(ctx.sells[1].qty, 10.0); // tail must not expand to the 90-unit target gap
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

TEST(r7_stkvw, convertible_bond_clear_can_sell_available_tail_below_ten)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("CBOND");
	ctx.pos = ctx.avail = 5;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kBond, 5, mkSCfg(5, 0, 0, 50.0, 1000.0, 0, 14400));
	EXPECT_TRUE(ctx.sells.empty());
	unit.clear_all_position(kBond);
	unit.on_tick(mkSTick(kBond, 100100000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 5.0); // CBOND tail behavior stays distinct from stock lots
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 空仓建仓目标应买入；不能发零量订单。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, flat_position_buy_target_requires_a_buy)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 0;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 1000, mkSCfg(5, 0, 0, 100.0, 1000.0));
	// 目标从空仓增至 1000，应产生正向且不超目标的买入。
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_GT(ctx.buys[0].qty, 0.0);
	EXPECT_LE(ctx.buys[0].qty, 1000.0);
	EXPECT_GT(ctx.buys[0].price, 0.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 普通股碎股清仓在最小手数合同外保持未完成，不自行假设交易所接受零股卖单。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, stock_odd_lot_clear_remains_pending_without_assuming_odd_lot_legality)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 40;
	ctx.avail = 40;
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg(5, 0, 0, 100.0, 1000.0, 0, 14400));
	unit.set_position(kStk, 40);
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit.on_channel_ready();
	EXPECT_EQ(unit.executionState(), WtStockVWapExeUnit::ExecutionState::complete);
	unit.clear_all_position(kStk);
	EXPECT_EQ(unit.executionState(), WtStockVWapExeUnit::ExecutionState::active);
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty()); // clear callback cannot reuse the quote already processed
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_EQ(unit.executionState(), WtStockVWapExeUnit::ExecutionState::pending_remainder);
	EXPECT_DOUBLE_EQ(unit.pendingRemainder(), -40.0);
	EXPECT_TRUE(ctx.hasLog("sell remainder is below one stock lot"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 买入目标余量不足一手时保持 pending；新的整手目标必须等新行情才能发合法数量。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, stock_buy_sublot_remainder_stays_pending_until_executable)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 950;
	ctx.avail = 950;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 1000, mkSCfg(5, 0, 0, 100.0, 1000.0, 0, 14400));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_EQ(unit.executionState(), WtStockVWapExeUnit::ExecutionState::pending_remainder);
	EXPECT_DOUBLE_EQ(unit.pendingRemainder(), 50.0);
	EXPECT_TRUE(ctx.hasLog("buy remainder is below one stock lot"));
	EXPECT_FALSE(ctx.hasLog("execution complete"));

	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.buys.empty()); // duplicate quote cannot repeat a calculation
	unit.set_position(kStk, 1050); // 100-share remainder is executable, but only on a new quote
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_EQ(unit.executionState(), WtStockVWapExeUnit::ExecutionState::active);
	EXPECT_DOUBLE_EQ(unit.pendingRemainder(), 0.0);
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);
	EXPECT_EQ(unit.executionState(), WtStockVWapExeUnit::ExecutionState::active);
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	EXPECT_EQ(ctx.buys.size(), 1u);
	ctx.comm->release();
}

TEST(r7_stkvw, stock_sell_sublot_remainder_stays_pending)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 950;
	ctx.avail = 950;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 900, mkSCfg(5, 0, 0, 100.0, 1000.0, 0, 14400));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_EQ(unit.executionState(), WtStockVWapExeUnit::ExecutionState::pending_remainder);
	EXPECT_DOUBLE_EQ(unit.pendingRemainder(), -50.0);
	EXPECT_TRUE(ctx.hasLog("sell remainder is below one stock lot"));
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_EQ(unit.executionState(), WtStockVWapExeUnit::ExecutionState::pending_remainder);
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
	c->setTradingMode(TM_Both);          // 非 T+0；avail=200、total=1000
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
	EXPECT_LE(ctx.sells[0].qty, 200.0);
	EXPECT_GT(ctx.sells[0].price, 0.0);
	EXPECT_TRUE(ctx.queriedPosition(false, 3)); // total position
	EXPECT_TRUE(ctx.queriedPosition(true, 1));  // available long position
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// T+1 可卖仓不足一手时保留原始目标为 pending；可卖量恢复后只发合法整手。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, t1_small_sellable_floor_does_not_finish_original_target)
{
	SVwapFile vf(1200.0);
	SFakeCtx ctx;
	WTSCommodityInfo* c = mkSComm("STK");
	c->setTradingMode(TM_Both);
	ctx.comm = c;
	ctx.pos = 1000;
	ctx.avail = 20;
	WtStockVWapExeUnit unit;
	// Zero fire span lets the test exercise new ticks without waiting for the default
	// cadence; each attempt still requires a fresh quote and a legal 100-share lot.
	bootS(unit, ctx, kStk, 0, mkSCfg(5, 0, 0, 100.0, 1000.0, 0, 14400));
	const size_t firstTickSellCount = ctx.sells.size();
	EXPECT_EQ(firstTickSellCount, 0u);
	EXPECT_EQ(unit.executionState(), WtStockVWapExeUnit::ExecutionState::pending_remainder);
	EXPECT_DOUBLE_EQ(unit.pendingRemainder(), -1000.0);
	EXPECT_TRUE(ctx.hasLog("T+1 sellable quantity is below one stock lot"));
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.sells.empty());

	// 可卖仓恢复到整手后，原清仓目标继续执行，且不会越过可卖量。
	ctx.avail = 100;
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.sells.size(), firstTickSellCount + 1u);
	EXPECT_DOUBLE_EQ(ctx.sells.back().qty, 100.0);
	EXPECT_EQ(unit.executionState(), WtStockVWapExeUnit::ExecutionState::active);
	EXPECT_DOUBLE_EQ(unit.pendingRemainder(), 0.0);
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	EXPECT_EQ(ctx.sells.size(), firstTickSellCount + 1u);
	EXPECT_GT(ctx.sells.back().price, 0.0);
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 同一 tick 可卖仓为零时不下单；下一个更新 tick 恢复到整手可卖量后继续清仓。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, t1_zero_sellable_same_tick_no_order_next_tick_advances)
{
	SVwapFile vf(1200.0);
	SFakeCtx ctx;
	WTSCommodityInfo* c = mkSComm("STK");
	c->setTradingMode(TM_Both);
	ctx.comm = c;
	ctx.pos = 1000;
	ctx.avail = 0;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 0, mkSCfg());
	EXPECT_TRUE(ctx.sells.empty());

	ctx.avail = 100;
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_GT(ctx.sells[0].qty, 0.0);
	EXPECT_LE(ctx.sells[0].qty, 100.0);
	EXPECT_GT(ctx.sells[0].price, 0.0);
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 14:59 在日盘模板映射到累计交易分钟 239，241 项 profile 可安全访问。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, vwap_maps_afternoon_using_session_trading_minutes)
{
	SFakeCtx ctx;
	SVwapV2File vf(*ctx.sess, 239, 1000.0);
	ctx.comm = mkSComm("STK");
	ctx.pos = 0;
	ctx.avail = 0;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 1000, mkSCfg(), 145900000);
	EXPECT_FALSE(ctx.hasLog("outside 241 buckets"));
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 1000.0);
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 无效 HHMMSSmmm 字段不得污染高水位；后续合法 tick 仍可执行。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, invalid_action_time_does_not_poison_valid_tick)
{
	SVwapFile vf(25.0, 241);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 0;
	ctx.avail = 0;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 1000, mkSCfg(), 250000000); // 25:00:00.000
	EXPECT_TRUE(ctx.hasLog("invalid action time"));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_GT(ctx.buys[0].qty, 0.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// V2 profile follows the configured offset night session and its cumulative
// trading-minute index instead of the stock day-session clock formula.
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, v2_profile_selects_night_session_and_maps_tick)
{
	SFakeCtx ctx;
	WTSSessionInfo* night = WTSSessionInfo::create("NIGHT_DAY", "NIGHT_DAY", 360);
	night->addTradingSection(2100, 2300);
	night->addTradingSection(0, 230);
	ctx.setSession(night);
	SVwapV2File vf(*night, 60, 1000.0);
	ctx.comm = mkSComm("STK");
	ctx.pos = ctx.avail = 0;
	WtStockVWapExeUnit unit;
	WTSVariant* cfg = mkSCfg(5, 0, 0, 100.0, 1000.0, 0, 0, true, 2100, 2300);
	unit.init(&ctx, kStk, cfg);
	cfg->release();
	EXPECT_TRUE(unit.initializationSucceeded());
	unit.set_position(kStk, 1000);
	unit.on_tick(mkSTick(kStk, 220000000, 10.50, 10.49, 10.51,
		0, 0, 20240103, 20240104)); // natural action date rolls into the trading date
	unit.on_channel_ready();
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 1000.0);
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

TEST(r7_stkvw, natural_action_date_increment_keeps_midnight_retry_fresh)
{
	SFakeCtx ctx;
	WTSSessionInfo* night = WTSSessionInfo::create("NIGHT_DAY", "NIGHT_DAY", 360);
	night->addTradingSection(2100, 2300);
	night->addTradingSection(0, 230);
	ctx.setSession(night);
	SVwapV2File vf(*night, 60, 1000.0);
	ctx.comm = mkSComm("STK");
	WtStockVWapExeUnit unit;
	WTSVariant* cfg = mkSCfg(5, 0, 0, 100.0, 1000.0, 0, 0, true, 2100, 230);
	unit.init(&ctx, kStk, cfg);
	cfg->release();
	ASSERT_TRUE(unit.initializationSucceeded());
	unit.set_position(kStk, 1000);
	unit.on_tick(mkSTick(kStk, 220000000, 10.50, 10.49, 10.51,
		0, 0, 20240105, 20240108)); // Friday natural action date, Monday trading date
	unit.on_channel_ready();
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.on_order(7000, kStk, true, 500, 10.50, true);
	EXPECT_EQ(ctx.buys.size(), 1u);
	unit.on_tick(mkSTick(kStk, 100000, 10.50, 10.49, 10.51,
		0, 0, 20240106, 20240108)); // Saturday natural date, Monday trading date
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_GT(ctx.buys[1].qty, 0.0);
	EXPECT_LE(ctx.buys[1].qty, 1000.0);
	EXPECT_FALSE(ctx.hasLog("stale tick"));
	ctx.comm->release();
}

TEST(r7_stkvw, unchanged_action_date_across_midnight_is_rejected_as_unsupported)
{
	SFakeCtx ctx;
	WTSSessionInfo* night = WTSSessionInfo::create("NIGHT_DAY", "NIGHT_DAY", 360);
	night->addTradingSection(2100, 2300);
	night->addTradingSection(0, 230);
	ctx.setSession(night);
	SVwapV2File vf(*night, 60, 1000.0);
	ctx.comm = mkSComm("STK");
	WtStockVWapExeUnit unit;
	WTSVariant* cfg = mkSCfg(5, 0, 0, 100.0, 1000.0, 0, 0, true, 2100, 230);
	unit.init(&ctx, kStk, cfg);
	cfg->release();
	ASSERT_TRUE(unit.initializationSucceeded());
	unit.set_position(kStk, 1000);
	unit.on_tick(mkSTick(kStk, 225900000, 10.50, 10.49, 10.51,
		0, 0, 20240105, 20240108));
	unit.on_channel_ready();
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.on_order(7000, kStk, true, 500, 10.50, true);
	unit.on_tick(mkSTick(kStk, 100000, 10.50, 10.49, 10.51,
		0, 0, 20240105, 20240108)); // provider failed to advance natural action date
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.hasLog("unsupported date rollover"));
	ctx.comm->release();
}

TEST(r7_stkvw, trading_day_action_date_combo_from_unsupported_provider_is_rejected)
{
	SFakeCtx ctx;
	WTSSessionInfo* night = WTSSessionInfo::create("NIGHT_DAY", "NIGHT_DAY", 360);
	night->addTradingSection(2100, 2300);
	night->addTradingSection(0, 230);
	ctx.setSession(night);
	SVwapV2File vf(*night, 60, 1000.0);
	ctx.comm = mkSComm("STK");
	WtStockVWapExeUnit unit;
	WTSVariant* cfg = mkSCfg(5, 0, 0, 100.0, 1000.0, 0, 0, true, 2100, 230);
	unit.init(&ctx, kStk, cfg);
	cfg->release();
	ASSERT_TRUE(unit.initializationSucceeded());
	unit.set_position(kStk, 1000);
	unit.on_tick(mkSTick(kStk, 220000000, 10.50, 10.49, 10.51,
		0, 0, 20240104, 20240104)); // TradingDay used as ActionDay incorrectly implies Jan 5
	unit.on_channel_ready();
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.hasLog("provider date semantics are not supported"));
	EXPECT_TRUE(ctx.hasLog("precedes it"));
	unit.on_tick(mkSTick(kStk, 220100000, 10.50, 10.49, 10.51,
		0, 0, 20240103, 20240104));
	EXPECT_TRUE(ctx.buys.empty()); // unsupported combination latches fail-closed until reinit
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// A time gap or post-close tick invalidates the old quote so a later event
// cannot use it to submit an order outside the session.
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, lunch_and_post_close_ticks_do_not_submit_orders)
{
	SVwapFile vf(1000.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	WtStockVWapExeUnit unit;
	WTSVariant* cfg = mkSCfg();
	unit.init(&ctx, kStk, cfg);
	cfg->release();
	ASSERT_TRUE(unit.initializationSucceeded());
	unit.set_position(kStk, 1000);
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit.on_channel_ready();
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.on_tick(mkSTick(kStk, 120000000, 10.50, 10.49, 10.51)); // lunch gap
	unit.on_entrust(7000, kStk, false, "rejected");
	EXPECT_EQ(ctx.buys.size(), 1u);
	unit.on_tick(mkSTick(kStk, 150100000, 10.50, 10.49, 10.51)); // after close
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51)); // delayed older in-session tick
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.hasLog("out-of-session action time"));
	EXPECT_TRUE(ctx.hasLog("stale tick"));
	ctx.comm->release();
}

TEST(r7_stkvw, execution_window_uses_configured_session_bounds)
{
	SVwapFile vf(1000.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	WtStockVWapExeUnit unit;
	WTSVariant* cfg = mkSCfg(5, 0, 0, 100.0, 1000.0, 0, 0, true, 1000, 1100);
	unit.init(&ctx, kStk, cfg);
	cfg->release();
	ASSERT_TRUE(unit.initializationSucceeded());
	unit.set_position(kStk, 1000);
	unit.on_tick(mkSTick(kStk, 95900000, 10.50, 10.49, 10.51)); // before begin
	unit.on_channel_ready();
	EXPECT_TRUE(ctx.buys.empty());
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 1000.0);
	ctx.comm->release();
}

TEST(r7_stkvw, mismatched_v2_profile_session_fails_initialization)
{
	SFakeCtx ctx;
	WTSSessionInfo* profile_session = WTSSessionInfo::create("OTHER_DAY", "OTHER_DAY");
	profile_session->addTradingSection(930, 1130);
	profile_session->addTradingSection(1300, 1500);
	SVwapV2File vf(*profile_session, 0, 1000.0);
	profile_session->release();
	ctx.comm = mkSComm("STK");
	WtStockVWapExeUnit unit;
	WTSVariant* cfg = mkSCfg();
	unit.init(&ctx, kStk, cfg);
	cfg->release();
	EXPECT_FALSE(unit.initializationSucceeded());
	unit.set_position(kStk, 1000);
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit.on_channel_ready();
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.hasLog("no profile matching this session"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// Invalid configuration and missing context resources remain unusable.
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, initialization_rejects_zero_times_missing_resources_and_bad_code)
{
	SVwapFile vf(25.0);
	{
		SFakeCtx ctx;
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = mkSCfg();
		unit.init(&ctx, kStk, cfg);
		cfg->release();
		EXPECT_FALSE(unit.initializationSucceeded());
		unit.on_channel_ready();
		EXPECT_TRUE(ctx.buys.empty());
	}
	{
		SFakeCtx ctx;
		ctx.comm = mkSComm("STK");
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = mkSCfg(0);
		unit.init(&ctx, kStk, cfg);
		cfg->release();
		EXPECT_FALSE(unit.initializationSucceeded());
		unit.on_channel_ready();
		EXPECT_TRUE(ctx.buys.empty());
		ctx.comm->release();
	}
	{
		SFakeCtx ctx;
		ctx.setSession(nullptr);
		ctx.comm = mkSComm("STK");
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = mkSCfg();
		unit.init(&ctx, kStk, cfg);
		cfg->release();
		EXPECT_FALSE(unit.initializationSucceeded());
		unit.on_channel_ready();
		EXPECT_TRUE(ctx.buys.empty());
		ctx.comm->release();
	}
	{
		SFakeCtx ctx;
		ctx.comm = mkSComm("STK");
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = mkSCfg();
		unit.init(&ctx, "SHSE.600000", cfg);
		cfg->release();
		EXPECT_FALSE(unit.initializationSucceeded());
		unit.on_channel_ready();
		EXPECT_TRUE(ctx.buys.empty());
		ctx.comm->release();
	}
}

TEST(r7_stkvw, missing_commodity_or_session_fails_closed_without_orders)
{
	SVwapFile vf(25.0);
	{
		SFakeCtx ctx; // The fixture supplies a valid session; commodity metadata is absent.
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = mkSCfg();
		unit.init(&ctx, kStk, cfg);
		cfg->release();
		EXPECT_FALSE(unit.initializationSucceeded());
		EXPECT_TRUE(ctx.hasLog("commodity information is unavailable"));
		unit.set_position(kStk, 1000);
		WTSTickData* tick = mkSTick(kStk, 100000000, 10.50, 10.49, 10.51);
		unit.on_tick(tick);
		tick->release();
		unit.on_channel_ready();
		EXPECT_TRUE(ctx.buys.empty());
		EXPECT_TRUE(ctx.sells.empty());
		EXPECT_TRUE(ctx.byidCancels.empty());
		EXPECT_TRUE(ctx.bydirCancels.empty());
	}
	{
		SFakeCtx ctx;
		ctx.comm = mkSComm("STK");
		ctx.setSession(nullptr);
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = mkSCfg();
		unit.init(&ctx, kStk, cfg);
		cfg->release();
		EXPECT_FALSE(unit.initializationSucceeded());
		EXPECT_TRUE(ctx.hasLog("trading session is unavailable"));
		unit.set_position(kStk, 1000);
		WTSTickData* tick = mkSTick(kStk, 100000000, 10.50, 10.49, 10.51);
		unit.on_tick(tick);
		tick->release();
		unit.on_channel_ready();
		EXPECT_TRUE(ctx.buys.empty());
		EXPECT_TRUE(ctx.sells.empty());
		EXPECT_TRUE(ctx.byidCancels.empty());
		EXPECT_TRUE(ctx.bydirCancels.empty());
		ctx.comm->release();
	}
}

TEST(r7_stkvw, minimum_order_lookup_keeps_borrowed_commodity_alive_across_units)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	WTSCommodityInfo* comm = ctx.comm;
	ASSERT_NE(comm, nullptr);
	for (int i = 0; i < 5; ++i)
		comm->retain(); // Keep a buggy helper from destroying the borrowed object mid-test.
	const uint32_t fixtureRefs = comm->retainCount();
	EXPECT_EQ(fixtureRefs, 6u);

	for (int i = 0; i < 2; ++i)
	{
		{
			WtStockVWapExeUnit unit;
			WTSVariant* cfg = mkSCfg();
			unit.init(&ctx, kStk, cfg);
			cfg->release();
			EXPECT_TRUE(unit.initializationSucceeded());
			EXPECT_EQ(comm->retainCount(), fixtureRefs + 1); // unit owns one retained reference
			EXPECT_DOUBLE_EQ(unit.get_minOrderQty(kStk), 100.0);
			EXPECT_DOUBLE_EQ(unit.get_minOrderQty(kStk), 100.0);
			EXPECT_EQ(comm->retainCount(), fixtureRefs + 1); // helper only borrows; unit still alive
		}
		EXPECT_EQ(comm->retainCount(), fixtureRefs); // destruction drops only the unit reference
		EXPECT_STREQ(comm->getProduct(), "STK");
	}

	ctx.comm = nullptr;
	releaseAllSCommReferences(comm);
}

TEST(r7_stkvw, minimum_order_lookup_fails_closed_without_context_or_commodity)
{
	WtStockVWapExeUnit unboundUnit;
	EXPECT_DOUBLE_EQ(unboundUnit.get_minOrderQty(kStk), 0.0);

	SVwapFile vf(25.0);
	SFakeCtx ctx; // Public initialization must reject unavailable commodity metadata.
	WtStockVWapExeUnit unit;
	WTSVariant* cfg = mkSCfg();
	unit.init(&ctx, kStk, cfg);
	cfg->release();
	EXPECT_FALSE(unit.initializationSucceeded());
	EXPECT_TRUE(ctx.hasLog("commodity information is unavailable"));
	EXPECT_DOUBLE_EQ(unit.get_minOrderQty(kStk), 0.0);
	EXPECT_TRUE(ctx.hasLog("minimum order quantity"));
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

TEST(r7_stkvw, price_mode_matrix_matches_initial_and_canceled_retry_prices)
{
	SVwapFile vf(25.0);
	const double buyInitial[] = { 10.50, 10.49, 10.51 };
	const double sellInitial[] = { 10.50, 10.51, 10.49 };
	const double buyRetry[] = { 10.61, 10.60, 10.62 };
	const double sellRetry[] = { 10.59, 10.60, 10.58 };
	for (uint32_t mode = 0; mode <= 2; ++mode)
	{
		for (int side = 0; side < 2; ++side)
		{
			const bool isBuy = side == 0;
			SCOPED_TRACE(::testing::Message() << "price_mode=" << mode << ", isBuy=" << isBuy);
			SFakeCtx ctx;
			ctx.comm = mkSComm("STK");
			ctx.pos = ctx.avail = 1000;
			WtStockVWapExeUnit unit;
			bootS(unit, ctx, kStk, isBuy ? 1500 : 100, mkSCfg(5, mode, 0, 100.0, 1000.0));
			const auto& initialOrders = isBuy ? ctx.buys : ctx.sells;
			ASSERT_EQ(initialOrders.size(), 1u);
			EXPECT_DOUBLE_EQ(initialOrders[0].price, isBuy ? buyInitial[mode] : sellInitial[mode]);
			unit.on_order(7000, kStk, isBuy, 50, initialOrders[0].price, true);
			unit.on_tick(mkSTick(kStk, 100100000, 10.60, 10.59, 10.61));
			const auto& retryOrders = isBuy ? ctx.buys : ctx.sells;
			ASSERT_EQ(retryOrders.size(), 2u);
			EXPECT_DOUBLE_EQ(retryOrders[1].price, isBuy ? buyRetry[mode] : sellRetry[mode]);
			EXPECT_TRUE(isBuy ? ctx.sells.empty() : ctx.buys.empty());
			ctx.comm->release();
		}
	}
}

TEST(r7_stkvw, canceled_retry_keeps_upper_and_lower_limit_caps)
{
	SVwapFile vf(25.0);
	for (int side = 0; side < 2; ++side)
	{
		const bool isBuy = side == 0;
		SCOPED_TRACE(::testing::Message() << "isBuy=" << isBuy);
		SFakeCtx ctx;
		ctx.comm = mkSComm("STK");
		ctx.pos = ctx.avail = 1000;
		WtStockVWapExeUnit unit;
		bootS(unit, ctx, kStk, isBuy ? 1500 : 100, mkSCfg(5, 2, 0, 100.0, 1000.0));
		const auto& initialOrders = isBuy ? ctx.buys : ctx.sells;
		ASSERT_EQ(initialOrders.size(), 1u);
		unit.on_order(7000, kStk, isBuy, 50, initialOrders[0].price, true);
		if (isBuy)
			unit.on_tick(mkSTick(kStk, 100100000, 10.60, 10.59, 10.60, 10.60, 0));
		else
			unit.on_tick(mkSTick(kStk, 100100000, 10.20, 10.19, 10.21, 0, 10.20));
		const auto& retryOrders = isBuy ? ctx.buys : ctx.sells;
		ASSERT_EQ(retryOrders.size(), 2u);
		EXPECT_DOUBLE_EQ(retryOrders[1].price, isBuy ? 10.60 : 10.20);
		EXPECT_TRUE(ctx.hasLog(isBuy ? "modified to upper limit price" : "modified to lower limit price"));
		ctx.comm->release();
	}
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
// 减仓应卖出且不能超过可用仓；价格分支命中下限收敛。
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
	EXPECT_LE(ctx.sells[0].qty, ctx.pos - 100.0);
	EXPECT_GT(ctx.sells[0].price, 0.0);
	EXPECT_DOUBLE_EQ(ctx.sells[0].price, 10.20);
	EXPECT_TRUE(ctx.hasLog("modified to lower limit price"));
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 减仓单撤销后重发仍须保持卖向，且重发量不能超过目标差或可卖仓。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, canceled_sell_refires_with_safe_sell_quantity)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 1000;
	ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 100, mkSCfg());
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_TRUE(ctx.buys.empty());
	unit.on_order(7000, kStk, false, 50, 10.50, true);
	EXPECT_EQ(ctx.sells.size(), 1u); // callback cannot reuse the quote which placed the canceled order
	EXPECT_TRUE(ctx.hasLog("waits for the next fresh in-session tick"));
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.sells.size(), 2u);
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_GT(ctx.sells[1].qty, 0.0);
	EXPECT_LE(ctx.sells[1].qty, 100.0);
	EXPECT_LE(ctx.sells[1].qty, ctx.avail);
	EXPECT_GT(ctx.sells[1].price, 0.0);
	ctx.comm->release();
}

TEST(r7_stkvw, canceled_order_does_not_retry_without_a_fresh_session_tick)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 100, mkSCfg());
	ASSERT_EQ(ctx.sells.size(), 1u);
	unit.on_order(7000, kStk, false, 50, 10.50, true); // no newer quote accompanies this reply
	EXPECT_EQ(ctx.sells.size(), 1u);
	unit.on_tick(mkSTick(kStk, 150100000, 10.50, 10.49, 10.51)); // out-of-session
	EXPECT_EQ(ctx.sells.size(), 1u);
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51)); // older valid tick must stay stale
	EXPECT_EQ(ctx.sells.size(), 1u);
	EXPECT_TRUE(ctx.hasLog("fresh in-session tick"));
	EXPECT_TRUE(ctx.hasLog("stale tick"));
	ctx.comm->release();
}

TEST(r7_stkvw, empty_retry_ids_quarantine_across_ticks_and_reconnect)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 100, mkSCfg(3, 0, 0, 100.0, 1000.0, 0, 0, true, 1000, 1100));
	ASSERT_EQ(ctx.sells.size(), 1u);
	const auto fired = unit._fired_times;
	const auto lastFire = unit._last_fire_time;
	unit.on_order(7000, kStk, false, 50, 10.50, true);
	ctx.returnEmptyOrderIds = true;
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.emptyOrderIdAttempts, 1u);
	EXPECT_TRUE(unit.hasUnverifiedSubmission());
	EXPECT_EQ(unit._fired_times, fired);
	EXPECT_EQ(unit._last_fire_time, lastFire);
	const uint32_t expiredId = 8888;
	unit._orders_mon.push_order(&expiredId, 1, ctx.curTime);
	unit._orders_mon.check_orders_with_generation(1, ctx.curTime + 1000,
		[&](uint32_t id, uint64_t generation) {
			EXPECT_TRUE(unit._orders_mon.mark_unverified_timeout(id, generation, 1, ctx.curTime + 1000));
		});
	EXPECT_TRUE(unit._orders_mon.has_unverified_timeout());
	ctx.returnEmptyOrderIds = false;
	unit.on_tick(mkSTick(kStk, 104100000, 10.50, 10.49, 10.51));
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51, 0, 0, 20240104, 20240104));
	ctx.undone = 100;
	unit.on_channel_lost();
	unit.on_channel_ready();
	unit.set_position(kStk, 500);
	unit.clear_all_position(kStk);
	unit.fire_at_once(100);
	EXPECT_EQ(ctx.emptyOrderIdAttempts, 1u);
	EXPECT_EQ(ctx.sells.size(), 1u);
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.bydirCancels.empty());
	EXPECT_TRUE(ctx.byidCancels.empty());
	EXPECT_TRUE(unit.hasUnverifiedSubmission());
	EXPECT_TRUE(unit._orders_mon.has_unverified_timeout());
	EXPECT_EQ(unit._fired_times, 0u); // explicit target updates reset the normal execution counter
	EXPECT_TRUE(ctx.hasLog("submission outcome unverified"));
	ctx.comm->release();
}

TEST(r7_stkvw, empty_initial_ids_quarantine_without_counting_or_auto_cancellation)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.returnEmptyOrderIds = true;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 1000, mkSCfg(1, 0, 0, 100.0, 1000.0, 0, 3600, true, 1000, 1100));
	ASSERT_EQ(ctx.emptyOrderIdAttempts, 1u);
	EXPECT_TRUE(unit.hasUnverifiedSubmission());
	EXPECT_EQ(unit._fired_times, 0u);
	EXPECT_EQ(unit._last_fire_time, 0u);
	ctx.returnEmptyOrderIds = false;
	ctx.undone = 100;
	unit.on_channel_lost();
	unit.on_channel_ready();
	unit.on_tick(mkSTick(kStk, 100010000, 10.50, 10.49, 10.51));
	unit.do_calc();
	unit.fire_at_once(100);
	EXPECT_EQ(ctx.emptyOrderIdAttempts, 1u);
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_TRUE(ctx.bydirCancels.empty());
	EXPECT_TRUE(ctx.byidCancels.empty());
	EXPECT_EQ(unit._fired_times, 0u);
	EXPECT_TRUE(ctx.hasLog("submission outcome unverified"));
	WTSVariant* cfg = mkSCfg();
	unit.init(&ctx, kStk, cfg);
	cfg->release();
	unit.set_position(kStk, 1000);
	unit.on_channel_ready();
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(unit.hasUnverifiedSubmission());
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.bydirCancels.empty());
	ctx.comm->release();
}

TEST(r7_stkvw, canceled_retry_keeps_sublot_remainder_pending_without_invalid_order)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.comm->setTradingMode(TM_Both);
	ctx.pos = ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 900, mkSCfg());
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 100.0);

	unit.on_order(7000, kStk, false, 50, 10.50, true);
	EXPECT_EQ(ctx.sells.size(), 1u);
	ctx.pos = 950;
	ctx.avail = 50;
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	EXPECT_EQ(ctx.sells.size(), 1u); // a 50-share retry is below the stock minimum and is not sent
	EXPECT_EQ(unit.executionState(), WtStockVWapExeUnit::ExecutionState::pending_remainder);
	EXPECT_DOUBLE_EQ(unit.pendingRemainder(), -50.0);
	EXPECT_TRUE(ctx.hasLog("canceled retry has less than one executable stock lot"));
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

TEST(r7_stkvw, canceled_retry_recomputes_direction_from_current_total_position)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 900, mkSCfg());
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_TRUE(ctx.buys.empty());

	unit.on_order(7000, kStk, false, 50, 10.50, true);
	ctx.pos = ctx.avail = 800; // new position crosses from a sell target diff to a buy diff
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);
	EXPECT_EQ(ctx.sells.size(), 1u);
	EXPECT_LE(ctx.buys[0].qty, 900.0 - ctx.pos);
	ctx.comm->release();
}

TEST(r7_stkvw, changed_target_cancel_recalculates_latest_target_on_fresh_tick)
{
	SVwapFile vf(250.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 100, mkSCfg());
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);

	unit.set_position(kStk, 500); // old order belongs to the previous target generation
	unit.on_order(7000, kStk, true, 100, ctx.buys[0].price, true);
	EXPECT_EQ(ctx.buys.size(), 1u); // callback only queues recalculation
	EXPECT_TRUE(ctx.hasLog("target recalculation waits for the next fresh in-session tick"));

	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 300.0); // sized from target 500, not stale slice target 100
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

TEST(r7_stkvw, shrinking_buy_target_cancels_oversized_live_order_then_keeps_sublot_pending)
{
	SVwapFile vf(250.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.cancelBydirRetCnt = 1;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 100, mkSCfg());
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);
	ctx.undone = 100;
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	unit.set_position(kStk, 50);
	ASSERT_EQ(ctx.bydirCancels.size(), 1u);
	EXPECT_TRUE(ctx.bydirCancels[0].second);
	EXPECT_TRUE(ctx.hasLog("exceeds current total target remainder 50"));
	EXPECT_EQ(ctx.buys.size(), 1u);

	unit.on_order(7000, kStk, true, 100, ctx.buys[0].price, true);
	ctx.undone = 0;
	unit.on_tick(mkSTick(kStk, 100200000, 10.50, 10.49, 10.51));
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_EQ(unit.executionState(), WtStockVWapExeUnit::ExecutionState::pending_remainder);
	EXPECT_DOUBLE_EQ(unit.pendingRemainder(), 50.0);
	EXPECT_TRUE(ctx.hasLog("buy remainder is below one stock lot"));
	ctx.comm->release();
}

TEST(r7_stkvw, t1_sell_order_oversize_uses_total_target_gap_not_executable_increment)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.comm->setTradingMode(TM_Both);
	ctx.pos = ctx.avail = 1000;
	ctx.cancelBydirRetCnt = 1;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 900, mkSCfg());
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 100.0);

	ctx.avail = 0; // the live sell reserves the only sellable quantity
	ctx.undone = -100;
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	unit.set_position(kStk, 950); // effective executable diff is zero; total gap is only -50
	ASSERT_EQ(ctx.bydirCancels.size(), 1u);
	EXPECT_FALSE(ctx.bydirCancels[0].second);
	EXPECT_TRUE(ctx.hasLog("exceeds current total target remainder -50"));

	unit.on_order(7000, kStk, false, 100, ctx.sells[0].price, true);
	ctx.undone = 0;
	unit.on_tick(mkSTick(kStk, 100200000, 10.50, 10.49, 10.51));
	EXPECT_EQ(ctx.sells.size(), 1u);
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_EQ(unit.executionState(), WtStockVWapExeUnit::ExecutionState::pending_remainder);
	EXPECT_DOUBLE_EQ(unit.pendingRemainder(), -50.0);
	EXPECT_TRUE(ctx.hasLog("T+1 sellable position currently prevents reaching the requested target"));
	ctx.comm->release();
}

TEST(r7_stkvw, clear_at_zero_position_cancels_pending_buy_and_does_not_retry_old_target)
{
	SVwapFile vf(250.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.cancelBydirRetCnt = 1;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 1000, mkSCfg());
	ASSERT_EQ(ctx.buys.size(), 1u);
	ctx.undone = ctx.buys[0].qty;

	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	unit.clear_all_position(kStk); // position is zero, but the buy remains open
	ASSERT_EQ(ctx.bydirCancels.size(), 1u);
	EXPECT_TRUE(ctx.bydirCancels[0].second);
	EXPECT_TRUE(ctx.buys.size() == 1u && ctx.sells.empty());

	unit.on_order(7000, kStk, true, ctx.undone, ctx.buys[0].price, true);
	EXPECT_EQ(ctx.buys.size(), 1u); // stale target is never retried by the report
	ctx.undone = 0;
	unit.on_tick(mkSTick(kStk, 100200000, 10.50, 10.49, 10.51));
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_EQ(unit.executionState(), WtStockVWapExeUnit::ExecutionState::complete);
	ctx.comm->release();
}

TEST(r7_stkvw, last_tick_reference_survives_calc_replacement_and_destruction)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	WTSTickData* first = mkSTick(kStk, 100000000, 10.50, 10.49, 10.51);
	WTSTickData* second = mkSTick(kStk, 100100000, 10.50, 10.49, 10.51);
	{
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = mkSCfg();
		unit.init(&ctx, kStk, cfg);
		cfg->release();
		ASSERT_TRUE(unit.initializationSucceeded());
		unit.set_position(kStk, 1000);
		unit.on_tick(first);
		unit.on_channel_ready();
		ASSERT_EQ(ctx.buys.size(), 1u);
		EXPECT_EQ(first->retainCount(), 2u); // caller plus unit, even after do_calc

		ctx.pos = ctx.avail = 1000;
		unit.on_tick(second);
		EXPECT_EQ(first->retainCount(), 1u); // replacing the tick drops only the unit ref
		EXPECT_EQ(second->retainCount(), 2u);
		unit.set_position(kStk, 2000);
		ASSERT_EQ(ctx.buys.size(), 2u);
		EXPECT_EQ(second->retainCount(), 2u); // a second do_calc keeps the unit ref
	}
	EXPECT_EQ(first->retainCount(), 1u);
	EXPECT_EQ(second->retainCount(), 1u);
	ctx.comm->release();
	first->release();
	second->release();
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
		unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
		EXPECT_TRUE(ctx.buys.empty());
		EXPECT_TRUE(ctx.sells.empty());
		unit.on_channel_ready();
		EXPECT_TRUE(ctx.hasLog("unmanaged orders of"));
		EXPECT_TRUE(ctx.hasLog("cancelcnt -> 2"));
		EXPECT_TRUE(ctx.hasLog("waits for outstanding cancellation reports before recalculation")); // 撤单回报未到前不重算发单
		EXPECT_TRUE(ctx.buys.empty());
		EXPECT_TRUE(ctx.sells.empty());
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
// on_order：未知回报忽略，成交回报从监控器摘除。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, on_order_reports_ignore_unknown_ids_and_track_fill)
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
	unit.on_order(7001, kStk, true, 5, 10.60, true);       // 未登记的撤单回报 -> 忽略
	EXPECT_EQ(ctx.buys.size(), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_trade 只写日志；拒单回调延迟到更新的有效行情后再计算。
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
	unit.on_entrust(7000, kStk, false, "rejected");        // 只排队，不能借旧 tick 重算
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.hasLog("rejected order deferred until the next fresh in-session tick"));
	unit.on_entrust(7000, kStk, true, "ok");
	EXPECT_EQ(ctx.buys.size(), 1u);
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_GT(ctx.buys[1].qty, 0.0);
	EXPECT_LE(ctx.buys[1].qty, 500.0); // current target diff, not a cached retry quantity
	ctx.comm->release();
}

TEST(r7_stkvw, rejected_order_defers_reverse_order_cancel_until_fresh_tick)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 1500, mkSCfg());
	ASSERT_EQ(ctx.buys.size(), 1u);
	ctx.undone = -100; // reverse-side live order; recalculation must cancel it

	unit.on_entrust(7000, kStk, false, "rejected");
	EXPECT_TRUE(ctx.bydirCancels.empty());
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.hasLog("rejected order deferred until the next fresh in-session tick"));

	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.bydirCancels.size(), 1u);
	EXPECT_FALSE(ctx.bydirCancels[0].second); // only after the fresh tick
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
	bootS(unit, ctx, kStk, 1500, mkSCfg(5, 0, 0, 100.0, 1000.0, 0, 14400));
	ASSERT_EQ(ctx.buys.size(), 1u);
	uint32_t n = (uint32_t)ctx.buys.size();
	// The initial buy is filled before requesting liquidation; keep the fake
	// account position/order state consistent with that fill.
	ctx.pos += ctx.buys[0].qty;
	ctx.avail += ctx.buys[0].qty;
	unit.on_order(7000, kStk, true, 0, ctx.buys[0].price, false);
	unit.clear_all_position("SHSE.STK.000001");            // 非本合约 -> 直接 return
	EXPECT_EQ(ctx.buys.size(), n);
	EXPECT_TRUE(ctx.sells.empty());
	unit.clear_all_position(kStk);                         // _target_pos=0; cached tick must not trigger a sell
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_TRUE(ctx.hasLog("No tick of"));
	unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
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
// 正控：首 tick 到达后再就绪，应实际发出数量与价格有效的买单。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, channel_ready_after_first_tick_submits_safe_order)
{
	ScopedProbeDir probeDir;
	ASSERT_TRUE(probeDir.ready());
	const ProbeChildResult result = run_channel_ready_probe_in_child(probeDir.path(), true, 1000.0);
	expect_channel_ready_probe_success(result);
	EXPECT_TRUE(probeDir.cleanup()) << "failed to remove private death-test directory";
}

//////////////////////////////////////////////////////////////////////////
// Malformed two-segment code is rejected cleanly instead of indexing past the split.
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, two_segment_stock_code_fails_initialization)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	WtStockVWapExeUnit unit;
	WTSVariant* cfg = mkSCfg();
	unit.init(&ctx, "SHSE.600000", cfg);
	cfg->release();
	EXPECT_FALSE(unit.initializationSucceeded());
	unit.on_channel_ready();
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// total_times 必须为正；非法配置确定性失败且执行单元保持 inert，不依赖 sanitizer。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, init_zero_total_times_rejected_and_inert)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	WtStockVWapExeUnit unit;
	WTSVariant* cfg = mkSCfg(0);
	unit.init(&ctx, kStk, cfg);
	cfg->release();
	EXPECT_FALSE(unit.initializationSucceeded());
	EXPECT_TRUE(ctx.hasLog("total_times must be greater than zero"));
	unit.set_position(kStk, 1000);
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit.on_channel_ready();
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

TEST(r7_stkvw, init_positive_total_times_control_returns)
{
	r7_test::ScopedTempDir probeDir("WT_R7_002_STOCK_VWAP");
	ASSERT_TRUE(probeDir.ready());
	const r7_test::ProbeResult result = r7_test::run_bounded_child(probeDir.path(), 5, run_stock_vwap_init_probe);
	r7_test::expect_normal_return(result);
	EXPECT_TRUE(probeDir.cleanup()) << "failed to remove private init-probe directory";
}

TEST(r7_stkvw, failed_concurrent_calc_guard_does_not_release_owner)
{
	WtStockVWapExeUnit unit;
	EXPECT_FALSE(unit._in_calc.load(std::memory_order_acquire));
	{
		WtStockVWapExeUnit::CalcFlag owner(&unit._in_calc);
		ASSERT_FALSE(static_cast<bool>(owner));

		bool contenderSawBusy = false;
		std::thread contender([&]() {
			WtStockVWapExeUnit::CalcFlag nested(&unit._in_calc);
			contenderSawBusy = static_cast<bool>(nested);
		});
		contender.join();

		EXPECT_TRUE(contenderSawBusy);
		EXPECT_TRUE(unit._in_calc.load(std::memory_order_acquire));
	}
	EXPECT_FALSE(unit._in_calc.load(std::memory_order_acquire));
}

TEST(r7_stkvw, startup_cancel_without_explicit_target_does_not_recalculate_default_zero)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 1000.0;
	ctx.avail = 1000.0;
	ctx.undone = 100.0;
	ctx.cancelBydirRetCnt = 1;
	{
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = mkSCfg();
		unit.init(&ctx, kStk, cfg);
		cfg->release();
		ASSERT_TRUE(unit.initializationSucceeded());
		unit.on_channel_ready();
		ASSERT_EQ(ctx.bydirCancels.size(), 1u);
		ASSERT_EQ(unit._cancel_cnt, 1u);
		EXPECT_FALSE(unit._target_pos_valid);

		ctx.undone = 0.0;
		unit.on_order(7000, kStk, true, 100.0, 10.50, true);
		unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
		EXPECT_EQ(unit._cancel_cnt, 0u);
		EXPECT_FALSE(unit._target_pos_valid);
		EXPECT_TRUE(ctx.buys.empty());
		EXPECT_TRUE(ctx.sells.empty());
	}
	ctx.comm->release();
}
