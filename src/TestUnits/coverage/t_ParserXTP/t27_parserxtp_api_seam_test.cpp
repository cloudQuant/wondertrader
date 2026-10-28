/*
 * 覆盖率用例 lane R27（第一波）：`src/ParserXTP/ParserXTP.cpp` 的登录 / 订阅状态机
 *
 * 目标空白段（llvm-cov 实测仍为 0% 的方法体，逐段见 manifest）：
 *   L446-495 `DoLogin()`、L498-582 `DoSubscribeMD()`、L608-678 `subscribe()` 登录后分支、
 *   L163-173 `disconnect()` 的「持 api 指针」分支、L142-148+L160 `connect()`（不走建线程那条），
 *   以及 3 个 `boost::asio::post` lambda（L462-464、L471-473、L482-488）。
 *
 * 手法：`t27_seam.hpp` 的布局镜像只负责把「init() 本来要灌进对象的 9 个配置字段 + api 指针」
 * 灌成确定值，之后**只走公开入口**（connect/subscribe/disconnect/release/isConnected）驱动产品代码，
 * 厂商调用落到本 TU 的 FakeApi；断言的全是产品自身的换算、分支、日志文本与调用次数。
 * 不调真实柜台、不 bind 端口、不起线程、不 sleep 等回调。
 *
 * suite 名取自 fixture 类名（XtpSeamF / XtpLoginF / XtpOnlineF），与 T1 lane 的 `ParserXTP.*`
 * 用例不重名；DISABLED 一律挂在**用例名**上，好让 coverage.sh 的 `grep -v '\.DISABLED_'` 滤得掉。
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "t27_seam.hpp"

using namespace t27;

namespace
{
	inline std::vector<std::string> sorted(const FakeApi::SubCall& call)
	{
		std::vector<std::string> out = call.codes;
		std::sort(out.begin(), out.end());
		return out;
	}

	inline std::string joined(const std::vector<std::string>& v)
	{
		std::string out;
		for (const auto& s : v)
		{
			out += s;
			out += '|';
		}
		return out;
	}
}

/* ==================== 镜像自证（先证明缝是对的，再谈覆盖） ==================== */

TEST_F(XtpSeamF, mirror_head_slots_track_ctor_and_register_spi)
{
	/* SetUp 已经先验过「灌值前」的状态（api 空、两个集合空、尾部 shared_ptr 空），
	 * 这里看的是「公开入口写进去的东西能不能从镜像槽里读回来」 */
	EXPECT_EQ(&sink, m->m_sink);
	EXPECT_EQ(&bdmgr, m->m_pBaseDataMgr);
	EXPECT_EQ(0u, m->m_uTradingDate);
	EXPECT_EQ(0, m->m_iRequestID);
	EXPECT_EQ(&api, m->m_pUserAPI);

	CodeSet codes;
	codes.insert("SSE.600519");
	codes.insert("SZSE.000001");
	codes.insert("SZSE.300750");
	codes.insert("HK.00700");
	codes.insert("noslashcode");
	parser.subscribe(codes);

	EXPECT_EQ(1u, m->m_fitSHSubs.size());
	EXPECT_EQ(2u, m->m_fitSZSubs.size());
	EXPECT_EQ(1u, m->m_fitSHSubs.count("600519"));
	EXPECT_EQ(0u, m->m_fitSHSubs.count("SSE.600519"));
	EXPECT_EQ(1u, m->m_fitSZSubs.count("000001"));
	EXPECT_EQ(1u, m->m_fitSZSubs.count("300750"));
	EXPECT_EQ(0u, m->m_fitSZSubs.count("HK.00700"));
	EXPECT_EQ(0u, m->m_fitSZSubs.count("noslashcode"));
}

