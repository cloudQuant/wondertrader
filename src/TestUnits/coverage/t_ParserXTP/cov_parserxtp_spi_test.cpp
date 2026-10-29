/*
 * 覆盖率用例：src/ParserXTP（lane T1 / 第六波·收发族）
 * 形状参照 t_TraderMocker/seam_tradermocker_test.cpp：直接构造 adapter、走公开接口、
 * 用自造 fake IParserSpi / IBaseDataMgr 驱动厂商回调结构体。
 * 注意：ParserXTP::init()/connect() 依赖真实 xtpquoteapi 动态库与不可停止的 worker 线程，
 * 在无库环境下必崩，见 docs/迭代计划/迭代01/bugs/T1_ctpopt_parserxtp.md，对应用例以 DISABLED_ 保留。
 */
#include <gtest/gtest.h>

#include <cfloat>
#include <cstring>
#include <string>
#include <vector>

#include "ParserXTP/ParserXTP.h"

#include <WTSDataDef.hpp>
#include <WTSContractInfo.hpp>
#include <WTSVariant.hpp>
#include <IBaseDataMgr.h>

USING_NS_WTP;

/* ParserXTP.cpp 中的文件级 inline 函数（外部链接、弱符号），可直接跨 TU 声明调用 */
uint32_t strToTime(const char* strTime);
double checkValid(double val);

namespace
{
	class FakeParserSpi : public IParserSpi
	{
	public:
		struct QueueCapture
		{
			WTSOrdQueStruct os{};
			std::string exchg;
		};

		void handleEvent(WTSParserEvent e, int32_t ec) override { events.emplace_back(e, ec); }
		void handleSymbolList(const WTSArray*) override { symbolListCalls++; }
		void handleQuote(WTSTickData* quote, uint32_t procFlag) override
		{
			quoteCalls++;
			quoteProcFlag = procFlag;
			tick = quote->getTickStruct();
		}
		void handleOrderQueue(WTSOrdQueData* ordQueData) override
		{
			QueueCapture cap;
			cap.os = ordQueData->getOrdQueStruct();
			cap.exchg = cap.os.exchg;
			queues.push_back(cap);
		}
		void handleOrderDetail(WTSOrdDtlData* ordDtlData) override
		{
			ordDtlCalls++;
			ordDtl = ordDtlData->getOrdDtlStruct();
		}
		void handleTransaction(WTSTransData* transData) override
		{
			transCalls++;
			trans = transData->getTransStruct();
		}
		void handleParserLog(WTSLogLevel ll, const char* message) override
		{
			logs.emplace_back(ll, message);
		}
		IBaseDataMgr* getBaseDataMgr() override
		{
			bdMgrQueryCalls++;
			return bdMgr;
		}

		bool hasLog(const char* needle) const
		{
			for (const auto& lg : logs)
				if (lg.second.find(needle) != std::string::npos)
					return true;
			return false;
		}

		std::vector<std::pair<WTSParserEvent, int32_t>> events;
		std::vector<std::pair<WTSLogLevel, std::string>> logs;
		std::vector<QueueCapture> queues;
		WTSTickStruct tick{};
		WTSOrdDtlStruct ordDtl{};
		WTSTransStruct trans{};
		IBaseDataMgr* bdMgr = nullptr;
		uint32_t quoteCalls = 0;
		uint32_t symbolListCalls = 0;
		uint32_t ordDtlCalls = 0;
		uint32_t transCalls = 0;
		uint32_t bdMgrQueryCalls = 0;
		uint32_t quoteProcFlag = 0;
	};

