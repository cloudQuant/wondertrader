/**
 * r111_cta_mocker_plugin_test.cpp  (lane R111)
 *
 * 靶子：src/WtBtCore/CtaMocker.cpp
 *   init_cta_factory 成功路径（L433-451）：真实构建产物 libWtCtaStraFact.dylib + DualThrust
 *   策略回调分发：on_init/on_bar_close/on_tick_updated/on_schedule/session begin/end
 *   （L968、L1023-1024、L1033-1034、L1039-1040、L1237-1238、L1266-1267）
 *   比率滑点分支（L1638-1651）：ratio=true 时按成交价百分比 + pricetick 取整
 *
 * 此前 R92 因「不加载真实策略 DLL」把整族留白；本 lane 只加载本机构建产物，
 * 不起线程、不联网、不接柜台。测试只用 ICtaStraCtx 公开方法。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cmath>
#include <filesystem>
#include <string>

#include <Includes/WTSContractInfo.hpp>
#include <WtBtCore/CtaMocker.h>
#include <WtBtCore/WtHelper.h>

#include "r111_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";

	WTSVariant* make_factory_cfg(const std::string& module, const char* code,
		bool includeParams = true, const char* strategyName = "DualThrust")
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("module", module.c_str());

		WTSVariant* stra = WTSVariant::createObject();
		stra->append("name", strategyName);
		stra->append("id", "r111_cta_plug");

		if (includeParams)
		{
			WTSVariant* params = WTSVariant::createObject();
			params->append("days", static_cast<uint32_t>(2));
			params->append("k1", 0.5);
			params->append("k2", 0.5);
			params->append("period", "d");
			params->append("count", static_cast<uint32_t>(5));
			params->append("code", code);
			params->append("stock", false);
			stra->append("params", params);
			params->release();
		}

		cfg->append("strategy", stra);
		stra->release();
		return cfg;
	}

	class FailedDeleteCtaStrategy : public CtaStrategy
	{
	public:
		explicit FailedDeleteCtaStrategy(const char* id) : CtaStrategy(id) {}
		const char* getName() override { return "FailedDelete"; }
		const char* getFactName() override { return "FailedDeleteFact"; }
	};

	class FailedDeleteCtaFact : public ICtaStrategyFact
	{
	public:
		~FailedDeleteCtaFact() override { delete _strategy; }
		const char* getName() override { return "FailedDeleteFact"; }
		void enumStrategy(FuncEnumStrategyCallback) override {}
		CtaStrategy* createStrategy(const char*, const char* id) override
		{
			_strategy = new FailedDeleteCtaStrategy(id);
			return _strategy;
		}
		bool deleteStrategy(CtaStrategy*) override
		{
			++_delete_attempts;
			return false;
		}
		uint32_t delete_attempts() const { return _delete_attempts; }

	private:
		FailedDeleteCtaStrategy* _strategy = nullptr;
		uint32_t _delete_attempts = 0;
	};

	class PreservedCtaMocker : public CtaMocker
	{
	public:
		PreservedCtaMocker(HisDataReplayer* replayer, const char* name)
			: CtaMocker(replayer, name, 0, false) {}

		void installFailedDeleteStrategy()
		{
			_factory._fact = &_fact;
			_strategy = _fact.createStrategy("FailedDelete", "r111_failed_delete");
		}
		void triggerStrategyDelete() { destroy_strategy(); }
		bool pluginWasPreserved() const { return _factory._preserve_module; }
		uint32_t deleteAttempts() const { return _fact.delete_attempts(); }

	private:
		FailedDeleteCtaFact _fact;
	};
}

class R111CtaMocker : public r62::ReplayFixture
{
protected:
	std::string module() const
	{
		return r111::plugin_path("WtCtaStraFact");
	}
};

// 工厂装载成功 + 六个策略回调 + 回放收尾落盘
TEST_F(R111CtaMocker, FactoryCreateAndStrategyDispatch)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	CtaMocker mocker(&_rp, "r111_cta_dispatch", 0, false);
	WTSVariant* cfg = make_factory_cfg(module(), kRb);
	ASSERT_TRUE(mocker.init_cta_factory(cfg));
	cfg->release();

	// on_init：stra_sub_ticks + stra_get_bars（无分钟数据时提前返回）
	mocker.handle_init();

	// 登记主K线并闭合一根日线，使 on_schedule 能进入 on_calculate 与策略
	// DualThrust::on_schedule 的 set_index_value/add_chart_mark 分支
	WTSKlineSlice* mainBars = mocker.stra_get_bars(kRb, "d", 5, true);
	if (mainBars != nullptr)
		mainBars->release();
	mocker.stra_sub_bar_events(kRb, "d1");
	WTSBarStruct bar = r62::make_bar(20200102u, 1500u, 10.0);
	mocker.handle_bar_close(kRb, "d", 1, &bar);

	// 订阅并分发 tick（L1033-1034）
	mocker.stra_sub_ticks(kRb);
	WTSTickData* tick = r62::make_tick(kRb, 20200102u, 93100000u, 3800.0, 3799.0, 3801.0);
	mocker.handle_tick(kRb, tick, 0);
	tick->release();

	// on_schedule / session begin / session end（L1039-1040、L1237-1238、L1266-1267）
	mocker.handle_session_begin(20200103u);
	mocker.handle_schedule(20200102u, 931u);
	mocker.handle_session_end(20200103u);
	mocker.handle_replay_done();

	// 全程无信号成交，策略回调不应改变账户
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kRb));
}

// 宿主 CtaMocker 通过 FuncDeleteStraFact 调用真实插件的按值 deleteStrategyFact，
// 作用域结束时必须能正常释放工厂；不创建策略，隔离验证 host-factory teardown。
TEST_F(R111CtaMocker, FactoryDestructorUsesPluginByValueDeleteAbi)
{
	const std::string plugin = module();
	ASSERT_TRUE(std::filesystem::exists(plugin)) << plugin;

	CtaMocker mocker(&_rp, "r111_cta_factory_dtor", 0, false);
	WTSVariant* cfg = make_factory_cfg(plugin, kRb);
	const bool initialized = mocker.init_cta_factory(cfg);
	cfg->release();
	ASSERT_TRUE(initialized);

	// mocker 的析构先销毁策略，再调用真实插件工厂删除器并卸载模块。
}

TEST_F(R111CtaMocker, FactoryRejectsMissingModuleAndCanRetry)
{
	CtaMocker mocker(&_rp, "r111_cta_missing_module", 0, false);
	WTSVariant* invalid = WTSVariant::createObject();
	EXPECT_FALSE(mocker.init_cta_factory(invalid));
	invalid->release();

	WTSVariant* valid = make_factory_cfg(module(), kRb);
	EXPECT_TRUE(mocker.init_cta_factory(valid));
	valid->release();
}

TEST_F(R111CtaMocker, FactoryRejectsMissingStrategyAndUnknownStrategy)
{
	const std::string plugin = module();
	ASSERT_TRUE(std::filesystem::exists(plugin)) << plugin;

	{
		CtaMocker mocker(&_rp, "r111_cta_no_strategy", 0, false);
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("module", plugin.c_str());
		EXPECT_FALSE(mocker.init_cta_factory(cfg));
		cfg->release();
	}
	{
		CtaMocker mocker(&_rp, "r111_cta_unknown_strategy", 0, false);
		WTSVariant* cfg = make_factory_cfg(plugin, kRb, true, "NoSuchStrategy");
		EXPECT_FALSE(mocker.init_cta_factory(cfg));
		cfg->release();
	}
}

TEST_F(R111CtaMocker, FactoryRejectsStrategyInitializationFailure)
{
	const std::string plugin = module();
	ASSERT_TRUE(std::filesystem::exists(plugin)) << plugin;

	CtaMocker mocker(&_rp, "r111_cta_init_failure", 0, false);
	WTSVariant* cfg = make_factory_cfg(plugin, kRb, false);
	EXPECT_FALSE(mocker.init_cta_factory(cfg));
	cfg->release();
}

TEST_F(R111CtaMocker, FailedStrategyDeletePinsPluginAndRejectsInitializationRetry)
{
	const std::string plugin = module();
	ASSERT_TRUE(std::filesystem::exists(plugin)) << plugin;

	PreservedCtaMocker mocker(&_rp, "r111_cta_failed_delete");
	mocker.installFailedDeleteStrategy();
	mocker.triggerStrategyDelete();
	EXPECT_TRUE(mocker.pluginWasPreserved());
	EXPECT_EQ(mocker.deleteAttempts(), 1u);

	WTSVariant* cfg = make_factory_cfg(plugin, kRb);
	EXPECT_FALSE(mocker.init_cta_factory(cfg));
	cfg->release();
	EXPECT_EQ(mocker.deleteAttempts(), 1u);
}


// 比率滑点：fill = trdPx + round(slippage*trdPx/10000/pricetick)*pricetick（L1638-1651）
TEST_F(R111CtaMocker, RatioSlippageOnSignalFill)
{
	WTSCommodityInfo* comm = _rp.get_commodity_info(kRb);
	ASSERT_NE(nullptr, comm);
	const double price_tick = comm->getPriceTick();
	ASSERT_GT(price_tick, 0.0);

	CtaMocker mocker(&_rp, "r111_cta_ratio", 10, false, nullptr, true);
	mocker.stra_set_position(kRb, 1.0, "ratio");

	WTSTickData* tick = r62::make_tick(kRb, 20200102u, 93100000u, 3800.0, 3799.0, 3801.0);
	mocker.handle_tick(kRb, tick, 0);
	tick->release();

	ASSERT_DOUBLE_EQ(1.0, mocker.stra_get_position(kRb));

	const double expected_slip = std::round(10.0 * 3800.0 / 10000.0 / price_tick) * price_tick;
	const double expected_fill = 3800.0 + expected_slip;
	EXPECT_DOUBLE_EQ(expected_fill, mocker.stra_get_last_enterprice(kRb));
	EXPECT_GT(mocker.stra_get_last_enterprice(kRb), 3800.0);

	// 明细 tag 不命中：遍历 continue 后返回 0（L2122-2123）
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_detail_profit(kRb, "missing-tag", 0));
}
