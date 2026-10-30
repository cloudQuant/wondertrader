/*!
 * \file r45_parseradapter_test.cpp
 * \brief WtDtServo/ParserAdapter 补测（迭代01 第十波 R45）
 *
 * 与 r8_parseradapter_test.cpp 互补：r8 只打了拒绝分支与 handle 守卫，
 * 本文件用 r45_fake_parserapi.h 的自用替身把「有 api」的正路径补齐：
 *  - initExt 成功链：registerSpi -> init(NULL) -> 合约集合 -> subscribe（:44-71）
 *  - initExt 的 api->init 失败分支与失败后 owner 保留
 *  - run 的 _parser_api!=NULL -> connect（:219-226）
 *  - release 的 api->release + delete（:205-217）
 *  - handleQuote 带 runner 的转发（:281-282）
 *  - handleTransaction/OrderDetail/OrderQueue 的「合约命中」尾段（:242-246/:256-260/:270-273）
 *  - ParserAdapterMgr 的 run/release 扇出（:301-309/:339-347）
 * 不加载任何插件 DLL，不建 socket/线程；堆分配的替身在 initExt 入口转交给 adapter，
 * 由同步 fake 验证先达到 fake 专属静默点、再释放对象。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <dlfcn.h>
#include <unistd.h>
#include <vector>

#include <filesystem>

#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSStruct.h>
#include <Includes/WTSVariant.hpp>
#include <WTSTools/WTSBaseDataMgr.h>
#include <WtDtServo/ParserAdapter.h>
#include <WtDtServo/WtDtRunner.h>

#include "r45_fake_parserapi.h"

namespace fs = std::filesystem;

namespace r45 {
int FakeParserApi::s_deleted = 0;
int FakeParserApi::s_release_calls = 0;
}

// Narrow friend access lets the test install a controllable module deleter
// without building or loading a test plugin DLL.
struct ParserAdapterTestAccess
{
	static void setRemover(ParserAdapter& adapter, FuncDeleteParser remover)
	{
		adapter._remover = remover;
	}

	static FuncDeleteParser remover(const ParserAdapter& adapter)
	{
		return adapter._remover;
	}

	static IParserApi* api(const ParserAdapter& adapter)
	{
		return adapter._parser_api;
	}

	static IParserApi* uncertainOriginalApi(const ParserAdapter& adapter)
	{
		return adapter._uncertain_original_api;
	}

	static bool releaseUncertain(const ParserAdapter& adapter)
	{
		return adapter._release_uncertain;
	}

	static void setModuleHandle(ParserAdapter& adapter, void* handle)
	{
		adapter._module_handle = handle;
	}

	static void* moduleHandle(const ParserAdapter& adapter)
	{
		return adapter._module_handle;
	}

	static void setConfig(ParserAdapter& adapter, WTSVariant* cfg)
	{
		adapter._cfg = cfg;
	}

	static WTSVariant* config(const ParserAdapter& adapter)
	{
		return adapter._cfg;
	}

	static void abandonUncertainStateForTest(ParserAdapter& adapter)
	{
		adapter._parser_api = nullptr;
		adapter._uncertain_original_api = nullptr;
		adapter._release_uncertain = false;
		adapter._remover = nullptr;
		adapter._module_handle = nullptr;
		adapter._cfg = nullptr;
	}
};

namespace {

// This fake has no worker thread and cannot emit callbacks asynchronously.
// Its release() marks this fake's callback source quiescent; this does not
// imply that arbitrary provider release() implementations drain callbacks.
class R45QuiescentParserApi : public r45::FakeParserApi
{
public:
	explicit R45QuiescentParserApi(bool initOk = true, bool connectOk = true)
		: r45::FakeParserApi(initOk, connectOk)
	{
	}

	~R45QuiescentParserApi() override
	{
		if (!_quiescent)
			++s_destroyed_before_quiescence;
	}

	void release() override
	{
		r45::FakeParserApi::release();
		_quiescent = true;
		++s_quiescent_releases;
	}

	static void reset()
	{
		s_quiescent_releases = 0;
		s_destroyed_before_quiescence = 0;
	}

	static int s_quiescent_releases;
	static int s_destroyed_before_quiescence;

private:
	bool _quiescent = false;
};

int R45QuiescentParserApi::s_quiescent_releases = 0;
int R45QuiescentParserApi::s_destroyed_before_quiescence = 0;

// The fake owns no worker or asynchronous callback source. The test marks the
// fake quiescent only after a separate simulated callback-return barrier; its
// API release() deliberately does not assert that barrier.
class R45ControlledDeleterApi : public r45::FakeParserApi
{
public:
	explicit R45ControlledDeleterApi(bool initOk = true)
		: r45::FakeParserApi(initOk, true)
	{
	}

	~R45ControlledDeleterApi() override
	{
		if (s_quiescent)
			++s_deleted_after_quiescence;
		else
			++s_deleted_before_quiescence;
	}

	static void reset()
	{
		s_quiescent = false;
		s_deleter_calls = 0;
		s_deleted_before_quiescence = 0;
		s_deleted_after_quiescence = 0;
	}

	static void markCallbacksQuiescent()
	{
		s_quiescent = true;
	}

	static bool s_quiescent;
	static int s_deleter_calls;
	static int s_deleted_before_quiescence;
	static int s_deleted_after_quiescence;
};

bool R45ControlledDeleterApi::s_quiescent = false;
int R45ControlledDeleterApi::s_deleter_calls = 0;
int R45ControlledDeleterApi::s_deleted_before_quiescence = 0;
int R45ControlledDeleterApi::s_deleted_after_quiescence = 0;

void r45_pa_controlled_delete_parser(wtp::IParserApi*& api)
{
	++R45ControlledDeleterApi::s_deleter_calls;
	if (api != nullptr && R45ControlledDeleterApi::s_quiescent)
	{
		delete api;
		api = nullptr;
	}
}

class R45MutatingParserDeleter
{
public:
	static void reset()
	{
		s_replacement = nullptr;
		s_calls = 0;
	}

	static void replacePointer(wtp::IParserApi*& api)
	{
		++s_calls;
		api = s_replacement;
	}

	static wtp::IParserApi* s_replacement;
	static int s_calls;
};

wtp::IParserApi* R45MutatingParserDeleter::s_replacement = nullptr;
int R45MutatingParserDeleter::s_calls = 0;

class R45ThrowingParserDeleter
{
public:
	static void reset() { s_calls = 0; }
	static void throwAfterCall(wtp::IParserApi*& /*api*/)
	{
		++s_calls;
		throw std::runtime_error("controlled parser deleter failure");
	}

	static int s_calls;
};

