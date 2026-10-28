// r94 / WtDataWriter：24:00 收盘时段导致的分钟线跨日归位（同步写路径）
//
// 覆盖（src/WtDataStorage/WtDataWriter.cpp）：
//  - pipeToKlines  min1 段 `barTime == 0 -> barDate = getNextDate` :1111-1113
//  - pipeToKlines  min5 段同型分支                          :1217-1219
//
// 手法：派生 r23::WriterBDMgr 注册一个 21:00-24:00 的品种（cu2401），
// 用 writeTick 公开入口写 23:59 的 tick；此时 minuteToTime(180) 回绕成 0，
// 分钟线必须落到 action_date 的下一日（trading_date）而不是当天。
// 仍然只 release 不 delete writer（r23 fixture 头注释的线程纪律）。
#include "r23_writer_fixture.h"

namespace r94 {

using namespace r23;
using std::string;

namespace {

// 只增加一个交易时段以 2400 结束的品种，其余沿用 r23 宿主
class MidnightBDMgr : public WriterBDMgr
{
public:
	MidnightBDMgr()
	{
		// offset 1440 与 0 等价（offsetTime 向左对齐后会回绕），但让 minuteToTime(总分钟)
		// 在 24:00 收盘时经 originalTime(2400) 归一成 0，从而走 barTime==0 的跨日分支。
		_mid = WTSSessionInfo::create("R94_MID", "r94 夜盘到 24 点", 1440);
		_mid->addTradingSection(2100, 2400);

		_cu = WTSCommodityInfo::create("cu", "r94 铜", "SHFE", "R94_MID", "CHN");
		_cu->setSessionInfo(_mid);
		_cu->setCategory(ContractCategory::CC_Future);

		add_contract("cu2401", _cu);
	}

	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		if (pid != NULL && strcmp(pid, "cu") == 0)
			return _cu;
		return WriterBDMgr::getCommodity(exchg, pid);
	}

	WTSSessionInfo* getSession(const char* sid) override
	{
		if (sid != NULL && strcmp(sid, "R94_MID") == 0)
			return _mid;
		return WriterBDMgr::getSession(sid);
	}

private:
	// 不在这里 release：基类析构会先 release 合约，再 release 自有的品种/时段；
	// 本对象的两个替身由 OS 在进程退出时回收（一条用例一个进程，泄漏有界）。
	WTSSessionInfo*		_mid = NULL;
	WTSCommodityInfo*	_cu = NULL;
};

} // namespace

class R94WriterMidnightTest : public R23WriterTest
{
protected:
	void SetUp() override
	{
		R23WriterTest::SetUp();
		_bd.reset(new MidnightBDMgr());
		_sink.reset(new WriterSink(_bd.get()));
		_sink->add_comm("SHFE.cu");
	}
};

// WtDataWriter.cpp: pipeToKlines L1111-1113 / L1217-1219
TEST_F(R94WriterMidnightTest, R94WriterMidnightBarWrapsToTradingDate)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec t;
	t.code = "cu2401";
	t.exchg = "SHFE";
	t.trading_date = 20240109;		// 夜盘 23:59 属于下一交易日
	t.action_date = 20240108;
	t.action_time = 235900000;
	t.price = 70000.0;
	t.open = 69900.0;
	t.high = 70100.0;
	t.low = 69800.0;
	t.volume = 5.0;
	t.turn_over = 350000.0;
	write_tick(w, t, 0);

	const string min1 = rt_min1_file("cu2401");
	const string min5 = rt_min5_file("cu2401");
	ASSERT_TRUE(file_exists(min1));
	ASSERT_TRUE(file_exists(min5));

	const RTDayBlockHeader h1 = read_block_header<RTDayBlockHeader>(min1);
	const RTDayBlockHeader h5 = read_block_header<RTDayBlockHeader>(min5);
	EXPECT_EQ(h1._size, 1u);
	EXPECT_EQ(h5._size, 1u);

	WTSBarStruct b1;
	WTSBarStruct b5;
	const bool got1 = read_block_item<RTDayBlockHeader, WTSBarStruct>(min1, 0, b1);
	const bool got5 = read_block_item<RTDayBlockHeader, WTSBarStruct>(min5, 0, b5);
	ASSERT_TRUE(got1);
	ASSERT_TRUE(got5);

	// barTime==0 之后被重新拼成「下一日 00:00」的分钟线标签
	EXPECT_EQ(b1.date, 20240109u);
	EXPECT_EQ(b5.date, 20240109u);
	EXPECT_EQ(TimeUtils::minBarToDate(b1.time), 20240109u);
	EXPECT_EQ(TimeUtils::minBarToDate(b5.time), 20240109u);
	EXPECT_EQ(TimeUtils::minBarToTime(b1.time), 0u);
	EXPECT_EQ(TimeUtils::minBarToTime(b5.time), 0u);
	EXPECT_DOUBLE_EQ(b1.open, 70000.0);
	EXPECT_DOUBLE_EQ(b5.open, 70000.0);
	EXPECT_DOUBLE_EQ(b1.vol, 5.0);
}

} // namespace r94
