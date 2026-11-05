/*!
 * \file r54_uft_tick_metadata_test.cpp
 * \brief BUG-R54-002: ParserAdapter 合约回退元数据与 UFT 持仓盯市
 *
 * 覆盖真实 ParserAdapter -> WtUftEngine -> WtUftRtTicker -> UftStraContext 路径：
 * parser quote 缺少 contractInfo 时由基础数据管理器回退解析，并让已订阅持仓盯市。
 * 负控验证下游收到缺失 metadata 的有效行情时不崩溃、不改写持仓盈亏，并继续回调策略。
 */
#include "q04_trader_fixture.h"

#include "WtUftCore/ParserAdapter.h"
#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/WtHelper.h"
#include "WtUftCore/WtUftDtMgr.h"
#include "WtUftCore/WtUftEngine.h"

#include "Includes/IParserApi.h"
#include "Includes/UftStrategyDefs.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace wtp;

namespace {

const char* kStdCode = "SHFE.ag2406";

class SessionBaseDataMgr : public q04::FakeBaseDataMgr
{
public:
	WTSSessionInfo* _session = nullptr;

	WTSSessionInfo* getSession(const char* /*sid*/) override { return _session; }
	WTSSessionInfo* getSessionByCode(const char* /*code*/, const char* /*exchg*/ = "") override
	{
		return _session;
	}
};

class SessionOwner
{
public:
	SessionOwner()
		: _session(WTSSessionInfo::create("ALLDAY", "r54 all day", 0))
	{
		// 卡2 2b=A（用户裁定 2026-11-03）：全天时段只能写 0-0，0-2400 被拒（会话无时段）；迭代03 第五会话 int2 归因更新。
		_session->addTradingSection(0, 0);
	}

	~SessionOwner()
	{
		if (_session != nullptr)
			_session->release();
	}

	WTSSessionInfo* _session;
};

class MetadataParserApi : public IParserApi
{
public:
	bool init(WTSVariant* config) override
	{
		_init_count++;
		_init_received_null_config = config == nullptr;
		return true;
	}

	void registerSpi(IParserSpi* /*spi*/) override {}

	uint32_t _init_count = 0;
	bool _init_received_null_config = false;
};

class CountingStrategy : public UftStrategy
{
public:
	CountingStrategy(const char* id, WtUftEngine* engine) : UftStrategy(id), _engine(engine) {}

	const char* getName() override { return "r54-metadata"; }
	const char* getFactName() override { return "r54-test"; }
	void on_init(IUftStraCtx* /*ctx*/) override {}
	void on_tick(IUftStraCtx* ctx, const char* /*stdCode*/, WTSTickData* tick) override
	{
		_tick_count++;
		_last_tick = tick;
		_callback_date = ctx->stra_get_date();
		_callback_time = ctx->stra_get_time();
		_callback_secs = ctx->stra_get_secs();
		_callback_trading_date = _engine->get_trading_date();
	}

	WtUftEngine* _engine;
	uint32_t _tick_count = 0;
	WTSTickData* _last_tick = nullptr;
	uint32_t _callback_date = 0;
	uint32_t _callback_time = 0;
	uint32_t _callback_secs = 0;
	uint32_t _callback_trading_date = 0;
};

WTSTickData* makeTick(const char* code, double price)
{
	WTSTickStruct tick;
	memset(&tick, 0, sizeof(tick));
	wt_strcpy(tick.exchg, "SHFE");
	wt_strcpy(tick.code, code);
	tick.action_date = 20991231;
	tick.action_time = 93000000;
	tick.trading_date = 20991231;
	tick.price = price;
	return WTSTickData::create(tick);
}

std::string uniqueContextName()
{
	static std::atomic<uint32_t> seq(0);
	return "r54_tick_metadata_" + std::to_string((long)::getpid()) + "_" + std::to_string(++seq);
}

struct OutputFolderGuard
{
	std::string _path;
	~OutputFolderGuard()
	{
		std::error_code ec;
		std::filesystem::remove_all(_path, ec);
	}
};

struct ParserReleaseGuard
{
	ParserAdapter& _adapter;
	~ParserReleaseGuard() { _adapter.release(); }
};

} // namespace

