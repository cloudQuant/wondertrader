/*
 * lane R60（第十二波）— TraderYD 公开回调补测
 *
 * 目标文件：src/TraderYD/TraderYD.cpp（missed 497/876）
 *
 * 与 t5_yd_api_test.cpp / t5_yd_map_test.cpp 的分工（不重复）：
 *  - T5 已覆盖 init/release、未连接时的 6 条拒绝、makeEntrustID、notifyReadyForLogin、
 *    notifyLogin 失败、notifyCaughtUp 幂等、废单/废撤错误映射、makeOrderInfo/makeTradeRecord
 *    在「已追上」分支的字段转换、notifyAccount 的两条空守卫、工厂入口。
 *  - 本文件新增：① 通过公开回调 notifyLogin(失败) 把 getErrorMsg 的**全部** switch 分支驱一遍；
 *    ② notifyOrder 在 m_bCatchup=false 时的本地订单缓存链（建表/首推/重复单/撤单状态）；
 *    ③ 拒单错误标志（自成交 vs 普通）、FOK、AllTraded/Accepted/Canceled 状态映射；
 *    ④ 未登记合约的成交回报丢弃；⑤ release 清理订单缓存；⑥ m_sink=NULL 时回调静默。
 *  - 本机没有厂商 libyd，m_pUserAPI 恒 NULL，因此 connect()/notifyFinishInit()/notifyLogin(成功)
 *    仍是必崩面：只留 DISABLED_ 与 bugs/R60.md，不硬闯。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "TraderYD/TraderYD.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "API/yd1.108.360/ydError.h"

USING_NS_WTP;

namespace
{
	class R60YdBdMgr : public IBaseDataMgr
	{
	public:
		virtual ~R60YdBdMgr()
		{
			for (WTSContractInfo* ct : _cts)
				ct->release();
			for (WTSCommodityInfo* comm : _comms)
				comm->release();
		}

		void regis(WTSCommodityInfo* comm, WTSContractInfo* ct)
		{
			ct->setCommInfo(comm);
			_comms.push_back(comm);
			_cts.push_back(ct);
			_key[std::string(ct->getCode()) + "|" + ct->getExchg()] = ct;
		}

		virtual WTSCommodityInfo* getCommodity(const char* exchgpid) override
		{
			for (WTSCommodityInfo* comm : _comms)
			{
				if (strcmp(comm->getFullPid(), exchgpid) == 0)
					return comm;
			}
			return NULL;
		}

		virtual WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
		{
			(void)exchg;
			(void)pid;
			return NULL;
		}

		virtual WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override
		{
			(void)uDate;
			auto it = _key.find(std::string(code ? code : "") + "|" + (exchg ? exchg : ""));
			return it == _key.end() ? NULL : it->second;
		}

		virtual WTSArray* getContracts(const char* exchg = "", uint32_t uDate = 0) override
		{
			(void)exchg;
			(void)uDate;
			return NULL;
		}
		virtual WTSSessionInfo* getSession(const char* sid) override { (void)sid; return NULL; }
		virtual WTSSessionInfo* getSessionByCode(const char* code, const char* exchg = "") override
		{
			(void)code;
			(void)exchg;
			return NULL;
		}
		virtual WTSArray* getAllSessions() override { return NULL; }
		virtual bool isHoliday(const char* pid, uint32_t uDate, bool isTpl = false) override
		{
			(void)pid;
			(void)uDate;
			(void)isTpl;
			return false;
		}
		virtual uint32_t calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession = false) override
		{
			(void)stdPID;
			(void)uTime;
			(void)isSession;
			return uDate;
		}
		virtual uint64_t getBoundaryTime(const char* stdPID, uint32_t tDate, bool isSession = false, bool isStart = true) override
		{
			(void)stdPID;
			(void)isSession;
			(void)isStart;
			return tDate;
		}
		virtual uint32_t getGlobalSize() override { return 8; }

	private:
		std::vector<WTSCommodityInfo*> _comms;
		std::vector<WTSContractInfo*> _cts;
		std::map<std::string, WTSContractInfo*> _key;
	};

	class R60YdSink : public ITraderSpi
	{
	public:
		struct OrderRec
		{
			bool valid = false;
			std::string code;
			std::string exchg;
			std::string order_id;
			std::string entrust_id;
			std::string user_tag;
			std::string state_msg;
			double price = 0;
			double volume = 0;
			double vol_traded = 0;
			double vol_left = 0;
			int direction = -1;
			int offset = -1;
			int price_type = -1;
			int order_flag = -1;
			int state = -1;
			bool is_error = false;
			bool is_self_trade = false;
		};
		struct TradeRec
		{
			bool valid = false;
			std::string code;
			std::string trade_id;
			double price = 0;
			double volume = 0;
		};
		struct LoginRec
		{
			bool succ = false;
			std::string msg;
			uint32_t date = 0;
		};

		virtual IBaseDataMgr* getBaseDataMgr() override { return &bd_; }

		virtual void handleEvent(WTSTraderEvent e, int32_t ec) override
		{
			std::lock_guard<std::mutex> lk(_mtx);
			_events.push_back(std::make_pair((int)e, ec));
			_event_.fetch_add(1);
		}

		virtual void handleTraderLog(WTSLogLevel ll, const char* message) override
		{
			(void)ll;
			std::lock_guard<std::mutex> lk(_mtx);
			_logs.push_back(message ? message : "");
			_log_.fetch_add(1);
		}

		virtual void onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override
		{
			LoginRec r;
			r.succ = bSucc;
			r.msg = msg ? msg : "";
			r.date = tradingdate;
			std::lock_guard<std::mutex> lk(_mtx);
			_logins.push_back(r);
			_login_.fetch_add(1);
		}

		virtual void onPushOrder(WTSOrderInfo* orderInfo) override
		{
			OrderRec r;
			if (orderInfo)
			{
				r.valid = true;
				r.code = orderInfo->getCode() ? orderInfo->getCode() : "";
				r.exchg = orderInfo->getExchg() ? orderInfo->getExchg() : "";
				r.order_id = orderInfo->getOrderID() ? orderInfo->getOrderID() : "";
				r.entrust_id = orderInfo->getEntrustID() ? orderInfo->getEntrustID() : "";
				r.user_tag = orderInfo->getUserTag() ? orderInfo->getUserTag() : "";
				r.state_msg = orderInfo->getStateMsg() ? orderInfo->getStateMsg() : "";
				r.price = orderInfo->getPrice();
				r.volume = orderInfo->getVolume();
				r.vol_traded = orderInfo->getVolTraded();
				r.vol_left = orderInfo->getVolLeft();
				r.direction = (int)orderInfo->getDirection();
				r.offset = (int)orderInfo->getOffsetType();
				r.price_type = (int)orderInfo->getPriceType();
				r.order_flag = (int)orderInfo->getOrderFlag();
				r.state = (int)orderInfo->getOrderState();
				r.is_error = orderInfo->isError();
				r.is_self_trade = orderInfo->isSelfTrade();
			}
			std::lock_guard<std::mutex> lk(_mtx);
			_orders.push_back(r);
			_push_order_.fetch_add(1);
		}

		virtual void onPushTrade(WTSTradeInfo* tradeRecord) override
		{
			TradeRec r;
			if (tradeRecord)
			{
				r.valid = true;
				r.code = tradeRecord->getCode() ? tradeRecord->getCode() : "";
				r.trade_id = tradeRecord->getTradeID() ? tradeRecord->getTradeID() : "";
				r.price = tradeRecord->getPrice();
				r.volume = tradeRecord->getVolume();
			}
			std::lock_guard<std::mutex> lk(_mtx);
			_trades.push_back(r);
			_push_trade_.fetch_add(1);
		}

		uint32_t n_event() const { return _event_.load(); }
		uint32_t n_log() const { return _log_.load(); }
		uint32_t n_login() const { return _login_.load(); }
		uint32_t n_push_order() const { return _push_order_.load(); }
		uint32_t n_push_trade() const { return _push_trade_.load(); }

		std::string last_log() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return _logs.empty() ? std::string() : _logs.back();
		}
		void clear_logs()
		{
			std::lock_guard<std::mutex> lk(_mtx);
			_logs.clear();
			_log_.store(0);
		}
		OrderRec last_order() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return _orders.empty() ? OrderRec{} : _orders.back();
		}
		OrderRec order_at(uint32_t idx) const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return idx < _orders.size() ? _orders[idx] : OrderRec{};
		}
		LoginRec last_login() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return _logins.empty() ? LoginRec{} : _logins.back();
		}

		R60YdBdMgr bd_;

	private:
		mutable std::mutex _mtx;
		std::atomic<uint32_t> _event_{ 0 };
		std::atomic<uint32_t> _log_{ 0 };
		std::atomic<uint32_t> _login_{ 0 };
		std::atomic<uint32_t> _push_order_{ 0 };
		std::atomic<uint32_t> _push_trade_{ 0 };
		std::vector<std::pair<int, int32_t>> _events;
		std::vector<std::string> _logs;
		std::vector<LoginRec> _logins;
		std::vector<OrderRec> _orders;
		std::vector<TradeRec> _trades;
	};

	class R60YdTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			WTSVariant* cfg = WTSVariant::createObject();
			cfg->append("config", "yd.conf");
			cfg->append("user", "yduser");
			cfg->append("pass", "ydpass");
			ASSERT_TRUE(api_.init(cfg));
			cfg->release();

			comm_ = WTSCommodityInfo::create("ag", "沪银", "SHFE", "TRADING", "CHINA");
			comm_->setCategory(CC_Future);
			comm_->setCoverMode(CM_CoverToday);
			comm_->setTradingMode(TM_Both);
			comm_->setVolScale(5);
			ct_ = WTSContractInfo::create("ag2512", "沪银2512", "SHFE", "ag");
			ct_->setTotalIndex(1);
			sink_.bd_.regis(comm_, ct_);

			api_.registerSpi(&sink_);
		}

		void TearDown() override
		{
			api_.release();
		}

		void fill_instrument(YDInstrument& inst, YDExchange& exch, const char* code, const char* exchg)
		{
			memset(&inst, 0, sizeof(inst));
			memset(&exch, 0, sizeof(exch));
			strncpy(inst.InstrumentID, code, sizeof(inst.InstrumentID) - 1);
			strncpy(exch.ExchangeID, exchg, sizeof(exch.ExchangeID) - 1);
			inst.m_pExchange = &exch;
		}

		YDOrder make_order(int direction, int offset, int order_type, double price, int volume,
			int status, int sys_id, int order_ref)
		{
			YDOrder ord;
			memset(&ord, 0, sizeof(ord));
			ord.Direction = direction;
			ord.OffsetFlag = offset;
			ord.OrderType = order_type;
			ord.Price = price;
			ord.OrderVolume = volume;
			ord.OrderStatus = status;
			ord.OrderSysID = sys_id;
			ord.OrderRef = order_ref;
			return ord;
		}

	protected:
		R60YdSink sink_;
		TraderYD api_;
		WTSCommodityInfo* comm_ = NULL;
		WTSContractInfo* ct_ = NULL;
	};

	/* ------------------------------------------------------------------ */
	/* getErrorMsg 全表（通过公开的 notifyLogin 失败回调驱动）              */
	/* ------------------------------------------------------------------ */

	/// TraderYD.cpp L258-301（notifyLogin 失败分支）→ L122-206（getErrorMsg 全部 case，
	/// 含 4/5/22/23 未列出而回落 Unknown 的现状）。
	TEST_F(R60YdTest, login_failure_walks_every_error_message_case)
	{
		const std::vector<std::pair<int, std::string>> cases = {
			{ YD_ERROR_NoError, "" },	// 0 走成功分支，不能喂；占位不用
			{ YD_ERROR_NoPositionToClose, "NoPositionToClose" },
			{ YD_ERROR_NoMoneyToOpen, "NoMoneyToOpen" },
			{ YD_ERROR_SystemNotReady, "SystemNotReady" },
			{ YD_ERROR_InvalidInstrument, "Unknown Error" },
			{ YD_ERROR_InvalidAccount, "Unknown Error" },
			{ YD_ERROR_OrderFieldError, "OrderFieldError" },
			{ YD_ERROR_MemoryExceed, "MemoryExceed" },
			{ YD_ERROR_NoTradingCodeInExchange, "NoTradingCodeInExchange" },
			{ YD_ERROR_CanNotSendToExchange, "CanNotSendToExchange" },
			{ YD_ERROR_NoTradingRight, "NoTradingRight" },
			{ YD_ERROR_InvalidOrderVolume, "InvalidOrderVolume" },
			{ YD_ERROR_InvalidClientAPP, "InvalidClientAPP" },
			{ YD_ERROR_PositionLimitExceed, "PositionLimitExceed" },
			{ YD_ERROR_TradeVolumeExceed, "TradeVolumeExceed" },
			{ YD_ERROR_OrderCancelLimitExceed, "OrderCancelLimitExceed" },
			{ YD_ERROR_OrderOpenLimitExceed, "OrderOpenLimitExceed" },
			{ YD_ERROR_InvalidConnectionID, "InvalidConnectionID" },
			{ YD_ERROR_AlreadyLogined, "AlreadyLogined" },
			{ YD_ERROR_PasswordError, "PasswordError" },
			{ YD_ERROR_TooManyRequests, "TooManyRequests" },
			{ YD_ERROR_InvalidUsername, "InvalidUsername" },
			{ YD_ERROR_UsernameMismatch, "Unknown Error" },
			{ YD_ERROR_OldPasswordMismatch, "Unknown Error" },
			{ YD_ERROR_InsertOrderTooFast, "InsertOrderTooFast" },
			{ YD_ERROR_PossibleSelfTrade, "PossibleSelfTrade" },
			{ YD_ERROR_NoAdminRight, "NoAdminRight" },
			{ YD_ERROR_InvalidAddress, "InvalidAddress" },
			{ YD_ERROR_OrderTypeNotSupported, "OrderTypeNotSupported" },
			{ YD_ERROR_CancelOrderFieldError, "CancelOrderFieldError" },
			{ YD_ERROR_InvalidExchange, "InvalidExchange" },
			{ YD_ERROR_OrderNotFound, "OrderNotFound" },
			{ YD_ERROR_OrderNotBelongToAccount, "OrderNotBelongToAccount" },
			{ YD_ERROR_OrderFinished, "OrderFinished" },
			{ YD_ERROR_OnlyLimitOrderCanBeCanceled, "OnlyLimitOrderCanBeCanceled" },
			{ YD_ERROR_ClientReportError, "ClientReportError" },
			{ YD_ERROR_TooManyOrders, "TooManyOrders" },
			{ YD_ERROR_InstrumentCanNotTrade, "InstrumentCanNotTrade" },
			{ YD_ERROR_YDOrderFlagNotSupported, "YDOrderFlagNotSupported" },
			{ YD_ERROR_NotOptionInstrument, "NotOptionInstrument" },
			{ YD_ERROR_PriceOutOfLimit, "PriceOutOfLimit" },
			{ YD_ERROR_CrossPriceInQuote, "CrossPriceInQuote" },
			{ YD_ERROR_QuoteFieldError, "QuoteFieldError" },
			{ YD_ERROR_QuoteVolumeError, "QuoteVolumeError" },
			{ YD_ERROR_QuoteNotFound, "QuoteNotFound" },
			{ YD_ERROR_CancelQuoteFieldError, "CancelQuoteFieldError" },
			{ YD_ERROR_QuoteNotBelongToAccount, "QuoteNotBelongToAccount" },
			{ YD_ERROR_QuoteFinished, "QuoteFinished" },
			{ YD_ERROR_QuoteNotSupported, "QuoteNotSupported" },
			{ YD_ERROR_CannotCancelQuoteDerivedOrder, "CannotCancelQuoteDerivedOrder" },
			{ YD_ERROR_TooManyLogines, "TooManyLogines" },
			{ YD_ERROR_NoEnoughPositiontoMakeCombPosition, "NoEnoughPositiontoMakeCombPosition" },
			{ YD_ERROR_NoEnoughCombPosition, "NoEnoughCombPosition" },
			{ YD_ERROR_NoMoneyForSplitCombPosition, "NoMoneyForSplitCombPosition" },
			{ YD_ERROR_InvalidCombPosition, "InvalidCombPosition" },
			{ YD_ERROR_CannotSelectConnection, "CannotSelectConnection" },
			{ YD_ERROR_SelectConnectionTooFrequently, "SelectConnectionTooFrequently" },
			{ YD_ERROR_InvalidSelectConnection, "InvalidSelectConnection" },
			{ YD_ERROR_TooLowApiVersion, "TooLowApiVersion" },
			{ YD_ERROR_InvalidTradingRight, "InvalidTradingRight" },
			{ YD_ERROR_InvalidProduct, "InvalidProduct" },
			{ YD_ERROR_InvalidAlterMoneyField, "InvalidAlterMoneyField" },
			{ YD_ERROR_TooHighApiVersion, "TooHighApiVersion" },
			{ YD_ERROR_MoneyUsageTooLow, "MoneyUsageTooLow" },
			{ YD_ERROR_InvalidInstrumentPairToExecuteTogether, "InvalidInstrumentPairToExecuteTogether" },
			{ YD_ERROR_NotOnExpireDay, "NotOnExpireDay" },
			{ YD_ERROR_NotProperTime, "NotProperTime" },
			{ YD_ERROR_OptonLongPositionCostLimitExceed, "OptonLongPositionCostLimitExceed" },
			{ YD_ERROR_PriceToTriggerFuse, "PriceToTriggerFuse" },
			{ YD_ERROR_ExchangeDoesNotSupport, "ExchangeDoesNotSupport" },
			{ YD_ERROR_CannotSend, "CannotSend" },
			{ YD_ERROR_TooManyInMultiOrders, "TooManyInMultiOrders" },
			{ YD_ERROR_ExchangeReportError, "ExchangeReportError" },
			{ 1026, "NotInTradingTime" },
			{ 1028, "OrderAllTraded" },
			{ 777777, "Unknown Error" },
			{ -1, "Unknown Error" },
		};

		uint32_t expected_logins = 0;
		for (const auto& item : cases)
		{
			if (item.first == YD_ERROR_NoError)
				continue;
			sink_.clear_logs();
			api_.notifyLogin(item.first, 0, false);
			ASSERT_EQ(1u, sink_.n_log()) << "error code " << item.first;
			EXPECT_NE(std::string::npos, sink_.last_log().find(item.second))
				<< "error code " << item.first << " log=" << sink_.last_log();
			++expected_logins;
		}

		EXPECT_EQ(expected_logins, sink_.n_login());
		EXPECT_FALSE(api_.isConnected());
	}

	/* ------------------------------------------------------------------ */
	/* notifyOrder 本地缓存链（m_bCatchup=false）                          */
	/* ------------------------------------------------------------------ */

	/// TraderYD.cpp L426-524：m_mapOrders 首次建表（L434-435）、开仓单不触碰持仓表
	/// （L444 的反向分支）、同 OrderSysID 的第二次回报走 else（L476-516）、
	/// L515 add(false)、L519-522 在非 Catchup 时不外发；notifyCaughtUp 后转主推。
	TEST_F(R60YdTest, order_cache_dedups_before_catchup_and_pushes_after_catchup)
	{
		YDInstrument inst;
		YDExchange exch;
		fill_instrument(inst, exch, "ag2512", "SHFE");

		YDOrder queuing = make_order(YD_D_Buy, YD_OF_Open, YD_ODT_Limit, 7000.0, 5,
			YD_OS_Queuing, 501, 11);
		api_.notifyOrder(&queuing, &inst, NULL);
		EXPECT_EQ(0u, sink_.n_push_order());		// 尚未 catchup

		const std::string eid = "yduser#0000000011";
		const std::string oid = "501";

		// 同一 OrderSysID 的撤单回报：走 else 分支；开仓不查持仓表
		YDOrder canceled = make_order(YD_D_Sell, YD_OF_Open, YD_ODT_Limit, 7000.0, 5,
			YD_OS_Canceled, 501, 11);
		api_.notifyOrder(&canceled, &inst, NULL);
		EXPECT_EQ(0u, sink_.n_push_order());

		api_.notifyCaughtUp();
		EXPECT_TRUE(api_.isConnected());
		ASSERT_EQ(1u, sink_.n_login());
		EXPECT_TRUE(sink_.last_login().succ);

		// catchup 后同一张单再推一次：跳过缓存，直接 onPushOrder
		YDOrder all_traded = make_order(YD_D_Buy, YD_OF_Open, YD_ODT_Limit, 7000.0, 5,
			YD_OS_AllTraded, 501, 11);
		all_traded.TradeVolume = 5;
		api_.notifyOrder(&all_traded, &inst, NULL);
		ASSERT_EQ(1u, sink_.n_push_order());
		const R60YdSink::OrderRec r = sink_.last_order();
		EXPECT_TRUE(r.valid);
		EXPECT_EQ("ag2512", r.code);
		EXPECT_EQ(eid, r.entrust_id);
		EXPECT_EQ(oid, r.order_id);
		EXPECT_EQ((int)WOS_AllTraded, r.state);
		EXPECT_DOUBLE_EQ(5.0, r.vol_traded);
		EXPECT_DOUBLE_EQ(0.0, r.vol_left);
	}

	/* ------------------------------------------------------------------ */
	/* 拒单错误标志：自成交 vs 普通错误 vs 无错误                           */
	/* ------------------------------------------------------------------ */

	/// TraderYD.cpp L993-1001（Rejected → WOS_Canceled；ErrorNo 命中自成交时 WOEF_SelfTrade，
	/// 其他 WOEF_Normal；非拒单一律 WOEF_None）与 L1005-1006（isError 时写 stateMsg）。
	TEST_F(R60YdTest, rejected_order_sets_self_trade_flag_and_state_message)
	{
		api_.notifyCaughtUp();
		YDInstrument inst;
		YDExchange exch;
		fill_instrument(inst, exch, "ag2512", "SHFE");

		YDOrder self = make_order(YD_D_Buy, YD_OF_Open, YD_ODT_Limit, 7000.0, 1,
			YD_OS_Rejected, 601, 21);
		self.ErrorNo = YD_ERROR_PossibleSelfTrade;
		api_.notifyOrder(&self, &inst, NULL);

		YDOrder normal = make_order(YD_D_Buy, YD_OF_Open, YD_ODT_Limit, 7000.0, 1,
			YD_OS_Rejected, 602, 22);
		normal.ErrorNo = YD_ERROR_OrderNotFound;
		api_.notifyOrder(&normal, &inst, NULL);

		YDOrder no_error = make_order(YD_D_Buy, YD_OF_Open, YD_ODT_Limit, 7000.0, 1,
			YD_OS_Queuing, 603, 23);
		no_error.ErrorNo = YD_ERROR_PossibleSelfTrade;	// 非拒单状态不读错误码
		api_.notifyOrder(&no_error, &inst, NULL);

		ASSERT_EQ(3u, sink_.n_push_order());
		const R60YdSink::OrderRec r1 = sink_.order_at(0);
		EXPECT_EQ((int)WOS_Canceled, r1.state);
		EXPECT_TRUE(r1.is_error);
		EXPECT_TRUE(r1.is_self_trade);
		EXPECT_EQ("PossibleSelfTrade(25)", r1.state_msg);

		const R60YdSink::OrderRec r2 = sink_.order_at(1);
		EXPECT_EQ((int)WOS_Canceled, r2.state);
		EXPECT_TRUE(r2.is_error);
		EXPECT_FALSE(r2.is_self_trade);
		EXPECT_EQ("OrderNotFound(31)", r2.state_msg);

		const R60YdSink::OrderRec r3 = sink_.order_at(2);
		EXPECT_FALSE(r3.is_error);
		EXPECT_FALSE(r3.is_self_trade);
		EXPECT_TRUE(r3.state_msg.empty());
	}

	/* ------------------------------------------------------------------ */
	/* FOK / 市价 / 终态映射                                               */
	/* ------------------------------------------------------------------ */

	/// TraderYD.cpp L980-981（0 价→ANYPRICE，FOK→WOF_FOK）、L107-120（AllTraded/
	/// Accepted/Canceled 的状态表）、L109-115（Rejected 与 Canceled 同归 WOS_Canceled 的邻接分支）。
	TEST_F(R60YdTest, fok_market_price_and_terminal_states_are_mapped)
	{
		api_.notifyCaughtUp();
		YDInstrument inst;
		YDExchange exch;
		fill_instrument(inst, exch, "ag2512", "SHFE");

		YDOrder fok = make_order(YD_D_Sell, YD_OF_Close, YD_ODT_FOK, 7000.0, 3,
			YD_OS_AllTraded, 701, 31);
		fok.TradeVolume = 3;
		api_.notifyOrder(&fok, &inst, NULL);

		YDOrder market = make_order(YD_D_Buy, YD_OF_Open, YD_ODT_Market, 0.0, 1,
			YD_OS_Accepted, 702, 32);
		api_.notifyOrder(&market, &inst, NULL);

		YDOrder canceled = make_order(YD_D_Buy, YD_OF_Open, YD_ODT_Limit, 7000.0, 1,
			YD_OS_Canceled, 703, 33);
		api_.notifyOrder(&canceled, &inst, NULL);

		ASSERT_EQ(3u, sink_.n_push_order());
		const R60YdSink::OrderRec r1 = sink_.order_at(0);
		EXPECT_EQ((int)WOF_FOK, r1.order_flag);
		EXPECT_EQ((int)WPT_LIMITPRICE, r1.price_type);
		EXPECT_EQ((int)WOS_AllTraded, r1.state);
		EXPECT_DOUBLE_EQ(0.0, r1.vol_left);

		const R60YdSink::OrderRec r2 = sink_.order_at(1);
		EXPECT_EQ((int)WPT_ANYPRICE, r2.price_type);
		EXPECT_EQ((int)WOF_NOR, r2.order_flag);
		EXPECT_EQ((int)WOS_NotTraded_NotQueuing, r2.state);

		const R60YdSink::OrderRec r3 = sink_.order_at(2);
		EXPECT_EQ((int)WOS_Canceled, r3.state);
	}

	/* ------------------------------------------------------------------ */
	/* 未登记合约的成交回报丢弃                                             */
	/* ------------------------------------------------------------------ */

	/// TraderYD.cpp L1059-1063（makeTradeRecord 在合约表未命中时返回 NULL）；
	/// notifyTrade 的 `if (trdInfo)` 两侧都不应触碰持仓表或外发。
	TEST_F(R60YdTest, trade_for_unregistered_contract_is_dropped_before_and_after_catchup)
	{
		YDInstrument inst;
		YDExchange exch;
		fill_instrument(inst, exch, "cu9999", "SHFE");

		YDTrade trd;
		memset(&trd, 0, sizeof(trd));
		trd.Direction = YD_D_Buy;
		trd.OffsetFlag = YD_OF_Open;
		trd.Price = 70000.0;
		trd.Volume = 1;
		trd.TradeID = 9001;
		trd.OrderSysID = 8001;

		api_.notifyTrade(&trd, &inst, NULL);	// catchup=false
		EXPECT_EQ(0u, sink_.n_push_trade());

		api_.notifyCaughtUp();
		api_.notifyTrade(&trd, &inst, NULL);	// catchup=true
		EXPECT_EQ(0u, sink_.n_push_trade());
	}

	/* ------------------------------------------------------------------ */
	/* release 清理订单缓存 + 幂等                                          */
	/* ------------------------------------------------------------------ */

	/// TraderYD.cpp L661-679（release：m_pUserAPI 为空、m_mapOrders 非空时 clear；
	/// m_mapPosition/m_mapTrades 仍为 NULL 的两侧）以及 isConnected 的 WS_ALLREADY 判定。
	TEST_F(R60YdTest, release_clears_order_cache_and_is_repeatable)
	{
		YDInstrument inst;
		YDExchange exch;
		fill_instrument(inst, exch, "ag2512", "SHFE");
		YDOrder ord = make_order(YD_D_Buy, YD_OF_Open, YD_ODT_Limit, 7000.0, 1,
			YD_OS_Queuing, 901, 41);
		api_.notifyOrder(&ord, &inst, NULL);	// 建出 m_mapOrders
		EXPECT_EQ(0u, sink_.n_push_order());

		api_.release();
		api_.release();							// 幂等：第二次全部 clear 空表
		EXPECT_FALSE(api_.isConnected());
		EXPECT_EQ(0u, sink_.n_push_order());
	}

	/* ------------------------------------------------------------------ */
	/* m_sink=NULL 时所有通知静默                                           */
	/* ------------------------------------------------------------------ */

	/// TraderYD.cpp L247-251（notifyReadyForLogin 的 if(m_sink)）、L294-301（登录失败的
	/// write_log/onLoginResult 两侧）、L610-624（notifyCaughtUp 的 if(m_sink)）。
	TEST_F(R60YdTest, null_sink_silences_login_ready_and_caught_up)
	{
		api_.registerSpi(NULL);

		api_.notifyLogin(YD_ERROR_PasswordError, 0, false);
		api_.notifyCaughtUp();
		api_.notifyReadyForLogin(false);

		EXPECT_EQ(0u, sink_.n_log());
		EXPECT_EQ(0u, sink_.n_login());
		EXPECT_EQ(0u, sink_.n_event());
		EXPECT_TRUE(api_.isConnected());		// 状态机仍推进，只是不外发
	}

	/* ------------------------------------------------------------------ */
	/* 明确不进入的崩溃面（只留钩子，不执行）                               */
	/* ------------------------------------------------------------------ */

	/// TraderYD.cpp L681-688：connect() 直接调用 m_funcCreator；
	/// 本机没有 libyd，init 后 m_funcCreator 恒 NULL → 空函数指针调用必崩。
	/// 见 bugs/R60.md BUG-R60-004。
	TEST_F(R60YdTest, DISABLED_connect_without_vendor_library_calls_null_creator)
	{
		api_.connect();
	}

} // namespace
