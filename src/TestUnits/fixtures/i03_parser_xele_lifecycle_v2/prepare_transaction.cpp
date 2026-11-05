// Snapshot transaction semantics under the card-9c single-init rule. The two
// existing R76 loopback tests re-init the same parser to change the TCP port,
// which is now rejected. Here one init points at one loopback port which
// serves successive payloads, so the same commit/rollback cases are checked
// without a second init. Built with the current source in its own executable
// so this friend definition does not collide with the R76 test's.
#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "ParserXeleSkt/ParserXeleSkt.h"
#include "ParserXeleSkt/md_struct.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"

USING_NS_WTP;
using namespace std::chrono_literals;

struct ParserXeleSktTestAccess
{
	static bool prepare(ParserXeleSkt& p) { return p.prepare(); }
	static bool reconnect(ParserXeleSkt& p) { return p.reconnect(); }
	static bool isPrepared(const ParserXeleSkt& p) { return p._prepared; }
	static bool hasTick(const ParserXeleSkt& p, int no) { return p._tick_cache && p._tick_cache->get(no); }
	static bool hasScale(const ParserXeleSkt& p, int no) { return p._price_scales.count(no) != 0; }
	static double scale(const ParserXeleSkt& p, int no) { auto it = p._price_scales.find(no); return it == p._price_scales.end() ? 0 : it->second; }
};

namespace {
std::string seed(const char* code, int no, double tick)
{
	std::string frame(sizeof(CXeleShfeMarketHead) + sizeof(CXeleShfeSnapShot), '\0');
	auto* head = reinterpret_cast<CXeleShfeMarketHead*>(&frame[0]);
	head->Version = XELE_MD_DATA_VERSION; head->Type = MESSAGE_SNAP_SHOT;
	auto* s = reinterpret_cast<CXeleShfeSnapShot*>(&frame[sizeof(CXeleShfeMarketHead)]);
	std::strncpy(s->InstrumentID, code, sizeof(s->InstrumentID) - 1); s->InstrumentNo = no; s->PriceTick = tick;
	return frame;
}
std::string fullReadThenReset(const std::string& frame)
{
	std::string bytes; while (bytes.size() + frame.size() <= 4096) bytes += frame;
	bytes.append(4096 - bytes.size(), 'R'); return bytes;
}

// One listening port; the n-th accepted peer gets payloads[n] then EOF, or RST.
class SequencedServer
{
public:
	explicit SequencedServer(std::vector<std::pair<std::string, bool>> payloads) : _payloads(std::move(payloads))
	{
		_fd = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (::bind(_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 && listen(_fd, 4) == 0)
		{ socklen_t len = sizeof(a); getsockname(_fd, reinterpret_cast<sockaddr*>(&a), &len); _port = ntohs(a.sin_port); }
		_thread = std::thread([this] { serve(); });
	}
	~SequencedServer() { _quit = true; _thread.join(); close(_fd); }
	uint16_t port() const { return _port; }
	int served() const { return _served.load(); }
private:
	void serve()
	{
		const auto deadline = std::chrono::steady_clock::now() + 10s;
		std::size_t next = 0;
		while (!_quit && next < _payloads.size() && std::chrono::steady_clock::now() < deadline)
		{
			pollfd p{ _fd, POLLIN, 0 }; if (poll(&p, 1, 10) <= 0) continue;
			int peer = accept(_fd, nullptr, nullptr); if (peer < 0) continue;
			const auto& item = _payloads[next++];
			std::size_t written = 0;
			while (written < item.first.size()) { ssize_t n = write(peer, item.first.data() + written, item.first.size() - written); if (n <= 0) break; written += size_t(n); }
			if (item.second) { std::this_thread::sleep_for(10ms); linger l{ 1, 0 }; setsockopt(peer, SOL_SOCKET, SO_LINGER, &l, sizeof(l)); }
			else shutdown(peer, SHUT_WR);
			close(peer); ++_served;
		}
	}
	std::vector<std::pair<std::string, bool>> _payloads; int _fd = -1; uint16_t _port = 0;
	std::atomic<bool> _quit{ false }; std::atomic<int> _served{ 0 }; std::thread _thread;
};

class Base : public IBaseDataMgr
{
public:
	Base() { for (const char* c : { "ag2512", "au2512", "cu2512" }) _c[c] = WTSContractInfo::create(c, c, "SHFE", "ag"); }
	~Base() { for (auto& i : _c) i.second->release(); }
	WTSCommodityInfo* getCommodity(const char*) override { return nullptr; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return nullptr; }
	WTSContractInfo* getContract(const char* code, const char* = "", uint32_t = 0) override
	{
		if (code && std::strcmp(code, "zz9999") == 0) throw std::runtime_error("injected contract lookup failure");
		auto it = _c.find(code ? code : ""); return it == _c.end() ? nullptr : it->second;
	}
	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return nullptr; }
	WTSSessionInfo* getSession(const char*) override { return nullptr; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return nullptr; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t d, uint32_t, bool = false) override { return d; }
	uint64_t getBoundaryTime(const char*, uint32_t d, bool = false, bool = true) override { return d; }
private:
	std::map<std::string, WTSContractInfo*> _c;
};
class Spi : public IParserSpi
{
public:
	Base base;
	IBaseDataMgr* getBaseDataMgr() override { return &base; }
	void handleSymbolList(const WTSArray*) override {}
	void handleQuote(WTSTickData*, uint32_t) override {}
	void handleParserLog(WTSLogLevel, const char*) override {}
};

void expectOnly(const ParserXeleSkt& p, int no, double tick)
{
	EXPECT_TRUE(ParserXeleSktTestAccess::hasTick(p, no));
	EXPECT_DOUBLE_EQ(tick, ParserXeleSktTestAccess::scale(p, no));
	for (int other : { 11, 22, 33 })
		if (other != no) { EXPECT_FALSE(ParserXeleSktTestAccess::hasTick(p, other)); EXPECT_FALSE(ParserXeleSktTestAccess::hasScale(p, other)); }
}
}

