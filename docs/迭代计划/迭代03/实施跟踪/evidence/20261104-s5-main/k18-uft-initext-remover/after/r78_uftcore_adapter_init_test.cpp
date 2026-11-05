/*!
 * \file r78_uftcore_adapter_init_test.cpp
 * \brief 第十五波 R78: TraderAdapter / ParserAdapter / UftStrategyMgr 的装载路径补测
 *
 * 目标:
 *  - TraderAdapter::init 的「模块名非空 -> 路径解析 -> load_library 失败」与
 *    「库可装载但 createTrader 符号缺失」两条早退
 *  - WtUftCore ParserAdapter::init 的完整成功链（load_library -> createParser ->
 *    filter 解析 -> 合约集合 -> subscribe），filter 为空/非空两支
 *  - UftStrategyMgr::loadFactories 的目录缺失、非插件文件跳过、缺入口符号跳过，
 *    以及 createStrategy/getStrategy 的未命中分支
 *
 * 接缝策略: ParserAdapter 的模块用构建产物 libParserUDP.dylib。ParserUDP::init
 * 只解析 host/port 并构造 asio endpoint（ParserUDP.cpp:98-113），不建 socket、
 * 不起线程、不连接；因此只调 init()，绝不调 run()/connect()。
 * TraderAdapter 的「符号缺失」库用系统里确定存在但无 createTrader 的 libfmt。
 *
 * 所有临时路径在系统临时目录，用例结束清理；不访问网络/柜台/共享内存。
 */
#include "q04_trader_fixture.h"

#include "WtUftCore/ParserAdapter.h"
#include "WtUftCore/UftStrategyMgr.h"
#include "WtUftCore/WtHelper.h"

#include "Includes/IParserApi.h"
#include "Includes/UftStrategyDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Share/DLLHelper.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"

#include <dlfcn.h>

using namespace wtp;

namespace {

// 测试可执行体位于 <bin>/TestUnits/，产品插件在 <bin>/ 下
std::string binDir()
{
	Dl_info info;
	if (dladdr(reinterpret_cast<void*>(&binDir), &info) == 0 || info.dli_fname == nullptr)
		return std::string();
	std::filesystem::path p = std::filesystem::path(info.dli_fname).parent_path().parent_path();
	std::string ret = p.string();
	if (!ret.empty() && ret.back() != '/')
		ret.push_back('/');
	return ret;
}

// 将插件复制到临时目录中的 <tmp>/parsers/libParserUDP.dylib，返回是否可用
bool stageUdpParser(q04::TempRoot& tmp, std::string& moduleRoot)
{
	const std::string bin = binDir();
	if (bin.empty())
		return false;

	const std::string src = bin + "libParserUDP.dylib";
	std::error_code ec;
	if (!std::filesystem::exists(src, ec))
		return false;

	// 只有 create/delete 两个符号都可解析时才交给 ParserAdapter::init
	void* h = dlopen(src.c_str(), RTLD_NOW);
	if (h == nullptr)
		return false;
	const bool symbols = dlsym(h, "createParser") != nullptr
		&& dlsym(h, "deleteParser") != nullptr;
	dlclose(h);
	if (!symbols)
		return false;

	const std::string root = tmp.dir().string() + "/modroot";
	std::filesystem::create_directories(root + "/parsers", ec);
	if (ec)
		return false;

	std::filesystem::copy_file(src, root + "/parsers/libParserUDP.dylib",
		std::filesystem::copy_options::overwrite_existing, ec);
	if (ec)
		return false;

	moduleRoot = root + "/";
	return true;
}

std::string systemDylibWithoutTraderEntry()
{
	const char* candidates[] = {
		"/opt/homebrew/lib/libfmt.dylib",
		"/opt/homebrew/lib/libspdlog.dylib",
		"/usr/local/lib/libfmt.dylib",
	};
	for (const char* one : candidates)
	{
		std::error_code ec;
		if (std::filesystem::exists(one, ec))
			return one;
	}
	return std::string();
}

std::string systemDylibWithoutParserEntry()
{
	const char* candidates[] = {
		"/opt/homebrew/lib/libfmt.dylib",
		"/opt/homebrew/lib/libspdlog.dylib",
		"/usr/local/lib/libfmt.dylib",
	};
	for (const char* one : candidates)
	{
		std::error_code ec;
		if (!std::filesystem::exists(one, ec))
			continue;
		void* h = dlopen(one, RTLD_NOW);
		if (h == nullptr)
			continue;
		const bool missing = dlsym(h, "createParser") == nullptr;
		dlclose(h);
		if (missing)
			return one;
	}
	return std::string();
}

// ParserAdapter 成功链需要至少一个合约，且 FakeBaseDataMgr::getContracts()
// 以 append(obj,false) 返回裸指针；adapter 释放数组时会 release 元素，
// 因此这里显式 retain，避免 fixture 析构二次 release。
struct ParserEnv
{
	q04::FakeBaseDataMgr bd;

