// Local, controllable endpoints for ParserXeleSkt lifecycle tests.
// - Snapshot channel: a loopback TCP listener on 127.0.0.1:<ephemeral>.
// - Market-data channel: the parser binds 0.0.0.0:<free port> and joins an
//   administratively scoped group on the loopback interface; the tests only
//   send *unicast* datagrams to 127.0.0.1:<port>. No multicast datagram is
//   sent and no vendor or exchange endpoint is contacted.
#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <dlfcn.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Includes/IParserApi.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "ParserXeleSkt/md_struct.h"

namespace xelefx {
using namespace wtp;
using namespace std::chrono_literals;

constexpr int kInstrumentNo = 11;
constexpr const char* kGroup = "239.255.77.77";

inline uint16_t freeUdpPort()
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) return 0;
	sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { close(fd); return 0; }
	socklen_t len = sizeof(a); if (getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0) { close(fd); return 0; }
	const uint16_t port = ntohs(a.sin_port); close(fd); return port;
}

inline std::string snapshotFrame(const char* code, int instrumentNo, double priceTick)
{
	std::string frame(sizeof(CXeleShfeMarketHead) + sizeof(CXeleShfeSnapShot), '\0');
	auto* head = reinterpret_cast<CXeleShfeMarketHead*>(&frame[0]);
	head->Version = XELE_MD_DATA_VERSION; head->Type = MESSAGE_SNAP_SHOT;
	auto* snap = reinterpret_cast<CXeleShfeSnapShot*>(&frame[sizeof(CXeleShfeMarketHead)]);
	std::strncpy(snap->InstrumentID, code, sizeof(snap->InstrumentID) - 1);
	snap->InstrumentNo = instrumentNo; snap->PriceTick = priceTick;
	return frame;
}

inline std::string marketDataFrame(int instrumentNo, int32_t lastPrice)
{
	std::string frame(sizeof(CXeleShfeMarketHead) + sizeof(CXeleShfeMarketData), '\0');
	auto* head = reinterpret_cast<CXeleShfeMarketHead*>(&frame[0]);
	head->Version = XELE_MD_DATA_VERSION; head->Type = MESSAGE_MARKET_DATA;
	auto* md = reinterpret_cast<CXeleShfeMarketData*>(&frame[sizeof(CXeleShfeMarketHead)]);
	md->InstrumentNo = static_cast<int16_t>(instrumentNo);
	md->SnapDateTime = (uint32_t(250000000) << 4); // ~2026, within the 28-bit field
	md->LastPrice = lastPrice; md->Volume = 1;
	return frame;
}

inline bool sendUnicast(uint16_t port, const std::string& bytes)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) return false;
	sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
	const auto sent = sendto(fd, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
	close(fd); return sent == static_cast<ssize_t>(bytes.size());
}

// Serves `connections` accepted TCP peers, each with `bytes` followed by EOF.
class SnapshotServer
{
public:
	explicit SnapshotServer(std::string bytes, int connections = 1) : _bytes(std::move(bytes)), _remaining(connections)
	{
		_fd = socket(AF_INET, SOCK_STREAM, 0);
		int one = 1; setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (::bind(_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 && listen(_fd, 4) == 0)
		{
			socklen_t len = sizeof(a); getsockname(_fd, reinterpret_cast<sockaddr*>(&a), &len); _port = ntohs(a.sin_port);
		}
		_thread = std::thread([this] { serve(); });
	}
	~SnapshotServer() { _quit = true; if (_thread.joinable()) _thread.join(); if (_fd >= 0) close(_fd); }
	uint16_t port() const { return _port; }
	int served() const { return _served.load(); }
private:
	void serve()
	{
		const auto deadline = std::chrono::steady_clock::now() + 10s;
		while (!_quit && _remaining > 0 && std::chrono::steady_clock::now() < deadline)
		{
			pollfd p{ _fd, POLLIN, 0 };
			if (poll(&p, 1, 10) <= 0) continue;
			int peer = accept(_fd, nullptr, nullptr); if (peer < 0) continue;
			std::size_t written = 0;
			while (written < _bytes.size())
			{
				const ssize_t n = write(peer, _bytes.data() + written, _bytes.size() - written);
				if (n <= 0) break; written += static_cast<std::size_t>(n);
			}
			shutdown(peer, SHUT_WR); close(peer); --_remaining; ++_served;
		}
	}
	std::string _bytes; int _remaining; int _fd = -1; uint16_t _port = 0;
	std::atomic<bool> _quit{ false }; std::atomic<int> _served{ 0 }; std::thread _thread;
};

class BaseData : public IBaseDataMgr
{
public:
	BaseData()
	{
		_commodity = WTSCommodityInfo::create("ag", "silver", "SHFE", "TRADING", "CHINA");
		_contract = WTSContractInfo::create("ag2512", "ag2512", "SHFE", "ag");
		_contract->setCommInfo(_commodity);
	}
	~BaseData() { _contract->release(); _commodity->release(); }
	WTSCommodityInfo* getCommodity(const char*) override { return nullptr; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return nullptr; }
	WTSContractInfo* getContract(const char* code, const char* = "", uint32_t = 0) override
	{ return code && std::strcmp(code, "ag2512") == 0 ? _contract : nullptr; }
	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return nullptr; }
	WTSSessionInfo* getSession(const char*) override { return nullptr; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return nullptr; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t date, uint32_t, bool = false) override { return date; }
	uint64_t getBoundaryTime(const char*, uint32_t date, bool = false, bool = true) override { return date; }
private:
	WTSCommodityInfo* _commodity; WTSContractInfo* _contract;
};

