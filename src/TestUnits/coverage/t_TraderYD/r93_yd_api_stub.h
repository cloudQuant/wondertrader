/*
 * r93_yd_api_stub.h — YDApi（yd1.108.360）的机械空实现
 *
 * 纯虚函数逐条取自 src/API/yd1.108.360/ydApi.h 的 YDApi（不含 YDExtendedApi），
 * 只记录调用与最近一次请求，不碰 socket、不加载厂商库、不起线程。
 * 用例把 TraderYD 探针的 m_pUserAPI 指向本类，从而进入 login/orderInsert/
 * orderAction/query* 的正向组装分支（lane R93）。
 */
#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "TraderYD/TraderYD.h"

class R93YdApiStub : public YDApi
{
public:
	int		n_start_ = 0;
	int		n_destroy_ = 0;
	int		n_disconnect_ = 0;
	int		n_login_ = 0;
	int		n_insert_ = 0;
	int		n_cancel_ = 0;
	int		n_write_log_ = 0;

	bool	stub_login_ret_ = true;
	bool	stub_insert_ret_ = true;
	bool	stub_cancel_ret_ = true;

	int		trading_day_ = 20260105;
	const char* version_ = "yd-stub-1.0";

	YDListener* listener_ = NULL;
	std::string login_user_;
	std::string login_pass_;

	YDInputOrder	last_order_;
	const YDInstrument* last_order_inst_ = NULL;
	YDCancelOrder	last_cancel_;
	const YDExchange* last_cancel_exchg_ = NULL;
	const YDAccount* last_cancel_account_ = NULL;

	YDAccount		account_;
	YDInstrument	instrument_;
	YDExchange		exchange_;
	std::vector<YDPrePosition> pre_positions_;

	R93YdApiStub()
	{
		memset(&last_order_, 0, sizeof(last_order_));
		memset(&last_cancel_, 0, sizeof(last_cancel_));
		memset(&account_, 0, sizeof(account_));
		memset(&instrument_, 0, sizeof(instrument_));
		memset(&exchange_, 0, sizeof(exchange_));
		instrument_.m_pExchange = &exchange_;
	}

	virtual ~R93YdApiStub() {}

	virtual bool start(YDListener* pListener) override { ++n_start_; listener_ = pListener; return true; }
	virtual void startDestroy(void) override { ++n_destroy_; }
	virtual void disconnect(void) override { ++n_disconnect_; }
	virtual bool login(const char* username, const char* password, const char*, const char*) override
	{
		++n_login_;
		login_user_ = username ? username : "";
		login_pass_ = password ? password : "";
		return stub_login_ret_;
	}
	virtual bool insertOrder(YDInputOrder* pInputOrder, const YDInstrument* pInstrument, const YDAccount*) override
	{
		++n_insert_;
		if (pInputOrder != NULL)
			last_order_ = *pInputOrder;
		last_order_inst_ = pInstrument;
		return stub_insert_ret_;
	}
	virtual bool cancelOrder(YDCancelOrder* pCancelOrder, const YDExchange* pExchange, const YDAccount* pAccount) override
	{
		++n_cancel_;
		if (pCancelOrder != NULL)
			last_cancel_ = *pCancelOrder;
		last_cancel_exchg_ = pExchange;
		last_cancel_account_ = pAccount;
		return stub_cancel_ret_;
	}
	virtual bool insertQuote(YDInputQuote*, const YDInstrument*, const YDAccount*) override { return false; }
	virtual bool cancelQuote(YDCancelQuote*, const YDExchange*, const YDAccount*) override { return false; }
	virtual bool insertCombPositionOrder(YDInputOrder*, const YDCombPositionDef*, const YDAccount*) override { return false; }
	virtual bool insertOptionExecTogetherOrder(YDInputOrder*, const YDInstrument*, const YDInstrument*, const YDAccount*) override { return false; }
	virtual bool insertMultiOrders(unsigned, YDInputOrder[], const YDInstrument*[], const YDAccount*) override { return false; }
	virtual bool cancelMultiOrders(unsigned, YDCancelOrder[], const YDExchange*[], const YDAccount*) override { return false; }
	virtual bool insertMultiQuotes(unsigned, YDInputQuote[], const YDInstrument*[], const YDAccount*) override { return false; }
	virtual bool cancelMultiQuotes(unsigned, YDCancelQuote[], const YDExchange*[], const YDAccount*) override { return false; }
	virtual bool subscribe(const YDInstrument*) override { return false; }
	virtual bool unsubscribe(const YDInstrument*) override { return false; }
	virtual bool setTradingRight(const YDAccount*, const YDInstrument*, const YDProduct*, const YDExchange*, int) override { return false; }
	virtual bool alterMoney(const YDAccount*, int, double) override { return false; }
	virtual bool updateMarginRate(const YDUpdateMarginRate*) override { return false; }
	virtual bool changePassword(const char*, const char*, const char*) override { return false; }
	virtual bool selectConnections(const YDExchange*, unsigned long long) override { return false; }