	ParserEnv()
	{
		WTSContractInfo* ag = bd.add("SHFE", "ag", "ag2406");
		WTSContractInfo* au = bd.add("SHFE", "au", "au2406");
		WTSContractInfo* m = bd.add("DCE", "m", "m2509");
		WTSContractInfo* all[] = { ag, au, m };
		for (WTSContractInfo* one : all)
		{
			one->setAltCode(one->getCode());
			one->retain();
		}
	}
};

// A dedicated module has no createParser symbol. Its constructor/destructor
// markers observe this platform's load/unload, not a general dlclose contract.
TEST(R78UftParserAdapterInit, missing_parser_factory_releases_config_and_module_reference)
{
#ifndef WT_R78_NO_PARSER_FIXTURE_PATH
	FAIL() << "WT_R78_NO_PARSER_FIXTURE_PATH is not configured";
#else
	const char* fixture = WT_R78_NO_PARSER_FIXTURE_PATH;
	q04::TempRoot tmp("r78missingparserfactory");
	const std::string root = tmp.dir().string() + "/module/";
	std::error_code ec;
	std::filesystem::create_directories(root + "parsers", ec);
	ASSERT_FALSE(ec);
	std::filesystem::copy_file(fixture, root + "parsers/" + DLLHelper::wrap_module("R78NoParser", "lib"), ec);
	ASSERT_FALSE(ec);
	const std::string marker = tmp.file("module-marker");
	ASSERT_EQ(0, setenv("R78_PARSER_MODULE_MARKER", marker.c_str(), 1));
	WtHelper::setInstDir(root.c_str());
	ParserEnv env;
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "R78NoParser");
	{
		ParserAdapter adapter;
		EXPECT_FALSE(adapter.init("r78missingfactory", cfg, nullptr, &env.bd));
		EXPECT_EQ(1u, cfg->retainCount());
		EXPECT_FALSE(adapter.run());
		adapter.release();
		EXPECT_EQ(1u, cfg->retainCount());
	}
	std::ifstream in(marker);
	std::string marks((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	EXPECT_EQ("LU", marks);
	WtHelper::setInstDir("");
	unsetenv("R78_PARSER_MODULE_MARKER");
	cfg->release();
#endif
}

class NonNullSpiParserApi : public IParserApi
{
public:
	static uint32_t register_calls;
	static uint32_t null_register_calls;
	static uint32_t release_calls;
	static uint32_t destructor_calls;
	static uint32_t connect_calls;

	~NonNullSpiParserApi() override { ++destructor_calls; }
	bool init(WTSVariant* /*config*/) override { return true; }
	bool connect() override { ++connect_calls; return true; }
	void release() override { ++release_calls; }
	void subscribe(const CodeSet& /*codes*/) override {}
	void registerSpi(IParserSpi* spi) override
	{
		++register_calls;
		if (spi == nullptr)
			++null_register_calls;
	}

	static void reset()
	{
		register_calls = 0;
		null_register_calls = 0;
		release_calls = 0;
		destructor_calls = 0;
		connect_calls = 0;
	}
};

uint32_t NonNullSpiParserApi::register_calls = 0;
uint32_t NonNullSpiParserApi::null_register_calls = 0;
uint32_t NonNullSpiParserApi::release_calls = 0;
uint32_t NonNullSpiParserApi::destructor_calls = 0;
uint32_t NonNullSpiParserApi::connect_calls = 0;

}	// namespace

// TraderAdapter.cpp:162-177 模块名非空、路径解析后 load_library 必然失败
TEST(R78TraderAdapterInit, unloadable_module_is_rejected)
{
	q04::TempRoot tmp("r78traderbad");
	q04::FakeTraderApi api;
	q04::FakeBaseDataMgr bd;
	ActionPolicyMgr policy;

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "R78NoSuchTraderModule");
	cfg->append("user", "u");
	cfg->append("pass", "p");
	cfg->append("product", "prod");

