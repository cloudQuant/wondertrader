/*!
 * \file r54_uftcore_guards_test.cpp
 * \brief 第十一波 R54: ParserAdapter 停止守卫/UftStrategyMgr 插件登记/WtUftEngine 边角
 *
 * 目标:
 *  - src/WtUftCore/ParserAdapter.cpp: initExt 全量订阅循环、release 后四类 handler 的 _stopped 守卫
 *  - src/WtUftCore/UftStrategyMgr.cpp: 真实策略插件登记与 createStrategy/getStrategy 命中、
 *    以及 dlopen 成功但缺 createStrategyFact 的失败分支
 *  - src/WtUftCore/WtUftEngine.cpp: _cfg 释放、get_session_info(code,true) 的 commInfo 空会话分支
 *
 * 策略插件从测试可执行文件同级的 WtUftRunner 目录取(构建产物), 不联网/不起线程;
 * 若该构建没有插件则 GTEST_SKIP, 不影响其他用例。
 */
#include "q04_trader_fixture.h"

#include "WtUftCore/ParserAdapter.h"
#include "WtUftCore/UftStrategyMgr.h"
#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/WtHelper.h"

#include "Includes/IParserApi.h"
#include "Includes/WTSDataDef.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include <mach-o/dyld.h>

using namespace wtp;

namespace {

//////////////////////////////////////////////////////////////////////////
// ParserAdapter 纯内存替身
struct R54ApiStats
{
	uint32_t _initCnt = 0;
	uint32_t _releaseCnt = 0;
	uint32_t _connectCnt = 0;
	uint32_t _regSpiCnt = 0;
	uint32_t _subCnt = 0;
	size_t	 _subbedSize = 0;
};

class R54FakeParserApi : public IParserApi
{
public:
	explicit R54FakeParserApi(R54ApiStats* stats) : _stats(stats) {}

	bool init(WTSVariant* /*config*/) override { _stats->_initCnt++; return true; }
	void release() override { _stats->_releaseCnt++; }
	bool connect() override { _stats->_connectCnt++; return true; }
	void subscribe(const CodeSet& setCodes) override
	{
		_stats->_subCnt++;
		_stats->_subbedSize = setCodes.size();
	}
	void registerSpi(IParserSpi* /*spi*/) override { _stats->_regSpiCnt++; }

private:
	R54ApiStats* _stats;
};

class R54FakeParserStub : public IParserStub
{
public:
	uint32_t _quoteCnt = 0;
	uint32_t _oqCnt = 0;
	uint32_t _odCnt = 0;
	uint32_t _trCnt = 0;

	void handle_push_quote(WTSTickData* /*curTick*/) override { _quoteCnt++; }
	void handle_push_order_queue(WTSOrdQueData* /*curOrdQue*/) override { _oqCnt++; }
	void handle_push_order_detail(WTSOrdDtlData* /*curOrdDtl*/) override { _odCnt++; }
	void handle_push_transaction(WTSTransData* /*curTrans*/) override { _trCnt++; }
};

//////////////////////////////////////////////////////////////////////////
// 可执行文件目录, 用于定位同级构建产物
std::string exeDir()
{
	char buf[4096] = { 0 };
	uint32_t size = sizeof(buf);
	if (_NSGetExecutablePath(buf, &size) != 0)
		return std::string();
	std::error_code ec;
	auto p = std::filesystem::weakly_canonical(std::filesystem::path(buf), ec);
	if (ec)
		return std::string();
	return p.parent_path().string();
}

std::string findRealStrategyPlugin()
{
	const std::string dir = exeDir();
	const std::string candidates[] = {
		dir + "/../WtUftRunner/libWtUftStraFact.dylib",
		dir + "/libWtUftStraFact.dylib",
		WtHelper::getCWD() + "WtUftRunner/libWtUftStraFact.dylib",
		WtHelper::getCWD() + "bin/WtUftRunner/libWtUftStraFact.dylib",
	};
	for (const auto& one : candidates)
	{
		std::error_code ec;
		if (std::filesystem::exists(one, ec))
			return one;
	}
	return std::string();
}

std::string findDylibWithoutEntry()
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

}	// namespace

