// BUG-R132-001（迭代03 最终缺口，slice2）：K37=B 的解压预算“可配置”——宿主配置入口
// wt_cmp_budget::apply（WTSUtils/WTSCmpBudgetConfig.hpp）。键：uncompress_max_bytes /
// uncompress_max_ratio，缺省保持 1 GiB / 1000:1；非法值 fail-closed（拒绝本模块全部解压）。
// 新接口，红控不适用；读端/写端/回放器接入的红绿见 l04/r23/p04 lane 的对应用例。
#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include "Includes/WTSVariant.hpp"
#include "WTSUtils/WTSCfgLoader.h"
#include "WTSUtils/WTSCmpBudgetConfig.hpp"

using namespace wtp;

namespace
{
	const uint64_t kDefaultBytes = 1ull << 30;
	const uint64_t kDefaultRatio = 1000;

	std::string compressed_zeros(size_t n)
	{
		const std::string raw(n, '\0');
		return WTSCmpHelper::compress_data(raw.data(), raw.size());
	}

	// 64 KiB：前 4 KiB 伪随机、其余为 0，压缩比约十几倍——高于 5、低于默认 1000
	std::string compressed_mixed()
	{
		std::string raw(64 * 1024, '\0');
		uint32_t x = 0x9E3779B9u;
		for (size_t i = 0; i < 4096; i++)
		{
			x = x * 1664525u + 1013904223u;
			raw[i] = static_cast<char>(x >> 24);
		}
		return WTSCmpHelper::compress_data(raw.data(), raw.size());
	}

	class I03R132CmpBudgetConfig : public ::testing::Test
	{
	protected:
		void SetUp() override { WTSCmpHelper::set_uncompress_budget(kDefaultBytes, kDefaultRatio); }
		void TearDown() override { WTSCmpHelper::set_uncompress_budget(kDefaultBytes, kDefaultRatio); }

		static WTSVariant* json(const char* text)
		{
			WTSVariant* v = WTSCfgLoader::load_from_content(text, false);
			EXPECT_NE(nullptr, v) << text;
			return v;
		}
		static WTSVariant* yaml(const char* text)
		{
			WTSVariant* v = WTSCfgLoader::load_from_content(text, true);
			EXPECT_NE(nullptr, v) << text;
			return v;
		}
	};
}

TEST_F(I03R132CmpBudgetConfig, AbsentKeysKeepTheK37Defaults)
{
	WTSVariant* cfg = json(R"({"path":"./storage/"})");
	std::string error;
	EXPECT_EQ(wt_cmp_budget::ApplyResult::Unchanged, wt_cmp_budget::apply(cfg, error));
	cfg->release();
	EXPECT_EQ(kDefaultBytes, WTSCmpHelper::uncompress_max_bytes());
	EXPECT_EQ(kDefaultRatio, WTSCmpHelper::uncompress_max_ratio());
	EXPECT_FALSE(WTSCmpHelper::uncompress_refused());
	EXPECT_EQ(wt_cmp_budget::ApplyResult::Unchanged, wt_cmp_budget::apply(NULL, error));
}

TEST_F(I03R132CmpBudgetConfig, JsonIntegersAndYamlDigitStringsAreApplied)
{
	std::string error;
	WTSVariant* cfg = json(R"({"uncompress_max_bytes":4096,"uncompress_max_ratio":5})");
	ASSERT_EQ(wt_cmp_budget::ApplyResult::Applied, wt_cmp_budget::apply(cfg, error));
	cfg->release();
	EXPECT_EQ(4096u, WTSCmpHelper::uncompress_max_bytes());
	EXPECT_EQ(5u, WTSCmpHelper::uncompress_max_ratio());

	// 只给一个键：另一项保持当前值
	cfg = yaml("uncompress_max_ratio: 0\n");
	ASSERT_EQ(wt_cmp_budget::ApplyResult::Applied, wt_cmp_budget::apply(cfg, error));
	cfg->release();
	EXPECT_EQ(4096u, WTSCmpHelper::uncompress_max_bytes());
	EXPECT_EQ(0u, WTSCmpHelper::uncompress_max_ratio()) << "0 disables the ratio ceiling";

	cfg = json(R"({"uncompress_max_bytes":1e6})");	// 整数值的有限实数
	ASSERT_EQ(wt_cmp_budget::ApplyResult::Applied, wt_cmp_budget::apply(cfg, error));
	cfg->release();
	EXPECT_EQ(1000000u, WTSCmpHelper::uncompress_max_bytes());
}

