/*
 * lane T6（收发族第六波）— TraderMocker 可直达面用例
 *
 * 目标文件：src/TraderMocker/TraderMocker.cpp（1006 行）+ src/TraderMocker/TraderMocker.h
 *
 * 可达性口径（先说清楚，避免假装覆盖）：
 *  - 同目录的 seam_tradermocker_test.cpp 是别人跑通的探针，本文件不改它，只新增面。
 *  - TraderMocker 的下单/撤单/查询回报全部写在 `boost::asio::post(_io_service, lambda)` 的
 *    lambda 体里；`_io_service` 是私有成员，只有 connect()/login() 会起线程去 run 它
 *    （connect() 还会真的 open+bind 一个 UDP 端口）。按本轮门禁规则（不起线程、不 sleep、
 *    不绑真实端口、不 `#define private public`），这些 lambda 体 **不进**本文件断言范围，
 *    本文件用「提交后未驱动 io_context 时不得有任何回报」这条可证伪契约把它们钉住。
 *  - `match_once()/extract_buffer()/handle_read()/load_positions()/save_positions()`
 *    与私有成员 `_positions/_awaits/_io_service` 没有任何公开接缝 → 撮合价量判定、
 *    持仓增减/冻结量推进、订单状态机、makeTradeID/makeOrderID 本轮无法直接进入，
 *    已记入 manifests/T6_ctploader_mocker_udp.md 与 bugs/T6_ctploader_mocker_udp.md。
 *  - 自由函数 `splitVolume()` 在 TraderMocker.cpp 里是全局非 static 符号，可直接链接调用，
 *    它是撮合拆量的纯计算内核（价量判定的下游），值得单独锁死不变式。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "TraderMocker/TraderMocker.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSTradeDef.hpp"

USING_NS_WTP;

/// 生产符号：src/TraderMocker/TraderMocker.cpp L52-80（撮合拆量内核）
/// 声明处不给默认实参，调用侧一律显式传三个参数
std::vector<uint32_t> splitVolume(uint32_t vol, uint32_t minQty, uint32_t maxQty);

extern "C"
{
	/// 生产符号：src/TraderMocker/TraderMocker.cpp L34-50（装载入口）
	ITraderApi* createTrader();
	void deleteTrader(ITraderApi*& trader);
}

namespace
{
	/// ITraderSpi 替身：只做计数与日志快照，不接真实柜台、不起线程
	class MockerSink : public ITraderSpi
	{
	public:
		uint32_t n_bdmgr_ = 0;
		uint32_t n_event_ = 0;
		uint32_t n_login_ = 0;
		uint32_t n_log_ = 0;
		uint32_t n_rsp_entrust_ = 0;
		uint32_t n_push_order_ = 0;
		uint32_t n_push_trade_ = 0;
		uint32_t n_rsp_account_ = 0;
		uint32_t n_rsp_position_ = 0;
		uint32_t n_rsp_orders_ = 0;
		uint32_t n_rsp_trades_ = 0;
		uint32_t n_trader_error_ = 0;
		std::vector<std::string> logs_;

		virtual ~MockerSink() {}

		virtual IBaseDataMgr* getBaseDataMgr() override { ++n_bdmgr_; return NULL; }

		virtual void handleTraderLog(WTSLogLevel ll, const char* message) override
		{
			(void)ll;
			++n_log_;
			logs_.emplace_back(message == NULL ? "" : message);
		}

		virtual void handleEvent(WTSTraderEvent e, int32_t ec) override { (void)e; (void)ec; ++n_event_; }

		virtual void onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override
		{
			(void)bSucc; (void)msg; (void)tradingdate;
			++n_login_;
		}

		virtual void onRspEntrust(WTSEntrust* entrust, WTSError* err) override
		{ (void)entrust; (void)err; ++n_rsp_entrust_; }

		virtual void onRspAccount(WTSArray* ayAccounts) override { (void)ayAccounts; ++n_rsp_account_; }

		virtual void onRspPosition(const WTSArray* ayPositions) override
		{ (void)ayPositions; ++n_rsp_position_; }

		virtual void onRspOrders(const WTSArray* ayOrders) override { (void)ayOrders; ++n_rsp_orders_; }

		virtual void onRspTrades(const WTSArray* ayTrades) override { (void)ayTrades; ++n_rsp_trades_; }

		virtual void onPushOrder(WTSOrderInfo* orderInfo) override { (void)orderInfo; ++n_push_order_; }

		virtual void onPushTrade(WTSTradeInfo* tradeRecord) override { (void)tradeRecord; ++n_push_trade_; }

		virtual void onTraderError(WTSError* err, void* pData) override { (void)err; (void)pData; ++n_trader_error_; }
	};

	/// init() 会在工作目录按 mocker_<id>/ 建落盘目录；这里保证用例前后都不留残留
	class MockerDirGuard
	{
	public:
		explicit MockerDirGuard(uint32_t mockerId) : _dir("mocker_" + std::to_string(mockerId)) { clean(); }
		~MockerDirGuard() { clean(); }

		void clean()
		{
			std::error_code ec;
			std::filesystem::remove_all(_dir, ec);
		}

		bool exists() const { return std::filesystem::is_directory(_dir); }

	private:
		std::string _dir;
	};

	WTSVariant* make_cfg(uint32_t mockerId, int span, uint32_t maxQty, uint32_t minQty, int udpPort, bool newpx)
	{
		WTSVariant* p = WTSVariant::createObject();
		p->append("span", span);
		p->append("newpx", newpx);
		p->append("mockerid", mockerId);
		p->append("maxqty", maxQty);
		p->append("minqty", minQty);
		p->append("udp_port", udpPort);
		return p;
	}

	uint32_t tail_seq(const char* eid) { return (uint32_t)strtoul(strrchr(eid, '.') + 1, NULL, 10); }
}

/* ------------------------------------------------------------------ */
/* 装载入口：extern "C" createTrader / deleteTrader                     */
/* ------------------------------------------------------------------ */

