/*
 * T5 lane — src/TraderXTP/TraderXTP.cpp 覆盖率用例（收发族）
 *
 * 可测边界（实测源码）：
 *   - 本机没有 libxtptraderapi.dylib → DLLHelper::load_library 返回 NULL →
 *     m_funcCreator=NULL，_api 恒为 NULL。因此 login/logout/orderInsert/orderAction
 *     的**拒绝分支**可确定性覆盖；connect()→reconnect() 会调用 NULL creator、
 *     query*() 四个入口完全没有 _api 判空 → 都会解引用 NULL（见 bugs 文件），
 *     相关路径整条 DISABLED_。
 *   - TraderSpi 的回调（OnXxx）都是 public，可手工构造厂商结构体直喂。
 *   - OnDisconnected 只往成员 io_context 里 post，用例不跑 io_context → 不起线程、不 sleep。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "TraderXTP/TraderXTP.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSTradeDef.hpp"

USING_NS_WTP;

extern "C"
{
	ITraderApi* createTrader();
	void deleteTrader(ITraderApi*& trader);
}

namespace {

class XTPFakeBdMgr : public IBaseDataMgr
{
public:
	virtual ~XTPFakeBdMgr() {}

	void regis(WTSCommodityInfo* comm, WTSContractInfo* ct)
	{
		ct->setCommInfo(comm);
		_comms.push_back(comm);
		_cts.push_back(ct);
		_ct_key[std::string(ct->getCode()) + "|" + ct->getExchg()] = ct;
	}

	virtual WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		for (auto* c : _comms)
			if (strcmp(c->getFullPid(), exchgpid) == 0)
				return c;
		return NULL;
	}
	virtual WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{ (void)exchg; (void)pid; return NULL; }

	virtual WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override
	{
		(void)uDate;
		++n_get_contract_;
		last_code_ = code ? code : "";
		last_exchg_ = exchg ? exchg : "";
		auto it = _ct_key.find(last_code_ + "|" + last_exchg_);
		return it == _ct_key.end() ? NULL : it->second;
	}

	virtual WTSArray* getContracts(const char* exchg = "", uint32_t uDate = 0) override
	{ (void)exchg; (void)uDate; return NULL; }
	virtual WTSSessionInfo* getSession(const char* sid) override { (void)sid; return NULL; }
	virtual WTSSessionInfo* getSessionByCode(const char* code, const char* exchg = "") override
	{ (void)code; (void)exchg; return NULL; }
	virtual WTSArray* getAllSessions() override { return NULL; }
	virtual bool isHoliday(const char* pid, uint32_t uDate, bool isTpl = false) override
	{ (void)pid; (void)uDate; (void)isTpl; return false; }
	virtual uint32_t calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession = false) override
	{ (void)stdPID; (void)uTime; (void)isSession; return uDate; }
	virtual uint64_t getBoundaryTime(const char* stdPID, uint32_t tDate, bool isSession = false, bool isStart = true) override
	{ (void)stdPID; (void)isSession; (void)isStart; return tDate; }

	uint32_t n_get_contract_ = 0;
	std::string last_code_;
	std::string last_exchg_;

private:
	std::vector<WTSCommodityInfo*> _comms;
	std::vector<WTSContractInfo*> _cts;
	wt_hashmap<std::string, WTSContractInfo*> _ct_key;
};

class XTPSink : public ITraderSpi
{
public:
	virtual ~XTPSink() {}

	virtual IBaseDataMgr* getBaseDataMgr() override { return &bd_; }

	virtual void handleEvent(WTSTraderEvent e, int32_t ec) override { events_.push_back({ (int)e, ec }); }
	virtual void onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override
	{ logins_.push_back({ bSucc, msg ? msg : "", tradingdate }); }
	virtual void handleTraderLog(WTSLogLevel ll, const char* message) override
	{ logs_.push_back({ (int)ll, message ? message : "" }); }

	virtual void onPushOrder(WTSOrderInfo* orderInfo) override
	{
		++n_push_order_;
		if (orderInfo)
			fill_order(orderInfo);
	}

	virtual void onPushTrade(WTSTradeInfo* tradeRecord) override
	{
		++n_push_trade_;
		if (tradeRecord)
			fill_trade(tradeRecord);
	}

	virtual void onRspEntrust(WTSEntrust* entrust, WTSError* err) override
	{
		++n_rsp_entrust_;
		last_entrust_null_ = (entrust == NULL);
		if (err)
		{
			last_err_code_ = err->getErrorCode();
			last_err_msg_ = err->getMessage();
		}
	}

	virtual void onTraderError(WTSError* err, void* pData = NULL) override
	{
		(void)pData;
		++n_trader_error_;
		if (err)
		{
			last_err_code_ = err->getErrorCode();
			last_err_msg_ = err->getMessage();
		}
	}

	/// 数组在回调返回后就被产品侧 clear()/release()，所以必须在回调里取字段
	virtual void onRspOrders(const WTSArray* ayOrders) override
	{
		++n_rsp_orders_;
		got_null_orders_ = (ayOrders == NULL);
		if (ayOrders)
		{
			last_array_size_ = ayOrders->size();
			if (ayOrders->size() > 0)
				fill_order(static_cast<WTSOrderInfo*>(const_cast<WTSArray*>(ayOrders)->at(0)));
		}
	}

	virtual void onRspTrades(const WTSArray* ayTrades) override
	{
		++n_rsp_trades_;
		got_null_trades_ = (ayTrades == NULL);
		if (ayTrades)
		{
			last_array_size_ = ayTrades->size();
			if (ayTrades->size() > 0)
				fill_trade(static_cast<WTSTradeInfo*>(const_cast<WTSArray*>(ayTrades)->at(0)));
		}
	}

	virtual void onRspPosition(const WTSArray* ayPositions) override
	{
		++n_rsp_position_;
		if (ayPositions == NULL)
			return;
		last_array_size_ = ayPositions->size();
		if (ayPositions->size() == 0)
			return;
		WTSPositionItem* pos = static_cast<WTSPositionItem*>(const_cast<WTSArray*>(ayPositions)->at(0));
		last_pos_.valid = true;
		last_pos_.code = pos->getCode();
		last_pos_.currency = pos->getCurrency();
		last_pos_.exchg = pos->getExchg();
		last_pos_.dir = pos->getDirection();
		last_pos_.pre_pos = pos->getPrePosition();
		last_pos_.new_pos = pos->getNewPosition();
		last_pos_.avail_pre = pos->getAvailPrePos();
		last_pos_.avail_new = pos->getAvailNewPos();
		last_pos_.margin = pos->getMargin();
		last_pos_.avg_price = pos->getAvgPrice();
	}

	virtual void onRspAccount(WTSArray* ayAccounts) override
	{
		++n_rsp_account_;
		if (ayAccounts == NULL)
			return;
		last_array_size_ = ayAccounts->size();
		if (ayAccounts->size() == 0)
			return;
		WTSAccountInfo* acc = static_cast<WTSAccountInfo*>(ayAccounts->at(0));
		last_acc_.valid = true;
		last_acc_.currency = acc->getCurrency();
		last_acc_.pre_balance = acc->getPreBalance();
		last_acc_.balance = acc->getBalance();
		last_acc_.available = acc->getAvailable();
		last_acc_.commission = acc->getCommission();
		last_acc_.frozen_margin = acc->getFrozenMargin();
		last_acc_.deposit = acc->getDeposit();
		last_acc_.withdraw = acc->getWithdraw();
	}

	struct EventRec { int e; int32_t ec; };
	struct LoginRec { bool succ; std::string msg; uint32_t date; };
	struct LogRec { int ll; std::string msg; };
	struct OrderRec {
		bool valid = false; std::string code, exchg, entrust_id, order_id, user_tag, state_msg;
		double price = 0, volume = 0, vol_traded = 0, vol_left = 0;
		WTSDirectionType dir = WDT_NET; WTSOffsetType offset = WOT_FORCECLOSE;
		WTSPriceType pxtype = WPT_LASTPRICE; WTSOrderFlag oflag = WOF_NOR;
		WTSOrderState state = WOS_Nottouched; uint32_t order_date = 0; uint64_t order_time = 0;
		bool is_error = false;
	};
	struct TradeRec {
		bool valid = false; std::string code, exchg, trade_id, ref_order, user_tag;
		double price = 0, volume = 0, amount = 0;
		uint32_t trade_date = 0;
		WTSDirectionType dir = WDT_NET; WTSOffsetType offset = WOT_FORCECLOSE;
		WTSTradeType trade_type = WTT_OTC;
	};
	struct PosRec {
		bool valid = false; std::string code, currency, exchg;
		WTSDirectionType dir = WDT_NET;
		double pre_pos = 0, new_pos = 0, avail_pre = 0, avail_new = 0, margin = 0, avg_price = 0;
	};
	struct AccRec {
		bool valid = false; std::string currency;
		double pre_balance = 0, balance = 0, available = 0, commission = 0, frozen_margin = 0, deposit = 0, withdraw = 0;
	};

	void fill_order(WTSOrderInfo* o)
	{
		last_order_.valid = true;
		last_order_.code = o->getCode();
		last_order_.exchg = o->getExchg();
		last_order_.price = o->getPrice();
		last_order_.volume = o->getVolume();
		last_order_.vol_traded = o->getVolTraded();
		last_order_.vol_left = o->getVolLeft();
		last_order_.dir = o->getDirection();
		last_order_.offset = o->getOffsetType();
		last_order_.pxtype = o->getPriceType();
		last_order_.oflag = o->getOrderFlag();
		last_order_.state = o->getOrderState();
		last_order_.entrust_id = o->getEntrustID();
		last_order_.order_id = o->getOrderID();
		last_order_.user_tag = o->getUserTag();
		last_order_.state_msg = o->getStateMsg();
		last_order_.order_date = o->getOrderDate();
		last_order_.order_time = o->getOrderTime();
		last_order_.is_error = o->isError();
	}

	void fill_trade(WTSTradeInfo* t)
	{
		last_trade_.valid = true;
		last_trade_.code = t->getCode();
		last_trade_.exchg = t->getExchg();
		last_trade_.price = t->getPrice();
		last_trade_.volume = t->getVolume();
		last_trade_.amount = t->getAmount();
		last_trade_.trade_id = t->getTradeID();
		last_trade_.ref_order = t->getRefOrder();
		last_trade_.dir = t->getDirection();
		last_trade_.offset = t->getOffsetType();
		last_trade_.trade_type = t->getTradeType();
		last_trade_.user_tag = t->getUserTag();
		last_trade_.trade_date = t->getTradeDate();
	}

	std::vector<EventRec> events_;
	std::vector<LoginRec> logins_;
	std::vector<LogRec> logs_;
	OrderRec last_order_;
	TradeRec last_trade_;
	PosRec last_pos_;
	AccRec last_acc_;
	uint32_t n_push_order_ = 0;
	uint32_t n_push_trade_ = 0;
	uint32_t n_rsp_entrust_ = 0;
	uint32_t n_trader_error_ = 0;
	uint32_t n_rsp_orders_ = 0;
	uint32_t n_rsp_trades_ = 0;
	uint32_t n_rsp_position_ = 0;
	uint32_t n_rsp_account_ = 0;
	uint32_t last_array_size_ = 0;
	bool got_null_orders_ = false;
	bool got_null_trades_ = false;
	bool last_entrust_null_ = false;
	WTSErroCode last_err_code_ = WEC_NONE;
	std::string last_err_msg_;

	XTPFakeBdMgr bd_;
};

WTSVariant* makeXTVCfg(const char* flowdir)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("user", "xtpuser");
	cfg->append("pass", "xtppass");
	cfg->append("acckey", "acc-key");
	cfg->append("host", "127.0.0.1");
	cfg->append("port", (int32_t)11101);
	cfg->append("client", (int32_t)1);
	cfg->append("quick", true);
	cfg->append("hbinterval", (uint32_t)12);
	if (flowdir != NULL)
		cfg->append("flowdir", flowdir);
	return cfg;
}

class TraderXTPTest : public ::testing::Test
{
protected:
	TraderXTP api_;
	XTPSink sink_;
	WTSCommodityInfo* stk_comm_ = NULL;
	WTSContractInfo* stk_ct_ = NULL;
	WTSCommodityInfo* fut_comm_ = NULL;
	WTSContractInfo* fut_ct_ = NULL;

	void SetUp() override
	{
		WTSVariant* cfg = makeXTVCfg("xtp_t5_flow");
		ASSERT_TRUE(api_.init(cfg));
		cfg->release();

		// 现货分支（isStock 为真时方向恒 WDT_LONG）
		stk_ct_ = WTSContractInfo::create("600000", "浦发银行", "SSE", "stock600");
		stk_comm_ = WTSCommodityInfo::create("stock600", "股票", "SSE", "TRADE", "CHINA");
		stk_comm_->setCategory(CC_Stock);
		stk_comm_->setVolScale(1);
		sink_.bd_.regis(stk_comm_, stk_ct_);

		// 非现货分支（走 wrapDirectionType / wrapOffsetType）
		fut_ct_ = WTSContractInfo::create("IF2512", "沪深2512", "SSE", "IF");
		fut_comm_ = WTSCommodityInfo::create("IF", "股指", "SSE", "TRADE", "CHINA");
		fut_comm_->setCategory(CC_Future);
		fut_comm_->setVolScale(3);
		sink_.bd_.regis(fut_comm_, fut_ct_);

		api_.registerSpi(&sink_);
	}

	void TearDown() override
	{
		api_.release();
		if (stk_ct_) stk_ct_->release();
		if (stk_comm_) stk_comm_->release();
		if (fut_ct_) fut_ct_->release();
		if (fut_comm_) fut_comm_->release();
	}
};

/* ---------------------------------------------------------------- 用例 */

