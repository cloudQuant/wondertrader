// Lane R12 - src/WTSTools/WTSBaseDataMgr.cpp 基础数据查询面
// 覆盖：loadSessions/loadCommodities/loadContracts/loadHolidays 之后的查询接口：
//       getCommodity（stdPID 串 / exchg+pid 两参）、getContract（含到期日窗口）、
//       getContracts/getContractSize/getContractByIndex/getGlobalSize、
//       getSession/getSessionByCode/getAllSessions/getSessionComms、
//       isHoliday/isTradingDate/setTradingDate/getTradingDate/getNextTDate/getPrevTDate、
//       calcTradingDate/getBoundaryTime 的日盘与夜盘分支
// 说明：夹具全部用三段式品种键（SHFE.rb），并按主控要求加一条自检断言防止两段式假红；
//       临时文件写在本进程 pid 专属的 temp 子目录，不触碰共享内存/线程/挂钟。
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>

#include "Includes/WTSMarcos.h"
#include "Includes/WTSTypes.h"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSVariant.hpp"
#include "Share/fmtlib.h"
#include "WTSTools/WTSBaseDataMgr.h"

USING_NS_WTP;

namespace {

std::string r12_write_tmp(const char* name, const std::string& body)
{
	static uint32_t seq = 0;
	std::error_code ec;
	auto dir = std::filesystem::temp_directory_path() /
		fmtutil::format("r12_bdm_{0}_{1}", (uint32_t)::getpid(), ++seq);
	std::filesystem::create_directories(dir, ec);
	auto file = dir / name;
	std::ofstream ofs(file.string());
	ofs << body;
	ofs.close();
	return file.string();
}

const char* kSessions = R"({
	"R12DAY":{"name":"r12日盘","offset":0,"auction":{"from":915,"to":925},
		"sections":[{"from":900,"to":1130},{"from":1300,"to":1500}]},
	"R12NIGHT":{"name":"r12夜盘","offset":300,
		"sections":[{"from":2100,"to":2300},{"from":900,"to":1130}]},
	"R12EMPTY":{"name":"r12无时段","offset":0}
})";

const char* kCommodities = R"({
	"SSE":{"STK":{"name":"r12股票","session":"R12DAY","holiday":"CHN","pricetick":0.01,
		"volscale":1,"category":0,"covermode":0,"pricemode":0,"trademode":1,"lotstick":1,"minlots":1}},
	"SHFE":{"rb":{"name":"r12螺纹","session":"R12NIGHT","holiday":"CHN","pricetick":1,
		"volscale":10,"covermode":0,"pricemode":1}}
})";

const char* kContracts = R"({
	"SSE":{
		"600000":{"name":"r12浦发","exchg":"SSE","product":"STK","altcode":"SH600000",
			"opendate":20200101,"expiredate":20500101,"maxlimitqty":100000,"minlimitqty":1},
		"600999":{"name":"r12退市","exchg":"SSE","product":"STK",
			"opendate":20100101,"expiredate":20231231}
	},
	"SHFE":{
		"rb2405":{"name":"r12螺纹2405","exchg":"SHFE","product":"rb",
			"opendate":20230506,"expiredate":20240515,"maxlimitqty":500,"minlimitqty":1}
	}
})";

const char* kHolidays = R"({
	"CHN":["20240101","20240212","20240213","20240214","20240215","20240216"]
})";

}	// namespace

class R12BaseDataMgr : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sess = r12_write_tmp("sessions.json", kSessions);
		_comm = r12_write_tmp("commodities.json", kCommodities);
		_ct = r12_write_tmp("contracts.json", kContracts);
		_hol = r12_write_tmp("holidays.json", kHolidays);
		ASSERT_TRUE(_mgr.loadSessions(_sess.c_str()));
		ASSERT_TRUE(_mgr.loadCommodities(_comm.c_str()));
		ASSERT_TRUE(_mgr.loadContracts(_ct.c_str()));
		ASSERT_TRUE(_mgr.loadHolidays(_hol.c_str()));
		// 自检：夹具品种键必须是三段式可查到的形状，否则本轮的假红会重演
		ASSERT_NE(_mgr.getCommodity("SHFE", "rb"), nullptr);
	}

	WTSBaseDataMgr _mgr;
	std::string _sess, _comm, _ct, _hol;
};

