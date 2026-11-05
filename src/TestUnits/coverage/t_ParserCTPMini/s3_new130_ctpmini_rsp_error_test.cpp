/*
 * s3_new130_ctpmini_rsp_error_test.cpp — I03-NEW-130（BUG-R105-004，原 BUG-T4-010）
 *
 * 合同：ParserCTPMini::IsErrorRspInfo 与同族 ParserCTPOpt/ParserFemas 一致，
 * pRspInfo 非空且 ErrorID != 0 才视为错误应答。
 *   - 错误的登录末包不得进入成功分支：不取交易日、不发 WPE_Login、不下发订阅；
 *   - ErrorID == 0 或 pRspInfo == NULL 的登录末包照常成功；
 *   - 错误应答写一条错误日志（OnRspError 只经该谓词产生可见效果）。
 *
 * 前置（合法 API 会话）：与 r105_parserctpmini_api_test.cpp 相同，测试侧白盒把
 * m_pUserAPI 指向 r105_ctpmini_api_stub.h 的可控空实现，再走公开 connect 与
 * OnFrontConnected（发出登录请求）后投递登录应答。不调 init、不加载厂商库、
 * 不打开 socket、不起线程。
 *
 * 与 DISABLED_login_success_branch_dereferences_null_api（ctpmini_parser_test.cpp）
 * 的区别：该例在未 connect、m_pUserAPI 为空的 parser 上投递应答，不属于 SDK 生命周期
 * 内的输入（BUG-T4-004 NOT_BUG 口径），故另写本文件的合法前置用例。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "r105_ctpmini_api_stub.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/IParserApi.h"
#include "Includes/WTSContractInfo.hpp"

#define private public
#include "ParserCTPMini/ParserCTPMini.h"
#undef private

USING_NS_WTP;

namespace s3n130 {

class DataMgr : public IBaseDataMgr
{
public:
	WTSCommodityInfo* getCommodity(const char*) override { return NULL; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return NULL; }
	WTSContractInfo* getContract(const char*, const char* = "", uint32_t = 0) override { return NULL; }
	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return NULL; }
	WTSSessionInfo* getSession(const char*) override { return NULL; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return NULL; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate; }
	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool = false, bool = true) override { return tDate; }
};

class Sink : public IParserSpi
{
public:
	DataMgr bd;
	std::vector<int> events;
	std::vector<std::pair<int, std::string>> logs;

	IBaseDataMgr* getBaseDataMgr() override { return &bd; }
	void handleSymbolList(const WTSArray*) override {}
	void handleQuote(WTSTickData*, uint32_t) override {}
	void handleParserLog(WTSLogLevel ll, const char* msg) override
	{
		logs.emplace_back((int)ll, msg == NULL ? "" : msg);
	}
	void handleEvent(WTSParserEvent e, int32_t) override { events.push_back((int)e); }

	size_t count(WTSParserEvent e) const
	{
		size_t n = 0;
		for (int v : events)
			if (v == (int)e)
				++n;
		return n;
	}
	bool hasErrorLog(const char* needle) const
	{
		for (const auto& lg : logs)
			if (lg.first == (int)LL_ERROR && lg.second.find(needle) != std::string::npos)
				return true;
		return false;
	}
};

class S3New130CtpMiniRspError : public ::testing::Test
{
protected:
	void SetUp() override
	{
		parser_.m_pUserAPI = &api_;
		parser_.m_strFrontAddr = "tcp://127.0.0.1:1";
		parser_.m_strBroker = "9999";
		parser_.m_strUserID = "s3n130";
		parser_.m_strPassword = "secret";
		parser_.m_iRequestID = 0;
		parser_.m_uTradingDate = 0;
		parser_.registerSpi(&sink_);
		api_.trading_day = "20260301";
	}
	void TearDown() override
	{
		parser_.m_pUserAPI = NULL;
	}

	/// 合法会话前置：connect 后前置机连上，parser 发出一次登录请求；行情过滤先缓存
	void bringToLoginRequested()
	{
		ASSERT_TRUE(parser_.connect());
		CodeSet codes;
		codes.insert("SHFE.rb2401");
		parser_.subscribe(codes);
		ASSERT_EQ(0, api_.sub_calls);
		parser_.OnFrontConnected();
		ASSERT_EQ(1, api_.login_calls);
		ASSERT_EQ(1u, sink_.count(WPE_Connect));
	}

	static CThostFtdcRspInfoField rsp(int id, const char* msg)
	{
		CThostFtdcRspInfoField info;
		std::memset(&info, 0, sizeof(info));
		info.ErrorID = id;
		std::strncpy(info.ErrorMsg, msg, sizeof(info.ErrorMsg) - 1);
		return info;
	}

	R105CtpMiniMdApiStub api_;
	ParserCTPMini parser_;
	Sink sink_;
};

}	// namespace s3n130

using s3n130::S3New130CtpMiniRspError;

TEST_F(S3New130CtpMiniRspError, error_login_last_packet_does_not_enter_success_branch)
{
	bringToLoginRequested();

	CThostFtdcRspUserLoginField login;
	std::memset(&login, 0, sizeof(login));
	CThostFtdcRspInfoField err = rsp(3, "invalid login");
	parser_.OnRspUserLogin(&login, &err, api_.last_reqid, true);

	EXPECT_EQ(0u, sink_.count(WPE_Login));
	EXPECT_EQ(0u, parser_.m_uTradingDate);
	EXPECT_EQ(0, api_.sub_calls);
	EXPECT_TRUE(sink_.hasErrorLog("error id 3"));

	// 仍未登录：之后的订阅只更新缓存，不向 API 下发
	CodeSet more;
	more.insert("SHFE.rb2405");
	parser_.subscribe(more);
	EXPECT_EQ(0, api_.sub_calls);
}

TEST_F(S3New130CtpMiniRspError, zero_error_id_login_still_succeeds)
{
	bringToLoginRequested();

	CThostFtdcRspUserLoginField login;
	std::memset(&login, 0, sizeof(login));
	CThostFtdcRspInfoField ok = rsp(0, "CTP:no error");
	parser_.OnRspUserLogin(&login, &ok, api_.last_reqid, true);

	EXPECT_EQ(1u, sink_.count(WPE_Login));
	EXPECT_EQ(20260301u, parser_.m_uTradingDate);
	ASSERT_EQ(1, api_.sub_calls);
	EXPECT_EQ(1u, api_.sub_codes.count("rb2401"));
	EXPECT_FALSE(sink_.hasErrorLog("error id"));
}

TEST_F(S3New130CtpMiniRspError, null_rspinfo_login_still_succeeds)
{
	bringToLoginRequested();

	CThostFtdcRspUserLoginField login;
	std::memset(&login, 0, sizeof(login));
	parser_.OnRspUserLogin(&login, NULL, api_.last_reqid, true);

	EXPECT_EQ(1u, sink_.count(WPE_Login));
	EXPECT_EQ(20260301u, parser_.m_uTradingDate);
	EXPECT_EQ(1, api_.sub_calls);
	EXPECT_FALSE(sink_.hasErrorLog("error id"));
}

TEST_F(S3New130CtpMiniRspError, rejected_login_does_not_block_a_later_successful_login)
{
	bringToLoginRequested();

	CThostFtdcRspUserLoginField login;
	std::memset(&login, 0, sizeof(login));
	CThostFtdcRspInfoField err = rsp(75, "too many sessions");
	parser_.OnRspUserLogin(&login, &err, api_.last_reqid, true);
	ASSERT_EQ(0u, sink_.count(WPE_Login));

	// 柜台断开重连：SDK 再次回调 OnFrontConnected，parser 重新发起登录
	parser_.OnFrontDisconnected(0x1001);
	parser_.OnFrontConnected();
	ASSERT_EQ(2, api_.login_calls);
	CThostFtdcRspInfoField ok = rsp(0, "");
	parser_.OnRspUserLogin(&login, &ok, api_.last_reqid, true);

	EXPECT_EQ(1u, sink_.count(WPE_Login));
	EXPECT_EQ(20260301u, parser_.m_uTradingDate);
	EXPECT_EQ(1, api_.sub_calls);
}

TEST_F(S3New130CtpMiniRspError, rsp_error_is_logged_only_for_nonzero_error_id)
{
	bringToLoginRequested();

	parser_.OnRspError(NULL, 9, true);
	CThostFtdcRspInfoField ok = rsp(0, "");
	parser_.OnRspError(&ok, 10, true);
	EXPECT_FALSE(sink_.hasErrorLog("error id"));

	CThostFtdcRspInfoField err = rsp(90, "request rejected");
	parser_.OnRspError(&err, 11, true);
	EXPECT_TRUE(sink_.hasErrorLog("error id 90"));
	EXPECT_EQ(0u, sink_.count(WPE_Login));
}
