// 迭代03 卡14（2026-11-03）BUG-T26-011：CTPLoader 请求完成收口的本机 fake 探针。
// 驱动真实 CTraderSpi 回调入口；SDK 为 t26_ctp_api_stub.hpp 替身，不连接柜台、不下单。
// 每个 mode 由 run_probe.py 在独立进程运行，按退出码与落盘文件判定。
#include "CTPLoader/TraderSpi.h"
#include "t26_ctp_api_stub.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

CThostFtdcTraderApi* pUserApi = nullptr;
std::string BROKER_ID, INVESTOR_ID, PASSWORD, AUTHCODE, APPID, SAVEPATH;
std::string COMM_FILE = "commodities.json", CONT_FILE = "contracts.json", FEES_FILE = "fees.json";
std::string FRONT_ADDR;
int iRequestID = 0;
uint32_t CLASSMASK = 1;
bool ONLYINCFG = false, INCREMENTAL = false, QRYFEES = true;
std::map<std::string, std::string> MAP_NAME, MAP_SESSION;
std::set<std::string> SET_FILTERS;
#ifdef WT_T26_FEES_TEST_HOOKS
bool wtT26FeesFailIo(int) { return false; }
#endif

struct CTraderSpiQueueTestAccess
{
	static void start(CTraderSpi& s) { s._trading_day = 20250102; s.ReqQryInstrument(); }
};

template <size_t N> void set(char (&out)[N], const char* value)
{
	std::strncpy(out, value, N - 1);
	out[N - 1] = '\0';
}

static CThostFtdcInstrumentField instrument()
{
	CThostFtdcInstrumentField f{};
	set(f.InstrumentID, "rb2510"); set(f.ProductID, "rb"); set(f.ExchangeID, "SHFE");
	set(f.InstrumentName, "steel"); set(f.OpenDate, "20170101"); set(f.ExpireDate, "20251231");
	f.ProductClass = THOST_FTDC_PC_Futures; f.VolumeMultiple = 10; f.PriceTick = 1;
	return f;
}

class API : public T26CtpApiStub
{
public:
	CTraderSpi* spi; std::string mode, path;
	std::mutex mutex; std::condition_variable cv;
	int instrumentId = 0, commission = 0, margin = 0;
	API(CTraderSpi* s, std::string m, std::string p) : spi(s), mode(std::move(m)), path(std::move(p)) {}

	int ReqQryInstrument(CThostFtdcQryInstrumentField*, int id) override
	{
		if (mode == "plain-send-failure") return -1;
		{ std::lock_guard<std::mutex> l(mutex); instrumentId = id; }
		cv.notify_all();
		// 费率模式和 plain-sync：合约回报在 SDK 调用返回前同步到达（已发送/已确认交错）。
		if (mode.rfind("fee-", 0) == 0 || mode == "plain-sync")
		{
			CThostFtdcInstrumentField f = instrument();
			if (mode == "fee-empty-instrument")
				spi->OnRspQryInstrument(nullptr, nullptr, id, true);
			else
				spi->OnRspQryInstrument(&f, nullptr, id, true);
		}
		return 0;
	}
	int ReqQryInstrumentCommissionRate(CThostFtdcQryInstrumentCommissionRateField*, int id) override
	{
		{ std::lock_guard<std::mutex> l(mutex); commission = id; }
		cv.notify_all();
		return 0;
	}
	int ReqQryInstrumentMarginRate(CThostFtdcQryInstrumentMarginRateField*, int id) override
	{
		{ std::lock_guard<std::mutex> l(mutex); margin = id; }
		cv.notify_all();
		return 0;
	}
	void sendCommission(int id, bool last, bool data)
	{
		CThostFtdcInstrumentCommissionRateField f{}; set(f.InstrumentID, "rb2510");
		f.OpenRatioByVolume = 1; f.CloseRatioByVolume = 1; f.CloseTodayRatioByVolume = 1;
		spi->OnRspQryInstrumentCommissionRate(data ? &f : nullptr, nullptr, id, last);
	}
	void sendMargin(int id, bool last, bool data)
	{
		CThostFtdcInstrumentMarginRateField f{}; set(f.InstrumentID, "rb2510");
		f.LongMarginRatioByMoney = 0.1; f.ShortMarginRatioByMoney = 0.1;
		spi->OnRspQryInstrumentMarginRate(data ? &f : nullptr, nullptr, id, last);
	}
};

