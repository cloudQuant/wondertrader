/*
 * r214_porter_hft_test.cpp  (lane R214 · wave29)
 *
 * 靶子：WtPorter 剩余可执行行
 *   - ExpHftContext.cpp:173-188  订阅后 on_tick -> ctx_on_tick(ET_HFT)
 *   - ExpHftContext.cpp:235-242  on_trade -> hft_on_trade + 基类持仓更新
 *   - WtPorter.cpp:(hft_get_ordque/hft_get_orddtl/hft_get_trans) 的 catch(...) 支
 *
 * 与既有 lane 的分工：R124 的同名用例在结尾用 ctx->stra_get_position() 收尾，
 * 该方法在未绑定 trader 时无条件解引用 _trader（BUG-R124-001），整条用例 SIGSEGV；
 * 本文件改用 stra_get_position_avgpx()（只读 _pos_map）证明基类 do_set_position
 * 已执行，不触碰 _trader。R163 只覆盖了三个切片命中支，catch 支需要
 * stra_get_* 抛异常；这里用公开的 ExpHftContext 子类 override 注入异常。
 *
 * 边界：不加载策略插件、不接柜台、不起行情线程；UDP/共享内存均不涉及；
 * 临时文件只落系统临时目录并在用例末尾清理。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include "WtCore/WtHelper.h"
#include "WtCore/WtHftEngine.h"
#include "WtPorter/ExpHftContext.h"
#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"

USING_NS_WTP;

extern WtRtRunner& getRunner();

namespace {

namespace stdfs = std::filesystem;

struct Rec
{
	uint32_t	hft_tick = 0;
	uint32_t	hft_trd = 0;
	CtxHandler	handle = 0;
	WtUInt32	localid = 0;
	std::string	code;
	bool		isbuy = false;
	double		vol = 0.0;
	double		price = 0.0;

	void clear() { *this = Rec(); }
};

Rec g;

void tick_cb(CtxHandler h, const char* code, WTSTickStruct*)
{
	g.hft_tick++;
	g.handle = h;
	g.code = code ? code : "";
}

void trd_cb(CtxHandler h, WtUInt32 localid, const char* code, bool isBuy,
	double vol, double price, const char*)
{
	g.hft_trd++;
	g.handle = h;
	g.localid = localid;
	g.code = code ? code : "";
	g.isbuy = isBuy;
	g.vol = vol;
	g.price = price;
}

std::string prepare_dir(const char* tag)
{
	stdfs::path p = stdfs::temp_directory_path()
		/ (std::string("r214_porter_") + tag + "_" + std::to_string((long)::getpid()));
	std::error_code ec;
	stdfs::remove_all(p, ec);
	stdfs::create_directories(p, ec);
	return p.string();
}

void write_text(const std::string& path, const std::string& body)
{
	std::ofstream ofs(path.c_str(), std::ios::binary | std::ios::trunc);
	ofs << body;
}

void reset_singleton()
{
	WtRtRunner& r = getRunner();
	r.~WtRtRunner();
	new (&r) WtRtRunner();
}

const char* kSessions =
	"{\"TRADING\":{\"name\":\"Trading\",\"offset\":0,"
	"\"sections\":[{\"from\":0,\"to\":2359}]}}";

const char* kCommodities =
	"{\"SHFE\":{\"rb\":{\"name\":\"Rebar\",\"session\":\"TRADING\",\"pricetick\":1,"
	"\"volscale\":10,\"covermode\":0,\"pricemode\":0,\"trademode\":0,\"category\":1}}}";

const char* kContracts =
	"{\"SHFE\":{\"rb2510\":{\"name\":\"rb2510\",\"product\":\"rb\",\"exchg\":\"SHFE\"}}}";

const char* kActions =
	"{\"default\":{\"order\":[{\"action\":\"open\"},{\"action\":\"close\"}]}}";

std::string cfg_plain(const std::string& dir, const char* env)
{
	return "{\"basefiles\":{},\"env\":{\"name\":\"" + std::string(env) + "\"},"
		"\"data\":{},\"bspolicy\":\"" + dir + "/actions.json\"}";
}

std::string cfg_with_contracts(const std::string& dir, const char* env)
{
	return "{\"basefiles\":{"
		"\"session\":\"" + dir + "/sessions.json\","
		"\"commodity\":\"" + dir + "/commodities.json\","
		"\"contract\":\"" + dir + "/contracts.json\"},"
		"\"env\":{\"name\":\"" + std::string(env) + "\"},"
		"\"data\":{},\"bspolicy\":\"" + dir + "/actions.json\"}";
}

WTSTickData* make_tick(const char* code)
{
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	wt_strcpy(ts.code, code);
	ts.action_date = 20240102;
	ts.action_time = 93000000;
	ts.price = 4500.0;
	ts.ask_prices[0] = 4501.0;
	ts.ask_qty[0] = 10;
	ts.bid_prices[0] = 4499.0;
	ts.bid_qty[0] = 10;
	return WTSTickData::create(ts);
}

/* 公开子类：只 override 切片入口，让 C API 的 catch(...) 可达 */
class R214ThrowHftCtx : public ExpHftContext
{
public:
	explicit R214ThrowHftCtx(WtHftEngine* eng)
		: ExpHftContext(eng, "r214_hft_throw", false, 0) {}

