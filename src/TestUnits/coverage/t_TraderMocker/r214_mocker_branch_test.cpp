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
 * orderInsert/query*）与本机回环 UDP 逐条构造。未注册合约用来命中 ct==NULL；
 * 零手委托用来命中 VolLeft==0；newpx=false 的配置对象由公开 WTSVariant 构造。
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

/* 生产: TraderMocker.cpp:344-346、363-365 */
TEST_F(R214MockerBranches, UnregisteredContractAndZeroVolumeAreSkipped)
{
	const uint16_t port = r103m::pick_free_udp_port();
	ASSERT_NE(0, port);
	set_udp_port((int)port);
	init_api(9821, (int)port);
	connect_api();
	login_api();

	/* 未注册进 BdMgr 的合约：orderInsert 仍可通过（entrust 自带 ContractInfo），
	 * 但 match_once 按 fullcode 反查 BdMgr 时会拿到 NULL。 */
	WTSContractInfo* zz_ct = make_unregistered_ct("zz", "zz2512", CC_Future, CM_OpenCover);
	WTSEntrust* entrust = r103m::make_entrust("zz2512", "SHFE", zz_ct, 7000.0, 1.0,
		WDT_LONG, WOT_OPEN);
	const uint32_t before = sink_.n_push_order();
	ASSERT_EQ(0, api_.orderInsert(entrust));
	entrust->release();
	ASSERT_TRUE(r113m::wait_until([this, before]() {
		return sink_.n_push_order() >= before + 1u;
	}));

	/* 零手委托：VolLeft 恒为 0，撮合循环必须跳过。 */
	insert_order(0.0, 7000.0, WDT_LONG, WOT_OPEN);

	/* ag2512 的 tick 让 _ticks 非空，并令 _codes 同时含 zz2512/ag2512，
	 * 一次 match_once 就会先遍历到两条被跳过的委托。 */
	send_tick(7000.0, 7000.0, 5.0, 7000.0, 5.0);
	std::this_thread::sleep_for(std::chrono::milliseconds(120));

	EXPECT_DOUBLE_EQ(0.0, sink_.total_trade_volume());
	EXPECT_FALSE(sink_.has_state((int)WOS_AllTraded));
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
