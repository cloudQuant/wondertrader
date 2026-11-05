// Production-path regression for the shared unverified-timeout guard used by
// all six execution units' account-wide channel reconciliation cancels.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "Includes/ExecuteDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "WtExeFact/WtMinImpactExeUnit.h"
#include "WtExeFact/WtOrdMon.h"
#define private public
#include "WtExeFact/WtDiffMinImpactExeUnit.h"
#include "WtExeFact/WtStockMinImpactExeUnit.h"
#include "WtExeFact/WtTWapExeUnit.h"
#include "WtExeFact/WtVWapExeUnit.h"
#include "WtExeFact/WtStockVWapExeUnit.h"
#undef private

using namespace wtp;

namespace
{
const char* const kStockCode = "SHSE.STK.600000";

class LatchCtx : public ExecuteContext
{
public:
	LatchCtx()
	{
		sess = WTSSessionInfo::create("N05_LATCH", "N05 timeout latch test session");
		sess->addTradingSection(900, 1500);
	}
	~LatchCtx() override
	{
		if (sess)
			sess->release();
	}

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return nullptr; }
	WTSTickData* grabLastTick(const char*) override { return nullptr; }
	double getPosition(const char*, bool = true, int32_t = 3) override { return 0; }
	OrderMap* getOrders(const char*) override { return nullptr; }
	double getUndoneQty(const char*) override { return undone; }

	OrderIDs buy(const char*, double, double, bool) override
	{
		OrderIDs ids{ nextId++ };
		return ids;
	}
	OrderIDs sell(const char*, double, double, bool) override
	{
		OrderIDs ids{ nextId++ };
		return ids;
	}
	bool cancel(uint32_t localid) override
	{
		localCancels.push_back(localid);
		return true;
	}
	OrderIDs cancel(const char*, bool isBuy, double = 0) override
	{
		batchCancels.push_back(isBuy);
		OrderIDs ids{ nextId++ };
		return ids;
	}
	void writeLog(const char* log) override { logs.emplace_back(log ? log : ""); }
	WTSCommodityInfo* getCommodityInfo(const char*) override { return comm; }
	WTSSessionInfo* getSessionInfo(const char*) override { return sess; }
	uint64_t getCurTime() override { return curTime; }

	bool hasLog(const std::string& needle) const
	{
		for (const auto& log : logs)
			if (log.find(needle) != std::string::npos)
				return true;
		return false;
	}

	double undone{ 0 };
	uint64_t curTime{ 20240101093000000ull };
	uint32_t nextId{ 8000 };
	WTSCommodityInfo* comm{ nullptr };
	WTSSessionInfo* sess{ nullptr };
	std::vector<bool> batchCancels;
	std::vector<uint32_t> localCancels;
	std::vector<std::string> logs;
};

WTSCommodityInfo* makeCommodity()
{
	WTSCommodityInfo* comm = WTSCommodityInfo::create("rb", "螺纹钢", "SHFE", "NIGHT_DAY", "CHN");
	comm->setVolScale(10);
	comm->setPriceTick(1.0);
	comm->setLotsTick(1.0);
	comm->setMinLots(1.0);
	comm->setCategory(CC_Future);
	comm->setCoverMode(CM_OpenCover);
	comm->setPriceMode(PM_Both);
	comm->setTradingMode(TM_Both);
	return comm;
}

WTSCommodityInfo* makeNamedFutureCommodity(const char* name)
{
	WTSCommodityInfo* comm = WTSCommodityInfo::create("rb", name, "SHFE", "N05_LATCH", "CHN");
	comm->setVolScale(10);
	comm->setPriceTick(1.0);
	comm->setLotsTick(1.0);
	comm->setMinLots(1.0);
	comm->setCategory(CC_Future);
	comm->setCoverMode(CM_OpenCover);
	comm->setPriceMode(PM_Both);
	comm->setTradingMode(TM_Both);
	return comm;
}

WTSCommodityInfo* makeStockCommodity(const char* name)
{
	WTSCommodityInfo* comm = WTSCommodityInfo::create("STK", name, "SHSE", "N05_LATCH", "CHN");
	comm->setVolScale(1);
	comm->setPriceTick(0.01);
	comm->setLotsTick(1.0);
	comm->setMinLots(1.0);
	comm->setCategory(CC_Stock);
	comm->setCoverMode(CM_OpenCover);
	comm->setPriceMode(PM_Both);
	comm->setTradingMode(TM_Long);
	return comm;
}