TEST_F(XtpSeamF, mirror_tail_slots_are_the_objects_the_parser_uses)
{
	EXPECT_EQ(15u, m->m_uHBInterval);
	EXPECT_EQ(2097152u, m->m_uBuffSize);
	EXPECT_EQ(7u, m->m_uClientID);
	EXPECT_EQ(6101, m->m_iPort);
	EXPECT_STREQ("127.0.0.1", m->m_strHost.c_str());
	EXPECT_STREQ("800123", m->m_strUser.c_str());
	EXPECT_STREQ("secret", m->m_strPass.c_str());
	EXPECT_STREQ("0.0.0.0", m->m_strLocalIP.c_str());
	EXPECT_EQ(XTP_PROTOCOL_UDP, m->m_iProtocol);
	EXPECT_EQ(&api, m->m_pUserAPI);
	/* 占位 worker 必须已经非空，否则 connect() 会去拉起不可回收线程（T1-XTP-02） */
	EXPECT_EQ(workerPlaceholder.get(), m->_thrd_worker.get());
	EXPECT_EQ(nullptr, m->_worker);
	/* 镜像里的 _asyncio 就是真对象那一个：此时队列全空 */
	EXPECT_EQ(std::size_t{0}, m->_asyncio.poll());
}
/* ==================== DoLogin：判空 / 两条失败支路 ==================== */

TEST_F(XtpLoginF, login_touches_nothing_when_api_pointer_is_null)
{
	m->m_pUserAPI = nullptr;
	api.loginRet = 0;

	EXPECT_TRUE(parser.connect());
	EXPECT_FALSE(parser.isConnected());
	EXPECT_EQ(0, api.loginCalls);
	EXPECT_EQ(int64_t{-1}, api.lastHbInterval);
	EXPECT_EQ(uint64_t{0}, api.lastUdpBuffer);
	EXPECT_TRUE(sink.logs.empty());
	EXPECT_TRUE(sink.events.empty());
	EXPECT_EQ(std::size_t{0}, pump());
	EXPECT_EQ(0u, m->m_uTradingDate);
}

TEST_F(XtpLoginF, login_minus_one_logs_connect_failure_and_defers_the_event)
{
	api.loginRet = -1;
	api.setErrMsg("tcp refused");

	EXPECT_TRUE(parser.connect());

	/* 同步段只有一条错误日志；WPE_Connect 是 post 进去的，没 pump 不该出现 */
	ASSERT_EQ(1u, sink.logs.size());
	EXPECT_EQ(LL_ERROR, sink.logs[0].first);
	EXPECT_EQ("[ParserXTP] Connecting server failed: tcp refused", sink.logs[0].second);
	EXPECT_TRUE(sink.events.empty());
	/* 失败支路不进 DoSubscribeMD */
	EXPECT_TRUE(api.mdCalls.empty());
	EXPECT_TRUE(api.tbtCalls.empty());
	EXPECT_EQ(0u, m->m_uTradingDate);
}

TEST_F(XtpLoginF, login_minus_one_pump_delivers_connect_event_carrying_the_raw_code)
{
	api.loginRet = -1;
	api.setErrMsg("tcp refused");
	parser.connect();

	EXPECT_EQ(1u, pump());
	ASSERT_EQ(1u, sink.events.size());
	EXPECT_EQ(WPE_Connect, sink.events[0].first);
	EXPECT_EQ(-1, sink.events[0].second);
}

TEST_F(XtpLoginF, login_other_failure_sends_one_sync_and_one_posted_connect_event)
{
	api.loginRet = 2;
	api.setErrMsg("send buf full");

	parser.connect();

	/* L470 同步一次 WPE_Connect(0)，L471-473 再 post 一次 WPE_Connect(0) */
	ASSERT_EQ(1u, sink.events.size());
	EXPECT_EQ(WPE_Connect, sink.events[0].first);
	EXPECT_EQ(0, sink.events[0].second);
	ASSERT_EQ(1u, sink.logs.size());
	EXPECT_EQ("[ParserXTP] Sending login request failed: send buf full", sink.logs[0].second);

	EXPECT_EQ(1u, pump());
	ASSERT_EQ(2u, sink.events.size());
	EXPECT_EQ(WPE_Connect, sink.events[1].first);
	EXPECT_EQ(0, sink.events[1].second);
	EXPECT_EQ(0u, m->m_uTradingDate);
}

