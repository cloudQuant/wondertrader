/*!
 * \file dumper_trader_adapter_test.cpp
 * \project WonderTrader — 覆盖率补写第六波（收发族）lane T3 / TraderDumper
 *
 * 覆盖：Dumper 的回调登记/记录转发（on_account/on_order/on_trade/on_position）、
 *       配置加载分支（Dumper::config）、TraderAdapter 的失败/未就绪和日期保护分支、
 *       TraderAdapterMgr 的注册/查重/刷新/释放。
 *
 * 安全约束：
 *   - 全部落盘动作只允许发生在临时目录：用例内 chdir 到 mkdtemp 目录；
 *   - 不连柜台：模块加载失败用例使用不存在的交易模块；日期保护用例注入只计数查询的假 API；
 *   - 不起线程：Dumper::run()（bOnce 为 for(;;) 死循环、否则起 worker 线程）整条 DISABLED；
 *   - 注入的假 API 不起线程、不访问外部环境；其余空 API 回调测试在调用查询前将 _done 置真。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <cstdio>
#include <string>
#include <fstream>
#include <memory>
#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#include <atomic>
#include <unordered_map>
#include <boost/noncopyable.hpp>

#include "ExecuteDefs.h"
#include "ITraderApi.h"

#define private public
#include "TraderDumper/TraderAdapter.h"
#undef private

#include "TraderDumper/Dumper.h"
#include "TraderDumper/WtHelper.h"
#include "TraderDumper/TraderDumper.h"

#include "WTSVariant.hpp"
#include "WTSTradeDef.hpp"
#include "WTSContractInfo.hpp"
#include "WTSCollection.hpp"
#include "IBaseDataMgr.h"
#include "ITraderApi.h"
#include "WTSBaseDataMgr.h"
#include "WTSCfgLoader.h"

USING_NS_WTP;

// Dumper.cpp 里的两个全局单例（同一二进制内可直接观察）
extern WTSBaseDataMgr g_bdMgr;
extern wtp::TraderAdapterMgr g_adapterMgr;

// TraderDumper.cpp:6 的进程级 Dumper 单例
Dumper& getDumper();

namespace {

//////////////////////////////////////////////////////////////////////////
// 记录回调捕获（C 回调 -> 全局量）
//////////////////////////////////////////////////////////////////////////
struct Captured
{
	int accountCalls, orderCalls, tradeCalls, positionCalls;
	char channelid[64];
	char currency[32];
	char code[64];
	char exchg[64];
	char orderid[64];
	char tradeid[64];
	char statemsg[64];
	uint32_t curTDate, direct, offset, ordertype, pricetype, state, volscale, tradetype;
	double prebalance, balance, dynbalance, closeprofit, dynprofit, fee, margin, deposit, withdraw, available;
	double volume, leftover, traded, price, amount, newvol, cost, avgpx;
	uint64_t ordertime, tradetime;
	bool lastFlag;

	void reset()
	{
		memset(this, 0, sizeof(*this));
	}
};
Captured g_cap;

void PORTER_FLAG onAccCb(const char* channelid, WtUInt32 curTDate, const char* currency, double prebalance, double balance,
	double dynbalance, double closeprofit, double dynprofit, double fee, double margin, double deposit, double withdraw,
	double available, bool isLast)
{
	++g_cap.accountCalls;
	strncpy(g_cap.channelid, channelid, sizeof(g_cap.channelid) - 1);
	strncpy(g_cap.currency, currency, sizeof(g_cap.currency) - 1);
	g_cap.curTDate = curTDate;
	g_cap.prebalance = prebalance; g_cap.balance = balance; g_cap.dynbalance = dynbalance;
	g_cap.closeprofit = closeprofit; g_cap.dynprofit = dynprofit; g_cap.fee = fee;
	g_cap.margin = margin; g_cap.deposit = deposit; g_cap.withdraw = withdraw;
	g_cap.available = available; g_cap.lastFlag = isLast;
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
	g_cap.curTDate = curTDate; g_cap.direct = direct; g_cap.offset = offset;
	g_cap.volume = volume; g_cap.leftover = leftover; g_cap.traded = traded;
	g_cap.price = price; g_cap.ordertype = ordertype; g_cap.pricetype = pricetype;
	g_cap.ordertime = ordertime; g_cap.state = state; g_cap.lastFlag = isLast;
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
	g_cap.curTDate = curTDate; g_cap.direct = direct; g_cap.offset = offset;
	g_cap.volume = volume; g_cap.price = price; g_cap.amount = amount;
	g_cap.ordertype = ordertype; g_cap.tradetype = tradetype;
	g_cap.tradetime = tradetime; g_cap.lastFlag = isLast;
}

void PORTER_FLAG onPosCb(const char* channelid, const char* exchg, const char* code, WtUInt32 curTDate,
	WtUInt32 direct, double volume, double newvol, double cost, double margin, double avgpx,
	double dynprofit, WtUInt32 volscale, bool isLast)
{
	++g_cap.positionCalls;
	strncpy(g_cap.channelid, channelid, sizeof(g_cap.channelid) - 1);
	strncpy(g_cap.exchg, exchg, sizeof(g_cap.exchg) - 1);
	strncpy(g_cap.code, code, sizeof(g_cap.code) - 1);
	g_cap.curTDate = curTDate; g_cap.direct = direct; g_cap.volume = volume;
	g_cap.newvol = newvol; g_cap.cost = cost; g_cap.margin = margin;
	g_cap.avgpx = avgpx; g_cap.dynprofit = dynprofit; g_cap.volscale = volscale;
	g_cap.lastFlag = isLast;
}

//////////////////////////////////////////////////////////////////////////
// 假 bdMgr：只认 ru2601
//////////////////////////////////////////////////////////////////////////
class FakeDataMgr : public IBaseDataMgr
{
public:
	WTSContractInfo* ct;
	FakeDataMgr() : ct(NULL) {}
	virtual WTSCommodityInfo* getCommodity(const char* /*exchgpid*/) override { return NULL; }
	virtual WTSCommodityInfo* getCommodity(const char* /*exchg*/, const char* /*pid*/) override { return NULL; }
	virtual WTSContractInfo* getContract(const char* code, const char* /*exchg*/ = "", uint32_t /*uDate*/ = 0) override
	{
		if (ct == NULL || code == NULL)
			return NULL;
		return strcmp(code, ct->getCode()) == 0 ? ct : NULL;
	}
	virtual WTSArray* getContracts(const char* /*exchg*/ = "", uint32_t /*uDate*/ = 0) override { return NULL; }
	virtual WTSSessionInfo* getSession(const char* /*sid*/) override { return NULL; }
	virtual WTSSessionInfo* getSessionByCode(const char* /*code*/, const char* /*exchg*/ = "") override { return NULL; }
	virtual WTSArray* getAllSessions() override { return NULL; }
	virtual bool isHoliday(const char* /*pid*/, uint32_t /*uDate*/, bool /*isTpl*/ = false) override { return false; }
	virtual uint32_t calcTradingDate(const char* /*stdPID*/, uint32_t /*uDate*/, uint32_t /*uTime*/, bool /*isSession*/ = false) override { return 0; }
	virtual uint64_t getBoundaryTime(const char* /*stdPID*/, uint32_t /*tDate*/, bool /*isSession*/ = false, bool /*isStart*/ = true) override { return 0; }
};