/// init 正配置 + _api 恒 NULL → 业务入口全部拒绝
TEST_F(TraderXTPTest, init_then_all_business_calls_rejected_without_api)
{
	EXPECT_FALSE(api_.isConnected());
	EXPECT_EQ(-1, api_.login("u", "p", ""));
	EXPECT_EQ(-1, api_.logout());

	WTSEntrust* ent = WTSEntrust::create("600000", 100, 10.5, "SSE");
	EXPECT_EQ(-1, api_.orderInsert(ent));
	ent->release();

	WTSEntrustAction* act = WTSEntrustAction::create("600000", "SSE");
	act->setEntrustID("xtpuser#20250102#1001");
	act->setOrderID("88990011");
	EXPECT_EQ(-1, api_.orderAction(act));
	act->release();

	EXPECT_TRUE(sink_.events_.empty());
	EXPECT_EQ(0u, sink_.n_push_order_);
}

/// init 负配置（缺 flowdir 回落默认值 XTPTDFlow）；release/disconnect 空状态可重复
TEST_F(TraderXTPTest, init_defaults_flowdir_and_release_is_repeatable)
{
	WTSVariant* cfg = makeXTVCfg(NULL);
	TraderXTP other;
	XTPSink other_sink;
	other.registerSpi(&other_sink);
	ASSERT_TRUE(other.init(cfg));
	cfg->release();

	other.release();
	other.release();
	other.disconnect();
	EXPECT_FALSE(other.isConnected());
	EXPECT_TRUE(other_sink.events_.empty());
}