	class FakeBaseDataMgr : public IBaseDataMgr
	{
	public:
		WTSCommodityInfo* getCommodity(const char*) override { return nullptr; }
		WTSCommodityInfo* getCommodity(const char*, const char*) override { return nullptr; }
		WTSContractInfo* getContract(const char* code, const char* /*exchg*/ = "", uint32_t /*uDate*/ = 0) override
		{
			getContractCalls++;
			lastQueried = code;
			if (contract == nullptr || rejectAll)
				return nullptr;
			if (std::strcmp(code, contract->getCode()) != 0)
				return nullptr;
			return contract;
		}
		WTSArray* getContracts(const char* = "", uint32_t = 0) override { return nullptr; }
		WTSSessionInfo* getSession(const char*) override { return nullptr; }
		WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return nullptr; }
		WTSArray* getAllSessions() override { return nullptr; }
		bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
		uint32_t calcTradingDate(const char*, uint32_t, uint32_t, bool = false) override { return 0; }
		uint64_t getBoundaryTime(const char*, uint32_t, bool = false, bool = true) override { return 0; }

		WTSContractInfo* contract = nullptr;
		bool rejectAll = false;
		uint32_t getContractCalls = 0;
		std::string lastQueried;
	};

	/* 由 ParserXTP.cpp 的 extern "C" 导出，供 createParser/deleteParser 用例声明 */
	extern "C" IParserApi* createParser();
	extern "C" void deleteParser(IParserApi*& parser);

	/* 组装一个可被 OnDepthMarketData/OnTickByTick 消化的合约（螺纹钢期货样例） */
	struct FixtureContract
	{
		FixtureContract()
		{
			comm = WTSCommodityInfo::create("rb", "螺纹钢", "SSE", "SY241", "trading");
			comm->setVolScale(10);
			comm->setCategory(CC_Future);
			comm->setCoverMode(CM_CoverToday);
			ct = WTSContractInfo::create("600519", "贵州茅台", "SSE", "rb");
			ct->setCommInfo(comm);
		}
		~FixtureContract()
		{
			if (ct != nullptr) ct->release();
			if (comm != nullptr) comm->release();
		}
		WTSCommodityInfo* comm = nullptr;
		WTSContractInfo* ct = nullptr;
	};

	XTPST makeTicker(XTP_EXCHANGE_TYPE exchg, const char* code)
	{
		XTPST st;
		std::memset(&st, 0, sizeof(st));
		st.exchange_id = exchg;
		std::snprintf(st.ticker, sizeof(st.ticker), "%s", code);
		return st;
	}
}

TEST(ParserXTP, factory_entries_create_and_destroy_parser)
{
	IParserApi* parser = createParser();
	ASSERT_NE(nullptr, parser);
	EXPECT_NE(nullptr, dynamic_cast<ParserXTP*>(parser));

	parser->registerSpi(nullptr);
	deleteParser(parser);
	EXPECT_EQ(nullptr, parser);

	/* 幂等：NULL 引用不得崩溃 */
	IParserApi* nullParser = nullptr;
	deleteParser(nullParser);
	EXPECT_EQ(nullptr, nullParser);
}

TEST(ParserXTP, state_queries_before_init_answer_deterministically)
{
	ParserXTP parser;
	FakeParserSpi spi;
	parser.registerSpi(&spi);

	/* 未 init（无 api 指针）：isConnected=false、disconnect/release 均安全返回 */
	EXPECT_FALSE(parser.isConnected());
	EXPECT_TRUE(parser.disconnect());
	EXPECT_NO_THROW(parser.release());
	EXPECT_TRUE(parser.disconnect());

	/* registerSpi 会把 sink 与 sink->getBaseDataMgr 接线过来 */
	EXPECT_EQ(1, spi.bdMgrQueryCalls);
	EXPECT_EQ(nullptr, spi.bdMgr);

	parser.registerSpi(nullptr);
}

TEST(ParserXTP, register_spi_accepts_null_sink_without_touching_callbacks)
{
	ParserXTP parser;
	FakeParserSpi spi;
	spi.bdMgr = nullptr;
	parser.registerSpi(&spi);
	parser.registerSpi(nullptr);

	/* m_sink 置空后 OnError 不再 dereference sink：无任何日志/事件产生 */
	XTPRI err;
	std::memset(&err, 0, sizeof(err));
	err.error_id = 404;
	std::snprintf(err.error_msg, sizeof(err.error_msg), "%s", "boom");
	EXPECT_NO_THROW(parser.OnError(&err));
	EXPECT_TRUE(spi.logs.empty());
	EXPECT_TRUE(spi.events.empty());
}

