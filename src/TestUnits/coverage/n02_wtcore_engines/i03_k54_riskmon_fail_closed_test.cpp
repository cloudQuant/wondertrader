/**
 * 迭代03 K54（BUG-R211-001，用户裁定 2026-11-06）：`riskmon.active=true` 而风控模块加载失败时拒绝启动。
 *
 * 引擎层合同：WtEngine::init 之后 `riskmon_config_valid()`（非虚，不改 init 的虚函数签名）
 *  - 未配置 riskmon：true（行为不变，组合资金按 5 秒更新）；
 *  - riskmon.active=false：true（不加载、不算失败）；
 *  - riskmon.active=true 但模块不存在 / 不兼容 / 无工厂 / 无该监控器：false。
 * WtRunner 与 WtPorter 据此让初始化返回失败、不进入交易（见 porter 侧 i03_k54 用例）。
 */
#include <TestUnits/coverage/q01_wtcore3/q01_sel_env.h>

#include <Includes/WTSRiskDef.hpp>
#include <WTSUtils/WTSCfgLoader.h>

#include <WtCore/TraderAdapter.h>
#include <WtCore/WtCtaEngine.h>
#include <WtCore/WtDtMgr.h>

#include <string>

using namespace wtp;

namespace {

class Probe : public WtCtaEngine
{
public:
	~Probe()
	{
		if (_port_fund != NULL)
		{
			_port_fund->release();
			_port_fund = NULL;
		}
	}
	void attach(IBaseDataMgr* bd, IHotMgr* hot, WtDtMgr* dm)
	{
		_base_data_mgr = bd;
		_hot_mgr = hot;
		_data_mgr = dm;
		set_date_time(20250110, 931, 0);
		set_trading_date(20250110);
	}
};

class I03K54RiskmonFixture : public ::testing::Test
{
protected:
	q01sel::Q01TempEnv env{"i03k54"};
	q01sel::HotMgr hot;
	q01sel::BaseDataMgr bd;
	WtDtMgr dm;

	bool initWith(const char* riskmonJson)
	{
		Probe engine;
		engine.attach(&bd, &hot, &dm);
		std::string cfgText = "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"";
		if (riskmonJson != NULL)
			cfgText += std::string(",\"riskmon\":") + riskmonJson;
		cfgText += "}";
		WTSVariant* cfg = WTSCfgLoader::load_from_content(cfgText.c_str());
		EXPECT_NE(nullptr, cfg);
		if (cfg == NULL)
			return false;
		engine.init(cfg, &bd, &dm, &hot, NULL);
		cfg->release();
		const bool valid = engine.riskmon_config_valid();
		engine.release();
		return valid;
	}
};

} // namespace

TEST_F(I03K54RiskmonFixture, NoRiskmonSectionIsValid)
{
	EXPECT_TRUE(initWith(NULL));
}

TEST_F(I03K54RiskmonFixture, InactiveRiskmonIsValidWithoutLoading)
{
	EXPECT_TRUE(initWith("{\"active\":false,\"module\":\"i03_k54_no_such_riskmon\",\"name\":\"x\"}"));
}

TEST_F(I03K54RiskmonFixture, ActiveRiskmonWithMissingModuleIsInvalid)
{
	EXPECT_FALSE(initWith("{\"active\":true,\"module\":\"i03_k54_no_such_riskmon\",\"name\":\"x\"}"));
}

TEST_F(I03K54RiskmonFixture, ActiveRiskmonWithIncompatibleModuleIsInvalid)
{
	// 系统 libz 能加载，但没有 createRiskMonFact 入口 → 不兼容
	EXPECT_FALSE(initWith("{\"active\":true,\"module\":\"/usr/lib/libz.1.dylib\",\"name\":\"x\"}"));
}
