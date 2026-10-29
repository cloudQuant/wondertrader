/*
 * lane R114 —— TraderDumper 正向面测试
 *
 * 生产 TU：src/TraderDumper/Dumper.cpp（163 行）、TraderAdapter.cpp（423 行）、
 * Dumper.h（53 行），与既有 dumper_adapter_test.cpp 一起编进
 * CoverageTests_t_TraderDumper。
 *
 * 关键手法：TraderAdapter 的 _trader_api 是私有字段，但 init() 会真实 dlopen
 * 一个交易模块。本文件把 module 指向构建目录里已有的 libTraderMocker.dylib /
 * libTraderCTP.dylib，让 _trader_api 成为真实对象：
 *   - Mocker 只用于 init + 回调转发；从不 connect/login，因此不会起撮合线程，
 *     queryAccount/queryPositions/queryTrades/queryOrders 只向未 run 的 io_context
 *     post，lambda 永不执行；
 *   - CTP 只用于 init + release（m_pUserAPI 为 NULL，release 不触网）。
 * 全部落盘都在 mkdtemp 的临时目录，模块路径只读构建产物，不访问柜台。
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "TraderDumper/Dumper.h"
#include "TraderDumper/TraderAdapter.h"
#include "TraderDumper/WtHelper.h"

#include "Share/ModuleHelper.hpp"

#include "WTSVariant.hpp"
#include "WTSTradeDef.hpp"
#include "WTSContractInfo.hpp"
#include "WTSCollection.hpp"
#include "IBaseDataMgr.h"
#include "WTSBaseDataMgr.h"
#include "WTSCfgLoader.h"

USING_NS_WTP;

extern WTSBaseDataMgr g_bdMgr;
extern wtp::TraderAdapterMgr g_adapterMgr;

// TraderDumper.cpp:6 的进程级 Dumper 单例
Dumper& getDumper();

namespace {

struct Captured
{
	int accountCalls = 0;
	int orderCalls = 0;
	int tradeCalls = 0;
	int positionCalls = 0;
	char channelid[64] = {0};
	char currency[32] = {0};
	char exchg[32] = {0};
	char code[32] = {0};
	char orderid[32] = {0};
	char tradeid[32] = {0};
	char statemsg[64] = {0};
	uint32_t curTDate = 0;
	uint32_t direct = 0;
	uint32_t offset = 0;
	uint32_t volscale = 0;
	uint32_t state = 0;
	double volume = 0;
	double leftover = 0;
	double traded = 0;
	double price = 0;
	double amount = 0;
	double newvol = 0;
	double cost = 0;
	double avgpx = 0;
	double dynprofit = 0;
	bool lastFlag = false;

	void reset() { *this = Captured(); }
};

Captured g_cap;

void PORTER_FLAG onAccCb(const char* channelid, WtUInt32 curTDate, const char* currency, double prebalance,
	double balance, double dynbalance, double closeprofit, double dynprofit, double fee, double margin,
	double deposit, double withdraw, double available, bool isLast)
{
	++g_cap.accountCalls;
	strncpy(g_cap.channelid, channelid, sizeof(g_cap.channelid) - 1);
	strncpy(g_cap.currency, currency, sizeof(g_cap.currency) - 1);
	g_cap.curTDate = curTDate;
	g_cap.lastFlag = isLast;
	(void)prebalance; (void)balance; (void)dynbalance; (void)closeprofit; (void)dynprofit;
	(void)fee; (void)margin; (void)deposit; (void)withdraw; (void)available;
}

void PORTER_FLAG onOrdCb(const char* channelid, const char* exchg, const char* code, WtUInt32 curTDate,
	const char* orderid, WtUInt32 direct, WtUInt32 offset, double volume, double leftover, double traded,
	double price, WtUInt32 ordertype, WtUInt32 pricetype, WtUInt64 ordertime, WtUInt32 state,
	const char* statemsg, bool isLast)
{
	++g_cap.orderCalls;
	strncpy(g_cap.channelid, channelid, sizeof(g_cap.channelid) - 1);
	strncpy(g_cap.exchg, exchg, sizeof(g_cap.exchg) - 1);
	strncpy(g_cap.code, code, sizeof(g_cap.code) - 1);
	strncpy(g_cap.orderid, orderid, sizeof(g_cap.orderid) - 1);
	strncpy(g_cap.statemsg, statemsg, sizeof(g_cap.statemsg) - 1);
	g_cap.curTDate = curTDate;
	g_cap.direct = direct;
	g_cap.offset = offset;
	g_cap.volume = volume;
	g_cap.leftover = leftover;
	g_cap.traded = traded;
	g_cap.price = price;
	g_cap.state = state;
	g_cap.lastFlag = isLast;
	(void)ordertype; (void)pricetype; (void)ordertime;
}

void PORTER_FLAG onTrdCb(const char* channelid, const char* exchg, const char* code, WtUInt32 curTDate,
	const char* tradeid, const char* orderid, WtUInt32 direct, WtUInt32 offset, double volume, double price,
	double amount, WtUInt32 ordertype, WtUInt32 tradetype, WtUInt64 tradetime, bool isLast)
{
	++g_cap.tradeCalls;
	strncpy(g_cap.channelid, channelid, sizeof(g_cap.channelid) - 1);
	strncpy(g_cap.exchg, exchg, sizeof(g_cap.exchg) - 1);
	strncpy(g_cap.code, code, sizeof(g_cap.code) - 1);
	strncpy(g_cap.tradeid, tradeid, sizeof(g_cap.tradeid) - 1);
	strncpy(g_cap.orderid, orderid, sizeof(g_cap.orderid) - 1);
	g_cap.curTDate = curTDate;
	g_cap.direct = direct;
	g_cap.offset = offset;
	g_cap.volume = volume;
	g_cap.price = price;
	g_cap.amount = amount;
	g_cap.lastFlag = isLast;
	(void)ordertype; (void)tradetype; (void)tradetime;
}

void PORTER_FLAG onPosCb(const char* channelid, const char* exchg, const char* code, WtUInt32 curTDate,
	WtUInt32 direct, double volume, double newvol, double cost, double margin, double avgpx,
	double dynprofit, WtUInt32 volscale, bool isLast)
{
	++g_cap.positionCalls;
	strncpy(g_cap.channelid, channelid, sizeof(g_cap.channelid) - 1);
	strncpy(g_cap.exchg, exchg, sizeof(g_cap.exchg) - 1);
	strncpy(g_cap.code, code, sizeof(g_cap.code) - 1);
	g_cap.curTDate = curTDate;
	g_cap.direct = direct;
	g_cap.volume = volume;
	g_cap.newvol = newvol;
	g_cap.cost = cost;
	g_cap.avgpx = avgpx;
	g_cap.dynprofit = dynprofit;
	g_cap.volscale = volscale;
	g_cap.lastFlag = isLast;
	(void)margin;
}

class FakeDataMgr : public IBaseDataMgr
{
public:
	WTSContractInfo* ct = nullptr;

	WTSCommodityInfo* getCommodity(const char*) override { return nullptr; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return nullptr; }
	WTSContractInfo* getContract(const char* code, const char* = "", uint32_t = 0) override
	{
		if (ct == nullptr || code == nullptr)
			return nullptr;
		return strcmp(code, ct->getCode()) == 0 ? ct : nullptr;
	}
	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return nullptr; }
	WTSSessionInfo* getSession(const char*) override { return nullptr; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return nullptr; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t, uint32_t, bool = false) override { return 0; }
	uint64_t getBoundaryTime(const char*, uint32_t, bool = false, bool = true) override { return 0; }
};

std::string find_module_dir(const char* dylib_name)
{
	const std::string bin = getBinDir();
	const char* subdirs[] = {"../WtPorter/", "../WtRunner/", "../WtUftRunner/", "../"};
	for (const char* sub : subdirs)
	{
		std::string dir = bin + sub;
		std::string mod = dir + "traders/" + dylib_name;
		if (access(mod.c_str(), F_OK) == 0)
			return dir;
	}
	return std::string();
}

class R114DumperFixture : public ::testing::Test
{
protected:
	char _old_cwd[1024];
	std::string _dir;

	void SetUp() override
	{
		ASSERT_NE(getcwd(_old_cwd, sizeof(_old_cwd)), nullptr);
		char tmpl[] = "/tmp/r114_dumper_XXXXXX";
		char* made = mkdtemp(tmpl);
		ASSERT_NE(made, nullptr);
		_dir = made;
		ASSERT_EQ(chdir(_dir.c_str()), 0);

		g_cap.reset();
		getDumper().register_callbacks(onAccCb, onOrdCb, onTrdCb, onPosCb);
	}

	void TearDown() override
	{
		chdir(_old_cwd);
		g_adapterMgr.release();
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
		g_cap.reset();
	}

	std::string write(const char* name, const std::string& content)
	{
		std::string path = _dir + "/" + name;
		std::ofstream f(path);
		f << content;
		f.close();
		return path;
	}
};

TraderAdapterPtr makeFailedAdapter(TraderAdapterMgr* mgr, FakeDataMgr* bd, const char* id)
{
	TraderAdapterPtr adapter(new TraderAdapter(mgr));
	WTSVariant* params = WTSCfgLoader::load_from_content(
		std::string("{\"module\":\"TraderNotBuiltForR114\"}"), false);
	adapter->init(id, params, bd);
	params->release();
	return adapter;
}

}	// namespace

/* ------------------------------------------------------------------ */
/* TraderAdapter.cpp:52-114、158-321 —— mocker 模块的正向回调链          */
/* ------------------------------------------------------------------ */
TEST_F(R114DumperFixture, mocker_adapter_forwards_account_position_trade_and_order_push_records)
{
	const std::string mod_dir = find_module_dir("libTraderMocker.dylib");
	if (mod_dir.empty())
		GTEST_SKIP() << "libTraderMocker.dylib 不在构建目录，跳过（不访问厂商/柜台）";
	WtHelper::set_module_dir(mod_dir.c_str());

	WTSCommodityInfo* comm = WTSCommodityInfo::create("ru", "Rubber", "SHFE", "SHFE_day", "SHFE", "CNY");
	comm->setVolScale(10);
	WTSContractInfo* ct = WTSContractInfo::create("ru2601", "Rubber2601", "SHFE", "ru");
	ct->setCommInfo(comm);

	FakeDataMgr bd;
	bd.ct = ct;
	TraderAdapterMgr mgr;
	mgr.run();		// 初始化空 manager；此用例中的 adapter 不在 manager 表内。
	TraderAdapterPtr ad(new TraderAdapter(&mgr));
	WTSVariant* params = WTSCfgLoader::load_from_content(
		std::string("{\"module\":\"TraderMocker\",\"mockerid\":114}"), false);
	ASSERT_TRUE(ad->init("mocker114", params, &bd));
	params->release();

	// :158-173 登录成功 -> queryPositions（mocker 只 post，不执行 lambda）
	ad->onLoginResult(true, "", 20261028);
	// :180-194 两个查询入口，_date 非 0
	ad->queryFund();
	ad->queryPosition();

	// :296-321 持仓：已知多/空各一条 + 未知合约一条被 continue
	WTSPositionItem* longPos = WTSPositionItem::create("ru2601", "CNY", "SHFE");
	longPos->setDirection(WDT_LONG);
	longPos->setPrePosition(6);
	longPos->setNewPosition(4);
	longPos->setPositionCost(80000);
	longPos->setMargin(5000);
	longPos->setAvgPrice(15000);
	longPos->setDynProfit(200);
	longPos->setContractInfo(ct);
	WTSPositionItem* shortPos = WTSPositionItem::create("ru2601", "CNY", "SHFE");
	shortPos->setDirection(WDT_SHORT);
	shortPos->setPrePosition(2);
	shortPos->setNewPosition(1);
	shortPos->setContractInfo(ct);
	WTSPositionItem* unknownPos = WTSPositionItem::create("unknown", "CNY", "SHFE");
	unknownPos->setDirection(WDT_LONG);
	WTSArray* ayPos = WTSArray::create();
	ayPos->append(unknownPos, true);
	ayPos->append(shortPos, true);
	ayPos->append(longPos, true);
	ad->onRspPosition(ayPos);
	ASSERT_EQ(g_cap.positionCalls, 2);
	EXPECT_EQ((uint32_t)0, g_cap.direct);
	EXPECT_DOUBLE_EQ(10.0, g_cap.volume);
	EXPECT_DOUBLE_EQ(4.0, g_cap.newvol);
	EXPECT_DOUBLE_EQ(10u, g_cap.volscale);
	EXPECT_TRUE(g_cap.lastFlag);
	ayPos->release();
	longPos->release();
	shortPos->release();
	unknownPos->release();

	// :196-213 资金：两条，最后一条 isLast=true，随后 queryTrades
	WTSAccountInfo* acc1 = WTSAccountInfo::create();
	acc1->setCurrency("CNY");
	acc1->setPreBalance(100000);
	acc1->setBalance(101000);
	acc1->setDynProfit(300);
	acc1->setAvailable(80000);
	WTSAccountInfo* acc2 = WTSAccountInfo::create();
	acc2->setCurrency("USD");
	acc2->setPreBalance(1);
	acc2->setBalance(2);
	WTSArray* ayAcc = WTSArray::create();
	ayAcc->append(acc1, true);
	ayAcc->append(acc2, true);
	ad->onRspAccount(ayAcc);
	ASSERT_EQ(g_cap.accountCalls, 2);
	EXPECT_STREQ("USD", g_cap.currency);
	EXPECT_TRUE(g_cap.lastFlag);
	ayAcc->release();
	acc1->release();
	acc2->release();

	// :215-226 推送成交
	WTSTradeInfo* pushTrade = WTSTradeInfo::create("ru2601", "SHFE");
	pushTrade->setTradeID("PUSH-T1");
	pushTrade->setRefOrder("PUSH-O1");
	pushTrade->setVolume(2);
	pushTrade->setPrice(15100);
	pushTrade->setAmount(302000);
	pushTrade->setDirection(WDT_LONG);
	pushTrade->setOffsetType(WOT_OPEN);
	pushTrade->setContractInfo(ct);
	ad->onPushTrade(pushTrade);
	ASSERT_EQ(g_cap.tradeCalls, 1);
	EXPECT_STREQ("PUSH-T1", g_cap.tradeid);
	pushTrade->release();

	// :228-250 成交查询：第一条带合约，第二条靠 bd 回查，第三条未知被 continue
	WTSTradeInfo* trd1 = WTSTradeInfo::create("ru2601", "SHFE");
	trd1->setTradeID("T1");
	trd1->setRefOrder("O1");
	trd1->setVolume(1);
	trd1->setPrice(15000);
	trd1->setAmount(150000);
	trd1->setContractInfo(ct);
	WTSTradeInfo* trd2 = WTSTradeInfo::create("ru2601", "SHFE");
	trd2->setTradeID("T2");
	trd2->setRefOrder("O2");
	trd2->setVolume(2);
	trd2->setPrice(15100);
	trd2->setAmount(302000);
	WTSTradeInfo* trd3 = WTSTradeInfo::create("unknown", "SHFE");
	WTSArray* ayTrades = WTSArray::create();
	ayTrades->append(trd1, true);
	ayTrades->append(trd3, true);
	ayTrades->append(trd2, true);
	ad->onRspTrades(ayTrades);
	ASSERT_EQ(g_cap.tradeCalls, 3);
	EXPECT_STREQ("T2", g_cap.tradeid);
	EXPECT_TRUE(g_cap.lastFlag);
	ayTrades->release();
	trd1->release();
	trd2->release();
	trd3->release();

	// :276-294 推送委托：活状态只落盘，死状态额外查资金/持仓
	WTSOrderInfo* liveOrder = WTSOrderInfo::create();
	liveOrder->setCode("ru2601");
	liveOrder->setExchange("SHFE");
	liveOrder->setOrderID("LIVE-1");
	liveOrder->setDirection(WDT_LONG);
	liveOrder->setOffsetType(WOT_OPEN);
	liveOrder->setVolume(5);
	liveOrder->setVolLeft(5);
	liveOrder->setPrice(14900);
	liveOrder->setPriceType(WPT_LIMITPRICE);
	liveOrder->setOrderState(WOS_NotTraded_Queuing);
	liveOrder->setStateMsg("queuing");
	liveOrder->setContractInfo(ct);
	ad->onPushOrder(liveOrder);
	ASSERT_EQ(g_cap.orderCalls, 1);
	EXPECT_STREQ("LIVE-1", g_cap.orderid);

	WTSOrderInfo* deadOrder = WTSOrderInfo::create();
	deadOrder->setCode("ru2601");
	deadOrder->setExchange("SHFE");
	deadOrder->setOrderID("DEAD-1");
	deadOrder->setDirection(WDT_LONG);
	deadOrder->setOffsetType(WOT_OPEN);
	deadOrder->setVolume(5);
	deadOrder->setVolLeft(0);
	deadOrder->setVolTraded(5);
	deadOrder->setPrice(14900);
	deadOrder->setPriceType(WPT_LIMITPRICE);
	deadOrder->setOrderState(WOS_AllTraded);
	deadOrder->setStateMsg("filled");
	deadOrder->setContractInfo(ct);
	ad->onPushOrder(deadOrder);
	ASSERT_EQ(g_cap.orderCalls, 2);
	EXPECT_STREQ("DEAD-1", g_cap.orderid);

	// Do not call onRspOrders on this unregistered adapter: that terminal callback
	// decrements mgr._live_cnt even though run() initialized it from an empty table.
	// The manager completion path is covered separately with registered adapters below.

	// 不能在 mocker 上调用 release()：其 registerSpi(NULL) 会解引用空 listener
	// （BUG-T6 同源）。adapter 析构不释放 api，本用例只验证回调面。
	ct->release();
	comm->release();
}

