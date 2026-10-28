/*
 * r93_xtp_api_stub.h — XTP::API::TraderApi（XTP2.2.32.2）的机械空实现
 *
 * 70 个纯虚函数逐条取自 src/API/XTP2.2.32.2/xtp_trader_api.h，
 * 只记录调用与最近一次请求，不碰 socket、不加载厂商库、不起线程。
 * 用例把 TraderXTP 的 _api 指向本类（测试 TU 以 #define private public 暴露），
 * 从而进入 connect/login/orderInsert/orderAction/query* 的正向组装分支（lane R93）。
 */
#pragma once

#include <cstring>
#include <string>

#include <API/XTP2.2.32.2/xtp_trader_api.h>

class R93XtpApiStub : public XTP::API::TraderApi
{
public:
	int		n_release_ = 0;
	int		n_register_spi_ = 0;
	int		n_subscribe_ = 0;
	int		n_set_version_ = 0;
	int		n_set_key_ = 0;
	int		n_set_hb_ = 0;
	int		n_login_ = 0;
	int		n_logout_ = 0;
	int		n_insert_ = 0;
	int		n_cancel_ = 0;
	int		n_query_asset_ = 0;
	int		n_query_position_ = 0;
	int		n_query_orders_ = 0;
	int		n_query_trades_ = 0;

	uint64_t	login_ret_ = 12345;
	uint64_t	insert_ret_ = 987650;
	uint64_t	cancel_ret_ = 987651;
	int			query_ret_ = 0;

	const char*	trading_day_ = "20260105";
	const char*	version_ = "xtp-stub-2.2";

	XTP::API::TraderSpi*	spi_ = NULL;
	XTPRI					last_error_;
	XTPOrderInsertInfo		insert_req_;
	XTPQueryOrderReq		order_query_;
	XTPQueryTraderReq		trade_query_;
	std::string				position_ticker_;
	std::string				software_version_;
	std::string				software_key_;
	uint32_t				heartbeat_ = 0;
	XTP_TE_RESUME_TYPE		resume_type_ = XTP_TERT_RESUME;

	std::string				login_ip_;
	int						login_port_ = 0;
	std::string				login_user_;
	std::string				login_pass_;
	XTP_PROTOCOL_TYPE		login_protocol_ = XTP_PROTOCOL_TCP;
	uint64_t				login_session_ = 0;
	uint64_t				logout_session_ = 0;
	uint64_t				insert_session_ = 0;
	uint64_t				cancel_order_id_ = 0;
	uint64_t				cancel_session_ = 0;
	uint64_t				query_session_ = 0;
	int						query_request_id_ = 0;

	R93XtpApiStub()
	{
		memset(&last_error_, 0, sizeof(last_error_));
		memset(&insert_req_, 0, sizeof(insert_req_));
		memset(&order_query_, 0, sizeof(order_query_));
		memset(&trade_query_, 0, sizeof(trade_query_));
	}
	virtual ~R93XtpApiStub() {}

	void setError(int id, const char* msg)
	{
		last_error_.error_id = id;
		strncpy(last_error_.error_msg, msg ? msg : "", sizeof(last_error_.error_msg) - 1);
	}