/// makeEntrustID：非法缓冲 false；合法产出 "<user>#<tradingday>#<ref>"，ref 每次 +1
TEST_F(TraderXTPTest, make_entrust_id_format_and_reject)
{
	char tmp[16] = { 0 };
	EXPECT_FALSE(api_.makeEntrustID(NULL, 16));
	EXPECT_FALSE(api_.makeEntrustID(tmp, 0));

	const char* head = "xtpuser#0#";
	char buf[64] = { 0 };
	ASSERT_TRUE(api_.makeEntrustID(buf, sizeof(buf)));
	std::string first(buf);
	EXPECT_EQ(0u, first.find(head));                  // 未登录 → _tradingday 仍是 0
	const unsigned long a = strtoul(first.c_str() + strlen(head), NULL, 10);
	EXPECT_GT(a, 0ul);

	char buf2[64] = { 0 };
	ASSERT_TRUE(api_.makeEntrustID(buf2, sizeof(buf2)));
	const unsigned long b = strtoul(std::string(buf2).c_str() + strlen(head), NULL, 10);
	EXPECT_EQ(b, a + 1);
}

/// OnError：error_msg 直接进 sink 日志（产品侧把它当格式化模板用，见 bugs）
TEST_F(TraderXTPTest, on_error_routes_vendor_message_into_trader_log)
{
	XTPRI err;
	memset(&err, 0, sizeof(err));
	err.error_id = 40;
	snprintf(err.error_msg, sizeof(err.error_msg), "%s", "xtp said: session not ready");

	api_.OnError(&err);
	ASSERT_EQ(1u, sink_.logs_.size());
	EXPECT_NE(std::string::npos, sink_.logs_[0].msg.find("session not ready"));
	EXPECT_EQ((int)LL_ERROR, sink_.logs_[0].ll);

	api_.OnError(NULL);                               // 空指针 → 静默
	EXPECT_EQ(1u, sink_.logs_.size());
}

