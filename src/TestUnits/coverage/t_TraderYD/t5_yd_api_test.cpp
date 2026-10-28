/*
 * T5 lane — src/TraderYD/TraderYD.cpp 覆盖率用例（收发族）
 *
 * 可测边界（实测源码）：
 *   - m_funcCreator 永远是 NULL（本机没有 libyd.dylib，DLLHelper::load_library 返回 NULL，
 *     get_symbol(NULL,..) 返回 NULL），所以 m_pUserAPI 恒为 NULL；
 *     因此 connect()/doLogin()/notifyLogin(0,..)/orderInsert 的发送分支不能进（必段崩溃），
 *     未登录/未就绪的**拒绝分支**反而可以确定性覆盖。
 *   - YDListener 的回调（notifyOrder/notifyTrade/notifyFailedOrder/notifyFailedCancelOrder/
 *     notifyReadyForLogin/notifyLogin(错误码)/notifyCaughtUp/notifyAccount）都是 public，
 *     可以手工构造厂商结构体直喂，覆盖「厂商回调结构 → WTS 结构」转换。
 *   - TraderYD 构造函数没有初始化 m_sink / m_lDate / m_orderRef / m_bUpdateCache
 *     （见 bugs/T5_yd_xtp_ctp_shm.md），所以每个用例都必须先 init()+registerSpi()
 *     再驱动回调，否则读到不确定值。
 * 不起线程、不 sleep、不连真柜台。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "TraderYD/TraderYD.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSDataDef.hpp"
#include "API/yd1.108.360/ydError.h"

USING_NS_WTP;

// TraderYD.cpp 里的插件工厂入口（本测试目标只有这一个收发族，不会与其它目录同名冲突）
extern "C"
{
	ITraderApi* createTrader();
	void deleteTrader(ITraderApi*& trader);
}

namespace {

/* ---------------------------------------------------------------- 替身 */

class YDFakeBdMgr : public IBaseDataMgr
{
public:
	virtual ~YDFakeBdMgr() {}

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
	virtual uint32_t getGlobalSize() override { return 8; }

	uint32_t n_get_contract_ = 0;
	std::string last_code_;
	std::string last_exchg_;

private:
	std::vector<WTSCommodityInfo*> _comms;
	std::vector<WTSContractInfo*> _cts;
	wt_hashmap<std::string, WTSContractInfo*> _ct_key;
};

/// ITraderSpi 替身：把每一次回调原样记下来（对象在回调返回后即被产品代码 release，故只存快照）
class YDSink : public ITraderSpi
{
public:
	virtual ~YDSink() {}

	virtual IBaseDataMgr* getBaseDataMgr() override { return &bd_; }

	virtual void handleEvent(WTSTraderEvent e, int32_t ec) override { events_.push_back({ (int)e, ec }); }

	virtual void onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override
	{ logins_.push_back({ bSucc, msg ? msg : "", tradingdate }); }

	virtual void handleTraderLog(WTSLogLevel ll, const char* message) override
	{ logs_.push_back({ (int)ll, message ? message : "" }); }

	virtual void onPushOrder(WTSOrderInfo* orderInfo) override
	{
		++n_push_order_;
		if (orderInfo == NULL)
			return;
		last_order_.valid = true;
		last_order_.code = orderInfo->getCode();
		last_order_.exchg = orderInfo->getExchg();
		last_order_.price = orderInfo->getPrice();
		last_order_.volume = orderInfo->getVolume();
		last_order_.vol_traded = orderInfo->getVolTraded();
		last_order_.vol_left = orderInfo->getVolLeft();
		last_order_.dir = orderInfo->getDirection();
		last_order_.offset = orderInfo->getOffsetType();
		last_order_.pxtype = orderInfo->getPriceType();
		last_order_.oflag = orderInfo->getOrderFlag();
		last_order_.state = orderInfo->getOrderState();
		last_order_.entrust_id = orderInfo->getEntrustID();
		last_order_.order_id = orderInfo->getOrderID();
		last_order_.user_tag = orderInfo->getUserTag();
		last_order_.order_date = orderInfo->getOrderDate();
	}

