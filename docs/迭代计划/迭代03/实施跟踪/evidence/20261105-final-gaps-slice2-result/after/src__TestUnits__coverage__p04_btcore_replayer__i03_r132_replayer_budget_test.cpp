/**
 * BUG-R132-001（迭代03 最终缺口，slice2）：HisDataReplayer 自身解压历史块（HisDataReplayer.cpp
 * proc_block_data / 第二套 K 线读取），K37 预算按其所在模块生效。配置位置与 store 路径相同：
 * 有 store 节点时取 store 中的 uncompress_max_bytes / uncompress_max_ratio，否则取回放器根配置。
 * 非法值：init 返回 false（fail-closed）。红控：以改前 HisDataReplayer.cpp 对象重链（配置被忽略）。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCmpHelper.hpp>

namespace
{
	std::filesystem::path i03r132_repo_root()
	{
		// 与 r43_replayer_init_basefiles_test.cpp 相同：源文件目录上溯 5 级到仓库根
		return std::filesystem::path(__FILE__).parent_path()
			.parent_path().parent_path().parent_path().parent_path();
	}

	class I03R132ReplayerBudget : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			WTSCmpHelper::set_uncompress_budget(1ull << 30, 1000);
			_base = std::filesystem::temp_directory_path() / ("i03_r132_rp_" + std::to_string(::getpid()));
			_out = _base / "out";
			std::error_code ec;
			std::filesystem::remove_all(_base, ec);
			std::filesystem::create_directories(_out, ec);
			WtHelper::setOutputDir(_out.string().c_str());
		}
		void TearDown() override
		{
			WTSCmpHelper::set_uncompress_budget(1ull << 30, 1000);
			std::error_code ec;
			std::filesystem::remove_all(_base, ec);
		}

		WTSVariant* base_cfg()
		{
			const std::filesystem::path root = i03r132_repo_root();
			WTSVariant* cfg = WTSVariant::createObject();
			cfg->append("mode", "csv");
			cfg->append("path", _base.string().c_str());
			cfg->append("stime", static_cast<uint64_t>(202001020930));
			cfg->append("etime", static_cast<uint64_t>(202001081500));
			WTSVariant* basefiles = WTSVariant::createObject();
			basefiles->append("session", (root / "dist/common/sessions.json").string().c_str());
			basefiles->append("commodity", (root / "dist/common/stk_comms.json").string().c_str());
			basefiles->append("holiday", (root / "dist/common/holidays.json").string().c_str());
			cfg->append("basefiles", basefiles);
			basefiles->release();
			return cfg;
		}

		std::filesystem::path _base;
		std::filesystem::path _out;
	};
}

TEST_F(I03R132ReplayerBudget, RootBudgetIsAppliedToTheReplayerModule)
{
	WTSVariant* cfg = base_cfg();
	cfg->append("uncompress_max_ratio", static_cast<uint32_t>(5));
	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	EXPECT_EQ(5u, WTSCmpHelper::uncompress_max_ratio());
	EXPECT_EQ(1ull << 30, WTSCmpHelper::uncompress_max_bytes());
}

TEST_F(I03R132ReplayerBudget, StoreSectionBudgetTakesPrecedenceLikeTheStorePath)
{
	WTSVariant* cfg = base_cfg();
	cfg->append("uncompress_max_ratio", static_cast<uint32_t>(9));	// store 存在时不读根节点
	WTSVariant* store = WTSVariant::createObject();
	store->append("path", _base.string().c_str());
	store->append("uncompress_max_bytes", "65536");	// YAML 风格数字串
	cfg->append("store", store);
	store->release();
	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	EXPECT_EQ(65536u, WTSCmpHelper::uncompress_max_bytes());
	EXPECT_EQ(1000u, WTSCmpHelper::uncompress_max_ratio());
}

TEST_F(I03R132ReplayerBudget, InvalidBudgetFailsInit)
{
	WTSVariant* cfg = base_cfg();
	WTSVariant* store = WTSVariant::createObject();
	store->append("path", _base.string().c_str());
	store->append("uncompress_max_ratio", "1.5");
	cfg->append("store", store);
	store->release();
	HisDataReplayer replayer;
	EXPECT_FALSE(replayer.init(cfg)) << "an invalid uncompress budget must fail the replayer configuration";
	cfg->release();
	EXPECT_TRUE(WTSCmpHelper::uncompress_refused());
}

TEST_F(I03R132ReplayerBudget, AbsentBudgetKeepsDefaults)
{
	WTSVariant* cfg = base_cfg();
	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	EXPECT_EQ(1ull << 30, WTSCmpHelper::uncompress_max_bytes());
	EXPECT_EQ(1000u, WTSCmpHelper::uncompress_max_ratio());
	EXPECT_FALSE(WTSCmpHelper::uncompress_refused());
}