/// OnDisconnected → WTE_Close + reason 透传（post 只入队，用例不跑 io_context）
TEST_F(TraderXTPTest, on_disconnected_publishes_close_event_with_reason)
{
	api_.OnDisconnected(7, 3);
	ASSERT_EQ(1u, sink_.events_.size());
	EXPECT_EQ((int)WTE_Close, sink_.events_[0].e);
	EXPECT_EQ(3, sink_.events_[0].ec);
	EXPECT_FALSE(api_.isConnected());
}

/// OnCancelOrderError：error_id==0 视为无错（IsErrorInfo 守卫），非零才外发 WTSError
TEST_F(TraderXTPTest, cancel_error_guarded_by_error_id)
{
	XTPOrderCancelInfo cancel;
	memset(&cancel, 0, sizeof(cancel));
	XTPRI noerr;
	memset(&noerr, 0, sizeof(noerr));
	noerr.error_id = 0;
	snprintf(noerr.error_msg, sizeof(noerr.error_msg), "%s", "fine");

	api_.OnCancelOrderError(&cancel, &noerr, 1);
	EXPECT_EQ(0u, sink_.n_trader_error_);

	XTPRI real;
	memset(&real, 0, sizeof(real));
	real.error_id = 13;
	snprintf(real.error_msg, sizeof(real.error_msg), "%s", "order not found");
	api_.OnCancelOrderError(&cancel, &real, 1);
	ASSERT_EQ(1u, sink_.n_trader_error_);
	EXPECT_EQ((int)WEC_ORDERCANCEL, (int)sink_.last_err_code_);
	EXPECT_EQ("order not found", sink_.last_err_msg_);
}