TEST_F(XtpLoginF, login_failure_without_sink_stays_silent_and_posts_nothing)
{
	parser.registerSpi(nullptr);
	api.loginRet = -1;
	api.setErrMsg("nobody home");

	EXPECT_TRUE(parser.connect());
	EXPECT_TRUE(sink.logs.empty());
	EXPECT_TRUE(sink.events.empty());
	EXPECT_EQ(std::size_t{0}, pump());
	/* 没 sink 也不该跳过 SetHeartBeatInterval：它在 m_sink 判定之前 */
	EXPECT_EQ(int64_t{15}, api.lastHbInterval);
	EXPECT_EQ(uint64_t{2097152}, api.lastUdpBuffer);
}

TEST_F(XtpLoginF, login_error_message_bytes_reach_the_sink_unchanged)
{
	/* 厂商 error_msg 是定长 GBK 数组（xtp_api_struct_common.h:36 XTPRI），不能被换算改字节 */
	const char* gbk = "\xc6\xb7\xb6\xc8\xb2\xbb\xd7\xe3";	// GBK「额度不足」
	api.loginRet = -1;
	api.setErrMsg(gbk);

	parser.connect();

	ASSERT_EQ(1u, sink.logs.size());
	const std::string expected = std::string("[ParserXTP] Connecting server failed: ") + gbk;
	EXPECT_EQ(expected, sink.logs[0].second);
	EXPECT_EQ(38u + 8u, sink.logs[0].second.size());
}

TEST_F(XtpLoginF, login_forwards_every_seeded_config_field_to_the_vendor_call)
{
	api.loginRet = -1;
	parser.connect();

	ASSERT_EQ(1, api.loginCalls);
	EXPECT_EQ("127.0.0.1", api.loginIp);
	EXPECT_EQ(6101, api.loginPort);
	EXPECT_EQ("800123", api.loginUser);
	EXPECT_EQ("secret", api.loginPass);
	EXPECT_EQ(XTP_PROTOCOL_UDP, api.loginProto);
	EXPECT_EQ("0.0.0.0", api.loginLocalIp);
	EXPECT_EQ(int64_t{15}, api.lastHbInterval);
	EXPECT_EQ(uint64_t{2097152}, api.lastUdpBuffer);
}

/* ==================== DoLogin：成功支路 ==================== */

TEST_F(XtpLoginF, login_success_sets_trading_date_and_starts_subscription_before_pump)
{
	api.loginRet = 0;
	std::snprintf(api.tradingDay, sizeof(api.tradingDay), "%s", "20260105");

	EXPECT_TRUE(parser.connect());

	/* strToTime 剥掉冒号后转 uint32；8 位数字原样通过 */
	EXPECT_EQ(20260105u, m->m_uTradingDate);
	/* 事件是 post 的，pump 之前一条都不该落地 */
	EXPECT_TRUE(sink.events.empty());
	/* DoSubscribeMD 是同步跑的 */
	ASSERT_EQ(2u, api.mdCalls.size());
	ASSERT_EQ(2u, api.tbtCalls.size());

	/* 5 条日志：1 条登录成功 + SH/SZ 各 2 条订阅成功 */
	ASSERT_EQ(5u, sink.logs.size());
	EXPECT_EQ(LL_INFO, sink.logs[0].first);
	EXPECT_EQ("[ParserXTP] Connecting server successed: 0, begin to subscibe data ...", sink.logs[0].second);
	EXPECT_EQ("[ParserXTP] Market data of 1 instruments of SSE subscribed", sink.logs[1].second);
	EXPECT_EQ("[ParserXTP] Tick_by_tick data of 1 instruments of SSE subscribed", sink.logs[2].second);
	EXPECT_EQ("[ParserXTP] Market data of 1 instruments of SZSE subscribed", sink.logs[3].second);
	EXPECT_EQ("[ParserXTP] Tick_by_tick data of 1 instruments of SZSE subscribed", sink.logs[4].second);
}