TEST(BugR54002UftTickMetadata, parserFallbackMarksPositionAndMissingMetadataStillForwards)
{
	q04::TempRoot temp("r54_tick_metadata");
	WtHelper::setGenerateDir((temp.dir().string() + "/").c_str());

	const std::string contextName = uniqueContextName();
	const std::string outputFolder = std::string(WtHelper::getOutputDir()) + contextName + "/";
	std::error_code ec;
	std::filesystem::remove_all(outputFolder, ec);
	std::filesystem::create_directories(outputFolder, ec);
	ASSERT_FALSE(ec) << outputFolder;
	OutputFolderGuard outputFolderGuard{ outputFolder };
	{
		std::ofstream manual(outputFolder + "mannual.yaml");
		ASSERT_TRUE(manual.good());
		manual << R"JSON({
  "details": [
    {"exchg":"SHFE","code":"ag2406","direct":0,"volume":2,"openprice":100}
  ]
})JSON";
	}

	SessionOwner session;
	SessionBaseDataMgr baseData;
	baseData._session = session._session;
	WTSContractInfo* contract = baseData.add("SHFE", "ag", "ag2406", 0, 0,
		CM_OpenCover, TM_Both, 10);
	ASSERT_NE(nullptr, contract);
	WtUftDtMgr dataMgr;
	WtUftEngine engine;
	engine.init(nullptr, &baseData, &dataMgr, nullptr);

	CountingStrategy strategy(contextName.c_str(), &engine);
	auto context = std::make_shared<UftStraContext>(&engine, contextName.c_str());
	context->set_strategy(&strategy);
	context->on_channel_ready(20991231);
	ASSERT_DOUBLE_EQ(2.0, context->stra_get_local_position(kStdCode));
	engine.addContext(context);
	context->stra_sub_ticks(kStdCode);

	// engine.run() installs its production ticker; ALLDAY keeps the test isolated
	// from wall-clock session boundaries and lets ParserAdapter use engine as its stub.
	engine.run();
	ParserAdapter adapter;
	ParserReleaseGuard parserRelease{ adapter };
	auto* parserApi = new MetadataParserApi();
	ASSERT_TRUE(adapter.initExt("r54-tick-metadata", parserApi, &engine, &baseData));
	EXPECT_EQ(1u, parserApi->_init_count);
	EXPECT_TRUE(parserApi->_init_received_null_config); // initExt's external-API contract

	const uint32_t contractRefs = contract->retainCount();
	// The first quote after ticker startup exercises fallback metadata and position marking.
	WTSTickData* quote = makeTick("ag2406", 110.0); // parser tick begins without ContractInfo
	ASSERT_NE(nullptr, quote);
	ASSERT_EQ(nullptr, quote->getContractInfo());
	adapter.handleQuote(quote, 0);

	EXPECT_STREQ(kStdCode, quote->code());
	EXPECT_EQ(contract, quote->getContractInfo());
	EXPECT_EQ(1u, strategy._tick_count);
	EXPECT_EQ(quote, strategy._last_tick);
	EXPECT_EQ(20991231u, strategy._callback_date);
	EXPECT_EQ(930u, strategy._callback_time);
	EXPECT_EQ(0u, strategy._callback_secs);
	EXPECT_EQ(20991231u, strategy._callback_trading_date);
	EXPECT_DOUBLE_EQ(200.0, context->stra_get_local_posprofit(kStdCode));
	EXPECT_EQ(contractRefs, contract->retainCount()); // borrowed metadata is never retained/released here
	quote->release();

	// Downstream negative control: a valid quote can arrive without metadata from
	// another producer. It must reach the strategy without corrupting existing PnL.
	WTSTickData* missingMetadata = makeTick(kStdCode, 120.0);
	ASSERT_NE(nullptr, missingMetadata);
	ASSERT_EQ(nullptr, missingMetadata->getContractInfo());
	engine.handle_push_quote(missingMetadata);
	EXPECT_EQ(2u, strategy._tick_count);
	EXPECT_EQ(missingMetadata, strategy._last_tick);
	EXPECT_DOUBLE_EQ(200.0, context->stra_get_local_posprofit(kStdCode));
	EXPECT_DOUBLE_EQ(120.0, engine.get_cur_price(kStdCode));
	missingMetadata->release();

	// A contract pointer can also be present while its commodity metadata is absent.
	// Keep the base-data-manager-owned contract borrowed and restore its fixture data
	// immediately after the synchronous callback.
	WTSCommodityInfo* commodity = contract->getCommInfo();
	ASSERT_NE(nullptr, commodity);
	const uint32_t refsBeforeMissingCommodity = contract->retainCount();
	contract->setCommInfo(nullptr);
	WTSTickData* missingCommodity = makeTick(kStdCode, 130.0);
	ASSERT_NE(nullptr, missingCommodity);
	missingCommodity->setContractInfo(contract);
	engine.handle_push_quote(missingCommodity);
	EXPECT_EQ(3u, strategy._tick_count);
	EXPECT_EQ(missingCommodity, strategy._last_tick);
	EXPECT_DOUBLE_EQ(200.0, context->stra_get_local_posprofit(kStdCode));
	EXPECT_DOUBLE_EQ(130.0, engine.get_cur_price(kStdCode));
	EXPECT_EQ(refsBeforeMissingCommodity, contract->retainCount());
	contract->setCommInfo(commodity);
	missingCommodity->release();

	adapter.release();
	engine.stop();
}