/* ------------------------------------------------------ 回调结构 → WTS */

/// 现货订单回报（无错误）：market→SSE、isStock→WDT_LONG、非期权→side 决定开平
TEST_F(TraderXTPTest, order_event_without_error_converts_stock_order)
{
	XTPQueryOrderRsp ord;
	memset(&ord, 0, sizeof(ord));
	ord.market = XTP_MKT_SH_A;
	snprintf(ord.ticker, sizeof(ord.ticker), "%s", "600000");
	ord.price = 10.55;
	ord.quantity = 300;
	ord.qty_traded = 100;
	ord.qty_left = 200;
	ord.side = XTP_SIDE_BUY;
	ord.position_effect = XTP_POSITION_EFFECT_OPEN;
	ord.price_type = XTP_PRICE_LIMIT;
	ord.order_client_id = 4001;
	ord.order_xtp_id = 8123456;
	ord.order_status = XTP_ORDER_STATUS_PARTTRADEDQUEUEING;
	ord.insert_time = 20250102093015123ULL;

	api_.OnOrderEvent(&ord, NULL, 1);
	ASSERT_EQ(1u, sink_.n_push_order_);
	const XTPSink::OrderRec& o = sink_.last_order_;
	EXPECT_TRUE(o.valid);
	EXPECT_EQ("600000", o.code);
	EXPECT_EQ("SSE", o.exchg);
	EXPECT_DOUBLE_EQ(10.55, o.price);
	EXPECT_DOUBLE_EQ(300.0, o.volume);
	EXPECT_DOUBLE_EQ(100.0, o.vol_traded);
	EXPECT_DOUBLE_EQ(200.0, o.vol_left);
	EXPECT_EQ((int)WDT_LONG, (int)o.dir);              // 现货：方向恒为多
	EXPECT_EQ((int)WOT_OPEN, (int)o.offset);           // 非期权：BUY→开
	EXPECT_EQ((int)WPT_LIMITPRICE, (int)o.pxtype);
	EXPECT_EQ((int)WOF_NOR, (int)o.oflag);
	EXPECT_EQ((int)WOS_PartTraded_Queuing, (int)o.state);
	EXPECT_EQ("xtpuser#0#4001", o.entrust_id);         // genEntrustID(user#tradingday#client_id)
	EXPECT_EQ("8123456", o.order_id);
	EXPECT_EQ("xtpuser#0#4001", o.user_tag);           // eid 缓存未命中 → 回落 entrustID
	EXPECT_TRUE(o.state_msg.empty());
	EXPECT_FALSE(o.is_error);
	EXPECT_EQ(20250102u, o.order_date);                // insert_time / 1e9
}

/// SZSE 未登记 → 丢弃；REJECTED 状态 → default 状态 + 错误标记；SELL → 平
TEST_F(TraderXTPTest, order_event_szse_unregistered_and_rejected_mapping)
{
	XTPQueryOrderRsp ord;
	memset(&ord, 0, sizeof(ord));
	ord.market = XTP_MKT_SZ_A;
	snprintf(ord.ticker, sizeof(ord.ticker), "%s", "600000");
	ord.quantity = 100;
	ord.side = XTP_SIDE_SELL;
	ord.order_status = XTP_ORDER_STATUS_CANCELED;

	api_.OnOrderEvent(&ord, NULL, 1);
	EXPECT_EQ(0u, sink_.n_push_order_);                // SZSE 下没登记 600000
	EXPECT_EQ("SZSE", sink_.bd_.last_exchg_);

	ord.market = XTP_MKT_SH_A;
	ord.order_status = XTP_ORDER_STATUS_REJECTED;
	api_.OnOrderEvent(&ord, NULL, 1);
	ASSERT_EQ(1u, sink_.n_push_order_);
	EXPECT_EQ((int)WOS_Nottouched, (int)sink_.last_order_.state);   // REJECTED 不在状态表内 → default
	EXPECT_TRUE(sink_.last_order_.is_error);                       // >= REJECTED → WOEF_Normal
	EXPECT_EQ((int)WDT_LONG, (int)sink_.last_order_.dir);          // 现货
	EXPECT_EQ((int)WOT_CLOSE, (int)sink_.last_order_.offset);      // 非期权：SELL→平
}

