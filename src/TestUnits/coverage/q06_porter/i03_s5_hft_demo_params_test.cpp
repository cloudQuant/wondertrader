/*
 * i03_s5_hft_demo_params_test.cpp (迭代03 s5-runners, BUG-WSN06-002)
 *
 * 卡E：策略参数严格 schema——未知字段、缺失必填、类型/取值非法均使 init 返回
 * false；发行配置 dist/WtRunnerHft/config.yaml 的参数（含遗留未用字段 count、
 * YAML 标量均为字符串）与 JSON 数值形式均可通过。通过 dlopen 加载覆盖率构建
 * 产出的真实 HFT 策略插件（WT_R6_HFT_FACTORY_PLUGIN），不连接柜台、不下单。
 */
#include <gtest/gtest.h>

#include <string>

#include "Includes/HftStrategyDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Share/DLLHelper.hpp"

#ifndef WT_R6_HFT_FACTORY_PLUGIN
#error "WT_R6_HFT_FACTORY_PLUGIN must point to the real HFT factory plugin"
#endif

USING_NS_WTP;

namespace
{
class HftDemoPlugin
{
public:
	HftDemoPlugin()
	{
		_module = DLLHelper::load_library(WT_R6_HFT_FACTORY_PLUGIN);
		if (_module == NULL)
			return;
		_create = (FuncCreateHftStraFact)DLLHelper::get_symbol(_module, "createStrategyFact");
		_delete = (FuncDeleteHftStraFact)DLLHelper::get_symbol(_module, "deleteStrategyFact");
		if (_create != NULL)
			_fact = _create();
	}
	~HftDemoPlugin()
	{
		if (_fact != NULL && _delete != NULL)
			_delete(_fact);
		// The module stays loaded: other cases in this process may reuse it.
	}
	bool ok() const { return _fact != NULL; }

	// Returns the init() result for a fresh SimpleHft instance.
	bool init_with(WTSVariant* params)
	{
		HftStrategy* stra = _fact->createStrategy("SimpleHft", "i03s5_hft_demo");
		if (stra == NULL)
		{
			ADD_FAILURE() << "SimpleHft was not created";
			return false;
		}
		const bool result = stra->init(params);
		_fact->deleteStrategy(stra);
		return result;
	}

private:
	DllHandle _module = NULL;
	FuncCreateHftStraFact _create = NULL;
	FuncDeleteHftStraFact _delete = NULL;
	IHftStrategyFact* _fact = NULL;
};

WTSVariant* shipped_yaml_like()
{
	// dist/WtRunnerHft/config.yaml: YAML scalars arrive as strings.
	WTSVariant* params = WTSVariant::createObject();
	params->append("code", "CFFEX.IF.HOT");
	params->append("count", "50");
	params->append("offset", "1");
	params->append("second", "10");
	params->append("stock", "false");
	return params;
}
}

TEST(I03S5HftDemoParams, ShippedAndJsonFormsAreAccepted)
{
	HftDemoPlugin plugin;
	ASSERT_TRUE(plugin.ok());
	WTSVariant* yaml = shipped_yaml_like();
	EXPECT_TRUE(plugin.init_with(yaml));
	yaml->release();

	WTSVariant* json = WTSVariant::createObject();
	json->append("code", "SHFE.rb.2405");
	json->append("second", static_cast<uint32_t>(10));
	json->append("freq", static_cast<uint32_t>(0));
	json->append("offset", static_cast<uint32_t>(0));
	json->append("reserve", 0.0);
	json->append("stock", false);
	EXPECT_TRUE(plugin.init_with(json));
	json->release();

	WTSVariant* minimal = WTSVariant::createObject();
	minimal->append("code", "SHFE.rb.2405");
	EXPECT_TRUE(plugin.init_with(minimal)) << "optional fields keep their zero/false defaults";
	minimal->release();
}

TEST(I03S5HftDemoParams, MissingOrMalformedParamsAreRejected)
{
	HftDemoPlugin plugin;
	ASSERT_TRUE(plugin.ok());
	EXPECT_FALSE(plugin.init_with(NULL));

	WTSVariant* array = WTSVariant::createArray();
	EXPECT_FALSE(plugin.init_with(array));
	array->release();

	struct Case { const char* name; const char* key; const char* value; bool remove_code; };
	const Case cases[] = {
		{ "missing code", NULL, NULL, true },
		{ "unknown field", "lots", "1", false },
		{ "typo field", "secnd", "10", false },
		{ "negative second", "second", "-1", false },
		{ "non-numeric freq", "freq", "fast", false },
		{ "fractional offset", "offset", "1.5", false },
		{ "negative reserve", "reserve", "-1", false },
		{ "non-boolean stock", "stock", "maybe", false },
	};
	for (const Case& item : cases)
	{
		WTSVariant* params = WTSVariant::createObject();
		if (!item.remove_code)
			params->append("code", "CFFEX.IF.HOT");
		if (item.key != NULL)
			params->append(item.key, item.value);
		EXPECT_FALSE(plugin.init_with(params)) << item.name;
		params->release();
	}

	WTSVariant* emptyCode = WTSVariant::createObject();
	emptyCode->append("code", "");
	EXPECT_FALSE(plugin.init_with(emptyCode));
	emptyCode->release();

	WTSVariant* objectCode = WTSVariant::createObject();
	objectCode->append("code", WTSVariant::createObject(), false);
	EXPECT_FALSE(plugin.init_with(objectCode));
	objectCode->release();
}
