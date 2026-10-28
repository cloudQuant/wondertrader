/*
 * R46 - WtBtRunner 回测引导链补测
 *
 * 目标：src/WtBtPorter/WtBtRunner.cpp
 *   - init() L437-447（WTSLogger::init 守卫、setInstDir/setOutputDir）
 *   - config() L449-462（空内容/坏 JSON 的早退）、L464-471 + L500-507（mode=none）、
 *     L472-479（mode=cta 创建 ExpCtaMocker 并尝试 init_cta_factory）
 *   - initCtaMocker() L266-283、initSelMocker() L299-313（含二次创建替换旧 mocker）
 *   - stop() L550-559（未启动时的早退）
 *
 * 与既有 lane 的分工：q06/r9 只覆盖回调分派、feed 链、config 文件缺失与
 * initEvtNotifier 的拒绝分支；config 的「内容解析成功」段、mocker 创建、stop 早退
 * 在报告里仍未覆盖。q06 的 config 用例还会断言单例 mockers 为空，所以本文件
 * 一律用堆上 new、故意不 delete 的 runner，绝不触碰 getRunner() 单例的 mocker 槽位。
 *
 * 注意：WtBtRunner 构造列表没有初始化 _hft_mocker/_exec_mocker/_worker/_cfg/
 * _loader_auto_trans（BUG-R46-001），堆实例只能走不读这些槽位的入口：
 * 不调用 initHftMocker()/isAutoTrans()/release()/run()，stop() 走单例静态零初始化。
 * 所有落盘都在系统临时目录的专属子目录，用例末尾 remove_all。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "WtBtPorter/WtBtPorter.h"
#include "WtBtPorter/WtBtRunner.h"

#include "WtBtCore/WtHelper.h"

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace {

	const char* kOutLeaf = "wt_r46_btporter_out";

	std::string prepare_out_dir()
	{
		std::filesystem::path p = std::filesystem::temp_directory_path() / kOutLeaf;
		std::error_code ec;
		std::filesystem::create_directories(p, ec);
		return p.string();
	}

	void clean_dir(const std::string& dir)
	{
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}

	std::string tail_char(const std::string& s)
	{
		return s.empty() ? std::string() : s.substr(s.size() - 1);
	}

	/*
	 * 堆上构造、永不 delete：析构会释放 _cta_mocker 等槽位，但这些槽位中有的是
	 * 未初始化指针（BUG-R46-001）。不析构即可安全驱动不读它们的入口。
	 */
	WtBtRunner* new_runner()
	{
		return new WtBtRunner();
	}

} // namespace

/* WtBtRunner.cpp:449-471 + 500-507：replayer 段给全（mode=none、basefiles 空对象、
 * 显式 path），env.mocker 给一个不存在的模式，config 不建任何 mocker。 */
TEST(R46BtRunner, ConfigWithUnknownMockerBuildsOnlyTheReplayer)
{
	const std::string dir = prepare_out_dir();
	ASSERT_FALSE(dir.empty());

	WtBtRunner* r = new_runner();
	ASSERT_NE(r, nullptr);
	EXPECT_EQ(r->cta_mocker(), nullptr);
	EXPECT_EQ(r->sel_mocker(), nullptr);
	EXPECT_FALSE(r->isAsync());

	const std::string cfg =
		"{\"replayer\":{\"mode\":\"none\",\"basefiles\":{},\"path\":\"" + dir + "\"},"
		"\"env\":{\"mocker\":\"none\"}}";

	r->config(cfg.c_str(), false);

	EXPECT_EQ(r->cta_mocker(), nullptr);
	EXPECT_EQ(r->sel_mocker(), nullptr);
	EXPECT_NE(&r->replayer(), (HisDataReplayer*)NULL);

	clean_dir(dir);
}

/* WtBtRunner.cpp:457-462：内容不是合法 JSON 时 load_from_content 返回 NULL，
 * 只打 "Loading config failed"，不建 mockers、不回放。 */
TEST(R46BtRunner, ConfigWithBrokenContentLeavesMockersEmpty)
{
	WtBtRunner* r = new_runner();
	ASSERT_NE(r, nullptr);

	r->config("{ this is not a json config", false);

	EXPECT_EQ(r->cta_mocker(), nullptr);
	EXPECT_EQ(r->sel_mocker(), nullptr);
	EXPECT_FALSE(r->isAsync());
}

/* WtBtRunner.cpp:266-283 + 299-313：CTA mocker 二次创建会 delete 旧实例并换新 id；
 * SEL mocker 走 register_task 的 "d" 周期分支。两个 mocker 都不需要行情存储。 */
