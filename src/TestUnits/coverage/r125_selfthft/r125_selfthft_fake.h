/*!
 * \file r125_selfthft_fake.h
 * \brief lane R125: SEL/UFT/HFT 三族的最小策略上下文与数据构造工具
 *
 * 三个上下文只实现各自接口的纯虚函数；委托/撤单/参数/日志均记录在内存里。
 * 不加载插件、不起线程、不读挂钟，临时对象全部由测试栈持有并释放。
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/ISelStraCtx.h"
#include "Includes/IUftStraCtx.h"
#include "Includes/IHftStraCtx.h"
#include "Includes/SelStrategyDefs.h"
#include "Includes/UftStrategyDefs.h"
#include "Includes/HftStrategyDefs.h"

USING_NS_WTP;

namespace r125_sh {

struct OrderReq
{
	std::string code;
	double price = 0.0;
	double qty = 0.0;
	int flag = 0;
	std::string tag;
};

inline WTSTickData* make_tick(const char* code, double price, double bid, double ask,
	double bid_qty, double ask_qty, uint32_t action_time)
{
	WTSTickStruct ts;
	std::strncpy(ts.code, code, sizeof(ts.code) - 1);
	ts.price = price;
	ts.bid_prices[0] = bid;
	ts.ask_prices[0] = ask;
	ts.bid_qty[0] = bid_qty;
	ts.ask_qty[0] = ask_qty;
	ts.action_time = action_time;
	return WTSTickData::create(ts);
}

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

// ------------------------------------------------------------------ SEL

class SelCtx : public ISelStraCtx
{
public:
	SelCtx() : ISelStraCtx("r125_sel")
	{
		session = WTSSessionInfo::create("r125", "r125");
		session->addTradingSection(900, 1500);
	}
	~SelCtx() override
	{
		if (session != NULL)
			session->release();
	}

	WTSKlineSlice* bars = NULL;
	double position = 0.0;
	std::string bars_code;
	uint32_t bars_count = 0;

	std::vector<std::string> set_positions;
	std::vector<std::string> subs;
	std::vector<std::string> logs;
	WTSSessionInfo* session = NULL;

	uint32_t id() override { return 1; }
	void on_init() override {}
	void on_session_begin(uint32_t) override {}
	void on_session_end(uint32_t) override {}
	void on_tick(const char*, WTSTickData*, bool) override {}
	void on_bar(const char*, const char*, uint32_t, WTSBarStruct*) override {}
	bool on_schedule(uint32_t, uint32_t, uint32_t) override { return true; }
	void on_bar_close(const char*, const char*, WTSBarStruct*) override {}
	void enum_position(FuncEnumSelPositionCallBack) override {}

	double stra_get_position(const char*, bool, const char*) override { return position; }
	void stra_set_position(const char* code, double qty, const char* tag) override
	{
		set_positions.push_back(std::string(code) + ":" + std::to_string(qty) + ":" + tag);
	}
	double stra_get_price(const char*) override { return 0.0; }
	double stra_get_day_price(const char*, int) override { return 0.0; }
	uint32_t stra_get_tdate() override { return 20240102; }
	uint32_t stra_get_date() override { return 20240102; }
	uint32_t stra_get_time() override { return 1000; }
	double stra_get_fund_data(int) override { return 0.0; }
	uint64_t stra_get_first_entertime(const char*) override { return 0; }
	uint64_t stra_get_last_entertime(const char*) override { return 0; }
	uint64_t stra_get_last_exittime(const char*) override { return 0; }
	double stra_get_last_enterprice(const char*) override { return 0.0; }
	const char* stra_get_last_entertag(const char*) override { return ""; }
	double stra_get_position_avgpx(const char*) override { return 0.0; }
	double stra_get_position_profit(const char*) override { return 0.0; }
	uint64_t stra_get_detail_entertime(const char*, const char*) override { return 0; }
	double stra_get_detail_cost(const char*, const char*) override { return 0.0; }
	double stra_get_detail_profit(const char*, const char*, int) override { return 0.0; }
	WTSCommodityInfo* stra_get_comminfo(const char*) override { return NULL; }
	WTSSessionInfo* stra_get_sessinfo(const char*) override { return session; }
	WTSKlineSlice* stra_get_bars(const char* code, const char*, uint32_t count) override
	{
		bars_code = code;
		bars_count = count;
		return bars;
	}
	WTSTickSlice* stra_get_ticks(const char*, uint32_t) override { return NULL; }
	WTSTickData* stra_get_last_tick(const char*) override { return NULL; }
	std::string stra_get_rawcode(const char*) override { return ""; }
	void stra_sub_ticks(const char* code) override { subs.push_back(code); }
	void stra_log_info(const char* msg) override { logs.push_back(msg); }
	void stra_log_debug(const char* msg) override { logs.push_back(msg); }
	void stra_log_error(const char* msg) override { logs.push_back(msg); }
};

// ------------------------------------------------------------------ UFT

class UftCtx : public IUftStraCtx
{
public:
	UftCtx() : IUftStraCtx("r125_uft")
	{
		comm = WTSCommodityInfo::create("ag", "ag", "SHFE", "", "");
		comm->setPriceTick(0.5);
	}
	~UftCtx() override
	{
		if (comm != NULL)
			comm->release();
	}

	WTSKlineSlice* bars = NULL;
	double position = 0.0;
	double undone = 0.0;
	uint32_t date = 20240102;
	uint32_t time = 1500;
	uint32_t secs = 30000;

	bool provide_last_tick = true;
	double tick_price = 100.0;
	double tick_bid = 101.0;
	double tick_ask = 102.0;
	double tick_bid_qty = 10.0;
	double tick_ask_qty = 10.0;
	uint32_t tick_time = 150030500;

	OrderIDs buy_ids{ 11 };
	OrderIDs sell_ids{ 12 };
	OrderIDs cancel_all_ids{ 21, 22 };

	std::vector<OrderReq> buys;
	std::vector<OrderReq> sells;
	std::vector<uint32_t> cancels;
	std::vector<std::string> cancel_all_calls;
	std::vector<std::string> subs;
	std::vector<std::string> logs;

	std::map<std::string, uint32_t> u_params;
	std::map<std::string, int32_t> i_params;
	std::map<std::string, double> d_params;
	int watch_calls = 0;
	int commit_calls = 0;

	WTSCommodityInfo* comm = NULL;

	uint32_t id() override { return 1; }
	void on_init() override {}
	void on_tick(const char*, WTSTickData*) override {}
	void on_order_queue(const char*, WTSOrdQueData*) override {}
	void on_order_detail(const char*, WTSOrdDtlData*) override {}
	void on_transaction(const char*, WTSTransData*) override {}

	const char* watch_param(const char*, const char* initVal = "") override { return initVal; }
	double watch_param(const char*, double initVal = 0) override { watch_calls++; return initVal; }
	uint32_t watch_param(const char*, uint32_t initVal = 0) override { watch_calls++; return initVal; }
	int32_t watch_param(const char*, int32_t initVal = 0) override { watch_calls++; return initVal; }
	void commit_param_watcher() override { commit_calls++; }

	const char* read_param(const char*, const char* defVal = "") override { return defVal; }
	double read_param(const char* name, double defVal = 0) override
	{
		auto it = d_params.find(name);
		return it == d_params.end() ? defVal : it->second;
	}
	uint32_t read_param(const char* name, uint32_t defVal = 0) override
	{
		auto it = u_params.find(name);
		return it == u_params.end() ? defVal : it->second;
	}
	int32_t read_param(const char* name, int32_t defVal = 0) override
	{
		auto it = i_params.find(name);
		return it == i_params.end() ? defVal : it->second;
	}

	uint32_t stra_get_date() override { return date; }
	uint32_t stra_get_time() override { return time; }
	uint32_t stra_get_secs() override { return secs; }

	bool stra_cancel(uint32_t localid) override { cancels.push_back(localid); return true; }
	OrderIDs stra_cancel_all(const char* code) override
	{
		cancel_all_calls.push_back(code);
		return cancel_all_ids;
	}
	OrderIDs stra_buy(const char* code, double price, double qty, int flag) override
	{
		buys.push_back(OrderReq{ code, price, qty, flag, "" });
		return buy_ids;
	}
	OrderIDs stra_sell(const char* code, double price, double qty, int flag) override
	{
		sells.push_back(OrderReq{ code, price, qty, flag, "" });
		return sell_ids;
	}

	WTSCommodityInfo* stra_get_comminfo(const char*) override { return comm; }
	WTSKlineSlice* stra_get_bars(const char*, const char*, uint32_t) override { return bars; }
	WTSTickSlice* stra_get_ticks(const char*, uint32_t) override { return NULL; }
	WTSOrdDtlSlice* stra_get_order_detail(const char*, uint32_t) override { return NULL; }
	WTSOrdQueSlice* stra_get_order_queue(const char*, uint32_t) override { return NULL; }
	WTSTransSlice* stra_get_transaction(const char*, uint32_t) override { return NULL; }
	WTSTickData* stra_get_last_tick(const char* code) override
	{
		if (!provide_last_tick)
			return NULL;
		return make_tick(code, tick_price, tick_bid, tick_ask, tick_bid_qty, tick_ask_qty, tick_time);
	}
	double stra_get_position(const char*, bool, int32_t) override { return position; }
	double stra_enum_position(const char*) override { return position; }
	double stra_get_local_position(const char*) override { return position; }
	double stra_get_price(const char*) override { return tick_price; }
	double stra_get_undone(const char*) override { return undone; }
	void stra_sub_ticks(const char* code) override { subs.push_back(code); }
	void stra_sub_order_queues(const char*) override {}
	void stra_sub_order_details(const char*) override {}
	void stra_sub_transactions(const char*) override {}
	void stra_log_info(const char* msg) override { logs.push_back(msg); }
	void stra_log_debug(const char* msg) override { logs.push_back(msg); }
	void stra_log_error(const char* msg) override { logs.push_back(msg); }
};

// ------------------------------------------------------------------ HFT

class HftCtx : public IHftStraCtx
{
public:
	HftCtx() : IHftStraCtx("r125_hft")
	{
		comm = WTSCommodityInfo::create("ag", "ag", "SHFE", "", "");
		comm->setPriceTick(0.5);
	}
	~HftCtx() override
	{
		if (comm != NULL)
			comm->release();
	}

	WTSKlineSlice* bars = NULL;
	double position = 0.0;
	double undone = 0.0;
	uint32_t date = 20240102;
	uint32_t time = 1500;
	uint32_t secs = 30000;

	bool provide_last_tick = true;
	double tick_price = 100.0;
	double tick_bid = 101.0;
	double tick_ask = 102.0;
	double tick_bid_qty = 10.0;
	double tick_ask_qty = 10.0;
	uint32_t tick_time = 150030500;

	OrderIDs buy_ids{ 31 };
	OrderIDs sell_ids{ 32 };
	OrderIDs cancel_ids{ 41, 42 };

	std::vector<OrderReq> buys;
	std::vector<OrderReq> sells;
	std::vector<uint32_t> local_cancels;
	std::vector<std::string> code_cancels;
	std::vector<std::string> subs;
	std::vector<std::string> logs;

	WTSCommodityInfo* comm = NULL;

	uint32_t id() override { return 1; }
	void on_init() override {}
	void on_tick(const char*, WTSTickData*) override {}
	void on_order_queue(const char*, WTSOrdQueData*) override {}
	void on_order_detail(const char*, WTSOrdDtlData*) override {}
	void on_transaction(const char*, WTSTransData*) override {}

	bool stra_cancel(uint32_t localid) override { local_cancels.push_back(localid); return true; }
	OrderIDs stra_cancel(const char* code, bool isBuy, double qty) override
	{
		code_cancels.push_back(std::string(code) + ":" + (isBuy ? "buy" : "sell") + ":" + std::to_string(qty));
		return cancel_ids;
	}
	OrderIDs stra_buy(const char* code, double price, double qty, const char* userTag, int flag, bool) override
	{
		buys.push_back(OrderReq{ code, price, qty, flag, userTag == NULL ? "" : userTag });
		return buy_ids;
	}
	OrderIDs stra_sell(const char* code, double price, double qty, const char* userTag, int flag, bool) override
	{
		sells.push_back(OrderReq{ code, price, qty, flag, userTag == NULL ? "" : userTag });
		return sell_ids;
	}

	WTSCommodityInfo* stra_get_comminfo(const char*) override { return comm; }
	WTSKlineSlice* stra_get_bars(const char*, const char*, uint32_t) override { return bars; }
	WTSTickSlice* stra_get_ticks(const char*, uint32_t) override { return NULL; }
	WTSOrdDtlSlice* stra_get_order_detail(const char*, uint32_t) override { return NULL; }
	WTSOrdQueSlice* stra_get_order_queue(const char*, uint32_t) override { return NULL; }
	WTSTransSlice* stra_get_transaction(const char*, uint32_t) override { return NULL; }
	WTSTickData* stra_get_last_tick(const char* code) override
	{
		if (!provide_last_tick)
			return NULL;
		return make_tick(code, tick_price, tick_bid, tick_ask, tick_bid_qty, tick_ask_qty, tick_time);
	}
	std::string stra_get_rawcode(const char* code) override { return code; }
	double stra_get_position(const char*, bool, int) override { return position; }
	double stra_get_position_avgpx(const char*) override { return 0.0; }
	double stra_get_position_profit(const char*) override { return 0.0; }
	double stra_get_price(const char*) override { return tick_price; }
	double stra_get_undone(const char*) override { return undone; }
	uint32_t stra_get_date() override { return date; }
	uint32_t stra_get_time() override { return time; }
	uint32_t stra_get_secs() override { return secs; }
	void stra_sub_ticks(const char* code) override { subs.push_back(code); }
	void stra_sub_order_queues(const char*) override {}
	void stra_sub_order_details(const char*) override {}
	void stra_sub_transactions(const char*) override {}
	void stra_log_info(const char* msg) override { logs.push_back(msg); }
	void stra_log_debug(const char* msg) override { logs.push_back(msg); }
	void stra_log_error(const char* msg) override { logs.push_back(msg); }
};

// ------------------------------------------------------------ 探针策略

class ForeignSelStrategy : public SelStrategy
{
public:
	ForeignSelStrategy() : SelStrategy("r125_foreign_sel") {}
	const char* getName() override { return "ForeignSel"; }
	const char* getFactName() override { return "OtherSelFact"; }
};

class ForeignUftStrategy : public UftStrategy
{
public:
	ForeignUftStrategy() : UftStrategy("r125_foreign_uft") {}
	const char* getName() override { return "ForeignUft"; }
	const char* getFactName() override { return "OtherUftFact"; }
	void on_init(IUftStraCtx*) override {}
};

class ForeignHftStrategy : public HftStrategy
{
public:
	ForeignHftStrategy() : HftStrategy("r125_foreign_hft") {}
	const char* getName() override { return "ForeignHft"; }
	const char* getFactName() override { return "OtherHftFact"; }
	void on_init(IHftStraCtx*) override {}
};

} // namespace r125_sh