static bool sentinel(const std::string& path, const char* file)
{
	std::ifstream f(path + "/" + file); std::string s; std::getline(f, s);
	return s == "old-sentinel";
}

int main(int argc, char** argv)
{
	if (argc != 3) return 90;
	const std::string mode = argv[1];
	SAVEPATH = std::string(argv[2]) + "/";
	QRYFEES = mode.rfind("fee-", 0) == 0;
	CTraderSpi spi; API api(&spi, mode, argv[2]); pUserApi = &api;
	CTraderSpiQueueTestAccess::start(spi);

	if (QRYFEES)
	{
		if (mode == "fee-empty-instrument") { std::this_thread::sleep_for(std::chrono::seconds(6)); return 91; }
		{
			std::unique_lock<std::mutex> l(api.mutex);
			if (!api.cv.wait_for(l, std::chrono::seconds(8), [&] { return api.commission && api.margin; })) return 92;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		// 已发送不等于已确认：两个费率请求都已发出但未回报时不能发布任何文件。
		if (!sentinel(argv[2], "fees.json") || !sentinel(argv[2], "contracts.json")) return 93;
		if (mode == "fee-reverse")
		{
			// 乱序：保证金先终结、未知请求号插入、佣金分两帧（数据帧 + 空终帧）。
			api.sendMargin(api.margin, true, true);
			api.sendCommission(99999, true, true);
			if (!sentinel(argv[2], "fees.json")) return 94;
			api.sendCommission(api.commission, false, true);
			if (!sentinel(argv[2], "fees.json")) return 95;
			api.sendCommission(api.commission, true, false);
		}
		else if (mode == "fee-empty-commission")
		{
			api.sendMargin(api.margin, true, true);
			api.sendCommission(api.commission, true, false);
		}
		else if (mode == "fee-empty-margin")
		{
			api.sendCommission(api.commission, true, true);
			api.sendMargin(api.margin, true, false);
		}
		else return 96;
		std::this_thread::sleep_for(std::chrono::seconds(5));
		return 97;
	}

	// 非费率模式
	if (mode == "plain-send-failure" || mode == "plain-sync")
	{
		std::this_thread::sleep_for(std::chrono::seconds(6));
		return 91;
	}
	int id = 0;
	{
		std::unique_lock<std::mutex> l(api.mutex);
		if (!api.cv.wait_for(l, std::chrono::seconds(8), [&] { return api.instrumentId != 0; })) return 92;
		id = api.instrumentId;
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	CThostFtdcInstrumentField f = instrument();
	CThostFtdcRspInfoField err{}; err.ErrorID = 7;
	if (mode == "plain-ok")
	{
		spi.OnRspQryInstrument(&f, nullptr, id, false);
		spi.OnRspQryInstrument(nullptr, nullptr, id, true);
	}
	else if (mode == "plain-error")
	{
		spi.OnRspQryInstrument(&f, nullptr, id, false);
		spi.OnRspQryInstrument(nullptr, &err, id, true);
	}
	else if (mode == "plain-empty")
	{
		spi.OnRspQryInstrument(nullptr, nullptr, id, true);
	}
	else if (mode == "plain-rsp-error")
	{
		spi.OnRspQryInstrument(&f, nullptr, id, false);
		spi.OnRspError(&err, id, true);
	}
	else if (mode == "plain-stale")
	{
		// 他号（迟到/未知）终帧先到，不能被当作本请求的完成。
		spi.OnRspQryInstrument(nullptr, nullptr, id + 100, true);
		spi.OnRspQryInstrument(&f, nullptr, id, true);
	}
	else return 96;
	std::this_thread::sleep_for(std::chrono::seconds(5));
	return 97;
}