	virtual void Release() override { ++n_release_; }
	virtual const char* GetTradingDay() override { return trading_day_; }
	virtual void RegisterSpi(XTP::API::TraderSpi* spi) override { ++n_register_spi_; spi_ = spi; }
	virtual XTPRI* GetApiLastError() override { return &last_error_; }
	virtual const char* GetApiVersion() override { return version_; }
	virtual uint8_t GetClientIDByXTPID(uint64_t) override { return 0; }
	virtual const char* GetAccountByXTPID(uint64_t) override { return NULL; }
	virtual void SubscribePublicTopic(XTP_TE_RESUME_TYPE resume_type) override
	{
		++n_subscribe_;
		resume_type_ = resume_type;
	}
	virtual void SetSoftwareVersion(const char* version) override
	{
		++n_set_version_;
		software_version_ = version ? version : "";
	}
	virtual void SetSoftwareKey(const char* key) override
	{
		++n_set_key_;
		software_key_ = key ? key : "";
	}
	virtual void SetHeartBeatInterval(uint32_t interval) override
	{
		++n_set_hb_;
		heartbeat_ = interval;
	}
	virtual uint64_t Login(const char* ip, int port, const char* user, const char* password,
		XTP_PROTOCOL_TYPE sock_type, const char*) override
	{
		++n_login_;
		login_ip_ = ip ? ip : "";
		login_port_ = port;
		login_user_ = user ? user : "";
		login_pass_ = password ? password : "";
		login_protocol_ = sock_type;
		login_session_ = login_ret_;
		return login_ret_;
	}
	virtual int Logout(uint64_t session_id) override
	{
		++n_logout_;
		logout_session_ = session_id;
		return query_ret_;
	}
	virtual bool IsServerRestart(uint64_t) override { return false; }
	virtual int ModifyUserTerminalInfo(const XTPUserTerminalInfoReq*, uint64_t) override { return query_ret_; }
	virtual uint64_t InsertOrder(XTPOrderInsertInfo* order, uint64_t session_id) override
	{
		++n_insert_;
		if (order)
			insert_req_ = *order;
		insert_session_ = session_id;
		return insert_ret_;
	}
	virtual uint64_t CancelOrder(const uint64_t order_xtp_id, uint64_t session_id) override
	{
		++n_cancel_;
		cancel_order_id_ = order_xtp_id;
		cancel_session_ = session_id;
		return cancel_ret_;
	}
	virtual int QueryOrderByXTPID(const uint64_t, uint64_t, int) override { return query_ret_; }
	virtual int QueryOrders(const XTPQueryOrderReq* query_param, uint64_t session_id, int request_id) override
	{
		++n_query_orders_;
		if (query_param)
			order_query_ = *query_param;
		query_session_ = session_id;
		query_request_id_ = request_id;
		return query_ret_;
	}
	virtual int QueryUnfinishedOrders(uint64_t, int) override { return query_ret_; }
	virtual int QueryOrdersByPage(const XTPQueryOrderByPageReq*, uint64_t, int) override { return query_ret_; }
	virtual int QueryOrderByXTPIDEx(const uint64_t, uint64_t, int) override { return query_ret_; }
	virtual int QueryOrdersEx(const XTPQueryOrderReq*, uint64_t, int) override { return query_ret_; }
	virtual int QueryUnfinishedOrdersEx(uint64_t, int) override { return query_ret_; }
	virtual int QueryOrdersByPageEx(const XTPQueryOrderByPageReq*, uint64_t, int) override { return query_ret_; }
	virtual int QueryTradesByXTPID(const uint64_t, uint64_t, int) override { return query_ret_; }
	virtual int QueryTrades(XTPQueryTraderReq* query_param, uint64_t session_id, int request_id) override
	{
		++n_query_trades_;
		if (query_param)
			trade_query_ = *query_param;
		query_session_ = session_id;
		query_request_id_ = request_id;
		return query_ret_;
	}
	virtual int QueryTradesByPage(const XTPQueryTraderByPageReq*, uint64_t, int) override { return query_ret_; }
	virtual int QueryPosition(const char* ticker, uint64_t session_id, int request_id, XTP_MARKET_TYPE) override
	{
		++n_query_position_;
		position_ticker_ = ticker ? ticker : "";
		query_session_ = session_id;
		query_request_id_ = request_id;
		return query_ret_;
	}
	virtual int QueryAsset(uint64_t session_id, int request_id) override
	{
		++n_query_asset_;
		query_session_ = session_id;
		query_request_id_ = request_id;
		return query_ret_;
	}
	virtual int QueryStructuredFund(XTPQueryStructuredFundInfoReq*, uint64_t, int) override { return query_ret_; }
	virtual uint64_t FundTransfer(XTPFundTransferReq*, uint64_t) override { return 0; }
	virtual int QueryFundTransfer(XTPQueryFundTransferLogReq*, uint64_t, int) override { return query_ret_; }
	virtual int QueryOtherServerFund(XTPFundQueryReq*, uint64_t, int) override { return query_ret_; }
	virtual int QueryETF(XTPQueryETFBaseReq*, uint64_t, int) override { return query_ret_; }
	virtual int QueryETFTickerBasket(XTPQueryETFComponentReq*, uint64_t, int) override { return query_ret_; }
	virtual int QueryIPOInfoList(uint64_t, int) override { return query_ret_; }
	virtual int QueryIPOQuotaInfo(uint64_t, int) override { return query_ret_; }
	virtual int QueryOptionAuctionInfo(XTPQueryOptionAuctionInfoReq*, uint64_t, int) override { return query_ret_; }
	virtual uint64_t CreditCashRepay(double, uint64_t) override { return 0; }
	virtual uint64_t CreditCashRepayDebtInterestFee(const char*, double, uint64_t) override { return 0; }
	virtual uint64_t CreditSellStockRepayDebtInterestFee(XTPOrderInsertInfo*, const char*, uint64_t) override { return 0; }
	virtual int QueryCreditCashRepayInfo(uint64_t, int) override { return query_ret_; }
	virtual int QueryCreditFundInfo(uint64_t, int) override { return query_ret_; }
	virtual int QueryCreditDebtInfo(uint64_t, int) override { return query_ret_; }
	virtual int QueryCreditTickerDebtInfo(XTPClientQueryCrdDebtStockReq*, uint64_t, int) override { return query_ret_; }
	virtual int QueryCreditAssetDebtInfo(uint64_t, int) override { return query_ret_; }
	virtual int QueryCreditTickerAssignInfo(XTPClientQueryCrdPositionStockReq*, uint64_t, int) override { return query_ret_; }
	virtual int QueryCreditExcessStock(XTPClientQueryCrdSurplusStkReqInfo*, uint64_t, int) override { return query_ret_; }
	virtual int QueryMulCreditExcessStock(XTPClientQueryCrdSurplusStkReqInfo*, uint64_t, int) override { return query_ret_; }
	virtual uint64_t CreditExtendDebtDate(XTPCreditDebtExtendReq*, uint64_t) override { return 0; }
	virtual int QueryCreditExtendDebtDateOrders(uint64_t, uint64_t, int) override { return query_ret_; }
	virtual int QueryCreditFundExtraInfo(uint64_t, int) override { return query_ret_; }
	virtual int QueryCreditPositionExtraInfo(XTPClientQueryCrdPositionStockReq*, uint64_t, int) override { return query_ret_; }
	virtual uint64_t InsertOptionCombinedOrder(XTPOptCombOrderInsertInfo*, uint64_t) override { return 0; }
	virtual uint64_t CancelOptionCombinedOrder(const uint64_t, uint64_t) override { return 0; }
	virtual int QueryOptionCombinedUnfinishedOrders(uint64_t, int) override { return query_ret_; }
	virtual int QueryOptionCombinedOrderByXTPID(const uint64_t, uint64_t, int) override { return query_ret_; }
	virtual int QueryOptionCombinedOrders(const XTPQueryOptCombOrderReq*, uint64_t, int) override { return query_ret_; }
	virtual int QueryOptionCombinedOrdersByPage(const XTPQueryOptCombOrderByPageReq*, uint64_t, int) override { return query_ret_; }
	virtual int QueryOptionCombinedUnfinishedOrdersEx(uint64_t, int) override { return query_ret_; }
	virtual int QueryOptionCombinedOrderByXTPIDEx(const uint64_t, uint64_t, int) override { return query_ret_; }
	virtual int QueryOptionCombinedOrdersEx(const XTPQueryOptCombOrderReq*, uint64_t, int) override { return query_ret_; }
	virtual int QueryOptionCombinedOrdersByPageEx(const XTPQueryOptCombOrderByPageReq*, uint64_t, int) override { return query_ret_; }
	virtual int QueryOptionCombinedTradesByXTPID(const uint64_t, uint64_t, int) override { return query_ret_; }
	virtual int QueryOptionCombinedTrades(const XTPQueryOptCombTraderReq*, uint64_t, int) override { return query_ret_; }
	virtual int QueryOptionCombinedTradesByPage(const XTPQueryOptCombTraderByPageReq*, uint64_t, int) override { return query_ret_; }
	virtual int QueryOptionCombinedPosition(const XTPQueryOptCombPositionReq*, uint64_t, int) override { return query_ret_; }
	virtual int QueryOptionCombinedStrategyInfo(uint64_t, int) override { return query_ret_; }
	virtual int QueryOptionCombinedExecPosition(const XTPQueryOptCombExecPosReq*, uint64_t, int) override { return query_ret_; }
};