class FakeTraderApi : public ITraderApi
{
public:
	int queryAccount() override
	{
		++account_queries;
		return 0;
	}

	int queryPositions() override
	{
		++position_queries;
		return 0;
	}

	int account_queries = 0;
	int position_queries = 0;
};

//////////////////////////////////////////////////////////////////////////
// 临时目录夹具：所有落盘都留在 /tmp 里
//////////////////////////////////////////////////////////////////////////
class TempCwd : public ::testing::Test
{
public:
	std::string dir;
	char oldCwd[512];

	void SetUp() override
	{
		g_cap.reset();
		if (getcwd(oldCwd, sizeof(oldCwd)) == NULL)
			oldCwd[0] = 0;
		char tmpl[128];
		snprintf(tmpl, sizeof(tmpl), "/tmp/wt_dumper_XXXXXX");
		const char* made = mkdtemp(tmpl);
		ASSERT_TRUE(made != NULL);
		dir = made;
		ASSERT_EQ(0, chdir(dir.c_str()));
	}

	void TearDown() override
	{
		if (oldCwd[0] != 0)
			chdir(oldCwd);
	}

	void writeFile(const char* name, const char* content)
	{
		std::ofstream f(name);
		f << content;
		f.close();
	}
};

const char* kCfgOk = R"({
	"config": {"refresh_span": 3},
	"basefiles": {"session": "no_such_session.json", "commodity": "no_such_comm.json", "contract": "no_such_contract.json"},
	"traders": [
		{"channelid": "dump_missing", "active": true, "module": "TraderNotBuiltForTest", "user": "u", "pass": "p"},
		{"channelid": "", "active": true, "module": "TraderNotBuiltForTest"},
		{"channelid": "dump_inactive", "active": false, "module": "TraderNotBuiltForTest"}
	]
})";

