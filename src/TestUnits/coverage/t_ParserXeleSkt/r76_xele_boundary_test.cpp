/*
 * lane R76 — ParserXeleSkt 帧处理与配置边界测试
 *
 * 目标文件：src/ParserXeleSkt/ParserXeleSkt.cpp（377 可执行行，missed 297）
 *
 * 既有 t2_parserxele_lifecycle_test.cpp 已覆盖插件工厂、init 合法/非法地址、
 * registerSpi 首次与替换、subscribe 幂等、unsubscribe 空实现、disconnect 未连接路径。
 * 本文件补：
 *   - 基于真实 SHFE 快照帧长度的 TCP 字节拼接 helper：单帧、多帧、分片、EOF 带数据、
 *     短尾和非法 InstrumentID 后有效帧；
 *   - IPv6 tcp_host 与 gpsize=0 的 init 组合；
 *   - 空或非法 mcast_host/local_host 在 init 中受控拒绝，connect 返回 false 且不创建 socket；
 *   - registerSpi(NULL) 之后下一次注册不再算「替换」，旧行为与新行为的分界；
 *   - 空订阅集合、含点标准码、重复订阅、退订空实现；
 *   - disconnect 在未 connect 时的幂等与「事件被 strand 推迟」契约。
 *
 * prepare() 的流处理与事务状态由本文件的 loopback TCP server 真实驱动；只验证本地
 * 协议帧和错误路径，不依赖易盛厂商环境。通过测试访问器直接调用 reconnect()，不启动
 * parser worker 线程；组播端点仅在完整 EOF 后触达，失败时不会进入组播初始化。
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/system/system_error.hpp>

#include "ParserXeleSkt/ParserXeleSkt.h"
#include "ParserXeleSkt/md_struct.h"

#include "Includes/FasterDefs.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/IParserApi.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"

USING_NS_WTP;

extern "C"
{
	IParserApi* createParser();
	void deleteParser(IParserApi*& parser);
}

struct ParserXeleSktTestAccess
{
	static bool prepare(ParserXeleSkt& parser) { return parser.prepare(); }
	static bool reconnect(ParserXeleSkt& parser) { return parser.reconnect(); }
	static bool isPrepared(const ParserXeleSkt& parser) { return parser._prepared; }

	static bool hasTick(const ParserXeleSkt& parser, int instrumentNo)
	{
		return parser._tick_cache != nullptr && parser._tick_cache->get(instrumentNo) != nullptr;
	}

	static std::string tickCode(const ParserXeleSkt& parser, int instrumentNo)
	{
		if (!hasTick(parser, instrumentNo))
			return std::string();
		WTSTickData* tick = static_cast<WTSTickData*>(parser._tick_cache->get(instrumentNo));
		return tick->getTickStruct().code;
	}

	static bool hasPriceScale(const ParserXeleSkt& parser, int instrumentNo)
	{
		return parser._price_scales.find(instrumentNo) != parser._price_scales.end();
	}

	static double priceScale(const ParserXeleSkt& parser, int instrumentNo)
	{
		auto it = parser._price_scales.find(instrumentNo);
		return it == parser._price_scales.end() ? 0.0 : it->second;
	}

	static void cleanup(ParserXeleSkt& parser)
	{
		if (parser._udp_socket != nullptr)
		{
			boost::system::error_code ec;
			parser._udp_socket->cancel(ec);
			parser._udp_socket->close(ec);
			delete parser._udp_socket;
			parser._udp_socket = nullptr;
		}
		if (parser._tick_cache != nullptr)
		{
			parser._tick_cache->release();
			parser._tick_cache = nullptr;
		}
	}
};

struct ParserXeleSktTestCleanup
{
	explicit ParserXeleSktTestCleanup(ParserXeleSkt& value) : parser(value) {}
	~ParserXeleSktTestCleanup() { ParserXeleSktTestAccess::cleanup(parser); }
	ParserXeleSkt& parser;
};

namespace r76xele
{

const std::size_t kXeleSnapshotSize = sizeof(CXeleShfeMarketHead) + sizeof(CXeleShfeSnapShot);

std::string makeXeleSnapshot(const char* instrumentCode,
	int8_t version = XELE_MD_DATA_VERSION, int8_t type = MESSAGE_SNAP_SHOT)
{
	std::string frame(kXeleSnapshotSize, '\0');
	CXeleShfeMarketHead* head = reinterpret_cast<CXeleShfeMarketHead*>(&frame[0]);
	head->Version = version;
	head->Type = type;

	CXeleShfeSnapShot* snapshot = reinterpret_cast<CXeleShfeSnapShot*>(
		&frame[sizeof(CXeleShfeMarketHead)]);
	if (instrumentCode == nullptr)
		std::memset(snapshot->InstrumentID, 'X', sizeof(snapshot->InstrumentID));
	else
		std::strncpy(snapshot->InstrumentID, instrumentCode, sizeof(snapshot->InstrumentID) - 1);
	return frame;
}

xele_skt_detail::SnapshotReadStatus processXeleRead(std::string& pending, const char* bytes,
	std::size_t byteCount, const boost::system::error_code& error,
	std::vector<std::string>& instrumentCodes)
{
	const bool readEof = error == boost::asio::error::eof;
	const bool readError = static_cast<bool>(error) && !readEof;
	return xele_skt_detail::processSnapshotReadResult(pending, bytes, byteCount,
		readEof, readError, kXeleSnapshotSize, [&](const char* frame, std::size_t) {
			const CXeleShfeMarketHead* head = reinterpret_cast<const CXeleShfeMarketHead*>(frame);
			if (head->Version != XELE_MD_DATA_VERSION || head->Type != MESSAGE_SNAP_SHOT)
				return false;

			const CXeleShfeSnapShot* snapshot = reinterpret_cast<const CXeleShfeSnapShot*>(
				frame + sizeof(CXeleShfeMarketHead));
			std::string instrumentCode;
			if (xele_skt_detail::tryGetInstrumentCode(snapshot->InstrumentID,
				sizeof(snapshot->InstrumentID), instrumentCode))
				instrumentCodes.push_back(instrumentCode);
			return true;
		});
}

std::string makeSeedSnapshot(const char* instrumentCode, int instrumentNo, double priceTick)
{
	std::string frame = makeXeleSnapshot(instrumentCode);
	CXeleShfeSnapShot* snapshot = reinterpret_cast<CXeleShfeSnapShot*>(
		&frame[sizeof(CXeleShfeMarketHead)]);
	snapshot->InstrumentNo = instrumentNo;
	snapshot->PriceTick = priceTick;
	return frame;
}

std::string makeFullReadThenResetPayload(const std::string& frame)
{
	const std::size_t readSize = 4096;
	if (frame.empty() || frame.size() >= readSize)
		throw std::invalid_argument("snapshot frame must fit in the parser read buffer");
	std::string bytes;
	while (bytes.size() + frame.size() <= readSize)
		bytes.append(frame);
	bytes.append(readSize - bytes.size(), 'R');
	return bytes;
}

class R76LoopbackBaseDataMgr : public IBaseDataMgr
{
public:
	virtual ~R76LoopbackBaseDataMgr()
	{
		for (auto& item : contracts_)
			item.second->release();
	}

	void addContract(const std::string& code)
	{
		contracts_[code] = WTSContractInfo::create(code.c_str(), code.c_str(), "SHFE", "ag");
	}

	void throwForCode(const std::string& code) { throw_codes_.insert(code); }

	virtual WTSCommodityInfo* getCommodity(const char*) override { return nullptr; }
	virtual WTSCommodityInfo* getCommodity(const char*, const char*) override { return nullptr; }
	virtual WTSContractInfo* getContract(const char* code, const char* = "", uint32_t = 0) override
	{
		const std::string key = code != nullptr ? code : "";
		if (throw_codes_.find(key) != throw_codes_.end())
			throw std::runtime_error("injected contract lookup failure");
		auto it = contracts_.find(key);
		return it == contracts_.end() ? nullptr : it->second;
	}
	virtual WTSArray* getContracts(const char* = "", uint32_t = 0) override { return nullptr; }
	virtual WTSSessionInfo* getSession(const char*) override { return nullptr; }
	virtual WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return nullptr; }
	virtual WTSArray* getAllSessions() override { return nullptr; }
	virtual bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	virtual uint32_t calcTradingDate(const char*, uint32_t date, uint32_t, bool = false) override { return date; }
	virtual uint64_t getBoundaryTime(const char*, uint32_t date, bool = false, bool = true) override { return date; }

private:
	std::map<std::string, WTSContractInfo*> contracts_;
	std::set<std::string> throw_codes_;
};

class R76LoopbackSnapshotServer
{
public:
	R76LoopbackSnapshotServer(const std::string& bytes, bool reset)
		: acceptor_(io_, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0))
		, port_(acceptor_.local_endpoint().port())
		, accepted_(false)
		, completed_(false)
	{
		boost::system::error_code ec;
		acceptor_.non_blocking(true, ec);
		if (ec)
			throw boost::system::system_error(ec);

		worker_ = std::thread([this, bytes, reset]() { serve(bytes, reset); });
	}

	~R76LoopbackSnapshotServer()
	{
		if (worker_.joinable())
			worker_.join();
	}

	uint16_t port() const { return port_; }

	bool joinAndReport()
	{
		if (worker_.joinable())
			worker_.join();
		return accepted_ && completed_;
	}

private:
	void serve(const std::string& bytes, bool reset)
	{
		using boost::asio::ip::tcp;
		struct AcceptorCloser
		{
			explicit AcceptorCloser(tcp::acceptor& value) : acceptor(value) {}
			~AcceptorCloser()
			{
				boost::system::error_code ignored;
				acceptor.close(ignored);
			}
			tcp::acceptor& acceptor;
		} closeAcceptor(acceptor_);

		tcp::socket peer(io_);
		boost::system::error_code ec;
		const auto acceptDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		for (;;)
		{
			acceptor_.accept(peer, ec);
			if (!ec)
			{
				accepted_ = true;
				boost::system::error_code closeError;
				acceptor_.close(closeError);
				break;
			}
			if (ec != boost::asio::error::would_block && ec != boost::asio::error::try_again)
				return;
			if (std::chrono::steady_clock::now() >= acceptDeadline)
			{
				boost::system::error_code closeError;
				acceptor_.close(closeError);
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		peer.non_blocking(true, ec);
		if (ec)
			return;

		const auto writeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		std::size_t written = 0;
		while (written < bytes.size())
		{
			const std::size_t count = peer.write_some(
				boost::asio::buffer(bytes.data() + written, bytes.size() - written), ec);
			written += count;
			if (written == bytes.size())
				break;
			if (!ec)
			{
				if (std::chrono::steady_clock::now() >= writeDeadline)
					return;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
			if (ec != boost::asio::error::would_block && ec != boost::asio::error::try_again)
				return;
			if (std::chrono::steady_clock::now() >= writeDeadline)
				return;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		if (reset)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			peer.set_option(boost::asio::socket_base::linger(true, 0), ec);
			if (ec)
				return;
			peer.close(ec);
			completed_ = !ec;
			return;
		}

		peer.shutdown(tcp::socket::shutdown_send, ec);
		if (ec)
			return;
		peer.close(ec);
		completed_ = !ec;
	}

	boost::asio::io_context io_;
	boost::asio::ip::tcp::acceptor acceptor_;
	uint16_t port_;
	std::thread worker_;
	bool accepted_;
	bool completed_;
};

/* ------------------------------------------------------------------ 替身 */