TEST_F(XtpLoginF, login_success_pump_delivers_connect_then_login_in_that_order)
{
	api.loginRet = 0;
	parser.connect();

	EXPECT_EQ(1u, pump());
	ASSERT_EQ(2u, sink.events.size());
	EXPECT_EQ(WPE_Connect, sink.events[0].first);
	EXPECT_EQ(0, sink.events[0].second);
	EXPECT_EQ(WPE_Login, sink.events[1].first);
	EXPECT_EQ(0, sink.events[1].second);
	/* 队列排空 */
	EXPECT_EQ(std::size_t{0}, pump());
}

TEST_F(XtpLoginF, login_success_with_dashed_trading_day_corrupts_the_trading_date)
{
	/*
	 * strToTime（ParserXTP.cpp:57-71）只剥 ':'，其余字符全交给 convert::to_uint32，
	 * 而后者是 `h*10 + *str - '0'` 且零校验（Converter.hpp:57-60）：
	 * "2026-01-05" → 2,0,2,6,'-0',1,'-',0,5 → 2025700705（'-' 贡献 -3）。
	 */
	api.loginRet = 0;
	std::snprintf(api.tradingDay, sizeof(api.tradingDay), "%s", "2026-01-05");

	parser.connect();

	EXPECT_EQ(2025700705u, m->m_uTradingDate);
	EXPECT_NE(20260105u, m->m_uTradingDate);
}

TEST_F(XtpLoginF, login_success_with_colon_separated_trading_day_strips_only_colons)
{
	api.loginRet = 0;
	std::snprintf(api.tradingDay, sizeof(api.tradingDay), "%s", "2026:01:05");

	parser.connect();

	EXPECT_EQ(20260105u, m->m_uTradingDate);
}

TEST_F(XtpLoginF, connect_returns_true_even_when_login_failed_and_never_creates_a_worker)
{
	api.loginRet = -1;
	EXPECT_TRUE(parser.connect());
	api.loginRet = 0;
	EXPECT_TRUE(parser.connect());
	EXPECT_EQ(2, api.loginCalls);
	/* L146 判空为假 → 一个线程都不该建，_worker 也保持空 */
	EXPECT_EQ(workerPlaceholder.get(), m->_thrd_worker.get());
	EXPECT_EQ(nullptr, m->_worker);
}

/* ==================== DoSubscribeMD ==================== */

TEST_F(XtpLoginF, subscribe_md_strips_exchange_prefix_and_routes_each_exchange)
{
	/* 夹具基线已有 SSE.600519 / SZSE.000001，再补三只（含两只不该进任何集合的） */
	CodeSet codes;
	codes.insert("SSE.688981");
	codes.insert("SSE.600000");
	codes.insert("SZSE.300750");
	codes.insert("HK.00700");
	parser.subscribe(codes);		// 未登录 → 只进缓存
	api.loginRet = 0;
	parser.connect();

	ASSERT_EQ(2u, api.mdCalls.size());
	EXPECT_EQ(XTP_EXCHANGE_SH, api.mdCalls[0].exchg);
	EXPECT_EQ(XTP_EXCHANGE_SZ, api.mdCalls[1].exchg);
	EXPECT_EQ(3, api.mdCalls[0].count);
	EXPECT_EQ(2, api.mdCalls[1].count);
	EXPECT_EQ(5, api.mdCalls[0].count + api.mdCalls[1].count);
	/* 下发的是剥掉 SSE./SZSE. 之后的裸 ticker；HK. 前缀整只被丢 */
	EXPECT_EQ("600000|600519|688981|", joined(sorted(api.mdCalls[0])));
	EXPECT_EQ("000001|300750|", joined(sorted(api.mdCalls[1])));
	EXPECT_EQ(0u, m->m_fitSHSubs.count("HK.00700"));
}