	virtual void onPushTrade(WTSTradeInfo* tradeRecord) override
	{
		++n_push_trade_;
		if (tradeRecord == NULL)
			return;
		last_trade_.valid = true;
		last_trade_.code = tradeRecord->getCode();
		last_trade_.exchg = tradeRecord->getExchg();
		last_trade_.price = tradeRecord->getPrice();
		last_trade_.volume = tradeRecord->getVolume();
		last_trade_.amount = tradeRecord->getAmount();
		last_trade_.trade_id = tradeRecord->getTradeID();
		last_trade_.ref_order = tradeRecord->getRefOrder();
		last_trade_.dir = tradeRecord->getDirection();
		last_trade_.offset = tradeRecord->getOffsetType();
		last_trade_.trade_type = tradeRecord->getTradeType();
		last_trade_.user_tag = tradeRecord->getUserTag();
	}

	virtual void onRspEntrust(WTSEntrust* entrust, WTSError* err) override
	{
		++n_rsp_entrust_;
		last_entrust_.valid = true;
		if (entrust)
		{
			last_entrust_.code = entrust->getCode();
			last_entrust_.exchg = entrust->getExchg();
			last_entrust_.price = entrust->getPrice();
			last_entrust_.volume = entrust->getVolume();
			last_entrust_.dir = entrust->getDirection();
			last_entrust_.offset = entrust->getOffsetType();
			last_entrust_.pxtype = entrust->getPriceType();
			last_entrust_.oflag = entrust->getOrderFlag();
			last_entrust_.entrust_id = entrust->getEntrustID();
			last_entrust_.user_tag = entrust->getUserTag();
		}
		if (err)
		{
			last_entrust_.err_code = err->getErrorCode();
			last_entrust_.err_msg = err->getMessage();
		}
	}

	virtual void onTraderError(WTSError* err, void* pData = NULL) override
	{
		(void)pData;
		++n_trader_error_;
		if (err == NULL)
			return;
		last_error_.code = err->getErrorCode();
		last_error_.msg = err->getMessage();
	}

	virtual void onRspAccount(WTSArray* ayAccounts) override { ++n_rsp_account_; last_account_size_ = ayAccounts ? ayAccounts->size() : 0; }
	virtual void onRspPosition(const WTSArray* ayPositions) override { ++n_rsp_position_; last_pos_size_ = ayPositions ? ayPositions->size() : 0; }
	virtual void onRspOrders(const WTSArray* ayOrders) override { ++n_rsp_orders_; last_ord_size_ = ayOrders ? ayOrders->size() : 0; }
	virtual void onRspTrades(const WTSArray* ayTrades) override { ++n_rsp_trades_; last_trd_size_ = ayTrades ? ayTrades->size() : 0; }

	struct EventRec { int e; int32_t ec; };
	struct LoginRec { bool succ; std::string msg; uint32_t date; };
	struct LogRec { int ll; std::string msg; };
	struct OrderRec {
		bool valid = false; std::string code, exchg, entrust_id, order_id, user_tag;
		double price = 0, volume = 0, vol_traded = 0, vol_left = 0;
		WTSDirectionType dir = WDT_NET; WTSOffsetType offset = WOT_FORCECLOSE;
		WTSPriceType pxtype = WPT_LASTPRICE; WTSOrderFlag oflag = WOF_NOR;
		WTSOrderState state = WOS_Nottouched; uint32_t order_date = 0;
	};
	struct TradeRec {
		bool valid = false; std::string code, exchg, trade_id, ref_order, user_tag;
		double price = 0, volume = 0, amount = 0;
		WTSDirectionType dir = WDT_NET; WTSOffsetType offset = WOT_FORCECLOSE;
		WTSTradeType trade_type = WTT_OTC;
	};
	struct EntrustRec {
		bool valid = false; std::string code, exchg, entrust_id, user_tag, err_msg;
		double price = 0, volume = 0;
		WTSDirectionType dir = WDT_NET; WTSOffsetType offset = WOT_FORCECLOSE;
		WTSPriceType pxtype = WPT_LASTPRICE; WTSOrderFlag oflag = WOF_NOR;
		WTSErroCode err_code = WEC_NONE;
	};
	struct ErrorRec { WTSErroCode code = WEC_NONE; std::string msg; };