// ParserAdapter.cpp:166-226 initExt 正常装配: registerSpi + 空 filter 全量订阅
// ParserAdapter.cpp:184-211 全量订阅循环体(两个合约)
// ParserAdapter.cpp:228-240 release: api->release + delete
// ParserAdapter.cpp:266-336 release 后 order_queue/order_detail/transaction/handleParserLog
// 的 _stopped 守卫(传 nullptr 也不会解引用)
TEST(R54ParserGuard, release_stops_all_handlers)
{
	q04::FakeBaseDataMgr bd;
	WTSContractInfo* ag = bd.add("SHFE", "ag", "ag2406");
	WTSContractInfo* au = bd.add("SHFE", "au", "au2406");
	ASSERT_NE(nullptr, ag);
	ASSERT_NE(nullptr, au);
	// 显式补 alt code, 让全量订阅集合可确定(不依赖未初始化的 m_strFullAltCode)
	ag->setAltCode("ag2406");
	au->setAltCode("au2406");
	// FakeBaseDataMgr 的 getContracts() 以 append(obj, false) 返回裸指针
	// （q04_trader_fixture.h），而 WTSArray::release() 的 clear() 会无条件
	// release 每个元素（WTSCollection.hpp:206-225），与 append 的 bAutoRetain
	// 无关。这里先 retain，避免适配器释放数组时把 fixture 仅有的引用计数清零，
	// 进而在 bd 析构时二次 release 悬空指针（原 CRASH 根因）。
	ag->retain();
	au->retain();
	R54FakeParserStub stub;
	R54ApiStats stats;

	ParserAdapter adapter;
	R54FakeParserApi* api = new R54FakeParserApi(&stats);
	ASSERT_TRUE(adapter.initExt("r54parser", api, &stub, &bd));
	EXPECT_EQ(1u, stats._initCnt);
	EXPECT_EQ(1u, stats._regSpiCnt);
	EXPECT_EQ(1u, stats._subCnt);
	EXPECT_EQ(2u, stats._subbedSize);
	EXPECT_TRUE(adapter.run());
	EXPECT_EQ(1u, stats._connectCnt);

	adapter.release();
	EXPECT_EQ(1u, stats._releaseCnt);

	// release 已把 _stopped 置真, 这些调用必须第一行返回
	adapter.handleOrderQueue(nullptr);
	adapter.handleOrderDetail(nullptr);
	adapter.handleTransaction(nullptr);
	adapter.handleParserLog(LL_INFO, "must be ignored");
	adapter.handleQuote(nullptr, 0);
	EXPECT_EQ(0u, stub._quoteCnt);
	EXPECT_EQ(0u, stub._oqCnt);
	EXPECT_EQ(0u, stub._odCnt);
	EXPECT_EQ(0u, stub._trCnt);
}

// UftStrategyMgr.cpp:30-92 真实插件登记: load_library/get_symbol/createStrategyFact/creator
// UftStrategyMgr.cpp:94-104 createStrategy(fact,unit,id) 成功
// UftStrategyMgr.cpp:106-123 createStrategy(name,id) 成功
// UftStrategyMgr.cpp:125-132 getStrategy 命中
//
// 注意：manager 故意放在堆上且不 delete。产品缺陷 BUG-R64-002：
// UftStrategyDefs.h:207 把删除函数的类型声明为 void(*)(IUftStrategyFact*&)，
// 而插件 WtUftStraFact.cpp:43 导出的是 void deleteStrategyFact(IUftStrategyFact*)（按值）。
// UftStrategyMgr 析构时 `_remover(_fact)` 实际把 _fact 成员的地址传给插件，
// 插件再 delete 这个内部地址 -> SIGSEGV。该崩溃发生在析构阶段，与 load/create 语义无关；
// 本用例只验证登记/创建/查询，进程退出时由 OS 回收 manager，崩溃复现见下方 DISABLED 用例。
TEST(R54StraMgr, loads_plugin_and_creates_strategy)
{
	const std::string plugin = findRealStrategyPlugin();
	if (plugin.empty())
		GTEST_SKIP() << "libWtUftStraFact.dylib not found in this build";

	q04::TempRoot tmp("r54mgr");
	const std::string link = tmp.file("libWtUftStraFact.dylib");
	std::error_code ec;
	std::filesystem::create_symlink(plugin, link, ec);
	ASSERT_FALSE(ec) << "create_symlink failed: " << ec.message();

	UftStrategyMgr* mgr = new UftStrategyMgr();
	EXPECT_TRUE(mgr->loadFactories(tmp.dir().string().c_str()));

	UftStrategyPtr p = mgr->createStrategy("WtUftStraFact.SimpleUft", "r54stra1");
	ASSERT_NE(nullptr, p);
	ASSERT_NE(nullptr, p->self());
	EXPECT_STREQ("UftDemoStrategy", p->self()->getName());
	EXPECT_NE(nullptr, mgr->getStrategy("r54stra1"));

	UftStrategyPtr p2 = mgr->createStrategy("WtUftStraFact", "SimpleUft", "r54stra2");
	ASSERT_NE(nullptr, p2);
	EXPECT_NE(nullptr, p2->self());
	EXPECT_NE(nullptr, mgr->getStrategy("r54stra2"));

	EXPECT_EQ(nullptr, mgr->createStrategy("MissingFact.SimpleUft", "r54stra3"));
	EXPECT_EQ(nullptr, mgr->getStrategy("r54stra3"));
}