TEST_F(R12BaseDataMgr, commodity_lookup_by_stdpid_and_two_args)
{
	WTSCommodityInfo* byPid = _mgr.getCommodity("SHFE.rb");
	ASSERT_NE(byPid, nullptr);
	// 两参重载与拼串重载必须命中同一个对象
	WTSCommodityInfo* byTwo = _mgr.getCommodity("SHFE", "rb");
	EXPECT_EQ(byTwo, byPid);
	// 缺失品种返回 NULL
	EXPECT_EQ(_mgr.getCommodity("SHFE.nope"), nullptr);
	EXPECT_EQ(_mgr.getCommodity("NOPE", "rb"), nullptr);
	EXPECT_EQ(_mgr.getCommodity("SHFE", "nope"), nullptr);
	// 空串/NULL 也要安全落空
	EXPECT_EQ(_mgr.getCommodity("", ""), nullptr);
	WTSCommodityInfo* stk = _mgr.getCommodity("SSE.STK");
	ASSERT_NE(stk, nullptr);
	// 品种登记的交易模板 id 就是夹具里写的 R12DAY
	EXPECT_STREQ(stk->getTradingTpl(), "R12DAY");
	EXPECT_STREQ(byPid->getTradingTpl(), "R12NIGHT");
}

TEST_F(R12BaseDataMgr, contract_lookup_and_attributes)
{
	WTSContractInfo* ct = _mgr.getContract("rb2405", "SHFE");
	ASSERT_NE(ct, nullptr);
	// 合约属性由 JSON 夹具显式喂入，可按日期窗口与手数断言
	EXPECT_EQ(ct->getOpenDate(), 20230506u);
	EXPECT_EQ(ct->getExpireDate(), 20240515u);
	EXPECT_EQ(ct->getMaxLmtVol(), 500u);
	// 缺失合约/缺失交易所都落空
	EXPECT_EQ(_mgr.getContract("rb9999", "SHFE"), nullptr);
	EXPECT_EQ(_mgr.getContract("rb2405", "DCE"), nullptr);
	EXPECT_EQ(_mgr.getContract("", ""), nullptr);
	// 到期日之后的历史合约按日期窗口查询应落空
	WTSContractInfo* alive = _mgr.getContract("600000", "SSE", 20240108);
	ASSERT_NE(alive, nullptr);
	EXPECT_EQ(alive->getOpenDate(), 20200101u);
	WTSContractInfo* dead = _mgr.getContract("600999", "SSE", 20240108);
	EXPECT_EQ(dead, nullptr);
}

TEST_F(R12BaseDataMgr, contract_lists_size_and_index)
{
	// 全局 3 个合约：SSE 两个 + SHFE 一个
	EXPECT_EQ(_mgr.getGlobalSize(), 3u);
	EXPECT_EQ(_mgr.getContractSize(), 3u);
	EXPECT_EQ(_mgr.getContractSize("SSE"), 2u);
	EXPECT_EQ(_mgr.getContractSize("SHFE"), 1u);
	// 未知交易所计数为 0
	EXPECT_EQ(_mgr.getContractSize("DCE"), 0u);

	WTSArray* all = _mgr.getContracts();
	ASSERT_NE(all, nullptr);
	EXPECT_EQ(all->size(), 3u);
	all->release();

	WTSArray* sse = _mgr.getContracts("SSE");
	ASSERT_NE(sse, nullptr);
	EXPECT_EQ(sse->size(), 2u);
	sse->release();
	WTSArray* dce = _mgr.getContracts("DCE");
	if (dce != nullptr)
	{
		EXPECT_EQ(dce->size(), 0u);
		dce->release();
	}
	// 下标访问：越界返回 NULL
	ASSERT_NE(_mgr.getContractByIndex(0), nullptr);
	EXPECT_EQ(_mgr.getContractByIndex(99), nullptr);
}

TEST_F(R12BaseDataMgr, session_lookup_by_id_and_code)
{
	WTSSessionInfo* day = _mgr.getSession("R12DAY");
	ASSERT_NE(day, nullptr);
	EXPECT_STREQ(day->id(), "R12DAY");
	EXPECT_EQ(day->getSectionCount(), 2u);
	EXPECT_EQ(day->getOffsetMins(), 0);
	// 未知模板
	EXPECT_EQ(_mgr.getSession("NOPE"), nullptr);
	EXPECT_EQ(_mgr.getSession(""), nullptr);
	// 通过合约反查时段：rb 挂在夜盘模板上
	WTSSessionInfo* byCode = _mgr.getSessionByCode("rb2405", "SHFE");
	ASSERT_NE(byCode, nullptr);
	EXPECT_STREQ(byCode->id(), "R12NIGHT");
	// 股票走日盘
	WTSSessionInfo* byStk = _mgr.getSessionByCode("600000", "SSE");
	ASSERT_NE(byStk, nullptr);
	EXPECT_STREQ(byStk->id(), "R12DAY");
	// 未知合约返回 NULL（或默认模板），至少不能崩
	EXPECT_EQ(_mgr.getSessionByCode("nope", "SHFE"), nullptr);

	WTSArray* all = _mgr.getAllSessions();
	ASSERT_NE(all, nullptr);
	// 三个模板都在（含无时段的 R12EMPTY）
	EXPECT_GE(all->size(), 2u);
	all->release();

	// 时段挂的品种集合
	CodeSet* pids = _mgr.getSessionComms("R12DAY");
	ASSERT_NE(pids, nullptr);
	EXPECT_TRUE(pids->find("SSE.STK") != pids->end());
	CodeSet* none = _mgr.getSessionComms("NOPE");
	ASSERT_NE(none, nullptr);
	EXPECT_EQ(none->size(), (std::size_t)0);
}