TEST(ParserXTP, on_error_filters_zero_error_id_and_logs_nonzero)
{
	ParserXTP parser;
	FakeParserSpi spi;
	parser.registerSpi(&spi);

	XTPRI ok;
	std::memset(&ok, 0, sizeof(ok));
	ok.error_id = 0;
	parser.OnError(&ok);
	EXPECT_TRUE(spi.logs.empty());

	XTPRI err;
	std::memset(&err, 0, sizeof(err));
	err.error_id = 1001;
	std::snprintf(err.error_msg, sizeof(err.error_msg), "%s", "md channel broken");
	parser.OnError(&err);
	ASSERT_EQ(1u, spi.logs.size());
	EXPECT_EQ(LL_ERROR, spi.logs[0].first);
	EXPECT_TRUE(spi.hasLog("[ParserXTP] Error occured: (1001)md channel broken"));

	parser.registerSpi(nullptr);
}

TEST(ParserXTP, unsubscribe_marketdata_callback_is_documented_noop)
{
	ParserXTP parser;
	FakeParserSpi spi;
	parser.registerSpi(&spi);

	XTPST st = makeTicker(XTP_EXCHANGE_SH, "600519");
	XTPRI err;
	std::memset(&err, 0, sizeof(err));
	err.error_id = 7;
	EXPECT_NO_THROW(parser.OnUnSubMarketData(&st, &err, true));
	EXPECT_TRUE(spi.logs.empty());
	EXPECT_TRUE(spi.events.empty());
	EXPECT_EQ(0u, spi.quoteCalls);

	parser.registerSpi(nullptr);
}

TEST(ParserXTP, sub_marketdata_error_path_formats_exchange_and_ticker)
{
	ParserXTP parser;
	FakeParserSpi spi;
	parser.registerSpi(&spi);

	XTPRI err;
	std::memset(&err, 0, sizeof(err));
	err.error_id = 21;
	std::snprintf(err.error_msg, sizeof(err.error_msg), "%s", "no quota");

	XTPST sh = makeTicker(XTP_EXCHANGE_SH, "600519");
	parser.OnSubMarketData(&sh, &err, true);
	ASSERT_EQ(1u, spi.logs.size());
	EXPECT_TRUE(spi.hasLog("SSE.600519"));
	EXPECT_TRUE(spi.hasLog("err code: 21"));
	EXPECT_TRUE(spi.hasLog("Market data subscribe failed"));

	XTPST sz = makeTicker(XTP_EXCHANGE_SZ, "000001");
	parser.OnSubTickByTick(&sz, &err, true);
	ASSERT_EQ(2u, spi.logs.size());
	EXPECT_TRUE(spi.hasLog("SZSE.000001"));
	EXPECT_TRUE(spi.hasLog("Tick-by-tick data subscribe failed"));

	/* 无错误时两条订阅回调都不发日志 */
	XTPRI ok;
	std::memset(&ok, 0, sizeof(ok));
	parser.OnSubMarketData(&sh, &ok, true);
	parser.OnSubTickByTick(&sh, &ok, true);
	EXPECT_EQ(2u, spi.logs.size());

	parser.registerSpi(nullptr);
}

TEST(ParserXTP, on_disconnected_delivers_close_event_without_running_relogin_post)
{
	ParserXTP parser;
	FakeParserSpi spi;
	parser.registerSpi(&spi);

	parser.OnDisconnected(0x2001);
	/* 同步路径：日志 + WPE_Close；重登录 posted 到 _asyncio，无 worker 线程时不得执行 */
	ASSERT_EQ(1u, spi.events.size());
	EXPECT_EQ(WPE_Close, spi.events[0].first);
	EXPECT_EQ(0, spi.events[0].second);
	EXPECT_TRUE(spi.hasLog("Market data server disconnected"));
	EXPECT_FALSE(spi.hasLog("relogin in 2 seconds"));

	parser.registerSpi(nullptr);
}