WTSTickData* makeTick(uint32_t actTime)
{
	WTSTickStruct ts{};
	std::snprintf(ts.code, sizeof(ts.code), "%s", "rb2401");
	ts.action_date = 20240101;
	ts.trading_date = 20240101;
	ts.action_time = actTime;
	ts.price = 3505.0;
	ts.pre_close = 3400.0;
	ts.bid_prices[0] = 3500.0;
	ts.bid_qty[0] = 100;
	ts.ask_prices[0] = 3510.0;
	ts.ask_qty[0] = 50;
	return WTSTickData::create(ts);
}

WTSVariant* makeConfig(uint32_t expireSeconds)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("offset", static_cast<int32_t>(0));
	cfg->append("expire", expireSeconds);
	cfg->append("pricemode", static_cast<int32_t>(0));
	cfg->append("span", static_cast<uint32_t>(0));
	cfg->append("byrate", false);
	cfg->append("lots", 2.0);
	cfg->append("rate", 0.0);
	cfg->append("minopenlots", 1.0);
	return cfg;
}

WTSVariant* makeTwapConfig()
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("ord_sticky", static_cast<uint32_t>(1));
	cfg->append("begin_time", static_cast<uint32_t>(900));
	cfg->append("end_time", static_cast<uint32_t>(1500));
	cfg->append("total_times", static_cast<uint32_t>(5));
	cfg->append("tail_secs", static_cast<uint32_t>(0));
	cfg->append("price_mode", static_cast<uint32_t>(0));
	cfg->append("price_offset", static_cast<uint32_t>(0));
	cfg->append("lots", 100.0);
	cfg->append("minopenlots", 1.0);
	return cfg;
}

WTSVariant* makeVwapConfig()
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("begin_time", static_cast<uint32_t>(930));
	cfg->append("end_time", static_cast<uint32_t>(1500));
	cfg->append("ord_sticky", static_cast<uint32_t>(1));
	cfg->append("tail_secs", static_cast<uint32_t>(0));
	cfg->append("total_times", static_cast<uint32_t>(5));
	cfg->append("price_mode", static_cast<uint32_t>(0));
	cfg->append("price_offset", static_cast<uint32_t>(0));
	cfg->append("lots", 100.0);
	cfg->append("minopenlots", 1.0);
	return cfg;
}

WTSVariant* makeStockVwapConfig()
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("begin_time", static_cast<uint32_t>(930));
	cfg->append("end_time", static_cast<uint32_t>(1500));
	cfg->append("ord_sticky", static_cast<uint32_t>(1));
	cfg->append("tail_secs", static_cast<uint32_t>(0));
	cfg->append("total_times", static_cast<uint32_t>(5));
	cfg->append("price_mode", static_cast<uint32_t>(0));
	cfg->append("offset", static_cast<uint32_t>(0));
	cfg->append("lots", 1000.0);
	cfg->append("minopenlots", 100.0);
	return cfg;
}

class TempVwapCwd
{
public:
	explicit TempVwapCwd(const char* tag)
		: _old(std::filesystem::current_path())
	{
		static std::atomic<uint64_t> sequence{ 0 };
		const std::filesystem::path tempRoot = std::filesystem::temp_directory_path();
		bool created = false;
		for (uint32_t attempt = 0; attempt < 64 && !created; ++attempt)
		{
			const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
			const uint64_t serial = sequence.fetch_add(1, std::memory_order_relaxed);
			_dir = tempRoot / (std::string("n05_latch_") + tag + "_" + std::to_string(stamp) + "_" + std::to_string(serial));
			std::error_code ec;
			created = std::filesystem::create_directory(_dir, ec);
			if (!created && ec && ec != std::errc::file_exists)
				throw std::filesystem::filesystem_error("create N05 latch test directory", _dir, ec);
		}
		if (!created)
			throw std::runtime_error("could not create a unique N05 latch test directory");

		try
		{
			std::filesystem::current_path(_dir);
		}
		catch (...)
		{
			std::error_code ec;
			std::filesystem::remove_all(_dir, ec);
			throw;
		}
	}

