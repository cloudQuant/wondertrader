/*!
 * \file n06_cta_ctx_mock_test.cpp
 * \brief lane N06: 用最小 ICtaStraCtx 假上下文驱动 WtStraDualThrust / WtStraTwoMa 的
 *        on_init / on_session_begin / on_schedule 纯逻辑分支（真实 WTSKlineSlice 合成K线）
 *
 * 依据源码（本轮已读）：
 * - WtStraDualThrust::on_schedule: maxprice/minprice(-days,-2)、extractData(KFT_CLOSE)、
 *   upper=open+k1*max(hh-lc,hc-ll)，curPx>=upper 且空仓 → stra_enter_long(...,2*trdUnit,"DT_EnterLong")
 * - WtStraTwoMa::on_schedule: 短/长期均线交叉，金叉 → stra_enter_long(...,"DT_EnterLong")
 * - on_session_begin: stra_get_rawcode 变化且旧主力有仓 → 两次 stra_set_position(switchout/switchin)
 */
#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Includes/WTSVariant.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/ICtaStraCtx.h"
#include "Includes/CtaStrategyDefs.h"
#include "WtCtaTwoMaFact/WtStraTwoMa.h"
#include "WtCtaStraFact/WtStraDualThrust.h"

USING_NS_WTP;

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
	WTSVariant* operator->() const { return _v; }
private:
	WTSVariant* _v;
};

/**
 * 假 CTA 上下文：记录策略侧的全部调用，返回调用方预置的 K 线切片
 */
class MockCtaCtx : public ICtaStraCtx
{
public:
	MockCtaCtx() : ICtaStraCtx("n06_mock"), _bars(NULL), _position(0.0), _rawcode("SHFE.au.2005") {}

	void set_bars(WTSKlineSlice* bars) { _bars = bars; }
	void set_position(double p) { _position = p; }
	void set_rawcode(const std::string& code) { _rawcode = code; }

	std::vector<std::string> orders;    // "enter_long:<code>:<qty>:<tag>" 等
	std::vector<std::string> logs;
	std::vector<std::string> subs;
	std::vector<std::string> setPositions;
	std::map<std::string, double> indexValues;

	uint32_t id() override { return 1; }
	void on_init() override {}
	void on_session_begin(uint32_t) override {}
	void on_session_end(uint32_t) override {}
	void on_tick(const char*, WTSTickData*, bool) override {}
	void on_bar(const char*, const char*, uint32_t, WTSBarStruct*) override {}
	bool on_schedule(uint32_t, uint32_t) override { return true; }
	void on_bar_close(const char*, const char*, WTSBarStruct*) override {}
	void on_calculate(uint32_t, uint32_t) override {}
	void enum_position(FuncEnumCtaPosCallBack, bool) override {}

	void stra_enter_long(const char* c, double q, const char* tag, double, double) override
	{ orders.push_back(std::string("enter_long:") + c + ":" + std::to_string(q) + ":" + tag); }
	void stra_enter_short(const char* c, double q, const char* tag, double, double) override
	{ orders.push_back(std::string("enter_short:") + c + ":" + std::to_string(q) + ":" + tag); }
	void stra_exit_long(const char* c, double q, const char* tag, double, double) override
	{ orders.push_back(std::string("exit_long:") + c + ":" + std::to_string(q) + ":" + tag); }
	void stra_exit_short(const char* c, double q, const char* tag, double, double) override
	{ orders.push_back(std::string("exit_short:") + c + ":" + std::to_string(q) + ":" + tag); }

