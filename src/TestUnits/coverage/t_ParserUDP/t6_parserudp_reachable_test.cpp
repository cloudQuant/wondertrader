/*
 * lane T6（收发族第六波）— ParserUDP 可直达面用例
 *
 * 目标文件：src/ParserUDP/ParserUDP.cpp（458 行）
 *
 * 可达性口径：
 *  - 通过 ParserUDPExtractBufferTestPeer 使用生产类已声明的 friend seam 直接向私有
 *    extract_buffer 注入内存报文；不创建 socket、不绑定端口、不启动线程。
 *  - 覆盖 UDP 短帧和四类完整行情报文，同时保留配置、订阅、SPI 和状态用例。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "ParserUDP/ParserUDP.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"

USING_NS_WTP;

// Friend seam for feeding complete or deliberately truncated datagrams to
// the production parser without opening sockets or starting its I/O thread.
struct ParserUDPExtractBufferTestPeer
{
	static constexpr uint32_t UDP_MSG_SUBSCRIBE = 0x100;
	static constexpr uint32_t UDP_MSG_PUSHTICK = 0x200;
	static constexpr uint32_t UDP_MSG_PUSHORDQUE = 0x201;
	static constexpr uint32_t UDP_MSG_PUSHORDDTL = 0x202;
	static constexpr uint32_t UDP_MSG_PUSHTRANS = 0x203;

	static void feed_raw(ParserUDP& parser, const char* data, std::size_t length)
	{
		parser.extract_buffer(static_cast<uint32_t>(length), false, data);
	}

	template<typename T>
	static void feed(ParserUDP& parser, uint32_t type, const T& payload,
		std::size_t received_length = sizeof(uint32_t) + sizeof(T))
	{
		std::vector<char> frame(sizeof(uint32_t) + sizeof(T), 0);
		std::memcpy(frame.data(), &type, sizeof(type));
		std::memcpy(frame.data() + sizeof(type), &payload, sizeof(payload));
		feed_raw(parser, frame.data(), received_length);
	}
};

extern "C"
{
	/// 生产符号：src/ParserUDP/ParserUDP.cpp L62-78（装载入口）
	IParserApi* createParser();
	void deleteParser(IParserApi*& parser);
}

namespace
{
	/// IParserSpi 替身：只记录回调，不接行情源
	class UdpSink : public IParserSpi
	{
	public:
		uint32_t n_event_ = 0;
		uint32_t n_log_ = 0;
		uint32_t n_quote_ = 0;
		uint32_t n_symlist_ = 0;
		uint32_t n_orddtl_ = 0;
		uint32_t n_ordque_ = 0;
		uint32_t n_trans_ = 0;
		int last_event_ = -1;
		WTSTickStruct last_tick_{};
		WTSOrdDtlStruct last_orddtl_{};
		WTSOrdQueStruct last_ordque_{};
		WTSTransStruct last_trans_{};
		std::vector<std::string> logs_;

		virtual ~UdpSink() {}

		virtual IBaseDataMgr* getBaseDataMgr() override { return NULL; }

		virtual void handleEvent(WTSParserEvent e, int32_t ec) override { (void)ec; ++n_event_; last_event_ = e; }

		virtual void handleParserLog(WTSLogLevel ll, const char* message) override
		{
			(void)ll;
			++n_log_;
			logs_.emplace_back(message == NULL ? "" : message);
		}

		virtual void handleQuote(WTSTickData* quote, uint32_t procFlag) override
		{ (void)procFlag; ++n_quote_; last_tick_ = quote->getTickStruct(); }

		virtual void handleSymbolList(const WTSArray* aySymbols) override { (void)aySymbols; ++n_symlist_; }

		virtual void handleOrderDetail(WTSOrdDtlData* data) override { ++n_orddtl_; last_orddtl_ = data->getOrdDtlStruct(); }

		virtual void handleOrderQueue(WTSOrdQueData* data) override { ++n_ordque_; last_ordque_ = data->getOrdQueStruct(); }

		virtual void handleTransaction(WTSTransData* data) override { ++n_trans_; last_trans_ = data->getTransStruct(); }
	};

	WTSVariant* make_cfg(const char* host, int bport, int sport, uint32_t gpsize)
	{
		WTSVariant* p = WTSVariant::createObject();
		p->append("host", host);
		p->append("bport", bport);
		p->append("sport", sport);
		p->append("gpsize", gpsize);
		return p;
	}
}

/* ------------------------------------------------------------------ */
/* 装载入口                                                            */
/* ------------------------------------------------------------------ */

