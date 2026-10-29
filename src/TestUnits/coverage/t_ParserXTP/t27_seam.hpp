/*
 * lane R27 共用夹具（只被本目录的 t27 用例包含，不进任何构建表）
 *
 * 为什么需要它：`src/ParserXTP/ParserXTP.cpp` 里剩下的 0% 行全部挂在
 * `m_pUserAPI`（私有、且只能由 `init()` 从 dlopen 的厂商库拿到）与
 * `m_uTradingDate`（只能由 `DoLogin()` 置位）上：
 *   - 本机没有 `libxtpquoteapi.dylib`（`src/API/XTP2.2.32.2/` 只有 linux/x64/x86 三份，
 *     macOS 一份都没有）→ `DLLHelper::load_library` 返回 NULL → `get_symbol` 返回 NULL →
 *     `ParserXTP.cpp:131` 直接调用空函数指针崩（T1-XTP-01，已登记）。
 *     所以 `init()` 本体不可覆盖，见 `t27_parserxtp_api_seam_test.cpp` 的 DISABLED_ 说明。
 *   - 但 `init()` 唯一的作用就是「把 api 指针与 8 个配置字段灌进对象」。这两件事本夹具
 *     用一个只读自 `ParserXTP.h` 的**布局镜像**直接灌进去，再用公开入口
 *     （`connect/subscribe/disconnect/isConnected/OnXxx`）驱动产品代码，
 *     `Login/SubscribeMarketData/GetTradingDay` 等厂商调用落到测试自己的 fake 对象上。
 *     不碰真实柜台、不占端口、不起线程、不 sleep 等回调。
 *   - 不用 `#define private public`，不改产品代码目录，镜像只是同一份声明的第二次 transcription。
 *
 * 镜像正确性不靠「相信」：`XtpSeamF::SetUp()` 在写任何东西之前先用 4 组可观测锚点
 * （`m_sink`/`m_pBaseDataMgr` 由公开 `registerSpi` 写入、`m_uTradingDate`/`m_iRequestID`
 * 由构造函数钉 0、`_thrd_worker`/`_worker` 是默认空的 shared_ptr、`m_fitSHSubs/m_fitSZSubs`
 * 由公开 `subscribe()` 填充）逐槽校验，任一对不上就 ASSERT 失败、用例立刻红，不会静默写坏内存。
 */
#pragma once

#include <gtest/gtest.h>

#include <cfloat>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "ParserXTP/ParserXTP.h"

#include <WTSDataDef.hpp>
#include <WTSContractInfo.hpp>
#include <WTSVariant.hpp>
#include <IBaseDataMgr.h>

USING_NS_WTP;

namespace t27
{
	/* ParserXTP.h:123 的私有 typedef，按同一类型在本 TU 重写一遍（typedef 不是可链接实体） */
	typedef XTP::API::QuoteApi* (*XtpCreaterSig)(uint8_t, const char*, XTP_LOG_LEVEL);

	/*
	 * 布局镜像：基类列表与成员声明序列照抄 `ParserXTP.h:29-131`。
	 * 两个基类（IParserApi、XTP::API::QuoteSpi）都不含数据成员，ParserXTP 的数据成员
	 * 全部集中在同一个 private 段里，所以把同样一段搬到 public 段不改变偏移
	 * （Itanium C++ ABI 按声明顺序排布；SetUp 的锚点断言是最终裁判）。
	 */
	class ParserXTPMirror : public IParserApi, public XTP::API::QuoteSpi
	{
	public:
		uint32_t m_uTradingDate;
		ParserXTP::LoginStatus m_loginState;
		XTP::API::QuoteApi* m_pUserAPI;

		std::string m_strHost;
		int32_t m_iPort;
		uint32_t m_uClientID;
		std::string m_strUser;
		std::string m_strPass;
		std::string m_strFlowDir;
		std::string m_strLocalIP;

		XTP_PROTOCOL_TYPE m_iProtocol;
		uint32_t m_uHBInterval;
		uint32_t m_uBuffSize;

