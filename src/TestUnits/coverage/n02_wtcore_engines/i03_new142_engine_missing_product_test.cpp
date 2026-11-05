/**
 * 迭代03 I03-NEW-142：env 配置缺 `product` 段时，CTA/HFT/SEL 引擎 run() 解引用空指针崩溃
 * （WtCtaEngine.cpp:81 `cfgProd->getCString("session")`，cfgProd 为 NULL）。
 * K56 用例在改前红态下暴露：WtPorter 不预检 env.product，配置通过后 run 即 SIGSEGV。
 *
 * 合同（fail-closed，与费用模板无效时一致）：缺 `product` 段时 run() 打 ERROR 并拒绝启动，
 * 不创建 ticker、不写启动标记 marker.json、不崩溃；带 `product` 段时照常启动（正控：同一夹具
 * 补上 product 后 marker.json 被写出）。
 */
#include <TestUnits/coverage/q01_wtcore3/q01_sel_env.h>

#include <Includes/WTSRiskDef.hpp>
#include <WTSUtils/WTSCfgLoader.h>

#include <WtCore/TraderAdapter.h>
#include <WtCore/WtCtaEngine.h>
#include <WtCore/WtDtMgr.h>
#include <WtCore/WtHftEngine.h>
#include <WtCore/WtSelEngine.h>

#include <filesystem>
#include <string>

using namespace wtp;

namespace {

namespace fs = std::filesystem;

template <typename Engine>
class Probe : public Engine
{
public:
	~Probe()
	{
		if (this->_port_fund != NULL)
		{
			this->_port_fund->release();
			this->_port_fund = NULL;
		}
	}
	void attach(IBaseDataMgr* bd, IHotMgr* hot, WtDtMgr* dm)
	{
		this->_base_data_mgr = bd;
		this->_hot_mgr = hot;
		this->_data_mgr = dm;
		this->set_date_time(20250110, 931, 0);
		this->set_trading_date(20250110);
	}
};

std::string marker_file() { return std::string(WtHelper::getBaseDir()) + "marker.json"; }

class I03New142Fixture : public ::testing::Test
{
protected:
	q01sel::Q01TempEnv env{"i03new142"};
	q01sel::HotMgr hot;
	q01sel::BaseDataMgr bd;
	WtDtMgr dm;
	TraderAdapterMgr adapters;

	void TearDown() override
	{
		std::error_code ec;
		fs::remove(marker_file(), ec);
	}

	// withProduct=false：期望拒绝启动；true：正控，期望照常启动并写出 marker.json
	template <typename Engine>
	void runOnce(bool withProduct)
	{
		std::error_code ec;
		fs::remove(marker_file(), ec);
		bd.regisSessionWithTime("TRADING", 900, 1500);
		Probe<Engine> engine;
		engine.attach(&bd, &hot, &dm);
		engine.set_adapter_mgr(&adapters);
		WTSVariant* cfg = WTSCfgLoader::load_from_content(withProduct
			? "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"}"
			: "{\"fees\":\"\",\"filters\":\"\"}");
		ASSERT_NE(nullptr, cfg);
		engine.init(cfg, &bd, &dm, &hot, NULL);
		cfg->release();
		ASSERT_TRUE(engine.fee_config_valid());

		engine.run();
		EXPECT_EQ(withProduct, fs::exists(marker_file()));
		engine.release();
	}
};

} // namespace

TEST_F(I03New142Fixture, CtaRunWithoutProductIsRejected) { runOnce<WtCtaEngine>(false); }
TEST_F(I03New142Fixture, HftRunWithoutProductIsRejected) { runOnce<WtHftEngine>(false); }
TEST_F(I03New142Fixture, SelRunWithoutProductIsRejected) { runOnce<WtSelEngine>(false); }
TEST_F(I03New142Fixture, CtaRunWithProductStarts) { runOnce<WtCtaEngine>(true); }
TEST_F(I03New142Fixture, HftRunWithProductStarts) { runOnce<WtHftEngine>(true); }
TEST_F(I03New142Fixture, SelRunWithProductStarts) { runOnce<WtSelEngine>(true); }
