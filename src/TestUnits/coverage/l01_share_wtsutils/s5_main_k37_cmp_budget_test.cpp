/*!
 * 迭代03 第五会话主线程：决策 K37=B（2026-11-04 用户裁定）——CMP_V2 解压资源预算。
 * 默认绝对上限 1 GiB、压缩比上限 1000:1（可配置）；帧头未声明或声明非法内容大小时拒绝。
 * 超限在分配解压缓冲前以 std::runtime_error 拒绝（读端已在 helper 外捕获异常）。
 *
 * 第一组用例只使用改前已有的 uncompress_data 接口，可用改前头文件单独编译做红控；
 * 第二组用例验证新增的可配置接口。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "WTSUtils/WTSCmpHelper.hpp"

namespace
{
	std::string lcg_bytes(size_t n)
	{
		std::string out(n, '\0');
		uint32_t x = 0x12345678u;
		for (size_t i = 0; i < n; i++)
		{
			x = x * 1664525u + 1013904223u;
			out[i] = static_cast<char>(x >> 24);
		}
		return out;
	}

	std::string frame_without_content_size(const std::string& raw)
	{
		ZSTD_CCtx* cctx = ZSTD_createCCtx();
		ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 0);
		std::string out(ZSTD_compressBound(raw.size()), '\0');
		const size_t n = ZSTD_compress2(cctx, &out[0], out.size(), raw.data(), raw.size());
		ZSTD_freeCCtx(cctx);
		out.resize(ZSTD_isError(n) ? 0 : n);
		return out;
	}
}

// —— 第一组：只用改前接口（红控可编译） ——
TEST(S5MainK37CmpBudget, DefaultRatioBudgetRejectsHighlyCompressedFrame)
{
	const std::string zeros(8u << 20, '\0');	// 8 MiB 全零，压缩比远超 1000:1
	const std::string packed = WTSCmpHelper::compress_data(zeros.data(), zeros.size());
	ASSERT_FALSE(packed.empty());
	ASSERT_GT(zeros.size() / packed.size(), 1000u);
	EXPECT_THROW(WTSCmpHelper::uncompress_data(packed.data(), packed.size()), std::runtime_error);
}

TEST(S5MainK37CmpBudget, FrameWithoutDeclaredContentSizeIsRejected)
{
	const std::string raw = lcg_bytes(4096);
	const std::string packed = frame_without_content_size(raw);
	ASSERT_FALSE(packed.empty());
	ASSERT_EQ(ZSTD_CONTENTSIZE_UNKNOWN, ZSTD_getFrameContentSize(packed.data(), packed.size()));
	EXPECT_THROW(WTSCmpHelper::uncompress_data(packed.data(), packed.size()), std::runtime_error);
}

TEST(S5MainK37CmpBudget, OrdinaryFrameWithinDefaultBudgetRoundTrips)
{
	const std::string raw = lcg_bytes(1u << 20);
	const std::string packed = WTSCmpHelper::compress_data(raw.data(), raw.size());
	EXPECT_EQ(raw, WTSCmpHelper::uncompress_data(packed.data(), packed.size()));
}

// —— 第二组：可配置预算（红控单独编译改前头文件时以 S5K37_LEGACY_HEADER_ONLY 排除） ——
#ifndef S5K37_LEGACY_HEADER_ONLY
class S5MainK37CmpBudgetConfig : public ::testing::Test
{
protected:
	void TearDown() override { WTSCmpHelper::set_uncompress_budget(1ull << 30, 1000); }
};

TEST_F(S5MainK37CmpBudgetConfig, DefaultsAreOneGibAndThousandToOne)
{
	EXPECT_EQ(1ull << 30, WTSCmpHelper::uncompress_max_bytes());
	EXPECT_EQ(1000u, WTSCmpHelper::uncompress_max_ratio());
}

TEST_F(S5MainK37CmpBudgetConfig, AbsoluteBudgetBoundaryIsInclusive)
{
	const std::string raw = lcg_bytes(256u << 10);
	const std::string packed = WTSCmpHelper::compress_data(raw.data(), raw.size());
	WTSCmpHelper::set_uncompress_budget(raw.size(), 0);
	EXPECT_EQ(raw, WTSCmpHelper::uncompress_data(packed.data(), packed.size()));
	WTSCmpHelper::set_uncompress_budget(raw.size() - 1, 0);
	EXPECT_THROW(WTSCmpHelper::uncompress_data(packed.data(), packed.size()), std::runtime_error);
}

TEST_F(S5MainK37CmpBudgetConfig, RatioBudgetIsConfigurableAndZeroDisablesIt)
{
	const std::string zeros(8u << 20, '\0');
	const std::string packed = WTSCmpHelper::compress_data(zeros.data(), zeros.size());
	const uint64_t ratioCeil = zeros.size() / packed.size() + 1;
	WTSCmpHelper::set_uncompress_budget(1ull << 30, ratioCeil);
	EXPECT_EQ(zeros, WTSCmpHelper::uncompress_data(packed.data(), packed.size()));
	WTSCmpHelper::set_uncompress_budget(1ull << 30, ratioCeil - 2);
	EXPECT_THROW(WTSCmpHelper::uncompress_data(packed.data(), packed.size()), std::runtime_error);
	WTSCmpHelper::set_uncompress_budget(1ull << 30, 0);
	EXPECT_EQ(zeros, WTSCmpHelper::uncompress_data(packed.data(), packed.size()));
}
#endif // S5K37_LEGACY_HEADER_ONLY