	std::vector<EventRec> events_;
	std::vector<LoginRec> logins_;
	std::vector<LogRec> logs_;
	OrderRec last_order_;
	TradeRec last_trade_;
	EntrustRec last_entrust_;
	ErrorRec last_error_;
	uint32_t n_push_order_ = 0;
	uint32_t n_push_trade_ = 0;
	uint32_t n_rsp_entrust_ = 0;
	uint32_t n_trader_error_ = 0;
	uint32_t n_rsp_account_ = 0;
	uint32_t n_rsp_position_ = 0;
	uint32_t n_rsp_orders_ = 0;
	uint32_t n_rsp_trades_ = 0;
	uint32_t last_account_size_ = 0;
	uint32_t last_pos_size_ = 0;
	uint32_t last_ord_size_ = 0;
	uint32_t last_trd_size_ = 0;

	YDFakeBdMgr bd_;
};

WTSVariant* makeYDCfg(bool with_module, bool update_cache)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("config", "yd.conf");
	cfg->append("user", "yduser");
	cfg->append("pass", "ydpass");
	if (with_module)
		cfg->append("ydmodule", "yd");
	cfg->append("update_cache", update_cache);
	return cfg;
}

class TraderYDTest : public ::testing::Test
{
protected:
	TraderYD api_;
	YDSink sink_;
	WTSCommodityInfo* comm_ = NULL;
	WTSContractInfo* ct_ = NULL;

	void SetUp() override
	{
		WTSVariant* cfg = makeYDCfg(false, false);
		ASSERT_TRUE(api_.init(cfg));
		cfg->release();

		ct_ = WTSContractInfo::create("ag2512", "沪银2512", "SHFE", "ag");
		ct_->setTotalIndex(1);
		comm_ = WTSCommodityInfo::create("ag", "沪银", "SHFE", "TRADING", "CHINA");
		comm_->setCategory(CC_Future);
		comm_->setCoverMode(CM_CoverToday);
		comm_->setTradingMode(TM_Both);
		comm_->setVolScale(5);
		sink_.bd_.regis(comm_, ct_);

		api_.registerSpi(&sink_);
	}

	void TearDown() override
	{
		api_.release();
		if (ct_) ct_->release();
		if (comm_) comm_->release();
	}

	/// 用厂商结构体形状构造最小可用的 instrument/exchange 对
	void fillInstrument(YDInstrument& inst, YDExchange& exch, const char* code, const char* exchg)
	{
		memset(&inst, 0, sizeof(inst));
		memset(&exch, 0, sizeof(exch));
		strncpy(inst.InstrumentID, code, sizeof(inst.InstrumentID) - 1);
		strncpy(exch.ExchangeID, exchg, sizeof(exch.ExchangeID) - 1);
		inst.m_pExchange = &exch;
	}

	/// m_bCatchup=true 且 m_bUpdateCache=false：回调只走「转换 + 主推」，不碰本地持仓缓存
	void becomeCaughtUp() { api_.notifyCaughtUp(); }
};

/* ---------------------------------------------------------------- 用例 */

/// init 正/负配置：缺 ydmodule 也要回落默认模块名并返回 true；本机无 dylib → creator=NULL
TEST_F(TraderYDTest, init_accepts_config_and_falls_back_to_default_module)
{
	// 缺 ydmodule 的配置已在 SetUp 中通过（返回 true），这里补一条显式给了模块名的
	WTSVariant* cfg = makeYDCfg(true, true);
	TraderYD other;
	YDSink other_sink;
	other.registerSpi(&other_sink);
	ASSERT_TRUE(other.init(cfg));
	cfg->release();

	// creator 一定是 NULL：本机没有 libyd.dylib
	EXPECT_FALSE(other.isConnected());
	EXPECT_EQ(-1, other.login("u", "p", ""));
	EXPECT_EQ(-1, other.logout());
	other.release();
}