TEST_F(I03R132CmpBudgetConfig, ConfiguredBudgetGovernsDecompression)
{
	const std::string frame = compressed_mixed();
	ASSERT_GT(64u * 1024u / frame.size(), 5u);
	ASSERT_LT(64u * 1024u / frame.size(), 1000u);
	EXPECT_NO_THROW(WTSCmpHelper::uncompress_data(frame.data(), frame.size()));

	std::string error;
	WTSVariant* cfg = json(R"({"uncompress_max_ratio":5})");
	ASSERT_EQ(wt_cmp_budget::ApplyResult::Applied, wt_cmp_budget::apply(cfg, error));
	cfg->release();
	EXPECT_THROW(WTSCmpHelper::uncompress_data(frame.data(), frame.size()), std::runtime_error);

	cfg = json(R"({"uncompress_max_ratio":0,"uncompress_max_bytes":1024})");
	ASSERT_EQ(wt_cmp_budget::ApplyResult::Applied, wt_cmp_budget::apply(cfg, error));
	cfg->release();
	EXPECT_THROW(WTSCmpHelper::uncompress_data(frame.data(), frame.size()), std::runtime_error)
		<< "64 KiB declared size exceeds the 1 KiB absolute ceiling";
}

TEST_F(I03R132CmpBudgetConfig, InvalidValuesFailClosedAndAValidBudgetLiftsIt)
{
	const char* invalid[] = {
		R"({"uncompress_max_bytes":-1})",
		R"({"uncompress_max_ratio":2.5})",
		R"({"uncompress_max_ratio":true})",
		R"({"uncompress_max_bytes":"12x"})",
		R"({"uncompress_max_bytes":"+5"})",
		R"({"uncompress_max_bytes":""})",
		R"({"uncompress_max_bytes":"99999999999999999999"})",	// 超出 uint64
		R"({"uncompress_max_ratio":{"v":1}})",
		R"({"uncompress_max_ratio":[1]})",
	};
	const std::string frame = compressed_zeros(256);
	for (const char* text : invalid)
	{
		WTSCmpHelper::set_uncompress_budget(kDefaultBytes, kDefaultRatio);
		WTSVariant* cfg = json(text);
		ASSERT_NE(nullptr, cfg);
		std::string error;
		EXPECT_EQ(wt_cmp_budget::ApplyResult::Invalid, wt_cmp_budget::apply(cfg, error)) << text;
		cfg->release();
		EXPECT_NE(std::string::npos, error.find("uncompress_max_")) << text << " -> " << error;
		EXPECT_TRUE(WTSCmpHelper::uncompress_refused()) << text;
		EXPECT_THROW(WTSCmpHelper::uncompress_data(frame.data(), frame.size()), std::runtime_error) << text;
		// 非法配置不得改写已有预算值（不回退到别的数值）
		EXPECT_EQ(kDefaultBytes, WTSCmpHelper::uncompress_max_bytes()) << text;
		EXPECT_EQ(kDefaultRatio, WTSCmpHelper::uncompress_max_ratio()) << text;
	}

	WTSVariant* cfg = json(R"({"uncompress_max_ratio":1000})");
	std::string error;
	ASSERT_EQ(wt_cmp_budget::ApplyResult::Applied, wt_cmp_budget::apply(cfg, error));
	cfg->release();
	EXPECT_FALSE(WTSCmpHelper::uncompress_refused());
	EXPECT_NO_THROW(WTSCmpHelper::uncompress_data(frame.data(), frame.size()));
}
