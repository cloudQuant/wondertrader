/**
 * q02_mocker_factory_cfg_test.cpp
 *
 * 靶子：四个 Mocker 的策略工厂装载入口（p08 明确留白：
 * 「init_uft_factory/init_hft_factory 读 use_newpx/error_rate/match_this_tick/module/strategy
 *  的正路径 … 本 lane 只覆盖 cfg==NULL 早退」）。
 *   CtaMocker::init_cta_factory  (CtaMocker.cpp:415)
 *   SelMocker::init_sel_factory  (SelMocker.cpp:234)
 *   HftMocker::init_hft_factory  (HftMocker.cpp:215)
 *   UftMocker::init_uft_factory  (UftMocker.cpp:130)
 * 全部在 DLLHelper::load_library 之前完成配置解析，用「模块文件不存在」这条
 * 唯一可确定复现的分支把 cfg->getCString/getBoolean/getUInt32/get("strategy")
 * 的读取行全部走一遍，然后返回 false。不加载任何真实动态库、不起线程。
 *
 * 落盘：temp_directory_path()/q02_fac_* （只建目录，不写仓库 dist/）。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

#include <WtBtCore/CtaMocker.h>
#include <WtBtCore/SelMocker.h>
#include <WtBtCore/HftMocker.h>
#include <WtBtCore/UftMocker.h>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>

namespace
{
	std::filesystem::path make_fac_dir()
	{
		auto path = std::filesystem::temp_directory_path() /
			("q02_fac_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		WtHelper::setOutputDir(path.string().c_str());
		return path;
	}

	/*
	 * 缺失模块的 cfg；strategy 子对象显式给出，
	 * 用来证明 load_library 早退时 createStrategy 那段根本不会被走到。
	 */
	WTSVariant* fac_cfg(const std::filesystem::path& root, const char* module,
		bool useNewPx, uint32_t errRate, bool matchThisTick)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("module", module);
		cfg->append("use_newpx", useNewPx);
		cfg->append("error_rate", errRate);
		cfg->append("match_this_tick", matchThisTick);
		WTSVariant* stra = WTSVariant::createObject();
		stra->append("name", "q02_missing_stra");
		stra->append("id", "q02_missing_id");
		cfg->append("strategy", stra);
		stra->release();
		(void)root;
		return cfg;
	}
}

class Q02FactoryTest : public ::testing::Test
{
protected:
	void SetUp() override { _dir = make_fac_dir(); }
	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}
	std::filesystem::path _dir;
};

/*
 * CtaMocker::init_cta_factory(L415-L452)：
 * cfg==NULL 与模块不存在都返回 false；两者都不应改动策略指针（_strategy 保持 NULL，
 * 后续 on_init/on_session_* 的判空分支才不会走到 createStrategy）。
 */
TEST_F(Q02FactoryTest, ctaFactoryRejectsNullAndMissingModule)
{
	CtaMocker mocker(NULL, "q02_fac_cta");
	EXPECT_FALSE(mocker.init_cta_factory(NULL));

	const std::string missing = (_dir / "q02_no_such_cta.dll").string();
	WTSVariant* cfg = fac_cfg(_dir, missing.c_str(), false, 0, false);
	EXPECT_FALSE(mocker.init_cta_factory(cfg));
	cfg->release();

	// 工厂未装载 -> _factory._fact 保持 NULL，作用域结束时 ~CtaMocker 不 free_library
}

/*
 * SelMocker::init_sel_factory(L234-L270)：同上，
 * 且模块路径为空串时 load_library 也必须返回 NULL（不是崩溃）。
 */
TEST_F(Q02FactoryTest, selFactoryRejectsEmptyAndMissingModule)
{
	SelMocker mocker(NULL, "q02_fac_sel");
	EXPECT_FALSE(mocker.init_sel_factory(NULL));

	WTSVariant* empty = fac_cfg(_dir, "", false, 0, false);
	EXPECT_FALSE(mocker.init_sel_factory(empty));
	empty->release();

	const std::string missing = (_dir / "q02_no_such_sel.dll").string();
	WTSVariant* cfg = fac_cfg(_dir, missing.c_str(), true, 33, true);
	EXPECT_FALSE(mocker.init_sel_factory(cfg));
	cfg->release();
}

/*
 * UftMocker::init_uft_factory(L130-L165)：
 * 与 Cta/Sel 的差别是它会先把 use_newpx / error_rate / match_this_tick 三个撮合参数
 * 读进成员（L137-L139）再决定装载失败，p08 只覆盖了 cfg==NULL。
 * 三个成员是私有的，本 lane 不用 #define private public，
 * 故只固化「解析过程走完 + 返回 false + 析构安全」这条可观测契约。
 */
TEST_F(Q02FactoryTest, uftFactoryParsesMatchParamsBeforeFailing)
{
	UftMocker mocker(NULL, "q02_fac_uft");
	EXPECT_FALSE(mocker.init_uft_factory(NULL));

	const std::string missing = (_dir / "q02_no_such_uft.dll").string();
	WTSVariant* cfg = fac_cfg(_dir, missing.c_str(), true, 7, false);
	EXPECT_FALSE(mocker.init_uft_factory(cfg));
	cfg->release();

	// 重复调用同一份 cfg 仍然稳定 false，且不留半成品工厂
	EXPECT_FALSE(mocker.init_uft_factory(cfg = fac_cfg(_dir, missing.c_str(), false, 1, true)));
	cfg->release();
}

