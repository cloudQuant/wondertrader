// BUG-R132-001（迭代03 最终缺口，slice2）：WtDataWriter 收盘作业会解压既有历史块，K37 预算可在
// 写端配置中与 path 同级给出（uncompress_max_bytes / uncompress_max_ratio）。非法值 fail-closed：
// init 返回 false 并记 ERROR。红控：以改前 WtDataWriter.cpp 对象重链（配置被忽略，init 成功）。
#include "r23_writer_fixture.h"

#include "WTSUtils/WTSCmpHelper.hpp"

namespace i03r132w {

using namespace r23;
using std::string;

class I03R132WriterBudgetTest : public R23WriterTest
{
protected:
	void SetUp() override
	{
		R23WriterTest::SetUp();
		WTSCmpHelper::set_uncompress_budget(1ull << 30, 1000);
	}
	void TearDown() override
	{
		R23WriterTest::TearDown();
		WTSCmpHelper::set_uncompress_budget(1ull << 30, 1000);
	}

	bool init_with(const char* extra, R23Writer*& out)
	{
		const string file = write_params(extra);
		WTSVariant* cfg = WTSCfgLoader::load_from_file(file.c_str());
		EXPECT_TRUE(cfg != NULL);
		if (cfg == NULL)
			return false;
		out = new R23Writer();
		const bool ok = out->init(cfg, _sink.get());
		cfg->release();
		return ok;
	}
};

TEST_F(I03R132WriterBudgetTest, ConfiguredBudgetReachesTheWriterModule)
{
	R23Writer* w = NULL;
	ASSERT_TRUE(init_with("\"uncompress_max_bytes\":8192,\"uncompress_max_ratio\":\"7\"", w));
	EXPECT_EQ(8192u, WTSCmpHelper::uncompress_max_bytes());
	EXPECT_EQ(7u, WTSCmpHelper::uncompress_max_ratio());
	EXPECT_FALSE(WTSCmpHelper::uncompress_refused());
	w->release();
}

TEST_F(I03R132WriterBudgetTest, InvalidBudgetFailsInitialization)
{
	R23Writer* w = NULL;
	EXPECT_FALSE(init_with("\"uncompress_max_ratio\":-3", w))
		<< "an invalid uncompress budget must not be silently ignored";
	EXPECT_TRUE(_sink->has_log("uncompress_max_ratio", LL_ERROR));
	EXPECT_FALSE(_sink->has_log("WtDataWriter initialized", LL_INFO));
	EXPECT_TRUE(WTSCmpHelper::uncompress_refused());
	if (w != NULL)
		w->release();
}

TEST_F(I03R132WriterBudgetTest, AbsentBudgetKeepsDefaults)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);
	EXPECT_EQ(1ull << 30, WTSCmpHelper::uncompress_max_bytes());
	EXPECT_EQ(1000u, WTSCmpHelper::uncompress_max_ratio());
}

} // namespace i03r132w
