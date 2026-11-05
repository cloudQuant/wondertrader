// 迭代03 卡3 3b=C（默认 A）：ParserCTP / ParserCTPMini / ParserFemas 的 ActionDay 夜盘纠偏（BUG-R105-001）。
// 本 TU 通过 WT_C3_PARSER 选择一个 parser；产品 parser TU 以 WT_ACTDAY_TEST_CLOCK 编译时，
// wt_actday::localDateTime 读取这里的可控本地时钟；基线（改动前）parser 不读该时钟。
// 标 [GREEN-ONLY] 的用例依赖新时钟钩子或新配置项，红构建（WT_GREEN=OFF）不编译。
// 只用本地 fake API，不连接真实前置、不调用真实厂商 SDK。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Includes/IBaseDataMgr.h"
#include "Includes/IParserApi.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "Share/TimeUtils.hpp"
#include "WTSUtils/WTSCfgLoader.h"
#if defined(WT_C3_GREEN)
#include "Share/ActionDayNormalizer.hpp"
#endif

#if WT_C3_PARSER == 1
#define private public
#include "ParserCTP/ParserCTP.h"
#undef private
#include "r105_ctp_api_stub.h"
typedef ParserCTP C3Parser;
typedef CThostFtdcDepthMarketDataField C3Depth;
typedef R105CtpMdApiStub C3Api;
#elif WT_C3_PARSER == 2
#include "r105_ctpmini_api_stub.h"
#define private public
#include "ParserCTPMini/ParserCTPMini.h"
#undef private
typedef ParserCTPMini C3Parser;
typedef CThostFtdcDepthMarketDataField C3Depth;
typedef R105CtpMiniMdApiStub C3Api;
#elif WT_C3_PARSER == 3
#include "r105_femas_api_stub.h"
#define private public
#include "ParserFemas/ParserFemas.h"
#undef private
typedef ParserFemas C3Parser;
typedef CUstpFtdcDepthMarketDataField C3Depth;
typedef R105FemasMdApiStub C3Api;
#else
#error "WT_C3_PARSER must be 1, 2 or 3"
#endif

USING_NS_WTP;

namespace
{
	uint32_t g_fakeDate = 0;
	uint32_t g_fakeTime = 0;
	int g_clockReads = 0;
}

namespace wt_actday
{
	// 产品头在 WT_ACTDAY_TEST_CLOCK 下声明；基线 parser 不引用它（定义无副作用）
	void testLocalDateTime(uint32_t& date, uint32_t& time)
	{
		++g_clockReads;
		date = g_fakeDate;
		time = g_fakeTime;
	}
}

namespace
{
	const char* kExchg = "SHFE";
	const char* kCode = "rb2601";

	class C3DataMgr : public IBaseDataMgr
	{
	public:
		WTSContractInfo* ct = NULL;
		WTSCommodityInfo* getCommodity(const char*) override { return NULL; }
		WTSCommodityInfo* getCommodity(const char*, const char*) override { return NULL; }
		WTSContractInfo* getContract(const char* code, const char* = "", uint32_t = 0) override
		{
			return (code != NULL && std::strcmp(code, kCode) == 0) ? ct : NULL;
		}
		WTSArray* getContracts(const char* = "", uint32_t = 0) override { return NULL; }
		WTSSessionInfo* getSession(const char*) override { return NULL; }
		WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return NULL; }
		WTSArray* getAllSessions() override { return NULL; }
		bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
		uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate; }
		uint64_t getBoundaryTime(const char*, uint32_t tDate, bool = false, bool = true) override { return tDate; }
	};

	class C3Sink : public IParserSpi
	{
	public:
		C3DataMgr bd;
		std::vector<WTSTickStruct> ticks;
		IBaseDataMgr* getBaseDataMgr() override { return &bd; }
		void handleSymbolList(const WTSArray*) override {}
		void handleQuote(WTSTickData* quote, uint32_t) override
		{
			if (quote != NULL)
				ticks.push_back(quote->getTickStruct());
		}
		void handleParserLog(WTSLogLevel, const char*) override {}
		void handleEvent(WTSParserEvent, int32_t) override {}
	};

	class C3ActDay : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			comm_ = WTSCommodityInfo::create("rb", "Rebar", kExchg, "TRADING", "CHINA");
			comm_->setCategory(CC_Future);
			comm_->setVolScale(10);
			ct_ = WTSContractInfo::create(kCode, "Rebar2601", kExchg, "rb");
			ct_->setCommInfo(comm_);
			sink_.bd.ct = ct_;
			parser_.registerSpi(&sink_);
			parser_.m_pUserAPI = &api_;
			g_clockReads = 0;
		}
		void TearDown() override
		{
			parser_.m_pUserAPI = NULL;
			ct_->release();
			comm_->release();
		}

		void login(const char* tradingDay)
		{
			api_.trading_day = tradingDay;
			parser_.OnRspUserLogin(NULL, NULL, 1, true);
			ASSERT_EQ((uint32_t)std::stoul(tradingDay), parser_.m_uTradingDate);
		}

		// 返回 sink 收到的 tick 数（0 表示被丢弃）
		size_t push(const char* actionDay, const char* updateTime, uint32_t localDate, uint32_t localTime)
		{
			g_fakeDate = localDate;
			g_fakeTime = localTime;
			C3Depth d;
			std::memset(&d, 0, sizeof(d));
			std::strncpy(d.InstrumentID, kCode, sizeof(d.InstrumentID) - 1);
#if WT_C3_PARSER != 3
			std::strncpy(d.ExchangeID, kExchg, sizeof(d.ExchangeID) - 1);
#endif
			std::strncpy(d.ActionDay, actionDay, sizeof(d.ActionDay) - 1);
			std::strncpy(d.UpdateTime, updateTime, sizeof(d.UpdateTime) - 1);
			d.LastPrice = 3500.0;
			d.Volume = 10;
			parser_.OnRtnDepthMarketData(&d);
			return sink_.ticks.size();
		}

		uint32_t lastActDate() const { return sink_.ticks.empty() ? 0 : sink_.ticks.back().action_date; }

		C3Api api_;
		C3Parser parser_;
		C3Sink sink_;
		WTSCommodityInfo* comm_ = NULL;
		WTSContractInfo* ct_ = NULL;
	};
}