	TraderAdapter adapter;
	EXPECT_FALSE(adapter.init("r78trader1", cfg, &bd, &policy));
	EXPECT_EQ(1u, cfg->retainCount());
	EXPECT_STREQ("", adapter.id());
	EXPECT_EQ(nullptr, adapter.getBaseDataMgr());
	EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, adapter.state());
	EXPECT_FALSE(adapter.run());

	//失败 init 已回滚；同一实例可用独立的外部 API 路径重试。
	EXPECT_TRUE(adapter.initExt("r78trader1-retry", &api, &bd, &policy));
	EXPECT_TRUE(adapter.run());
	adapter.release();
	EXPECT_FALSE(adapter.initExt("r78trader1-closed", &api, &bd, &policy));

	cfg->release();
}

// TraderAdapter.cpp:180-187 库装载成功但入口符号 createTrader 缺失
TEST(R78TraderAdapterInit, module_without_create_symbol_is_rejected)
{
	const std::string dylib = systemDylibWithoutTraderEntry();
	if (dylib.empty())
		GTEST_SKIP() << "no candidate dylib without createTrader";

	q04::TempRoot tmp("r78tradersym");
	std::error_code ec;
	std::filesystem::create_directories(tmp.file("traders"), ec);
	ASSERT_FALSE(ec);
	std::filesystem::copy_file(dylib, tmp.file("traders/libfmt.dylib"),
		std::filesystem::copy_options::overwrite_existing, ec);
	ASSERT_FALSE(ec) << ec.message();

	WtHelper::setInstDir((tmp.dir().string() + "/").c_str());

	q04::FakeTraderApi api;
	q04::FakeBaseDataMgr bd;
	ActionPolicyMgr policy;
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "fmt");
	cfg->append("user", "u");
	cfg->append("pass", "p");
	cfg->append("product", "prod");

	{
		TraderAdapter adapter;
		EXPECT_FALSE(adapter.init("r78trader2", cfg, &bd, &policy));
		EXPECT_EQ(1u, cfg->retainCount());
		EXPECT_STREQ("", adapter.id());
		EXPECT_EQ(nullptr, adapter.getBaseDataMgr());
		EXPECT_FALSE(adapter.run());
	}

	WtHelper::setInstDir("");
	cfg->release();
}

// ParserAdapter.cpp:42-163 完整 init 成功链（filter 非空 -> 逐合约六种键匹配）
TEST(R78UftParserAdapterInit, loads_udp_plugin_with_filter)
{
	q04::TempRoot tmp("r78parserflt");
	std::string moduleRoot;
	if (!stageUdpParser(tmp, moduleRoot))
		GTEST_SKIP() << "libParserUDP.dylib not found in this build";
	WtHelper::setInstDir(moduleRoot.c_str());

	ParserEnv env;
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "ParserUDP");
	cfg->append("filter", "SHFE,ag2406");
	cfg->append("host", "127.0.0.1");
	cfg->append("bport", (uint32_t)17801);
	cfg->append("sport", (uint32_t)17802);

	{
		ParserAdapter adapter;
		EXPECT_TRUE(adapter.init("r78parser1", cfg, nullptr, &env.bd));
		EXPECT_STREQ("r78parser1", adapter.id());
		EXPECT_EQ(static_cast<IBaseDataMgr*>(&env.bd), adapter.getBaseDataMgr());
		// 只验证装载；run() 会 connect UDP，本用例不调用
		adapter.release();
	}

	WtHelper::setInstDir("");
	cfg->release();
}

// ParserAdapter.cpp:137-148 filter 为空 -> 全量合约集合分支
TEST(R78UftParserAdapterInit, loads_udp_plugin_without_filter)
{
	q04::TempRoot tmp("r78parsernoflt");
	std::string moduleRoot;
	if (!stageUdpParser(tmp, moduleRoot))
		GTEST_SKIP() << "libParserUDP.dylib not found in this build";
	WtHelper::setInstDir(moduleRoot.c_str());

	ParserEnv env;
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "ParserUDP");
	cfg->append("filter", "");
	cfg->append("host", "127.0.0.1");
	cfg->append("bport", (uint32_t)17803);
	cfg->append("sport", (uint32_t)17804);

	{
		ParserAdapter adapter;
		EXPECT_TRUE(adapter.init("r78parser2", cfg, nullptr, &env.bd));
		EXPECT_STREQ("r78parser2", adapter.id());
		adapter.release();
	}

	WtHelper::setInstDir("");
	cfg->release();
}