TEST(XelePrepareSingleInit, FailedSnapshotsKeepCommittedBatch)
{
	const std::string a = seed("ag2512", 11, 0.01), b = seed("au2512", 22, 0.02), c = seed("cu2512", 33, 0.03);
	SequencedServer server({ { a, false }, { fullReadThenReset(b), true }, { b + "tail", false },
		{ b + seed("zz9999", 99, 0.09), false }, { c, false } });
	Spi spi; ParserXeleSkt parser; parser.registerSpi(&spi);
	CodeSet subs; subs.insert("SHFE.ag2512"); subs.insert("SHFE.au2512"); subs.insert("SHFE.cu2512"); parser.subscribe(subs);
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("tcp_host", "127.0.0.1"); cfg->append("tcp_port", int32_t(server.port()));
	cfg->append("mcast_host", "239.255.77.77"); cfg->append("mcast_port", int32_t(0)); cfg->append("local_host", "127.0.0.1");
	ASSERT_TRUE(parser.init(cfg)); EXPECT_FALSE(parser.init(cfg)) << "card 9c"; cfg->release();

	ASSERT_TRUE(ParserXeleSktTestAccess::prepare(parser)); expectOnly(parser, 11, 0.01);
	EXPECT_FALSE(ParserXeleSktTestAccess::prepare(parser)) << "reset after a full read"; expectOnly(parser, 11, 0.01);
	EXPECT_FALSE(ParserXeleSktTestAccess::prepare(parser)) << "truncated tail"; expectOnly(parser, 11, 0.01);
	EXPECT_FALSE(ParserXeleSktTestAccess::prepare(parser)) << "throwing lookup"; expectOnly(parser, 11, 0.01);
	EXPECT_FALSE(ParserXeleSktTestAccess::isPrepared(parser));
	EXPECT_TRUE(ParserXeleSktTestAccess::reconnect(parser)) << "commit then loopback-interface group join";
	EXPECT_TRUE(ParserXeleSktTestAccess::isPrepared(parser)); expectOnly(parser, 33, 0.03);
	EXPECT_EQ(5, server.served());
	parser.release();
}