	double stra_get_position(const char*, bool, const char*) override { return _position; }
	void stra_set_position(const char* c, double q, const char* tag, double, double) override
	{ setPositions.push_back(std::string(c) + ":" + std::to_string(q) + ":" + tag); }
	double stra_get_price(const char*) override { return 0.0; }
	double stra_get_day_price(const char*, int) override { return 0.0; }
	uint32_t stra_get_tdate() override { return 20240102; }
	uint32_t stra_get_date() override { return 20240102; }
	uint32_t stra_get_time() override { return 150000; }
	double stra_get_fund_data(int) override { return 0.0; }
	uint64_t stra_get_first_entertime(const char*) override { return 0; }
	uint64_t stra_get_last_entertime(const char*) override { return 0; }
	uint64_t stra_get_last_exittime(const char*) override { return 0; }
	double stra_get_last_enterprice(const char*) override { return 0.0; }
	double stra_get_position_avgpx(const char*) override { return 0.0; }
	double stra_get_position_profit(const char*) override { return 0.0; }
	uint64_t stra_get_detail_entertime(const char*, const char*) override { return 0; }
	double stra_get_detail_cost(const char*, const char*) override { return 0.0; }
	double stra_get_detail_profit(const char*, const char*, int) override { return 0.0; }
	WTSCommodityInfo* stra_get_comminfo(const char*) override { return NULL; }
	WTSKlineSlice* stra_get_bars(const char*, const char*, uint32_t, bool) override { return _bars; }
	WTSTickSlice* stra_get_ticks(const char*, uint32_t) override { return NULL; }
	WTSTickData* stra_get_last_tick(const char*) override { return NULL; }
	std::string stra_get_rawcode(const char*) override { return _rawcode; }
	void stra_sub_ticks(const char* c) override { subs.push_back(c); }
	void stra_sub_bar_events(const char*, const char*) override {}
	void stra_log_info(const char* m) override { logs.push_back(m); }
	void stra_log_debug(const char* m) override { logs.push_back(m); }
	void stra_log_error(const char* m) override { logs.push_back(m); }
	const char* stra_get_last_entertag(const char*) override { return ""; }

	// 图表类虚函数带默认实现，这里覆写以便记录
	void set_chart_kline(const char* c, const char* p) override { subs.push_back(std::string("chart:") + c + ":" + p); }
	void register_index(const char* n, uint32_t t) override { subs.push_back(std::string("index:") + n + ":" + std::to_string(t)); }
	bool register_index_line(const char* n, const char* l, uint32_t t) override
	{ subs.push_back(std::string("idxline:") + n + ":" + l + ":" + std::to_string(t)); return true; }
	bool set_index_value(const char*, const char* l, double v) override
	{ indexValues[l] = v; return true; }

private:
	WTSKlineSlice* _bars;
	double _position;
	std::string _rawcode;
};

// create() 内部按指针引用 bars 数组，容器需覆盖 slice 使用期（进程单测结束后随 OS 回收）
std::vector<std::vector<WTSBarStruct>*> g_bar_holders;

// 用收盘价序列构造真实 WTSKlineSlice（策略内部走 extractData/maxprice 真代码）
WTSKlineSlice* makeKlineSlice(const std::vector<double>& closes, double lastOpen)
{
	std::vector<WTSBarStruct>* bars = new std::vector<WTSBarStruct>();
	for (size_t i = 0; i < closes.size(); i++)
	{
		WTSBarStruct bar;
		bar.date = 20240100 + (uint32_t)i;
		bar.time = 150000;
		bar.open = (i + 1 == closes.size()) ? lastOpen : closes[i];
		bar.high = closes[i] + 1;
		bar.low = closes[i] - 1;
		bar.close = closes[i];
		bar.settle = closes[i];
		bars->push_back(bar);
	}
	WTSKlineSlice* slice = WTSKlineSlice::create("SHFE.au.2005",
		static_cast<WTSKlinePeriod>(1), 1, bars->data(), (int32_t)bars->size());
	// create() 内部拷贝 bars 指针引用， bars 生命周期需覆盖 slice 使用期
	g_bar_holders.push_back(bars);
	return slice;
}

WTSVariant* makeDtCfg(uint32_t days, double k1, double k2)
{
	WTSVariant* root = WTSVariant::createObject();
	root->append("days", days);
	root->append("k1", k1);
	root->append("k2", k2);
	root->append("period", "d1");
	root->append("count", (uint32_t)10);
	root->append("code", "SHFE.au.2005");
	root->append("stock", false);
	return root;
}

bool hasPrefix(const std::vector<std::string>& vec, const std::string& prefix)
{
	for (const auto& s : vec)
		if (s.compare(0, prefix.size(), prefix) == 0)
			return true;
	return false;
}

} // namespace