TEST(ParserXTP, depth_market_data_without_basedata_or_contract_bails_out)
{
	ParserXTP parser;
	FakeParserSpi spiNoMgr;
	parser.registerSpi(&spiNoMgr); /* getBaseDataMgr()==nullptr */

	XTPMD md;
	std::memset(&md, 0, sizeof(md));
	md.exchange_id = XTP_EXCHANGE_SH;
	std::snprintf(md.ticker, sizeof(md.ticker), "%s", "600519");
	md.data_time = 20260105093000123LL;
	double noPrice = 0;
	int64_t noQty = 0;
	int32_t zeroCount = 0;
	/* m_pBaseDataMgr==NULL：L287 早退，不产行情、不打日志 */
	parser.OnDepthMarketData(&md, &noQty, zeroCount, zeroCount, &noQty, zeroCount, zeroCount);
	(void)noPrice;
	EXPECT_EQ(0u, spiNoMgr.quoteCalls);
	EXPECT_TRUE(spiNoMgr.logs.empty());

	FakeBaseDataMgr bd;			/* contract==nullptr → getContract 返回 NULL */
	FakeParserSpi spi;
	spi.bdMgr = &bd;
	parser.registerSpi(&spi);

	parser.OnDepthMarketData(&md, &noQty, zeroCount, zeroCount, &noQty, zeroCount, zeroCount);
	EXPECT_EQ(0u, spi.quoteCalls);
	EXPECT_EQ(1u, bd.getContractCalls);
	EXPECT_EQ("600519", bd.lastQueried);
	ASSERT_EQ(1u, spi.logs.size());
	EXPECT_TRUE(spi.hasLog("Instrument SSE.600519 not exists..."));

	parser.registerSpi(nullptr);
}

TEST(ParserXTP, depth_market_data_converts_snapshot_and_level1_queues)
{
	ParserXTP parser;
	FakeBaseDataMgr bd;
	FixtureContract fc;
	bd.contract = fc.ct;
	FakeParserSpi spi;
	spi.bdMgr = &bd;
	parser.registerSpi(&spi);

	XTPMD md;
	std::memset(&md, 0, sizeof(md));
	md.exchange_id = XTP_EXCHANGE_SZ;	/* 非 SH → exchg 走 "SZSE" 分支 */
	std::snprintf(md.ticker, sizeof(md.ticker), "%s", "600519");
	md.data_time = 20260105093000123LL;
	md.last_price = DBL_MAX;			/* checkValid → 0 */
	md.open_price = 12.5;
	md.high_price = 13.0;
	md.low_price = FLT_MAX;				/* checkValid → 0 */
	md.pre_close_price = 12.0;
	md.qty = 12345;
	md.turnover = 154321.25;
	md.settl_price = 99.5;
	md.pre_settl_price = DBL_MAX;		/* checkValid → 0 */
	md.total_long_positon = 777;
	md.pre_total_long_positon = 666;
	md.upper_limit_price = 13.2;
	md.lower_limit_price = 10.8;
	for (int i = 0; i < 10; i++)
	{
		md.bid[i] = 12.0 + i;
		md.ask[i] = 12.5 + i;
		md.bid_qty[i] = 100 + i;
		md.ask_qty[i] = 200 + i;
	}

	int64_t bid1_qty[3] = { 11, 22, 33 };
	int64_t ask1_qty[2] = { 44, 55 };

	parser.OnDepthMarketData(&md, bid1_qty, 3, 100, ask1_qty, 2, 200);

	/* 快照转换 */
	ASSERT_EQ(1u, spi.quoteCalls);
	EXPECT_EQ(1u, spi.quoteProcFlag);
	EXPECT_EQ(20260105u, spi.tick.action_date);
	EXPECT_EQ(93000123u, spi.tick.action_time);
	EXPECT_DOUBLE_EQ(0.0, spi.tick.price);
	EXPECT_DOUBLE_EQ(12.5, spi.tick.open);
	EXPECT_DOUBLE_EQ(13.0, spi.tick.high);
	EXPECT_DOUBLE_EQ(0.0, spi.tick.low);
	EXPECT_DOUBLE_EQ(12.0, spi.tick.pre_close);
	EXPECT_DOUBLE_EQ(13.2, spi.tick.upper_limit);
	EXPECT_DOUBLE_EQ(10.8, spi.tick.lower_limit);
	EXPECT_EQ(12345u, spi.tick.total_volume);
	EXPECT_DOUBLE_EQ(154321.25, spi.tick.total_turnover);
	/* 期货品类（CC_Future）才写结算价/持仓 */
	EXPECT_DOUBLE_EQ(99.5, spi.tick.settle_price);
	EXPECT_EQ(777u, spi.tick.open_interest);
	EXPECT_DOUBLE_EQ(0.0, spi.tick.pre_settle);
	EXPECT_EQ(666u, spi.tick.pre_interest);
	EXPECT_EQ(0u, spi.tick.trading_date);	/* 未登录 → m_uTradingDate 保持 0 */
	EXPECT_DOUBLE_EQ(12.0, spi.tick.bid_prices[0]);
	EXPECT_EQ(100u, spi.tick.bid_qty[0]);
	EXPECT_DOUBLE_EQ(21.0, spi.tick.bid_prices[9]);
	EXPECT_DOUBLE_EQ(12.5, spi.tick.ask_prices[0]);
	EXPECT_EQ(209u, spi.tick.ask_qty[9]);
	EXPECT_STREQ("SSE", spi.tick.exchg);

	/* 委托队列：买盘 3 笔 + 卖盘 2 笔 */
	ASSERT_EQ(2u, spi.queues.size());
	const auto& buy = spi.queues[0].os;
	EXPECT_EQ(BDT_Buy, buy.side);
	EXPECT_EQ(3u, buy.qsize);
	EXPECT_EQ(100u, buy.order_items);
	EXPECT_DOUBLE_EQ(12.0, buy.price);
	EXPECT_EQ(11u, buy.volumes[0]);
	EXPECT_EQ(33u, buy.volumes[2]);
	EXPECT_EQ(20260105u, buy.action_date);
	const auto& sell = spi.queues[1].os;
	EXPECT_EQ(BDT_Sell, sell.side);
	EXPECT_EQ(2u, sell.qsize);
	EXPECT_EQ(200u, sell.order_items);
	EXPECT_DOUBLE_EQ(12.5, sell.price);
	EXPECT_EQ(55u, sell.volumes[1]);

	parser.registerSpi(nullptr);
}