const char* kCfgNoModule = R"({
	"config": {},
	"basefiles": {"session": "no_such_session.json"},
	"traders": [
		{"channelid": "dump_empty_module", "active": true, "module": ""}
	]
})";

TraderAdapterPtr makeFailedAdapter(TraderAdapterMgr* mgr, FakeDataMgr* bd, const char* id)
{
	TraderAdapterPtr adapter(new TraderAdapter(mgr));
	WTSVariant* params = WTSCfgLoader::load_from_content(std::string("{\"module\":\"TraderNotBuiltForTest\"}"), false);
	adapter->init(id, params, bd);
	params->release();
	return adapter;
}

} // namespace

//////////////////////////////////////////////////////////////////////////
// Dumper：回调登记与记录转发
//////////////////////////////////////////////////////////////////////////
TEST(dumper, forwards_account_record_to_registered_callback)
{
	Dumper d;
	d.register_callbacks(onAccCb, onOrdCb, onTrdCb, onPosCb);

	d.on_account("ch1", 20260105, "CNY", 100000.0, 101000.0, 101300.0, 500.0, 300.0,
		50.0, 20000.0, 1000.0, 200.0, 80000.0, true);

	ASSERT_EQ(1, g_cap.accountCalls);
	EXPECT_STREQ("ch1", g_cap.channelid);
	EXPECT_STREQ("CNY", g_cap.currency);
	EXPECT_EQ(20260105u, g_cap.curTDate);
	EXPECT_DOUBLE_EQ(100000.0, g_cap.prebalance);
	EXPECT_DOUBLE_EQ(101000.0, g_cap.balance);
	EXPECT_DOUBLE_EQ(101300.0, g_cap.dynbalance);
	EXPECT_DOUBLE_EQ(500.0, g_cap.closeprofit);
	EXPECT_DOUBLE_EQ(300.0, g_cap.dynprofit);
	EXPECT_DOUBLE_EQ(50.0, g_cap.fee);
	EXPECT_DOUBLE_EQ(20000.0, g_cap.margin);
	EXPECT_DOUBLE_EQ(1000.0, g_cap.deposit);
	EXPECT_DOUBLE_EQ(200.0, g_cap.withdraw);
	EXPECT_DOUBLE_EQ(80000.0, g_cap.available);
	EXPECT_TRUE(g_cap.lastFlag);
}

TEST(dumper, forwards_order_trade_and_position_records)
{
	Dumper d;
	d.register_callbacks(onAccCb, onOrdCb, onTrdCb, onPosCb);

	d.on_order("ch2", "SHFE", "ru2601", 20260105, "123", 0, 1, 10.0, 6.0, 4.0, 15000.0,
		0, 2, 1735000000000ull, 3, "已报", false);
	ASSERT_EQ(1, g_cap.orderCalls);
	EXPECT_STREQ("ch2", g_cap.channelid);
	EXPECT_STREQ("SHFE", g_cap.exchg);
	EXPECT_STREQ("ru2601", g_cap.code);
	EXPECT_STREQ("123", g_cap.orderid);
	EXPECT_STREQ("已报", g_cap.statemsg);
	EXPECT_EQ(20260105u, g_cap.curTDate);
	EXPECT_DOUBLE_EQ(10.0, g_cap.volume);
	EXPECT_DOUBLE_EQ(6.0, g_cap.leftover);
	EXPECT_DOUBLE_EQ(4.0, g_cap.traded);
	EXPECT_DOUBLE_EQ(15000.0, g_cap.price);
	EXPECT_EQ((uint32_t)3, g_cap.state);
	EXPECT_EQ((uint64_t)1735000000000ull, g_cap.ordertime);
	EXPECT_FALSE(g_cap.lastFlag);

	d.on_trade("ch2", "SHFE", "ru2601", 20260105, "T9", "123", 0, 0, 2.0, 15100.0, 302000.0,
		0, 0, 1735000001000ull, true);
	ASSERT_EQ(1, g_cap.tradeCalls);
	EXPECT_STREQ("T9", g_cap.tradeid);
	EXPECT_DOUBLE_EQ(302000.0, g_cap.amount);
	EXPECT_EQ((uint64_t)1735000001000ull, g_cap.tradetime);
	EXPECT_TRUE(g_cap.lastFlag);

	d.on_position("ch2", "CZCE", "TA601", 20260105, 1, 14.0, 4.0, 80000.0, 5000.0, 571.4, 200.0, 5, true);
	ASSERT_EQ(1, g_cap.positionCalls);
	EXPECT_STREQ("TA601", g_cap.code);
	EXPECT_EQ(1u, g_cap.direct);
	EXPECT_DOUBLE_EQ(14.0, g_cap.volume);
	EXPECT_DOUBLE_EQ(4.0, g_cap.newvol);
	EXPECT_DOUBLE_EQ(571.4, g_cap.avgpx);
	EXPECT_EQ(5u, g_cap.volscale);
}

