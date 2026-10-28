// r100 / WtDataWriter：收盘任务中的空 dumper 与未注册品种跳过
//
// 覆盖（src/WtDataStorage/WtDataWriter.cpp）：
//  - transHisData 的 session comm 中品种未注册 -> `pCommInfo == NULL -> continue` :1586-1588
//  - dump_bars_via_dumper 三处 `dumper == NULL -> continue` :1733-1734 / :1759-1760 / :1793-1794
//
// 手法：_dumpers 只注册一个 NULL 指针；tick/min1/min5 收盘路径因此只走空指针分支。
// 为了让收盘任务能处理到 rb2210 的 K 线，_disable_tick=true 但 K 线不关闭
// （pipeToTicks 被跳过，pipeToKlines 仍由 tick 驱动）。
// 走 transHisData 的 writer 按 r23 纪律：等 marker.ini 后 abandon，不 release/delete。
#include "r23_writer_fixture.h"

namespace r100 {

using namespace r23;
using std::string;

// getCommodity 对未注册品种返回 NULL，用于驱动 transHisData 的 continue 分支
class GhostBDMgr : public WriterBDMgr
{
public:
	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		if (pid != NULL && strcmp(pid, "ghost") == 0)
			return NULL;
		return WriterBDMgr::getCommodity(exchg, pid);
	}
};

class R100WriterTest : public R23WriterTest
{
protected:
	void SetUp() override
	{
		R23WriterTest::SetUp();
		_bd.reset(new GhostBDMgr());
		_sink.reset(new WriterSink(_bd.get()));
		_sink->add_comm("SHFE.ghost");
		_sink->add_comm("SHFE.rb");
	}
};

// WtDataWriter.cpp: L1586-1588 / L1733-1734 / L1759-1760 / L1793-1794
TEST_F(R100WriterTest, R100WriterNullDumperAndUnknownSessionComm)
{
	R23Writer* w = make_writer("\"disabletick\":true,\"disabletrans\":true,"
		"\"disableordque\":true,\"disableorddtl\":true");
	ASSERT_TRUE(w != NULL);
	w->add_dumper("null", NULL);

	TickSpec spec;
	spec.code = "rb2210";
	spec.action_time = 93000000;
	spec.price = 3600.0;
	write_tick(w, spec, 0);

	spec.action_time = 93100000;
	spec.price = 3610.0;
	write_tick(w, spec, 0);

	w->transHisData("R100SID");
	ASSERT_TRUE(wait_marker("R100SID")) << "收盘任务未在 10s 内排空";

	EXPECT_TRUE(file_exists(his_min1_file("rb2210")));
	EXPECT_TRUE(file_exists(his_min5_file("rb2210")));
	EXPECT_TRUE(file_exists(his_day_file("rb2210")));
	EXPECT_TRUE(_sink->has_log("ClosingTask of rb2210", LL_INFO));

	// proc_loop 线程的 release 有丢唤醒窗口（BUG-R23-005），只标记不再触碰
	abandon_writer(w);
}

} // namespace r100