/// 只记录回调；getBaseDataMgr 返回 NULL（registerSpi 只存指针，不在此路径解引用）
class R76XeleSink : public IParserSpi
{
public:
	virtual IBaseDataMgr* getBaseDataMgr() override
	{
		++bd_calls;
		return nullptr;
	}

	virtual void handleSymbolList(const WTSArray*) override { ++symbol_lists; }
	virtual void handleQuote(WTSTickData*, uint32_t procFlag) override
	{
		++quotes;
		last_proc_flag = procFlag;
	}
	virtual void handleParserLog(WTSLogLevel, const char* message) override
	{
		logs.push_back(message != nullptr ? message : "");
	}
	virtual void handleEvent(WTSParserEvent, int32_t) override { ++events; }

	uint32_t bd_calls = 0;
	uint32_t symbol_lists = 0;
	uint32_t quotes = 0;
	uint32_t events = 0;
	uint32_t last_proc_flag = 0;
	std::vector<std::string> logs;
};

WTSVariant* makeCfg(const char* tcpHost, int32_t tcpPort, const char* mcastHost,
	int32_t mcastPort, const char* localHost, uint32_t gpsize)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("tcp_host", tcpHost);
	cfg->append("tcp_port", tcpPort);
	cfg->append("mcast_host", mcastHost);
	cfg->append("mcast_port", mcastPort);
	cfg->append("local_host", localHost);
	cfg->append("gpsize", gpsize);
	return cfg;
}