TEST(dumper, records_are_dropped_when_no_callback_registered)
{
	g_cap.reset();
	Dumper d;
	// 未 register_callbacks：_cb_* 都是 nullptr，转发必须被跳过而不是崩
	EXPECT_NO_THROW(d.on_account("ch", 20260105, "CNY", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, true));
	EXPECT_NO_THROW(d.on_order("ch", "SHFE", "ru2601", 20260105, "1", 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, "x", true));
	EXPECT_NO_THROW(d.on_trade("ch", "SHFE", "ru2601", 20260105, "T", "1", 0, 0, 1, 1, 1, 0, 0, 1, true));
	EXPECT_NO_THROW(d.on_position("ch", "SHFE", "ru2601", 20260105, 0, 1, 1, 1, 1, 1, 1, 10, true));
	EXPECT_EQ(0, g_cap.accountCalls);
	EXPECT_EQ(0, g_cap.orderCalls);
	EXPECT_EQ(0, g_cap.tradeCalls);
	EXPECT_EQ(0, g_cap.positionCalls);
}

TEST_F(TempCwd, dumper_init_with_missing_log_profile_is_a_noop)
{
	Dumper d;
	// WTSLogger::init 对不存在的 propFile 直接 return（WTSLogger.cpp:209）
	EXPECT_NO_THROW(d.init("definitely_no_such_logcfg.json"));
	EXPECT_NO_THROW(d.release());
}

TEST_F(TempCwd, dumper_config_does_not_register_adapters_when_init_fails)
{
	g_adapterMgr.release();
	const size_t before = g_adapterMgr.size();

	Dumper d;
	// isFile=true 分支：走 load_from_file（.json 后缀）
	writeFile("dumper_cfg.json", kCfgOk);
	EXPECT_FALSE(d.config("dumper_cfg.json", true, dir.c_str()));
	// 两条 active adapter 均不能初始化：均不得登记到 manager。
	EXPECT_EQ(before, g_adapterMgr.size());
	EXPECT_TRUE(g_adapterMgr.getAdapter("") == NULL);
	EXPECT_TRUE(g_adapterMgr.getAdapter("dump_missing") == NULL);

	g_adapterMgr.release();
	EXPECT_EQ(0u, g_adapterMgr.size());
}

TEST_F(TempCwd, dumper_config_rejects_active_adapter_with_empty_module)
{
	g_adapterMgr.release();
	const size_t before = g_adapterMgr.size();

	Dumper d;
	// isFile=false 分支：走 load_from_content(JSON)
	EXPECT_FALSE(d.config(kCfgNoModule, false, dir.c_str()));
	EXPECT_EQ(before, g_adapterMgr.size());
	EXPECT_TRUE(g_adapterMgr.getAdapter("dump_empty_module") == NULL);
	g_adapterMgr.release();
}

TEST_F(TempCwd, dumper_config_rejects_invalid_document_and_missing_or_wrong_required_sections)
{
	g_adapterMgr.release();
	Dumper nullConfig;
	EXPECT_FALSE(nullConfig.config(NULL, false, dir.c_str()));
	EXPECT_FALSE(nullConfig.config("{}", false, NULL));
	EXPECT_EQ(0u, g_adapterMgr.size());

	const char* invalidConfigs[] = {
		"{",
		"[]",
		R"({"traders":[]})",
		R"({"basefiles":{}})",
		R"({"basefiles":[],"traders":[]})",
		R"({"basefiles":{},"traders":{}})",
		R"({"basefiles":{},"traders":[1]})"
	};

	for (const char* cfg : invalidConfigs)
	{
		Dumper d;
		EXPECT_FALSE(d.config(cfg, false, dir.c_str())) << cfg;
		EXPECT_EQ(0u, g_adapterMgr.size()) << cfg;
	}

	writeFile("invalid_dumper_cfg.json", "{");
	Dumper fromFile;
	EXPECT_FALSE(fromFile.config("invalid_dumper_cfg.json", true, dir.c_str()));
	EXPECT_EQ(0u, g_adapterMgr.size());
}