	bool writeProfile(const char* commodityName, WTSSessionInfo& session) const
	{
		std::ofstream out((std::string("Vwap_") + commodityName + ".txt").c_str(),
			std::ios::out | std::ios::binary | std::ios::trunc);
		if (!out.is_open())
			return false;

		out << "{\"version\":2,\"profiles\":[{\"session_id\":\"" << session.id()
			<< "\",\"offset_minutes\":" << session.getOffsetMins() << ",\"sections\":[";
		const WTSSessionInfo::TradingTimes& sections = session.getTradingSections();
		for (size_t i = 0; i < sections.size(); ++i)
		{
			if (i != 0)
				out << ',';
			out << "{\"from\":" << sections[i].first_raw
				<< ",\"to\":" << sections[i].second_raw << '}';
		}
		out << "],\"bucket_unit\":\"trading_minute\",\"value_mode\":\"cumulative\","
			<< "\"close_inclusive\":true,\"values\":[";
		for (uint32_t i = 0; i <= session.getTradingMins(); ++i)
		{
			if (i != 0)
				out << ',';
			out << "1.0";
		}
		out << "]}]}";
		out.close();
		return !out.fail();
	}

	~TempVwapCwd()
	{
		std::error_code ec;
		std::filesystem::current_path(_old, ec);
		std::filesystem::remove_all(_dir, ec);
	}

private:
	std::filesystem::path _dir;
	std::filesystem::path _old;
};

class StockMinImpactProbe : public WtStockMinImpactExeUnit
{
public:
	void bindForReconciliation(ExecuteContext* ctx)
	{
		_ctx = ctx;
		_code = kStockCode;
		_initialization_succeeded = true;
		_now = 20240101093000000ull;
	}
};

void armTimeoutLatch(WtOrdMon& monitor, uint32_t localid)
{
	const WtOrdMonOrderIdentity identity = monitor.push_order_with_identity(localid, 0, true);
	ASSERT_TRUE(identity.valid());
	ASSERT_TRUE(monitor.mark_unverified_timeout(identity, 1, 1000));
}

// Test seam for reaching the otherwise-unmanaged ready branch: use the real
// WtOrdMon timeout-candidate and mark contract, then remove only the synthetic
// local order so the production unit's ready/reconcile callback sees an empty
// local monitor plus the latched state. This does not exercise each unit's
// own on_tick timeout path, so this verifies ready/reconcile call sites rather
// than the end-to-end timeout-to-ready sequence for these four unit classes.
void armDetachedTimeoutLatch(WtOrdMon& monitor, uint32_t localid)
{
	const WtOrdMonOrderIdentity identity = monitor.push_order_with_identity(localid, 0, true);
	ASSERT_TRUE(identity.valid());
	bool marked = false;
	monitor.check_orders_with_identity(1, 1000,
		[&](uint32_t candidateId, WtOrdMonOrderIdentity candidate) {
			if (candidateId == localid)
				marked = monitor.mark_unverified_timeout(candidate, 1, 1000);
		});
	ASSERT_TRUE(marked);
	ASSERT_TRUE(monitor.erase_order(identity));
	EXPECT_FALSE(monitor.has_order());
	EXPECT_TRUE(monitor.has_unverified_timeout());
}
}

TEST(n05_unmanaged_cancel_latch, timeout_remains_fail_closed_through_ready_reentry)
{
	LatchCtx ctx;
	ctx.comm = makeCommodity();
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = makeConfig(1);
		unit.init(&ctx, "rb2401", cfg);
		cfg->release();
		ASSERT_TRUE(unit.initializationSucceeded());

		unit.set_position("rb2401", 6);
		WTSTickData* firstTick = makeTick(93000000);
		unit.on_tick(firstTick);
		firstTick->release();
		const uint32_t timedOutOrder = ctx.nextId - 1;

		// Expire a real unit-owned order through on_tick. The timeout callback
		// records the permanent latch instead of using an unverified local ID.
		ctx.curTime += 1000;
		WTSTickData* nextTick = makeTick(93001000);
		unit.on_tick(nextTick);
		nextTick->release();
		ASSERT_TRUE(ctx.hasLog("cannot be auto-canceled: legacy localid is unverified"));

		// A terminal report removes the order record but must not clear the latch.
		unit.on_order(timedOutOrder, "rb2401", true, 0, 3505, true);
		ctx.undone = 5; // account-level unmanaged orders from another owner
		unit.on_channel_ready();
		unit.on_channel_ready(); // ready re-entry must remain fail-closed
		EXPECT_TRUE(ctx.batchCancels.empty());
		EXPECT_TRUE(ctx.hasLog("suppressed after an unverified timeout"));
	}
	ctx.comm->release();
}