TEST_F(XtpLoginF, subscribe_md_order_is_market_data_then_tick_by_tick_per_exchange)
{
	api.loginRet = 0;
	parser.connect();

	ASSERT_EQ(2u, api.mdCalls.size());
	ASSERT_EQ(2u, api.tbtCalls.size());
	EXPECT_EQ(XTP_EXCHANGE_SH, api.mdCalls[0].exchg);
	EXPECT_EQ(XTP_EXCHANGE_SZ, api.mdCalls[1].exchg);
	EXPECT_EQ(XTP_EXCHANGE_SH, api.tbtCalls[0].exchg);
	EXPECT_EQ(XTP_EXCHANGE_SZ, api.tbtCalls[1].exchg);
	/* 逐笔用的是同一批 ticker（没有 SSE./SZSE. 前缀） */
	EXPECT_EQ(joined(sorted(api.mdCalls[0])), joined(sorted(api.tbtCalls[0])));
	EXPECT_EQ(joined(sorted(api.mdCalls[1])), joined(sorted(api.tbtCalls[1])));
}

TEST_F(XtpLoginF, subscribe_md_market_data_failure_logs_sse_and_still_subs_tick_by_tick)
{
	api.loginRet = 0;
	api.mdRet = -5;
	parser.connect();

	EXPECT_EQ(2u, api.mdCalls.size());
	EXPECT_EQ(2u, api.tbtCalls.size());
	EXPECT_TRUE(sink.hasLog("[ParserXTP] Sending md subscribe request of SSE failed: -5"));
	EXPECT_TRUE(sink.hasLog("[ParserXTP] Sending md subscribe request of SZSE failed: -5"));
	EXPECT_TRUE(sink.hasLog("[ParserXTP] Tick_by_tick data of 1 instruments of SSE subscribed"));
	EXPECT_FALSE(sink.hasLog("Market data of 1 instruments"));
}

TEST_F(XtpLoginF, subscribe_md_tick_by_tick_failure_logs_its_own_line)
{
	api.loginRet = 0;
	api.tbtRet = 7;
	parser.connect();

	EXPECT_EQ(2u, sink.logCount("[ParserXTP] Sending tick_by_tick subscribe request"));
	EXPECT_TRUE(sink.hasLog("[ParserXTP] Sending tick_by_tick subscribe request of SSE failed: 7"));
	EXPECT_TRUE(sink.hasLog("[ParserXTP] Sending tick_by_tick subscribe request of SZSE failed: 7"));
	EXPECT_TRUE(sink.hasLog("[ParserXTP] Market data of 1 instruments of SZSE subscribed"));
}

TEST_F(XtpLoginF, subscribe_md_with_empty_buffers_touches_the_api_zero_times)
{
	m->m_fitSHSubs.clear();
	m->m_fitSZSubs.clear();
	api.loginRet = 0;
	sink.logs.clear();

	EXPECT_TRUE(parser.connect());
	EXPECT_TRUE(api.mdCalls.empty());
	EXPECT_TRUE(api.tbtCalls.empty());
	/* 只剩登录成功那一条日志，四个订阅成功/失败分支都不进 */
	ASSERT_EQ(1u, sink.logs.size());
	EXPECT_TRUE(sink.hasLog("begin to subscibe data ..."));
	EXPECT_EQ(20260105u, m->m_uTradingDate);
}

TEST_F(XtpLoginF, subscribe_md_clears_only_its_local_copy_so_buffers_survive)
{
	/* L536/L578 的 codeFilter.clear() 清的是拷贝，成员集合只增不减 */
	api.loginRet = 0;
	parser.connect();

	ASSERT_EQ(1u, m->m_fitSHSubs.size());
	ASSERT_EQ(1u, m->m_fitSZSubs.size());
	EXPECT_EQ(1u, m->m_fitSHSubs.count("600519"));
	EXPECT_EQ(1u, m->m_fitSZSubs.count("000001"));
}