TEST(t6_tradermocker_factory, create_delete_roundtrip_clears_caller_pointer)
{
	ITraderApi* nullApi = NULL;
	deleteTrader(nullApi);			// L44：NULL 入参必须被挡住
	EXPECT_EQ(nullptr, nullApi);

	ITraderApi* api = createTrader();
	ASSERT_NE(nullptr, api);
	// 刚造出来的实例不该自称已连接（_thrd_match 为空，L718-721）
	EXPECT_FALSE(api->isConnected());
	api->release();

	deleteTrader(api);
	// deleteTrader 的契约是把出参置空，防止调用方二次 delete
	EXPECT_EQ(nullptr, api);
}

/* ------------------------------------------------------------------ */
/* 纯计算内核：splitVolume（撮合拆量的价量不变式）                       */
/* ------------------------------------------------------------------ */

TEST(t6_split_volume, volume_at_or_below_min_returns_single_whole_part)
{
	// L56-59：vol <= minQty 原样返回，不做随机拆分
	std::vector<uint32_t> below = splitVolume(3, 5, 100);
	ASSERT_EQ(1u, below.size());
	EXPECT_EQ(3u, below[0]);

	std::vector<uint32_t> equal = splitVolume(5, 5, 100);
	ASSERT_EQ(1u, equal.size());
	EXPECT_EQ(5u, equal[0]);

	std::vector<uint32_t> zero = splitVolume(0, 1, 100);
	ASSERT_EQ(1u, zero.size());
	EXPECT_EQ(0u, zero[0]);
}

TEST(t6_split_volume, parts_sum_back_to_volume_and_stay_inside_bounds)
{
	const uint32_t vol = 1000, minQty = 1, maxQty = 100;
	std::vector<uint32_t> parts = splitVolume(vol, minQty, maxQty);

	uint64_t sum = 0;
	for (uint32_t p : parts)
	{
		// 每一片都必须非 0 且落在 [minQty, maxQty]（L66-75 的 clamp 保证上界）
		EXPECT_GT(p, 0u) << "拆量结果不允许出现 0 片: " << p;
		EXPECT_GE(p, minQty);
		EXPECT_LE(p, maxQty);
		sum += p;
	}
	// 随机只影响「怎么切」，不影响总量：拆量必须守恒
	EXPECT_EQ((uint64_t)vol, sum);
	EXPECT_GE(parts.size(), 10u);
}