TEST(n05_unmanaged_cancel_latch, unlatched_channel_ready_keeps_existing_cancel_behavior)
{
	LatchCtx ctx;
	ctx.comm = makeCommodity();
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = makeConfig(1);
		unit.init(&ctx, "rb2401", cfg);
		cfg->release();
		ASSERT_TRUE(unit.initializationSucceeded());

		ctx.undone = 5;
		unit.on_channel_ready();
		ASSERT_EQ(ctx.batchCancels.size(), 1u);
		EXPECT_TRUE(ctx.batchCancels.front());
		EXPECT_TRUE(ctx.localCancels.empty());
	}
	ctx.comm->release();
}

TEST(n05_unmanaged_cancel_latch, stock_minimpact_ready_preserves_latched_monitor_and_suppresses_cancel)
{
	LatchCtx ctx;
	ctx.undone = 500;
	{
		StockMinImpactProbe unit;
		unit.bindForReconciliation(&ctx);
		const uint32_t timedOutOrder = 9101;
		armTimeoutLatch(unit._orders_mon, timedOutOrder);

		unit.on_channel_ready();
		unit.on_channel_ready();
		EXPECT_TRUE(ctx.batchCancels.empty());
		EXPECT_TRUE(unit._orders_mon.has_order(timedOutOrder));
		EXPECT_TRUE(unit._orders_mon.has_unverified_timeout());
		EXPECT_TRUE(ctx.hasLog("suppressed after an unverified timeout"));
	}
}

TEST(n05_unmanaged_cancel_latch, stock_minimpact_unlatched_ready_keeps_batch_cancel_behavior)
{
	LatchCtx ctx;
	ctx.undone = 500;
	{
		StockMinImpactProbe unit;
		unit.bindForReconciliation(&ctx);
		unit.on_channel_ready();
		ASSERT_EQ(ctx.batchCancels.size(), 1u);
		EXPECT_TRUE(ctx.batchCancels.front());
	}
}

TEST(n05_unmanaged_cancel_latch, diff_minimpact_ready_suppresses_latched_cancel_and_keeps_unlatched_behavior)
{
	LatchCtx lockedCtx;
	lockedCtx.comm = makeCommodity();
	lockedCtx.undone = 5;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = makeConfig(1);
		unit.init(&lockedCtx, "rb2401", cfg);
		cfg->release();
		ASSERT_TRUE(unit.initializationSucceeded());

		armDetachedTimeoutLatch(unit._orders_mon, 9201);
		unit.on_channel_ready();
		EXPECT_TRUE(lockedCtx.batchCancels.empty());
		EXPECT_TRUE(lockedCtx.hasLog("suppressed after an unverified timeout"));
	}
	lockedCtx.comm->release();

	LatchCtx openCtx;
	openCtx.comm = makeCommodity();
	openCtx.undone = -5;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = makeConfig(1);
		unit.init(&openCtx, "rb2401", cfg);
		cfg->release();
		ASSERT_TRUE(unit.initializationSucceeded());

		unit.on_channel_ready();
		ASSERT_EQ(openCtx.batchCancels.size(), 1u);
		EXPECT_FALSE(openCtx.batchCancels.front());
		EXPECT_TRUE(openCtx.localCancels.empty());
	}
	openCtx.comm->release();
}

TEST(n05_unmanaged_cancel_latch, twap_ready_suppresses_latched_cancel_and_keeps_unlatched_behavior)
{
	LatchCtx lockedCtx;
	lockedCtx.comm = makeCommodity();
	lockedCtx.undone = 5;
	{
		WtTWapExeUnit unit;
		WTSVariant* cfg = makeTwapConfig();
		unit.init(&lockedCtx, "rb2401", cfg);
		cfg->release();
		ASSERT_TRUE(unit.initializationSucceeded());

		armDetachedTimeoutLatch(unit._orders_mon, 9301);
		unit.on_channel_ready();
		EXPECT_TRUE(lockedCtx.batchCancels.empty());
		EXPECT_TRUE(lockedCtx.hasLog("suppressed after an unverified timeout"));
	}
	lockedCtx.comm->release();

	LatchCtx openCtx;
	openCtx.comm = makeCommodity();
	openCtx.undone = -5;
	{
		WtTWapExeUnit unit;
		WTSVariant* cfg = makeTwapConfig();
		unit.init(&openCtx, "rb2401", cfg);
		cfg->release();
		ASSERT_TRUE(unit.initializationSucceeded());

		unit.on_channel_ready();
		ASSERT_EQ(openCtx.batchCancels.size(), 1u);
		EXPECT_FALSE(openCtx.batchCancels.front());
		EXPECT_TRUE(openCtx.localCancels.empty());
	}
	openCtx.comm->release();
}