TEST(R46BtRunner, CtaAndSelMockersCanBeCreatedAndReplaced)
{
	WtBtRunner* r = new_runner();
	ASSERT_NE(r, nullptr);

	const uint32_t ctaId1 = r->initCtaMocker("r46bt_cta_a", 0, false, false, false, false);
	ASSERT_NE(ctaId1, 0u);
	ASSERT_NE(r->cta_mocker(), nullptr);

	const uint32_t ctaId2 = r->initCtaMocker("r46bt_cta_b", 2, false, false, false, false);
	ASSERT_NE(ctaId2, 0u);
	EXPECT_NE(ctaId2, ctaId1) << "重建 mocker 必须换新上下文 id";
	EXPECT_NE(r->cta_mocker(), nullptr);

	const uint32_t selId = r->initSelMocker("r46bt_sel", 20240102, 93000, "d");
	ASSERT_NE(selId, 0u);
	EXPECT_NE(r->sel_mocker(), nullptr);
}

/* WtBtRunner.cpp:550-559：未启动（_running=false 且 _worker 为空）时 stop() 直接返回。
 * 只能用静态零初始化的单例：堆实例的 _worker 是未初始化 shared_ptr（BUG-R46-001）。 */
TEST(R46BtRunner, StopBeforeRunIsANoOpOnTheSingleton)
{
	EXPECT_NO_THROW(getRunner().stop());
	EXPECT_NO_THROW(getRunner().stop());
}

/* WtBtRunner.cpp:437-447：不存在的日志文件由 WTSLogger::init 的 exists 守卫吞掉；
 * 随后 setInstDir(getBinDir()) 与 setOutputDir(outDir) 把全局目录改到临时目录。 */
TEST(R46BtRunner, InitStampsInstanceAndOutputDirectories)
{
	const std::string dir = prepare_out_dir();
	ASSERT_FALSE(dir.empty());

	WtBtRunner* r = new_runner();
	ASSERT_NE(r, nullptr);

	r->init((dir + "/r46_absent_logcfg.yaml").c_str(), true, dir.c_str());

	EXPECT_FALSE(WtHelper::getInstDir().empty());
	EXPECT_EQ(tail_char(WtHelper::getInstDir()), "/");
	EXPECT_EQ(std::string(WtHelper::getOutputDir()), dir + "/");

	clean_dir(dir);
}

/* WtBtRunner.cpp:469-471 对 env 段、467 对 replayer 段都没有 NULL 守卫：
 * 只要配置里漏掉其中一段，下一行就解引用空 WTSVariant。两条都会打死 gtest 宿主
 * （SIGSEGV，catch(...) 抓不到），故整条 DISABLED_。缺陷见 bugs/R46.md BUG-R46-003。 */
TEST(R46BtRunner, DISABLED_ConfigWithoutEnvSectionDerefsNull)
{
	WtBtRunner* r = new_runner();
	ASSERT_NE(r, nullptr);

	const std::string dir = prepare_out_dir();
	const std::string cfg =
		"{\"replayer\":{\"mode\":\"none\",\"basefiles\":{},\"path\":\"" + dir + "\"}}";
	r->config(cfg.c_str(), false);
}

TEST(R46BtRunner, DISABLED_ConfigWithoutReplayerSectionDerefsNull)
{
	WtBtRunner* r = new_runner();
	ASSERT_NE(r, nullptr);

	r->config("{\"env\":{\"mocker\":\"none\"}}", false);
}

/* WtBtRunner.cpp:472-479：env.mocker=="cta" 时按 cta 段创建 ExpCtaMocker 并注册到
 * replayer；cta 段没有 module，init_cta_factory 在 dlopen 失败处早退，但不影响
 * mocker 槽位已经建立。 */
TEST(R46BtRunner, ConfigCtaModeCreatesCtaMockerSlot)
{
	const std::string dir = prepare_out_dir();
	ASSERT_FALSE(dir.empty());

	WtBtRunner* r = new_runner();
	ASSERT_NE(r, nullptr);

	const std::string cfg =
		"{\"replayer\":{\"mode\":\"none\",\"basefiles\":{},\"path\":\"" + dir + "\"},"
		"\"env\":{\"mocker\":\"cta\"},"
		"\"cta\":{\"name\":\"r46bt_cta_cfg\",\"slippage\":0}}";

	r->config(cfg.c_str(), false);

	EXPECT_NE(r->cta_mocker(), nullptr) << "cta 模式应在 config 内建好 mocker 槽位";
	EXPECT_EQ(r->sel_mocker(), nullptr);

	clean_dir(dir);
}
