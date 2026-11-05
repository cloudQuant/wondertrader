/*!
 * \file s4_ctpmini_query_status_test.cpp
 * BUG-I02-NEW-154 / K42 (slice4 final gaps): TraderCTPMini order queries report
 * COMPLETE / PARTIAL / FAILED through onRspOrdersWithStatus when the host declared
 * WT_TRADER_SPI_FEATURE_QUERY_STATUS; legacy hosts keep onRspOrders.
 * Basis: CTPMini1.5.8 ThostFtdcTraderApi.h defines no "no records" error for
 * OnRspQryOrder (empty result = last frame without rows), so any error frame is FAILED.
 * No vendor API is created (m_pUserAPI stays NULL).
 */
#include <gtest/gtest.h>

#include <cstring>
#include <cstdint>

#include "TraderCTPMini/TraderCTPMini.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/ITraderApi.h"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSTradeDef.hpp"

USING_NS_WTP;

namespace {

class S4QsBaseData : public IBaseDataMgr
{
public:
	WTSContractInfo* ct = NULL;
	WTSCommodityInfo* getCommodity(const char*) override { return NULL; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return NULL; }
	WTSContractInfo* getContract(const char* code, const char* = "", uint32_t = 0) override
	{
		return (ct != NULL && code != NULL && strcmp(code, ct->getCode()) == 0) ? ct : NULL;
	}
	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return NULL; }
	WTSSessionInfo* getSession(const char*) override { return NULL; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return NULL; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t, uint32_t, bool = false) override { return 0; }
	uint64_t getBoundaryTime(const char*, uint32_t, bool = false, bool = true) override { return 0; }
};

// Records both order-query callbacks separately.
class S4QsSink : public ITraderSpi
{
public:
	S4QsBaseData bd;
	uint32_t legacyCalls = 0;
	uint32_t statusCalls = 0;
	uint32_t lastStatus = 0xFFFFFFFFu;
	std::size_t lastSize = 0;
	bool lastNull = false;

	IBaseDataMgr* getBaseDataMgr() override { return &bd; }
	void handleEvent(WTSTraderEvent, int32_t) override {}
	void onLoginResult(bool, const char*, uint32_t) override {}
	void onRspOrders(const WTSArray*) override { ++legacyCalls; }
	void onRspOrdersWithStatus(const WTSArray* ay, uint32_t status) override
	{
		++statusCalls;
		lastStatus = status;
		lastNull = (ay == NULL);
		lastSize = ay ? ay->size() : 0;
	}
};

struct S4QsContract
{
	WTSCommodityInfo* comm;
	WTSContractInfo* ct;
	S4QsContract(const char* code, const char* exchg, const char* pid)
	{
		comm = WTSCommodityInfo::create(pid, pid, exchg, "SN001", "trdtpl", "CNY");
		comm->setVolScale(10);
		comm->setCategory(CC_Future);
		ct = WTSContractInfo::create(code, code, exchg, pid);
		ct->setCommInfo(comm);
	}
	~S4QsContract()
	{
		ct->release();
		comm->release();
	}
};

class S4MiniProbe : public TraderCTPMini
{
public:
	void setTradeDate(uint32_t d) { m_lDate = d; }
};

CThostFtdcOrderField s4MiniOrder(const char* code)
{
	CThostFtdcOrderField o;
	memset(&o, 0, sizeof(o));
	strncpy(o.InstrumentID, code, sizeof(o.InstrumentID) - 1);
	strncpy(o.ExchangeID, "SHFE", sizeof(o.ExchangeID) - 1);
	strncpy(o.InsertTime, "10:00:00", sizeof(o.InsertTime) - 1);
	strncpy(o.OrderSysID, "S1", sizeof(o.OrderSysID) - 1);
	strncpy(o.OrderRef, "1001", sizeof(o.OrderRef) - 1);
	o.TimeCondition = THOST_FTDC_TC_GFD;
	o.VolumeTotalOriginal = 1;
	o.LimitPrice = 3500.0;
	o.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
	return o;
}

} // namespace

TEST(S4CTPMiniQueryStatus, k42_order_query_reports_completeness_when_host_supports_it)
{
	S4QsContract fx("ru2601", "SHFE", "ru");
	S4QsSink sink;
	sink.bd.ct = fx.ct;
	S4MiniProbe api;
	api.registerSpi(&sink);
	api.setTradeDate(20260105);

	CThostFtdcOrderField o = s4MiniOrder("ru2601");
	CThostFtdcOrderField unknown = s4MiniOrder("zz9999");
	CThostFtdcRspInfoField ok;
	memset(&ok, 0, sizeof(ok));
	CThostFtdcRspInfoField err;
	memset(&err, 0, sizeof(err));
	err.ErrorID = 3000;

	// Legacy host: unchanged callback.
	api.OnRspQryOrder(&o, &ok, 1, true);
	EXPECT_EQ(1u, sink.legacyCalls);
	EXPECT_EQ(0u, sink.statusCalls);

	api.setSpiFeatures(WT_TRADER_SPI_FEATURE_QUERY_STATUS);
	api.OnRspQryOrder(&o, &ok, 2, true);
	ASSERT_EQ(1u, sink.statusCalls) << "a host that understands status gets the versioned callback";
	EXPECT_EQ(WT_QUERY_STATUS_COMPLETE, sink.lastStatus);
	EXPECT_EQ(1u, sink.lastSize);
	EXPECT_EQ(1u, sink.legacyCalls) << "and only that one";

	api.OnRspQryOrder(NULL, &ok, 3, true);	// "no orders": last frame without rows
	EXPECT_EQ(WT_QUERY_STATUS_COMPLETE, sink.lastStatus);
	EXPECT_FALSE(sink.lastNull) << "an empty complete snapshot is an empty array, not NULL";
	EXPECT_EQ(0u, sink.lastSize);

	api.OnRspQryOrder(&o, &ok, 4, false);
	api.OnRspQryOrder(&unknown, &ok, 4, true);
	EXPECT_EQ(WT_QUERY_STATUS_PARTIAL, sink.lastStatus) << "a dropped row makes the snapshot partial";
	EXPECT_EQ(1u, sink.lastSize);

	api.OnRspQryOrder(&o, &err, 5, false);
	api.OnRspQryOrder(&o, &ok, 5, true);
	EXPECT_EQ(WT_QUERY_STATUS_FAILED, sink.lastStatus) << "an error frame fails the whole query";

	api.OnRspQryOrder(&o, &ok, 6, true);	// the next query starts clean
	EXPECT_EQ(WT_QUERY_STATUS_COMPLETE, sink.lastStatus);
	EXPECT_EQ(1u, sink.lastSize);
	EXPECT_EQ(5u, sink.statusCalls);
	EXPECT_EQ(1u, sink.legacyCalls);

	api.registerSpi(NULL);
}
