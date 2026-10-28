/*!
 * \file r85_uftcore_trader_paths_test.cpp
 * \brief 迭代01 第十六波 R85 - TraderAdapter 模块装载成功链与错单去重早退
 *
 * 覆盖（只读生产代码）:
 *  - TraderAdapter.cpp:189-205 createTrader 之后的 _remover 取符号与
 *    _trader_api->init(params) 成功返回。模块用构建产物 libTraderMocker.dylib，
 *    TraderMocker::init 只解析参数并 create_directories("./mocker_<id>/")，
 *    不 connect / 不起线程；release() 在未 connect 时只置终止标志。
 *    用例临时切到系统临时目录再切回，保证 mocker 目录不落进仓库/构建目录。
 *  - TraderAdapter.cpp:1183-1189 onRspEntrust 的「错单但未完成量为 0」早退：
 *    实盘中同一条错单会推送两次，第二次必须静默丢弃。
 *
 * 不调用 checkCancelLimits/checkOrderLimits 的风险参数回退分支：getRiskParams
 * 在未命中时直接对 end() 取址（BUG-Q04-005），无 default 组时是 UB，本文件不碰。
 */
#include "q04_trader_fixture.h"

#include "WtUftCore/ActionPolicyMgr.h"
#include "WtUftCore/TraderAdapter.h"
#include "WtUftCore/WtHelper.h"

#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"

#include <gtest/gtest.h>

#include <dlfcn.h>
#include <unistd.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace wtp;

namespace {

// 测试可执行体在 <bin>/TestUnits/，产品插件在 <bin>/ 下
std::string binDir()
{
	Dl_info info;
	if (dladdr(reinterpret_cast<void*>(&binDir), &info) == 0 || info.dli_fname == nullptr)
		return std::string();
	fs::path p = fs::path(info.dli_fname).parent_path().parent_path();
	std::string ret = p.string();
	if (!ret.empty() && ret.back() != '/')
		ret.push_back('/');
	return ret;
}

// getModulePath 拼的是 <instdir>/traders/<module>，所以临时模块根下要有一层 traders/
bool stageTrader(const fs::path& root, const std::string& bin, const char* name)
{
	std::error_code ec;
	fs::create_directories(root / "traders", ec);
	if (ec)
		return false;
	const std::string src = bin + name;
	if (!fs::exists(src, ec))
		return false;
	fs::create_symlink(src, root / "traders" / name, ec);
	return !ec;
}

class InstDirGuard
{
public:
	explicit InstDirGuard(const std::string& dir)
		: _old(WtHelper::getInstDir())
	{
		WtHelper::setInstDir(dir.c_str());
	}
	~InstDirGuard() { WtHelper::setInstDir(_old.c_str()); }
	InstDirGuard(const InstDirGuard&) = delete;
	InstDirGuard& operator=(const InstDirGuard&) = delete;

private:
	std::string _old;
};

class CwdGuard
{
public:
	CwdGuard() : _old(fs::current_path()) {}
	~CwdGuard() { std::error_code ec; fs::current_path(_old, ec); }
	CwdGuard(const CwdGuard&) = delete;
	CwdGuard& operator=(const CwdGuard&) = delete;

private:
	fs::path _old;
};

}	// namespace

// TraderAdapter.cpp:189-205：createTrader 成功 + deleteTrader 取符号 + api->init 成功
TEST(R85UftTraderInit, loads_mock_trader_module_and_returns_true)
{
	const std::string bin = binDir();
	if (bin.empty())
		GTEST_SKIP() << "cannot resolve build bin dir";

	q04::TempRoot tmp("r85_trader");
	if (!stageTrader(tmp.dir(), bin, "libTraderMocker.dylib"))
		GTEST_SKIP() << "libTraderMocker.dylib not found in this build";

	InstDirGuard instdir(tmp.dir().string() + "/");

	q04::FakeBaseDataMgr bd;
	bd.add("SHFE", "ag", "ag2406");
	ActionPolicyMgr policy;

	WTSVariant* params = WTSVariant::createObject();
	params->append("module", "TraderMocker");
	params->append("mockerid", (uint32_t)7777);
	params->append("span", (uint32_t)0);

	// riskmon 只用来走 init 里的解析段（default 组保留，避免后面误触 BUG-Q04-005）
	WTSVariant* riskmon = WTSVariant::createObject();
	riskmon->append("active", true);
	WTSVariant* policyCfg = WTSVariant::createObject();
	WTSVariant* rule = WTSVariant::createObject();
	rule->append("cancel_total_limits", (uint32_t)0);
	rule->append("cancel_times_boundary", (uint32_t)0);
	rule->append("cancel_stat_timespan", (uint32_t)0);
	rule->append("order_total_limits", (uint32_t)0);
	rule->append("order_times_boundary", (uint32_t)0);
	rule->append("order_stat_timespan", (uint32_t)0);
	policyCfg->append("default", rule);
	riskmon->append("policy", policyCfg);
	params->append("riskmon", riskmon);

	{
		CwdGuard cwd;
		std::error_code ec;
		fs::current_path(tmp.dir(), ec);
		ASSERT_FALSE(ec);
		TraderAdapter adapter;
		EXPECT_TRUE(adapter.init("r85mocktrader", params, &bd, &policy));
		EXPECT_STREQ("r85mocktrader", adapter.id());
		EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, adapter.state());
		// 未 connect 的 mock：release 只置终止标志并清空 spi
		adapter.release();
	}

	params->release();

	EXPECT_FALSE(fs::exists(tmp.dir() / "mocker_7777" / "positions.json"));
}

// TraderAdapter.cpp:1183-1189：错单推送时未完成量为 0（重复通知）直接返回
TEST(R85UftTraderReply, error_report_without_undone_qty_is_ignored)
{
	q04::FakeTraderApi api;
	q04::FakeBaseDataMgr bd;
	ActionPolicyMgr policy;
	q04::FakeSink sink;
	bd.add("SHFE", "ag", "ag2406");

	TraderAdapter adapter;
	ASSERT_TRUE(adapter.initExt("r85reply", &api, &bd, &policy));
	adapter.addSink(&sink);

	WTSEntrust* entrust = WTSEntrust::create("ag2406", 2.0, 5000.0, "SHFE");
	ASSERT_NE(nullptr, entrust);
	entrust->setDirection(WDT_LONG);
	entrust->setOffsetType(WOT_OPEN);
	entrust->setUserTag("wtp.r85reply.1");

	WTSError* err = WTSError::create(WEC_ORDERINSERT, "rejected by exchange");
	ASSERT_NE(nullptr, err);

	// _undone_qty 里没有 "SHFE.ag2406"，说明这是重复通知 -> 直接 return
	adapter.onRspEntrust(entrust, err);
	EXPECT_DOUBLE_EQ(0.0, adapter.getUndoneQty("SHFE.ag2406"));
	EXPECT_TRUE(sink._entrusts.empty());

	err->release();
	entrust->release();
}
