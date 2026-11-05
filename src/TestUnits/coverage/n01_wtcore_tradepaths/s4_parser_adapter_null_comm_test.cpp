/**
 * I03-NEW-087 (iteration 03, 2026-11-05): WtCore ParserAdapter dereferenced
 * WTSContractInfo::getCommInfo() without a null check in handleQuote and in the
 * three level2 handlers. A contract without commodity metadata must be dropped
 * (no delivery to the stub, no crash); an ordinary contract is still delivered.
 * Each case runs in its own process in the regression tooling; before the fix
 * the null-commodity cases terminate with SIGSEGV.
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include <WtCore/ParserAdapter.h>
#include <Includes/IBaseDataMgr.h>
#include <Includes/IParserApi.h>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSCollection.hpp>

USING_NS_WTP;

namespace {

class S4NullCommBaseDataMgr : public IBaseDataMgr
{
public:
	~S4NullCommBaseDataMgr()
	{
		for (WTSContractInfo* ct : _contracts)
			ct->release();
		for (WTSCommodityInfo* comm : _comms)
			comm->release();
	}

	// Registers a contract; comm may be NULL to model missing commodity metadata.
	void add(WTSContractInfo* ct, WTSCommodityInfo* comm)
	{
		if (comm != NULL)
		{
			ct->setCommInfo(comm);
			_comms.push_back(comm);
		}
		_contracts.push_back(ct);
	}

	WTSCommodityInfo* getCommodity(const char*) override { return NULL; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return NULL; }
	WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t = 0) override
	{
		for (WTSContractInfo* ct : _contracts)
		{
			if (strcmp(ct->getCode(), code) == 0 && (exchg == NULL || exchg[0] == '\0' || strcmp(ct->getExchg(), exchg) == 0))
				return ct;
		}
		return NULL;
	}
	WTSArray* getContracts(const char* = "", uint32_t = 0) override
	{
		WTSArray* ay = WTSArray::create();
		for (WTSContractInfo* ct : _contracts)
			ay->append(ct, true);
		return ay;
	}
	WTSSessionInfo* getSession(const char*) override { return NULL; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return NULL; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate; }
	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool = false, bool = true) override { return tDate; }

private:
	std::vector<WTSContractInfo*> _contracts;
	std::vector<WTSCommodityInfo*> _comms;
};

class S4CountingStub : public IParserStub
{
public:
	void handle_push_quote(WTSTickData*) override { ++quotes; }
	void handle_push_order_queue(WTSOrdQueData*) override { ++queues; }
	void handle_push_order_detail(WTSOrdDtlData*) override { ++details; }
	void handle_push_transaction(WTSTransData*) override { ++trans; }

	uint32_t quotes = 0;
	uint32_t queues = 0;
	uint32_t details = 0;
	uint32_t trans = 0;
};

class S4SpiCapturingApi : public IParserApi
{
public:
	bool init(WTSVariant*) override { return true; }
	void registerSpi(IParserSpi* spi) override { spi_ = spi; }
	IParserSpi* spi_ = NULL;
};

class S4ParserAdapterNullComm : public ::testing::Test
{
protected:
	void SetUp() override
	{
		// Contract registered without commodity metadata (getCommInfo() == NULL).
		_bd.add(WTSContractInfo::create("600999", "orphan", "SSE", "ORPH"), NULL);
		// Ordinary positive-control contract.
		WTSCommodityInfo* comm = WTSCommodityInfo::create("STK", "stock", "SSE", "TRADING", "CHINA");
		comm->setCategory(CC_Stock);
		_bd.add(WTSContractInfo::create("600000", "pufa", "SSE", "STK"), comm);
		_api = new S4SpiCapturingApi();
		ASSERT_TRUE(_adapter.initExt("s4_null_comm", _api, &_stub, &_bd));
		ASSERT_NE(nullptr, _api->spi_);
	}

	void TearDown() override { _adapter.release(); }

	WTSTickData* tick(const char* code)
	{
		WTSTickStruct st;
		memset(&st, 0, sizeof(st));
		strcpy(st.exchg, "SSE");
		strcpy(st.code, code);
		st.action_date = 20250109;
		st.trading_date = 20250109;
		st.action_time = 93000000;
		st.price = 7.2;
		return WTSTickData::create(st);
	}

	template <typename Struct>
	static void stamp(Struct& st)
	{
		strcpy(st.exchg, "SSE");
		st.trading_date = 20250109;
		st.action_date = 20250109;
		st.action_time = 93000000;
	}

	WTSOrdQueData* queue(const char* code)
	{
		WTSOrdQueData* data = WTSOrdQueData::create(code);
		if (data != NULL)
			stamp(data->getOrdQueStruct());
		return data;
	}

	WTSOrdDtlData* detail(const char* code)
	{
		WTSOrdDtlData* data = WTSOrdDtlData::create(code);
		if (data != NULL)
			stamp(data->getOrdDtlStruct());
		return data;
	}

	WTSTransData* transaction(const char* code)
	{
		WTSTransData* data = WTSTransData::create(code);
		if (data != NULL)
			stamp(data->getTransStruct());
		return data;
	}

	S4NullCommBaseDataMgr _bd;
	S4CountingStub _stub;
	S4SpiCapturingApi* _api = NULL;
	ParserAdapter _adapter;
};

} // namespace

TEST_F(S4ParserAdapterNullComm, QuoteForContractWithoutCommodityIsDroppedWithoutCrash)
{
	WTSTickData* t = tick("600999");
	ASSERT_NE(nullptr, t);
	_api->spi_->handleQuote(t, 0);
	EXPECT_EQ(0u, _stub.quotes);
	t->release();

	WTSTickData* ok = tick("600000");
	ASSERT_NE(nullptr, ok);
	_api->spi_->handleQuote(ok, 0);
	EXPECT_EQ(1u, _stub.quotes) << "ordinary contract is still delivered";
	ok->release();
}

TEST_F(S4ParserAdapterNullComm, OrderQueueForContractWithoutCommodityIsDroppedWithoutCrash)
{
	WTSOrdQueData* q = queue("600999");
	ASSERT_NE(nullptr, q);
	_api->spi_->handleOrderQueue(q);
	EXPECT_EQ(0u, _stub.queues);
	q->release();

	WTSOrdQueData* ok = queue("600000");
	ASSERT_NE(nullptr, ok);
	_api->spi_->handleOrderQueue(ok);
	EXPECT_EQ(1u, _stub.queues);
	EXPECT_STREQ("SSE.STK.600000", ok->code());
	ok->release();
}

TEST_F(S4ParserAdapterNullComm, OrderDetailForContractWithoutCommodityIsDroppedWithoutCrash)
{
	WTSOrdDtlData* d = detail("600999");
	ASSERT_NE(nullptr, d);
	_api->spi_->handleOrderDetail(d);
	EXPECT_EQ(0u, _stub.details);
	d->release();

	WTSOrdDtlData* ok = detail("600000");
	ASSERT_NE(nullptr, ok);
	_api->spi_->handleOrderDetail(ok);
	EXPECT_EQ(1u, _stub.details);
	ok->release();
}

TEST_F(S4ParserAdapterNullComm, TransactionForContractWithoutCommodityIsDroppedWithoutCrash)
{
	WTSTransData* tr = transaction("600999");
	ASSERT_NE(nullptr, tr);
	_api->spi_->handleTransaction(tr);
	EXPECT_EQ(0u, _stub.trans);
	tr->release();

	WTSTransData* ok = transaction("600000");
	ASSERT_NE(nullptr, ok);
	_api->spi_->handleTransaction(ok);
	EXPECT_EQ(1u, _stub.trans);
	ok->release();
}