// Missing module/entry failures occur before API creation and remain retryable.
// ParserUDP::init only parses the endpoint; the malformed address throws before
// connect can create sockets or asynchronous callbacks. This test explicitly
// releases that known-quiescent candidate before retrying. That provider fact
// does not imply IParserApi::release() drains callbacks in general.
TEST(R78UftParserAdapterInit, precreate_failures_retry_and_quiescent_init_failure_can_be_released)
{
	q04::TempRoot tmp("r78parserretry");
	std::string moduleRoot;
	if (!stageUdpParser(tmp, moduleRoot))
		GTEST_SKIP() << "libParserUDP.dylib not found in this build";
	WtHelper::setInstDir(moduleRoot.c_str());

	ParserEnv env;
	ParserAdapter adapter;

	WTSVariant* missingCfg = WTSVariant::createObject();
	missingCfg->append("module", "R78NoSuchParserModule");
	EXPECT_FALSE(adapter.init("r78parserretry", missingCfg, nullptr, &env.bd));
	EXPECT_EQ(1u, missingCfg->retainCount());
	EXPECT_FALSE(adapter.run());

	const std::string noParserDylib = systemDylibWithoutParserEntry();
	if (!noParserDylib.empty())
	{
		std::error_code ec;
		std::filesystem::copy_file(noParserDylib,
			moduleRoot + "parsers/libNoParser.dylib",
			std::filesystem::copy_options::overwrite_existing, ec);
		ASSERT_FALSE(ec);
		WTSVariant* missingEntryCfg = WTSVariant::createObject();
		missingEntryCfg->append("module", "NoParser");
		EXPECT_FALSE(adapter.init("r78parserretry", missingEntryCfg, nullptr, &env.bd));
		EXPECT_EQ(1u, missingEntryCfg->retainCount());
		EXPECT_FALSE(adapter.run());
		missingEntryCfg->release();
	}

	WTSVariant* badEndpointCfg = WTSVariant::createObject();
	badEndpointCfg->append("module", "ParserUDP");
	badEndpointCfg->append("host", "not-an-ip-address");
	badEndpointCfg->append("bport", (uint32_t)17805);
	badEndpointCfg->append("sport", (uint32_t)17806);
	EXPECT_FALSE(adapter.init("r78parserretry", badEndpointCfg, nullptr, &env.bd));
	EXPECT_EQ(2u, badEndpointCfg->retainCount());
	EXPECT_TRUE(adapter.hasApi());
	EXPECT_FALSE(adapter.isInitialized());
	EXPECT_FALSE(adapter.run());
	adapter.release();
	EXPECT_FALSE(adapter.hasApi());
	EXPECT_EQ(1u, badEndpointCfg->retainCount());

	WTSVariant* goodCfg = WTSVariant::createObject();
	goodCfg->append("module", "ParserUDP");
	goodCfg->append("host", "127.0.0.1");
	goodCfg->append("bport", (uint32_t)17807);
	goodCfg->append("sport", (uint32_t)17808);
	EXPECT_TRUE(adapter.init("r78parserretry", goodCfg, nullptr, &env.bd));
	EXPECT_EQ(2u, goodCfg->retainCount());
	adapter.release();
	EXPECT_EQ(1u, goodCfg->retainCount());
	EXPECT_FALSE(adapter.run());
	adapter.release();
	EXPECT_EQ(1u, goodCfg->retainCount());

	WtHelper::setInstDir("");
	missingCfg->release();
	badEndpointCfg->release();
	goodCfg->release();
}

