/**
 * r121_cta_mocker_test.cpp  (lane R121)
 *
 * 靶子：src/WtBtCore/CtaMocker.cpp
 *   - init_cta_factory 成功段（L433-451）与策略回调 on_init/on_bar/on_tick/
 *     session begin/end（L968、L1024、L1034、L1238、L1267）；
 *     使用本机构建产物 libWtCtaStraFact.dylib + DualThrust；
 *   - handle_session_end 的资金通知（L1292-1294）；
 *   - handle_replay_done 的持久化 JSON（dump_stradata/dump_chartdata 的
 *     _persist_data 块，L208-225、L310-326）；
 *   - do_set_position 比率滑点（L1637-1651）。
 *
 * R111 的 Cta 用例在 stra_get_bars/调度段崩溃；本文件不调用 stra_get_bars、
 * handle_schedule，避开该崩溃点。不联网、不起线程。
 */
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>

#include <Includes/WTSContractInfo.hpp>
#include <WtBtCore/CtaMocker.h>
#include <WtBtCore/EventNotifier.h>
#include <WtBtCore/WtHelper.h>

#include "r121_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";

	WTSVariant* make_cta_stra_cfg(const std::string& module)
	{
		WTSVariant* cfg = r121::make_module_cfg(module);

		WTSVariant* stra = WTSVariant::createObject();
		stra->append("name", "DualThrust");
		stra->append("id", "r121_cta_plug");

		WTSVariant* params = WTSVariant::createObject();
		params->append("days", static_cast<uint32_t>(2));
		params->append("k1", 0.5);
		params->append("k2", 0.5);
		params->append("period", "d");
		params->append("count", static_cast<uint32_t>(5));
		params->append("code", kRb);
		params->append("stock", false);
		stra->append("params", params);
		params->release();

		cfg->append("strategy", stra);
		stra->release();
		return cfg;
	}
}

class R121CtaMocker : public r62::ReplayFixture
{
protected:
	std::string module() const
	{
		return r111::plugin_path("WtCtaStraFact");
	}

	void feed(CtaMocker& m, const char* code, double px)
	{
		WTSTickData* t = r62::make_tick(code, 20200102u, 93100000u, px, px - 1.0, px + 1.0);
		m.handle_tick(code, t, 0);
		t->release();
	}
};

// 真实策略插件：工厂创建 + on_init + bar/tick 分发 + session 回调
TEST_F(R121CtaMocker, PluginStrategyEventDispatch)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	CtaMocker m(&_rp, "r121_cta_plugin", 0, false);
	WTSVariant* cfg = make_cta_stra_cfg(module());
	fprintf(stderr, "C1 factory\n");
	ASSERT_TRUE(m.init_cta_factory(cfg));
	cfg->release();
	fprintf(stderr, "C2 handle_init\n");

	// on_init（L968）：策略基类空实现；不取 K 线，避开 R111 崩溃点
	m.handle_init();
	fprintf(stderr, "C3 session_begin\n");

	// session begin/end 转发（L1238、L1267）
	m.handle_session_begin(20200102u);
	fprintf(stderr, "C4 session_end\n");
	m.handle_session_end(20200103u);
	fprintf(stderr, "C5 sub ticks\n");

	// tick 订阅 + 分发（L1034）
	m.stra_sub_ticks(kRb);
	feed(m, kRb, 3800.0);
	fprintf(stderr, "C6 bar events\n");

	// bar 事件订阅 + 闭合分发（L1024）
	m.stra_sub_bar_events(kRb, "d1");
	WTSBarStruct bar = r62::make_bar(20200102u, 1500u, 3800.0);
	m.handle_bar_close(kRb, "d", 1, &bar);
	fprintf(stderr, "C7 replay done\n");

	m.handle_replay_done();
	fprintf(stderr, "C8 end\n");

	// 全程没有信号，账户不变
	fprintf(stderr, "D6 pos\n");
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_position(kRb));
	fprintf(stderr, "D7 done\n");
}

// _persist_data=true：dump_stradata/dump_chartdata/dump_outputs 落盘；
// notifier 非空时 session_end 走 notifyFund（L1292-1294）
TEST_F(R121CtaMocker, PersistDumpsAndFundNotification)
{
	fprintf(stderr, "D1 notifier\n");
	EventNotifier notifier;
	fprintf(stderr, "D2 mocker\n");
	CtaMocker m(&_rp, "r121_cta_persist", 0, true, &notifier);
	fprintf(stderr, "D3 session_end\n");

	m.handle_session_end(20240102u);
	fprintf(stderr, "D4 replay_done\n");
	m.handle_replay_done();
	fprintf(stderr, "D5 dump done\n");

	const std::filesystem::path dir = _out / "r121_cta_persist";
	const std::string stradata = r62::read_file(dir / "r121_cta_persist.json");
	ASSERT_FALSE(stradata.empty());
	EXPECT_NE(std::string::npos, stradata.find("\"positions\""));
	EXPECT_NE(std::string::npos, stradata.find("\"fund\""));
	EXPECT_FALSE(r62::read_file(dir / "btchart.json").empty());
	EXPECT_FALSE(r62::read_file(dir / "funds.csv").empty());
	EXPECT_FALSE(r62::read_file(dir / "signals.csv").empty());
}

// 比率滑点：fill = 3800 + round(10*3800/10000/pricetick)*pricetick（L1637-1651）
TEST_F(R121CtaMocker, RatioSlippageFill)
{
	WTSCommodityInfo* comm = _rp.get_commodity_info(kRb);
	ASSERT_NE(nullptr, comm);
	const double price_tick = comm->getPriceTick();
	ASSERT_GT(price_tick, 0.0);

	CtaMocker m(&_rp, "r121_cta_ratio", 10, false, nullptr, true);
	m.stra_set_position(kRb, 1.0, "ratio");
	feed(m, kRb, 3800.0);

	ASSERT_DOUBLE_EQ(1.0, m.stra_get_position(kRb));
	const double expected_slip = std::round(10.0 * 3800.0 / 10000.0 / price_tick) * price_tick;
	EXPECT_DOUBLE_EQ(3800.0 + expected_slip, m.stra_get_last_enterprice(kRb));

	// on_tick 空实现（L1017-1019）
	m.on_tick(kRb, nullptr, true);
}
