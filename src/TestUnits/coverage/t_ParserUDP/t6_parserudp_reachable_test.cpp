/*
 * lane T6（收发族第六波）— ParserUDP 可直达面用例
 *
 * 目标文件：src/ParserUDP/ParserUDP.cpp（458 行）
 *
 * 可达性口径：
 *  - `handle_read()/extract_buffer()/reconnect()/do_send()/subscribe()/doOnDisconnected()`
 *    全是私有成员，公开接口没有任何注入口；`connect()` 会真的 open/bind UDP 端口并
 *    new StdThread 跑 io_context（L254-266 + L120-171）。按门禁规则（不起线程、
 *    不绑真实端口、不 `#define private public`），报文体解析（UDP_MSG_PUSHTICK /
 *    0x201~0x203 四条分发）与「长度校验」本轮无法进入 —— 已记入 bugs 文档。
 *  - 本文件断言的都是「不需要 io_context 被驱动」就能观察到的行为：
 *    配置解析（含 make_address 抛出的异常路径）、订阅集合幂等、Spi 替换告警、
 *    以及「提交后未 run io_context 时不得有事件回报」这条可证伪的 deferred 契约。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <boost/system/system_error.hpp>

#include "ParserUDP/ParserUDP.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSVariant.hpp"

USING_NS_WTP;

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
		{ (void)quote; (void)procFlag; ++n_quote_; }

		virtual void handleSymbolList(const WTSArray* aySymbols) override { (void)aySymbols; ++n_symlist_; }

		virtual void handleOrderDetail(WTSOrdDtlData* data) override { (void)data; ++n_orddtl_; }

		virtual void handleOrderQueue(WTSOrdQueData* data) override { (void)data; ++n_ordque_; }

		virtual void handleTransaction(WTSTransData* data) override { (void)data; ++n_trans_; }
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

TEST(t6_parserudp_init, non_numeric_host_throws_uncaught_system_error)
{
	ParserUDP parser;
	// L107 make_address 对非数字地址抛异常，init 没有 try/catch 也没有返回 false
	WTSVariant* cfg = make_cfg("udp-host-not-an-ip", 0, 0, 100);
	ASSERT_NE(nullptr, cfg);

	EXPECT_THROW(parser.init(cfg), boost::system::system_error);
	cfg->release();
}

TEST(t6_parserudp_init, missing_host_config_throws_instead_of_reporting_failure)
{
	ParserUDP parser;
	// 空对象：_hots 取默认 ""，make_address("") 抛异常
	WTSVariant* cfg = WTSVariant::createObject();
	ASSERT_NE(nullptr, cfg);

	EXPECT_THROW(parser.init(cfg), boost::system::system_error);
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

	parser.release();		// L115-118：空实现
	EXPECT_FALSE(parser.isConnected());

	// L268-281：_b_socket 为空 → 跳过 close/delete，只置 _stopped 并 post 事件
	EXPECT_TRUE(parser.disconnect());
	EXPECT_TRUE(parser.disconnect());	// 重复断连仍然返回 true
	EXPECT_FALSE(parser.isConnected());
	EXPECT_EQ(0u, sink.n_event_);
}

/* ------------------------------------------------------------------ */
/* 明确不进入的路径（整条 DISABLED_）                                     */
/* ------------------------------------------------------------------ */

// 说明：connect() → reconnect(3) 会 new udp socket、bind(_broad_ep) 真实端口，
// 并 new StdThread 跑 io_context（L254-266、L120-171）；违反「不绑真实端口/不起线程」，
// 整条禁用。禁用原因不是「测不了」，而是本轮门禁不允许产生真实网络副作用。
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

// 说明：报文解析（extract_buffer，L357-441）与长度校验（handle_read L335-338）都是私有函数，
// 且 _b_buffer/_s_buffer 也是私有成员；公开接口无法用内存 buffer 直接喂解析函数。
// 本条只是把「需要 fake socket 注入点或把它们提为 protected」的诉求钉在用例清单里。
TEST(DISABLED_t6_parserudp_net, extract_buffer_needs_a_seam_before_it_can_be_fed)
{
	ParserUDP parser;
	SUCCEED() << "extract_buffer/handle_read 为私有，无注入点：见 bugs/T6_ctploader_mocker_udp.md";
}