TEST(t6_split_volume, tail_part_is_clamped_and_uniform_when_max_equals_min)
{
	// 尾片 clamp（L68-69）：737 用 [50,100] 拆，最后一片允许小于 minQty 但不得超上界
	std::vector<uint32_t> odd = splitVolume(737, 50, 100);
	uint64_t oddSum = 0;
	for (uint32_t p : odd)
	{
		EXPECT_GT(p, 0u);
		EXPECT_LE(p, 100u);
		oddSum += p;
	}
	EXPECT_EQ(737ull, oddSum);

	// length = maxQty-minQty+1 = 1 → curVol 恒等于 minQty，结果完全可预测
	std::vector<uint32_t> fixed = splitVolume(12, 4, 4);
	ASSERT_EQ(3u, fixed.size());
	for (uint32_t p : fixed)
		EXPECT_EQ(4u, p);
}

/* ------------------------------------------------------------------ */
/* makeEntrustID：参数防护 + 编号推进                                    */
/* ------------------------------------------------------------------ */

TEST(t6_make_entrust_id, rejects_null_buffer_and_zero_length)
{
	TraderMocker api;
	char buf[64];
	std::memset(buf, 'x', sizeof(buf));

	// L122-123：两个空值防护分支都必须返回 false 且不改写缓冲
	EXPECT_FALSE(api.makeEntrustID(NULL, 64));
	EXPECT_FALSE(api.makeEntrustID(buf, 0));
	EXPECT_EQ('x', buf[0]);
}

TEST(t6_make_entrust_id, writes_mocker_scoped_sequence_after_init)
{
	MockerDirGuard dir(77);
	TraderMocker api;
	WTSVariant* cfg = make_cfg(77, 5, 0, 0, 0, true);
	ASSERT_TRUE(api.init(cfg));
	cfg->release();

	char first[64] = { 0 }, second[64] = { 0 };
	ASSERT_TRUE(api.makeEntrustID(first, (int)sizeof(first)));
	ASSERT_TRUE(api.makeEntrustID(second, (int)sizeof(second)));

	// 形如 me.<日期>.<mockerid>.<序号>：前缀与 mocker 段可证伪 init 真读到了配置（L522）
	EXPECT_EQ(0, strncmp(first, "me.", 3));
	EXPECT_NE(nullptr, strstr(first, ".77."));
	EXPECT_STRNE(first, second);
	// 序号自增（L127 的后置 ++）
	EXPECT_GT(tail_seq(second), tail_seq(first));
}

/* ------------------------------------------------------------------ */
/* init：配置正/负路径                                                   */
/* ------------------------------------------------------------------ */

TEST(t6_init, explicit_config_creates_mocker_dir_and_keeps_given_id)
{
	MockerDirGuard dir(9311);
	TraderMocker api;
	WTSVariant* real = make_cfg(9311, 10, 30, 2, 0, false);
	EXPECT_TRUE(api.init(real));
	real->release();

	// L538-544：init 按 mocker_<id>/ 建落盘目录，_pos_file 指向其中 positions.json
	EXPECT_TRUE(dir.exists());
}

TEST(t6_init, missing_keys_fall_back_to_qty_defaults_and_zero_id)
{
	MockerDirGuard dir(0);
	TraderMocker api;
	// 空对象：span/newpx/mockerid/maxqty/minqty/udp_port 全部取默认 0
	// → 命中 L531-535 的 maxqty 0→100、minqty 0→1 兜底分支，mockerid 落 0
	WTSVariant* cfg = WTSVariant::createObject();
	EXPECT_TRUE(api.init(cfg));
	cfg->release();

	EXPECT_TRUE(dir.exists());

	char eid[64] = { 0 };
	ASSERT_TRUE(api.makeEntrustID(eid, (int)sizeof(eid)));
	EXPECT_NE(nullptr, strstr(eid, ".0."));
}

