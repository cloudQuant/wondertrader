/*
 * r214_mocker_branch_test.cpp  (lane R214 · wave29)
 *
 * 靶子：TraderMocker 撮合/撤单/查询的剩余分支
 *   - match_once 合约缺失 continue                :344-346
 *   - match_once VolLeft==0 continue              :363-365
 *   - match_once ask 量为 0 continue              :381-382
 *   - match_once ask 价为 0 continue（newpx=false）:384-390
 *   - match_once 限价不成交 continue              :392-395
 *   - match_once CM_None 的空持仓块               :444-447
 *   - disconnect() 的 join 与二次早退             :705-715
 *
 * 与既有 lane 的分工：R113 覆盖了正常成交/部分成交/撤单主链，但对上面这些
 * 「跳过」分支只走过其中一部分；本文件用公开 API（init/connect/login/
 * orderInsert/query*）与本机回环 UDP 逐条构造。跨合约隔离与零量分支分开验证，
 * 通过成交代码和成交所引用的委托 ID 检查撮合归属；newpx=false 的配置对象由公开
 * WTSVariant 构造。
 *
 * 边界：不镜像私有布局、不直接调 match_once；每条用例在 TearDown 里 release()
 * 并 join 撮合线程；UDP 只发本机回环，端口由内核临时分配。
 */
#include "r113_mocker_common.h"

using R214MockerBranches = r113m::MockerTest;

namespace {

WTSContractInfo* make_unregistered_ct(const char* commid, const char* code, ContractCategory cat,
	CoverMode cover)
{
	WTSCommodityInfo* comm = WTSCommodityInfo::create(commid, commid, "SHFE", "TRADING", "CHINA");
	comm->setCategory(cat);
	comm->setCoverMode(cover);
	comm->setPriceMode(PM_Both);
	comm->setTradingMode(TM_Both);
	comm->setPriceTick(1.0);
	comm->setVolScale(10);

	WTSContractInfo* ct = WTSContractInfo::create(code, code, "SHFE", commid);
	ct->setCommInfo(comm);
	return ct;
}

} // namespace

/* 生产: TraderMocker.cpp:344-346 */
TEST_F(R214MockerBranches, UnregisteredOrderDoesNotTradeOnOtherContractTick)
{
	const uint16_t port = r103m::pick_free_udp_port();
	ASSERT_NE(0, port);
	set_udp_port((int)port);
	init_api(9821, (int)port);
	connect_api();
	login_api();

	/* 未注册进 BdMgr 的合约：orderInsert 仍可通过（entrust 自带 ContractInfo），
	 * 但 match_once 按 fullcode 反查 BdMgr 时会拿到 NULL。 */
	EXPECT_EQ(nullptr, sink_.bd_.getContract("zz2512", "SHFE"));
	WTSContractInfo* zz_ct = make_unregistered_ct("zz", "zz2512", CC_Future, CM_OpenCover);
	WTSEntrust* entrust = r103m::make_entrust("zz2512", "SHFE", zz_ct, 7000.0, 1.0,
		WDT_LONG, WOT_OPEN);
	const uint32_t before = sink_.n_push_order();
	ASSERT_EQ(0, api_.orderInsert(entrust));
	entrust->release();
	ASSERT_TRUE(r113m::wait_until([this, before]() {
		return sink_.n_push_order() >= before + 1u;
	}));
	const std::string unregistered_order_id = sink_.last_order_id();
	ASSERT_FALSE(unregistered_order_id.empty());

	/* 注册合约上的有效订单作为正控：证明本轮 match_once 确实完成一次撮合。 */
	insert_order(5.0, 7000.0, WDT_LONG, WOT_OPEN);
	const std::string registered_order_id = sink_.last_order_id();
	ASSERT_FALSE(registered_order_id.empty());
	ASSERT_NE(unregistered_order_id, registered_order_id);

	/* ag2512 行情只应成交 ag2512 委托；zz2512 的委托不得借另一个合约的行情成交。 */
	send_tick(7000.0, 7000.0, 5.0, 7000.0, 5.0);
	ASSERT_TRUE(r113m::wait_until([this]() {
		return sink_.total_trade_volume() >= 5.0;
	})) << "ag2512 正控未成交，无法检查跨合约委托隔离";
	/* 完整撮合处理屏障：成交回调在 match_once 的 _awaits 遍历中同步发出；
	 * release() join 撮合线程，确保该次 ag tick 的同一轮订单遍历已结束后再快照。 */
	api_.release();

	const std::vector<r113m::TradeSnap> trades = sink_.trades();
	ASSERT_FALSE(trades.empty());
	double registered_trade_volume = 0.0;
	for (const r113m::TradeSnap& trade : trades)
	{
		EXPECT_EQ("ag2512", trade.code);
		if (trade.ref_order == unregistered_order_id)
			ADD_FAILURE() << "ag2512 行情成交了 zz2512 委托 " << unregistered_order_id;
		else
		{
			EXPECT_EQ(registered_order_id, trade.ref_order);
			registered_trade_volume += trade.volume;
		}
	}
	EXPECT_DOUBLE_EQ(5.0, registered_trade_volume);
}

