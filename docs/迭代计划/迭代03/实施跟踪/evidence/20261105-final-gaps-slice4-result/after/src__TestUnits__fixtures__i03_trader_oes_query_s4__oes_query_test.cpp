// I03-NEW-101 (iteration 03, 2026-11-05): TraderOES::handle_rsp_orders /
// handle_rsp_trades returned early when a query row could not be converted, so a
// failing *last* row (pQryCursor->isEnd) dropped the final array callback and
// left the host's query hanging. Contract (K42 / BUG-L01-014):
//   - the final callback is always delivered once the cursor reports the end;
//   - a host that declared WT_TRADER_SPI_FEATURE_QUERY_STATUS gets the order
//     snapshot through onRspOrdersWithStatus: COMPLETE when every row converted,
//     PARTIAL when any row was dropped (the host draws no conclusion from a
//     missing order); a legacy host keeps onRspOrders;
//   - trade queries keep the legacy onRspTrades (K42 does not version them).
// Only local fake objects are used; TraderOES is never connected, no OES API
// function is called (the vendor symbols stay unresolved at link time).
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

// Everything TraderOES.h includes is included first, so the access override
// below only applies to the TraderOES class itself. The posted final callback
// runs on TraderOES' private io_context; the test drains it directly, as
// several coverage tests do for private members.
#include <atomic>
#include <stdint.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <oes_api/oes_async_api.h>
#include "Includes/ITraderApi.h"
#include "Includes/WTSCollection.hpp"
#include "Share/WtKVCache.hpp"
#include "Share/StdUtils.hpp"
#include "Share/DLLHelper.hpp"
#define private public
#include "TraderOES/TraderOES.h"
#undef private

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSTradeDef.hpp"

USING_NS_WTP;

namespace {

class OesFakeBaseData final : public IBaseDataMgr
{
public:
	OesFakeBaseData()
	{
		_comm = WTSCommodityInfo::create("STK", "stock", "SSE", "TRADING", "CHINA");
		_comm->setCategory(CC_Stock);
		_ct = WTSContractInfo::create("600000", "pufa", "SSE", "STK");
		_ct->setCommInfo(_comm);
	}
	~OesFakeBaseData()
	{
		_ct->release();
		_comm->release();
	}
	WTSCommodityInfo* getCommodity(const char*) override { return _comm; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return _comm; }
	WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t = 0) override
	{
		if (code != NULL && strcmp(code, "600000") == 0 && exchg != NULL && strcmp(exchg, "SSE") == 0)
			return _ct;
		return NULL;
	}
	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return NULL; }
	WTSSessionInfo* getSession(const char*) override { return NULL; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return NULL; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate; }
	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool = false, bool = true) override { return tDate; }

private:
	WTSCommodityInfo* _comm = NULL;
	WTSContractInfo* _ct = NULL;
};

class OesRecordingSink final : public ITraderSpi
{
public:
	IBaseDataMgr* getBaseDataMgr() override { return &bd; }
	void handleEvent(WTSTraderEvent, int32_t) override {}
	void onLoginResult(bool, const char*, uint32_t) override {}

	void onRspOrders(const WTSArray* ayOrders) override
	{
		++legacy_orders;
		last_orders_size = ayOrders ? ayOrders->size() : 0;
	}
	void onRspOrdersWithStatus(const WTSArray* ayOrders, uint32_t status) override
	{
		++status_orders;
		last_status = status;
		last_orders_size = ayOrders ? ayOrders->size() : 0;
	}
	void onRspTrades(const WTSArray* ayTrades) override
	{
		++trades_calls;
		last_trades_size = ayTrades ? ayTrades->size() : 0;
	}
	void onUnresolvedProviderReport(const char* kind, const char*, const char*) override
	{
		unresolved.push_back(kind ? kind : "");
	}

	OesFakeBaseData bd;
	uint32_t legacy_orders = 0;
	uint32_t status_orders = 0;
	uint32_t last_status = 0xFFFFFFFFu;
	std::size_t last_orders_size = 0;
	uint32_t trades_calls = 0;
	std::size_t last_trades_size = 0;
	std::vector<std::string> unresolved;
};

OesOrdItemT orderRow(const char* securityId, int32_t clSeqNo)
{
	OesOrdItemT row;
	memset(&row, 0, sizeof(row));
	row.mktId = OES_MKT_SH_ASHARE;
	snprintf(row.securityId, sizeof(row.securityId), "%s", securityId);
	row.ordQty = 100;
	row.ordPrice = 100000;
	row.bsType = OES_BS_TYPE_BUY;
	row.clSeqNo = clSeqNo;
	row.ordDate = 20250110;
	row.ordTime = 93000000;
	return row;
}