TEST(t6_init, init_is_repeatable_and_later_config_wins)
{
	MockerDirGuard dirA(9312), dirB(9313);
	TraderMocker api;

	WTSVariant* c1 = make_cfg(9312, 5, 10, 1, 0, true);
	WTSVariant* c2 = make_cfg(9313, 5, 10, 1, 0, true);
	ASSERT_TRUE(api.init(c1));
	ASSERT_TRUE(api.init(c2));

	char eid[64] = { 0 };
	ASSERT_TRUE(api.makeEntrustID(eid, (int)sizeof(eid)));
	// 第二次 init 覆盖了 _mocker_id：证明 init 不是「只读一次」的哑函数
	EXPECT_NE(nullptr, strstr(eid, ".9313."));
	EXPECT_TRUE(dirB.exists());

	c1->release();
	c2->release();
}

/* ------------------------------------------------------------------ */
/* 连接态与生命周期：registerSpi / release / disconnect / logout          */
/* ------------------------------------------------------------------ */

TEST(t6_lifecycle, register_spi_asks_for_base_data_mgr_once)
{
	TraderMocker api;
	MockerSink sink;

	api.registerSpi(&sink);
	// L659-664：registerSpi 必须立刻取一次 BDMgr（替身返回 NULL，仅证明调用发生）
	EXPECT_EQ(1u, sink.n_bdmgr_);
	EXPECT_FALSE(api.isConnected());
}

TEST(t6_lifecycle, release_and_disconnect_are_idempotent_and_emit_no_events)
{
	TraderMocker a;
	MockerSink sinkA;
	a.registerSpi(&sinkA);
	a.release();		// L642 未终止 → L645 置位；_thrd_match/_thrd_worker 皆空，不 join
	a.release();		// L642-643：已终止 → 直接 return
	a.disconnect();		// L707-708：已终止 → 直接 return
	EXPECT_FALSE(a.isConnected());
	EXPECT_EQ(0u, sinkA.n_event_);

	TraderMocker b;
	b.disconnect();		// L710 置位，_thrd_match 空 → 不 join
	EXPECT_FALSE(b.isConnected());
	b.release();		// 此时 release 走 L645 之后的分支（_thrd_worker 仍为空）
}

TEST(t6_lifecycle, logout_reports_zero_and_emits_no_replies)
{
	TraderMocker api;
	MockerSink sink;
	api.registerSpi(&sink);

	// L745-748：mocker 的 logout 是空实现，返回 0 且不产生 onLogout/handleEvent
	EXPECT_EQ(0, api.logout());
	EXPECT_EQ(0u, sink.n_event_);
	EXPECT_EQ(0u, sink.n_login_);
}

/* ------------------------------------------------------------------ */
/* 提交即入队：未驱动 io_context 时不得有任何回报（deferred 语义契约）      */
/* ------------------------------------------------------------------ */

TEST(t6_deferred_submit, query_apis_only_enqueue_and_report_nothing)
{
	TraderMocker api;
	MockerSink sink;
	api.registerSpi(&sink);

	WTSVariant* cfg = make_cfg(9320, 5, 10, 1, 0, true);
	ASSERT_TRUE(api.init(cfg));
	cfg->release();

	// L830-933：四个查询本体都只是 boost::asio::post，回报体在未被 run 的 io_context 上
	EXPECT_EQ(0, api.queryAccount());
	EXPECT_EQ(0, api.queryPositions());
	EXPECT_EQ(0, api.queryOrders());
	EXPECT_EQ(0, api.queryTrades());

	EXPECT_EQ(0u, sink.n_rsp_account_);
	EXPECT_EQ(0u, sink.n_rsp_position_);
	EXPECT_EQ(0u, sink.n_rsp_orders_);
	EXPECT_EQ(0u, sink.n_rsp_trades_);
	EXPECT_EQ(0u, sink.n_trader_error_);
}