TEST_F(TempCwd, dumper_config_keeps_optional_config_and_inactive_adapter_compatibility)
{
	g_adapterMgr.release();
	const char* cfg = R"({"basefiles":{},"traders":[{"active":false}]})";
	Dumper d;
	EXPECT_TRUE(d.config(cfg, false, dir.c_str()));
	EXPECT_EQ(0u, g_adapterMgr.size());
}

TEST_F(TempCwd, dumper_config_rejects_invalid_refresh_span_and_accepts_numeric_yaml_string)
{
	g_adapterMgr.release();
	const char* invalidConfigs[] = {
		R"({"config":{"refresh_span":0},"basefiles":{},"traders":[]})",
		R"({"config":{"refresh_span":-1},"basefiles":{},"traders":[]})",
		R"({"config":{"refresh_span":4294967296},"basefiles":{},"traders":[]})",
		R"({"config":{"refresh_span":1.5},"basefiles":{},"traders":[]})",
		R"({"config":{"refresh_span":"1.5"},"basefiles":{},"traders":[]})",
		R"({"config":{"refresh_span":"invalid"},"basefiles":{},"traders":[]})",
		R"({"config":{"refresh_span":true},"basefiles":{},"traders":[]})"
	};

	for (const char* cfg : invalidConfigs)
	{
		Dumper d;
		EXPECT_FALSE(d.config(cfg, false, dir.c_str())) << cfg;
		EXPECT_EQ(0u, g_adapterMgr.size()) << cfg;
	}

	Dumper numericString;
	EXPECT_TRUE(numericString.config(
		R"({"config":{"refresh_span":"3"},"basefiles":{},"traders":[]})", false, dir.c_str()));
	EXPECT_EQ(0u, g_adapterMgr.size());
}

//////////////////////////////////////////////////////////////////////////
// TraderAdapter：失败/未就绪分支
//////////////////////////////////////////////////////////////////////////
TEST_F(TempCwd, adapter_init_rejects_null_params_and_missing_module)
{
	FakeDataMgr bd;
	TraderAdapterMgr mgr;
	TraderAdapter ad(&mgr);

	// params == NULL（:54）
	EXPECT_FALSE(ad.init("c", NULL, &bd));

	// module 为空（:67）
	WTSVariant* empty = WTSCfgLoader::load_from_content(std::string("{\"module\":\"\"}"), false);
	ASSERT_TRUE(empty != NULL);
	EXPECT_FALSE(ad.init("c", empty, &bd));
	empty->release();

	// 模块加载失败（:81-86）：本机没有编译 TraderXXX 动态库
	WTSVariant* miss = WTSCfgLoader::load_from_content(std::string("{\"module\":\"TraderNotBuiltForTest\"}"), false);
	ASSERT_TRUE(miss != NULL);
	EXPECT_FALSE(ad.init("c", miss, &bd));
	miss->release();

	// 但 id / bdMgr 已在失败前写入 → 可继续驱动回调
	EXPECT_STREQ("c", ad.id());
	EXPECT_EQ(&bd, ad.getBaseDataMgr());
	EXPECT_FALSE(ad.run());
}

TEST_F(TempCwd, adapter_date_defaults_to_zero_and_failed_login_refresh_skips_queries)
{
	FakeTraderApi api;
	TraderAdapterMgr mgr;
	TraderAdapterPtr ad(new TraderAdapter(&mgr));
	ad->_trader_api = &api;

	EXPECT_EQ(0u, ad->_date);
	ASSERT_TRUE(mgr.addAdapter("date_guard", ad));
	mgr.run();

	ad->onLoginResult(false, "bad credentials", 0);
	EXPECT_TRUE(ad->isDone());
	mgr.refresh();
	EXPECT_EQ(0, api.account_queries);
	EXPECT_EQ(0, api.position_queries);

	mgr.release();
}

TEST_F(TempCwd, adapter_successful_login_sets_date_and_preserves_queries)
{
	FakeTraderApi api;
	TraderAdapter ad(nullptr);
	ad._trader_api = &api;

	EXPECT_EQ(0u, ad._date);
	ad.onLoginResult(true, "ok", 20260302);
	EXPECT_EQ(20260302u, ad._date);
	EXPECT_EQ(1, api.position_queries);

	ad.queryPosition();
	ad.queryFund();
	EXPECT_EQ(2, api.position_queries);
	EXPECT_EQ(1, api.account_queries);
}