void expectAddressConfigRejected(const char* mcastHost, const char* localHost)
{
	ParserXeleSkt parser;
	WTSVariant* cfg = makeCfg("127.0.0.1", 9101, mcastHost, 9102, localHost, 100);
	bool initialized = true;
	EXPECT_NO_THROW(initialized = parser.init(cfg));
	EXPECT_FALSE(initialized);

	bool connected = true;
	EXPECT_NO_THROW(connected = parser.connect());
	EXPECT_FALSE(connected);
	EXPECT_FALSE(parser.isConnected());
	EXPECT_NO_THROW(parser.release());
	cfg->release();
}

class R76LoopbackSink : public R76XeleSink
{
public:
	virtual IBaseDataMgr* getBaseDataMgr() override
	{
		++bd_calls;
		return &base_data_mgr;
	}

	R76LoopbackBaseDataMgr base_data_mgr;
};

bool initLoopbackParser(ParserXeleSkt& parser, uint16_t tcpPort)
{
	WTSVariant* cfg = makeCfg("127.0.0.1", static_cast<int32_t>(tcpPort),
		"224.1.2.3", 0, "0.0.0.0", 100);
	const bool result = parser.init(cfg);
	cfg->release();
	return result;
}

void addLoopbackContracts(R76LoopbackSink& sink, ParserXeleSkt& parser)
{
	sink.base_data_mgr.addContract("ag2512");
	sink.base_data_mgr.addContract("au2512");
	sink.base_data_mgr.addContract("cu2512");
	sink.base_data_mgr.throwForCode("zz9999");
	parser.registerSpi(&sink);
	CodeSet subscriptions;
	subscriptions.insert("SHFE.ag2512");
	subscriptions.insert("SHFE.au2512");
	subscriptions.insert("SHFE.cu2512");
	parser.subscribe(subscriptions);
}

