/**
 * r111_replayer_notifier_env_test.cpp  (lane R111)
 *
 * 靶子：src/WtBtCore/HisDataReplayer.cpp 剩余公开切片
 *   - EventNotifier 非空时的 BT_START/BT_STATE/BT_END 与 dump_btstate
 *     （L518-555、L557-591、L667、L786、L904、L922-924）
 *   - 无主K线/无 tick 的两条收尾 BT_END（L730、L768）
 *   - 复权后缀订阅（L3097-3101、L3121-3125、L3139-3143、L3157-3161）
 *   - checkUnbars 的「已订阅跳过 / m5 自动加载失败」（L3179、L3190-3192、L3224）
 *
 * notifier 用 active=false 初始化：不创建 WtMsgQue 服务、不联网，
 * notify* 因 _publisher 为空而安全空转；只驱动回放器侧的公开分支。
 * 临时目录由 fixture 建在系统临时目录并在 TearDown 清理。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include <Includes/WTSDataDef.hpp>
#include <WtBtCore/EventNotifier.h>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>

#include "r111_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";

	class R111Sink : public IDataSink
	{
	public:
		uint32_t init_calls = 0;
		uint32_t begin_calls = 0;
		uint32_t end_calls = 0;
		uint32_t bar_calls = 0;
		uint32_t tick_calls = 0;
		uint32_t done_calls = 0;

		void handle_tick(const char*, WTSTickData*, uint32_t) override { tick_calls++; }
		void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct*) override { bar_calls++; }
		void handle_schedule(uint32_t, uint32_t) override {}
		void handle_init() override { init_calls++; }
		void handle_session_begin(uint32_t) override { begin_calls++; }
		void handle_session_end(uint32_t) override { end_calls++; }
		void handle_replay_done() override { done_calls++; }
	};
}

class R111ReplayerEnv : public ::testing::Test
{
protected:
	HisDataReplayer _rp;
	EventNotifier _notifier;
	R111Sink _sink;
	std::filesystem::path _root;
	std::filesystem::path _base;
	std::filesystem::path _out;

	void SetUp() override
	{
		_root = r62::repo_root();
		ASSERT_FALSE(_root.empty());
		_base = r62::make_temp_dir("r111_base");
		_out = r62::make_temp_dir("r111_out");
		WtHelper::setOutputDir(_out.string().c_str());

		ASSERT_TRUE(r62::write_daily_csv(_base, kRb));

		WTSVariant* ncfg = WTSVariant::createObject();
		ncfg->append("active", false);
		EXPECT_FALSE(_notifier.init(ncfg));
		ncfg->release();

		WTSVariant* cfg = r62::make_replay_cfg(_root, _base);
		ASSERT_TRUE(_rp.init(cfg, &_notifier));
		cfg->release();
		_rp.register_sink(&_sink, "r111_notifier");
	}

	void TearDown() override
	{
		WtHelper::setOutputDir("");
		std::error_code ec;
		std::filesystem::remove_all(_base, ec);
		std::filesystem::remove_all(_out, ec);
	}
};

// 主K线回放：BT_START、逐根 BT_STATE、BT_END、dump_btstate 全走 notifier 非空分支
TEST_F(R111ReplayerEnv, NotifierBarRunStateAndDump)
{
	ASSERT_TRUE(_rp.prepare());

	WTSKlineSlice* bars = _rp.get_kline_slice(kRb, "d", 5, 1, true);
	ASSERT_NE(nullptr, bars);
	bars->release();

	_rp.run(true);

	EXPECT_EQ(1u, _sink.init_calls);
	EXPECT_GT(_sink.bar_calls, 0u);
	EXPECT_EQ(1u, _sink.done_calls);
	EXPECT_GE(_sink.begin_calls, 1u);
	EXPECT_GE(_sink.end_calls, 1u);

	const std::filesystem::path btenv = _out / "r111_notifier" / "btenv.json";
	ASSERT_TRUE(std::filesystem::exists(btenv)) << btenv;
}

// 无主K线且 tick 关闭 -> run 的收尾 BT_END（L730）；tick 打开但无数据 ->
// run_by_ticks 的收尾 BT_END（L768），两条都不起线程
TEST_F(R111ReplayerEnv, EmptyRunsNotifyBacktestEnd)
{
	{
		R111Sink sink;
		HisDataReplayer rp;
		WTSVariant* cfg = r62::make_replay_cfg(_root, _base);
		ASSERT_TRUE(rp.init(cfg, &_notifier));
		cfg->release();
		rp.register_sink(&sink, "r111_empty");
		ASSERT_TRUE(rp.prepare());
		rp.run();
		EXPECT_EQ(1u, sink.done_calls);
	}

	{
		R111Sink sink;
		HisDataReplayer rp;
		WTSVariant* cfg = r62::make_replay_cfg(_root, _base);
		cfg->append("tick", true);
		ASSERT_TRUE(rp.init(cfg, &_notifier));
		cfg->release();
		rp.register_sink(&sink, "r111_ticks");
		ASSERT_TRUE(rp.prepare());
		rp.run();
		EXPECT_EQ(1u, sink.done_calls);
	}
}

// 复权后缀订阅 + checkUnbars：prepare 会清掉订阅表，因此先 prepare 再订阅，
// 由 run_by_bars 循环内的 checkUnbars 触发「已订阅跳过 / 无历史数据 continue」
TEST_F(R111ReplayerEnv, AdjustedSuffixSubscriptionsAndCheckUnbars)
{
	ASSERT_TRUE(_rp.prepare());

	// 先让 kRb 进入 _codes_in_subbed
	WTSKlineSlice* daily = _rp.get_kline_slice(kRb, "d", 5);
	ASSERT_NE(nullptr, daily);
	daily->release();

	_rp.sub_tick(1, kRb);
	_rp.sub_tick(2, "SHFE.ag.2412");	// 无日线 CSV，checkUnbars 必然 continue

	// SUFFIX_QFQ='-' / SUFFIX_HFQ='+'（Share/CodeHelper.hpp:28-34）
	_rp.sub_tick(3, "SHFE.rb.2405-");
	_rp.sub_tick(4, "SHFE.rb.2405+");
	_rp.sub_order_detail(5, "SHFE.rb.2405-");
	_rp.sub_order_detail(6, "SHFE.rb.2405+");
	_rp.sub_order_queue(7, "SHFE.rb.2405-");
	_rp.sub_order_queue(8, "SHFE.rb.2405+");
	_rp.sub_transaction(9, "SHFE.rb.2405-");
	_rp.sub_transaction(10, "SHFE.rb.2405+");

	// 主K线必须有数据，run_by_bars 才会进入循环并调用 checkUnbars
	WTSKlineSlice* mainBars = _rp.get_kline_slice(kRb, "d", 5, 1, true);
	ASSERT_NE(nullptr, mainBars);
	mainBars->release();

	_rp.run();
	EXPECT_EQ(1u, _sink.done_calls);
}