/* ------------------------------------------------------------------ */
/* TraderAdapter.cpp:127-134 —— 非空 _trader_api 的 release 分支        */
/* ------------------------------------------------------------------ */
TEST_F(R114DumperFixture, adapter_release_with_real_ctp_module_is_safe)
{
	const std::string mod_dir = find_module_dir("libTraderCTP.dylib");
	if (mod_dir.empty())
		GTEST_SKIP() << "libTraderCTP.dylib 不在构建目录，跳过（不访问厂商/柜台）";
	WtHelper::set_module_dir(mod_dir.c_str());

	FakeDataMgr bd;
	TraderAdapterMgr mgr;
	TraderAdapter ad(&mgr);
	WTSVariant* params = WTSCfgLoader::load_from_content(
		std::string("{\"module\":\"TraderCTP\",\"flowdir\":\"") + _dir + "/ctpflow/\"}", false);
	ASSERT_TRUE(ad.init("ctp_release", params, &bd));
	params->release();

	// :137-151 连接成功事件 -> login()；TraderCTP 的 m_pUserAPI 仍为 NULL，
	// login 在 :415-421 早退，不会建线程/连前置。
	ad.handleEvent(WTE_Connect, 0);

	// TraderCTP::init 在 macOS 只保存 creator 地址，connect 前 m_pUserAPI 为 NULL；
	// release 走 :129-133 真分支（registerSpi(NULL) 有判空，release 只清空数组）。
	ad.release();
	ad.release();
	EXPECT_STREQ("ctp_release", ad.id());
}

