/*
 * lane R76 — ParserUDP 公开面补测（配置边界/监听者生命周期/空订阅/断连）
 *
 * 目标文件：src/ParserUDP/ParserUDP.cpp（310 可执行行，missed 240）
 *
 * 既有 t6_parserudp_reachable_test.cpp 已覆盖 init 正/负配置（含 make_address 抛出）、
 * 工厂、registerSpi 首次与替换、subscribe 幂等、release/disconnect 未连接路径。
 * 本文件补它没覆盖的组合：
 *   - IPv6 单播 host 与 bport/sport=0 的 init；
 *   - registerSpi(NULL) 后再注册：下一次必须按「首次注册」处理、不告警；
 *   - 空订阅集合、无点原始代码、含点标准码、重复订阅与空 unsubscribe；
 *   - 先 disconnect（无 sink）再注册再 disconnect 的幂等组合，以及事件被 strand 推迟。
 *
 * connect()/reconnect()/subscribe()/do_send()/handle_read()/extract_buffer() 均私有，
 * 且正路径要 bind 真实 UDP 端口 + 起 io_context 线程，本 lane 不进入
 * （T6 已留 DISABLED_t6_parserudp_net.connect_binds_ports_and_starts_io_thread）。
 * 注意：doOnConnected()（ParserUDP.cpp:443-450）在本文件里没有任何调用者，属结构性死代码，
 * 已登记 bugs/R76.md R76-02。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <boost/system/system_error.hpp>

#include "ParserUDP/ParserUDP.h"

#include "Includes/FasterDefs.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/IParserApi.h"
#include "Includes/WTSVariant.hpp"

USING_NS_WTP;

extern "C"
{
	IParserApi* createParser();
	void deleteParser(IParserApi*& parser);
}

namespace r76udp
{

/* ------------------------------------------------------------------ 替身 */

/// 只记录回调；getBaseDataMgr 返回 NULL（ParserUDP 不在 registerSpi 里解引用）
class R76UdpSink : public IParserSpi
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
	virtual void handleOrderDetail(WTSOrdDtlData*) override { ++order_details; }
	virtual void handleOrderQueue(WTSOrdQueData*) override { ++order_queues; }
	virtual void handleTransaction(WTSTransData*) override { ++transactions; }

	uint32_t bd_calls = 0;
	uint32_t symbol_lists = 0;
	uint32_t quotes = 0;
	uint32_t events = 0;
	uint32_t order_details = 0;
	uint32_t order_queues = 0;
	uint32_t transactions = 0;
	uint32_t last_proc_flag = 0;
	std::vector<std::string> logs;
};

WTSVariant* makeCfg(const char* host, int32_t bport, int32_t sport, uint32_t gpsize)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("host", host);
	cfg->append("bport", bport);
	cfg->append("sport", sport);
	cfg->append("gpsize", gpsize);
	return cfg;
}

/* ------------------------------------------------------------------ 用例 */

/// IPv6 单播 host + 两个端口都为 0 也能建 endpoint；init 不开 socket
/// （ParserUDP.cpp:98-113、283-286）
TEST(R76ParserUDPTest, InitAcceptsIpv6HostAndZeroPorts)
{
	ParserUDP parser;
	WTSVariant* cfg = makeCfg("::1", 0, 0, 0);
	ASSERT_TRUE(parser.init(cfg));
	cfg->release();

	EXPECT_FALSE(parser.isConnected());
	EXPECT_NO_THROW(parser.release());
	EXPECT_FALSE(parser.isConnected());
}

