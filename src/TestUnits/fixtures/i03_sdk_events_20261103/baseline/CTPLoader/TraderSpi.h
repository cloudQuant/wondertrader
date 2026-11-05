#pragma once
#ifdef __APPLE__
#include <API/CTP6.6.9/ThostFtdcTraderApi.h>
#else
//v6.3.15
#include <API/CTP6.3.15/ThostFtdcTraderApi.h>
#endif
#include "../Share/StdUtils.hpp"
#include "../Includes/LoaderDef.hpp"

#include <functional>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <map>
#include <chrono>
#include <boost/thread.hpp>

using namespace loader;

typedef std::function<bool()> QueryTask;

class CTraderSpi : public CThostFtdcTraderSpi
{
public:
	CTraderSpi():_stopped(false){}
	~CTraderSpi();

public:
	///当客户端与交易后台建立起通信连接时（还未登录前）,该方法被调用。
	virtual void OnFrontConnected() override;

	virtual void OnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	///登录请求响应
	virtual void OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin,	CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;
	
	///请求查询合约响应
	virtual void OnRspQryInstrument(CThostFtdcInstrumentField *pInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	virtual void OnRspQryInstrumentCommissionRate(CThostFtdcInstrumentCommissionRateField *pInstrumentCommissionRate, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	virtual void OnRspQryInstrumentMarginRate(CThostFtdcInstrumentMarginRateField *pInstrumentMarginRate, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	///错误应答
	virtual void OnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;
	
	///当客户端与交易后台通信连接断开时,该方法被调用。当发生这个情况后,API会自动重新连接,客户端可不做处理。
	virtual void OnFrontDisconnected(int nReason) override;

private:
	void ReqAuth();
	///用户登录请求
	void ReqUserLogin();
	///请求查询合约
	void ReqQryInstrument();

	void ReqQryCommission(const Contract& cInfo);

	void ReqQryMargin(const Contract& cInfo);

	// 是否收到成功的响应
	bool IsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo);

	void DumpToJson();

	bool DumpFees();

	void LoadFromJson();

	void AppendQuery(const QueryTask& task);
	void AppendFeeQuery(const QueryTask& task);
	void RunQueryWorker();
	bool IsQueryQueueEmpty();
	enum class FeeQueryKind { Instrument, Commission, Margin };
	struct FeeQueryState
	{
		FeeQueryKind kind;
		bool callReturned = false;
		bool terminal = false;
		std::chrono::steady_clock::time_point deadline;
		std::string code;
		std::string altcode;
		std::string product;
		std::string exchange;
	};
	int BeginFeeQuery(FeeQueryKind kind, const Contract* contract = nullptr);
	void FinishFeeSubmission(int requestId, int result);
	bool AcceptFeeCallback(int requestId, FeeQueryKind kind, bool responseError);
	bool FeeCallbackIdentityMatches(int requestId, const std::string& code, const std::string& exchange);
	bool RememberFeeOrigin(int requestId, std::map<std::string, std::string>& origins, const std::string& code);
	void FinishFeeCallback(int requestId, bool last);
	void FinishFeeBatchIfReady();
	bool FeeQueryExpiredLocked() const;
	void RunFeeWatchdog();

private:
	friend struct CTraderSpiQueueTestAccess;

	int		_trading_day;

	std::queue<QueryTask>	_queries;
	std::mutex		_mtx;
	std::condition_variable	_query_ready;
	std::condition_variable _fee_watchdog_ready;
	std::shared_ptr<boost::thread>	_worker;
	std::shared_ptr<boost::thread> _fee_watchdog;
	bool			_stopped;
	// A failed fee/margin response remains a failure for this loader instance.
	std::atomic<bool> _fee_response_failed{false};
	// Guards the callback-owned maps and prevents their mutation after a batch
	// has been claimed for publication. SDK calls never run under this lock.
	std::mutex _fee_callback_mutex;
	std::map<int, FeeQueryState> _fee_requests;
	std::map<std::string, std::string> _fee_origins;
	std::map<std::string, std::string> _margin_origins;
	size_t _fee_queued = 0;
	bool _fee_batch_active = false;
	bool _fee_batch_sealed = false;
	bool _fee_finish_claimed = false;
};