/* 生产: TraderMocker.cpp:363-365 */
TEST_F(R214MockerBranches, ZeroVolumeOrderIsSkipped)
{
	const uint16_t port = r103m::pick_free_udp_port();
	ASSERT_NE(0, port);
	set_udp_port((int)port);
	init_api(9825, (int)port);
	connect_api();
	login_api();

	/* 零手委托的 VolLeft 为 0；随后加入同合约正量委托作为成交正控。 */
	insert_order(0.0, 7000.0, WDT_LONG, WOT_OPEN);
	const std::string zero_order_id = sink_.last_order_id();
	ASSERT_FALSE(zero_order_id.empty());

	insert_order(5.0, 7000.0, WDT_LONG, WOT_OPEN);
	const std::string live_order_id = sink_.last_order_id();
	ASSERT_FALSE(live_order_id.empty());
	ASSERT_NE(zero_order_id, live_order_id);

	send_tick(7000.0, 7000.0, 5.0, 7000.0, 5.0);
	ASSERT_TRUE(r113m::wait_until([this]() {
		return sink_.total_trade_volume() >= 5.0;
	})) << "正量订单未成交，无法确认零量订单被跳过";
	api_.release();

	const std::vector<r113m::OrderSnap> orders = sink_.orders();
	bool found_zero_order = false;
	for (const r113m::OrderSnap& order : orders)
	{
		if (order.id == zero_order_id)
		{
			found_zero_order = true;
			EXPECT_DOUBLE_EQ(0.0, order.volume);
			EXPECT_DOUBLE_EQ(0.0, order.vol_left);
			EXPECT_NE((int)WOS_AllTraded, order.state);
		}
	}
	EXPECT_TRUE(found_zero_order);

	const std::vector<r113m::TradeSnap> trades = sink_.trades();
	ASSERT_FALSE(trades.empty());
	double live_trade_volume = 0.0;
	for (const r113m::TradeSnap& trade : trades)
	{
		EXPECT_EQ("ag2512", trade.code);
		EXPECT_EQ(live_order_id, trade.ref_order);
		EXPECT_NE(zero_order_id, trade.ref_order);
		live_trade_volume += trade.volume;
	}
	EXPECT_DOUBLE_EQ(5.0, live_trade_volume);
}

