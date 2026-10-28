/*!
 * \file r125_sel_test.cpp
 * \brief lane R125: WtStraDtSel 的 on_init/on_schedule 全分支 + WtSelStraFact 公开工厂接口
 *
 * 覆盖依据（本轮读源码确认）：
 * - WtStraDtSel.cpp L70-97 init 参数（codes 逗号分割）；
 *   L104-112 on_init 逐 code 订阅；L121-234 on_schedule：非交易时段 continue、
 *   缺/空 K 线早退、stock 后缀与 100 股单位、无仓突破进场、多/空仓突破出场；
 *   L243-259 on_tick/on_bar 空实现。
 * - WtSelStraFact.cpp L28-42 C 导出 createSelStrategyFact/deleteSelStrategyFact；
 *   L87-99 deleteStrategy 非本厂拒删；L108-123 enumStrategy/getName。
 * 假上下文见 r125_selfthft_fake.h；K 线为真实 WTSKlineSlice，不加载插件。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "Includes/WTSVariant.hpp"
#include "Includes/SelStrategyDefs.h"
#include "WtSelStraFact/WtSelStraFact.h"
#include "WtSelStraFact/WtStraDtSel.h"

#include "r125_selfthft_fake.h"

USING_NS_WTP;

extern "C" ISelStrategyFact* createSelStrategyFact();
extern "C" void deleteSelStrategyFact(ISelStrategyFact* fact);

namespace {

class VarHolder
{
public:
	explicit VarHolder(WTSVariant* v = NULL) : _v(v) {}
	~VarHolder() { reset(); }
	VarHolder(const VarHolder&) = delete;
	VarHolder& operator=(const VarHolder&) = delete;
	void reset() { if (_v != NULL) { _v->release(); _v = NULL; } }
	WTSVariant* get() const { return _v; }
private:
	WTSVariant* _v;
};

WTSVariant* makeSelCfg(const char* codes, bool stock)
{
	WTSVariant* root = WTSVariant::createObject();
	root->append("days", (uint32_t)2);
	root->append("k1", 0.25);
	root->append("k2", 0.25);
	root->append("period", "d1");
	root->append("count", (uint32_t)10);
	root->append("stock", stock);
	root->append("codes", codes);
	return root;
}

struct EnumLog
{
	std::string fact;
	std::string stra;
	bool isLast;
};
std::vector<EnumLog>& enum_logs()
{
	static std::vector<EnumLog> logs;
	return logs;
}
void collect_enum(const char* fact, const char* stra, bool isLast)
{
	enum_logs().push_back(EnumLog{ fact == NULL ? "" : fact, stra == NULL ? "" : stra, isLast });
}

bool hasString(const std::vector<std::string>& values, const std::string& needle)
{
	for (const auto& value : values)
	{
		if (value.find(needle) != std::string::npos)
			return true;
	}
	return false;
}

} // namespace

TEST(R125SelFactory, CreateDeleteAndForeignReject)
{
	// WtSelStraFact.cpp L28-42：C 导出工厂的创建/删除（NULL 与非 NULL）
	ISelStrategyFact* api = createSelStrategyFact();
	ASSERT_NE(nullptr, api);
	EXPECT_STREQ("WtSelStraFact", api->getName());
	EXPECT_EQ(nullptr, api->createStrategy("NoSuchSel", "x"));

	enum_logs().clear();
	api->enumStrategy(collect_enum);
	ASSERT_EQ(1u, enum_logs().size());
	EXPECT_TRUE(enum_logs()[0].isLast);
	EXPECT_STREQ("DualThrustSelection", enum_logs()[0].stra.c_str());

	SelStrategy* stra = api->createStrategy("DualThrustSelection", "r125_sel_api");
	ASSERT_NE(nullptr, stra);
	EXPECT_STREQ("DualThrustSelection", stra->getName());
	EXPECT_TRUE(api->deleteStrategy(stra));

	// L87-99：非本厂策略拒删；NULL 视为成功
	r125_sh::ForeignSelStrategy foreign;
	EXPECT_FALSE(api->deleteStrategy(&foreign));
	EXPECT_TRUE(api->deleteStrategy(NULL));

	deleteSelStrategyFact(api);
	deleteSelStrategyFact(NULL);
}

TEST(R125SelStrategy, InitSubscribesAllCodes)
{
	// WtStraDtSel.cpp L70-97 + L104-112：codes 按逗号分割进 _codes，on_init 全量订阅
	WtStraDtSel stra("r125_sel_init");
	VarHolder cfg(makeSelCfg("SHFE.au.2005,SHFE.ag.2005", false));
	ASSERT_TRUE(stra.init(cfg.get()));

	r125_sh::SelCtx ctx;
	stra.on_init(&ctx);
	ASSERT_EQ(2u, ctx.subs.size());
	EXPECT_TRUE(hasString(ctx.subs, "SHFE.au.2005"));
	EXPECT_TRUE(hasString(ctx.subs, "SHFE.ag.2005"));

	// init(NULL) 走 L72-73 返回 false
	EXPECT_FALSE(stra.init(NULL));
}

TEST(R125SelStrategy, ScheduleEarlyReturns)
{
	// WtStraDtSel.cpp L127-150：非交易时段 continue；缺 K 线 / 空 K 线均直接 return
	WtStraDtSel stra("r125_sel_early");
	VarHolder cfg(makeSelCfg("SHFE.au.2005", false));
	ASSERT_TRUE(stra.init(cfg.get()));

	r125_sh::SelCtx ctx;

	// uTime=800 在 900-1500 时段之前 → isInTradingTime=false → continue
	stra.on_schedule(&ctx, 20240102, 800);
	EXPECT_TRUE(ctx.set_positions.empty());
	EXPECT_TRUE(ctx.bars_code.empty());

	// 交易时段内，但 bars==NULL
	ctx.bars = NULL;
	stra.on_schedule(&ctx, 20240102, 1000);
	EXPECT_TRUE(ctx.set_positions.empty());
	EXPECT_STREQ("SHFE.au.2005", ctx.bars_code.c_str());

	// 交易时段内，但 K 线为空
	ctx.bars = r125_sh::make_empty_kline();
	stra.on_schedule(&ctx, 20240102, 1000);
	ctx.bars = NULL;
	EXPECT_TRUE(ctx.set_positions.empty());
}

TEST(R125SelStrategy, ScheduleBreakoutMatrix)
{
	// WtStraDtSel.cpp L152-233：days=2/k=0.25、closes={5..9}（high=10/low=8）时
	// hh=9/ll=7/hc=8/lc=8 → range=1，upper=lastOpen+0.25、lower=lastOpen-0.25。
	std::vector<WTSBarStruct> store;

	// 无仓 + high>=upper → 多仓进场 +1
	{
		WtStraDtSel stra("r125_sel_long");
		VarHolder cfg(makeSelCfg("SHFE.au.2005", false));
		ASSERT_TRUE(stra.init(cfg.get()));
		r125_sh::SelCtx ctx;
		ctx.position = 0.0;
		ctx.bars = r125_sh::make_kline(store, {5, 6, 7, 8, 9}, 8.4);
		stra.on_schedule(&ctx, 20240102, 1000);
		ctx.bars = NULL;
		ASSERT_EQ(1u, ctx.set_positions.size());
		EXPECT_NE(std::string::npos, ctx.set_positions[0].find("SHFE.au.2005:1.000000:DT_EnterLong"));
		EXPECT_TRUE(hasString(ctx.logs, "向上突破"));
	}

	// 无仓 + low<=lower 且 high<upper → 空仓进场 -1
	{
		WtStraDtSel stra("r125_sel_short");
		VarHolder cfg(makeSelCfg("SHFE.au.2005", false));
		ASSERT_TRUE(stra.init(cfg.get()));
		r125_sh::SelCtx ctx;
		ctx.position = 0.0;
		ctx.bars = r125_sh::make_kline(store, {5, 6, 7, 8, 9}, 10.0);
		stra.on_schedule(&ctx, 20240102, 1000);
		ctx.bars = NULL;
		ASSERT_EQ(1u, ctx.set_positions.size());
		EXPECT_NE(std::string::npos, ctx.set_positions[0].find("-1.000000:DT_EnterShort"));
		EXPECT_TRUE(hasString(ctx.logs, "向下突破"));
	}

	// 多仓 + low<=lower → 平多至 0
	{
		WtStraDtSel stra("r125_sel_exitlong");
		VarHolder cfg(makeSelCfg("SHFE.au.2005", false));
		ASSERT_TRUE(stra.init(cfg.get()));
		r125_sh::SelCtx ctx;
		ctx.position = 3.0;
		ctx.bars = r125_sh::make_kline(store, {5, 6, 7, 8, 9}, 10.0);
		stra.on_schedule(&ctx, 20240102, 1000);
		ctx.bars = NULL;
		ASSERT_EQ(1u, ctx.set_positions.size());
		EXPECT_NE(std::string::npos, ctx.set_positions[0].find(":0.000000:DT_ExitLong"));
	}

	// 空仓 + high>=upper → 平空至 0
	{
		WtStraDtSel stra("r125_sel_exitshort");
		VarHolder cfg(makeSelCfg("SHFE.au.2005", false));
		ASSERT_TRUE(stra.init(cfg.get()));
		r125_sh::SelCtx ctx;
		ctx.position = -3.0;
		ctx.bars = r125_sh::make_kline(store, {5, 6, 7, 8, 9}, 8.4);
		stra.on_schedule(&ctx, 20240102, 1000);
		ctx.bars = NULL;
		ASSERT_EQ(1u, ctx.set_positions.size());
		EXPECT_NE(std::string::npos, ctx.set_positions[0].find(":0.000000:DT_ExitShort"));
	}

	// stock=true → 标准化代码追加 "-"，trdUnit=100，突破上轨买 100 股
	{
		WtStraDtSel stra("r125_sel_stock");
		VarHolder cfg(makeSelCfg("SSE.600000", true));
		ASSERT_TRUE(stra.init(cfg.get()));
		r125_sh::SelCtx ctx;
		ctx.position = 0.0;
		ctx.bars = r125_sh::make_kline(store, {5, 6, 7, 8, 9}, 8.4);
		stra.on_schedule(&ctx, 20240102, 1000);
		ctx.bars = NULL;
		EXPECT_STREQ("SSE.600000-", ctx.bars_code.c_str());
		ASSERT_EQ(1u, ctx.set_positions.size());
		EXPECT_NE(std::string::npos, ctx.set_positions[0].find("SSE.600000:100.000000:DT_EnterLong"));
	}
}

TEST(R125SelStrategy, TickAndBarCallbacksAreNoop)
{
	// WtStraDtSel.cpp L243-259：on_tick/on_bar 空实现，不改持仓、不写日志
	WtStraDtSel stra("r125_sel_callbacks");
	VarHolder cfg(makeSelCfg("SHFE.au.2005", false));
	ASSERT_TRUE(stra.init(cfg.get()));

	r125_sh::SelCtx ctx;
	WTSTickStruct tick;
	WTSTickData* data = WTSTickData::create(tick);
	stra.on_tick(&ctx, "SHFE.au.2005", data);
	data->release();

	WTSBarStruct bar;
	stra.on_bar(&ctx, "SHFE.au.2005", "d1", &bar);
	EXPECT_TRUE(ctx.set_positions.empty());
	EXPECT_TRUE(ctx.logs.empty());
}
