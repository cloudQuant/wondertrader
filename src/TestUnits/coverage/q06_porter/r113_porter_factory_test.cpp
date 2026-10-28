/**
 * r113_porter_factory_test.cpp  (lane R113 · wave19)
 *
 * 靶子：WtRtRunner 配置链中只能靠真实策略工厂触达的两段
 *   - basefiles.rules 回填主力的 `isCustomHot == false -> continue`  WtRtRunner.cpp:562-580
 *   - initCtaStrategies 的 active 项：createStrategy + init + addContext  WtRtRunner.cpp:726-740
 *
 * 用公开的 WtRtRunner::addCtaFactories 加载构建目录里的 libWtCtaStraFact.dylib，
 * 再以 "WtCtaStraFact.DualThrust" 建一个 active CTA 策略。runner 是堆实例且不 delete
 * （构造列表漏初始化 _config/_engine，见 BUG-R46-001），不影响进程单例。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "WtCore/WtHelper.h"
#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

USING_NS_WTP;

namespace
{
	namespace stdfs = std::filesystem;

	std::string prepare_dir(const char* tag)
	{
		stdfs::path p = stdfs::temp_directory_path()
			/ (std::string("r113_porter_") + tag + "_" + std::to_string((long)::getpid()));
		std::error_code ec;
		stdfs::remove_all(p, ec);
		stdfs::create_directories(p, ec);
		return p.string();
	}

	void clean_dir(const std::string& dir)
	{
		std::error_code ec;
		stdfs::remove_all(dir, ec);
	}

	void write_text(const std::string& path, const std::string& body)
	{
		std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
		ofs << body;
	}


	/* 测试二进制位于 <bin>/TestUnits/，插件在 <bin>/；不依赖 WtHelper 的
	 * dladdr 缓存，直接由可执行文件路径推导，避免跨 lane 的 instDir 竞争。 */
	std::string bin_dir()
	{
#ifdef __APPLE__
		uint32_t size = 0;
		_NSGetExecutablePath(nullptr, &size);
		std::vector<char> buf(size + 1, 0);
		_NSGetExecutablePath(buf.data(), &size);
		std::error_code ec;
		stdfs::path exe = stdfs::weakly_canonical(stdfs::path(buf.data()), ec);
		if (ec)
			exe = stdfs::path(buf.data());
		stdfs::path dir = exe.parent_path();
#else
		char buf[4096] = { 0 };
		const ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
		stdfs::path dir = len > 0 ? stdfs::path(std::string(buf, (std::size_t)len)).parent_path()
			: stdfs::path();
#endif
		if (dir.filename() == "TestUnits")
			dir = dir.parent_path();
		return dir.string();
	}

	const char* kSessions =
		"{\"TRADING\":{\"name\":\"Trading\",\"offset\":0,"
		"\"sections\":[{\"from\":900,\"to\":1500}]}}";

	/* au 与 ag 两个品种：主力规则只覆盖 ag，au 合约在回填循环里走 continue */
	const char* kCommodities =
		"{\"SHFE\":{"
		"\"au\":{\"name\":\"Gold\",\"session\":\"TRADING\",\"pricetick\":0.02,"
		"\"volscale\":1000,\"covermode\":0,\"pricemode\":0,\"trademode\":0,\"category\":1},"
		"\"ag\":{\"name\":\"Silver\",\"session\":\"TRADING\",\"pricetick\":1,"
		"\"volscale\":15,\"covermode\":0,\"pricemode\":0,\"trademode\":0,\"category\":1}}}";

	const char* kContracts =
		"{\"SHFE\":{"
		"\"au2406\":{\"name\":\"au2406\",\"product\":\"au\",\"exchg\":\"SHFE\"},"
		"\"ag2406\":{\"name\":\"ag2406\",\"product\":\"ag\",\"exchg\":\"SHFE\"}}}";

	const char* kHotRules =
		"{\"SHFE\":{\"ag\":[{\"from\":\"ag2406\",\"to\":\"ag2406\",\"date\":20240101,"
		"\"oldclose\":100.0,\"newclose\":100.0}]}}";

	const char* kActions = "{}";

	void write_fixtures(const std::string& dir)
	{
		write_text(dir + "/sessions.json", kSessions);
		write_text(dir + "/commodities.json", kCommodities);
		write_text(dir + "/contracts.json", kContracts);
		write_text(dir + "/hot.json", kHotRules);
		write_text(dir + "/actions.json", kActions);
	}

	std::string base_cfg(const std::string& dir)
	{
		return "{\"basefiles\":{"
			"\"session\":\"" + dir + "/sessions.json\","
			"\"commodity\":\"" + dir + "/commodities.json\","
			"\"contract\":\"" + dir + "/contracts.json\","
			"\"rules\":{\"R113\":\"" + dir + "/hot.json\"}},"
			"\"env\":{\"name\":\"cta\"},"
			"\"data\":{},"
			"\"bspolicy\":\"" + dir + "/actions.json\"";
	}

	WtRtRunner* new_heap_runner(const std::string& dir)
	{
		/* 堆上构造、永不 delete：BUG-R46-001（构造列表漏初始化 _config/_engine）。 */
		WtRtRunner* r = new WtRtRunner();
		r->init((dir + "/r113_absent_logcfg.yaml").c_str(), true, dir.c_str());
		return r;
	}
}

/*
 * 生产: WtRtRunner.cpp:562-580（getHotTags + 合约遍历；au 不命中 -> continue）
 *       838-882（env=cta 的引擎初始化）
 */
TEST(R113PorterHotRule, HotRuleForOtherProductContinuesOverContract)
{
	const std::string dir = prepare_dir("hot");
	write_fixtures(dir);

	WtRtRunner* r = new_heap_runner(dir);
	ASSERT_NE(nullptr, r);
	EXPECT_TRUE(r->config((base_cfg(dir) + "}").c_str(), false));
	EXPECT_NE(dynamic_cast<WtCtaEngine*>(r->getEngine()), nullptr);

	clean_dir(dir);
}

/*
 * 生产: WtRtRunner.cpp:726-740（active 策略：_cta_mgr.createStrategy(name,id) ->
 *       stra->self()->init(params) -> new CtaStraContext -> addContext）
 *       562-580（同一配置里的 hot 回填 continue）
 */
TEST(R113PorterCtaFactory, ActiveStrategyFromBuiltinFactoryIsWiredIntoEngine)
{
	const std::string dir = prepare_dir("factory");
	write_fixtures(dir);

	WtRtRunner* r = new_heap_runner(dir);
	ASSERT_NE(nullptr, r);

	const std::string inst = bin_dir();
	const std::string lib = inst + "/libWtCtaStraFact.dylib";
	if (!stdfs::exists(lib))
		GTEST_SKIP() << "构建目录缺少 " << lib;

	ASSERT_FALSE(inst.empty());
	ASSERT_TRUE(r->addCtaFactories(inst.c_str()));

	const std::string cfg = base_cfg(dir) +
		",\"strategies\":{\"cta\":[{"
		"\"active\":true,\"id\":\"r113_cta\",\"name\":\"WtCtaStraFact.DualThrust\","
		"\"slippage\":0,"
		"\"params\":{\"days\":5,\"k1\":0.5,\"k2\":0.5,\"period\":\"d1\","
		"\"count\":10,\"code\":\"SHFE.au.2406\",\"stock\":false}}]}}";

	EXPECT_TRUE(r->config(cfg.c_str(), false));
	EXPECT_NE(dynamic_cast<WtCtaEngine*>(r->getEngine()), nullptr);

	clean_dir(dir);
}