void expectOnlyCachedSeed(const ParserXeleSkt& parser, int instrumentNo,
	const char* code, double priceTick)
{
	EXPECT_TRUE(ParserXeleSktTestAccess::hasTick(parser, instrumentNo));
	EXPECT_EQ(code, ParserXeleSktTestAccess::tickCode(parser, instrumentNo));
	EXPECT_TRUE(ParserXeleSktTestAccess::hasPriceScale(parser, instrumentNo));
	EXPECT_DOUBLE_EQ(priceTick, ParserXeleSktTestAccess::priceScale(parser, instrumentNo));
	const int instrumentNos[] = { 11, 22, 33 };
	for (int candidateNo : instrumentNos)
	{
		if (candidateNo == instrumentNo)
			continue;
		EXPECT_FALSE(ParserXeleSktTestAccess::hasTick(parser, candidateNo));
		EXPECT_FALSE(ParserXeleSktTestAccess::hasPriceScale(parser, candidateNo));
	}
}

/* ------------------------------------------------------------------ 用例 */

TEST(R76ParserXeleSktSnapshotTest, ConsumesOneExactProtocolSnapshot)
{
	const std::string frame = makeXeleSnapshot("ag2512");
	std::string pending;
	std::vector<std::string> codes;

	EXPECT_EQ(xele_skt_detail::SnapshotReadStatus::continue_reading,
		processXeleRead(pending, frame.data(), frame.size(), boost::system::error_code(), codes));
	ASSERT_EQ(1u, codes.size());
	EXPECT_EQ("ag2512", codes[0]);
	EXPECT_TRUE(pending.empty());
}

TEST(R76ParserXeleSktSnapshotTest, ConsumesMultipleProtocolSnapshots)
{
	const std::string frames = makeXeleSnapshot("ag2512") + makeXeleSnapshot("au2512");
	std::string pending;
	std::vector<std::string> codes;

	EXPECT_EQ(xele_skt_detail::SnapshotReadStatus::continue_reading,
		processXeleRead(pending, frames.data(), frames.size(), boost::system::error_code(), codes));
	ASSERT_EQ(2u, codes.size());
	EXPECT_EQ("ag2512", codes[0]);
	EXPECT_EQ("au2512", codes[1]);
	EXPECT_TRUE(pending.empty());
}

TEST(R76ParserXeleSktSnapshotTest, InvalidVersionIsFatalAndDoesNotProcessFollowingFrame)
{
	const std::string frames = makeXeleSnapshot("bad", XELE_MD_DATA_VERSION + 1) +
		makeXeleSnapshot("ag2512");
	std::string pending;
	std::vector<std::string> codes;

	EXPECT_EQ(xele_skt_detail::SnapshotReadStatus::invalid_frame,
		processXeleRead(pending, frames.data(), frames.size(), boost::system::error_code(), codes));
	EXPECT_TRUE(codes.empty());
	EXPECT_TRUE(pending.empty());
}

