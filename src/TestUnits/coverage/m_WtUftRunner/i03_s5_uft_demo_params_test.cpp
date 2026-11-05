/*
 * i03_s5_uft_demo_params_test.cpp (迭代03 s5-runners, BUG-WSN06-002)
 *
 * 卡E：UFT 示例策略参数严格 schema。本 lane 二进制不链接任何策略工厂，因此把
 * 产品 TU WtUftStraFact/WtUftStraDemo.cpp 直接编入本测试 TU（只此一处定义
 * FACT_NAME），验证真实 init()：未知字段、缺失必填 code/lots、非法取值拒绝；
 * 发行配置 dist/WtRunnerUft/config.yaml 的参数（含遗留字段 count/level2/maxloss，
 * YAML 标量为字符串）与 JSON 数值形式通过。不连接柜台、不下单。
 */
#include <gtest/gtest.h>

#include "../../../WtUftStraFact/WtUftStraDemo.cpp"

const char* FACT_NAME = "WtUftStraFact";

namespace
{
bool init_with(WTSVariant* params)
{
	WtUftStraDemo stra("i03s5_uft_demo");
	return stra.init(params);
}

WTSVariant* shipped_yaml_like()
{
	WTSVariant* params = WTSVariant::createObject();
	params->append("code", "SHFE.ni2209");
	params->append("count", "50");
	params->append("offset", "1");
	params->append("second", "5");
	params->append("lots", "1");
	params->append("level2", "true");
	params->append("maxloss", "-10");
	return params;
}
}

TEST(I03S5UftDemoParams, ShippedAndJsonFormsAreAccepted)
{
	WTSVariant* yaml = shipped_yaml_like();
	EXPECT_TRUE(init_with(yaml));
	yaml->release();

	WTSVariant* json = WTSVariant::createObject();
	json->append("code", "SHFE.rb.2405");
	json->append("second", static_cast<uint32_t>(10));
	json->append("freq", static_cast<uint32_t>(0));
	json->append("offset", static_cast<uint32_t>(0));
	json->append("lots", 1.0);
	EXPECT_TRUE(init_with(json));
	json->release();
}

TEST(I03S5UftDemoParams, MissingOrMalformedParamsAreRejected)
{
	EXPECT_FALSE(init_with(NULL));

	struct Case { const char* name; const char* drop; const char* key; const char* value; };
	const Case cases[] = {
		{ "missing code", "code", NULL, NULL },
		{ "missing lots (used to become zero-quantity orders)", "lots", NULL, NULL },
		{ "unknown field", NULL, "reserve", "1" },
		{ "zero lots", NULL, "lots", "0" },
		{ "negative lots", NULL, "lots", "-1" },
		{ "non-numeric second", NULL, "second", "soon" },
		{ "negative offset", NULL, "offset", "-2" },
		{ "non-boolean level2", NULL, "level2", "maybe" },
		{ "non-numeric maxloss", NULL, "maxloss", "big" },
	};
	for (const Case& item : cases)
	{
		WTSVariant* params = WTSVariant::createObject();
		if (item.drop == NULL || std::string(item.drop) != "code")
			params->append("code", "SHFE.rb.2405");
		if (item.drop == NULL || std::string(item.drop) != "lots")
		{
			if (item.key == NULL || std::string(item.key) != "lots")
				params->append("lots", "1");
		}
		if (item.key != NULL)
			params->append(item.key, item.value);
		EXPECT_FALSE(init_with(params)) << item.name;
		params->release();
	}
}
