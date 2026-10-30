/**
 * r111_uft_mocker_plugin_test.cpp  (lane R111)
 *
 * 靶子：src/WtBtCore/UftMocker.cpp
 *   init_uft_factory 成功路径（L154-166）：本机构建产物 WtUftRunner/libWtUftStraFact.dylib
 *     + 演示策略 SimpleUft
 *   析构删除策略（L62-64）、handle_session_begin/end（L206-213）
 *   事件分发：on_bar/on_tick/on_order_queue/on_order_detail/on_transaction/on_init
 *     （L225、L296、L307、L318、L329、L340）
 *   on_session_begin 冻结释放 / on_session_end 持仓与资金日志（L344-390）
 *   IMatchSink 回调（L804、L813、L819、L825）、stra_enum_position（L1031-1047）
 *   update_position 未注册品种早退（L1170-1172）
 *
 * R92 因不加载策略 DLL 整族留白；本 lane 加载本地插件，不联网、不起线程。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

#include <Includes/WTSContractInfo.hpp>
#include <WtBtCore/UftMocker.h>
#include <WtBtCore/WtHelper.h>

#include "r111_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
	const char* const kCu = "SHFE.cu.2406";
	const char* const kStk = "SSE.STK.000001";

	std::vector<std::string> g_factory_events;
	bool g_strategy_init_result = false;

	class ControlledUftStrategy final : public UftStrategy
	{
	public:
		explicit ControlledUftStrategy(const char* id) : UftStrategy(id) {}
		~ControlledUftStrategy() override { g_factory_events.emplace_back("delete_strategy"); }
		const char* getName() override { return "ControlledUft"; }
		const char* getFactName() override { return "ControlledUftFactory"; }
		bool init(WTSVariant*) override
		{
			g_factory_events.emplace_back("init_strategy");
			return g_strategy_init_result;
		}
		void on_init(IUftStraCtx*) override {}
	};

	class ControlledUftFactory final : public IUftStrategyFact
	{
	public:
		~ControlledUftFactory() override { g_factory_events.emplace_back("delete_factory"); }
		const char* getName() override { return "ControlledUftFactory"; }
		void enumStrategy(FuncEnumUftStrategyCallback) override {}
		UftStrategy* createStrategy(const char* name, const char* id) override
		{
			g_factory_events.emplace_back("create_strategy");
			if (name == nullptr || std::string(name) != "ControlledUft")
				return nullptr;
			return new ControlledUftStrategy(id);
		}
		bool deleteStrategy(UftStrategy* strategy) override
		{
			g_factory_events.emplace_back("delete_strategy_request");
			delete strategy;
			return true;
		}
	};

	IUftStrategyFact* create_controlled_uft_factory()
	{
		g_factory_events.emplace_back("create_factory");
		return new ControlledUftFactory();
	}

	void delete_controlled_uft_factory(IUftStrategyFact* factory)
	{
		delete factory;
	}

	WTSVariant* make_factory_cfg(const std::string& module, const char* code,
		const char* strategyName = "SimpleUft")
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("module", module.c_str());

		WTSVariant* stra = WTSVariant::createObject();
		if (strategyName != nullptr)
			stra->append("name", strategyName);
		stra->append("id", "r111_uft_plug");

		WTSVariant* params = WTSVariant::createObject();
		params->append("code", code);
		params->append("second", static_cast<uint32_t>(10));
		params->append("freq", static_cast<uint32_t>(0));
		params->append("offset", static_cast<uint32_t>(0));
		params->append("lots", 1.0);
		stra->append("params", params);
		params->release();

		cfg->append("strategy", stra);
		stra->release();
		return cfg;
	}

	WTSVariant* make_module_cfg(const std::string& module)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("module", module.c_str());
		return cfg;
	}

	WTSVariant* make_controlled_uft_cfg()
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("module", "controlled_fixture");
		WTSVariant* stra = WTSVariant::createObject();
		stra->append("name", "ControlledUft");
		stra->append("id", "controlled_uft_id");
		WTSVariant* params = WTSVariant::createObject();
		stra->append("params", params);
		params->release();
		cfg->append("strategy", stra);
		stra->release();
		return cfg;
	}
}

static_assert(std::is_same<FuncCreateUftStraFact, IUftStrategyFact*(*)()>::value,
	"UFT factory creator must match the repository declaration");
static_assert(std::is_same<FuncDeleteUftStraFact, void(*)(IUftStrategyFact*)>::value,
	"UFT factory deleter must match the repository by-value declaration");

struct UftMockerTestAccess
{
	static bool initWithCallbacks(UftMocker& mocker, WTSVariant* cfg,
		FuncCreateUftStraFact creator, FuncDeleteUftStraFact remover)
	{
		return mocker.init_uft_factory_impl(cfg, nullptr, "controlled_fixture", creator, remover);
	}

	static bool hasFactory(const UftMocker& mocker) { return mocker._factory._fact != nullptr; }
	static bool hasStrategy(const UftMocker& mocker) { return mocker._strategy != nullptr; }
	static bool hasModule(const UftMocker& mocker) { return mocker._factory._module_inst != nullptr; }
};

class R111UftMocker : public r62::ReplayFixture
{
protected:
	std::string module() const
	{
		return r111::plugin_path("WtUftStraFact", "WtUftRunner");
	}
};

// 工厂装载 + on_init + 六类事件分发 + IMatchSink 回调 + 析构删策略
TEST_F(R111UftMocker, FactoryCreateAndEventDispatch)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	UftMocker mocker(&_rp, "r111_uft_dispatch");
	WTSVariant* cfg = make_factory_cfg(module(), kRb);
	ASSERT_TRUE(mocker.init_uft_factory(cfg));
	EXPECT_TRUE(UftMockerTestAccess::hasFactory(mocker));
	EXPECT_TRUE(UftMockerTestAccess::hasStrategy(mocker));
	cfg->release();

	// on_init：watch_param/commit/stra_get_bars/stra_sub_ticks（L340）
	mocker.handle_init();
	// handle_session_begin/end 转发（L206-213）
	mocker.handle_session_begin(20200102u);
	mocker.handle_session_end(20200102u);

	// 订阅后分发 tick（L296）
	mocker.stra_sub_ticks(kCu);
	WTSTickData* tick = r62::make_tick(kCu, 20200102u, 93100000u, 70000.0, 69990.0, 70010.0);
	mocker.handle_tick(kCu, tick, 0);
	tick->release();

	// Level2 三类分发（L307/318/329）
	mocker.handle_order_queue(kCu, nullptr);
	mocker.handle_order_detail(kCu, nullptr);
	mocker.handle_transaction(kCu, nullptr);

	// 日线闭合（L225）
	WTSBarStruct bar = r62::make_bar(20200102u, 1500u, 10.0);
	mocker.handle_bar_close(kCu, "m1", 1, &bar);

	// IMatchSink 回调（L819/804/813/825）
	mocker.on_entrust(1u, kCu, true, "ok");
	mocker.on_order(2u, kCu, true, 0u, 1.0, 1.0, 70000.0, false);
	mocker.on_trade(3u, kCu, true, 0u, 1.0, 70000.0);
	mocker.on_channel_ready();

	EXPECT_DOUBLE_EQ(1.0, mocker.stra_get_position(kCu));
}

TEST_F(R111UftMocker, FactoryRejectsMissingStrategyAndEmptyOrMissingName)
{
	const std::string plugin = module();
	ASSERT_TRUE(std::filesystem::exists(plugin)) << plugin;

	{
		UftMocker mocker(&_rp, "r111_uft_no_strategy");
		WTSVariant* cfg = make_module_cfg(plugin);
		EXPECT_FALSE(mocker.init_uft_factory(cfg));
		EXPECT_FALSE(UftMockerTestAccess::hasModule(mocker));
		EXPECT_FALSE(UftMockerTestAccess::hasFactory(mocker));
		EXPECT_FALSE(UftMockerTestAccess::hasStrategy(mocker));
		cfg->release();
	}
	{
		UftMocker mocker(&_rp, "r111_uft_empty_strategy_name");
		WTSVariant* cfg = make_factory_cfg(plugin, kRb, "");
		EXPECT_FALSE(mocker.init_uft_factory(cfg));
		EXPECT_FALSE(UftMockerTestAccess::hasModule(mocker));
		EXPECT_FALSE(UftMockerTestAccess::hasFactory(mocker));
		EXPECT_FALSE(UftMockerTestAccess::hasStrategy(mocker));
		cfg->release();
	}
	{
		UftMocker mocker(&_rp, "r111_uft_missing_strategy_name");
		WTSVariant* cfg = make_factory_cfg(plugin, kRb, nullptr);
		EXPECT_FALSE(mocker.init_uft_factory(cfg));
		EXPECT_FALSE(UftMockerTestAccess::hasModule(mocker));
		EXPECT_FALSE(UftMockerTestAccess::hasFactory(mocker));
		EXPECT_FALSE(UftMockerTestAccess::hasStrategy(mocker));
		cfg->release();
	}
}

TEST_F(R111UftMocker, FactoryRejectsUnknownStrategyAndCleansPlugin)
{
	const std::string plugin = module();
	ASSERT_TRUE(std::filesystem::exists(plugin)) << plugin;

	UftMocker mocker(&_rp, "r111_uft_unknown_strategy");
	WTSVariant* cfg = make_factory_cfg(plugin, kRb, "NoSuchStrategy");
	EXPECT_FALSE(mocker.init_uft_factory(cfg));
	EXPECT_FALSE(UftMockerTestAccess::hasModule(mocker));
	EXPECT_FALSE(UftMockerTestAccess::hasFactory(mocker));
	EXPECT_FALSE(UftMockerTestAccess::hasStrategy(mocker));
	cfg->release();
}

TEST_F(R111UftMocker, FactoryRejectsStrategyInitFalseAndCleansInOrder)
{
	g_factory_events.clear();
	g_strategy_init_result = false;
	WTSVariant* cfg = make_controlled_uft_cfg();
	{
		UftMocker mocker(nullptr, "r111_uft_init_false");
		EXPECT_FALSE(UftMockerTestAccess::initWithCallbacks(
			mocker, cfg, create_controlled_uft_factory, delete_controlled_uft_factory));
		EXPECT_FALSE(UftMockerTestAccess::hasFactory(mocker));
		EXPECT_FALSE(UftMockerTestAccess::hasStrategy(mocker));
		EXPECT_EQ((std::vector<std::string>{
			"create_factory", "create_strategy", "init_strategy",
			"delete_strategy_request", "delete_strategy", "delete_factory"}), g_factory_events);
	}
	cfg->release();
}

TEST_F(R111UftMocker, ControlledSuccessfulInitDeletesStrategyBeforeFactory)
{
	g_factory_events.clear();
	g_strategy_init_result = true;
	WTSVariant* cfg = make_controlled_uft_cfg();
	{
		UftMocker mocker(nullptr, "r111_uft_controlled_success");
		ASSERT_TRUE(UftMockerTestAccess::initWithCallbacks(
			mocker, cfg, create_controlled_uft_factory, delete_controlled_uft_factory));
		ASSERT_TRUE(UftMockerTestAccess::hasFactory(mocker));
		ASSERT_TRUE(UftMockerTestAccess::hasStrategy(mocker));
		EXPECT_EQ((std::vector<std::string>{
			"create_factory", "create_strategy", "init_strategy"}), g_factory_events);
	}
	EXPECT_EQ((std::vector<std::string>{
		"create_factory", "create_strategy", "init_strategy",
		"delete_strategy_request", "delete_strategy", "delete_factory"}), g_factory_events);
	cfg->release();
}

// 交易日开始释放冻结、结束写持仓/资金日志；未注册品种成交早退
TEST_F(R111UftMocker, SessionFreezeReleaseEnumAndUnknownCommodity)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	WTSCommodityInfo* stkComm = _rp.get_commodity_info(kStk);
	ASSERT_NE(nullptr, stkComm);
	stkComm->setTradingMode(TM_LongT1);

	UftMocker mocker(&_rp, "r111_uft_pos");
	WTSVariant* cfg = make_factory_cfg(module(), kRb);
	ASSERT_TRUE(mocker.init_uft_factory(cfg));
	cfg->release();

	// 开 100 股 T+1：今仓可用为 0
	mocker.on_trade(1u, kStk, true, 0u, 100.0, 10.0);
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position(kStk, false));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kStk, true));

	// 跨日释放冻结（L350-358），随后枚举持仓（L1031-1047）
	mocker.handle_session_begin(20200103u);
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position(kStk, true));
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_enum_position(kStk));

	// 未注册品种：update_position 在 commInfo==NULL 处早退（L1170-1172）
	mocker.on_trade(2u, "SHFE.xx.9999", true, 0u, 1.0, 100.0);
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position("SHFE.xx.9999"));

	// 会话结束：遍历非零持仓写 positions/funds（L364-390）
	mocker.handle_session_end(20200103u);
	mocker.handle_replay_done();

	const std::filesystem::path dir = _out / "r111_uft_pos";
	ASSERT_TRUE(std::filesystem::exists(dir / "trades.csv"));
	ASSERT_TRUE(std::filesystem::exists(dir / "positions.csv"));
	ASSERT_TRUE(std::filesystem::exists(dir / "funds.csv"));
}
