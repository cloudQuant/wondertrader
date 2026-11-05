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
#include <functional>
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
#ifndef WT_T26_TEST_BASELINE
	// Join the fee watchdog once it has returned (it does right after publishing a timeout).
	static void joinWatchdog(CTraderSpi& s)
	{
		std::shared_ptr<boost::thread> w;
		{ std::lock_guard<std::mutex> l(s._mtx); w = s._fee_watchdog; }
		if (w && w->joinable() && w->get_id() != boost::this_thread::get_id()) w->join();
	}
#endif
};

#ifdef WT_T26_TEST_BASELINE
// 2026-11-05: the pre-card-14 baseline predates K02=B and ends the process with exit()
// inside the callback; it never publishes a result here. These shims only keep the probe
// compiling against it, so the baseline flow and exit codes are unchanged.
namespace loader_completion
{
	inline bool finished(int* = nullptr) { return false; }
	inline bool wait_for(int&, std::chrono::milliseconds timeout) { std::this_thread::sleep_for(timeout); return false; }
}
static void stopWorkers(CTraderSpi&) {}
#else
// 2026-11-05 (K02=B): the loader publishes its terminal result through loader_completion and
// no longer exits from the callback. Like the host teardown, stop and join its own threads
// (bounded) before ending the process with that result.
static void stopWorkers(CTraderSpi& spi)
{
	if (!spi.WaitWorkersStopped(std::chrono::seconds(2))) CTraderSpiQueueTestAccess::joinWatchdog(spi);
}
#endif

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
	// The process ends with the loader's first published result, exactly where the old
	// exit() inside the callback would have ended it (K02=B, see stopWorkers above).
	int code = 0;
	auto finish = [&](int c) { stopWorkers(spi); std::_Exit(c); };
	auto exitIfDone = [&]() { if (loader_completion::finished(&code)) finish(code); };
	auto waitResult = [&](std::chrono::milliseconds t, int fallback) {
		if (loader_completion::wait_for(code, t)) finish(code);
		return fallback;
	};
	auto waitApi = [&](const std::function<bool()>& pred) {
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
		std::unique_lock<std::mutex> l(api.mutex);
		while (!pred())
		{
			if (loader_completion::finished() || std::chrono::steady_clock::now() >= until) break;
			api.cv.wait_for(l, std::chrono::milliseconds(20));
		}
		const bool ok = pred();
		l.unlock();
		exitIfDone();
		return ok;
	};
	CTraderSpiQueueTestAccess::start(spi);
	exitIfDone();

	if (QRYFEES)
	{
		if (mode == "fee-empty-instrument") return waitResult(std::chrono::seconds(6), 91);
		if (!waitApi([&] { return api.commission && api.margin; })) return 92;
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		exitIfDone();
		// 已发送不等于已确认：两个费率请求都已发出但未回报时不能发布任何文件。
		if (!sentinel(argv[2], "fees.json") || !sentinel(argv[2], "contracts.json")) return 93;
		if (mode == "fee-reverse")
		{
			// 乱序：保证金先终结、未知请求号插入、佣金分两帧（数据帧 + 空终帧）。
			api.sendMargin(api.margin, true, true);
			api.sendCommission(99999, true, true);
			exitIfDone();
			if (!sentinel(argv[2], "fees.json")) return 94;
			api.sendCommission(api.commission, false, true);
			exitIfDone();
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
		return waitResult(std::chrono::seconds(5), 97);
	}

	// 非费率模式
	if (mode == "plain-send-failure" || mode == "plain-sync")
		return waitResult(std::chrono::seconds(6), 91);
	int id = 0;
	if (!waitApi([&] { return api.instrumentId != 0; })) return 92;
	{
		std::lock_guard<std::mutex> l(api.mutex);
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
	return waitResult(std::chrono::seconds(5), 97);
}