/* ------------------------------------------------------------------ */
/* TraderAdapter.cpp:344-423 —— mgr 的 decAlive 嵌套与 refresh          */
/* ------------------------------------------------------------------ */
TEST_F(R114DumperFixture, adapter_mgr_dec_alive_reports_undone_and_refresh_queries_done)
{
	FakeDataMgr bd;
	TraderAdapterMgr mgr;
	mgr.run();
	EXPECT_FALSE(mgr.isAnyAlive());
	EXPECT_EQ(0u, mgr.size());

	// 3 个 init 失败的适配器：run() 只返回 false，不起线程
	TraderAdapterPtr a1 = makeFailedAdapter(&mgr, &bd, "dec1");
	TraderAdapterPtr a2 = makeFailedAdapter(&mgr, &bd, "dec2");
	TraderAdapterPtr a3 = makeFailedAdapter(&mgr, &bd, "dec3");
	EXPECT_TRUE(mgr.addAdapter("dec1", a1));
	EXPECT_TRUE(mgr.addAdapter("dec2", a2));
	EXPECT_TRUE(mgr.addAdapter("dec3", a3));
	mgr.run();
	EXPECT_TRUE(mgr.isAnyAlive());
	// _live_cnt=3 -> left=2，命中 :400-407 的「仍有未完成通道」日志分支
	mgr.decAlive();
	mgr.decAlive();
	mgr.decAlive();
	EXPECT_FALSE(mgr.isAnyAlive());
	mgr.release();
	EXPECT_EQ(0u, mgr.size());

	const std::string mod_dir = find_module_dir("libTraderMocker.dylib");
	if (mod_dir.empty())
		GTEST_SKIP() << "libTraderMocker.dylib 不在构建目录，跳过 refresh 正向分支";
	WtHelper::set_module_dir(mod_dir.c_str());

	WTSCommodityInfo* comm = WTSCommodityInfo::create("ru", "Rubber", "SHFE", "SHFE_day", "SHFE", "CNY");
	comm->setVolScale(10);
	WTSContractInfo* ct = WTSContractInfo::create("ru2601", "Rubber2601", "SHFE", "ru");
	ct->setCommInfo(comm);
	FakeDataMgr bd2;
	bd2.ct = ct;

	TraderAdapterMgr mgr2;
	mgr2.run();		// 空表，只为初始化 _live_cnt
	TraderAdapterPtr ad(new TraderAdapter(&mgr2));
	WTSVariant* params = WTSCfgLoader::load_from_content(
		std::string("{\"module\":\"TraderMocker\",\"mockerid\":115}"), false);
	ASSERT_TRUE(ad->init("mocker115", params, &bd2));
	params->release();
	ad->onLoginResult(true, "", 20261029);
	WTSArray* empty = WTSArray::create();
	ad->onRspOrders(empty);		// _done=true
	empty->release();
	ASSERT_TRUE(ad->isDone());

	// :413-423：done 的通道走 queryPosition/queryFund；mocker 只 post
	EXPECT_TRUE(mgr2.addAdapter("mocker115", ad));
	mgr2.refresh();
	ct->release();
	comm->release();
	// 不调用 mgr2.release()：mocker 的 registerSpi(NULL) 空指针（同 BUG-T3-010 家族）
}