TEST_F(XtpLoginF, second_login_resubscribes_the_very_same_codes)
{
	api.loginRet = 0;
	parser.connect();
	parser.connect();

	EXPECT_EQ(2, api.loginCalls);
	/* 两次登录 → 两遍完全相同的订阅（重连风暴时这是重复请求，见 BUG-T27-002） */
	ASSERT_EQ(4u, api.mdCalls.size());
	EXPECT_EQ(joined(sorted(api.mdCalls[0])), joined(sorted(api.mdCalls[2])));
	EXPECT_EQ(joined(sorted(api.mdCalls[1])), joined(sorted(api.mdCalls[3])));
	ASSERT_EQ(4u, api.tbtCalls.size());
	EXPECT_EQ(1u, m->m_fitSHSubs.size());
}

/* ==================== subscribe() 登录后支路 ==================== */

TEST_F(XtpOnlineF, post_login_subscribe_sends_market_data_but_never_tick_by_tick)
{
	CodeSet codes;
	codes.insert("SSE.688981");
	parser.subscribe(codes);

	ASSERT_EQ(1u, api.mdCalls.size());
	EXPECT_EQ(XTP_EXCHANGE_SH, api.mdCalls[0].exchg);
	EXPECT_EQ(1, api.mdCalls[0].count);
	EXPECT_EQ("688981", api.mdCalls[0].codes[0]);
	/* DoSubscribeMD 会订逐笔，登录后的增量订阅不会（BUG-T27-003） */
	EXPECT_TRUE(api.tbtCalls.empty());
	EXPECT_TRUE(sink.hasLog("[ParserXTP] Market data of 1 instruments of SSE subscribed"));
}

TEST_F(XtpOnlineF, post_login_subscribe_splits_two_exchanges_into_two_calls)
{
	CodeSet codes;
	codes.insert("SSE.600000");
	codes.insert("SZSE.000001");
	codes.insert("SZSE.300750");
	parser.subscribe(codes);

	ASSERT_EQ(2u, api.mdCalls.size());
	EXPECT_EQ(XTP_EXCHANGE_SH, api.mdCalls[0].exchg);
	EXPECT_EQ(1, api.mdCalls[0].count);
	EXPECT_EQ(XTP_EXCHANGE_SZ, api.mdCalls[1].exchg);
	EXPECT_EQ(2, api.mdCalls[1].count);
	EXPECT_EQ("000001|300750|", joined(sorted(api.mdCalls[1])));
	EXPECT_TRUE(sink.hasLog("[ParserXTP] Market data of 1 instruments of SSE subscribed"));
	EXPECT_TRUE(sink.hasLog("[ParserXTP] Market data of 2 instruments of SZSE subscribed"));
	/* 一共 2 个交易所、各 1 次调用：没有第三次 */
	EXPECT_EQ(2u, api.mdCalls.size());
	EXPECT_EQ(0u, api.tbtCalls.size());
}

TEST_F(XtpOnlineF, post_login_subscribe_drops_foreign_prefixes_without_any_call)
{
	const size_t mdBefore = api.mdCalls.size();
	CodeSet codes;
	codes.insert("HK.00700");
	codes.insert("SGX.DJI");
	codes.insert("barecode");
	parser.subscribe(codes);

	EXPECT_EQ(mdBefore, api.mdCalls.size());
	EXPECT_TRUE(sink.logs.empty());
	/* 两个空集合分支都不进 → 成员集合也不该被污染 */
	EXPECT_EQ(1u, m->m_fitSHSubs.size());
	EXPECT_EQ(1u, m->m_fitSZSubs.size());
	EXPECT_EQ(0u, m->m_fitSHSubs.count("barecode"));
}