/// 未 connected（m_pUserAPI==NULL）时所有业务入口必须拒绝，且返回值可证伪
TEST_F(TraderYDTest, all_business_calls_rejected_without_api_instance)
{
	WTSEntrust* ent = WTSEntrust::create("ag2512", 2, 7000.0, "SHFE");
	ent->setDirection(WDT_LONG);
	ent->setOffsetType(WOT_OPEN);
	ent->setPriceType(WPT_LIMITPRICE);
	EXPECT_EQ(-1, api_.orderInsert(ent));
	ent->release();

	WTSEntrustAction* act = WTSEntrustAction::create();
	act->setEntrustID("yduser#0000000007");
	act->setOrderID("12345");
	EXPECT_EQ(-1, api_.orderAction(act));
	act->release();

	EXPECT_EQ(-1, api_.queryAccount());
	EXPECT_EQ(-1, api_.queryPositions());
	EXPECT_EQ(-1, api_.queryOrders());
	EXPECT_EQ(-1, api_.queryTrades());
	// 拒绝路径不得产生任何回报
	EXPECT_EQ(0u, sink_.n_push_order_);
	EXPECT_EQ(0u, sink_.n_rsp_account_);
}

/// makeEntrustID：缓冲区/长度非法必须 false；合法产出 "<user>#<10位零填充 orderRef>" 且自增
TEST_F(TraderYDTest, make_entrust_id_validates_buffer_and_increments_ref)
{
	EXPECT_FALSE(api_.makeEntrustID(NULL, 64));
	EXPECT_FALSE(api_.makeEntrustID(NULL, 0));

	char buf[64] = { 0 };
	char zero_len_buf[8] = { 0 };
	EXPECT_FALSE(api_.makeEntrustID(zero_len_buf, 0));

	ASSERT_TRUE(api_.makeEntrustID(buf, sizeof(buf)));
	std::string first(buf);
	EXPECT_EQ(0u, first.find("yduser#"));
	ASSERT_EQ(7u, first.find('#'));
	const std::string first_ref = first.substr(7);
	EXPECT_EQ(10u, first_ref.size());
	for (auto c : first_ref)
		EXPECT_TRUE(c >= '0' && c <= '9') << "ref part not digits: " << first_ref;

	char buf2[64] = { 0 };
	ASSERT_TRUE(api_.makeEntrustID(buf2, sizeof(buf2)));
	const std::string second(buf2);
	EXPECT_NE(first, second);
	EXPECT_EQ(0u, second.compare(0, 7, "yduser#"));
	// 两次 fetch_add(1) 之间恰好相差 1
	const unsigned long a = strtoul(first_ref.c_str(), NULL, 10);
	const unsigned long b = strtoul(second.substr(7).c_str(), NULL, 10);
	EXPECT_EQ(b, a + 1);
}

/// registerSpi 拉取 IBaseDataMgr；notifyReadyForLogin 必须翻成 WTE_Connect 事件
TEST_F(TraderYDTest, register_spi_then_ready_event)
{
	EXPECT_TRUE(sink_.events_.empty());
	api_.notifyReadyForLogin(false);
	ASSERT_EQ(1u, sink_.events_.size());
	EXPECT_EQ((int)WTE_Connect, sink_.events_[0].e);
	EXPECT_EQ(0, sink_.events_[0].ec);

	// 重新注册 NULL 后，事件不再外发（m_sink 为空则静默）
	api_.registerSpi(NULL);
	api_.notifyReadyForLogin(true);
	EXPECT_EQ(1u, sink_.events_.size());
}

