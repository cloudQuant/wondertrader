/**
 * r72_hft_uft_paths_test.cpp  (lane R72)
 *
 * 靶子：HftMocker / UftMocker 的公开订单回调链（p08 用私有注入覆盖过装配，
 * 这里全部从 stra_buy/stra_sell/stra_enter_* + handle_tick 进入）：
 *   HftMocker.cpp L345-L430 on_tick -> procOrder(L700-L787) -> on_trade/do_set_position(L1092+)
 *   HftMocker.cpp L548-L572 stra_cancel 任务排空、L1024-L1076 落盘
 *   UftMocker.cpp L228-L287 on_tick -> procOrder(L889-L970) -> update_position(L1155+)
 *   UftMocker.cpp L407-L454 stra_cancel、L1112-L1139 落盘
 *
 * 成交对手价来自 replayer 的 loader tick（公开 get_last_tick），不依赖私有 _last_tick。
 */
#include <gtest/gtest.h>

#include <WtBtCore/HftMocker.h>
#include <WtBtCore/UftMocker.h>

#include "r72_common.h"

namespace
{
	namespace fs = std::filesystem;

	const char* const kCode = "SSE.STK.000001";

	WTSTickStruct book_tick(uint32_t date, uint32_t hhmmssmmm, double last, double bid, double ask)
	{
		return r72::make_tick_struct(kCode, date, hhmmssmmm, last, bid, ask, 100u, 100u);
	}

	void init_loader(r72::Loader& loader)
	{
		std::vector<WTSTickStruct> ticks;
		ticks.emplace_back(book_tick(20200102u, 93000000u, 10.0, 9.99, 10.01));
		ticks.emplace_back(book_tick(20200102u, 93100000u, 10.2, 10.19, 10.21));
		loader.ticks[std::string(kCode) + "#20200102"] = ticks;
	}
}

class R72HftUftPaths : public r72::Fixture
{
};

/*
 * HftMocker 市价/限价成交 + 部分平仓 + 落盘：
 * on_tick 先 procOrder，再用 replayer 的 loader tick 作为对手价成交；
 * 买单吃 ask 10.01，卖单吃 bid 9.99，剩余持仓按买一价更新浮盈。
 */
TEST_F(R72HftUftPaths, HftMarketOrderFillsAndDumps)
{
	r72::Loader loader;
	init_loader(loader);

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	HftMocker hft(&rp, "r72_hft");
	rp.register_sink(&hft, "r72_hft");
	ASSERT_TRUE(rp.prepare());

	ASSERT_EQ(1u, hft.stra_buy(kCode, 0.0, 2, "buy").size());
	EXPECT_DOUBLE_EQ(2.0, hft.stra_get_undone(kCode));

	WTSTickData* t1 = r72::make_tick(kCode, 20200102u, 93000000u, 10.0, 9.99, 10.01);
	hft.handle_tick(kCode, t1, 0);
	t1->release();

	EXPECT_DOUBLE_EQ(2.0, hft.stra_get_position(kCode));
	EXPECT_DOUBLE_EQ(10.01, hft.stra_get_position_avgpx(kCode));
	EXPECT_DOUBLE_EQ(0.0, hft.stra_get_undone(kCode));

	// 限价卖单：bid 9.99 >= 9.5，立即成交 1 手
	ASSERT_EQ(1u, hft.stra_sell(kCode, 9.5, 1, "sell").size());
	WTSTickData* t2 = r72::make_tick(kCode, 20200102u, 93100000u, 10.2, 9.99, 10.21);
	hft.handle_tick(kCode, t2, 0);
	t2->release();
	EXPECT_DOUBLE_EQ(1.0, hft.stra_get_position(kCode));
	EXPECT_DOUBLE_EQ(0.0, hft.stra_get_undone(kCode));

	hft.handle_replay_done();
	const fs::path dir = _out / "r72_hft";
	EXPECT_GE(r72::line_count(r72::read_text(dir / "trades.csv")), 3u);
	EXPECT_GE(r72::line_count(r72::read_text(dir / "closes.csv")), 2u);
	EXPECT_GE(r72::line_count(r72::read_text(dir / "signals.csv")), 3u);
}

/*
 * HftMocker 限价不越过时保持挂单（L747-L759 的 false 分支），
 * stra_cancel 只入队，下一次 on_tick 的 procTask 才真正撤掉；随后空回调/落盘安全。
 */