/* ------------------------------------------------------------------ */
/* Dumper.cpp:22-100 —— commodity/contract 数组与无 session 分支        */
/* ------------------------------------------------------------------ */
TEST_F(R114DumperFixture, dumper_config_accepts_array_basefiles_and_empty_traders)
{
	g_adapterMgr.release();
	const std::string cfg = write("array_cfg.json", R"({
		"config": {},
		"basefiles": {
			"commodity": ["no_such_commodity_a.json", "no_such_commodity_b.json"],
			"contract": ["no_such_contract_a.json", "no_such_contract_b.json"]
		},
		"traders": []
	})");

	Dumper d;
	ASSERT_TRUE(d.config(cfg.c_str(), true, _dir.c_str()));
	EXPECT_EQ(0u, g_adapterMgr.size());
}

/* ------------------------------------------------------------------ */
/* Dumper.cpp:102-135 —— run(true) 空表立即返回；run(false) 线程可 join */
/* ------------------------------------------------------------------ */
TEST_F(R114DumperFixture, dumper_run_once_returns_immediately_and_worker_joins)
{
	g_adapterMgr.release();
	Dumper once;
	once.run(true);		// _adapters 为空 -> isAnyAlive()=false -> 立刻 break
	EXPECT_EQ(0u, g_adapterMgr.size());

	const std::string cfg = write("run_cfg.json", R"({
		"config": {"refresh_span": 0},
		"basefiles": {"session": "no_such_session_r114.json"},
		"traders": []
	})");
	Dumper worker;
	ASSERT_TRUE(worker.config(cfg.c_str(), true, _dir.c_str()));
	worker.run(false);	// 唯一线程是一个 sleep(0) 的 refresh 循环，紧接着 release() join
	worker.release();
	EXPECT_EQ(0u, g_adapterMgr.size());
}
