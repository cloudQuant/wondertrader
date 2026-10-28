/**
 * r62_cta_mocker_gap_test.cpp  (lane R62)
 *
 * 靶子：src/WtBtCore/CtaMocker.cpp 在第十波后仍未覆盖的块：
 *   load_incremental_data(L454-625)      —— 五份 csv + 策略 json（持仓/资金/信号/条件单）
 *   proc_tick 条件单算法/动作(L695-896)  —— WCT_* 比较、同算法择优、OL/CL/OS/CS/SP
 *   on_schedule(L1096-1214)              —— 主K线闭合、非交易时段、二次调度
 *   stra_get_bars/ticks/last_tick/sub_*  —— L1773-1883 的 key/主图冲突/tick 缓存
 *   on_session_begin/end、enum_position  —— L1215-1295；T+1 冻结释放与持仓汇总
 *
 * 只用公开 API + 真实 HisDataReplayer（csv 模式，dist/common 只读）驱动，
 * 不使用 #define private public，不注入私有成员。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <WtBtCore/CtaMocker.h>

#include "r62_mocker_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
	const char* const kCu = "SHFE.cu.2406";
	const char* const kAg = "SHFE.ag.2406";
	const char* const kRu = "SHFE.ru.2406";
	const char* const kHc = "SHFE.hc.2405";
	const char* const kStk = "SSE.STK.000001";

	void write_prev_file(const std::filesystem::path& dir, const char* name, const char* content)
	{
		std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
		out << content;
	}
}

class R62CtaMockerGap : public r62::ReplayFixture
{
protected:
	void feed(CtaMocker& m, const char* code, double px, uint32_t pxType = 0)
	{
		WTSTickData* t = r62::make_tick(code, 20240102u, 93000000u, px, px - 1.0, px + 1.0);
		m.handle_tick(code, t, pxType);
		t->release();
	}
};

// load_incremental_data：五份 csv 行与策略 json 的持仓/资金/信号/条件单全部恢复，
// 回放结束后再落盘时必须原样带出（不丢字段、不重复表头）。
TEST_F(R62CtaMockerGap, LoadIncrementalDataRestoresCsvAndJson)
{
	const std::filesystem::path prev = _out / "r62_prev";
	std::filesystem::create_directories(prev);

	write_prev_file(prev, "trades.csv",
		"code,time,direct,action,price,qty,tag,fee,barno\n"
		"SHFE.rb.2405,20240102093000,LONG,OPEN,3800,3,prev,0.00,0\n");
	write_prev_file(prev, "closes.csv",
		"code,direct,opentime,openprice,closetime,closeprice,qty,profit,maxprofit,maxloss,totalprofit,entertag,exittag,openbarno,closebarno\n"
		"SHFE.rb.2405,LONG,1,3800,2,3810,1,10.00,10.00,0.00,10.00,a,b,0,0\n");
	write_prev_file(prev, "funds.csv",
		"date,closeprofit,positionprofit,dynbalance,fee\n"
		"20240102,100.00,7.50,106.25,1.25\n");
	write_prev_file(prev, "positions.csv",
		"date,code,volume,closeprofit,dynprofit\n"
		"20240102,SHFE.rb.2405,3,12.50,7.50\n");
	write_prev_file(prev, "signals.csv",
		"code,target,sigprice,gentime,usertag\n"
		"SHFE.ag.2406,2,5900,20240102093100,sigprev\n");
	write_prev_file(prev, "r62_prev.json",
		"{\n"
		"  \"positions\": [{\"code\":\"SHFE.rb.2405\",\"volume\":3,\"closeprofit\":12.5,\"dynprofit\":7.5,"
		"\"lastentertime\":20240102093000,\"lastexittime\":0,\"details\":[{\"long\":true,\"price\":3800.0,"
		"\"maxprice\":3810.0,\"minprice\":3790.0,\"volume\":3.0,\"opentime\":20240102093000,"
		"\"opentdate\":20240102,\"profit\":15.0,\"maxprofit\":20.0,\"maxloss\":-5.0,\"opentag\":\"prev\"}]}],\n"
		"  \"fund\": {\"total_profit\":100.0,\"total_dynprofit\":7.5,\"total_fees\":1.25},\n"
		"  \"signals\": {\"SHFE.ag.2406\":{\"usertag\":\"sigprev\",\"volume\":2.0,"
		"\"sigprice\":5900.0,\"gentime\":20240102093100}},\n"
		"  \"conditions\": {\"settime\":20240102093000,\"items\":{"
		"\"SHFE.cu.2406\":[{\"code\":\"SHFE.cu.2406\",\"usertag\":\"condsp\",\"field\":0,"
		"\"alg\":0,\"target\":70000.0,\"qty\":2.0,\"action\":4}],"
		"\"SHFE.ru.2406\":[{\"code\":\"SHFE.ru.2406\",\"usertag\":\"condol\",\"field\":0,"
		"\"alg\":1,\"target\":10000.0,\"qty\":1.0,\"action\":0}],"
		"\"SHFE.hc.2405\":[{\"code\":\"SHFE.hc.2405\",\"usertag\":\"condos\",\"field\":0,"
		"\"alg\":2,\"target\":3500.0,\"qty\":2.0,\"action\":2}]}}\n"
		"}\n");

	CtaMocker mocker(&_rp, "r62_inc");
	mocker.load_incremental_data("r62_prev");

	// json 持仓优先于空仓状态：数量/均价/开仓标签/首末时间都可查
	EXPECT_DOUBLE_EQ(3.0, mocker.stra_get_position(kRb));
	EXPECT_DOUBLE_EQ(3800.0, mocker.stra_get_position_avgpx(kRb));
	EXPECT_DOUBLE_EQ(3800.0, mocker.stra_get_detail_cost(kRb, "prev"));
	EXPECT_EQ(static_cast<uint64_t>(20240102093000ull), mocker.stra_get_last_entertime(kRb));
	EXPECT_STREQ("prev", mocker.stra_get_last_entertag(kRb));

	// json 信号在成交前就是可读仓位（CtaMocker.cpp:1999-2042）
	EXPECT_DOUBLE_EQ(2.0, mocker.stra_get_position(kAg));

	// json 资金：flag 0=净权益, 1=平仓盈亏, 2=浮盈, 3=手续费
	EXPECT_DOUBLE_EQ(106.25, mocker.stra_get_fund_data(0));
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_fund_data(1));
	EXPECT_DOUBLE_EQ(7.5, mocker.stra_get_fund_data(2));
	EXPECT_DOUBLE_EQ(1.25, mocker.stra_get_fund_data(3));

	mocker.handle_replay_done();

	const std::filesystem::path dir = _out / "r62_inc";
	EXPECT_EQ(2u, r62::line_count(r62::read_file(dir / "trades.csv")));
	EXPECT_EQ(2u, r62::line_count(r62::read_file(dir / "closes.csv")));
	EXPECT_EQ(2u, r62::line_count(r62::read_file(dir / "funds.csv")));
	EXPECT_EQ(2u, r62::line_count(r62::read_file(dir / "positions.csv")));
	EXPECT_EQ(2u, r62::line_count(r62::read_file(dir / "signals.csv")));
	EXPECT_NE(std::string::npos, r62::read_file(dir / "trades.csv").find("prev"));

	const std::string dump = r62::read_file(dir / "r62_inc.json");
	ASSERT_FALSE(dump.empty());
	EXPECT_NE(std::string::npos, dump.find("\"positions\""));
	EXPECT_NE(std::string::npos, dump.find("\"prev\""));
	EXPECT_NE(std::string::npos, dump.find("\"sigprev\""));
	EXPECT_NE(std::string::npos, dump.find("\"condsp\""));
	EXPECT_NE(std::string::npos, dump.find("\"action\""));
	EXPECT_NE(std::string::npos, dump.find("\"settime\""));

	// 条件单已恢复且可执行：
	// cu WCT_Equal/SP，70000 的 tick 把仓位设为 2
	feed(mocker, kCu, 70000.0);
	EXPECT_DOUBLE_EQ(2.0, mocker.stra_get_position(kCu));
	// ru WCT_Larger/OL，10001 触发开多 1 手
	feed(mocker, kRu, 10001.0);
	EXPECT_DOUBLE_EQ(1.0, mocker.stra_get_position(kRu));
	// hc WCT_Smaller/OS，3499 触发开空 2 手
	feed(mocker, kHc, 3499.0);
	EXPECT_DOUBLE_EQ(-2.0, mocker.stra_get_position(kHc));
	// 未触发的 ag 信号保持原样
	EXPECT_DOUBLE_EQ(2.0, mocker.stra_get_position(kAg));
}

// 条件单动作全覆盖：OL/CL/OS/CS/SP 各走一条独立品种，触发后由同一 tick 的
// 第二次 proc_tick 完成 do_set_position（CtaMocker.cpp:695-896、1305-1585）。
TEST_F(R62CtaMockerGap, ConditionalActionsTriggerAndFill)
{
	CtaMocker mocker(&_rp, "r62_cond_actions");

	// rb 先建 2 手多仓，再用 limit=3799 的条件平 1 手（CL）
	mocker.stra_enter_long(kRb, 2, "mkt_rb");
	feed(mocker, kRb, 3800.0);
	ASSERT_DOUBLE_EQ(2.0, mocker.stra_get_position(kRb));
	mocker.stra_exit_long(kRb, 1, "cl", 3799.0, 0.0);
	feed(mocker, kRb, 3800.0);
	EXPECT_DOUBLE_EQ(1.0, mocker.stra_get_position(kRb));

	// cu 以止损价 70000 开多（WCT_LargerOrEqual，OL）
	mocker.stra_enter_long(kCu, 3, "ol", 0.0, 70000.0);
	feed(mocker, kCu, 70100.0);
	EXPECT_DOUBLE_EQ(3.0, mocker.stra_get_position(kCu));

	// ag 先建 2 手空仓，再以 limit=6000 条件平 1 手（CS，WCT_SmallerOrEqual）
	mocker.stra_enter_short(kAg, 2, "mkt_ag");
	feed(mocker, kAg, 6000.0);
	ASSERT_DOUBLE_EQ(-2.0, mocker.stra_get_position(kAg));
	mocker.stra_exit_short(kAg, 1, "cs", 6000.0, 0.0);
	feed(mocker, kAg, 5999.0);
	EXPECT_DOUBLE_EQ(-1.0, mocker.stra_get_position(kAg));

	// ru 直接设目标仓位 4（SP，WCT_SmallerOrEqual 买入条件）
	mocker.stra_set_position(kRu, 4, "sp", 10000.0, 0.0);
	feed(mocker, kRu, 9999.0);
	EXPECT_DOUBLE_EQ(4.0, mocker.stra_get_position(kRu));

	// hc 以 limit=3500 条件开空（OS，WCT_LargerOrEqual）
	mocker.stra_enter_short(kHc, 2, "os", 3500.0, 0.0);
	feed(mocker, kHc, 3500.0);
	EXPECT_DOUBLE_EQ(-2.0, mocker.stra_get_position(kHc));

	// 手续费累计已发生；四个品种的持仓互不影响
	EXPECT_GT(mocker.stra_get_fund_data(3), 0.0);
	EXPECT_DOUBLE_EQ(1.0, mocker.stra_get_position(kRb));
	EXPECT_DOUBLE_EQ(3.0, mocker.stra_get_position(kCu));
}

// 同合约同算法的多条条件单：先登记的宽价被后登记的紧价替换，
// 触发后只成交紧价那条的数量（CtaMocker.cpp:804-846）。
TEST_F(R62CtaMockerGap, ConditionalPriorityKeepsTightestTarget)
{
	CtaMocker mocker(&_rp, "r62_cond_prio");

	mocker.stra_enter_long(kCu, 5, "loose", 0.0, 70100.0);
	mocker.stra_enter_long(kCu, 2, "tight", 0.0, 70000.0);

	feed(mocker, kCu, 70500.0);

	EXPECT_DOUBLE_EQ(2.0, mocker.stra_get_position(kCu));
	EXPECT_DOUBLE_EQ(70500.0, mocker.stra_get_position_avgpx(kCu));
	EXPECT_STREQ("tight", mocker.stra_get_last_entertag(kCu));

	// 同一轮只允许一条条件单触发：整张条件表已清空，再喂 tick 不再加仓
	feed(mocker, kCu, 70600.0);
	EXPECT_DOUBLE_EQ(2.0, mocker.stra_get_position(kCu));
}

// 主K线只能设置一次；单字符周期自动补 1；bar 事件订阅 + 主K线闭合驱动调度；
// tick 订阅/缓存/切片/品种/原始码查询（CtaMocker.cpp:1096-1214、1773-1883）。
TEST_F(R62CtaMockerGap, MainBarEventsAndTickCache)
{
	CtaMocker mocker(&_rp, "r62_bar_tick");

	EXPECT_EQ(nullptr, mocker.stra_get_bars(kRb, "m1", 5, true));
	// 第二个主K线被拒绝
	EXPECT_EQ(nullptr, mocker.stra_get_bars(kCu, "m1", 5, true));
	// 单字符周期 "m" -> key 补 "1"，非主图
	EXPECT_EQ(nullptr, mocker.stra_get_bars(kCu, "m", 5, false));

	mocker.stra_sub_bar_events(kRb, "m1");
	WTSBarStruct bar = r62::make_bar(20240102u, 930u, 3800.0);
	mocker.handle_bar_close(kRb, "m", 1, &bar);

	// 主K线已闭合 -> 本轮 emit；同一闭合只消费一次
	EXPECT_TRUE(mocker.on_schedule(20240102u, 930u));
	EXPECT_FALSE(mocker.on_schedule(20240102u, 931u));
	// 空 bar 指针早退
	mocker.handle_bar_close(kRb, "m", 1, nullptr);

	// tick 订阅 + 最新 tick 拷贝（CtaMocker.cpp:1827-1858）
	mocker.stra_sub_ticks(kRb);
	feed(mocker, kRb, 3810.0);
	WTSTickData* last = mocker.stra_get_last_tick(kRb);
	ASSERT_NE(nullptr, last);
	EXPECT_STREQ(kRb, last->code());
	EXPECT_DOUBLE_EQ(3810.0, last->price());
	last->release();

	// tick 回测关闭 -> 切片为空；未知合约无缓存
	EXPECT_EQ(nullptr, mocker.stra_get_ticks(kRb, 10));
	EXPECT_EQ(nullptr, mocker.stra_get_last_tick(kCu));

	ASSERT_NE(nullptr, mocker.stra_get_comminfo(kRb));
	EXPECT_EQ(std::string(""), mocker.stra_get_rawcode(kRb));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_day_price(kRb, 0));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_price(kRb));
	EXPECT_EQ(0u, mocker.stra_get_tdate());
	EXPECT_EQ(0u, mocker.stra_get_date());
	EXPECT_EQ(0u, mocker.stra_get_time());
	mocker.handle_section_end(20240102u, 930u);
}

// T+1 冻结在交易日开始时释放；enum_position 合并持仓表与未成交信号；
// 会话结束落 positions/funds 行，回放结束仍能落盘（CtaMocker.cpp:1215-1295）。
TEST_F(R62CtaMockerGap, SessionBeginUnfreezesAndEnumerationMergesSignals)
{
	CtaMocker mocker(&_rp, "r62_session");

	// stk_comms.json 无 trademode，默认 TM_Both（WTSBaseDataMgr.cpp:313-316）；
	// 本用例验证 T+1 冻结/解冻，需显式改为 TM_LongT1。
	WTSCommodityInfo* stkComm = _rp.get_commodity_info(kStk);
	ASSERT_NE(nullptr, stkComm);
	stkComm->setTradingMode(TM_LongT1);

	mocker.stra_enter_long(kStk, 100, "t1");
	feed(mocker, kStk, 10.0);

	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position(kStk, false));
	// 股票 T+1：当日买入全部冻结
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kStk, true));
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position(kStk, false, "t1"));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kStk, false, "other"));

	uint32_t enumCnt = 0;
	double stockQty = 0.0;
	mocker.enum_position([&](const char* code, double qty) {
		enumCnt++;
		if (strcmp(code, kStk) == 0)
			stockQty = qty;
	}, false);
	EXPECT_EQ(1u, enumCnt);
	EXPECT_DOUBLE_EQ(100.0, stockQty);

	// 未成交信号也参与枚举（以信号目标仓位为准）
	mocker.stra_enter_long(kRb, 2, "pending");
	enumCnt = 0;
	mocker.enum_position([&](const char*, double) { enumCnt++; }, false);
	EXPECT_EQ(2u, enumCnt);

	mocker.handle_session_begin(20240103u);
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position(kStk, true));
	EXPECT_EQ(0ull, mocker.stra_get_last_exittime(kStk));

	mocker.handle_session_end(20240103u);
	EXPECT_DOUBLE_EQ(2.0, mocker.stra_get_position(kRb));

	mocker.handle_replay_done();
	const std::filesystem::path dir = _out / "r62_session";
	EXPECT_EQ(2u, r62::line_count(r62::read_file(dir / "positions.csv")));
	EXPECT_EQ(3u, r62::line_count(r62::read_file(dir / "signals.csv")));
	EXPECT_NE(std::string::npos, r62::read_file(dir / "positions.csv").find(kStk));
	EXPECT_NE(std::string::npos, r62::read_file(dir / "funds.csv").find("20240103"));
}
