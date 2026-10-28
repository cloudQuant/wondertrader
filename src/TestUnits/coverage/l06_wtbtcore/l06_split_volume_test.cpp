/**
 * l06_split_volume_test.cpp
 * 覆盖 src/WtBtCore/HftMocker.cpp 中的全局工具函数:
 * splitVolume(uint32_t) / splitVolume(double, min, max, tick) / genRand(uint32_t),
 * 这些函数同时被 UftMocker / HftMocker 下单拆单逻辑复用.
 * 内部使用 rand() 但断言均为不依赖随机顺序的确定性不变量(总量守恒、区间约束).
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

// HftMocker.cpp 中的定义, 测试声明不带默认参数, 调用处显式给全
extern std::vector<uint32_t> splitVolume(uint32_t vol);
extern std::vector<double> splitVolume(double vol, double minQty, double maxQty, double qtyTick);
extern uint32_t genRand(uint32_t maxVal);

namespace
{
template <typename T>
T sum_of(const std::vector<T>& v)
{
	T s = 0;
	for (const T& x : v)
		s += x;
	return s;
}
}

TEST(l06_split_volume, uint_zero_returns_empty)
{
	std::vector<uint32_t> ret = splitVolume(0);
	EXPECT_TRUE(ret.empty());
}

TEST(l06_split_volume, uint_minimal_returns_single)
{
	std::vector<uint32_t> ret = splitVolume(1);
	ASSERT_EQ(1u, ret.size());
	EXPECT_EQ(1u, ret[0]);
}

TEST(l06_split_volume, uint_invariants_sum_and_bounds)
{
	const uint32_t vol = 253u;
	std::vector<uint32_t> ret = splitVolume(vol);
	EXPECT_EQ(vol, sum_of(ret));
	// 单笔区间 [1, 100]
	for (uint32_t x : ret)
	{
		EXPECT_GE(x, 1u);
		EXPECT_LE(x, 100u);
	}
	// 253 至少拆成 3 笔 (maxQty=100)
	EXPECT_GE(ret.size(), 3u);
	// 除最后一笔外, 每笔都达到了 minQty, 且最后一笔是剩余量
	for (std::size_t i = 0; i + 1 < ret.size(); i++)
		EXPECT_GE(ret[i], 1u);
	EXPECT_LT(sum_of(std::vector<uint32_t>(ret.begin(), ret.end() - 1)), vol);
}

TEST(l06_split_volume, uint_exact_max_chunk_boundary)
{
	// 100 可能一笔也可能拆多笔, 但总量守恒且不超过 100/笔
	std::vector<uint32_t> ret = splitVolume(100);
	EXPECT_EQ(100u, sum_of(ret));
	for (uint32_t x : ret)
		EXPECT_LE(x, 100u);
}

TEST(l06_split_volume, double_zero_returns_single_zero)
{
	// vol(0) <= minQty(1) → 直接返回 {0}
	std::vector<double> ret = splitVolume(0.0, 1.0, 100.0, 1.0);
	ASSERT_EQ(1u, ret.size());
	EXPECT_DOUBLE_EQ(0.0, ret[0]);
}

TEST(l06_split_volume, double_below_min_returns_single)
{
	std::vector<double> ret = splitVolume(0.5, 1.0, 100.0, 1.0);
	ASSERT_EQ(1u, ret.size());
	EXPECT_DOUBLE_EQ(0.5, ret[0]);
}

TEST(l06_split_volume, double_sum_conserved_with_qty_tick)
{
	const double vol = 50.0;
	const double minQty = 2.0, maxQty = 10.0, qtyTick = 2.0;
	std::vector<double> ret = splitVolume(vol, minQty, maxQty, qtyTick);
	EXPECT_NEAR(vol, sum_of(ret), 1e-9);
	for (std::size_t i = 0; i < ret.size(); i++)
	{
		EXPECT_GT(ret[i], 0.0);
		if (i + 1 < ret.size())
		{
			// 非末笔必须落在 [minQty, maxQty] 且对齐到 qtyTick 网格
			EXPECT_GE(ret[i], minQty);
			EXPECT_LE(ret[i], maxQty);
			double grid = std::fmod(ret[i] - minQty, qtyTick);
			EXPECT_NEAR(0.0, std::min(grid, qtyTick - grid), 1e-9);
		}
		else
		{
			EXPECT_LE(ret[i], maxQty);
		}
	}
}

TEST(l06_split_volume, double_single_lot_when_volume_equals_min)
{
	std::vector<double> ret = splitVolume(1.0, 1.0, 100.0, 1.0);
	ASSERT_EQ(1u, ret.size());
	EXPECT_DOUBLE_EQ(1.0, ret[0]);
}

TEST(l06_split_volume, double_degenerate_range_returns_whole)
{
	// minQty == maxQty 时 length=1, 每笔固定为 minQty
	std::vector<double> ret = splitVolume(25.0, 5.0, 5.0, 1.0);
	EXPECT_NEAR(25.0, sum_of(ret), 1e-9);
	for (std::size_t i = 0; i + 1 < ret.size(); i++)
		EXPECT_DOUBLE_EQ(5.0, ret[i]);
}

TEST(l06_split_volume, gen_rand_within_bounds)
{
	for (int i = 0; i < 32; i++)
	{
		EXPECT_LT(genRand(10000), 10000u);
		EXPECT_LT(genRand(100), 100u);
	}
	// maxVal=1 时 rand()%1 恒为 0
	EXPECT_EQ(0u, genRand(1));
}
