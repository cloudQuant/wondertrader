/**
 * r103_replayer_task_branch_test.cpp  (lane R103 · wave18)
 *
 * 靶子：run_by_tasks 日频及以上/分钟任务的剩余公开分支
 *   - endtime 跨日（_task->_time==0）的 bIsPreDay        HisDataReplayer.cpp:951-954
 *   - 月频精确日命中、节假日内同月顺延                     :987-1005
 *   - 年频触发                                             :1029-1032
 *   - 周频「上一交易日在周内更靠后」补发                   :1018-1022
 *   - 分钟任务起始分钟不对齐的修正                         :1114-1120
 *
 * 不接真实数据；只注册任务并推进交易日历，断言调度日期序列。
 */
#include "r103_common.h"

using R103Fixture = r91::Fixture;

namespace
{
	struct TaskRun
	{
		std::vector<uint32_t> dates;
		uint32_t begins { 0 };
		uint32_t ends { 0 };
		uint32_t done { 0 };
	};

	TaskRun run_task(const std::filesystem::path& repo, const std::filesystem::path& base,
		const r91::CfgOpts& opts, uint32_t day, uint32_t time, const char* period,
		const char* session, uint64_t stime, uint64_t etime,
		const std::filesystem::path& holiday = std::filesystem::path())
	{
		TaskRun ret;
		WTSVariant* cfg = r103::make_cfg(repo, base, stime, etime, opts, holiday);
		HisDataReplayer replayer;
		EXPECT_TRUE(replayer.init(cfg, NULL, NULL));
		cfg->release();

		r103::Sink sink;
		replayer.register_sink(&sink, "r103_task");
		replayer.register_task(1u, day, time, period, "CHINA", session);
		if (!replayer.prepare())
			return ret;
		replayer.run(false);

		ret.dates = sink.scheduleDates;
		ret.begins = sink.beginCalls;
		ret.ends = sink.endCalls;
		ret.done = sink.doneCalls;
		return ret;
	}
}

/*
 * 生产: HisDataReplayer.cpp:987-1005（月频精确日 + 同月节假日内顺延）
 *       1029-1032（年频）
 *       951-954（任务时间为 0 时 endtime 跨日）
 *       1114-1120（分钟任务起点不对齐的修正）
 */
TEST_F(R103Fixture, R103Replayer_TaskCalendarBranches)
{
	r91::CfgOpts opts;
	opts.mode = "csv";
	opts.tick = false;

	// 月频：20200106 精确命中（_day 口径为 YYMMDD）
	TaskRun monthly = run_task(_root, _base, opts, 200106u, 931u, "m", "SD0930",
		202001020930ull, 202001071500ull);
	ASSERT_EQ(1u, monthly.dates.size());
	EXPECT_EQ(20200106u, monthly.dates[0]);
	EXPECT_EQ(1u, monthly.done);

	// 月频：目标 01/15 落在自定义假日，01/16 同月顺延
	const std::filesystem::path holi1 = _base / "holidays_midmonth.json";
	ASSERT_TRUE(r91::write_text(holi1, "{\n  \"CHINA\": [20200115]\n}\n"));
	TaskRun shifted = run_task(_root, _base, opts, 200115u, 931u, "m", "SD0930",
		202001140930ull, 202001171500ull, holi1);
	ASSERT_EQ(1u, shifted.dates.size());
	EXPECT_EQ(20200116u, shifted.dates[0]);

	// 年频：01/03 触发（_day 口径为 MMDD）
	TaskRun yearly = run_task(_root, _base, opts, 103u, 931u, "y", "SD0930",
		202001030930ull, 202001031500ull);
	ASSERT_EQ(1u, yearly.dates.size());
	EXPECT_EQ(20200103u, yearly.dates[0]);

	// 日频任务时间 00:00 -> endtime 23:59 跨日，_cur_date 先退一天
	TaskRun daily = run_task(_root, _base, opts, 0u, 0u, "d", "SD0930",
		202001020930ull, 202001022359ull);
	ASSERT_EQ(1u, daily.dates.size());
	EXPECT_EQ(20200102u, daily.dates[0]);

	// 分钟任务 7 分钟步长，09:31 不对齐 -> 先修正到第 7 个交易分钟
	TaskRun mins = run_task(_root, _base, opts, 0u, 7u, "min", "SD0930",
		202001020931ull, 202001021500ull);
	EXPECT_GT(mins.dates.size(), 0u);
	EXPECT_EQ(1u, mins.done);

	// 周频：自定义 01/06-01/08 休市，周二任务在 01/09 补发
	const std::filesystem::path holi = _base / "holidays_week.json";
	ASSERT_TRUE(r91::write_text(holi, "{\n  \"CHINA\": [20200106, 20200107, 20200108]\n}\n"));
	TaskRun weekly = run_task(_root, _base, opts, 2u, 1500u, "w", "SD0930",
		202001020930ull, 202001101500ull, holi);
	ASSERT_EQ(1u, weekly.dates.size());
	EXPECT_EQ(20200109u, weekly.dates[0]);
}