// 卡3 3b 默认 A：行情 00:30、本地 23:30、ActionDay=TradingDay -> 本地次日（自然日）。
// 周五 20261106 夜盘，交易日为下周一 20261109：发生日应为周六 20261107。
TEST_F(C3ActDay, MidnightTickWithLocalLaggingUsesLocalNextDay)
{
	login("20261109");
	ASSERT_EQ(1u, push("20261109", "00:30:00", 20261106, 233000000));
	EXPECT_EQ(20261107u, lastActDate());
}

// 行情 00:30、本地 00:30、ActionDay=TradingDay -> 本地日期
TEST_F(C3ActDay, MidnightTickWithLocalAlignedUsesLocalDate)
{
	login("20261109");
	ASSERT_EQ(1u, push("20261109", "00:30:00", 20261107, 3000000));
	EXPECT_EQ(20261107u, lastActDate());
}

// 03–09 时过滤语义同样适用于 00 时行情：早盘启动后收到的隔夜残留丢弃
TEST_F(C3ActDay, MidnightTickAtMorningStartupIsDropped)
{
	login("20261109");
	EXPECT_EQ(0u, push("20261109", "00:30:00", 20261109, 51500000));
}

// 正控：ActionDay != TradingDay 时不纠偏（两侧一致）
TEST_F(C3ActDay, MidnightTickWithDistinctActionDayIsKept)
{
	login("20261109");
	ASSERT_EQ(1u, push("20261107", "00:30:00", 20261106, 233000000));
	EXPECT_EQ(20261107u, lastActDate());
}

// 正控：普通日盘 ActionDay=TradingDay 不纠偏（两侧一致）
TEST_F(C3ActDay, DaySessionTickIsKept)
{
	login("20261109");
	ASSERT_EQ(1u, push("20261109", "10:00:00", 20261109, 100000000));
	EXPECT_EQ(20261109u, lastActDate());
	EXPECT_EQ(0, g_clockReads) << "day-session ticks must not read the local clock";
}

#if defined(WT_C3_GREEN)
// [GREEN-ONLY] 原 23 时纠偏语义保持：本地 23:30 -> 本地日期；本地 00:10 -> 本地前一天；03–09 时丢弃
TEST_F(C3ActDay, EveningTickKeepsLegacyCorrection)
{
	login("20261109");
	ASSERT_EQ(1u, push("20261109", "23:30:00", 20261106, 233000000));
	EXPECT_EQ(20261106u, lastActDate());
	ASSERT_EQ(2u, push("20261109", "23:59:00", 20261107, 1000000));
	EXPECT_EQ(20261106u, lastActDate());
	EXPECT_EQ(2u, push("20261109", "23:30:00", 20261109, 60000000));
	EXPECT_GE(g_clockReads, 3);
}

// [GREEN-ONLY] 可配置：midnight_actionday=legacy（卡3 3b 选项 B）保持旧行为，00 时不进入纠偏
TEST_F(C3ActDay, LegacyModeKeepsActionDayAtMidnight)
{
	parser_.m_iMidnightMode = (int)wt_actday::parseMode("legacy");
	login("20261109");
	ASSERT_EQ(1u, push("20261109", "00:30:00", 20261106, 233000000));
	EXPECT_EQ(20261109u, lastActDate());
	EXPECT_EQ(0, g_clockReads);
}

// [GREEN-ONLY] 默认/未知配置值为 A
TEST(C3ActDayHelper, ParseModeDefaultsToNextLocalDay)
{
	EXPECT_EQ(wt_actday::MidnightMode::NextLocalDay, wt_actday::parseMode(""));
	EXPECT_EQ(wt_actday::MidnightMode::NextLocalDay, wt_actday::parseMode(NULL));
	EXPECT_EQ(wt_actday::MidnightMode::NextLocalDay, wt_actday::parseMode("A"));
	EXPECT_EQ(wt_actday::MidnightMode::Legacy, wt_actday::parseMode("B"));
	EXPECT_EQ(wt_actday::MidnightMode::Legacy, wt_actday::parseMode("legacy"));
}

// [GREEN-ONLY] 配置入口：init 读取 midnight_actionday
TEST_F(C3ActDay, InitReadsMidnightModeConfig)
{
	WTSVariant* cfg = WTSCfgLoader::load_from_content(
		"front: tcp://127.0.0.1:1\nbroker: 9999\nuser: c3\npass: x\nflowdir: /tmp/wt-i03-card3-20261104-flow\nmidnight_actionday: legacy\n", true);
	ASSERT_NE(nullptr, cfg);
	parser_.m_pUserAPI = NULL;
#if WT_C3_PARSER == 1
	r105_ctp::factory_target() = &api_;
#endif
	parser_.init(cfg);
	cfg->release();
	EXPECT_EQ((int)wt_actday::MidnightMode::Legacy, parser_.m_iMidnightMode);
#if WT_C3_PARSER == 1
	r105_ctp::factory_target() = NULL;
	parser_.disconnect();
#endif
	parser_.m_pUserAPI = &api_;
}
#endif