TEST(ParserXTP, tickbytick_converts_entrust_and_trade_branches)
{
	ParserXTP parser;
	FakeBaseDataMgr bd;
	FixtureContract fc;
	bd.contract = fc.ct;
	FakeParserSpi spi;
	spi.bdMgr = &bd;
	parser.registerSpi(&spi);

	XTPTBT tbt;
	std::memset(&tbt, 0, sizeof(tbt));
	tbt.exchange_id = XTP_EXCHANGE_SH;
	std::snprintf(tbt.ticker, sizeof(tbt.ticker), "%s", "600519");
	tbt.data_time = 20260105145930250LL;
	tbt.type = XTP_TBT_ENTRUST;
	tbt.entrust.seq = 987654;
	tbt.entrust.price = 1700.5;
	tbt.entrust.qty = 300;
	tbt.entrust.side = 'B';
	tbt.entrust.ord_type = 'A';

	parser.OnTickByTick(&tbt);
	ASSERT_EQ(1u, spi.ordDtlCalls);
	EXPECT_EQ(20260105u, spi.ordDtl.action_date);
	EXPECT_EQ(145930250u, spi.ordDtl.action_time);
	EXPECT_EQ(987654u, spi.ordDtl.index);
	EXPECT_EQ(BDT_Buy, spi.ordDtl.side);
	EXPECT_DOUBLE_EQ(1700.5, spi.ordDtl.price);
	EXPECT_EQ(300u, spi.ordDtl.volume);
	EXPECT_EQ(0u, spi.transCalls);

	tbt.type = XTP_TBT_TRADE;
	tbt.trade.seq = 555;
	tbt.trade.price = 1701.0;
	tbt.trade.qty = 100;
	tbt.trade.trade_flag = '4';		/* 撤单标识 */
	tbt.trade.bid_no = 12;
	tbt.trade.ask_no = 34;

	parser.OnTickByTick(&tbt);
	ASSERT_EQ(1u, spi.transCalls);
	EXPECT_EQ(555u, spi.trans.index);
	EXPECT_EQ(TT_Cancel, spi.trans.ttype);
	EXPECT_DOUBLE_EQ(1701.0, spi.trans.price);
	EXPECT_EQ(100u, spi.trans.volume);
	EXPECT_EQ(12u, spi.trans.bidorder);
	EXPECT_EQ(34u, spi.trans.askorder);

	/* 未知合约：只打日志，不产逐笔 */
	std::snprintf(tbt.ticker, sizeof(tbt.ticker), "%s", "999999");
	tbt.type = XTP_TBT_ENTRUST;
	parser.OnTickByTick(&tbt);
	EXPECT_EQ(1u, spi.ordDtlCalls);
	EXPECT_TRUE(spi.hasLog("Instrument SSE.999999 not exists..."));

	/* 未注册有效 basedata 时早退：registerSpi(&spi2) 把 m_pBaseDataMgr 刷回 nullptr */
	parser.registerSpi(nullptr);
	FakeParserSpi spi2;
	spi2.bdMgr = nullptr;
	parser.registerSpi(&spi2);
	EXPECT_NO_THROW(parser.OnTickByTick(&tbt));
	EXPECT_EQ(0u, spi2.ordDtlCalls);
	EXPECT_EQ(0u, spi2.transCalls);
	EXPECT_TRUE(spi2.logs.empty());

	parser.registerSpi(nullptr);
}