// IParserApi does not define registerSpi(NULL) as an unregister operation.
// Card 9 (BUG-R78-001): an external API without a remover has no proof that
// its callbacks stopped, so release() quarantines it instead of calling
// IParserApi::release()/delete. The previous oracle expected one release and
// one destructor call (destroy without proof).
TEST(R78UftParserAdapterInit, release_does_not_register_a_null_spi)
{
	NonNullSpiParserApi::reset();
	ParserEnv env;
	NonNullSpiParserApi* api = new NonNullSpiParserApi();
	{
		ParserAdapter adapter;
		EXPECT_TRUE(adapter.initExt("r78parsernonnull", api, nullptr, &env.bd));
		EXPECT_TRUE(adapter.run());
		adapter.release();
		adapter.release();

		EXPECT_EQ(1u, NonNullSpiParserApi::register_calls);
		EXPECT_EQ(0u, NonNullSpiParserApi::null_register_calls);
		EXPECT_EQ(0u, NonNullSpiParserApi::release_calls);
		EXPECT_EQ(0u, NonNullSpiParserApi::destructor_calls);
		EXPECT_EQ(1u, NonNullSpiParserApi::connect_calls);
		EXPECT_TRUE(adapter.hasApi());
#ifdef WT_UFT_PARSER_LIFECYCLE_CARD9	// absent from the pre-change header (red control)
		EXPECT_TRUE(adapter.isQuarantined());
#endif
		EXPECT_FALSE(adapter.run());
	}
	// The quiescent test fake is reclaimed by the test after its owner is gone.
	delete api;
	EXPECT_EQ(1u, NonNullSpiParserApi::destructor_calls);
}

//////////////////////////////////////////////////////////////////////////
// BUG-R78-001 / card 9: host callback stop barrier (2026-11-04).
namespace {

struct BlockingStub : public IParserStub
{
	std::mutex mtx;
	std::condition_variable cv;
	bool entered = false;
	bool releaseCallback = false;
	std::atomic<uint32_t> quotes{ 0 };
	std::function<void()> inside;

	void handle_push_quote(WTSTickData* /*curTick*/) override
	{
		quotes.fetch_add(1);
		if (inside)
		{
			inside();
			return;
		}
		std::unique_lock<std::mutex> lock(mtx);
		entered = true;
		cv.notify_all();
		cv.wait(lock, [this]() { return releaseCallback; });
	}
};

std::atomic<uint32_t> g_barrierRemoved{ 0 };

void removeBarrierApi(IParserApi*& api)
{
	g_barrierRemoved.fetch_add(1);
	delete api;
	api = nullptr;
}

WTSTickData* r78Tick()
{
	WTSTickStruct st;
	memset(&st, 0, sizeof(st));
	wt_strcpy(st.exchg, "SHFE");
	wt_strcpy(st.code, "ag2406");
	st.action_date = 20240315;
	st.action_time = 93000000;
	st.trading_date = 20240315;
	st.price = 5000.0;
	return WTSTickData::create(st);
}

}	// namespace

// A release() racing with an admitted provider callback must not destroy the
// API or clear borrowed objects while that callback runs; it reports pending
// and a later release() completes teardown exactly once.
TEST(R78UftParserAdapterInit, release_waits_for_active_callback_before_teardown)
{
	NonNullSpiParserApi::reset();
	g_barrierRemoved = 0;
	ParserEnv env;
	BlockingStub stub;
	ParserAdapter adapter;
	ASSERT_TRUE(adapter.initExt("r78barrier", new NonNullSpiParserApi(), &stub, &env.bd, removeBarrierApi));

	WTSTickData* tick = r78Tick();
	std::thread provider([&]() { adapter.handleQuote(tick, 0); });
	{
		std::unique_lock<std::mutex> lock(stub.mtx);
		ASSERT_TRUE(stub.cv.wait_for(lock, std::chrono::seconds(10), [&]() { return stub.entered; }));
	}

	adapter.release();
	EXPECT_EQ(0u, g_barrierRemoved.load()) << "API destroyed under an active callback";
	EXPECT_EQ(0u, NonNullSpiParserApi::release_calls);
	EXPECT_TRUE(adapter.hasApi());
	EXPECT_TRUE(adapter.isTeardownPending());
	EXPECT_EQ(static_cast<IBaseDataMgr*>(&env.bd), adapter.getBaseDataMgr());

	// No new callback is admitted once release() has been requested.
	adapter.handleQuote(tick, 0);
	EXPECT_EQ(1u, stub.quotes.load());

	{
		std::lock_guard<std::mutex> lock(stub.mtx);
		stub.releaseCallback = true;
	}
	stub.cv.notify_all();
	provider.join();

	adapter.release();
	// 决策 K18=否（2026-11-04）：带 remover 的外部 API 同无 remover：安全点 release 也不调用 release/remover、不删除，隔离到进程退出。
	EXPECT_EQ(0u, g_barrierRemoved.load());
	EXPECT_EQ(0u, NonNullSpiParserApi::release_calls);
	EXPECT_TRUE(adapter.hasApi());
	EXPECT_FALSE(adapter.isTeardownPending());
#ifdef WT_UFT_PARSER_K18_RESIDENT
	EXPECT_TRUE(adapter.isResidentQuarantined());
#endif
	adapter.release();
	EXPECT_EQ(0u, g_barrierRemoved.load());
	tick->release();
}