/// 非现货分支：方向由 wrapDirectionType(side, position_effect) 决定
TEST_F(TraderXTPTest, order_event_non_stock_uses_wrap_tables)
{
	XTPQueryOrderRsp ord;
	memset(&ord, 0, sizeof(ord));
	ord.market = XTP_MKT_SH_A;
	snprintf(ord.ticker, sizeof(ord.ticker), "%s", "IF2512");
	ord.price = 3900.0;
	ord.quantity = 2;
	ord.side = XTP_SIDE_SELL;
	ord.position_effect = XTP_POSITION_EFFECT_OPEN;
	ord.price_type = XTP_PRICE_LIMIT;
	ord.order_client_id = 77;
	ord.order_status = XTP_ORDER_STATUS_ALLTRADED;
	ord.qty_traded = 2;
	ord.qty_left = 0;

	api_.OnOrderEvent(&ord, NULL, 1);
	ASSERT_EQ(1u, sink_.n_push_order_);
	EXPECT_EQ("IF2512", sink_.last_order_.code);
	// 期货不是 stock → wrapDirectionType(SELL, OPEN)=WDT_SHORT；非 option → SELL→WOT_CLOSE
	EXPECT_EQ((int)WDT_SHORT, (int)sink_.last_order_.dir);
	EXPECT_EQ((int)WOT_CLOSE, (int)sink_.last_order_.offset);
	EXPECT_EQ((int)WOS_AllTraded, (int)sink_.last_order_.state);
	EXPECT_EQ("xtpuser#0#77", sink_.last_order_.entrust_id);
	EXPECT_FALSE(sink_.last_order_.is_error);
}

/// 成交回报 → WTSTradeInfo（成交额 = quantity * price，日期从 trade_time 拆出）
TEST_F(TraderXTPTest, trade_event_converts_report_into_trade_info)
{
	XTPTradeReport trd;
	memset(&trd, 0, sizeof(trd));
	trd.market = XTP_MKT_SH_A;
	snprintf(trd.ticker, sizeof(trd.ticker), "%s", "600000");
	trd.price = 10.00;
	trd.quantity = 400;
	snprintf(trd.exec_id, sizeof(trd.exec_id), "%s", "EXEC-0001");
	trd.order_xtp_id = 8123456;
	trd.trade_time = 20250102143000500ULL;
	trd.side = XTP_SIDE_SELL;

	api_.OnTradeEvent(&trd, 1);
	ASSERT_EQ(1u, sink_.n_push_trade_);
	const XTPSink::TradeRec& t = sink_.last_trade_;
	EXPECT_EQ("600000", t.code);
	EXPECT_EQ("SSE", t.exchg);
	EXPECT_DOUBLE_EQ(10.0, t.price);
	EXPECT_DOUBLE_EQ(400.0, t.volume);
	EXPECT_DOUBLE_EQ(4000.0, t.amount);
	EXPECT_EQ("EXEC-0001", t.trade_id);
	EXPECT_EQ("8123456", t.ref_order);
	EXPECT_EQ((int)WDT_LONG, (int)t.dir);              // 现货
	EXPECT_EQ((int)WOT_CLOSE, (int)t.offset);          // SELL → 平
	EXPECT_EQ((int)WTT_Common, (int)t.trade_type);
	EXPECT_EQ(20250102u, t.trade_date);
}

/// 未知合约的成交回报 → makeTradeInfo NULL → 不外发也不崩
TEST_F(TraderXTPTest, trade_event_of_unregistered_contract_dropped)
{
	XTPTradeReport trd;
	memset(&trd, 0, sizeof(trd));
	trd.market = XTP_MKT_SZ_A;
	snprintf(trd.ticker, sizeof(trd.ticker), "%s", "000001");
	api_.OnTradeEvent(&trd, 1);
	EXPECT_EQ(0u, sink_.n_push_trade_);
	EXPECT_EQ("000001", sink_.bd_.last_code_);
}

/// 订单查询分页：非 last 累积、last 一次性外发
TEST_F(TraderXTPTest, query_order_accumulates_then_publishes_on_last)
{
	XTPQueryOrderRsp first;
	memset(&first, 0, sizeof(first));
	first.market = XTP_MKT_SH_A;
	snprintf(first.ticker, sizeof(first.ticker), "%s", "600000");
	first.quantity = 100;
	first.side = XTP_SIDE_BUY;
	first.order_client_id = 5001;
	first.order_status = XTP_ORDER_STATUS_NOTRADEQUEUEING;

	api_.OnQueryOrder(&first, NULL, 11, false, 1);
	EXPECT_EQ(0u, sink_.n_rsp_orders_);

	XTPQueryOrderRsp second;
	memcpy(&second, &first, sizeof(second));
	second.order_client_id = 5002;
	api_.OnQueryOrder(&second, NULL, 11, true, 1);
	ASSERT_EQ(1u, sink_.n_rsp_orders_);
	EXPECT_EQ(2u, sink_.last_array_size_);             // 两条都进了累积数组
	EXPECT_EQ("xtpuser#0#5001", sink_.last_order_.entrust_id);
	EXPECT_EQ((int)WOS_NotTraded_Queuing, (int)sink_.last_order_.state);

	// 上一轮之后 _orders 已 clear（数组对象保留）；空数据 + last 仍然外发
	api_.OnQueryOrder(NULL, NULL, 12, true, 1);
	ASSERT_EQ(2u, sink_.n_rsp_orders_);
	EXPECT_FALSE(sink_.got_null_orders_);
	EXPECT_EQ(0u, sink_.last_array_size_);
}

