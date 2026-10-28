/*!
 * \file r125_cta_fake.h
 * \brief lane R125: 最小 ICtaStraCtx 假上下文 + 真实 WTSKlineSlice 构造工具
 *
 * 仅实现 ICtaStraCtx 的纯虚接口，记录策略调用的委托/日志/指标/图表标记；
 * 不加载插件、不起线程、不读挂钟。
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/ICtaStraCtx.h"
#include "Includes/CtaStrategyDefs.h"

USING_NS_WTP;

namespace r125_cta {

class Ctx : public ICtaStraCtx
{
public:
	Ctx() : ICtaStraCtx("r125_cta") {}

	WTSKlineSlice* bars = NULL;
	double position = 0.0;
	std::string rawcode = "SHFE.au.2005";

	std::vector<std::string> orders;
	std::vector<std::string> logs;
	std::vector<std::string> subs;
	std::vector<std::string> set_positions;
	std::vector<std::string> marks;
	std::map<std::string, double> index_values;

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

	void stra_enter_long(const char* code, double qty, const char* tag, double, double) override
	{
		orders.push_back(std::string("enter_long:") + code + ":" + std::to_string(qty) + ":" + tag);
	}
	void stra_enter_short(const char* code, double qty, const char* tag, double, double) override
	{
		orders.push_back(std::string("enter_short:") + code + ":" + std::to_string(qty) + ":" + tag);
	}
	void stra_exit_long(const char* code, double qty, const char* tag, double, double) override
	{
		orders.push_back(std::string("exit_long:") + code + ":" + std::to_string(qty) + ":" + tag);
	}
	void stra_exit_short(const char* code, double qty, const char* tag, double, double) override
	{
		orders.push_back(std::string("exit_short:") + code + ":" + std::to_string(qty) + ":" + tag);
	}

	double stra_get_position(const char*, bool, const char*) override { return position; }
	void stra_set_position(const char* code, double qty, const char* tag, double, double) override
	{
		set_positions.push_back(std::string(code) + ":" + std::to_string(qty) + ":" + tag);
	}
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
	WTSKlineSlice* stra_get_bars(const char*, const char*, uint32_t, bool) override { return bars; }
	WTSTickSlice* stra_get_ticks(const char*, uint32_t) override { return NULL; }
	WTSTickData* stra_get_last_tick(const char*) override { return NULL; }
	std::string stra_get_rawcode(const char*) override { return rawcode; }
	void stra_sub_ticks(const char* code) override { subs.push_back(code); }
	void stra_sub_bar_events(const char*, const char*) override {}
	void stra_log_info(const char* msg) override { logs.push_back(msg); }
	void stra_log_debug(const char* msg) override { logs.push_back(msg); }
	void stra_log_error(const char* msg) override { logs.push_back(msg); }
	const char* stra_get_last_entertag(const char*) override { return ""; }

	void set_chart_kline(const char* code, const char* period) override
	{
		subs.push_back(std::string("chart:") + code + ":" + period);
	}
	void add_chart_mark(double price, const char* icon, const char* tag) override
	{
		marks.push_back(std::string(icon) + ":" + tag + ":" + std::to_string(price));
	}
	void register_index(const char* name, uint32_t type) override
	{
		subs.push_back(std::string("index:") + name + ":" + std::to_string(type));
	}
	bool register_index_line(const char* name, const char* line, uint32_t type) override
	{
		subs.push_back(std::string("idxline:") + name + ":" + line + ":" + std::to_string(type));
		return true;
	}
	bool set_index_value(const char* name, const char* line, double value) override
	{
		index_values[std::string(name) + "/" + line] = value;
		return true;
	}
};

/// 用收盘价序列填充分钟 K 线，最后一根开盘价单独指定（用于控制 DualThrust 轨道）
inline void fill_bars(std::vector<WTSBarStruct>& store, const std::vector<double>& closes, double last_open)
{
	store.clear();
	for (std::size_t i = 0; i < closes.size(); i++)
	{
		WTSBarStruct bar;
		bar.date = 20240100u + static_cast<uint32_t>(i);
		bar.time = 150000;
		bar.open = (i + 1 == closes.size()) ? last_open : closes[i];
		bar.high = closes[i] + 1.0;
		bar.low = closes[i] - 1.0;
		bar.close = closes[i];
		bar.settle = closes[i];
		store.push_back(bar);
	}
}

inline WTSKlineSlice* make_kline(std::vector<WTSBarStruct>& store, const std::vector<double>& closes, double last_open)
{
	fill_bars(store, closes, last_open);
	return WTSKlineSlice::create("SHFE.au.2005", KP_Minute1, 1,
		store.data(), static_cast<int32_t>(store.size()));
}

inline WTSKlineSlice* make_empty_kline()
{
	return WTSKlineSlice::create("SHFE.au.2005", KP_Minute1, 1, NULL, 0);
}

/// 工厂“非本厂策略拒删”分支的探针策略
class ForeignCtaStrategy : public CtaStrategy
{
public:
	ForeignCtaStrategy() : CtaStrategy("r125_foreign") {}

	const char* getName() override { return "ForeignCta"; }
	const char* getFactName() override { return "OtherCtaFact"; }
};

} // namespace r125_cta