/// notifyLogin 失败分支：状态置 LOGINFAILED、回 onLoginResult(false)，且不影响 isConnected
TEST_F(TraderYDTest, login_failure_callback_reports_and_keeps_disconnected)
{
	becomeCaughtUp();
	ASSERT_TRUE(api_.isConnected());

	api_.notifyLogin(YD_ERROR_PasswordError, 0, false);
	EXPECT_FALSE(api_.isConnected());           // WS_LOGINFAILED != WS_ALLREADY
	ASSERT_EQ(2u, sink_.logins_.size());        // 第 1 条来自 notifyCaughtUp
	EXPECT_FALSE(sink_.logins_[1].succ);
	EXPECT_STREQ("Login failed", sink_.logins_[1].msg.c_str());
	EXPECT_EQ(0u, sink_.logins_[1].date);
	bool logged_failed = false;
	for (auto& l : sink_.logs_)
		if (l.msg.find("Login failed: PasswordError") != std::string::npos)
			logged_failed = true;
	EXPECT_TRUE(logged_failed);
}

/// notifyLogin(0,..) 成功分支要解引用 m_pUserAPI（本机恒为 NULL）→ 必崩，留给主控
TEST_F(TraderYDTest, DISABLED_login_success_branch_needs_real_api)
{
	api_.notifyLogin(0, 100, false);
}

/// 未追上（m_bCatchup=false）时，废单/废撤回调必须被吞掉，不外发错误
TEST_F(TraderYDTest, failed_order_and_failed_cancel_ignored_before_catchup)
{
	YDInputOrder bad;
	memset(&bad, 0, sizeof(bad));
	bad.ErrorNo = YD_ERROR_NoMoneyToOpen;
	YDInstrument inst; YDExchange exch;
	fillInstrument(inst, exch, "ag2512", "SHFE");
	api_.notifyFailedOrder(&bad, &inst, NULL);

	YDFailedCancelOrder badc;
	memset(&badc, 0, sizeof(badc));
	badc.ErrorNo = YD_ERROR_OrderNotFound;
	api_.notifyFailedCancelOrder(&badc, &exch, NULL);

	EXPECT_EQ(0u, sink_.n_rsp_entrust_);
	EXPECT_EQ(0u, sink_.n_trader_error_);
}

/// notifyCaughtUp 幂等：只有第一次会回 onLoginResult(true)
TEST_F(TraderYDTest, caught_up_publishes_login_success_only_once)
{
	api_.notifyCaughtUp();
	api_.notifyCaughtUp();
	api_.notifyCaughtUp();
	ASSERT_EQ(1u, sink_.logins_.size());
	EXPECT_TRUE(sink_.logins_[0].succ);
	EXPECT_TRUE(api_.isConnected());
}

/// 追上之后：撤单失败的错误码→WTSError 映射（1028 被吞、其他外发 onTraderError）
TEST_F(TraderYDTest, failed_cancel_after_catchup_maps_error_code_to_wts_error)
{
	becomeCaughtUp();
	YDExchange exch;
	memset(&exch, 0, sizeof(exch));
	strncpy(exch.ExchangeID, "SHFE", sizeof(exch.ExchangeID) - 1);
	YDAccount acc;
	memset(&acc, 0, sizeof(acc));

	YDFailedCancelOrder ok1028;
	memset(&ok1028, 0, sizeof(ok1028));
	ok1028.ErrorNo = 1028;            // 待撤订单已成交，产品侧直接忽略
	api_.notifyFailedCancelOrder(&ok1028, &exch, &acc);
	EXPECT_EQ(0u, sink_.n_trader_error_);

	YDFailedCancelOrder real;
	memset(&real, 0, sizeof(real));
	real.ErrorNo = YD_ERROR_OrderNotFound;
	api_.notifyFailedCancelOrder(&real, &exch, &acc);
	ASSERT_EQ(1u, sink_.n_trader_error_);
	EXPECT_EQ((int)WEC_ORDERCANCEL, (int)sink_.last_error_.code);
	EXPECT_EQ("OrderNotFound", sink_.last_error_.msg.substr(0, 12));
	EXPECT_NE(std::string::npos, sink_.last_error_.msg.find("(" + std::to_string(YD_ERROR_OrderNotFound) + ")"));
}