TEST(t6_deferred_submit, order_insert_with_null_entrust_is_rejected_before_enqueue)
{
	TraderMocker api;
	MockerSink sink;
	api.registerSpi(&sink);

	// L140-143：空委托直接返回 0，且不得产生任何回报/日志
	EXPECT_EQ(0, api.orderInsert(NULL));
	EXPECT_EQ(0u, sink.n_rsp_entrust_);
	EXPECT_EQ(0u, sink.n_push_order_);
	EXPECT_EQ(0u, sink.n_log_);
}

TEST(t6_deferred_submit, valid_insert_and_cancel_requests_are_deferred_not_reported)
{
	TraderMocker api;
	MockerSink sink;
	api.registerSpi(&sink);

	WTSVariant* cfg = make_cfg(9321, 5, 10, 1, 0, true);
	ASSERT_TRUE(api.init(cfg));
	cfg->release();

	WTSEntrust* ent = WTSEntrust::create("ag2512", 2, 7000.0, "SHFE");
	ASSERT_NE(nullptr, ent);
	ent->setEntrustID("t6-defer-1");
	ent->setDirection(WDT_LONG);
	ent->setOffsetType(WOT_OPEN);
	ent->setPriceType(WPT_LIMITPRICE);

	// L138-146 + L270-322：orderInsert 先 retain 再把校验/建仓塞进 lambda
	EXPECT_EQ(0, api.orderInsert(ent));

	WTSEntrustAction* act = WTSEntrustAction::create("ag2512", "SHFE");
	ASSERT_NE(nullptr, act);
	act->setOrderID("mo.9321.1001");
	act->setEntrustID("t6-defer-1");
	// L750-755：撤单同样只提交，不驱动 io_context
	EXPECT_EQ(0, api.orderAction(act));

	// 未驱动 io_context ⇒ 委托应答/订单推送/成交推送/错误回报一条都不许出现
	EXPECT_EQ(0u, sink.n_rsp_entrust_);
	EXPECT_EQ(0u, sink.n_push_order_);
	EXPECT_EQ(0u, sink.n_push_trade_);
	EXPECT_EQ(0u, sink.n_trader_error_);
	EXPECT_EQ(0u, sink.n_rsp_orders_);

	ent->release();
	act->release();
}

/* ------------------------------------------------------------------ */
/* 明确不进入的路径（整条 DISABLED_，见文件头口径说明）                    */
/* ------------------------------------------------------------------ */

// login() 会 new StdThread 跑 match_once 循环，循环体每轮 sleep 5ms（L725-733），
// 且依赖 io_context 被 run；按「不起线程、不 sleep」整条禁用。
TEST(DISABLED_t6_thread_paths, login_spawns_match_thread_and_sleeps)
{
	TraderMocker api;
	MockerSink sink;
	api.registerSpi(&sink);
	WTSVariant* cfg = make_cfg(9399, 5, 10, 1, 0, true);
	ASSERT_TRUE(api.init(cfg));
	cfg->release();

	api.login("u", "p", "x");	// 起撮合线程
	(void)api.isConnected();	// 此时才为 true
	api.release();				// join 该线程
}

// connect() 走 reconn_udp()：真的 open/bind 一个 UDP 端口并 new StdThread 跑 io_context
// （L689-703 + L666-687）；按「不绑真实端口、不起线程」整条禁用。
TEST(DISABLED_t6_thread_paths, connect_binds_real_udp_socket_and_runs_io_context)
{
	TraderMocker api;
	MockerSink sink;
	api.registerSpi(&sink);
	WTSVariant* cfg = make_cfg(9398, 5, 10, 1, 19827, true);
	ASSERT_TRUE(api.init(cfg));
	cfg->release();

	api.connect();
	api.disconnect();
	api.release();
}

// splitVolume(minQty=0, maxQty=0) 时 length==1、curVol 恒为 0，
// L64-76 的 while(left>0) 走 continue 死循环（挂死路径，只记录不执行）。
// 生产里被 init 的 L531-535 兜底挡住（minqty 0→1），所以是潜在风险而非现网缺陷。
TEST(DISABLED_t6_hang_paths, split_volume_with_zero_bounds_spins_forever)
{
	volatile size_t n = splitVolume(10, 0, 0).size();
	(void)n;
	SUCCEED();
}