TEST(R76ParserXeleSktSnapshotTest, InvalidTypeIsFatalAndDoesNotProcessFollowingFrame)
{
	const std::string frames = makeXeleSnapshot("bad", XELE_MD_DATA_VERSION, MESSAGE_DEPTH) +
		makeXeleSnapshot("ag2512");
	std::string pending;
	std::vector<std::string> codes;

	EXPECT_EQ(xele_skt_detail::SnapshotReadStatus::invalid_frame,
		processXeleRead(pending, frames.data(), frames.size(), boost::system::error_code(), codes));
	EXPECT_TRUE(codes.empty());
	EXPECT_TRUE(pending.empty());
}

TEST(R76ParserXeleSktSnapshotTest, ResetConsumesReturnedBytesButFailsRead)
{
	const std::string frame = makeXeleSnapshot("ag2512");
	std::string pending;
	std::vector<std::string> codes;

	EXPECT_EQ(xele_skt_detail::SnapshotReadStatus::io_error,
		processXeleRead(pending, frame.data(), frame.size(),
			boost::system::error_code(boost::asio::error::connection_reset), codes));
	ASSERT_EQ(1u, codes.size());
	EXPECT_EQ("ag2512", codes[0]);
	EXPECT_TRUE(pending.empty());
}

TEST(R76ParserXeleSktSnapshotTest, InvalidInstrumentIdDoesNotHideFollowingSnapshot)
{
	const std::string frames = makeXeleSnapshot(nullptr) + makeXeleSnapshot("ag2512");
	std::string pending;
	std::vector<std::string> codes;

	EXPECT_EQ(xele_skt_detail::SnapshotReadStatus::continue_reading,
		processXeleRead(pending, frames.data(), frames.size(), boost::system::error_code(), codes));
	ASSERT_EQ(1u, codes.size());
	EXPECT_EQ("ag2512", codes[0]);
	EXPECT_TRUE(pending.empty());
}

TEST(R76ParserXeleSktSnapshotTest, RetainsSplitFrameAndShortTailUntilMoreBytesArrive)
{
	const std::string firstFrame = makeXeleSnapshot("ag2512");
	const std::string secondFrame = makeXeleSnapshot("au2512");
	const std::string thirdFrame = makeXeleSnapshot("cu2512");
	const std::size_t split = firstFrame.size() / 2;
	const std::size_t shortTailSize = 7;
	std::string pending;
	std::vector<std::string> codes;

	EXPECT_EQ(xele_skt_detail::SnapshotReadStatus::continue_reading,
		processXeleRead(pending, firstFrame.data(), split, boost::system::error_code(), codes));
	EXPECT_TRUE(codes.empty());
	EXPECT_EQ(split, pending.size());

	std::string nextBytes = firstFrame.substr(split) + secondFrame + thirdFrame.substr(0, shortTailSize);
	EXPECT_EQ(xele_skt_detail::SnapshotReadStatus::continue_reading,
		processXeleRead(pending, nextBytes.data(), nextBytes.size(), boost::system::error_code(), codes));
	ASSERT_EQ(2u, codes.size());
	EXPECT_EQ("ag2512", codes[0]);
	EXPECT_EQ("au2512", codes[1]);
	EXPECT_EQ(shortTailSize, pending.size());

	EXPECT_EQ(xele_skt_detail::SnapshotReadStatus::continue_reading,
		processXeleRead(pending, thirdFrame.data() + shortTailSize,
			thirdFrame.size() - shortTailSize, boost::system::error_code(), codes));
	ASSERT_EQ(3u, codes.size());
	EXPECT_EQ("cu2512", codes[2]);
	EXPECT_TRUE(pending.empty());
}

TEST(R76ParserXeleSktSnapshotTest, EofWithShortTailFailsAfterProcessingCompleteSnapshots)
{
	const std::string frame = makeXeleSnapshot("ag2512");
	const std::string shortTail = "tail";
	const std::string bytes = frame + shortTail;
	std::string pending;
	std::vector<std::string> codes;

	EXPECT_EQ(xele_skt_detail::SnapshotReadStatus::truncated_frame,
		processXeleRead(pending, bytes.data(), bytes.size(),
			boost::system::error_code(boost::asio::error::eof), codes));
	ASSERT_EQ(1u, codes.size());
	EXPECT_EQ("ag2512", codes[0]);
	EXPECT_EQ(shortTail, pending);
}

