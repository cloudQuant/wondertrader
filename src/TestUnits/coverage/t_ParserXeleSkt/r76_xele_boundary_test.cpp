/*
 * lane R76 — ParserXeleSkt 公开面补测（配置边界/监听者生命周期/空订阅）
 *
 * 目标文件：src/ParserXeleSkt/ParserXeleSkt.cpp（377 可执行行，missed 297）
 *
 * 既有 t2_parserxele_lifecycle_test.cpp 已覆盖插件工厂、init 合法/非法地址、
 * registerSpi 首次与替换、subscribe 幂等、unsubscribe 空实现、disconnect 未连接路径。
 * 本文件补：
 *   - IPv6 tcp_host 与 gpsize=0 的 init 组合；
 *   - init 只解析 tcp_host，mcast_host/local_host 非法也会返回 true（校验被推迟到
 *     connect()，见 bugs/R76.md R76-04）；
 *   - registerSpi(NULL) 之后下一次注册不再算「替换」，旧行为与新行为的分界；
 *   - 空订阅集合、含点标准码、重复订阅、退订空实现；
 *   - disconnect 在未 connect 时的幂等与「事件被 strand 推迟」契约。
 *
 * connect()/reconnect()/prepare()/handle_udp_read()/extract_buffer() 全部私有且需要
 * 真实组播 socket + io_context 线程，本 lane 不进入（T2 已留 DISABLED_connect_joins_multicast...）。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <boost/system/system_error.hpp>

#include "ParserXeleSkt/ParserXeleSkt.h"

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

namespace r76xele
{

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

/* ------------------------------------------------------------------ 用例 */

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

/// init 只对 tcp_host 调 make_address；mcast_host/local_host 非法仍返回 true，
/// 失败被推迟到 connect() 的 join_group（ParserXeleSkt.cpp:110-116、124-154）
TEST(R76ParserXeleSktTest, InitDefersMulticastValidationUntilConnect)
{
	ParserXeleSkt parser;
	WTSVariant* cfg = makeCfg("127.0.0.1", 9101, "not-an-ip", 9102, "also-bad", 100);
	ASSERT_TRUE(parser.init(cfg)); // 当前实现不校验 mcast/local
	cfg->release();

	EXPECT_FALSE(parser.isConnected());
	EXPECT_NO_THROW(parser.release());
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