// release() from inside this adapter's own callback only requests stop; the
// API and borrowed graph survive until a release() from a safe point.
TEST(R78UftParserAdapterInit, release_inside_callback_is_deferred_to_safe_point)
{
	NonNullSpiParserApi::reset();
	g_barrierRemoved = 0;
	ParserEnv env;
	BlockingStub stub;
	ParserAdapter adapter;
	ASSERT_TRUE(adapter.initExt("r78deferred", new NonNullSpiParserApi(), &stub, &env.bd, removeBarrierApi));

	uint32_t removedInside = 99;
	bool hasApiInside = false;
	stub.inside = [&]() {
		adapter.release();
		removedInside = g_barrierRemoved.load();
		hasApiInside = adapter.hasApi();
	};
	WTSTickData* tick = r78Tick();
	adapter.handleQuote(tick, 0);
	EXPECT_EQ(0u, removedInside);
	EXPECT_TRUE(hasApiInside);
	EXPECT_TRUE(adapter.isTeardownPending());
	EXPECT_EQ(0u, NonNullSpiParserApi::release_calls);

	adapter.release();
	// 决策 K18=否（2026-11-04）：带 remover 的外部 API 同无 remover：安全点 release 也不调用 release/remover、不删除，隔离到进程退出。
	EXPECT_EQ(0u, g_barrierRemoved.load());
	EXPECT_TRUE(adapter.hasApi());
#ifdef WT_UFT_PARSER_K18_RESIDENT
	EXPECT_TRUE(adapter.isResidentQuarantined());
#endif
	tick->release();
}

// UftStrategyMgr.cpp:30-92 目录缺失返回 false；非 dylib/子目录跳过；
// dylib 可装载但缺 createStrategyFact 时 free_library + continue
TEST(R78UftStraMgr, load_factories_skips_unusable_entries)
{
	q04::TempRoot tmp("r78mgrload");
	UftStrategyMgr mgr;

	// 目录不存在
	EXPECT_FALSE(mgr.loadFactories((tmp.dir().string() + "/no_such_dir").c_str()));

	// 混合目录: 子目录 + 文本文件 + 无入口符号的 dylib
	const std::string mixed = tmp.file("mixed");
	std::error_code ec;
	std::filesystem::create_directories(mixed + "/sub", ec);
	ASSERT_FALSE(ec);
	{
		std::ofstream ofs(mixed + "/readme.txt", std::ios::out | std::ios::trunc);
		ofs << "not a plugin";
	}
	const std::string dylib = systemDylibWithoutTraderEntry();
	if (!dylib.empty())
	{
		std::filesystem::copy_file(dylib, mixed + "/libNoEntry.dylib",
			std::filesystem::copy_options::overwrite_existing, ec);
		ASSERT_FALSE(ec);
	}

	EXPECT_TRUE(mgr.loadFactories(mixed.c_str()));
}

// UftStrategyMgr.cpp:94-132 空工厂表上的三种创建未命中 + getStrategy 未命中
TEST(R78UftStraMgr, create_and_get_misses)
{
	UftStrategyMgr mgr;

	// "NoDot" 分割后不足两段
	EXPECT_EQ(nullptr, mgr.createStrategy("NoDot", "r78stra1"));
	// 两段形态但工厂名未登记
	EXPECT_EQ(nullptr, mgr.createStrategy("NoFact.SimpleUft", "r78stra2"));
	// 三段形态但工厂名未登记
	EXPECT_EQ(nullptr, mgr.createStrategy("NoFact", "SimpleUft", "r78stra3"));
	// 未创建过的策略 id
	EXPECT_EQ(nullptr, mgr.getStrategy("r78stra_missing"));
}