TEST_F(XtpOnlineF, post_login_subscribe_accumulates_into_member_buffers)
{
	CodeSet codes;
	codes.insert("SSE.688981");
	parser.subscribe(codes);
	parser.subscribe(codes);

	/* 直订分支同样往成员集合里塞（CodeSet 去重 → 尺寸不涨，但内容变了） */
	EXPECT_EQ(2u, m->m_fitSHSubs.size());
	EXPECT_EQ(1u, m->m_fitSHSubs.count("688981"));
	EXPECT_EQ(2u, api.mdCalls.size());
	/* 缓存里现在是 2 只，下一次登录会按 2 只一起发 */
	api.loginRet = 0;
	sink.logs.clear();
	parser.connect();
	ASSERT_EQ(4u, api.mdCalls.size());
	EXPECT_EQ(2, api.mdCalls[2].count);
	EXPECT_EQ("600519|688981|", joined(sorted(api.mdCalls[2])));
	EXPECT_TRUE(sink.hasLog("[ParserXTP] Market data of 2 instruments of SSE subscribed"));
}

TEST_F(XtpOnlineF, post_login_subscribe_failure_logs_the_api_return_code)
{
	api.mdRet = -3;
	CodeSet codes;
	codes.insert("SZSE.300750");
	parser.subscribe(codes);

	ASSERT_EQ(1u, api.mdCalls.size());
	EXPECT_TRUE(sink.hasLog("[ParserXTP] Sending md subscribe request of SZSE failed: -3"));
	EXPECT_FALSE(sink.hasLog("Market data of 1 instruments of SZSE subscribed"));
}

TEST_F(XtpOnlineF, post_login_subscribe_logs_sse_failure_on_its_own_line)
{
	/* L638-642：上证侧的失败文案与深证侧是两条独立的分支 */
	api.mdRet = 11;
	CodeSet codes;
	codes.insert("SSE.600000");
	parser.subscribe(codes);

	ASSERT_EQ(1u, api.mdCalls.size());
	EXPECT_EQ(XTP_EXCHANGE_SH, api.mdCalls[0].exchg);
	EXPECT_EQ(1u, sink.logs.size());
	EXPECT_EQ(LL_ERROR, sink.logs[0].first);
	EXPECT_EQ("[ParserXTP] Sending md subscribe request of SSE failed: 11", sink.logs[0].second);
	/* 失败也照样进缓存 → 下次登录会重发（见 BUG-T27-002） */
	EXPECT_EQ(1u, m->m_fitSHSubs.count("600000"));
	EXPECT_EQ(0u, api.tbtCalls.size());
}

TEST_F(XtpOnlineF, post_login_subscribe_without_api_buffers_only)
{
	parser.disconnect();
	ASSERT_EQ(nullptr, m->m_pUserAPI);
	const size_t mdBefore = api.mdCalls.size();

	CodeSet codes;
	codes.insert("SSE.688981");
	codes.insert("SZSE.000002");
	parser.subscribe(codes);

	/* 交易日还在（disconnect 不清 m_uTradingDate）→ 走登录后分支，但 api 判空挡住下发 */
	EXPECT_EQ(20260105u, m->m_uTradingDate);
	EXPECT_EQ(mdBefore, api.mdCalls.size());
	EXPECT_EQ(1u, m->m_fitSHSubs.count("688981"));
	EXPECT_TRUE(sink.logs.empty());
}

/* ==================== disconnect / release / isConnected ==================== */

TEST_F(XtpOnlineF, disconnect_unregisters_spi_releases_once_and_nulls_the_pointer)
{
	EXPECT_TRUE(parser.isConnected());

	EXPECT_TRUE(parser.disconnect());

	ASSERT_EQ(1u, api.spiRegs.size());
	EXPECT_EQ(nullptr, api.spiRegs[0]);
	EXPECT_EQ(1, api.releaseCalls);
	EXPECT_EQ(nullptr, m->m_pUserAPI);
	EXPECT_FALSE(parser.isConnected());
	/* disconnect 不碰交易日，也不动缓存 */
	EXPECT_EQ(20260105u, m->m_uTradingDate);
	EXPECT_EQ(1u, m->m_fitSHSubs.size());
}