/// 废单回报（追上后）：厂商 YDInputOrder → WTSEntrust + WTSError
TEST_F(TraderYDTest, failed_order_after_catchup_converts_input_order_into_entrust)
{
	becomeCaughtUp();

	YDInputOrder bad;
	memset(&bad, 0, sizeof(bad));
	bad.Direction = YD_D_Buy;
	bad.OffsetFlag = YD_OF_Open;
	bad.OrderType = YD_ODT_Limit;
	bad.OrderVolume = 3;
	bad.Price = 7010.5;
	bad.OrderRef = 88;
	bad.ErrorNo = YD_ERROR_InvalidOrderVolume;

	YDInstrument inst; YDExchange exch;
	fillInstrument(inst, exch, "ag2512", "SHFE");

	api_.notifyFailedOrder(&bad, &inst, NULL);
	ASSERT_EQ(1u, sink_.n_rsp_entrust_);
	EXPECT_TRUE(sink_.last_entrust_.valid);
	EXPECT_EQ("ag2512", sink_.last_entrust_.code);
	EXPECT_EQ("SHFE", sink_.last_entrust_.exchg);
	EXPECT_DOUBLE_EQ(7010.5, sink_.last_entrust_.price);
	EXPECT_DOUBLE_EQ(3.0, sink_.last_entrust_.volume);
	// wrapDirectionType(买,开) → WDT_LONG，wrapOffsetType(开) → WOT_OPEN，限价 → WPT_LIMITPRICE
	EXPECT_EQ((int)WDT_LONG, (int)sink_.last_entrust_.dir);
	EXPECT_EQ((int)WOT_OPEN, (int)sink_.last_entrust_.offset);
	EXPECT_EQ((int)WPT_LIMITPRICE, (int)sink_.last_entrust_.pxtype);
	EXPECT_EQ((int)WOF_NOR, (int)sink_.last_entrust_.oflag);
	// generateEntrustID：user#10位orderRef
	EXPECT_EQ("yduser#0000000088", sink_.last_entrust_.entrust_id);
	// 缓存未命中 → userTag 保持空（产品侧只有命中 eid 缓存才 set）
	EXPECT_TRUE(sink_.last_entrust_.user_tag.empty());
	EXPECT_EQ((int)WEC_ORDERINSERT, (int)sink_.last_entrust_.err_code);
	EXPECT_NE(std::string::npos, sink_.last_entrust_.err_msg.find("InvalidOrderVolume"));

	// 未知合约 → makeEntrust 返回 NULL → 走 onTraderError 而不是 onRspEntrust
	YDInstrument unknown; YDExchange exch2;
	fillInstrument(unknown, exch2, "cu9999", "SHFE");
	const uint32_t before_err = sink_.n_trader_error_;
	api_.notifyFailedOrder(&bad, &unknown, NULL);
	EXPECT_EQ(1u, sink_.n_rsp_entrust_);
	EXPECT_EQ(before_err + 1, sink_.n_trader_error_);
}