// Records quotes and Close events; an optional action runs inside handleQuote
// (on the parser worker) and may block until resume().
class Sink : public IParserSpi
{
public:
	BaseData base;
	std::function<void()> onQuote;
	bool hold = false;
	std::mutex mutex; std::condition_variable cv;
	int quotes = 0, closes = 0, connects = 0, logins = 0; bool resumed = false;
	std::vector<std::thread::id> quoteThreads;

	IBaseDataMgr* getBaseDataMgr() override { return &base; }
	void handleSymbolList(const WTSArray*) override {}
	void handleParserLog(WTSLogLevel, const char*) override {}
	void handleEvent(WTSParserEvent e, int32_t) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (e == WPE_Close) ++closes; else if (e == WPE_Connect) ++connects; else if (e == WPE_Login) ++logins;
		cv.notify_all();
	}
	void handleQuote(WTSTickData*, uint32_t) override
	{
		if (onQuote) onQuote();
		std::unique_lock<std::mutex> lock(mutex);
		++quotes; quoteThreads.push_back(std::this_thread::get_id()); cv.notify_all();
		if (hold) cv.wait(lock, [this] { return resumed; });
	}
	int quoteCount() { std::lock_guard<std::mutex> l(mutex); return quotes; }
	int closeCount() { std::lock_guard<std::mutex> l(mutex); return closes; }
	bool waitQuotes(int n, std::chrono::milliseconds t = 2s)
	{ std::unique_lock<std::mutex> l(mutex); return cv.wait_for(l, t, [&] { return quotes >= n; }); }
	bool waitCloses(int n, std::chrono::milliseconds t = 2s)
	{ std::unique_lock<std::mutex> l(mutex); return cv.wait_for(l, t, [&] { return closes >= n; }); }
	void resume() { std::lock_guard<std::mutex> l(mutex); resumed = true; cv.notify_all(); }
};

struct Config
{
	WTSVariant* value = WTSVariant::createObject();
	Config(uint16_t tcpPort, uint16_t mcastPort)
	{
		value->append("tcp_host", "127.0.0.1"); value->append("tcp_port", int32_t(tcpPort));
		value->append("mcast_host", kGroup); value->append("mcast_port", int32_t(mcastPort));
		value->append("local_host", "127.0.0.1"); value->append("gpsize", uint32_t(1000));
	}
	~Config() { value->release(); }
};

inline CodeSet subscription() { CodeSet s; s.insert("SHFE.ag2512"); return s; }

// Sends market data until `count` quotes are observed or the deadline passes.
inline bool pumpUntilQuotes(Sink& sink, uint16_t port, int count, std::chrono::milliseconds t = 2s)
{
	const auto deadline = std::chrono::steady_clock::now() + t;
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (!sendUnicast(port, marketDataFrame(kInstrumentNo, 100))) return false;
		if (sink.waitQuotes(count, 20ms)) return true;
	}
	return sink.waitQuotes(count, 0ms);
}

using FuncDeleteParser = void (*)(IParserApi*&);
} // namespace xelefx