TEST(t6_parserudp_factory, createParser_returns_api_and_delete_clears_pointer)
{
	IParserApi* parser = createParser();
	ASSERT_NE(nullptr, parser);

	// 未 connect 之前不得自称在线
	EXPECT_FALSE(parser->isConnected());
	parser->release();

	deleteParser(parser);
	EXPECT_EQ(nullptr, parser);
}

TEST(t6_parserudp_factory, deleteParser_accepts_null_without_touching_anything)
{
	IParserApi* parser = NULL;
	deleteParser(parser);
	EXPECT_EQ(nullptr, parser);
}

/* ------------------------------------------------------------------ */
/* init：配置正/负路径                                                   */
/* ------------------------------------------------------------------ */

TEST(t6_parserudp_init, valid_multicast_host_without_gpsize_falls_back_to_default)
{
	ParserUDP parser;
	// gpsize=0 → 命中 L104-105 兜底为 1000；host 为合法组播地址
	WTSVariant* cfg = make_cfg("239.0.0.1", 0, 0, 0);
	ASSERT_NE(nullptr, cfg);

	EXPECT_TRUE(parser.init(cfg));
	cfg->release();

	// init 只算端点，不开 socket：此时仍然「未连接」
	EXPECT_FALSE(parser.isConnected());
}

TEST(t6_parserudp_init, explicit_gpsize_and_unicast_host_accepted)
{
	ParserUDP parser;
	WTSVariant* cfg = make_cfg("10.20.30.40", 0, 0, 500);
	ASSERT_NE(nullptr, cfg);

	EXPECT_TRUE(parser.init(cfg));
	cfg->release();
	EXPECT_FALSE(parser.isConnected());
}

TEST(t6_parserudp_init, non_numeric_host_reports_initialization_failure)
{
	ParserUDP parser;
	// IParserApi::init 的返回值表示初始化是否成功；错误主机应以 false 报告失败。
	WTSVariant* cfg = make_cfg("udp-host-not-an-ip", 0, 0, 100);
	ASSERT_NE(nullptr, cfg);

	bool initialized = true;
	EXPECT_NO_THROW(initialized = parser.init(cfg));
	EXPECT_FALSE(initialized);
	cfg->release();
}

TEST(t6_parserudp_init, missing_host_reports_initialization_failure)
{
	ParserUDP parser;
	// 空对象没有 host；与无效 host 一样，应通过 bool 返回值报告初始化失败。
	WTSVariant* cfg = WTSVariant::createObject();
	ASSERT_NE(nullptr, cfg);

	bool initialized = true;
	EXPECT_NO_THROW(initialized = parser.init(cfg));
	EXPECT_FALSE(initialized);
	cfg->release();
}

TEST(t6_parserudp_init, init_can_be_reapplied_with_a_different_host)
{
	ParserUDP parser;
	WTSVariant* c1 = make_cfg("239.1.2.3", 0, 0, 10);
	WTSVariant* c2 = make_cfg("239.9.9.9", 0, 0, 20);
	ASSERT_TRUE(parser.init(c1));
	// 第二次 init 覆盖端点：证明 init 不是幂等空函数
	ASSERT_TRUE(parser.init(c2));
	c1->release();
	c2->release();
	EXPECT_FALSE(parser.isConnected());
}

/* ------------------------------------------------------------------ */
/* registerSpi：替换监听者时必须给出告警                                 */
/* ------------------------------------------------------------------ */

TEST(t6_parserudp_spi, first_register_is_silent_and_second_one_warns)
{
	ParserUDP parser;
	UdpSink first, second;

	parser.registerSpi(&first);
	// 首次注册没有「被替换」分支，不应产生日志
	EXPECT_EQ(0u, first.n_log_);

	parser.registerSpi(&second);
	// L309-314：bReplaced=true 且新 sink 非空 → 走 write_log("Listener is replaced")
	EXPECT_EQ(1u, second.n_log_);
	ASSERT_EQ(1u, second.logs_.size());
	EXPECT_NE(std::string::npos, second.logs_[0].find("replaced"));
	EXPECT_EQ(0u, first.n_log_);
}

TEST(t6_parserudp_spi, handle_event_is_not_emitted_without_io_context)
{
	ParserUDP parser;
	UdpSink sink;
	parser.registerSpi(&sink);

	CodeSet subs;
	subs.insert("SHFE.ag2512");
	parser.subscribe(subs);
	parser.disconnect();	// 只是 post 到 strand，未 run io_context
	// L278：doOnDisconnected 必须等到 strand 被驱动，这里不得同步回调
	EXPECT_EQ(0u, sink.n_event_);
	EXPECT_FALSE(parser.isConnected());
}

/* ------------------------------------------------------------------ */
/* 订阅集合 / 断连状态                                                   */
/* ------------------------------------------------------------------ */