TEST_F(TempCwd, adapter_close_event_only_logs_and_connect_failure_marks_done)
{
	FakeDataMgr bd;
	TraderAdapterMgr mgr;
	TraderAdapterPtr ad = makeFailedAdapter(&mgr, &bd, "evt");

	// WTE_Close 只写日志（:152）
	ad->handleEvent(WTE_Close, -9);
	EXPECT_FALSE(ad->isDone());

	// WTE_Connect 且 ec!=0 → 记错误 + decAlive + _done=true（:146-150）
	ad->handleEvent(WTE_Connect, 7);
	EXPECT_TRUE(ad->isDone());

	// 其他事件不处理
	ad->handleEvent(WTE_Login, 0);
	EXPECT_TRUE(ad->isDone());

	// _done 已经为真 → 这两个回调不会再碰空的 _trader_api
	WTSArray* ay = WTSArray::create();
	ad->onRspAccount(ay);
	EXPECT_EQ(0, g_cap.accountCalls); // 空数组没有可转发的账户记录
	ad->onRspPosition(ay);
	EXPECT_EQ(0, g_cap.positionCalls); // 空数组没有可转发的持仓记录
	ay->release();

	EXPECT_NO_THROW(ad->onLogout());
	EXPECT_NO_THROW(ad->handleTraderLog(LL_INFO, "raw trader log line"));
}

TEST_F(TempCwd, adapter_error_report_and_null_guard)
{
	FakeDataMgr bd;
	TraderAdapterMgr mgr;
	TraderAdapterPtr ad = makeFailedAdapter(&mgr, &bd, "err");

	WTSError* e = WTSError::create(WEC_ORDERINSERT, "order rejected");
	ad->onTraderError(e);
	e->release();
	// err==NULL 走 if(err) 之外（:325），不得崩
	ad->onTraderError(NULL);
	EXPECT_EQ(0, g_cap.orderCalls);
}

TEST_F(TempCwd, adapter_forwards_orders_trades_and_positions_to_dumper)
{
	FakeDataMgr bd;
	WTSCommodityInfo* comm = WTSCommodityInfo::create("ru", "橡胶", "SHFE", "SHFE_day", "SHFE", "CNY");
	comm->setVolScale(10);
	WTSContractInfo* ct = WTSContractInfo::create("ru2601", "橡胶2601", "SHFE", "ru");
	ct->setCommInfo(comm);
	bd.ct = ct;

	TraderAdapterMgr mgr;
	TraderAdapterPtr ad = makeFailedAdapter(&mgr, &bd, "fwd");
	// 先把 _done 置真，绕开空的 _trader_api
	ad->handleEvent(WTE_Connect, 1);
	g_cap.reset();
	// 本用例由独立进程运行；显式注册回调，不依赖其他测试的单例状态。
	getDumper().register_callbacks(onAccCb, onOrdCb, onTrdCb, onPosCb);
	struct ResetDumperCallbacks
	{
		~ResetDumperCallbacks()
		{
			getDumper().register_callbacks(nullptr, nullptr, nullptr, nullptr);
		}
	} resetDumperCallbacks;

	WTSOrderInfo* o = WTSOrderInfo::create();
	o->setCode("ru2601");
	o->setExchange("SHFE");
	o->setOrderID("8888");
	o->setPrice(15000.0);
	o->setVolume(10.0);
	o->setVolLeft(6.0);
	o->setVolTraded(4.0);
	o->setDirection(WDT_LONG);
	o->setOffsetType(WOT_OPEN);
	o->setPriceType(WPT_LIMITPRICE);
	o->setOrderState(WOS_AllTraded);
	o->setStateMsg("全部成交");
	o->setContractInfo(ct);

	WTSArray* ay = WTSArray::create();
	ay->append(o, true);			// 数组额外持有一份引用，o 由本用例负责最终释放
	ad->onRspOrders(ay);
	ASSERT_EQ(1, g_cap.orderCalls);
	EXPECT_STREQ("fwd", g_cap.channelid);
	EXPECT_STREQ("SHFE", g_cap.exchg);
	EXPECT_STREQ("ru2601", g_cap.code);
	EXPECT_STREQ("8888", g_cap.orderid);
	EXPECT_STREQ("全部成交", g_cap.statemsg);
	EXPECT_DOUBLE_EQ(10.0, g_cap.volume);
	EXPECT_DOUBLE_EQ(6.0, g_cap.leftover);
	EXPECT_DOUBLE_EQ(15000.0, g_cap.price);
	EXPECT_TRUE(g_cap.lastFlag);	// 末笔 idx==size-1
	// onRspOrders 收尾一定把 _done 置真（:272-273）
	EXPECT_TRUE(ad->isDone());
	ay->release();

	// 推送路径：有合约 → 直接转发。状态必须是「活的」，否则 :287 会去解引用空的 _trader_api
	o->setOrderState(WOS_NotTraded_Queuing);
	ad->onPushOrder(o);
	EXPECT_EQ(2, g_cap.orderCalls);
	o->setOrderState(WOS_AllTraded);

	WTSTradeInfo* t = WTSTradeInfo::create("ru2601", "SHFE");
	t->setTradeID("T77");
	t->setRefOrder("8888");
	t->setVolume(2.0);
	t->setPrice(15100.0);
	t->setAmount(302000.0);
	t->setDirection(WDT_SHORT);
	t->setOffsetType(WOT_CLOSE);
	t->setContractInfo(ct);
	ad->onPushTrade(t);
	ASSERT_EQ(1, g_cap.tradeCalls);
	EXPECT_STREQ("T77", g_cap.tradeid);
	EXPECT_DOUBLE_EQ(302000.0, g_cap.amount);
	t->release();

	// 未知合约：查不到 contract → 直接 return，不转发
	WTSContractInfo* keep = bd.ct;
	bd.ct = NULL;
	g_cap.reset();
	WTSOrderInfo* o2 = WTSOrderInfo::create();
	o2->setCode("unknown");
	o2->setExchange("SHFE");
	o2->setOrderState(WOS_Submitting);
	ad->onPushOrder(o2);
	EXPECT_EQ(0, g_cap.orderCalls);
	o2->release();
	bd.ct = keep;

	// 空数组 / NULL 数组：只写日志，_done 已真不会再调 api
	WTSArray* empty = WTSArray::create();
	ad->onRspOrders(empty);
	ad->onRspAccount(NULL);
	empty->release();

	o->release();
	ct->release();
	comm->release();
}