TEST(n05_unmanaged_cancel_latch, vwap_reconcile_suppresses_latched_cancel_and_keeps_unlatched_behavior)
{
	TempVwapCwd lockedCwd("future_locked");
	LatchCtx lockedCtx;
	lockedCtx.comm = makeNamedFutureCommodity("N05FutureLocked");
	ASSERT_TRUE(lockedCwd.writeProfile("N05FutureLocked", *lockedCtx.sess));
	lockedCtx.undone = 5;
	{
		WtVWapExeUnit unit;
		WTSVariant* cfg = makeVwapConfig();
		unit.init(&lockedCtx, "rb2401", cfg);
		cfg->release();
		ASSERT_TRUE(unit.initializationSucceeded());

		armDetachedTimeoutLatch(unit._orders_mon, 9401);
		unit.on_channel_ready();
		EXPECT_TRUE(lockedCtx.batchCancels.empty()); // reconciliation waits for a valid tick
		WTSTickData* tick = makeTick(93000000);
		unit.on_tick(tick);
		tick->release();
		EXPECT_TRUE(lockedCtx.batchCancels.empty());
		EXPECT_TRUE(lockedCtx.hasLog("suppressed after an unverified timeout"));
	}
	lockedCtx.comm->release();

	TempVwapCwd openCwd("future_open");
	LatchCtx openCtx;
	openCtx.comm = makeNamedFutureCommodity("N05FutureOpen");
	ASSERT_TRUE(openCwd.writeProfile("N05FutureOpen", *openCtx.sess));
	openCtx.undone = -5;
	{
		WtVWapExeUnit unit;
		WTSVariant* cfg = makeVwapConfig();
		unit.init(&openCtx, "rb2401", cfg);
		cfg->release();
		ASSERT_TRUE(unit.initializationSucceeded());

		unit.on_channel_ready();
		EXPECT_TRUE(openCtx.batchCancels.empty()); // reconciliation waits for a valid tick
		WTSTickData* tick = makeTick(93000000);
		unit.on_tick(tick);
		tick->release();
		ASSERT_EQ(openCtx.batchCancels.size(), 1u);
		EXPECT_FALSE(openCtx.batchCancels.front());
		EXPECT_TRUE(openCtx.localCancels.empty());
	}
	openCtx.comm->release();
}

TEST(n05_unmanaged_cancel_latch, stock_vwap_ready_suppresses_latched_cancel_and_keeps_unlatched_behavior)
{
	TempVwapCwd lockedCwd("stock_locked");
	LatchCtx lockedCtx;
	lockedCtx.comm = makeStockCommodity("N05StockLocked");
	ASSERT_TRUE(lockedCwd.writeProfile("N05StockLocked", *lockedCtx.sess));
	lockedCtx.undone = 500;
	{
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = makeStockVwapConfig();
		unit.init(&lockedCtx, kStockCode, cfg);
		cfg->release();
		ASSERT_TRUE(unit.initializationSucceeded());

		armDetachedTimeoutLatch(unit._orders_mon, 9501);
		unit.on_channel_ready();
		EXPECT_TRUE(lockedCtx.batchCancels.empty());
		EXPECT_TRUE(lockedCtx.hasLog("suppressed after an unverified timeout"));
	}
	lockedCtx.comm->release();

	TempVwapCwd openCwd("stock_open");
	LatchCtx openCtx;
	openCtx.comm = makeStockCommodity("N05StockOpen");
	ASSERT_TRUE(openCwd.writeProfile("N05StockOpen", *openCtx.sess));
	openCtx.undone = -500;
	{
		WtStockVWapExeUnit unit;
		WTSVariant* cfg = makeStockVwapConfig();
		unit.init(&openCtx, kStockCode, cfg);
		cfg->release();
		ASSERT_TRUE(unit.initializationSucceeded());

		unit.on_channel_ready();
		ASSERT_EQ(openCtx.batchCancels.size(), 1u);
		EXPECT_FALSE(openCtx.batchCancels.front());
		EXPECT_TRUE(openCtx.localCancels.empty());
	}
	openCtx.comm->release();
}