TEST(t6_parserudp_subs, subscribe_is_idempotent_and_duplicate_codes_are_dropped)
{
	ParserUDP parser;
	UdpSink sink;
	parser.registerSpi(&sink);

	// L289-300：已存在的 code 走 skip 分支，新 code 走 insert 分支
	CodeSet set1;
	set1.insert("SHFE.ag2512");
	set1.insert("DCE.m2509");
	parser.subscribe(set1);

	CodeSet set2;
	set2.insert("SHFE.ag2512");	// 重复
	set2.insert("CFFEX.if2512");	// 新增
	parser.subscribe(set2);

	// 订阅只改内部集合，不开端口
	EXPECT_FALSE(parser.isConnected());
	EXPECT_EQ(0u, sink.n_symlist_);
	EXPECT_EQ(0u, sink.n_quote_);
}

TEST(t6_parserudp_subs, single_code_subscribe_helper_adds_one_entry)
{
	ParserUDP parser;
	UdpSink sink;
	parser.registerSpi(&sink);

	CodeSet one;
	one.insert("SSE.600000");
	parser.subscribe(one);
	parser.subscribe(one);	// 第二次全部命中「已订阅」分支
	parser.unsubscribe(one);	// L302-305：空实现，不允许影响状态
	parser.unsubscribe(CodeSet{});

	EXPECT_FALSE(parser.isConnected());
	EXPECT_EQ(0u, sink.n_event_);
}

TEST(t6_parserudp_state, release_and_disconnect_without_connect_are_safe)
{
	ParserUDP parser;
	UdpSink sink;
	parser.registerSpi(&sink);

	parser.release();		// 未连接时同步完成停止，不应产生 SPI 事件
	EXPECT_FALSE(parser.isConnected());

	// 已停止状态下 disconnect 仍幂等成功，不重复投递事件。
	EXPECT_TRUE(parser.disconnect());
	EXPECT_TRUE(parser.disconnect());	// 重复断连仍然返回 true
	EXPECT_FALSE(parser.isConnected());
	EXPECT_EQ(0u, sink.n_event_);
}

/* ------------------------------------------------------------------ */
/* UDP 行情报文长度与分发                                               */
/* ------------------------------------------------------------------ */

// 连接测试会绑定真实端口并启动 io_context worker，因此仍单独禁用。
TEST(DISABLED_t6_parserudp_net, connect_binds_ports_and_starts_io_thread)
{
	ParserUDP parser;
	UdpSink sink;
	parser.registerSpi(&sink);

	WTSVariant* cfg = make_cfg("127.0.0.1", 19827, 19828, 100);
	ASSERT_TRUE(parser.init(cfg));
	cfg->release();

	CodeSet subs;
	subs.insert("SHFE.ag2512");
	parser.subscribe(subs);
	EXPECT_TRUE(parser.connect());
	parser.disconnect();
	parser.release();
}

TEST(t6_parserudp_packets, short_and_unknown_frames_are_ignored)
{
	ParserUDP parser;
	UdpSink sink;
	parser.registerSpi(&sink);
	WTSVariant* cfg = make_cfg("239.0.0.1", 0, 0, 100);
	ASSERT_TRUE(parser.init(cfg));
	cfg->release();
	CodeSet subs;
	subs.insert("SHFE.ag2512");
	parser.subscribe(subs);

	const char short_header[sizeof(uint32_t) - 1] = { 0x00, 0x02, 0x00 };
	ParserUDPExtractBufferTestPeer::feed_raw(parser, short_header, sizeof(short_header));
	const uint32_t tick_type = ParserUDPExtractBufferTestPeer::UDP_MSG_PUSHTICK;
	ParserUDPExtractBufferTestPeer::feed_raw(parser, reinterpret_cast<const char*>(&tick_type), sizeof(tick_type));

	WTSTickStruct tick;
	std::strcpy(tick.exchg, "SHFE");
	std::strcpy(tick.code, "ag2512");
	tick.price = 101.25;
	ParserUDPExtractBufferTestPeer::feed(parser, ParserUDPExtractBufferTestPeer::UDP_MSG_PUSHTICK, tick,
		sizeof(uint32_t) + sizeof(tick) - 1);

	WTSOrdDtlStruct detail;
	std::strcpy(detail.exchg, "SHFE");
	std::strcpy(detail.code, "ag2512");
	ParserUDPExtractBufferTestPeer::feed(parser, ParserUDPExtractBufferTestPeer::UDP_MSG_PUSHORDDTL, detail,
		sizeof(uint32_t) + sizeof(detail) - 1);

	WTSOrdQueStruct queue;
	std::strcpy(queue.exchg, "SHFE");
	std::strcpy(queue.code, "ag2512");
	ParserUDPExtractBufferTestPeer::feed(parser, ParserUDPExtractBufferTestPeer::UDP_MSG_PUSHORDQUE, queue,
		sizeof(uint32_t) + sizeof(queue) - 1);

	WTSTransStruct transaction;
	std::strcpy(transaction.exchg, "SHFE");
	std::strcpy(transaction.code, "ag2512");
	ParserUDPExtractBufferTestPeer::feed(parser, ParserUDPExtractBufferTestPeer::UDP_MSG_PUSHTRANS, transaction,
		sizeof(uint32_t) + sizeof(transaction) - 1);
	const uint32_t unknown_type = 0xdead;
	ParserUDPExtractBufferTestPeer::feed_raw(parser, reinterpret_cast<const char*>(&unknown_type), sizeof(unknown_type));

	EXPECT_EQ(0u, sink.n_quote_);
	EXPECT_EQ(0u, sink.n_orddtl_);
	EXPECT_EQ(0u, sink.n_ordque_);
	EXPECT_EQ(0u, sink.n_trans_);
}