		CodeSet m_fitSHSubs;
		CodeSet m_fitSZSubs;

		int m_iRequestID;

		IParserSpi* m_sink;
		IBaseDataMgr* m_pBaseDataMgr;

		DllHandle m_hInst;
		XtpCreaterSig m_funcCreator;

		boost::asio::io_context _asyncio;
		StdThreadPtr _thrd_worker;
		std::shared_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> _worker;
	};

	static_assert(sizeof(ParserXTPMirror) == sizeof(ParserXTP),
		"R27 布局镜像与 ParserXTP 尺寸不一致：ParserXTP.h 的成员声明被改过，镜像需要重新对照");

	/* 捕获用 IParserSpi：所有回调都只记账 + 拷贝定长结构体出来 */
	class FakeSink : public IParserSpi
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
			lastTick = quote->getTickStruct();
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
			lastOrdDtl = ordDtlData->getOrdDtlStruct();
		}
		void handleTransaction(WTSTransData* transData) override
		{
			transCalls++;
			lastTrans = transData->getTransStruct();
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

		bool hasLog(const std::string& needle) const
		{
			for (const auto& lg : logs)
				if (lg.second.find(needle) != std::string::npos)
					return true;
			return false;
		}
		uint32_t logCount(const std::string& needle) const
		{
			uint32_t n = 0;
			for (const auto& lg : logs)
				if (lg.second.find(needle) != std::string::npos)
					n++;
			return n;
		}

		std::vector<std::pair<WTSParserEvent, int32_t>> events;
		std::vector<std::pair<WTSLogLevel, std::string>> logs;
		std::vector<QueueCapture> queues;
		WTSTickStruct lastTick{};
		WTSOrdDtlStruct lastOrdDtl{};
		WTSTransStruct lastTrans{};
		IBaseDataMgr* bdMgr = nullptr;
		uint32_t quoteCalls = 0;
		uint32_t symbolListCalls = 0;
		uint32_t ordDtlCalls = 0;
		uint32_t transCalls = 0;
		uint32_t bdMgrQueryCalls = 0;
		uint32_t quoteProcFlag = 0;
	};

	/* 只认一个合约的 IBaseDataMgr，同时记录查询用的 (code, exchg) 对 */
	class FakeBdMgr : public IBaseDataMgr
	{
	public:
		WTSCommodityInfo* getCommodity(const char*) override { return nullptr; }
		WTSCommodityInfo* getCommodity(const char*, const char*) override { return nullptr; }
		WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t /*uDate*/ = 0) override
		{
			getContractCalls++;
			lastCode = code;
			lastExchg = exchg;
			if (contract == nullptr || rejectAll)
				return nullptr;
			if (exchg == nullptr || std::strcmp(code, contract->getCode()) != 0 ||
				std::strcmp(exchg, contract->getExchg()) != 0)
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
		std::string lastCode;
		std::string lastExchg;
	};

	/*
	 * 本地可控的假厂商 api：只实现 ParserXTP 真正会调的那几个方法，
	 * 其余 pure virtual 一律空实现（签名照抄 xtp_quote_api.h:235-449）。
	 */
	class FakeApi : public XTP::API::QuoteApi
	{
	public:
		struct SubCall
		{
			XTP_EXCHANGE_TYPE exchg = XTP_EXCHANGE_UNKNOWN;
			int count = 0;
			std::vector<std::string> codes;
		};

		FakeApi() { std::memset(&apiErr, 0, sizeof(apiErr)); }

		/*--- 可控返回 ---*/
		int loginRet = 0;
		int mdRet = 0;
		int tbtRet = 0;
		char tradingDay[16] = "20260105";
		XTPRI apiErr;

		/*--- 调用记录 ---*/
		int loginCalls = 0;
		int releaseCalls = 0;
		int logoutCalls = 0;
		std::string loginIp;
		int loginPort = -1;
		std::string loginUser;
		std::string loginPass;
		XTP_PROTOCOL_TYPE loginProto = XTP_PROTOCOL_TCP;
		std::string loginLocalIp;
		int64_t lastHbInterval = -1;
		uint64_t lastUdpBuffer = 0;
		std::vector<XTP::API::QuoteSpi*> spiRegs;
		std::vector<SubCall> mdCalls;
		std::vector<SubCall> tbtCalls;

		void setErrMsg(const char* msg)
		{
			std::memset(apiErr.error_msg, 0, sizeof(apiErr.error_msg));
			std::snprintf(apiErr.error_msg, sizeof(apiErr.error_msg), "%s", msg);
		}

		static SubCall capture(char* ticker[], int count, XTP_EXCHANGE_TYPE exchg)
		{
			SubCall call;
			call.exchg = exchg;
			call.count = count;
			for (int i = 0; i < count; i++)
				call.codes.emplace_back(ticker[i] != nullptr ? ticker[i] : "");
			return call;
		}

		/*--- ParserXTP 会真正用到的 ---*/
		void RegisterSpi(XTP::API::QuoteSpi* spi) override { spiRegs.push_back(spi); }
		void Release() override { releaseCalls++; }
		const char* GetTradingDay() override { return tradingDay; }
		XTPRI* GetApiLastError() override { return &apiErr; }
		void SetUDPBufferSize(uint32_t buff_size) override { lastUdpBuffer = buff_size; }
		void SetHeartBeatInterval(uint32_t interval) override { lastHbInterval = interval; }
		int Login(const char* ip, int port, const char* user, const char* password,
			XTP_PROTOCOL_TYPE sock_type, const char* local_ip) override
		{
			loginCalls++;
			loginIp = (ip != nullptr) ? ip : "";
			loginPort = port;
			loginUser = (user != nullptr) ? user : "";
			loginPass = (password != nullptr) ? password : "";
			loginProto = sock_type;
			loginLocalIp = (local_ip != nullptr) ? local_ip : "";
			return loginRet;
		}
		int SubscribeMarketData(char* ticker[], int count, XTP_EXCHANGE_TYPE exchange_id) override
		{
			mdCalls.push_back(capture(ticker, count, exchange_id));
			return mdRet;
		}
		int SubscribeTickByTick(char* ticker[], int count, XTP_EXCHANGE_TYPE exchange_id) override
		{
			tbtCalls.push_back(capture(ticker, count, exchange_id));
			return tbtRet;
		}

		/*--- 未被 ParserXTP 调用，仅为满足抽象接口 ---*/
		const char* GetApiVersion() override { return "fake-xtp-2.2.32.2"; }
		void SetUDPRecvThreadAffinity(int32_t) override {}
		void SetUDPRecvThreadAffinityArray(int32_t[], int32_t) override {}
		void SetUDPParseThreadAffinity(int32_t) override {}
		void SetUDPParseThreadAffinityArray(int32_t[], int32_t) override {}
		void SetUDPSeqLogOutPutFlag(bool) override {}
		int UnSubscribeMarketData(char* ticker[], int count, XTP_EXCHANGE_TYPE exchange_id) override
		{
			unsubMdCalls.push_back(capture(ticker, count, exchange_id));
			return 0;
		}
		int UnSubscribeTickByTick(char* ticker[], int count, XTP_EXCHANGE_TYPE exchange_id) override
		{
			unsubTbtCalls.push_back(capture(ticker, count, exchange_id));
			return 0;
		}
		int SubscribeOrderBook(char* ticker[], int count, XTP_EXCHANGE_TYPE exchange_id) override
		{
			orderBookCalls.push_back(capture(ticker, count, exchange_id));
			return 0;
		}
		int UnSubscribeOrderBook(char* ticker[], int count, XTP_EXCHANGE_TYPE exchange_id) override
		{
			unsubOrderBookCalls.push_back(capture(ticker, count, exchange_id));
			return 0;
		}
		int SubscribeAllMarketData(XTP_EXCHANGE_TYPE) override { return 0; }
		int UnSubscribeAllMarketData(XTP_EXCHANGE_TYPE) override { return 0; }
		int SubscribeAllOrderBook(XTP_EXCHANGE_TYPE) override { return 0; }
		int UnSubscribeAllOrderBook(XTP_EXCHANGE_TYPE) override { return 0; }
		int SubscribeAllTickByTick(XTP_EXCHANGE_TYPE) override { return 0; }
		int UnSubscribeAllTickByTick(XTP_EXCHANGE_TYPE) override { return 0; }
		int Logout() override { logoutCalls++; return 0; }
		int QueryAllTickers(XTP_EXCHANGE_TYPE) override { return 0; }
		int QueryTickersPriceInfo(char* ticker[], int count, XTP_EXCHANGE_TYPE exchange_id) override
		{
			priceQueryCalls.push_back(capture(ticker, count, exchange_id));
			return 0;
		}
		int QueryAllTickersPriceInfo() override { return 0; }
		int SubscribeAllOptionMarketData(XTP_EXCHANGE_TYPE) override { return 0; }
		int UnSubscribeAllOptionMarketData(XTP_EXCHANGE_TYPE) override { return 0; }
		int SubscribeAllOptionOrderBook(XTP_EXCHANGE_TYPE) override { return 0; }
		int UnSubscribeAllOptionOrderBook(XTP_EXCHANGE_TYPE) override { return 0; }
		int SubscribeAllOptionTickByTick(XTP_EXCHANGE_TYPE) override { return 0; }
		int UnSubscribeAllOptionTickByTick(XTP_EXCHANGE_TYPE) override { return 0; }
		int QueryAllTickersFullInfo(XTP_EXCHANGE_TYPE) override { return 0; }

		std::vector<SubCall> unsubMdCalls;
		std::vector<SubCall> unsubTbtCalls;
		std::vector<SubCall> orderBookCalls;
		std::vector<SubCall> unsubOrderBookCalls;
		std::vector<SubCall> priceQueryCalls;
	};

	/* 可被 OnDepthMarketData / OnTickByTick 消化的合约夹具 */
	class FixtureContract
	{
	public:
		FixtureContract(const char* pid = "rb", const char* exchg = "SSE",
			const char* code = "600519", ContractCategory cat = CC_Future)
		{
			comm = WTSCommodityInfo::create(pid, "覆盖率品种", exchg, "SY241", "trading");
			comm->setVolScale(10);
			comm->setCategory(cat);
			comm->setCoverMode(CM_CoverToday);
			ct = WTSContractInfo::create(code, "覆盖率合约", exchg, pid);
			ct->setCommInfo(comm);
		}
		~FixtureContract()
		{
			if (ct != nullptr)
				ct->release();
			if (comm != nullptr)
				comm->release();
		}
		WTSCommodityInfo* comm = nullptr;
		WTSContractInfo* ct = nullptr;
	};

	inline XTPST makeTicker(XTP_EXCHANGE_TYPE exchg, const char* code)
	{
		XTPST st;
		std::memset(&st, 0, sizeof(st));
		st.exchange_id = exchg;
		std::snprintf(st.ticker, sizeof(st.ticker), "%s", code);
		return st;
	}

	inline XTPRI makeError(int32_t id, const char* msg)
	{
		XTPRI err;
		std::memset(&err, 0, sizeof(err));
		err.error_id = id;
		std::snprintf(err.error_msg, sizeof(err.error_msg), "%s", msg);
		return err;
	}

	/*
	 * 主夹具：把「init() 会写的那 9 个字段 + api 指针」灌进真对象，并把
	 * `_thrd_worker` 预先占位成非空，使 `connect()` 走「已有 worker」分支
	 * （ParserXTP.cpp:146 判空为假）—— 这样驱动 DoLogin 不会拉起不可回收线程。
	 */
	class XtpSeamF : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			sink.bdMgr = &bdmgr;
			parser.registerSpi(&sink);
			m = reinterpret_cast<ParserXTPMirror*>(&parser);

			/* --- 布局锚点：先证明镜像对得上，再往里灌东西 --- */
			ASSERT_EQ(&sink, m->m_sink);
			ASSERT_EQ(&bdmgr, m->m_pBaseDataMgr);
			ASSERT_EQ(0u, m->m_uTradingDate);
			ASSERT_EQ(0, m->m_iRequestID);
			ASSERT_EQ(nullptr, m->m_pUserAPI);
			ASSERT_TRUE(m->m_fitSHSubs.empty());
			ASSERT_TRUE(m->m_fitSZSubs.empty());
			/* init() assigns these raw fields after loading the vendor library;
			 * the constructor leaves them indeterminate, so don't read them here. */
			ASSERT_EQ(nullptr, m->_thrd_worker);
			ASSERT_EQ(nullptr, m->_worker);

			/* --- 代替 init() 的配置灌值（ParserXTP.cpp:96-105 的等价物） --- */
			m->m_strHost = "127.0.0.1";
			m->m_iPort = 6101;
			m->m_strUser = "800123";
			m->m_strPass = "secret";
			m->m_iProtocol = XTP_PROTOCOL_UDP;
			m->m_uClientID = 7u;
			m->m_uHBInterval = 15u;
			m->m_uBuffSize = 2097152u;
			m->m_strLocalIP = "0.0.0.0";
			m->m_pUserAPI = &api;
			/* connect() 的 worker 判空必须为假，否则拉起 while(true) 线程 */
			workerPlaceholder = std::make_shared<StdThread>();
			m->_thrd_worker = workerPlaceholder;
			/* 未登录态：DoSubscribeMD/subscribe 走缓存分支 */
			m->m_uTradingDate = 0u;
		}

		void TearDown() override
		{
			/* 别让 ~ParserXTP 去 Release() 一个已经先析构的 fake（断开连接由用例自己做） */
			m->m_pUserAPI = nullptr;
			m->_thrd_worker = nullptr;
		}

		/* 跑掉 `_asyncio` 里已 post 的 handler（生产里由 connect() 起的 worker 线程做同一件事） */
		uint64_t pump()
		{
			return m->_asyncio.poll();
		}

		FakeApi api;
		FakeSink sink;
		FakeBdMgr bdmgr;
		ParserXTP parser;
		ParserXTPMirror* m = nullptr;
		std::shared_ptr<StdThread> workerPlaceholder;
	};

	/* 登录链夹具：SetUp 里先按未登录态缓存两只代码（SSE/SZSE 各一只） */
	class XtpLoginF : public XtpSeamF
	{
	protected:
		void SetUp() override
		{
			XtpSeamF::SetUp();
			CodeSet codes;
			codes.insert("SSE.600519");
			codes.insert("SZSE.000001");
			parser.subscribe(codes);
			ASSERT_EQ(1u, m->m_fitSHSubs.size());
			ASSERT_EQ(1u, m->m_fitSZSubs.size());
			sink.logs.clear();
			sink.events.clear();
		}
	};

	/* 已登录夹具：SetUp 里跑一次成功登录，再把记账清空，只留下「交易日已置位」这个状态 */
	class XtpOnlineF : public XtpLoginF
	{
	protected:
		void SetUp() override
		{
			XtpLoginF::SetUp();
			api.loginRet = 0;
			ASSERT_TRUE(parser.connect());
			ASSERT_EQ(20260105u, m->m_uTradingDate);
			/* 登录成功链路上 DoSubscribeMD 已经跑过一轮 */
			ASSERT_EQ(2u, api.mdCalls.size());
			ASSERT_EQ(2u, api.tbtCalls.size());
			/* 把成功登录 post 出去的 2 个事件跑掉并清空，用例从「干净在线态」起步 */
			pump();
			api.mdCalls.clear();
			api.tbtCalls.clear();
			sink.logs.clear();
			sink.events.clear();
		}
	};
}