TEST_F(TempCwd, adapter_position_list_without_contract_is_skipped)
{
	FakeDataMgr bd;
	bd.ct = NULL;
	TraderAdapterMgr mgr;
	TraderAdapterPtr ad = makeFailedAdapter(&mgr, &bd, "pos");
	ad->handleEvent(WTE_Connect, 1);
	g_cap.reset();

	WTSPositionItem* p = WTSPositionItem::create("ru2601", "CNY", "SHFE");
	p->setDirection(WDT_LONG);
	p->setNewPosition(4.0);
	p->setPrePosition(6.0);
	p->setMargin(5000.0);
	p->setAvgPrice(15000.0);
	p->setDynProfit(200.0);
	p->setPositionCost(80000.0);
	WTSArray* ay = WTSArray::create();
	ay->append(p, false);
	ad->onRspPosition(ay);
	// 合约查不到 → continue，一条都不落地
	EXPECT_EQ(0, g_cap.positionCalls);
	ay->release();
}

//////////////////////////////////////////////////////////////////////////
// TraderAdapterMgr
//////////////////////////////////////////////////////////////////////////
TEST(adapter_mgr, add_get_and_duplicate_rules)
{
	TraderAdapterMgr mgr;
	FakeDataMgr bd;

	TraderAdapterPtr nullPtr;
	EXPECT_FALSE(mgr.addAdapter("x", nullPtr));			// adapter==NULL（:346）

	TraderAdapterPtr a1(new TraderAdapter(&mgr));
	EXPECT_FALSE(mgr.addAdapter("", a1));				// 名字为空
	EXPECT_TRUE(mgr.addAdapter("chan1", a1));
	EXPECT_EQ(1u, mgr.size());
	ASSERT_TRUE(mgr.getAdapter("chan1") != NULL);
	EXPECT_TRUE(mgr.getAdapter("not_here") == NULL);

	TraderAdapterPtr a2(new TraderAdapter(&mgr));
	EXPECT_FALSE(mgr.addAdapter("chan1", a2));			// 重名（:351）
	EXPECT_EQ(1u, mgr.size());

	// 空 mgr 上的 run/refresh/release 都必须安全
	// 注意：_live_cnt 在 TraderAdapterMgr 里没有构造函数初始化（见 bug 文件），
	// 只有 run() 赋值之后 isAnyAlive() 才是确定的
	TraderAdapterMgr emptyMgr;
	emptyMgr.run();
	EXPECT_FALSE(emptyMgr.isAnyAlive());
	EXPECT_NO_THROW(emptyMgr.refresh());
	EXPECT_NO_THROW(emptyMgr.release());
	EXPECT_EQ(0u, emptyMgr.size());

	mgr.release();
	EXPECT_EQ(0u, mgr.size());
}