/*
 * HftMocker::init_hft_factory(L215-L250)：同样先解析三个撮合参数并写日志（L226）。
 */
TEST_F(Q02FactoryTest, hftFactoryParsesMatchParamsBeforeFailing)
{
	HftMocker mocker(NULL, "q02_fac_hft");
	EXPECT_FALSE(mocker.init_hft_factory(NULL));

	WTSVariant* empty = fac_cfg(_dir, "", false, 0, false);
	EXPECT_FALSE(mocker.init_hft_factory(empty));
	empty->release();

	const std::string missing = (_dir / "q02_no_such_hft.dll").string();
	WTSVariant* cfg = fac_cfg(_dir, missing.c_str(), true, 100, true);
	EXPECT_FALSE(mocker.init_hft_factory(cfg));
	cfg->release();
}

/*
 * 缺 key 的配置：getCString("module") 缺失时返回空串而不是 NULL（若返回 NULL
 * load_library(NULL) 会直接崩，属于本用例要守住的契约），
 * getBoolean/getUInt32 缺失时取 0/false 默认值。
 */
TEST_F(Q02FactoryTest, cfgWithoutKeysFallsBackToDefaults)
{
	WTSVariant* bare = WTSVariant::createObject();
	// 完全没有 module/use_newpx/error_rate/match_this_tick/strategy
	UftMocker uft(NULL, "q02_fac_bare_uft");
	EXPECT_FALSE(uft.init_uft_factory(bare));

	HftMocker hft(NULL, "q02_fac_bare_hft");
	EXPECT_FALSE(hft.init_hft_factory(bare));

	CtaMocker cta(NULL, "q02_fac_bare_cta");
	EXPECT_FALSE(cta.init_cta_factory(bare));

	SelMocker sel(NULL, "q02_fac_bare_sel");
	EXPECT_FALSE(sel.init_sel_factory(bare));

	bare->release();
}

/*
 * 同一份 cfg 连续装载两次（都失败）不应残留半初始化的工厂状态：
 * 第二次调用同样在 load_library 处早退，_factory._fact 保持 NULL，
 * 于是 ~HftMocker/~UftMocker/~CtaMocker/~SelMocker 的 free_library 分支不会解引用。
 * 这里用「作用域结束即析构」来固化该契约（若析构崩溃，本用例会失败）。
 */
TEST_F(Q02FactoryTest, repeatedFailedLoadLeavesNoHalfInitFactory)
{
	const std::string missing = (_dir / "q02_no_such_any.dll").string();
	WTSVariant* cfg = fac_cfg(_dir, missing.c_str(), true, 5, true);
	for (uint32_t i = 0; i < 3; i++)
	{
		UftMocker uft(NULL, "q02_fac_rep_uft");
		HftMocker hft(NULL, "q02_fac_rep_hft");
		CtaMocker cta(NULL, "q02_fac_rep_cta");
		SelMocker sel(NULL, "q02_fac_rep_sel");
		EXPECT_FALSE(uft.init_uft_factory(cfg));
		EXPECT_FALSE(hft.init_hft_factory(cfg));
		EXPECT_FALSE(cta.init_cta_factory(cfg));
		EXPECT_FALSE(sel.init_sel_factory(cfg));
	}
	cfg->release();
}

/*
 * 会真的加载动态库 / 会阻塞 / 会除零的分支整族收在这里（启用集合零残留）：
 *   1) module 指向真实存在的策略 so 时，DLLHelper::load_library 成功 ->
 *      get_symbol("createStrategyFact") -> _factory._fact->createStrategy(...) ->
 *      _strategy->init(params)：需要仓库外编译产物，且不同 TU 下同名 wtp:: 类
 *      有 ODR/惰性抽取择一风险（BUG-L00-005）。
 *   2) SelMocker::handle_replay_done(L303-L314) 无条件计算
 *      `_total_calc_time / _emit_times`，而 _emit_times 构造为 0（SelMocker.cpp:43）
 *      -> 整数除零 UB：arm64 的 UDIV 不陷入（返回 0），x86 直接 SIGFPE。
 *      CtaMocker 同一位置有 `if(_emit_times > 0)` 保护（CtaMocker.cpp:665），
 *      SelMocker 没有 -> 已登记缺陷，修复前不进启用用例。
 *   3) CtaMocker/SelMocker/HftMocker 的 install_hook/enable_hook(true)/step_calc
 *      走 _cond_calc 条件变量；EventNotifier::init 起 MQ server。
 */
TEST_F(Q02FactoryTest, DISABLED_RealModuleLoadAndSelDivZeroFamily)
{
	SelMocker sel(NULL, "q02_fac_disabled_sel");
	// _emit_times == 0 -> _total_calc_time / _emit_times 整数除零
	sel.handle_replay_done();

	HftMocker hft(NULL, "q02_fac_disabled_hft");
	hft.install_hook();
	hft.enable_hook(true);

	const std::filesystem::path real = "/nonexistent-module-dir/libq02_fact.dylib";
	WTSVariant* cfg = fac_cfg(_dir, real.string().c_str(), true, 50, true);
	EXPECT_FALSE(sel.init_sel_factory(cfg));
	EXPECT_FALSE(hft.init_hft_factory(cfg));
	cfg->release();
}
