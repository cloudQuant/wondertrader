/**
 * r121_hft_mocker_test.cpp  (lane R121)
 *
 * 靶子：src/WtBtCore/HftMocker.cpp
 *   - stra_cancel 任务体（L548-570，核心 L561）：置 left=0、回报撤单、erase；
 *   - procOrder 的三条早退：对手量 0（L731-732）、_use_newpx=false 且对手价 0
 *     （L739-742）、卖限价高于买一价（L755-759）。
 *
 * 用纯内存 IBtDataLoader 给回放器提供可编程 tick，使 procOrder 能走到
 * stra_get_last_tick 之后的分支；不起线程、不 sleep、不读挂钟（订单量 1，
 * splitVolume 走 vol<=minQty 的确定分支）。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include <WtBtCore/HftMocker.h>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>

#include "r121_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
	const char* const kCu = "SHFE.cu.2406";
	const char* const kAg = "SHFE.ag.2412";
	const char* const kHc = "SHFE.hc.2505";
}

class R121HftMocker : public ::testing::Test
{
protected:
	HisDataReplayer _rp;
	r111::TickLoader _loader;
	r121::NullSink _sink;
	std::filesystem::path _root;
	std::filesystem::path _base;
	std::filesystem::path _out;

	void SetUp() override
	{
		_root = r62::repo_root();
		ASSERT_FALSE(_root.empty());
		_base = r62::make_temp_dir("r121_hft_base");
		_out = r62::make_temp_dir("r121_hft_out");
		WtHelper::setOutputDir(_out.string().c_str());

		_loader.add(kRb, 20200102u, 93100000u, 3800.0, 3799.0, 3801.0, 100, 100);
		_loader.add(kCu, 20200102u, 93100000u, 70000.0, 69990.0, 70010.0, 100, 0);
		_loader.add(kAg, 20200102u, 93100000u, 3500.0, 3499.0, 0.0, 100, 100);
		_loader.add(kHc, 20200102u, 93100000u, 3500.0, 3500.0, 3501.0, 100, 100);

		WTSVariant* cfg = r111::make_tick_cfg(_root, _base,
			202001020932ull, 202001021500ull);
		ASSERT_TRUE(_rp.init(cfg, nullptr, &_loader));
		cfg->release();
		_rp.register_sink(&_sink, "r121_hft");
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
};

// 撤单任务体：买限价低于卖一价 -> 不成交；下一 tick 的任务排空把单删掉
TEST_F(R121HftMocker, CancelTaskDrainsAndErasesOrder)
{
	HftMocker m(&_rp, "r121_hft_cancel");

	OrderIDs ids = m.stra_buy(kRb, 3790.0, 1.0, "limit");
	ASSERT_EQ(1u, ids.size());
	feed(m, kRb, 3800.0, 3799.0, 3801.0);
	EXPECT_DOUBLE_EQ(1.0, m.stra_get_undone(kRb));

	EXPECT_TRUE(m.stra_cancel(ids[0]));
	feed(m, kRb, 3800.0, 3799.0, 3801.0);
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_undone(kRb));
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_position(kRb));
}

// 三条 procOrder 早退：askqty=0、askprice=0、卖限价高于买一价
TEST_F(R121HftMocker, ProcOrderZeroQtyZeroPriceAndSellLimit)
{
	HftMocker m(&_rp, "r121_hft_guards");

	// 对手量 0（L731-732）
	OrderIDs cu = m.stra_buy(kCu, 0.0, 1.0, "zeroqty");
	ASSERT_EQ(1u, cu.size());
	feed(m, kCu, 70000.0, 69990.0, 70010.0, 100, 0);
	EXPECT_DOUBLE_EQ(1.0, m.stra_get_undone(kCu));

	// _use_newpx=false 且 askprice=0（L739-742）
	OrderIDs ag = m.stra_buy(kAg, 3900.0, 1.0, "zeropx");
	ASSERT_EQ(1u, ag.size());
	feed(m, kAg, 3500.0, 3499.0, 0.0, 100, 100);
	EXPECT_DOUBLE_EQ(1.0, m.stra_get_undone(kAg));

	// 卖限价 3600 高于买一价 3500（L755-759）
	OrderIDs hc = m.stra_sell(kHc, 3600.0, 1.0, "selllimit");
	ASSERT_EQ(1u, hc.size());
	feed(m, kHc, 3500.0, 3500.0, 3501.0);
	EXPECT_DOUBLE_EQ(-1.0, m.stra_get_undone(kHc));

	// 排空三笔挂单的任务，避免析构时任务仍引用本对象
	EXPECT_TRUE(m.stra_cancel(cu[0]));
	EXPECT_TRUE(m.stra_cancel(ag[0]));
	EXPECT_TRUE(m.stra_cancel(hc[0]));
	feed(m, kHc, 3500.0, 3500.0, 3501.0);
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_undone(kCu));
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_undone(kAg));
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_undone(kHc));
}