TEST_F(R72HftUftPaths, HftPendingLimitCancelAndEmptyCallbacks)
{
	r72::Loader loader;
	init_loader(loader);

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	HftMocker hft(&rp, "r72_hft_cancel");
	rp.register_sink(&hft, "r72_hft_cancel");
	ASSERT_TRUE(rp.prepare());

	auto ids = hft.stra_buy(kCode, 5.0, 1, "low");
	ASSERT_EQ(1u, ids.size());
	EXPECT_DOUBLE_EQ(1.0, hft.stra_get_undone(kCode));

	WTSTickData* t1 = r72::make_tick(kCode, 20200102u, 93000000u, 10.0, 9.99, 10.01);
	hft.handle_tick(kCode, t1, 0);
	t1->release();
	EXPECT_DOUBLE_EQ(1.0, hft.stra_get_undone(kCode));
	EXPECT_DOUBLE_EQ(0.0, hft.stra_get_position(kCode));

	EXPECT_TRUE(hft.stra_cancel(ids[0]));
	WTSTickData* t2 = r72::make_tick(kCode, 20200102u, 93100000u, 10.0, 9.99, 10.01);
	hft.handle_tick(kCode, t2, 0);
	t2->release();
	EXPECT_DOUBLE_EQ(0.0, hft.stra_get_undone(kCode));

	hft.handle_session_begin(20200102u);
	hft.handle_session_end(20200102u);
	hft.handle_schedule(20200102u, 930u);
	hft.handle_bar_close(kCode, "m1", 1, nullptr);
	hft.handle_replay_done();

	const fs::path dir = _out / "r72_hft_cancel";
	EXPECT_TRUE(fs::exists(dir / "trades.csv"));
	EXPECT_EQ(1u, r72::line_count(r72::read_text(dir / "trades.csv")));
}

/*
 * UftMocker 市价开仓 -> 平今/平昨（默认 CM_OpenCover）：
 * procOrder 吃 ask 10.01，update_position offset=0 建仓；
 * stra_exit_long 占用可用量后 offset=1 平仓，成交价取 bid 9.99。
 */
TEST_F(R72HftUftPaths, UftMarketOrderFillsAndCloses)
{
	r72::Loader loader;
	init_loader(loader);

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	UftMocker uft(&rp, "r72_uft");
	rp.register_sink(&uft, "r72_uft");
	ASSERT_TRUE(rp.prepare());

	ASSERT_NE(0u, uft.stra_enter_long(kCode, 0.0, 2));
	EXPECT_DOUBLE_EQ(2.0, uft.stra_get_undone(kCode));

	WTSTickData* t1 = r72::make_tick(kCode, 20200102u, 93000000u, 10.0, 9.99, 10.01);
	uft.handle_tick(kCode, t1, 0);
	t1->release();

	EXPECT_DOUBLE_EQ(2.0, uft.stra_get_position(kCode, false, 1));
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_undone(kCode));

	ASSERT_NE(0u, uft.stra_exit_long(kCode, 0.0, 1));
	WTSTickData* t2 = r72::make_tick(kCode, 20200102u, 93100000u, 10.2, 9.99, 10.21);
	uft.handle_tick(kCode, t2, 0);
	t2->release();

	EXPECT_DOUBLE_EQ(1.0, uft.stra_get_position(kCode, false, 1));
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_undone(kCode));

	uft.handle_replay_done();
	const fs::path dir = _out / "r72_uft";
	EXPECT_GE(r72::line_count(r72::read_text(dir / "trades.csv")), 3u);
	EXPECT_GE(r72::line_count(r72::read_text(dir / "closes.csv")), 2u);
}

/*
 * UftMocker 限价不越过保持挂单 + 撤单任务在下一次 on_tick 排空；
 * 空仓查询族与空 session_end 不改变状态（L392-L405、L456-L470、L1013-L1028）。
 */
TEST_F(R72HftUftPaths, UftPendingLimitCancelAndQueries)
{
	r72::Loader loader;
	init_loader(loader);

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	UftMocker uft(&rp, "r72_uft_cancel");
	rp.register_sink(&uft, "r72_uft_cancel");
	ASSERT_TRUE(rp.prepare());

	uint32_t id = uft.stra_enter_long(kCode, 5.0, 1);
	ASSERT_NE(0u, id);
	EXPECT_DOUBLE_EQ(1.0, uft.stra_get_undone(kCode));

	WTSTickData* t1 = r72::make_tick(kCode, 20200102u, 93000000u, 10.0, 9.99, 10.01);
	uft.handle_tick(kCode, t1, 0);
	t1->release();
	EXPECT_DOUBLE_EQ(1.0, uft.stra_get_undone(kCode));
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_position(kCode));

	EXPECT_TRUE(uft.stra_cancel(id));
	WTSTickData* t2 = r72::make_tick(kCode, 20200102u, 93100000u, 10.0, 9.99, 10.01);
	uft.handle_tick(kCode, t2, 0);
	t2->release();
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_undone(kCode));

	EXPECT_TRUE(uft.stra_cancel_all(kCode).empty());
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_local_position(kCode));
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_position(kCode, true, 2));

	const fs::path dir = _out / "r72_uft_cancel";
	uft.handle_replay_done();
	EXPECT_TRUE(fs::exists(dir / "trades.csv"));
}

/*
 * 缺陷类（禁用，见 bugs/R72.md BUG-R72-001）：
 * UftMocker::stra_enum_position 在持仓非空时无条件解引用 _strategy
 * （UftMocker.cpp L1041），而未装载策略工厂的公开用法下 _strategy==NULL。
 * 同类 null 解引用在 on_session_begin/on_session_end 也存在（p08 已归档）。
 */
TEST_F(R72HftUftPaths, DISABLED_UftEnumPositionWithoutStrategyDerefsNull)
{
	HisDataReplayer rp;
	UftMocker uft(&rp, "r72_uft_enum");
	uft.on_trade(1u, kCode, true, 0, 1.0, 10.0);
	// 持仓表已非空 -> 回调循环解引用 _strategy
	(void)uft.stra_enum_position("");
	FAIL() << "stra_enum_position must not dereference a null strategy";
}