TEST(n06_cta_ctx, dualthrust_on_init_subscribes_and_registers_index)
{
	WtStraDualThrust dt("s1");
	VarHolder cfg(makeDtCfg(2, 0.5, 0.5));
	ASSERT_TRUE(dt.init(cfg.get()));

	MockCtaCtx ctx;
	WTSKlineSlice* slice = makeKlineSlice({5, 6, 7, 8, 9}, 8.5);
	ctx.set_bars(slice);
	dt.on_init(&ctx);

	EXPECT_TRUE(hasPrefix(ctx.subs, "SHFE.au.2005"));         // stra_sub_ticks
	EXPECT_TRUE(hasPrefix(ctx.subs, "chart:SHFE.au.2005"));   // set_chart_kline
	EXPECT_TRUE(hasPrefix(ctx.subs, "index:DualThrust"));     // register_index
	EXPECT_TRUE(hasPrefix(ctx.subs, "idxline:DualThrust:upper_bound"));
	EXPECT_TRUE(hasPrefix(ctx.subs, "idxline:DualThrust:lower_bound"));
}

TEST(n06_cta_ctx, dualthrust_on_session_begin_switches_main_contract)
{
	WtStraDualThrust dt("s2");
	VarHolder cfg(makeDtCfg(2, 0.5, 0.5));
	ASSERT_TRUE(dt.init(cfg.get()));

	MockCtaCtx ctx;
	// 第一天：拿到主力 SHFE.au.2005，无旧主力，不清理
	dt.on_session_begin(&ctx, 20240101);
	EXPECT_TRUE(ctx.setPositions.empty());

	// 换月前持多仓 3 手
	ctx.set_position(3.0);
	ctx.set_rawcode("SHFE.au.2006");
	dt.on_session_begin(&ctx, 20240102);
	ASSERT_EQ(2u, ctx.setPositions.size());
	EXPECT_NE(std::string::npos, ctx.setPositions[0].find("2005:0:switchout"));
	EXPECT_NE(std::string::npos, ctx.setPositions[1].find("2006:3:switchin"));

	// 主力不变 → 不重复触发
	ctx.setPositions.clear();
	dt.on_session_begin(&ctx, 20240103);
	EXPECT_TRUE(ctx.setPositions.empty());
}

TEST(n06_cta_ctx, dualthrust_on_schedule_breaks_upper_enters_long)
{
	WtStraDualThrust dt("s3");
	// days=2: hh/ll/hc/lc 都取倒数第 2 根；close[-2]=8 → hh=hc=9? high=close+1
	// 序列 {5,6,7,8,9}: bar[-2] close=8, high=9, low=7；closes[-2]=8
	// hh=9, ll=7, hc=8, lc=8 → max(hh-lc, hc-ll)=max(1,1)=1
	// lastOpen=9 → upper=9+0.5*1=9.5, lower=9-0.5=8.5；curPx=closes[-1]=9
	// 9 >= 9.5? 否 —— 所以把 lastOpen 调到 8.4：upper=8.9，curPx=9 >= 8.9 触发做多
	VarHolder cfg(makeDtCfg(2, 0.5, 0.5));
	ASSERT_TRUE(dt.init(cfg.get()));

	MockCtaCtx ctx;
	ctx.set_bars(makeKlineSlice({5, 6, 7, 8, 9}, 8.4));
	ctx.set_position(0.0);
	dt.on_schedule(&ctx, 20240102, 150000);

	EXPECT_TRUE(hasPrefix(ctx.orders, "enter_long:")) << "金叉应触发 DT_EnterLong";
	EXPECT_TRUE(hasPrefix(ctx.orders, ":DT_EnterLong"));
	EXPECT_NE(ctx.indexValues.end(), ctx.indexValues.find("upper_bound"));
	EXPECT_NE(ctx.indexValues.end(), ctx.indexValues.find("lower_bound"));
	EXPECT_GT(ctx.logs.size(), 0u);
}