/// 订单回报：YDOrder → WTSOrderInfo 的字段映射（方向/开平/状态/数量拆分/错误标记）
TEST_F(TraderYDTest, order_notification_maps_yd_order_into_wts_order_info)
{
	becomeCaughtUp();

	YDOrder ord;
	memset(&ord, 0, sizeof(ord));
	ord.Direction = YD_D_Sell;
	ord.OffsetFlag = YD_OF_CloseToday;
	ord.OrderType = YD_ODT_FAK;
	ord.OrderVolume = 5;
	ord.TradeVolume = 2;
	ord.Price = 7088.0;
	ord.OrderStatus = YD_OS_Queuing;
	ord.OrderRef = 1234;
	ord.OrderSysID = 555;

	YDInstrument inst; YDExchange exch;
	fillInstrument(inst, exch, "ag2512", "SHFE");
	api_.notifyOrder(&ord, &inst, NULL);

	ASSERT_EQ(1u, sink_.n_push_order_);
	EXPECT_EQ("ag2512", sink_.last_order_.code);
	EXPECT_EQ("SHFE", sink_.last_order_.exchg);
	EXPECT_DOUBLE_EQ(7088.0, sink_.last_order_.price);
	EXPECT_DOUBLE_EQ(5.0, sink_.last_order_.volume);
	EXPECT_DOUBLE_EQ(2.0, sink_.last_order_.vol_traded);
	EXPECT_DOUBLE_EQ(3.0, sink_.last_order_.vol_left);
	// 卖+平今 → wrapDirectionType 返回 WDT_LONG（多头持仓被卖出）
	EXPECT_EQ((int)WDT_LONG, (int)sink_.last_order_.dir);
	EXPECT_EQ((int)WOT_CLOSETODAY, (int)sink_.last_order_.offset);
	EXPECT_EQ((int)WPT_LIMITPRICE, (int)sink_.last_order_.pxtype);
	EXPECT_EQ((int)WOF_FAK, (int)sink_.last_order_.oflag);
	EXPECT_EQ((int)WOS_NotTraded_Queuing, (int)sink_.last_order_.state);
	EXPECT_EQ("yduser#0000001234", sink_.last_order_.entrust_id);
	EXPECT_EQ("555", sink_.last_order_.order_id);
	// eid 缓存未命中 → userTag 回落为 entrustID
	EXPECT_EQ("yduser#0000001234", sink_.last_order_.user_tag);
}

/// 价格为 0 的市价/全价单与拒单错误标记
TEST_F(TraderYDTest, order_zero_price_maps_any_price_and_rejected_sets_error_flag)
{
	becomeCaughtUp();

	YDOrder ord;
	memset(&ord, 0, sizeof(ord));
	ord.Direction = YD_D_Buy;
	ord.OffsetFlag = YD_OF_Open;
	ord.OrderType = YD_ODT_Market;
	ord.Price = 0.0;
	ord.OrderVolume = 1;
	ord.OrderStatus = YD_OS_Rejected;
	ord.ErrorNo = YD_ERROR_PossibleSelfTrade;
	ord.OrderRef = 7;

	YDInstrument inst; YDExchange exch;
	fillInstrument(inst, exch, "ag2512", "SHFE");
	api_.notifyOrder(&ord, &inst, NULL);

	ASSERT_EQ(1u, sink_.n_push_order_);
	EXPECT_EQ((int)WPT_ANYPRICE, (int)sink_.last_order_.pxtype);   // price==0 → 全价
	EXPECT_EQ((int)WOF_NOR, (int)sink_.last_order_.oflag);         // 非 FAK/FOK
	EXPECT_EQ((int)WDT_LONG, (int)sink_.last_order_.dir);
	EXPECT_EQ((int)WOT_OPEN, (int)sink_.last_order_.offset);
	EXPECT_EQ((int)WOS_Canceled, (int)sink_.last_order_.state);    // Rejected → Canceled
}

/// 未登记的合约（getContract 返回 NULL）→ 不产生任何推送，且查询过正确的 (code, exchg)
TEST_F(TraderYDTest, order_of_unregistered_instrument_produces_no_push)
{
	becomeCaughtUp();

	YDOrder ord;
	memset(&ord, 0, sizeof(ord));
	ord.OrderStatus = YD_OS_Accepted;
	YDInstrument inst; YDExchange exch;
	fillInstrument(inst, exch, "unknown999", "DCE");
	api_.notifyOrder(&ord, &inst, NULL);

	EXPECT_EQ(0u, sink_.n_push_order_);
	EXPECT_EQ("unknown999", sink_.bd_.last_code_);
	EXPECT_EQ("DCE", sink_.bd_.last_exchg_);
}