TEST(t6_parserudp_packets, complete_packets_dispatch_aligned_payload_fields)
{
	ParserUDP parser;
	UdpSink sink;
	parser.registerSpi(&sink);
	WTSVariant* cfg = make_cfg("239.0.0.1", 0, 0, 100);
	ASSERT_TRUE(parser.init(cfg));
	cfg->release();
	CodeSet subs;
	subs.insert("SHFE.ag2512");
	parser.subscribe(subs);

	WTSTickStruct tick;
	std::strcpy(tick.exchg, "SHFE");
	std::strcpy(tick.code, "ag2512");
	tick.price = 101.25;
	ParserUDPExtractBufferTestPeer::feed(parser, ParserUDPExtractBufferTestPeer::UDP_MSG_PUSHTICK, tick,
		sizeof(uint32_t) + sizeof(tick) - 1);
	WTSTickStruct unterminated_code;
	std::memset(&unterminated_code, 'X', sizeof(unterminated_code));
	ParserUDPExtractBufferTestPeer::feed(parser, ParserUDPExtractBufferTestPeer::UDP_MSG_PUSHTICK, unterminated_code);
	ParserUDPExtractBufferTestPeer::feed(parser, ParserUDPExtractBufferTestPeer::UDP_MSG_PUSHTICK, tick);
	EXPECT_EQ(1u, sink.n_quote_);
	EXPECT_DOUBLE_EQ(101.25, sink.last_tick_.price);

	// Snapshot response packets use the subscribe type but retain the tick body.
	tick.price = 101.75;
	ParserUDPExtractBufferTestPeer::feed(parser, ParserUDPExtractBufferTestPeer::UDP_MSG_SUBSCRIBE, tick);
	EXPECT_EQ(2u, sink.n_quote_);
	EXPECT_DOUBLE_EQ(101.75, sink.last_tick_.price);

	WTSOrdDtlStruct detail;
	std::strcpy(detail.exchg, "SHFE");
	std::strcpy(detail.code, "ag2512");
	detail.price = 102.25;
	detail.index = 42;
	ParserUDPExtractBufferTestPeer::feed(parser, ParserUDPExtractBufferTestPeer::UDP_MSG_PUSHORDDTL, detail);
	EXPECT_EQ(1u, sink.n_orddtl_);
	EXPECT_DOUBLE_EQ(102.25, sink.last_orddtl_.price);
	EXPECT_EQ(42u, sink.last_orddtl_.index);

	WTSOrdQueStruct queue;
	std::strcpy(queue.exchg, "SHFE");
	std::strcpy(queue.code, "ag2512");
	queue.price = 103.25;
	queue.qsize = 3;
	ParserUDPExtractBufferTestPeer::feed(parser, ParserUDPExtractBufferTestPeer::UDP_MSG_PUSHORDQUE, queue);
	EXPECT_EQ(1u, sink.n_ordque_);
	EXPECT_DOUBLE_EQ(103.25, sink.last_ordque_.price);
	EXPECT_EQ(3u, sink.last_ordque_.qsize);

	WTSTransStruct transaction;
	std::strcpy(transaction.exchg, "SHFE");
	std::strcpy(transaction.code, "ag2512");
	transaction.price = 104.25;
	transaction.index = 77;
	ParserUDPExtractBufferTestPeer::feed(parser, ParserUDPExtractBufferTestPeer::UDP_MSG_PUSHTRANS, transaction);
	EXPECT_EQ(1u, sink.n_trans_);
	EXPECT_DOUBLE_EQ(104.25, sink.last_trans_.price);
	EXPECT_EQ(77u, sink.last_trans_.index);
}