TEST(n06_cta_ctx, dualthrust_on_schedule_long_position_exits_on_lower_break)
{
	WtStraDualThrust dt("s4");
	VarHolder cfg(makeDtCfg(2, 0.5, 0.5));
	ASSERT_TRUE(dt.init(cfg.get()));

	MockCtaCtx ctx;
	// curPx=9, lower=8.5+... 用 lastOpen=9.6: lower=9.6-0.5=9.1 → 9<=9.1 触发平多
	ctx.set_bars(makeKlineSlice({5, 6, 7, 8, 9}, 9.6));
	ctx.set_position(2.0);   // 持多
	dt.on_schedule(&ctx, 20240102, 150000);
	EXPECT_TRUE(hasPrefix(ctx.orders, "exit_long:"));
	EXPECT_FALSE(hasPrefix(ctx.orders, "enter_"));
}

TEST(n06_cta_ctx, twoma_on_init_and_empty_slice_paths)
{
	WtStraTwoMa ma("s5");
	VarHolder cfg(WTSVariant::createObject());
	cfg->append("short_days", (uint32_t)2);
	cfg->append("long_days", (uint32_t)3);
	cfg->append("period", "d1");
	cfg->append("count", (uint32_t)10);
	cfg->append("code", "SHFE.au.2005");
	cfg->append("stock", false);
	ASSERT_TRUE(ma.init(cfg.get()));

	MockCtaCtx ctx;
	ctx.set_bars(makeKlineSlice({1, 2, 3, 4, 5}, 5));
	ma.on_init(&ctx);
	EXPECT_TRUE(hasPrefix(ctx.subs, "index:TwoMa"));
	EXPECT_TRUE(hasPrefix(ctx.subs, "idxline:TwoMa:upper_bound"));
}

TEST(n06_cta_ctx, twoma_on_schedule_golden_cross_enters_long)
{
	WtStraTwoMa ma("s6");
	VarHolder cfg(WTSVariant::createObject());
	cfg->append("short_days", (uint32_t)2);
	cfg->append("long_days", (uint32_t)3);
	cfg->append("period", "d1");
	cfg->append("count", (uint32_t)10);
	cfg->append("code", "SHFE.au.2005");
	cfg->append("stock", false);
	ASSERT_TRUE(ma.init(cfg.get()));

	MockCtaCtx ctx;
	// closes: [0,100,1,50,60] → pre_s=(1+50)/2=25.5 < pre_l=(100+1+50)/3=50.33
	// now_s=(50+60)/2=55 > now_l=(1+50+60)/3=37 → 金叉做多
	ctx.set_bars(makeKlineSlice({0, 100, 1, 50, 60}, 60));
	ctx.set_position(0.0);
	ma.on_schedule(&ctx, 20240102, 150000);
	EXPECT_TRUE(hasPrefix(ctx.orders, "enter_long:"));
	EXPECT_TRUE(hasPrefix(ctx.orders, ":DT_EnterLong"));
	EXPECT_GT(ctx.logs.size(), 0u);
}

TEST(n06_cta_ctx, DISABLED_ZERO_PERIOD_DAYS_DIVIDE_CLASS)
{
	// 缺陷类：TwoMa 对 short_days/long_days==0 不校验，on_schedule 里
	// pre_short_ma/short_days 除零得到 inf/nan，交易信号进入无意义分支；
	// 属于数值域缺陷（不是崩溃），但同类的 days==0 在 DualThrust 里
	// maxprice(0,-2) 会把窗口放大到整个序列，语义错误。
	// 与 lane 纪律一致：同类输入全部收进本 DISABLED_ 用例。
	WtStraTwoMa ma("s7");
	VarHolder cfg(WTSVariant::createObject());
	cfg->append("short_days", (uint32_t)0);
	cfg->append("long_days", (uint32_t)0);
	cfg->append("period", "d1");
	cfg->append("count", (uint32_t)10);
	cfg->append("code", "SHFE.au.2005");
	ma.init(cfg.get());
	MockCtaCtx ctx;
	ctx.set_bars(makeKlineSlice({1, 2, 3, 4, 5}, 5));
	ma.on_schedule(&ctx, 20240102, 150000);   // 除零 → inf，不崩溃但结果无意义

	WtStraDualThrust dt("s8");
	VarHolder cfg2(makeDtCfg(0, 0.5, 0.5));
	dt.init(cfg2.get());
	dt.on_schedule(&ctx, 20240102, 150000);
}