int R45ThrowingParserDeleter::s_calls = 0;

struct R45PaTickProbe
{
	static std::vector<std::string> codes;
};
std::vector<std::string> R45PaTickProbe::codes;

void r45_pa_on_tick(const char* stdCode, WTSTickStruct* /*tick*/)
{
	R45PaTickProbe::codes.emplace_back(stdCode == nullptr ? "" : stdCode);
}

void r45_pa_on_bar(const char* /*stdCode*/, const char* /*period*/, WTSBarStruct* /*bar*/)
{
}

fs::path r45_pa_make_tmp_dir()
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ ("r45_pa_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

void r45_pa_write(const fs::path& p, const std::string& content)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

class R45ParserAdapterFakeTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r45_pa_make_tmp_dir();
		r45::FakeParserApi::resetCounters();
		R45QuiescentParserApi::reset();
		R45ControlledDeleterApi::reset();
		R45MutatingParserDeleter::reset();
		R45ThrowingParserDeleter::reset();
		R45PaTickProbe::codes.clear();

		r45_pa_write(_dir / "sessions.json",
			R"JSON({"SD0930":{"name":"r45","offset":0,"auction":{"from":929,"to":930},
				"sections":[{"from":930,"to":1130}]}})JSON");
		r45_pa_write(_dir / "commodities.json",
			R"JSON({"CFFEX":{"IC":{"covermode":0,"pricemode":0,"category":1,"precision":1,
				"pricetick":0.2,"volscale":200,"name":"CSI500","exchg":"CFFEX",
				"session":"SD0930","holiday":"CHINA"}},
				"SSE":{"600000":{"covermode":0,"pricemode":0,"category":0,"precision":2,
				"pricetick":0.01,"volscale":100,"name":"SPDB","exchg":"SSE",
				"session":"SD0930","holiday":"CHINA"}}})JSON");
		r45_pa_write(_dir / "contracts.json",
			R"JSON({"CFFEX":{"IC2204":{"name":"CSI500 2204","code":"IC2204","exchg":"CFFEX",
				"product":"IC","maxlimitqty":20,"maxmarketqty":10}},
				"SSE":{"600000":{"name":"SPDB","code":"600000","exchg":"SSE",
				"product":"600000","maxlimitqty":100000,"maxmarketqty":1000}}})JSON");

		ASSERT_TRUE(_bdMgr.loadSessions((_dir / "sessions.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadCommodities((_dir / "commodities.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadContracts((_dir / "contracts.json").string().c_str()));
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	void initRunner(WtDtRunner& runner)
	{
		const std::string d = _dir.string();
		const std::string cfg = std::string("{\"disable_dump\":true,\"basefiles\":{")
			+ "\"session\":\"" + d + "/sessions.json\","
			+ "\"commodity\":\"" + d + "/commodities.json\","
			+ "\"contract\":\"" + d + "/contracts.json\"}}";
		r45_pa_write(_dir / "wtdt.json", cfg);
		runner.initialize((_dir / "wtdt.json").string().c_str(), true, "",
			(_dir / "no_such_logcfg.yaml").string().c_str(), r45_pa_on_tick, r45_pa_on_bar);
	}

	fs::path _dir;
	WTSBaseDataMgr _bdMgr;
};

// ---------------------------------------------------------------------------
// initExt 正路径与所有权
// ---------------------------------------------------------------------------

TEST_F(R45ParserAdapterFakeTest, InitExtRegistersSpiAndSubscribesAllContracts)
{
	R45QuiescentParserApi* api = new R45QuiescentParserApi(true, true);
	ParserAdapter pa(&_bdMgr, nullptr);

	// ParserAdapter.cpp:46-71：api 非空 -> registerSpi -> init(NULL) 成功 ->
	// 遍历基础数据合约 -> subscribe(全量集合)
	ASSERT_TRUE(pa.initExt("r45_ext", api));
	EXPECT_STREQ(pa.id(), "r45_ext");
	EXPECT_EQ(api->register_calls, 1);
	EXPECT_EQ(api->spi_ptr, static_cast<IParserSpi*>(&pa));
	EXPECT_EQ(api->init_calls, 1);
	EXPECT_EQ(api->last_cfg, nullptr);
	EXPECT_EQ(api->subscribe_calls, 1);
	// initExt 用 getFullCode()（exchg + code），与 init 的 getFullAltCode() 不同
	EXPECT_NE(api->subscribed.find("CFFEX.IC2204"), api->subscribed.end());
	EXPECT_NE(api->subscribed.find("SSE.600000"), api->subscribed.end());
	EXPECT_EQ(api->subscribed.size(), 2u);

	// ParserAdapter.cpp:219-226：api 已有 -> connect 并返回 true
	EXPECT_TRUE(pa.run());
	EXPECT_EQ(api->connect_calls, 1);

	// ParserAdapter.cpp:293-296 透传
	EXPECT_EQ(pa.getBaseDataMgr(), static_cast<IBaseDataMgr*>(&_bdMgr));

	pa.release();
	EXPECT_EQ(R45QuiescentParserApi::s_quiescent_releases, 1);
	EXPECT_EQ(R45QuiescentParserApi::s_destroyed_before_quiescence, 0);
}

TEST_F(R45ParserAdapterFakeTest, InitExtApiInitFailureSkipsSubscribe)
{
	R45QuiescentParserApi* api = new R45QuiescentParserApi(false, true);
	ParserAdapter pa(&_bdMgr, nullptr);

	// Ownership transfers at entry even when init() fails. This synchronous fake
	// has no asynchronous callback source; its release() is its test-only quiet
	// point before the adapter destroys it.
	EXPECT_FALSE(pa.initExt("r45_ext_bad", api));
	EXPECT_EQ(api->init_calls, 1);
	EXPECT_EQ(api->subscribe_calls, 0);
	EXPECT_TRUE(api->subscribed.empty());
	EXPECT_FALSE(pa.run());
	EXPECT_EQ(api->connect_calls, 0);
	EXPECT_EQ(r45::FakeParserApi::s_deleted, 0);
	EXPECT_EQ(r45::FakeParserApi::s_release_calls, 0);

	pa.release();
	EXPECT_EQ(r45::FakeParserApi::s_release_calls, 1);
	EXPECT_EQ(r45::FakeParserApi::s_deleted, 1);
	EXPECT_EQ(R45QuiescentParserApi::s_quiescent_releases, 1);
	EXPECT_EQ(R45QuiescentParserApi::s_destroyed_before_quiescence, 0);
}

TEST_F(R45ParserAdapterFakeTest, InitExtTransfersNonNullApiEvenWhenIdIsInvalid)
{
	R45QuiescentParserApi* api = new R45QuiescentParserApi(true, true);
	ParserAdapter pa(&_bdMgr, nullptr);

	EXPECT_FALSE(pa.initExt(nullptr, api));
	EXPECT_EQ(api->register_calls, 0);
	EXPECT_EQ(r45::FakeParserApi::s_deleted, 0);

	pa.release();
	EXPECT_EQ(r45::FakeParserApi::s_release_calls, 1);
	EXPECT_EQ(r45::FakeParserApi::s_deleted, 1);
	EXPECT_EQ(R45QuiescentParserApi::s_quiescent_releases, 1);
	EXPECT_EQ(R45QuiescentParserApi::s_destroyed_before_quiescence, 0);
}

TEST_F(R45ParserAdapterFakeTest, BusyInitExtRetainsEachUniqueTransferredApi)
{
	R45QuiescentParserApi* primary = new R45QuiescentParserApi(true, true);
	R45QuiescentParserApi* rejected = new R45QuiescentParserApi(true, true);
	ParserAdapter pa(&_bdMgr, nullptr);
	ASSERT_TRUE(pa.initExt("r45_busy_primary", primary));

	// Re-passing an already-owned pointer does not create another owner.
	EXPECT_FALSE(pa.initExt("r45_busy_same", primary));
	// A distinct API is transferred despite rejection; its repeated handoff is
	// deduplicated and the API is never initialized or registered here.
	EXPECT_FALSE(pa.initExt("r45_busy_distinct", rejected));
	EXPECT_FALSE(pa.initExt("r45_busy_distinct_again", rejected));
	EXPECT_EQ(rejected->register_calls, 0);
	EXPECT_EQ(rejected->init_calls, 0);
	EXPECT_EQ(r45::FakeParserApi::s_deleted, 0);

	pa.release();
	EXPECT_EQ(r45::FakeParserApi::s_release_calls, 2);
	EXPECT_EQ(r45::FakeParserApi::s_deleted, 2);
	EXPECT_EQ(R45QuiescentParserApi::s_quiescent_releases, 2);
	EXPECT_EQ(R45QuiescentParserApi::s_destroyed_before_quiescence, 0);
}

TEST_F(R45ParserAdapterFakeTest, InitExtWithoutBaseDataManagerRetainsApi)
{
	R45QuiescentParserApi* api = new R45QuiescentParserApi(true, true);
	ParserAdapter pa(nullptr, nullptr);

	EXPECT_FALSE(pa.initExt("r45_no_base_data_manager", api));
	EXPECT_EQ(api->init_calls, 1);
	EXPECT_EQ(api->subscribe_calls, 0);
	EXPECT_EQ(r45::FakeParserApi::s_deleted, 0);

	pa.release();
	EXPECT_EQ(r45::FakeParserApi::s_release_calls, 1);
	EXPECT_EQ(r45::FakeParserApi::s_deleted, 1);
	EXPECT_EQ(R45QuiescentParserApi::s_destroyed_before_quiescence, 0);
}

TEST_F(R45ParserAdapterFakeTest, ReleaseCallsApiReleaseThenDeletesIt)
{
	R45QuiescentParserApi* api = new R45QuiescentParserApi(true, true);
	ParserAdapter pa(&_bdMgr, nullptr);
	ASSERT_TRUE(pa.initExt("r45_ext_rel", api));

	// ParserAdapter.cpp:205-217：_remover 为空 -> api->release() 后 delete
	pa.release();
	EXPECT_EQ(r45::FakeParserApi::s_release_calls, 1);
	EXPECT_EQ(r45::FakeParserApi::s_deleted, 1);
	EXPECT_EQ(R45QuiescentParserApi::s_quiescent_releases, 1);
	EXPECT_EQ(R45QuiescentParserApi::s_destroyed_before_quiescence, 0);

	// :207 置 _stopped 后，即使再喂 tick 也不再转发
	WTSTickData* tick = WTSTickData::create("IC2204");
	ASSERT_NE(tick, nullptr);
	WTSTickStruct& ts = tick->getTickStruct();
	ts.action_date = 20240102;
	ts.trading_date = 20240102;
	pa.handleQuote(tick, 0);
	EXPECT_TRUE(R45PaTickProbe::codes.empty());
	tick->release();
}

TEST_F(R45ParserAdapterFakeTest, ReleaseRetainsModuleContextWhenDeleterDefersSamePointer)
{
	void* moduleHandle = ::dlopen(nullptr, RTLD_NOW);
	ASSERT_NE(moduleHandle, nullptr);
	R45ControlledDeleterApi* api = new R45ControlledDeleterApi(true);
	ParserAdapterPtr adapter(new ParserAdapter(&_bdMgr, nullptr));
	ASSERT_TRUE(adapter->initExt("r45_deferred_delete", api));

	WTSVariant* context = WTSVariant::createObject();
	ASSERT_NE(context, nullptr);
	context->retain(); // one ref remains with the test; one is installed as adapter context
	ParserAdapterTestAccess::setRemover(*adapter, &r45_pa_controlled_delete_parser);
	ParserAdapterTestAccess::setModuleHandle(*adapter, moduleHandle);
	ParserAdapterTestAccess::setConfig(*adapter, context);
	ParserAdapterMgr mgr;
	ASSERT_TRUE(mgr.addAdapter("r45_deferred_delete", adapter));
	adapter.reset(); // the manager is now the only adapter owner

	// The fake deleter leaves the same API pointer while callback-stack cleanup
	// is not yet safe. IParserApi::release() does not set the fake's quiescence bit.
	mgr.release();
	EXPECT_EQ(R45ControlledDeleterApi::s_deleter_calls, 1);
	EXPECT_EQ(r45::FakeParserApi::s_release_calls, 1);
	EXPECT_EQ(r45::FakeParserApi::s_deleted, 0);
	EXPECT_TRUE(mgr.hasRetainedAdapters());
	EXPECT_EQ(mgr.size(), 1u);
	ParserAdapterPtr retained = mgr.getAdapter("r45_deferred_delete");
	ASSERT_NE(retained.get(), nullptr);
	EXPECT_EQ(ParserAdapterTestAccess::api(*retained), static_cast<IParserApi*>(api));
	EXPECT_EQ(ParserAdapterTestAccess::remover(*retained), &r45_pa_controlled_delete_parser);
	EXPECT_FALSE(ParserAdapterTestAccess::releaseUncertain(*retained));
	EXPECT_EQ(ParserAdapterTestAccess::moduleHandle(*retained), moduleHandle);
	EXPECT_EQ(ParserAdapterTestAccess::config(*retained), context);
	EXPECT_EQ(context->retainCount(), 2u);
	retained.reset();

	// A separate test-controlled barrier marks callbacks drained. Retry on that
	// safe path; successful cleanup also releases the valid process module handle.
	R45ControlledDeleterApi::markCallbacksQuiescent();
	mgr.release();
	EXPECT_EQ(R45ControlledDeleterApi::s_deleter_calls, 2);
	EXPECT_EQ(r45::FakeParserApi::s_release_calls, 2);
	EXPECT_EQ(r45::FakeParserApi::s_deleted, 1);
	EXPECT_EQ(R45ControlledDeleterApi::s_deleted_before_quiescence, 0);
	EXPECT_EQ(R45ControlledDeleterApi::s_deleted_after_quiescence, 1);
	EXPECT_FALSE(mgr.hasRetainedAdapters());
	EXPECT_EQ(mgr.size(), 0u);
	EXPECT_EQ(context->retainCount(), 1u);
	context->release();
}

TEST_F(R45ParserAdapterFakeTest, ReleaseStopsWhenDeleterChangesApiPointer)
{
	void* moduleHandle = ::dlopen(nullptr, RTLD_NOW);
	ASSERT_NE(moduleHandle, nullptr);
	r45::FakeParserApi* original = new r45::FakeParserApi(true, true);
	r45::FakeParserApi* replacement = new r45::FakeParserApi(true, true);
	ParserAdapter pa(&_bdMgr, nullptr);
	ASSERT_TRUE(pa.initExt("r45_uncertain_delete", original));

	WTSVariant* context = WTSVariant::createObject();
	ASSERT_NE(context, nullptr);
	context->retain();
	R45MutatingParserDeleter::s_replacement = replacement;
	ParserAdapterTestAccess::setRemover(pa, &R45MutatingParserDeleter::replacePointer);
	ParserAdapterTestAccess::setModuleHandle(pa, moduleHandle);
	ParserAdapterTestAccess::setConfig(pa, context);

	// V1 does not define a non-null replacement pointer's identity. Preserve both
	// addresses and all context, then make later release attempts inert.
	pa.release();
	EXPECT_EQ(R45MutatingParserDeleter::s_calls, 1);
	EXPECT_EQ(r45::FakeParserApi::s_release_calls, 1);
	EXPECT_EQ(r45::FakeParserApi::s_deleted, 0);
	EXPECT_EQ(ParserAdapterTestAccess::api(pa), static_cast<IParserApi*>(replacement));
	EXPECT_EQ(ParserAdapterTestAccess::uncertainOriginalApi(pa), static_cast<IParserApi*>(original));
	EXPECT_TRUE(ParserAdapterTestAccess::releaseUncertain(pa));
	EXPECT_EQ(ParserAdapterTestAccess::moduleHandle(pa), moduleHandle);
	EXPECT_EQ(ParserAdapterTestAccess::config(pa), context);
	EXPECT_EQ(context->retainCount(), 2u);

	pa.release();
	EXPECT_EQ(R45MutatingParserDeleter::s_calls, 1);
	EXPECT_EQ(r45::FakeParserApi::s_release_calls, 1);
	EXPECT_EQ(r45::FakeParserApi::s_deleted, 0);
	EXPECT_EQ(ParserAdapterTestAccess::moduleHandle(pa), moduleHandle);
	EXPECT_EQ(ParserAdapterTestAccess::config(pa), context);

	// The deliberately nonconforming fake left ownership identity unresolved.
	// Detach only in test cleanup, then reclaim its two synchronous fake objects.
	ParserAdapterTestAccess::abandonUncertainStateForTest(pa);
	::dlclose(moduleHandle);
	delete original;
	delete replacement;
	context->release(); // adapter's retained reference
	context->release(); // test's reference
}

TEST_F(R45ParserAdapterFakeTest, ManagerRetainsAdapterWhenPluginDeleterThrows)
{
	r45::FakeParserApi* api = new r45::FakeParserApi(true, true);
	ParserAdapterPtr adapter(new ParserAdapter(&_bdMgr, nullptr));
	ASSERT_TRUE(adapter->initExt("r45_throwing_deleter", api));
	ParserAdapterTestAccess::setRemover(*adapter, &R45ThrowingParserDeleter::throwAfterCall);

	ParserAdapterMgr mgr;
	ASSERT_TRUE(mgr.addAdapter("r45_throwing_deleter", adapter));
	adapter.reset();

	// The adapter catches the plugin exception and marks ownership uncertain;
	// manager release must retain its sole shared owner instead of clearing it.
	mgr.release();
	EXPECT_TRUE(mgr.hasRetainedAdapters());
	EXPECT_EQ(mgr.size(), 1u);
	EXPECT_EQ(R45ThrowingParserDeleter::s_calls, 1);
	EXPECT_EQ(r45::FakeParserApi::s_release_calls, 1);
	EXPECT_EQ(r45::FakeParserApi::s_deleted, 0);
	ParserAdapterPtr retained = mgr.getAdapter("r45_throwing_deleter");
	ASSERT_NE(retained.get(), nullptr);
	EXPECT_TRUE(ParserAdapterTestAccess::releaseUncertain(*retained));
	EXPECT_EQ(ParserAdapterTestAccess::api(*retained), static_cast<IParserApi*>(api));
	retained.reset();

	// Sticky uncertainty blocks further provider calls until an external recovery
	// decision; it must not silently clear the map on another shutdown pass.
	mgr.release();
	EXPECT_TRUE(mgr.hasRetainedAdapters());
	EXPECT_EQ(mgr.size(), 1u);
	EXPECT_EQ(R45ThrowingParserDeleter::s_calls, 1);
	EXPECT_EQ(r45::FakeParserApi::s_release_calls, 1);
	EXPECT_EQ(r45::FakeParserApi::s_deleted, 0);

	// Test cleanup manually reclaims the synchronous fake after proving it stayed
	// retained; this is not an adapter recovery path.
	retained = mgr.getAdapter("r45_throwing_deleter");
	ASSERT_NE(retained.get(), nullptr);
	ParserAdapterTestAccess::abandonUncertainStateForTest(*retained);
	delete api;
	retained.reset();
	mgr.release();
	EXPECT_FALSE(mgr.hasRetainedAdapters());
	EXPECT_EQ(mgr.size(), 0u);
}

// ---------------------------------------------------------------------------
// handle* 正路径（有 runner / 有合约）
// ---------------------------------------------------------------------------

TEST_F(R45ParserAdapterFakeTest, HandleQuoteForwardsTickToRunnerCallback)
{
	WtDtRunner runner;
	initRunner(runner);
	runner.sub_tick("CFFEX.IC.2204", true, false);

	ParserAdapter pa(&runner.getBaseDataMgr(), &runner);
	WTSTickData* tick = WTSTickData::create("IC2204");
	ASSERT_NE(tick, nullptr);
	WTSTickStruct& ts = tick->getTickStruct();
	wt_strcpy(ts.exchg, "CFFEX");
	ts.price = 101.5;
	ts.action_date = 20240102;
	ts.trading_date = 20240102;

	// ParserAdapter.cpp:278-282：日期齐全且 _dt_runner!=NULL -> proc_tick ->
	// WtDtRunner 分月码标准化 -> 订阅回调
	pa.handleQuote(tick, 0);
	ASSERT_EQ(R45PaTickProbe::codes.size(), 1u);
	EXPECT_EQ(R45PaTickProbe::codes[0], "CFFEX.IC.2204");
	tick->release();
}

TEST_F(R45ParserAdapterFakeTest, DetailHandlersPassContractLookup)
{
	ParserAdapter pa(&_bdMgr, nullptr);
	ASSERT_NE(_bdMgr.getContract("IC2204", "CFFEX"), nullptr);

	WTSTransStruct tss;
	memset(&tss, 0, sizeof(tss));
	wt_strcpy(tss.code, "IC2204");
	wt_strcpy(tss.exchg, "CFFEX");
	tss.action_date = 20240102;
	tss.trading_date = 20240102;
	WTSTransData* trans = WTSTransData::create(tss);
	ASSERT_NE(trans, nullptr);
	// :242-246 合约命中后函数正常走完（当前实现无后续动作）
	pa.handleTransaction(trans);
	trans->release();

	WTSOrdDtlStruct ods;
	memset(&ods, 0, sizeof(ods));
	wt_strcpy(ods.code, "IC2204");
	wt_strcpy(ods.exchg, "CFFEX");
	ods.action_date = 20240102;
	ods.trading_date = 20240102;
	WTSOrdDtlData* ordDtl = WTSOrdDtlData::create(ods);
	ASSERT_NE(ordDtl, nullptr);
	// :256-260
	pa.handleOrderDetail(ordDtl);
	ordDtl->release();

	WTSOrdQueStruct oqs;
	memset(&oqs, 0, sizeof(oqs));
	wt_strcpy(oqs.code, "IC2204");
	wt_strcpy(oqs.exchg, "CFFEX");
	oqs.action_date = 20240102;
	oqs.trading_date = 20240102;
	WTSOrdQueData* ordQue = WTSOrdQueData::create(oqs);
	ASSERT_NE(ordQue, nullptr);
	// :270-273
	pa.handleOrderQueue(ordQue);
	ordQue->release();
}

// ---------------------------------------------------------------------------
// ParserAdapterMgr 扇出
// ---------------------------------------------------------------------------

TEST_F(R45ParserAdapterFakeTest, ManagerRunsAndReleasesLiveAdapters)
{
	R45QuiescentParserApi* api1 = new R45QuiescentParserApi(true, true);
	R45QuiescentParserApi* api2 = new R45QuiescentParserApi(true, true);
	ParserAdapterPtr pa1(new ParserAdapter(&_bdMgr, nullptr));
	ParserAdapterPtr pa2(new ParserAdapter(&_bdMgr, nullptr));
	ASSERT_TRUE(pa1->initExt("r45_mgr1", api1));
	ASSERT_TRUE(pa2->initExt("r45_mgr2", api2));

	ParserAdapterMgr mgr;
	ASSERT_TRUE(mgr.addAdapter("r45_mgr1", pa1));
	ASSERT_TRUE(mgr.addAdapter("r45_mgr2", pa2));
	EXPECT_EQ(mgr.size(), 2u);
	EXPECT_EQ(mgr.getAdapter("r45_mgr2").get(), pa2.get());

	// ParserAdapter.cpp:339-347 逐个 run -> connect
	mgr.run();
	EXPECT_EQ(api1->connect_calls, 1);
	EXPECT_EQ(api2->connect_calls, 1);

	// ParserAdapter.cpp:301-309 逐个 release -> api release + delete，表清空
	mgr.release();
	EXPECT_EQ(r45::FakeParserApi::s_release_calls, 2);
	EXPECT_EQ(r45::FakeParserApi::s_deleted, 2);
	EXPECT_EQ(R45QuiescentParserApi::s_quiescent_releases, 2);
	EXPECT_EQ(R45QuiescentParserApi::s_destroyed_before_quiescence, 0);
	EXPECT_EQ(mgr.size(), 0u);
	EXPECT_EQ(mgr.getAdapter("r45_mgr1").get(), nullptr);
}

// ---------------------------------------------------------------------------
/*!
 * ParserAdapterMgr::release() 对登记 adapter 释放一次后清空集合；公开生命周期没有
 * 声明 ParserAdapter::release() 可重复调用。直接二次调用不据此判定为产品缺陷。
 */
TEST_F(R45ParserAdapterFakeTest, DISABLED_ReleaseTwiceContractPending)
{
	GTEST_SKIP() << "Only one release per registered adapter is established; repeated release has no public contract.";
}

}  // namespace