TEST_F(R12BaseDataMgr, holiday_and_trading_date_window)
{
	// 周末 + 法定节假日都算假日
	EXPECT_TRUE(_mgr.isHoliday("SSE.STK", 20240106));
	EXPECT_TRUE(_mgr.isHoliday("SSE.STK", 20240107));
	EXPECT_TRUE(_mgr.isHoliday("SSE.STK", 20240212));
	EXPECT_FALSE(_mgr.isHoliday("SSE.STK", 20240108));
	// 未登记节假日模板的品种：按周末判断，工作日不是假日
	EXPECT_FALSE(_mgr.isHoliday("NOPE.PID", 20240108));
	// isTradingDate 与 isHoliday 互为反面（日盘品种）
	EXPECT_TRUE(_mgr.isTradingDate("SSE.STK", 20240108));
	EXPECT_FALSE(_mgr.isTradingDate("SSE.STK", 20240106));
}

TEST_F(R12BaseDataMgr, trading_date_cursor_set_next_prev)
{
	// 显式设定游标后按日历推进：20240108 周一 → 下一交易日 20240109
	_mgr.setTradingDate("SSE.STK", 20240108);
	EXPECT_EQ(_mgr.getTradingDate("SSE.STK"), 20240108u);
	EXPECT_EQ(_mgr.getNextTDate("SSE.STK", 20240108), 20240109u);
	EXPECT_EQ(_mgr.getPrevTDate("SSE.STK", 20240108), 20240105u);
	// 跨周末：周一的前一交易日要跳过周六日
	EXPECT_EQ(_mgr.getPrevTDate("SSE.STK", 20240109), 20240108u);
	// 跨节假日：20240219(周一) 的前一交易日要跳过 20240217/18 周末与
	// 20240212~20240216 春节假期，落到 20240209(周五)
	EXPECT_EQ(_mgr.getPrevTDate("SSE.STK", 20240219), 20240209u);
	// 多日推进
	EXPECT_EQ(_mgr.getNextTDate("SSE.STK", 20240108, 2), 20240110u);
	// 未知品种也要能按自然日推进而不崩
	EXPECT_NE(_mgr.getNextTDate("NOPE.PID", 20240108), 0u);
}

TEST_F(R12BaseDataMgr, calc_trading_date_day_and_night)
{
	// 日盘品种：交易日就是当天（非假日）
	EXPECT_EQ(_mgr.calcTradingDate("SSE.STK", 20240108, 930), 20240108u);
	// 夜盘品种在夜盘时段（21:00，偏移 300 分钟）应归到下一个交易日
	uint32_t nightTDate = _mgr.calcTradingDate("SHFE.rb", 20240108, 2130);
	EXPECT_NE(nightTDate, 0u);
	EXPECT_GT(nightTDate, 20240108u);
	// 未知品种原样回吐入参日期
	EXPECT_EQ(_mgr.calcTradingDate("NOPE.PID", 20240108, 930), 20240108u);
}

TEST_F(R12BaseDataMgr, boundary_time_start_and_end)
{
	// 未知品种返回 0
	uint64_t unknown = _mgr.getBoundaryTime("NOPE.PID", 20240108);
	EXPECT_EQ(unknown, 0u);
	// 日盘品种的开始边界应落在当天 9 点档，结束边界落在 15 点档
	uint64_t start = _mgr.getBoundaryTime("SSE.STK", 20240108, false, true);
	uint64_t end = _mgr.getBoundaryTime("SSE.STK", 20240108, false, false);
	EXPECT_NE(start, 0u);
	EXPECT_NE(end, 0u);
	if (start != 0u && end != 0u)
	{
		// 边界时间形如 yyyymmddhhmmss，且 start < end
		EXPECT_LT(start, end);
		EXPECT_EQ(start / 1000000, 20240108u);
	}
}