/// init 可重复应用，后一次覆盖两个 endpoint；不产生任何回调
/// （ParserUDP.cpp:98-113）
TEST(R76ParserUDPTest, InitIsRepeatableAndOverwritesEndpoints)
{
	ParserUDP parser;
	R76UdpSink sink;
	parser.registerSpi(&sink);

	WTSVariant* first = makeCfg("127.0.0.1", 18001, 18002, 10);
	WTSVariant* second = makeCfg("10.20.30.40", 18003, 18004, 20);
	ASSERT_TRUE(parser.init(first));
	ASSERT_TRUE(parser.init(second));
	first->release();
	second->release();

	EXPECT_FALSE(parser.isConnected());
	EXPECT_EQ(0u, sink.events);
	EXPECT_EQ(0u, sink.symbol_lists);
	EXPECT_TRUE(sink.logs.empty());
}

/// registerSpi(NULL) 清空 _sink；下一次注册 bReplaced=false 不告警，再换才告警
/// （ParserUDP.cpp:307-315）
TEST(R76ParserUDPTest, RegisterSpiNullMakesNextRegistrationSilent)
{
	ParserUDP parser;
	R76UdpSink first;
	parser.registerSpi(&first);
	ASSERT_TRUE(first.logs.empty());

	parser.registerSpi(nullptr);
	R76UdpSink second;
	parser.registerSpi(&second);
	EXPECT_TRUE(second.logs.empty());

	R76UdpSink third;
	parser.registerSpi(&third);
	ASSERT_EQ(1u, third.logs.size());
	EXPECT_NE(std::string::npos, third.logs[0].find("replaced"));
	EXPECT_TRUE(first.logs.empty());
	EXPECT_TRUE(second.logs.empty());
}

/// 空集合 / 无点原始代码 / 含点标准码 / 重复订阅：只改私有集合，
/// unsubscribe 空实现，不触发任何回调（ParserUDP.cpp:289-305）
TEST(R76ParserUDPTest, SubscribeAcceptsMixedCodeShapesWithoutCallbacks)
{
	ParserUDP parser;
	R76UdpSink sink;
	parser.registerSpi(&sink);

	CodeSet empty;
	EXPECT_NO_THROW(parser.subscribe(empty));
	EXPECT_NO_THROW(parser.unsubscribe(empty));

	CodeSet raw;
	raw.insert("ag2512"); // 不含点，subscribe 只做字符串插入
	parser.subscribe(raw);

	CodeSet standard;
	standard.insert("SHFE.ag2512");
	standard.insert("DCE.m2509");
	parser.subscribe(standard);
	parser.subscribe(standard); // 重复
	parser.unsubscribe(standard); // 空实现

	EXPECT_FALSE(parser.isConnected());
	EXPECT_EQ(0u, sink.quotes);
	EXPECT_EQ(0u, sink.symbol_lists);
	EXPECT_EQ(0u, sink.events);
	EXPECT_TRUE(sink.logs.empty());
}

/// 先 disconnect（无 sink）再注册再 disconnect：幂等且事件仍被 strand 推迟
/// （ParserUDP.cpp:268-281、452-458）
TEST(R76ParserUDPTest, DisconnectBeforeAndAfterRegisterSpiIsIdempotent)
{
	ParserUDP parser;
	EXPECT_TRUE(parser.disconnect()); // 无 sink，仅 post

	R76UdpSink sink;
	parser.registerSpi(&sink);
	EXPECT_TRUE(parser.disconnect());
	EXPECT_TRUE(parser.disconnect());
	EXPECT_FALSE(parser.isConnected());
	EXPECT_EQ(0u, sink.events);
	EXPECT_NO_THROW(parser.release());
}

/// 工厂 + 已注册 sink 的销毁：createParser/deleteParser 往返，出参置空且二次调用安全
/// （ParserUDP.cpp:62-78、82-96）
TEST(R76ParserUDPTest, FactoryDeleteWithRegisteredSinkIsSafe)
{
	IParserApi* api = createParser();
	ASSERT_NE(nullptr, api);
	EXPECT_NE(nullptr, dynamic_cast<ParserUDP*>(api));

	WTSVariant* cfg = makeCfg("127.0.0.1", 19001, 19002, 5);
	ASSERT_TRUE(api->init(cfg));
	cfg->release();

	R76UdpSink sink;
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

} // namespace r76udp