TEST(ParserXTP, subscribe_before_login_only_buffers_without_api_calls)
{
	ParserXTP parser;
	FakeParserSpi spi;
	parser.registerSpi(&spi);

	/* 未登录（m_uTradingDate==0）：仅缓存 SSE./SZSE. 前缀代码，不应触发任何 sink 交互 */
	CodeSet codes;
	codes.insert("SSE.600519");
	codes.insert("SZSE.000001");
	codes.insert("HK.00700");
	codes.insert("garbage_without_dot");
	EXPECT_NO_THROW(parser.subscribe(codes));
	EXPECT_TRUE(spi.logs.empty());
	EXPECT_EQ(0u, spi.quoteCalls);
	EXPECT_TRUE(spi.events.empty());

	EXPECT_NO_THROW(parser.unsubscribe(codes));
	EXPECT_TRUE(spi.logs.empty());

	parser.registerSpi(nullptr);
}

TEST(ParserXTP, time_and_price_helpers_strip_colons_and_guard_sentinels)
{
	EXPECT_EQ(93001u, strToTime("09:30:01"));
	EXPECT_EQ(145959999u, strToTime("14:59:59.999"));
	EXPECT_EQ(0u, strToTime(""));
	EXPECT_EQ(120000u, strToTime("120000"));

	EXPECT_DOUBLE_EQ(0.0, checkValid(DBL_MAX));
	EXPECT_DOUBLE_EQ(0.0, checkValid(FLT_MAX));
	EXPECT_DOUBLE_EQ(3.5, checkValid(3.5));
	EXPECT_DOUBLE_EQ(0.0, checkValid(0.0));
}

/*
 * 触发条件见 bug 文档：ParserXTP::init 会 dlopen 厂商库并无判空地调用 m_funcCreator，
 * 无 xtpquoteapi 库的 CI 环境里直接崩；connect() 又会拉起 while(true) 不可停止线程，
 * release/disconnect 不回收它，进程无法退出。整条用例保持 DISABLED_。
 */
TEST(DISABLED_ParserXTP, init_and_connect_require_vendor_lib_and_leak_thread)
{
	ParserXTP parser;
	WTSVariant* cfg = WTSVariant::createObject();
	ASSERT_NE(nullptr, cfg);
	parser.init(cfg);
	EXPECT_TRUE(parser.connect());
	parser.release();
}