/// 成交查询：error_id≠0 的行被跳过；last 才外发
TEST_F(TraderXTPTest, query_trade_skips_row_when_error_info_present)
{
	XTPRI err;
	memset(&err, 0, sizeof(err));
	err.error_id = 9;
	snprintf(err.error_msg, sizeof(err.error_msg), "%s", "qry failed");

	XTPQueryTradeRsp row;
	memset(&row, 0, sizeof(row));
	row.market = XTP_MKT_SH_A;
	snprintf(row.ticker, sizeof(row.ticker), "%s", "600000");
	row.quantity = 100;
	row.price = 9.9;
	snprintf(row.exec_id, sizeof(row.exec_id), "%s", "E1");

	api_.OnQueryTrade(&row, &err, 21, true, 1);
	ASSERT_EQ(1u, sink_.n_rsp_trades_);
	EXPECT_TRUE(sink_.got_null_trades_);               // 行被跳过 → _trades 还没创建

	api_.OnQueryTrade(&row, NULL, 22, false, 1);
	EXPECT_EQ(1u, sink_.n_rsp_trades_);                // 非 last 不外发
	api_.OnQueryTrade(NULL, NULL, 22, true, 1);
	ASSERT_EQ(2u, sink_.n_rsp_trades_);
	EXPECT_FALSE(sink_.got_null_trades_);              // 这一轮 _trades 里有 1 条
	EXPECT_EQ(1u, sink_.last_array_size_);
	EXPECT_EQ("E1", sink_.last_trade_.trade_id);
}

/// 持仓查询：方向/昨仓今仓/保证金/可用
TEST_F(TraderXTPTest, query_position_maps_direction_and_availability)
{
	XTPQueryStkPositionRsp pos;
	memset(&pos, 0, sizeof(pos));
	pos.market = XTP_MKT_SH_A;
	snprintf(pos.ticker, sizeof(pos.ticker), "%s", "600000");
	pos.position_direction = XTP_POSITION_DIRECTION_LONG;
	pos.total_qty = 1000;
	pos.yesterday_position = 600;
	pos.sellable_qty = 600;
	pos.avg_price = 10.0;

	api_.OnQueryPosition(&pos, NULL, 31, true, 1);
	ASSERT_EQ(1u, sink_.n_rsp_position_);
	const XTPSink::PosRec& p = sink_.last_pos_;
	EXPECT_TRUE(p.valid);
	EXPECT_EQ("600000", p.code);
	EXPECT_EQ("CNY", p.currency);
	EXPECT_EQ("SSE", p.exchg);
	EXPECT_EQ((int)WDT_LONG, (int)p.dir);
	EXPECT_DOUBLE_EQ(600.0, p.pre_pos);
	EXPECT_DOUBLE_EQ(400.0, p.new_pos);                // total - yesterday
	EXPECT_DOUBLE_EQ(600.0, p.avail_pre);              // sellable_qty
	EXPECT_DOUBLE_EQ(0.0, p.avail_new);
	EXPECT_DOUBLE_EQ(10000.0, p.margin);               // total_qty * avg_price
	EXPECT_DOUBLE_EQ(10.0, p.avg_price);

	// 空头方向 + 非 last → 不外发
	pos.position_direction = XTP_POSITION_DIRECTION_SHORT;
	api_.OnQueryPosition(&pos, NULL, 32, false, 1);
	EXPECT_EQ(1u, sink_.n_rsp_position_);
}

/// 资金查询：只有 is_last 且有数据才外发；字段一一对应
TEST_F(TraderXTPTest, query_asset_maps_into_account_info)
{
	XTPQueryAssetRsp asset;
	memset(&asset, 0, sizeof(asset));
	asset.orig_banlance = 100000.0;
	asset.total_asset = 120000.0;
	asset.buying_power = 50000.0;
	asset.fund_sell_fee = 12.5;
	asset.withholding_amount = 300.0;
	asset.deposit_withdraw = 5000.0;

	api_.OnQueryAsset(&asset, NULL, 41, false, 1);
	EXPECT_EQ(0u, sink_.n_rsp_account_);               // 非 last → 丢弃

	api_.OnQueryAsset(NULL, NULL, 41, true, 1);
	EXPECT_EQ(0u, sink_.n_rsp_account_);               // 数据为空 → 丢弃

	api_.OnQueryAsset(&asset, NULL, 41, true, 1);
	ASSERT_EQ(1u, sink_.n_rsp_account_);
	const XTPSink::AccRec& a = sink_.last_acc_;
	EXPECT_TRUE(a.valid);
	EXPECT_EQ("CNY", a.currency);
	EXPECT_DOUBLE_EQ(100000.0, a.pre_balance);
	EXPECT_DOUBLE_EQ(120000.0, a.balance);
	EXPECT_DOUBLE_EQ(50000.0, a.available);
	EXPECT_DOUBLE_EQ(12.5, a.commission);
	EXPECT_DOUBLE_EQ(300.0, a.frozen_margin);
	EXPECT_DOUBLE_EQ(5000.0, a.deposit);
}