// 产品缺陷占位（BUG-R64-002）：真实插件 register 成功后，UftStrategyMgr 析构即 SIGSEGV。
// 见 docs/迭代计划/迭代01/bugs/R64.md；默认不执行，避免进程崩溃。
// UftStrategyDefs.h:207 / WtUftStraFact.cpp:43 / UftStrategyMgr.cpp:76
TEST(R54StraMgr, DISABLED_manager_destructor_releases_factory_with_mismatched_abi)
{
	const std::string plugin = findRealStrategyPlugin();
	if (plugin.empty())
		GTEST_SKIP() << "libWtUftStraFact.dylib not found in this build";

	q04::TempRoot tmp("r54mgrdtor");
	const std::string link = tmp.file("libWtUftStraFact.dylib");
	std::error_code ec;
	std::filesystem::create_symlink(plugin, link, ec);
	ASSERT_FALSE(ec) << "create_symlink failed: " << ec.message();

	{
		UftStrategyMgr mgr;
		ASSERT_TRUE(mgr.loadFactories(tmp.dir().string().c_str()));
		// 作用域结束时 _factories 析构，_remover(_fact) 用错误签名调用插件 deleteStrategyFact
	}
}

// UftStrategyMgr.cpp:57-66 dlopen 成功但缺 createStrategyFact 时 free_library + continue
TEST(R54StraMgr, skips_module_without_entry_symbol)
{
	const std::string dylib = findDylibWithoutEntry();
	if (dylib.empty())
		GTEST_SKIP() << "no candidate dylib without createStrategyFact";

	q04::TempRoot tmp("r54mgrbad");
	const std::string link = tmp.file("libNoEntry.dylib");
	std::error_code ec;
	std::filesystem::create_symlink(dylib, link, ec);
	ASSERT_FALSE(ec) << "create_symlink failed: " << ec.message();

	UftStrategyMgr mgr;
	EXPECT_TRUE(mgr.loadFactories(tmp.dir().string().c_str()));
	EXPECT_EQ(nullptr, mgr.createStrategy("NoEntry.SimpleUft", "r54stra4"));
}

// WtUftEngine.cpp:58-71 析构时释放 _cfg
// WtUftEngine.cpp:152-168 get_session_info(code,true) 合约存在但 commInfo 无 session
TEST(R54EngineGap, cfg_release_and_session_lookup)
{
	q04::FakeBaseDataMgr bd;
	WTSContractInfo* ag = bd.add("SHFE", "ag", "ag2406");
	// WTSCommodityInfo 构造未初始化 m_pSession, 显式置空让分支可确定
	ASSERT_NE(nullptr, ag);
	ASSERT_NE(nullptr, ag->getCommInfo());
	ag->getCommInfo()->setSessionInfo(nullptr);

	WTSVariant* cfg = WTSVariant::createObject();
	{
		WtUftEngine engine;
		engine.init(cfg, &bd, nullptr, nullptr);
		EXPECT_EQ(nullptr, engine.get_session_info("SHFE.ag2406", true));
		EXPECT_EQ(nullptr, engine.get_session_info("SHFE.unknown", true));
	}
	// engine 析构已 release 一次, 这里释放创建时的那一份
	cfg->release();
}