TEST(R76ParserXeleSktSnapshotTest, CompleteEofProcessesFramesAndReportsSuccess)
{
	const std::string frames = makeXeleSnapshot("ag2512") + makeXeleSnapshot("au2512");
	std::string pending;
	std::vector<std::string> codes;

	EXPECT_EQ(xele_skt_detail::SnapshotReadStatus::complete,
		processXeleRead(pending, frames.data(), frames.size(),
			boost::system::error_code(boost::asio::error::eof), codes));
	ASSERT_EQ(2u, codes.size());
	EXPECT_EQ("ag2512", codes[0]);
	EXPECT_EQ("au2512", codes[1]);
	EXPECT_TRUE(pending.empty());
}

TEST(R76ParserXeleSktLoopbackTest, PrepareCommitsOnlyCompleteTcpSnapshotBatches)
{
	R76LoopbackSink sink;
	ParserXeleSkt parser;
	ParserXeleSktTestCleanup cleanup(parser);
	addLoopbackContracts(sink, parser);

	{
		const std::string seedA = makeSeedSnapshot("ag2512", 11, 0.01);
		R76LoopbackSnapshotServer server(seedA, false);
		ASSERT_TRUE(initLoopbackParser(parser, server.port()));
		ASSERT_TRUE(ParserXeleSktTestAccess::prepare(parser));
		ASSERT_TRUE(server.joinAndReport());
		expectOnlyCachedSeed(parser, 11, "ag2512", 0.01);
	}

	{
		const std::string seedB = makeSeedSnapshot("au2512", 22, 0.02);
		R76LoopbackSnapshotServer server(makeFullReadThenResetPayload(seedB), true);
		ASSERT_TRUE(initLoopbackParser(parser, server.port()));
		EXPECT_FALSE(ParserXeleSktTestAccess::reconnect(parser));
		ASSERT_TRUE(server.joinAndReport());
		EXPECT_FALSE(ParserXeleSktTestAccess::isPrepared(parser));
		expectOnlyCachedSeed(parser, 11, "ag2512", 0.01);
	}

	{
		const std::string seedB = makeSeedSnapshot("au2512", 22, 0.02);
		R76LoopbackSnapshotServer server(seedB + "tail", false);
		ASSERT_TRUE(initLoopbackParser(parser, server.port()));
		EXPECT_FALSE(ParserXeleSktTestAccess::reconnect(parser));
		ASSERT_TRUE(server.joinAndReport());
		EXPECT_FALSE(ParserXeleSktTestAccess::isPrepared(parser));
		expectOnlyCachedSeed(parser, 11, "ag2512", 0.01);
	}

	{
		const std::string seedB = makeSeedSnapshot("au2512", 22, 0.02);
		const std::string throwingFrame = makeSeedSnapshot("zz9999", 99, 0.09);
		R76LoopbackSnapshotServer server(seedB + throwingFrame, false);
		ASSERT_TRUE(initLoopbackParser(parser, server.port()));
		EXPECT_FALSE(ParserXeleSktTestAccess::prepare(parser));
		ASSERT_TRUE(server.joinAndReport());
		expectOnlyCachedSeed(parser, 11, "ag2512", 0.01);
	}

	{
		const std::string seedC = makeSeedSnapshot("cu2512", 33, 0.03);
		R76LoopbackSnapshotServer server(seedC, false);
		ASSERT_TRUE(initLoopbackParser(parser, server.port()));
		ParserXeleSktTestAccess::reconnect(parser);
		ASSERT_TRUE(server.joinAndReport());
		EXPECT_TRUE(ParserXeleSktTestAccess::isPrepared(parser));
		expectOnlyCachedSeed(parser, 33, "cu2512", 0.03);
	}

}

