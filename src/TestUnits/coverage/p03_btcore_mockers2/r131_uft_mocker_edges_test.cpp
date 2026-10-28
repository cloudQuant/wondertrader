/**
 * r131_uft_mocker_edges_test.cpp  (lane R131 · wave21)
 *
 * 靶子：src/WtBtCore/UftMocker.cpp
 *   - procOrder 部分成交后仍有剩余时返回 false（953-969），订单保留到下一 tick；
 *   - 第二笔 tick 把剩余量吃完后返回 true，订单被 erase（964-967）。
 *
 * 纯内存 IBtDataLoader 只提供 askqty=1 的 last tick，委托 2 手 -> 每次最多成交 1 手；
 * maxQty=1 时 splitVolume 走确定性的单元素分支，不触发随机拆单；不起线程。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/UftMocker.h>
#include <WtBtCore/WtHelper.h>

#include "r131_common.h"
#include "../p08_btcore_mockers3/r111_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
}

class R131UftOrder : public ::testing::Test
{
protected:
	HisDataReplayer _rp;
	r111::TickLoader _loader;
	r131::NullSink _sink;
	std::filesystem::path _root;
	std::filesystem::path _base;
	std::filesystem::path _out;

	void SetUp() override
	{
		_root = r62::repo_root();
		ASSERT_FALSE(_root.empty());
		_base = r62::make_temp_dir("r131_uft_base");
		_out = r62::make_temp_dir("r131_uft_out");
		WtHelper::setOutputDir(_out.string().c_str());

		// askqty=1：委托 2 手时单次 tick 只能成交 1 手
		_loader.add(kRb, 20200102u, 93100000u, 3800.0, 3799.0, 3801.0, 100, 1);

		WTSVariant* cfg = r111::make_tick_cfg(_root, _base, 202001020932ull, 202001021500ull);
		ASSERT_TRUE(_rp.init(cfg, nullptr, &_loader));
		cfg->release();
		_rp.register_sink(&_sink, "r131_uft");
		ASSERT_TRUE(_rp.prepare());
	}

	void TearDown() override
	{
		WtHelper::setOutputDir("");
		std::error_code ec;
		std::filesystem::remove_all(_base, ec);
		std::filesystem::remove_all(_out, ec);
	}
};

// 生产: UftMocker.cpp:953-969（部分成交 -> left>0 -> return false）
TEST_F(R131UftOrder, PartialFillKeepsOrderUntilNextTick)
{
	UftMocker m(&_rp, "r131_uft_partial");

	OrderIDs ids = m.stra_buy(kRb, 0.0, 2.0, 0);
	ASSERT_EQ(1u, ids.size());

	// 第一笔 tick：对手量 1 < 委托 2，成交 1 手，剩余 1 手，procOrder 返回 false
	r131::feed(m, kRb, 3800.0, 3799.0, 3801.0);
	EXPECT_DOUBLE_EQ(1.0, m.stra_get_position(kRb));
	EXPECT_DOUBLE_EQ(1.0, m.stra_get_undone(kRb));

	// 第二笔 tick：剩余 1 手成交，left==0 -> 返回 true，订单摘除
	r131::feed(m, kRb, 3800.0, 3799.0, 3801.0);
	EXPECT_DOUBLE_EQ(2.0, m.stra_get_position(kRb));
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_undone(kRb));
}