/* 生产: TraderMocker.cpp:381-395（382/390/395 三个 continue） */
TEST_F(R214MockerBranches, ZeroBookAndMismatchedLimitDoNotTrade)
{
	const uint16_t port = r103m::pick_free_udp_port();
	ASSERT_NE(0, port);
	set_udp_port((int)port);

	/* newpx=false：卖价优先取 ask_price，而不是最新价，390 才可达。 */
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("span", 5);
	cfg->append("newpx", false);
	cfg->append("mockerid", 9822);
	cfg->append("maxqty", 10.0);
	cfg->append("minqty", 1.0);
	cfg->append("udp_port", (int)port);
	ASSERT_TRUE(api_.init(cfg));
	cfg->release();

	connect_api();
	login_api();

	insert_order(5.0, 6900.0, WDT_LONG, WOT_OPEN);

	/* ask 量 0 -> 381-382 */
	send_tick(7000.0, 7000.0, 0.0, 7000.0, 5.0);
	std::this_thread::sleep_for(std::chrono::milliseconds(40));
	EXPECT_DOUBLE_EQ(0.0, sink_.total_trade_volume());

	/* ask 价 0、量非 0 -> 389-390 */
	send_tick(7000.0, 0.0, 5.0, 7000.0, 5.0);
	std::this_thread::sleep_for(std::chrono::milliseconds(40));
	EXPECT_DOUBLE_EQ(0.0, sink_.total_trade_volume());

	/* 限价 6900 低于卖价 7000 -> 394-395 */
	send_tick(7000.0, 7000.0, 5.0, 7000.0, 5.0);
	std::this_thread::sleep_for(std::chrono::milliseconds(40));
	EXPECT_DOUBLE_EQ(0.0, sink_.total_trade_volume());

	/* 回到可成交价，证明前三次只是被 continue 跳过，委托仍在 _awaits。 */
	send_tick(6900.0, 6900.0, 5.0, 6900.0, 5.0);
	ASSERT_TRUE(r113m::wait_until([this]() {
		return sink_.total_trade_volume() >= 5.0;
	}));
	EXPECT_DOUBLE_EQ(5.0, sink_.total_trade_volume());
}

/* 生产: TraderMocker.cpp:444-447（CM_None 的空持仓处理块） */
TEST_F(R214MockerBranches, CoverNoneTradesWithoutPositionBook)
{
	const uint16_t port = r103m::pick_free_udp_port();
	ASSERT_NE(0, port);
	set_udp_port((int)port);
	init_api(9823, (int)port);
	connect_api();
	login_api();

	/* 注册一个 CM_None 合约：撮合到 444 的分支体为空，仅跳过持仓累加。 */
	WTSCommodityInfo* au_comm = WTSCommodityInfo::create("au", "gold", "SHFE", "TRADING", "CHINA");
	au_comm->setCategory(CC_Future);
	au_comm->setCoverMode(CM_None);
	au_comm->setPriceMode(PM_Both);
	au_comm->setTradingMode(TM_Both);
	au_comm->setPriceTick(1.0);
	au_comm->setVolScale(10);
	WTSContractInfo* au_ct = WTSContractInfo::create("au2512", "au2512", "SHFE", "au");
	sink_.bd_.regis(au_comm, au_ct);

	WTSEntrust* entrust = r103m::make_entrust("au2512", "SHFE", au_ct, 5000.0, 2.0,
		WDT_LONG, WOT_OPEN);
	const uint32_t before = sink_.n_push_order();
	ASSERT_EQ(0, api_.orderInsert(entrust));
	entrust->release();
	ASSERT_TRUE(r113m::wait_until([this, before]() {
		return sink_.n_push_order() >= before + 1u;
	}));

	send_tick(5000.0, 5000.0, 2.0, 5000.0, 2.0, "au2512", "SHFE");
	ASSERT_TRUE(r113m::wait_until([this]() {
		return sink_.total_trade_volume() >= 2.0;
	})) << "CM_None 合约未成交";
	EXPECT_DOUBLE_EQ(2.0, sink_.total_trade_volume());

	/* CM_None 不记持仓：queryPositions 必须返回空列表而不是崩溃。 */
	const uint32_t q = sink_.n_position();
	ASSERT_EQ(0, api_.queryPositions());
	ASSERT_TRUE(r113m::wait_until([this, q]() { return sink_.n_position() >= q + 1u; }));
	EXPECT_TRUE(sink_.last_positions().empty());
}

/* 生产: TraderMocker.cpp:705-715
 * 不调用 connect()，因此没有 io worker 线程；login() 只起撮合线程，
 * disconnect() 的 join 因此是确定性的，且不会留下未停止的 worker。 */
TEST_F(R214MockerBranches, DisconnectJoinsMatchThreadWithoutWorker)
{
	init_api(9824, 0);
	ASSERT_EQ(0, api_.login("", "", ""));
	EXPECT_TRUE(api_.isConnected());

	EXPECT_NO_THROW(api_.disconnect());

	/* 二次 disconnect 命中 _terminated 早退（不重复 join）。 */
	EXPECT_NO_THROW(api_.disconnect());
}