TEST(R76ParserXeleSktLoopbackTest, FirstReconnectFailureLeavesRetryUnseeded)
{
	R76LoopbackSink sink;
	ParserXeleSkt parser;
	ParserXeleSktTestCleanup cleanup(parser);
	addLoopbackContracts(sink, parser);

	const std::string seedB = makeSeedSnapshot("au2512", 22, 0.02);
	{
		R76LoopbackSnapshotServer server(makeFullReadThenResetPayload(seedB), true);
		ASSERT_TRUE(initLoopbackParser(parser, server.port()));
		EXPECT_FALSE(ParserXeleSktTestAccess::reconnect(parser));
		ASSERT_TRUE(server.joinAndReport());
		EXPECT_FALSE(ParserXeleSktTestAccess::isPrepared(parser));
		EXPECT_FALSE(ParserXeleSktTestAccess::hasTick(parser, 22));
		EXPECT_FALSE(ParserXeleSktTestAccess::hasPriceScale(parser, 22));
	}

	{
		const std::string seedC = makeSeedSnapshot("cu2512", 33, 0.03);
		R76LoopbackSnapshotServer server(seedC, false);
		ASSERT_TRUE(initLoopbackParser(parser, server.port()));
		ParserXeleSktTestAccess::reconnect(parser);
		ASSERT_TRUE(server.joinAndReport());
		EXPECT_TRUE(ParserXeleSktTestAccess::isPrepared(parser));
		expectOnlyCachedSeed(parser, 33, "cu2512", 0.03);
	}

}

/// IPv6 tcp_host 也能构造 endpoint；gpsize=0 走默认值；init 不开组播口
/// （ParserXeleSkt.cpp:99-117、294-297）
TEST(R76ParserXeleSktTest, InitAcceptsIpv6TcpHostAndZeroGpsize)
{
	ParserXeleSkt parser;
	WTSVariant* cfg = makeCfg("::1", 9001, "224.1.2.3", 9002, "127.0.0.1", 0);
	ASSERT_TRUE(parser.init(cfg));
	cfg->release();

	EXPECT_FALSE(parser.isConnected());
	EXPECT_NO_THROW(parser.release());
	EXPECT_FALSE(parser.isConnected());
}

/// 空和非法组播地址应由 init 返回 false，之后 connect 也应安全返回 false。
TEST(R76ParserXeleSktTest, InitRejectsEmptyAndMalformedMulticastAddresses)
{
	expectAddressConfigRejected("", "127.0.0.1");
	expectAddressConfigRejected("not-an-ip", "127.0.0.1");
	expectAddressConfigRejected("127.0.0.1", "127.0.0.1"); // IPv4 但不是组播组
}

/// local_host 是组播接口 IPv4；空或无法按 IPv4 解析时不得延迟到 socket join。
TEST(R76ParserXeleSktTest, InitRejectsEmptyAndMalformedLocalAddresses)
{
	expectAddressConfigRejected("224.1.2.3", "");
	expectAddressConfigRejected("224.1.2.3", "also-bad");
	expectAddressConfigRejected("224.1.2.3", "::1"); // TCP 可用 IPv6，本地组播接口要求 IPv4
}

/// TCP 快照通道在 loopback port 0 上立即拒绝；失败须阻止 UDP socket 与 parser worker 启动。
TEST(R76ParserXeleSktTest, ConnectRejectsSnapshotTcpFailureWithoutStartingReceiver)
{
	ParserXeleSkt parser;
	R76XeleSink sink;
	parser.registerSpi(&sink);

	WTSVariant* cfg = makeCfg("127.0.0.1", 0, "224.1.2.3", 9102, "127.0.0.1", 100);
	ASSERT_TRUE(parser.init(cfg));
	cfg->release();

	bool connected = true;
	EXPECT_NO_THROW(connected = parser.connect());
	EXPECT_FALSE(connected);
	EXPECT_FALSE(parser.isConnected());
	EXPECT_EQ(0u, sink.events);
	// connect() 只在 reconnect() 成功后创建 std::thread；失败路径返回 false，
	// 离开本测试时的析构也验证没有留下 joinable 后台线程。
}

/// init 可重复应用，后一次覆盖 tcp endpoint；release 为空实现，可重复调用
/// （ParserXeleSkt.cpp:99-122）
TEST(R76ParserXeleSktTest, InitIsRepeatableAndReleaseIsIdempotent)
{
	ParserXeleSkt parser;
	R76XeleSink sink;
	parser.registerSpi(&sink);

	WTSVariant* first = makeCfg("127.0.0.1", 9201, "224.1.2.3", 9202, "127.0.0.1", 10);
	WTSVariant* second = makeCfg("10.20.30.40", 9203, "224.1.2.4", 9204, "0.0.0.0", 20);
	ASSERT_TRUE(parser.init(first));
	ASSERT_TRUE(parser.init(second));
	first->release();
	second->release();

	EXPECT_NO_THROW(parser.release());
	EXPECT_NO_THROW(parser.release());
	EXPECT_FALSE(parser.isConnected());
	EXPECT_EQ(0u, sink.events);
	EXPECT_EQ(0u, sink.logs.size());
}