	WTSOrdQueSlice* stra_get_order_queue(const char*, uint32_t) override
	{
		throw std::runtime_error("r214 ordque");
	}

	WTSOrdDtlSlice* stra_get_order_detail(const char*, uint32_t) override
	{
		throw std::runtime_error("r214 orddtl");
	}

	WTSTransSlice* stra_get_transaction(const char*, uint32_t) override
	{
		throw std::runtime_error("r214 trans");
	}
};

} // namespace

/* 生产: ExpHftContext.cpp:173-188 + :235-242 */
TEST(R214PorterHftCtx, SubscribedTickAndTradeAreForwardedWithoutTrader)
{
	const std::string dir = prepare_dir("hft");
	write_text(dir + "/actions.json", kActions);
	write_text(dir + "/sessions.json", kSessions);
	write_text(dir + "/commodities.json", kCommodities);
	write_text(dir + "/contracts.json", kContracts);

	reset_singleton();
	getRunner().init((dir + "/r214_absent_logcfg.yaml").c_str(), true, dir.c_str());
	WtHelper::setGenerateDir(dir.c_str());
	ASSERT_TRUE(getRunner().config(cfg_with_contracts(dir, "hft").c_str(), false));

	g.clear();
	register_hft_callbacks(NULL, tick_cb, NULL, NULL, NULL, trd_cb, NULL,
		NULL, NULL, NULL, NULL, NULL);

	const uint32_t id = getRunner().createHftContext("r214_hft", "R214_TRADER_ABSENT", false, 0);
	ASSERT_NE(0U, id);
	HftContextPtr base = getRunner().getHftContext(id);
	ASSERT_TRUE(base != nullptr);
	ExpHftContext* ctx = dynamic_cast<ExpHftContext*>(base.get());
	ASSERT_NE(nullptr, ctx);

	const char* code = "SHFE.rb.2510";
	ctx->stra_sub_ticks(code);
	WTSTickData* tick = make_tick(code);
	ASSERT_NE(nullptr, tick);
	ctx->on_tick(code, tick);
	tick->release();

	EXPECT_EQ(1U, g.hft_tick);
	EXPECT_EQ(id, g.handle);
	EXPECT_EQ(std::string(code), g.code);

	ctx->on_trade(9001, code, true, 2.0, 4500.0);
	EXPECT_EQ(1U, g.hft_trd);
	EXPECT_EQ(id, g.handle);
	EXPECT_EQ(9001U, g.localid);
	EXPECT_EQ(std::string(code), g.code);
	EXPECT_TRUE(g.isbuy);
	EXPECT_DOUBLE_EQ(2.0, g.vol);
	EXPECT_DOUBLE_EQ(4500.0, g.price);

	/* 基类 do_set_position 已把 2 手 @4500 写入 _pos_map；avgpx 只读持仓表，
	 * 不会像 stra_get_position 那样解引用未绑定的 _trader。 */
	EXPECT_DOUBLE_EQ(4500.0, ctx->stra_get_position_avgpx(code));

	register_hft_callbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL);
	reset_singleton();
	std::error_code ec;
	stdfs::remove_all(dir, ec);
}

/* 生产: WtPorter.cpp hft_get_ordque / hft_get_orddtl / hft_get_trans 的 catch(...) 支
 * （1881-1983 区间内三段 catch，逐段 return 0） */
TEST(R214PorterHftSlices, ThrowingSliceGettersAreCaughtByCApi)
{
	const std::string dir = prepare_dir("hft_throw");
	write_text(dir + "/actions.json", kActions);

	reset_singleton();
	getRunner().init((dir + "/r214_absent_logcfg.yaml").c_str(), true, dir.c_str());
	WtHelper::setGenerateDir(dir.c_str());
	ASSERT_TRUE(getRunner().config(cfg_plain(dir, "hft").c_str(), false));

	WtHftEngine* eng = dynamic_cast<WtHftEngine*>(getRunner().getEngine());
	ASSERT_NE(nullptr, eng);
	HftContextPtr ctx(new R214ThrowHftCtx(eng));
	eng->addContext(ctx);
	const CtxHandler h = ctx->id();
	ASSERT_NE(0U, h);

	EXPECT_EQ(0U, hft_get_ordque(h, "SHFE.rb.2510", 5, NULL));
	EXPECT_EQ(0U, hft_get_orddtl(h, "SHFE.rb.2510", 5, NULL));
	EXPECT_EQ(0U, hft_get_trans(h, "SHFE.rb.2510", 5, NULL));

	reset_singleton();
	std::error_code ec;
	stdfs::remove_all(dir, ec);
}