/// 负 deposit_withdraw 仅用于确认资产回报仍被转发；负号映射契约待厂商确认（T5-05）
TEST_F(TraderXTPTest, query_asset_negative_deposit_withdraw_forwards_account_without_sign_oracle)
{
	XTPQueryAssetRsp asset;
	memset(&asset, 0, sizeof(asset));
	asset.total_asset = 90000.0;
	asset.deposit_withdraw = -8000.0;

	api_.OnQueryAsset(&asset, NULL, 42, true, 1);
	ASSERT_EQ(1u, sink_.n_rsp_account_);
	EXPECT_TRUE(sink_.last_acc_.valid);
	EXPECT_DOUBLE_EQ(90000.0, sink_.last_acc_.balance);
	// XTP 头文件没有说明负值是否表示出金；不把它映射为 0 或 8000 写成合同。
}

/// 下单被拒（error_id≠0）且合约已登记：走 makeEntrust + onRspEntrust(entrust, error)
TEST_F(TraderXTPTest, order_event_with_error_converts_entrust_and_error)
{
	XTPRI err;
	memset(&err, 0, sizeof(err));
	err.error_id = 2;
	snprintf(err.error_msg, sizeof(err.error_msg), "%s", "price out of range");

	XTPOrderInfo ord;
	memset(&ord, 0, sizeof(ord));
	ord.market = XTP_MKT_SH_A;
	snprintf(ord.ticker, sizeof(ord.ticker), "%s", "IF2512");
	ord.price = 9999.0;
	ord.quantity = 3;
	ord.side = XTP_SIDE_BUY;
	ord.position_effect = XTP_POSITION_EFFECT_OPEN;
	ord.price_type = XTP_PRICE_LIMIT;
	ord.order_client_id = 60006;

	api_.OnOrderEvent(&ord, &err, 1);
	ASSERT_EQ(1u, sink_.n_rsp_entrust_);
	EXPECT_FALSE(sink_.last_entrust_null_);
	EXPECT_EQ((int)WEC_ORDERINSERT, (int)sink_.last_err_code_);
	EXPECT_EQ("price out of range", sink_.last_err_msg_);
}

/* ------------------------------------------------------- 不能进的路径 */

/// connect() → reconnect() 调用 m_funcCreator（本机恒 NULL）→ 空函数指针调用，必崩
TEST_F(TraderXTPTest, DISABLED_connect_calls_null_creator_and_writes_flow_dirs)
{
	api_.connect();
}

/// queryAccount/Positions/Orders/Trades 四个入口没有 _api 判空 → NULL 解引用
TEST_F(TraderXTPTest, DISABLED_query_apis_dereference_null_api_without_guard)
{
	EXPECT_EQ(0, api_.queryAccount());
}

/// OnOrderEvent 错误分支：makeEntrust 返回 NULL 时产品侧仍然 entrust->release() → NULL 解引用
TEST_F(TraderXTPTest, DISABLED_order_event_error_branch_releases_null_entrust)
{
	XTPRI err;
	memset(&err, 0, sizeof(err));
	err.error_id = 1;
	snprintf(err.error_msg, sizeof(err.error_msg), "%s", "bad order");
	XTPOrderInfo ord;
	memset(&ord, 0, sizeof(ord));
	ord.market = XTP_MKT_SZ_A;                         // 未登记 → makeEntrust 返回 NULL
	snprintf(ord.ticker, sizeof(ord.ticker), "%s", "999999");
	api_.OnOrderEvent(&ord, &err, 1);
}

/* ---------------------------------------------------------------- 工厂 */

TEST(xtp_plugin_factory, create_and_delete_roundtrip_null_safe)
{
	ITraderApi* api = createTrader();
	ASSERT_NE(nullptr, api);
	EXPECT_FALSE(api->isConnected());
	EXPECT_FALSE(api->makeEntrustID(NULL, 32));
	api->release();
	deleteTrader(api);
	EXPECT_EQ(nullptr, api);

	ITraderApi* nil = NULL;
	deleteTrader(nil);
	EXPECT_EQ(nullptr, nil);
}

} // namespace
