/**
 * r111_hft_mocker_match_edge_test.cpp  (lane R111)
 *
 * 靶子：src/WtBtCore/HftMocker.cpp 撮合早退与撤单残余
 *   stra_cancel 任务体（L545-570，重点 L561）
 *   procOrder 对手量为 0（L731-732）、对手价为 0（L739-742）、
 *   卖限价高于买一价（L755-759）
 *
 * 用纯内存 IBtDataLoader 给回放器提供可编程 tick，使 procOrder 能走到
 * stra_get_last_tick 之后的分支；不起线程，不接真实柜台。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include <WtBtCore/HftMocker.h>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>

#include "r111_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
	const char* const kCu = "SHFE.cu.2406";
	const char* const kAg = "SHFE.ag.2412";
	const char* const kHc = "SHFE.hc.2505";

	class R111HftSink : public IDataSink
	{
	public:
		void handle_tick(const char*, WTSTickData*, uint32_t) override {}
		void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct*) override {}
		void handle_schedule(uint32_t, uint32_t) override {}
		void handle_init() override {}
		void handle_session_begin(uint32_t) override {}
		void handle_session_end(uint32_t) override {}
	};
}

class R111HftMocker : public ::testing::Test
{
protected:
	HisDataReplayer _rp;
	r111::TickLoader _loader;
	R111HftSink _sink;
	std::filesystem::path _root;
	std::filesystem::path _base;
	std::filesystem::path _out;

	void SetUp() override
	{
		_root = r62::repo_root();
		ASSERT_FALSE(_root.empty());
		_base = r62::make_temp_dir("r111_hft_base");
		_out = r62::make_temp_dir("r111_hft_out");
		WtHelper::setOutputDir(_out.string().c_str());

		// 回放起点设在 tick 之后，确保 get_last_tick 能定位到该笔 tick
		_loader.add(kRb, 20200102u, 93100000u, 3800.0, 3799.0, 3801.0, 100, 100);
		_loader.add(kCu, 20200102u, 93100000u, 70000.0, 69990.0, 70010.0, 100, 0);
		_loader.add(kAg, 20200102u, 93100000u, 3500.0, 3499.0, 0.0, 100, 100);
		_loader.add(kHc, 20200102u, 93100000u, 3500.0, 3500.0, 3501.0, 100, 100);

		WTSVariant* cfg = r111::make_tick_cfg(_root, _base,
			202001020932ull, 202001021500ull);
		ASSERT_TRUE(_rp.init(cfg, nullptr, &_loader));
		cfg->release();
		_rp.register_sink(&_sink, "r111_hft");
		ASSERT_TRUE(_rp.prepare());
	}

	void TearDown() override
	{
		WtHelper::setOutputDir("");
		std::error_code ec;
		std::filesystem::remove_all(_base, ec);
		std::filesystem::remove_all(_out, ec);
	}

	void feed(HftMocker& m, const char* code, double last, double bid, double ask,
		uint32_t bidQty = 100, uint32_t askQty = 100)
	{
		WTSTickData* t = r62::make_tick(code, 20200102u, 93100000u, last, bid, ask, bidQty, askQty);
		m.handle_tick(code, t, 0);
		t->release();
	}

	void cancel_and_drain(HftMocker& m, uint32_t id, const char* code,
		double last, double bid, double ask, uint32_t bidQty = 100, uint32_t askQty = 100)
	{
		EXPECT_TRUE(m.stra_cancel(id));
		feed(m, code, last, bid, ask, bidQty, askQty);
	}
};

// 撤单任务体 + 三种 procOrder 早退
TEST_F(R111HftMocker, CancelTaskAndProcOrderGuards)
{
	HftMocker m(&_rp, "r111_hft_edges");

	// (1) 买限价低于卖一价 -> procOrder 不成交；随后撤单任务在下一 tick 排空
	OrderIDs rb = m.stra_buy(kRb, 3790.0, 1.0, "limit");
	ASSERT_EQ(1u, rb.size());
	feed(m, kRb, 3800.0, 3799.0, 3801.0);
	EXPECT_DOUBLE_EQ(1.0, m.stra_get_undone(kRb));
	cancel_and_drain(m, rb[0], kRb, 3800.0, 3799.0, 3801.0);
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_undone(kRb));

	// (2) 对手量 askqty=0 -> L731-732 早退
	OrderIDs cu = m.stra_buy(kCu, 0.0, 1.0, "zeroqty");
	ASSERT_EQ(1u, cu.size());
	feed(m, kCu, 70000.0, 69990.0, 70010.0, 100, 0);
	EXPECT_DOUBLE_EQ(1.0, m.stra_get_undone(kCu));
	cancel_and_drain(m, cu[0], kCu, 70000.0, 69990.0, 70010.0, 100, 0);
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_undone(kCu));

	// (3) _use_newpx=false 且 askprice=0 -> L739-742 早退
	OrderIDs ag = m.stra_buy(kAg, 3900.0, 1.0, "zeropx");
	ASSERT_EQ(1u, ag.size());
	feed(m, kAg, 3500.0, 3499.0, 0.0, 100, 100);
	EXPECT_DOUBLE_EQ(1.0, m.stra_get_undone(kAg));
	cancel_and_drain(m, ag[0], kAg, 3500.0, 3499.0, 0.0);
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_undone(kAg));

	// (4) 卖限价 3600 高于买一价 3500 -> L755-759 早退
	OrderIDs hc = m.stra_sell(kHc, 3600.0, 1.0, "selllimit");
	ASSERT_EQ(1u, hc.size());
	feed(m, kHc, 3500.0, 3500.0, 3501.0);
	EXPECT_DOUBLE_EQ(-1.0, m.stra_get_undone(kHc));
	cancel_and_drain(m, hc[0], kHc, 3500.0, 3500.0, 3501.0);
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_undone(kHc));
}
