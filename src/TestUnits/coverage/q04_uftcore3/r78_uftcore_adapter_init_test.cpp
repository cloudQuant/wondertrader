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

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

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

// 在临时目录里搭 <tmp>/parsers/libParserUDP.dylib，返回是否可用
bool linkUdpParser(q04::TempRoot& tmp, std::string& moduleRoot)
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

	std::filesystem::create_symlink(src, root + "/parsers/libParserUDP.dylib", ec);
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
	EXPECT_STREQ("r78trader1", adapter.id());
	EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, adapter.state());
	EXPECT_FALSE(adapter.run());

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
	std::filesystem::create_symlink(dylib, tmp.file("traders/libfmt.dylib"), ec);
	ASSERT_FALSE(ec);

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
		EXPECT_STREQ("r78trader2", adapter.id());
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
	if (!linkUdpParser(tmp, moduleRoot))
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
	if (!linkUdpParser(tmp, moduleRoot))
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
		std::filesystem::create_symlink(dylib, mixed + "/libNoEntry.dylib", ec);
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