/// 成交回报：YDTrade → WTSTradeInfo（成交额按 volScale 放大）
TEST_F(TraderYDTest, trade_notification_maps_amount_with_volume_scale)
{
	becomeCaughtUp();

	YDTrade trd;
	memset(&trd, 0, sizeof(trd));
	trd.Direction = YD_D_Buy;
	trd.OffsetFlag = YD_OF_Open;
	trd.Price = 7000.0;
	trd.Volume = 2;
	trd.TradeID = 987654;
	trd.OrderSysID = 555;
	trd.TradeTime = 93015;

	YDInstrument inst; YDExchange exch;
	fillInstrument(inst, exch, "ag2512", "SHFE");
	api_.notifyTrade(&trd, &inst, NULL);

	ASSERT_EQ(1u, sink_.n_push_trade_);
	EXPECT_EQ("ag2512", sink_.last_trade_.code);
	EXPECT_EQ("SHFE", sink_.last_trade_.exchg);
	EXPECT_DOUBLE_EQ(7000.0, sink_.last_trade_.price);
	EXPECT_DOUBLE_EQ(2.0, sink_.last_trade_.volume);
	EXPECT_DOUBLE_EQ(5.0 * 2.0 * 7000.0, sink_.last_trade_.amount);   // volScale=5
	EXPECT_EQ("987654", sink_.last_trade_.trade_id);
	EXPECT_EQ("555", sink_.last_trade_.ref_order);
	EXPECT_EQ((int)WDT_LONG, (int)sink_.last_trade_.dir);
	EXPECT_EQ((int)WOT_OPEN, (int)sink_.last_trade_.offset);
	EXPECT_EQ((int)WTT_Common, (int)sink_.last_trade_.trade_type);
	EXPECT_TRUE(sink_.last_trade_.user_tag.empty());
}

/// notifyAccount 的两条前置守卫：资金数组为空 / 厂商指针为空 → 直接返回，不外发
TEST_F(TraderYDTest, account_notification_guarded_by_empty_fund_array)
{
	YDAccount acc;
	memset(&acc, 0, sizeof(acc));
	acc.PreBalance = 123456.0;
	api_.notifyAccount(&acc);          // m_ayFunds == NULL → return
	api_.notifyAccount(NULL);          // 同上，且指针为空
	EXPECT_EQ(0u, sink_.n_rsp_account_);
	EXPECT_EQ(0u, sink_.events_.size());
}

/// 追上之后 state 已是 WS_ALLREADY，但 m_pUserAPI 仍为 NULL：
/// orderInsert 走 api 判空，orderAction 走 entrustID 解析失败——两条不同的拒绝分支
TEST_F(TraderYDTest, ready_state_without_api_still_rejects_insert_and_bad_entrustid)
{
	becomeCaughtUp();
	ASSERT_TRUE(api_.isConnected());

	WTSEntrust* ent = WTSEntrust::create("ag2512", 1, 7000.0, "SHFE");
	EXPECT_EQ(-1, api_.orderInsert(ent));    // m_pUserAPI == NULL
	ent->release();

	WTSEntrustAction* act = WTSEntrustAction::create();
	act->setEntrustID("yduser#111#222");     // 两个 '#' → extractEntrustID false
	EXPECT_EQ(-1, api_.orderAction(act));
	act->release();

	WTSEntrustAction* act2 = WTSEntrustAction::create();
	act2->setEntrustID("no_separator_here"); // 找不到 '#' → idx==-1 且 idx2==-1 → 相等 → 通过解析
	// 这条会往下走到 m_pUserAPI->getInstrumentByID，必崩；见 bugs（DISABLED_ 见下）
	act2->release();
}

/// 合法 entrustID + state=ALLREADY + api=NULL → 产品侧解引用 NULL，整条挂死/崩溃路径
TEST_F(TraderYDTest, DISABLED_order_action_with_valid_entrustid_dereferences_null_api)
{
	becomeCaughtUp();
	WTSEntrustAction* act = WTSEntrustAction::create();
	act->setEntrustID("yduser#0000000001");
	EXPECT_EQ(0, api_.orderAction(act));
	act->release();
}

/// 工厂入口：createTrader/deleteTrader 的成对语义（同一目标里只有 TraderYD 定义这对符号）
TEST(yd_plugin_factory, create_and_delete_roundtrip_null_safe)
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