TEST_F(TempCwd, extern_c_entries_route_to_the_singleton_dumper)
{
	// TraderDumper.cpp 的 5 个导出入口（不调 run，它要么死循环要么起线程）
	init("no_such_logcfg_for_singleton.json");
	register_callbacks(onAccCb, onOrdCb, onTrdCb, onPosCb);

	getDumper().on_account("capi", 20260105, "CNY", 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5, false);
	ASSERT_EQ(1, g_cap.accountCalls);
	EXPECT_STREQ("capi", g_cap.channelid);
	EXPECT_DOUBLE_EQ(10.5, g_cap.available);

	g_adapterMgr.release();
	const size_t before = g_adapterMgr.size();
	EXPECT_FALSE(config(kCfgOk, false));
	// C 入口在任一 active adapter 初始化失败时返回 false，且不登记失败项。
	EXPECT_EQ(before, g_adapterMgr.size());

	release();
	EXPECT_NO_THROW(getDumper().release());
	g_adapterMgr.release();
}

TEST_F(TempCwd, cwd_handles_long_paths_and_reports_unavailable_cwd)
{
	WtHelper::set_module_dir("/tmp/wt_dummy_mod_dir/");
	EXPECT_STREQ("/tmp/wt_dummy_mod_dir/", WtHelper::get_module_dir());

	// A deleted current directory makes current_path/getcwd fail without depending
	// on permissions or a platform-specific fixed buffer size. No production call
	// site caches this helper before this test.
	const std::string parentCwd = std::filesystem::current_path().string();
	const std::string deletedCwd = dir + "/deleted_cwd";
	ASSERT_TRUE(std::filesystem::create_directory(deletedCwd));
	ASSERT_EQ(0, chdir(deletedCwd.c_str()));
	ASSERT_EQ(0, rmdir(deletedCwd.c_str()));
	const char* unavailable = WtHelper::get_cwd();
	ASSERT_NE(unavailable, nullptr);
	EXPECT_STREQ("", unavailable);

	// Failure must not be cached; a later successful lookup must work.
	ASSERT_EQ(0, chdir(parentCwd.c_str()));

	// Every component is within NAME_MAX, while the full valid cwd exceeds the
	// old 255-byte getcwd buffer.
	std::filesystem::path deepPath(dir);
	for (char segment = 'a'; segment < 'e'; ++segment)
		deepPath /= std::string(72, segment);
	std::error_code ec;
	ASSERT_TRUE(std::filesystem::create_directories(deepPath, ec));
	ASSERT_FALSE(ec);
	const std::string deepPathString = deepPath.string();
	ASSERT_EQ(0, chdir(deepPathString.c_str()));
	const std::string actualCwd = std::filesystem::current_path(ec).string();
	ASSERT_FALSE(ec);
	const char* cwd = WtHelper::get_cwd();
	ASSERT_NE(cwd, nullptr);
	EXPECT_STREQ((actualCwd + "/").c_str(), cwd);
	EXPECT_GT(strlen(cwd), 255u);
	ASSERT_EQ(0, chdir(parentCwd.c_str()));
	std::filesystem::remove_all(deepPath, ec);
	ASSERT_FALSE(ec);
}

//////////////////////////////////////////////////////////////////////////
// 挂死 / 崩溃路径：整条 DISABLED
//////////////////////////////////////////////////////////////////////////
TEST(dumper, DISABLED_run_once_loops_forever_and_run_threaded_spawns_worker)
{
	GTEST_SKIP() << "Dumper::run(true) 是 for(;;) 轮询 isAnyAlive()（Dumper.cpp:103-111）；"
					"run(false) 起 StdThread。本 lane 禁止起线程/挂死";
}

TEST(trader_adapter, DISABLED_on_rsp_trades_always_derefs_null_api)
{
	GTEST_SKIP() << "TraderAdapter::onRspTrades:249 无条件 _trader_api->queryOrders()";
}

TEST(trader_adapter, DISABLED_login_and_connect_success_paths_deref_null_api)
{
	GTEST_SKIP() << "handleEvent(WTE_Connect,0):143 / onLoginResult(true):171 / queryFund:185 / "
					"queryPosition:193 都要 _trader_api->xxx()，且 _date 未初始化（见 bug）";
}