/// registerSpi(NULL) 清空 _sink；下一次注册 bReplaced=false，不告警；
/// 再换一个才走替换告警（ParserXeleSkt.cpp:318-329）
TEST(R76ParserXeleSktTest, RegisterSpiNullMakesNextRegistrationSilent)
{
	ParserXeleSkt parser;
	R76XeleSink first;
	parser.registerSpi(&first);
	EXPECT_EQ(1u, first.bd_calls);

	parser.registerSpi(nullptr); // 只清 _sink
	ASSERT_TRUE(first.logs.empty());

	R76XeleSink second;
	parser.registerSpi(&second);
	EXPECT_TRUE(second.logs.empty()); // 不视为替换
	EXPECT_EQ(1u, second.bd_calls);

	R76XeleSink third;
	parser.registerSpi(&third);
	ASSERT_EQ(1u, third.logs.size());
	EXPECT_NE(std::string::npos, third.logs[0].find("Listener is replaced"));
	EXPECT_TRUE(first.logs.empty());
	EXPECT_TRUE(second.logs.empty());
}

/// 空集合、含点标准码、重复订阅都只改内部集合；unsubscribe 是空实现
/// （ParserXeleSkt.cpp:300-316）
TEST(R76ParserXeleSktTest, SubscribeMutatesOnlyInternalSetAndUnsubscribeIsStub)
{
	ParserXeleSkt parser;
	R76XeleSink sink;
	parser.registerSpi(&sink);

	CodeSet empty;
	EXPECT_NO_THROW(parser.subscribe(empty));
	EXPECT_NO_THROW(parser.unsubscribe(empty));

	CodeSet codes;
	codes.insert("SHFE.ag2512");
	codes.insert("DCE.m2509");
	parser.subscribe(codes);
	parser.subscribe(codes); // 全部命中已存在分支
	parser.unsubscribe(codes); // 空实现

	EXPECT_FALSE(parser.isConnected());
	EXPECT_EQ(0u, sink.quotes);
	EXPECT_EQ(0u, sink.symbol_lists);
	EXPECT_EQ(0u, sink.events);
	EXPECT_TRUE(sink.logs.empty());
}

/// 未 connect 时 disconnect 只 post doOnDisconnected；io_context 未 run，
/// 因此不得同步回调 WPE_Close，且重复调用幂等（ParserXeleSkt.cpp:279-297、535-541）
TEST(R76ParserXeleSktTest, DisconnectBeforeConnectDefersCloseAndIsIdempotent)
{
	ParserXeleSkt parser;
	R76XeleSink sink;
	parser.registerSpi(&sink);

	EXPECT_TRUE(parser.disconnect());
	EXPECT_TRUE(parser.disconnect());
	EXPECT_FALSE(parser.isConnected());
	EXPECT_EQ(0u, sink.events);

	CodeSet codes;
	codes.insert("CFFEX.IF2512");
	EXPECT_NO_THROW(parser.subscribe(codes)); // 无 _stopped 守卫，仍可缓存
	EXPECT_NO_THROW(parser.unsubscribe(codes));
	EXPECT_NO_THROW(parser.release());
}

/// 插件工厂：createParser 返回 ParserXeleSkt，deleteParser 置空且二次调用安全
/// （ParserXeleSkt.cpp:64-80、84-97）
TEST(R76ParserXeleSktTest, FactoryRoundtripClearsPointerTwice)
{
	IParserApi* api = createParser();
	ASSERT_NE(nullptr, api);
	EXPECT_NE(nullptr, dynamic_cast<ParserXeleSkt*>(api));

	WTSVariant* cfg = makeCfg("127.0.0.1", 9301, "224.1.2.5", 9302, "127.0.0.1", 5);
	ASSERT_TRUE(api->init(cfg));
	cfg->release();

	R76XeleSink sink;
	api->registerSpi(&sink);
	CodeSet codes;
	codes.insert("SHFE.ag2512");
	api->subscribe(codes);

	deleteParser(api);
	EXPECT_EQ(nullptr, api);
	deleteParser(api);
	EXPECT_EQ(nullptr, api);
	EXPECT_EQ(0u, sink.events);
}

} // namespace r76xele
