/**
 * r111_uft_mocker_plugin_test.cpp  (lane R111)
 *
 * 靶子：src/WtBtCore/UftMocker.cpp
 *   init_uft_factory 成功路径（L154-166）：本机构建产物 WtUftRunner/libWtUftStraFact.dylib
 *     + 演示策略 SimpleUft
 *   析构删除策略（L62-64）、handle_session_begin/end（L206-213）
 *   事件分发：on_bar/on_tick/on_order_queue/on_order_detail/on_transaction/on_init
 *     （L225、L296、L307、L318、L329、L340）
 *   on_session_begin 冻结释放 / on_session_end 持仓与资金日志（L344-390）
 *   IMatchSink 回调（L804、L813、L819、L825）、stra_enum_position（L1031-1047）
 *   update_position 未注册品种早退（L1170-1172）
 *
 * R92 因不加载策略 DLL 整族留白；本 lane 加载本地插件，不联网、不起线程。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include <Includes/WTSContractInfo.hpp>
#include <WtBtCore/UftMocker.h>
#include <WtBtCore/WtHelper.h>

#include "r111_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
	const char* const kCu = "SHFE.cu.2406";
	const char* const kStk = "SSE.STK.000001";

	WTSVariant* make_factory_cfg(const std::string& module, const char* code)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("module", module.c_str());

		WTSVariant* stra = WTSVariant::createObject();
		stra->append("name", "SimpleUft");
		stra->append("id", "r111_uft_plug");

		WTSVariant* params = WTSVariant::createObject();
		params->append("code", code);
		params->append("second", static_cast<uint32_t>(10));
		params->append("freq", static_cast<uint32_t>(0));
		params->append("offset", static_cast<uint32_t>(0));
		params->append("lots", 1.0);
		stra->append("params", params);
		params->release();

		cfg->append("strategy", stra);
		stra->release();
		return cfg;
	}
}

class R111UftMocker : public r62::ReplayFixture
{
protected:
	std::string module() const
	{
		return r111::plugin_path("WtUftStraFact", "WtUftRunner");
	}
};

// 工厂装载 + on_init + 六类事件分发 + IMatchSink 回调 + 析构删策略
TEST_F(R111UftMocker, FactoryCreateAndEventDispatch)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	UftMocker mocker(&_rp, "r111_uft_dispatch");
	WTSVariant* cfg = make_factory_cfg(module(), kRb);
	ASSERT_TRUE(mocker.init_uft_factory(cfg));
	cfg->release();

	// on_init：watch_param/commit/stra_get_bars/stra_sub_ticks（L340）
	mocker.handle_init();
	// handle_session_begin/end 转发（L206-213）
	mocker.handle_session_begin(20200102u);
	mocker.handle_session_end(20200102u);

	// 订阅后分发 tick（L296）
	mocker.stra_sub_ticks(kCu);
	WTSTickData* tick = r62::make_tick(kCu, 20200102u, 93100000u, 70000.0, 69990.0, 70010.0);
	mocker.handle_tick(kCu, tick, 0);
	tick->release();

	// Level2 三类分发（L307/318/329）
	mocker.handle_order_queue(kCu, nullptr);
	mocker.handle_order_detail(kCu, nullptr);
	mocker.handle_transaction(kCu, nullptr);

	// 日线闭合（L225）
	WTSBarStruct bar = r62::make_bar(20200102u, 1500u, 10.0);
	mocker.handle_bar_close(kCu, "m1", 1, &bar);

	// IMatchSink 回调（L819/804/813/825）
	mocker.on_entrust(1u, kCu, true, "ok");
	mocker.on_order(2u, kCu, true, 0u, 1.0, 1.0, 70000.0, false);
	mocker.on_trade(3u, kCu, true, 0u, 1.0, 70000.0);
	mocker.on_channel_ready();

	EXPECT_DOUBLE_EQ(1.0, mocker.stra_get_position(kCu));
}

// 交易日开始释放冻结、结束写持仓/资金日志；未注册品种成交早退
TEST_F(R111UftMocker, SessionFreezeReleaseEnumAndUnknownCommodity)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	WTSCommodityInfo* stkComm = _rp.get_commodity_info(kStk);
	ASSERT_NE(nullptr, stkComm);
	stkComm->setTradingMode(TM_LongT1);

	UftMocker mocker(&_rp, "r111_uft_pos");
	WTSVariant* cfg = make_factory_cfg(module(), kRb);
	ASSERT_TRUE(mocker.init_uft_factory(cfg));
	cfg->release();

	// 开 100 股 T+1：今仓可用为 0
	mocker.on_trade(1u, kStk, true, 0u, 100.0, 10.0);
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position(kStk, false));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kStk, true));

	// 跨日释放冻结（L350-358），随后枚举持仓（L1031-1047）
	mocker.handle_session_begin(20200103u);
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position(kStk, true));
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_enum_position(kStk));

	// 未注册品种：update_position 在 commInfo==NULL 处早退（L1170-1172）
	mocker.on_trade(2u, "SHFE.xx.9999", true, 0u, 1.0, 100.0);
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position("SHFE.xx.9999"));

	// 会话结束：遍历非零持仓写 positions/funds（L364-390）
	mocker.handle_session_end(20200103u);
	mocker.handle_replay_done();

	const std::filesystem::path dir = _out / "r111_uft_pos";
	ASSERT_TRUE(std::filesystem::exists(dir / "trades.csv"));
	ASSERT_TRUE(std::filesystem::exists(dir / "positions.csv"));
	ASSERT_TRUE(std::filesystem::exists(dir / "funds.csv"));
}