	virtual bool hasFinishedInit(void) override { return true; }
	virtual int getSystemParamCount(void) override { return 0; }
	virtual const YDSystemParam* getSystemParam(int) override { return NULL; }
	virtual const YDSystemParam* getSystemParamByName(const char*, const char*) override { return NULL; }
	virtual int getExchangeCount(void) override { return 1; }
	virtual const YDExchange* getExchange(int) override { return &exchange_; }
	virtual const YDExchange* getExchangeByID(const char*) override { return &exchange_; }
	virtual int getProductCount(void) override { return 0; }
	virtual const YDProduct* getProduct(int) override { return NULL; }
	virtual const YDProduct* getProductByID(const char*) override { return NULL; }
	virtual int getInstrumentCount(void) override { return 1; }
	virtual const YDInstrument* getInstrument(int) override { return &instrument_; }
	virtual const YDInstrument* getInstrumentByID(const char*) override { return &instrument_; }
	virtual int getCombPositionDefCount(void) override { return 0; }
	virtual const YDCombPositionDef* getCombPositionDef(int) override { return NULL; }
	virtual const YDCombPositionDef* getCombPositionDefByID(const char*, int) override { return NULL; }
	virtual int getAccountCount(void) override { return 1; }
	virtual const YDAccount* getAccount(int) override { return &account_; }
	virtual const YDAccount* getAccountByID(const char*) override { return &account_; }
	virtual const YDAccount* getMyAccount(void) override { return &account_; }
	virtual int getPrePositionCount(void) override { return (int)pre_positions_.size(); }
	virtual const YDPrePosition* getPrePosition(int pos) override
	{
		if (pos < 0 || pos >= (int)pre_positions_.size())
			return NULL;
		return &pre_positions_[(size_t)pos];
	}
	virtual int getMarginRateCount(void) override { return 0; }
	virtual const YDMarginRate* getMarginRate(int) override { return NULL; }
	virtual int getCommissionRateCount(void) override { return 0; }
	virtual const YDCommissionRate* getCommissionRate(int) override { return NULL; }
	virtual const YDAccountExchangeInfo* getAccountExchangeInfo(const YDExchange*, const YDAccount*) override { return NULL; }
	virtual const YDAccountProductInfo* getAccountProductInfo(const YDProduct*, const YDAccount*) override { return NULL; }
	virtual const YDAccountInstrumentInfo* getAccountInstrumentInfo(const YDInstrument*, const YDAccount*) override { return NULL; }
	virtual int getGeneralRiskParamCount(void) override { return 0; }
	virtual const YDGeneralRiskParam* getGeneralRiskParam(int) override { return NULL; }

	virtual void writeLog(const char*, ...) override { ++n_write_log_; }
	virtual const char* getVersion(void) override { return version_; }
	virtual int getClientPacketHeader(YDPacketType, unsigned char*, int) override { return 0; }
	virtual int getTradingDay(void) override { return trading_day_; }
};