OesTrdItemT tradeRow(const char* securityId)
{
	OesTrdItemT row;
	memset(&row, 0, sizeof(row));
	row.mktId = OES_MKT_SH_ASHARE;
	snprintf(row.securityId, sizeof(row.securityId), "%s", securityId);
	row.trdQty = 100;
	row.trdPrice = 100000;
	row.trdDate = 20250110;
	row.trdTime = 93000000;
	row.trdSide = OES_BS_TYPE_BUY;
	return row;
}

OesQryCursorT cursor(bool isEnd)
{
	OesQryCursorT c;
	memset(&c, 0, sizeof(c));
	c.isEnd = isEnd ? 1 : 0;
	return c;
}

class OesQueryFixture : public ::testing::Test
{
protected:
	void SetUp() override { trader.registerSpi(&sink); }

	// Runs the callbacks TraderOES posted to its (not started) io_context.
	void drain()
	{
		trader._asyncio.restart();
		trader._asyncio.run();
	}

	TraderOES trader;
	OesRecordingSink sink;
	SMsgHeadT head{};
};

} // namespace

TEST_F(OesQueryFixture, FailingLastOrderRowStillDeliversTheFinalCallback)
{
	OesOrdItemT good = orderRow("600000", 1);
	OesOrdItemT bad = orderRow("999999", 2);	// unknown to the base data
	OesQryCursorT more = cursor(false);
	OesQryCursorT end = cursor(true);
	trader.handle_rsp_orders(&head, &good, &more);
	trader.handle_rsp_orders(&head, &bad, &end);
	drain();
	EXPECT_EQ(1u, sink.legacy_orders) << "the final array callback must not be lost";
	EXPECT_EQ(1u, sink.last_orders_size);
	EXPECT_EQ(0u, sink.status_orders);
}

TEST_F(OesQueryFixture, StatusHostLearnsPartialSnapshotWhenARowWasDropped)
{
	trader.setSpiFeatures(WT_TRADER_SPI_FEATURE_QUERY_STATUS | WT_TRADER_SPI_FEATURE_RAW_UNRESOLVED);
	OesOrdItemT good = orderRow("600000", 1);
	OesOrdItemT bad = orderRow("999999", 2);
	OesQryCursorT more = cursor(false);
	OesQryCursorT end = cursor(true);
	trader.handle_rsp_orders(&head, &good, &more);
	trader.handle_rsp_orders(&head, &bad, &end);
	drain();
	ASSERT_EQ(1u, sink.status_orders);
	EXPECT_EQ(WT_QUERY_STATUS_PARTIAL, sink.last_status);
	EXPECT_EQ(1u, sink.last_orders_size);
	EXPECT_EQ(0u, sink.legacy_orders) << "a status host gets only the versioned callback";
	ASSERT_EQ(1u, sink.unresolved.size());
	EXPECT_EQ("order", sink.unresolved[0]);

	// The next query starts clean and is complete.
	trader.handle_rsp_orders(&head, &good, &end);
	drain();
	ASSERT_EQ(2u, sink.status_orders);
	EXPECT_EQ(WT_QUERY_STATUS_COMPLETE, sink.last_status);
	EXPECT_EQ(1u, sink.last_orders_size);
}

TEST_F(OesQueryFixture, FailingLastTradeRowStillDeliversTheFinalCallback)
{
	OesTrdItemT good = tradeRow("600000");
	OesTrdItemT bad = tradeRow("999999");
	OesQryCursorT more = cursor(false);
	OesQryCursorT end = cursor(true);
	trader.handle_rsp_trades(&head, &good, &more);
	trader.handle_rsp_trades(&head, &bad, &end);
	drain();
	EXPECT_EQ(1u, sink.trades_calls) << "the final trade array callback must not be lost";
	EXPECT_EQ(1u, sink.last_trades_size);
}

TEST_F(OesQueryFixture, OnlyFailingRowsStillEndTheQuery)
{
	trader.setSpiFeatures(WT_TRADER_SPI_FEATURE_QUERY_STATUS);
	OesOrdItemT bad = orderRow("999999", 3);
	OesQryCursorT end = cursor(true);
	trader.handle_rsp_orders(&head, &bad, &end);
	drain();
	ASSERT_EQ(1u, sink.status_orders);
	EXPECT_EQ(WT_QUERY_STATUS_PARTIAL, sink.last_status);
	EXPECT_EQ(0u, sink.last_orders_size);
}