TEST_F(XtpOnlineF, second_disconnect_is_a_no_op)
{
	EXPECT_TRUE(parser.disconnect());
	EXPECT_TRUE(parser.disconnect());
	EXPECT_TRUE(parser.disconnect());

	EXPECT_EQ(1u, api.spiRegs.size());
	EXPECT_EQ(1, api.releaseCalls);
	EXPECT_FALSE(parser.isConnected());
}

TEST_F(XtpOnlineF, release_routes_through_disconnect_and_keeps_the_io_context_idle)
{
	parser.release();

	EXPECT_EQ(1, api.releaseCalls);
	EXPECT_FALSE(parser.isConnected());
	EXPECT_EQ(std::size_t{0}, pump());
}

TEST_F(XtpSeamF, is_connected_and_disconnect_pair_without_an_api_pointer)
{
	/* 未持指针：isConnected false、disconnect/release 都是空转到 return true */
	m->m_pUserAPI = nullptr;
	EXPECT_FALSE(parser.isConnected());
	EXPECT_TRUE(parser.disconnect());
	parser.release();
	EXPECT_EQ(0, api.releaseCalls);
	EXPECT_TRUE(api.spiRegs.empty());
}

/* ==================== 崩溃/挂死输入：只登记，不启用 ==================== */

/*
 * ParserXTP.cpp:119-131：dlopen 失败 → m_hInst=NULL → get_symbol 返回 NULL →
 * L131 直接调用空函数指针 = SIGSEGV。本机没有 macOS 版 libxtpquoteapi（src/API/XTP2.2.32.2
 * 只有 linux/x64/x86 三份），所以 init() 一旦真跑就必崩，profraw 也写不出来。
 * 根因与阻塞面已由 T1-XTP-01 登记，这里只留可复现入口。
 */
TEST_F(XtpLoginF, DISABLED_init_dereferences_null_creator_when_vendor_dylib_is_absent)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("host", "127.0.0.1");
	cfg->append("port", 6101);
	cfg->append("user", "800123");
	cfg->append("pass", "secret");
	cfg->append("protocol", static_cast<uint32_t>(XTP_PROTOCOL_UDP));
	cfg->append("clientid", 7u);
	cfg->append("hbinterval", 15u);
	cfg->append("buffsize", 2097152u);
	cfg->append("flowdir", "/tmp/xtp_flow");
	cfg->append("xtpmodule", "xtpquoteapi");
	EXPECT_TRUE(parser.init(cfg));
	cfg->release();
}

/*
 * L462-464 / L471-473 两个 post 的 lambda 直接 `m_sink->handleEvent(...)`，
 * 而 L482-488 那个成功分支的 lambda 外面包了 `if (m_sink)`。
 * 登录失败时 m_sink 判定为真 → 入队；handler 真正执行前若被 registerSpi(nullptr) 摘掉 sink，
 * worker 线程跑这个 handler 就是解引用 NULL → SIGSEGV（启用即崩，故 DISABLED）。
 */
TEST_F(XtpLoginF, DISABLED_posted_login_handlers_deref_sink_nulled_after_post)
{
	api.loginRet = -1;
	api.setErrMsg("tcp refused");
	parser.connect();
	parser.registerSpi(nullptr);
	pump();	// 跑 lambda：m_sink 已经是 NULL
}

/*
 * connect() 在 `_thrd_worker == NULL` 时拉起 `while(true){sleep(2ms); _asyncio.run_one();}`
 * 且 release/disconnect 都不回收它（L150-157、L137-140），parser 析构后 io_context 变成
 * 悬垂对象 → 挂死或退出期崩。同 T1-XTP-02。
 */
TEST_F(XtpSeamF, DISABLED_connect_without_pre_seeded_worker_starts_unstoppable_thread)
{
	m->_thrd_worker = nullptr;
	api.loginRet = -1;
	api.setErrMsg("tcp refused");
	EXPECT_TRUE(parser.connect());
	EXPECT_NE(nullptr, m->_thrd_worker.get());
	EXPECT_NE(nullptr, m->_worker.get());
}
