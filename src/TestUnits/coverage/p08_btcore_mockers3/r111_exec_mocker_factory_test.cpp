/**
 * r111_exec_mocker_factory_test.cpp  (lane R111)
 *
 * 靶子：src/WtBtCore/ExecMocker.cpp
 *   init 成功路径（L71-85）：真实构建产物 libWtExeFact.dylib + WtMinImpactExeUnit
 *   handle_init（L140-161）、handle_schedule 三种 volmode（L163-192）
 *   handle_entrust/handle_order/handle_trade（L285-335）与 handle_replay_done（L337-347）
 *
 * 此前 n04/r20/r92 只覆盖「模块缺失」分支，_exec_unit 非空路径整族留白。
 * 本文件通过 ExecMocker::init 的公开配置加载本机构建产物插件（不联网、不起线程），
 * 执行单元只做纯内存撮合回报，不接真实柜台。
 *
 * 不使用 #define private public；临时文件由 r62 fixture 写入系统临时目录并清理。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <WtBtCore/ExecMocker.h>
#include <WtBtCore/WtHelper.h>

#include "r111_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";

	WTSVariant* make_exec_cfg(const std::string& module, const char* id,
		double volunit, int32_t volmode)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("module", module.c_str());
		cfg->append("code", kRb);
		cfg->append("period", "m1");
		cfg->append("volunit", volunit);
		cfg->append("volmode", volmode);

		WTSVariant* exec = WTSVariant::createObject();
		exec->append("name", "WtMinImpactExeUnit");
		exec->append("id", id);

		WTSVariant* params = WTSVariant::createObject();
		params->append("offset", static_cast<int32_t>(0));
		params->append("expire", static_cast<uint32_t>(0));
		params->append("pricemode", static_cast<int32_t>(0));
		params->append("span", static_cast<uint32_t>(0));
		params->append("byrate", false);
		params->append("lots", 1.0);
		params->append("rate", 1.0);
		params->append("minopenlots", 1.0);
		exec->append("params", params);
		params->release();

		cfg->append("executer", exec);
		exec->release();
		return cfg;
	}

	std::string read_text(const std::filesystem::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		if (!in.is_open())
			return std::string();
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	uint32_t line_count(const std::string& content)
	{
		if (content.empty())
			return 0;
		uint32_t lines = 0;
		for (char c : content)
		{
			if (c == '\n')
				lines++;
		}
		if (content.back() != '\n')
			lines++;
		return lines;
	}
}

class R111ExecMocker : public r62::ReplayFixture
{
protected:
	std::string module() const
	{
		return r111::plugin_path("WtExeFact");
	}
};

// init 成功（L71-85）→ handle_init（L140-161）→ 15:00 早退与正常调度（L163-192）
TEST_F(R111ExecMocker, InitWithRealExecuterAndInitSchedule)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	ExecMocker m(&_rp);
	WTSVariant* cfg = make_exec_cfg(module(), "r111_exec_init", 0.0, 0);
	ASSERT_TRUE(m.init(cfg));
	cfg->release();

	// 真实插件装载后商品/时段查询仍透传回放器
	ASSERT_NE(nullptr, m.getCommodityInfo(kRb));
	ASSERT_NE(nullptr, m.getSessionInfo(kRb));
	EXPECT_TRUE(m.getOrders(kRb) == nullptr);

	// handle_init：K 线预取 + sub_tick + 表头 + on_channel_ready + set_position(0)
	m.handle_init();
	EXPECT_DOUBLE_EQ(0.0, m.getPosition(kRb));
	EXPECT_DOUBLE_EQ(0.0, m.getUndoneQty(kRb));

	// 15:00 早退：没有 tick 也不能解引用 _last_tick
	m.handle_schedule(20240103u, 1500u);
	EXPECT_DOUBLE_EQ(0.0, m.getPosition(kRb));

	// 正常调度：volmode=0 且 position<=0 → target=volunit(0)，do_calc 在同值处返回
	WTSTickData* t1 = r62::make_tick(kRb, 20240103u, 142500000u, 3800.0, 3799.0, 3801.0);
	m.handle_tick(kRb, t1, 0);
	t1->release();
	m.handle_schedule(20240103u, 931u);
	m.handle_schedule(20240103u, 932u);
	EXPECT_DOUBLE_EQ(0.0, m.getPosition(kRb));
}

// IMatchSink 三回调（L285-335）与落盘（L337-347）：撤销/成交各写一条 csv
TEST_F(R111ExecMocker, EntrustOrderTradeAndReplayDump)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	ExecMocker m(&_rp);
	WTSVariant* cfg = make_exec_cfg(module(), "r111_exec_trade", 1.0, 0);
	ASSERT_TRUE(m.init(cfg));
	cfg->release();
	m.handle_init();

	WTSTickData* t1 = r62::make_tick(kRb, 20240103u, 142500000u, 3800.0, 3799.0, 3801.0, 5, 7);
	m.handle_tick(kRb, t1, 0);
	t1->release();

	const uint64_t ordTime = (uint64_t)20240103 * 1000000000ull + 142500000ull;

	// entrust 成功/失败都只转发给执行单元，不影响账户
	m.handle_entrust(1u, kRb, true, "ok", ordTime);
	m.handle_entrust(2u, kRb, false, "rejected", ordTime);
	EXPECT_DOUBLE_EQ(0.0, m.getPosition(kRb));

	// 撤单回报：leftover 回写 _undone 且写 cancel=true 记录
	m.handle_order(10u, kRb, true, 2.0, 3799.0, true, ordTime);
	EXPECT_DOUBLE_EQ(-2.0, m.getUndoneQty(kRb));

	// 成交回报：买入 2 → 卖出 1，持仓净 1
	m.handle_trade(11u, kRb, true, 2.0, 3800.0, 3800.0, ordTime);
	EXPECT_DOUBLE_EQ(2.0, m.getPosition(kRb));
	m.handle_order(12u, kRb, false, 1.0, 3801.0, false, ordTime);
	EXPECT_DOUBLE_EQ(2.0, m.getPosition(kRb));
	m.handle_trade(13u, kRb, false, 1.0, 3801.0, 3801.0, ordTime);
	EXPECT_DOUBLE_EQ(1.0, m.getPosition(kRb));

	// 第二笔撤单回报
	m.handle_order(14u, kRb, true, 1.0, 3800.0, true, ordTime);

	m.handle_replay_done();

	const std::filesystem::path csv = _out / "exec" / "trades_r111_exec_trade.csv";
	ASSERT_TRUE(std::filesystem::exists(csv)) << csv;
	const std::string content = read_text(csv);
	// 表头 + 撤单(10) + 买(11) + 卖(13) + 撤单(14)
	EXPECT_EQ(5u, line_count(content));
	EXPECT_NE(std::string::npos, content.find(
		"localid,signaltime,ordertime,bs,sigprice,ordprice,lmtprice,tradetime,trdprice,qty,sigtimespan,exectime,cancel"));
	EXPECT_NE(std::string::npos, content.find(",true\n"));
	EXPECT_NE(std::string::npos, content.find(",false\n"));
}

// volmode=-1/+1 的 target 累计分支（L180-187）
TEST_F(R111ExecMocker, ScheduleNegativeAndPositiveVolumeModes)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	{
		ExecMocker m(&_rp);
		WTSVariant* cfg = make_exec_cfg(module(), "r111_exec_sell", 0.0, -1);
		ASSERT_TRUE(m.init(cfg));
		cfg->release();
		WTSTickData* t = r62::make_tick(kRb, 20240103u, 142500000u, 3800.0, 3799.0, 3801.0);
		m.handle_tick(kRb, t, 0);
		t->release();
		m.handle_schedule(20240103u, 931u);
		m.handle_schedule(20240103u, 932u);
		EXPECT_DOUBLE_EQ(0.0, m.getPosition(kRb));
	}

	{
		ExecMocker m(&_rp);
		WTSVariant* cfg = make_exec_cfg(module(), "r111_exec_buy", 0.0, 1);
		ASSERT_TRUE(m.init(cfg));
		cfg->release();
		WTSTickData* t = r62::make_tick(kRb, 20240103u, 142500000u, 3800.0, 3799.0, 3801.0);
		m.handle_tick(kRb, t, 0);
		t->release();
		m.handle_schedule(20240103u, 931u);
		m.handle_schedule(20240103u, 932u);
		EXPECT_DOUBLE_EQ(0.0, m.getPosition(kRb));
	}
}
