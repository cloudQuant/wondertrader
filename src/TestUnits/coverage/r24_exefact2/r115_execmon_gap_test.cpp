// R115 lane：WtExecMon 的残余行（WtSimpDataMgr / WtExecRunner / WtExecPorter）
// 目标文件与行段（llvm-cov show 实读）：
//   src/WtExecMon/WtSimpDataMgr.cpp: L50（store.module 留空走默认 WtDataStorage）、
//       L63-67（dylib 载入成功但没有 createDataReader）、L111-113（get_date/get_min_time/get_secs）、
//       L156-158（tick 正好落在时段末 -> minutes--）、L169-171（缓存里没有该 code）、
//       L173-175（抓最新 tick 并 retain）、L183（reader 非空时 readTickSlice）、
//       L192-197（times==1 直读）、L200-220（times>1 的重采样入口与空库返回）
//   src/WtExecMon/WtExecRunner.cpp: L262-264/L290-292/L301-304（local/diff 未配 trader、
//       diff 配了不存在的 trader）、L440-447（品种存在时返回其 session）
//   src/WtExecMon/WtExecPorter.cpp: L72（config_exec 空串走默认 cfgexec.json）、L82-85（run_exec）
//
// 与 R7/R17 的分工：R7 覆盖 runner 的 init/config 拒绝面与空跑；R17 覆盖 SimpData 的拒绝分支、
// Porter 的 init/config/set/commit/write_log。R17 把 initStore 成功面判为不可达（BUG-R7-018），
// 实测不是：只要把 InstDir 指到 bin/（测试运行目录的上一级），空 module 的默认 WtDataStorage
// 就能载入；本文件据此补成功面，不重复 R17 的拒绝面用例。
//
// 纪律：不读挂钟做断言；临时目录用系统临时目录；不加载除 WtDataStorage 之外的插件；
// 不启线程（executers 均不写 poolsize）。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "Includes/WTSVariant.hpp"
#include "Includes/WTSTypes.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "WtCore/WtHelper.h"
#include "WtExecMon/WtExecRunner.h"
#include "WtExecMon/WtSimpDataMgr.h"
#include "WtExecMon/WtExecPorter.h"

using namespace wtp;

namespace
{
class TempDir
{
public:
	explicit TempDir(const char* tag)
	{
		static uint32_t seq = 0;
		_dir = std::filesystem::temp_directory_path() /
			("r115_" + std::string(tag) + "_" + std::to_string((long long)::getpid()) + "_" + std::to_string(++seq));
		std::error_code ec;
		std::filesystem::create_directories(_dir, ec);
	}

	std::string dir() const { return _dir.string(); }
	std::string file(const std::string& name) const { return (_dir / name).string(); }

	void mkdir(const std::string& name) const
	{
		std::error_code ec;
		std::filesystem::create_directories(_dir / name, ec);
	}

	void write(const std::string& name, const std::string& body) const
	{
		std::ofstream ofs((_dir / name).string().c_str());
		ofs << body;
		ofs.close();
	}

	~TempDir()
	{
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

private:
	std::filesystem::path _dir;
};

class ScopedCwd
{
public:
	explicit ScopedCwd(const std::string& path)
	{
		_old = std::filesystem::current_path();
		std::error_code ec;
		std::filesystem::current_path(path, ec);
	}

	~ScopedCwd()
	{
		std::error_code ec;
		std::filesystem::current_path(_old, ec);
	}

private:
	std::filesystem::path _old;
};

// 保存/恢复 WtHelper::InstDir，避免影响同进程的其它用例
class ScopedInstDir
{
public:
	ScopedInstDir() : _old(WtHelper::getInstDir()) {}
	~ScopedInstDir() { WtHelper::setInstDir(_old.c_str()); }
	void set(const std::string& dir) { WtHelper::setInstDir(dir.c_str()); }

private:
	std::string _old;
};

const char* const kSessionCfg = R"({
	"R115S":{"name":"r115连续","offset":0,
		"sections":[{"from":930,"to":1130},{"from":1300,"to":1500}]}
})";

const char* const kCommodityCfg = R"({
	"SHFE":{"rb":{"name":"r115螺纹","session":"R115S","holiday":"CHINA",
		"pricetick":1.0,"volscale":10,"covermode":0,"pricemode":1}}
})";

const char* const kContractCfg = R"({
	"SHFE":{"rb2405":{"name":"螺纹2405","exchg":"SHFE","product":"rb"}}
})";

const char* const kPolicyCfg = R"({
	"default":{"order":[{"action":"close","limit":0},{"action":"open","limit":0}]}
})";

const char* const kExecutersCfg = R"({
	"executers":[
		{"id":"r115e1","name":"local","active":true,"scale":1.0},
		{"id":"r115e2","name":"diff","active":true,"scale":1.0},
		{"id":"r115e3","name":"diff","active":true,"trader":"r115_ghost"}
	]
})";

WTSTickData* mkTick(const char* code, uint32_t date, uint32_t actTime, double px)
{
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	snprintf(ts.code, sizeof(ts.code), "%s", code);
	ts.action_date = date;
	ts.trading_date = date;
	ts.action_time = actTime;
	ts.price = px;
	ts.pre_close = px - 0.5;
	ts.bid_prices[0] = px - 0.01;
	ts.bid_qty[0] = 100;
	ts.ask_prices[0] = px + 0.01;
	ts.ask_qty[0] = 100;
	return WTSTickData::create(ts);
}
}	// namespace

//////////////////////////////////////////////////////////////////////////
// SimpData 成功面：store 不写 module -> 默认 WtDataStorage；tick 落在 11:30 时段末；
// 缓存抓取 + reader 直读/重采样入口。
// 生产文件: src/WtExecMon/WtSimpDataMgr.cpp:50,111-113,156-158,169-175,183,192-197,200-220
//////////////////////////////////////////////////////////////////////////
TEST(r115_execmon, simpdata_default_store_quote_getters_and_slices)
{
	TempDir env("simpdata");
	env.write("sessions.json", kSessionCfg);
	env.write("commodities.json", kCommodityCfg);
	env.write("contracts.json", kContractCfg);
	env.write("actpolicy.json", kPolicyCfg);
	env.mkdir("storedata");

	WtExecRunner runner;
	ASSERT_TRUE(runner.init("./r115_no_such_logcfg.json", true));

	std::string cfg;
	cfg += "{\"basefiles\":{\"session\":\"" + env.file("sessions.json") + "\",";
	cfg += "\"commodity\":\"" + env.file("commodities.json") + "\",";
	cfg += "\"contract\":\"" + env.file("contracts.json") + "\"}";
	cfg += ",\"bspolicy\":\"" + env.file("actpolicy.json") + "\"}";
	ASSERT_TRUE(runner.config(cfg.c_str(), false));

	// handle_push_quote 要用 _s_info（L153），先确认 session/commodity/contract 都装进来了
	ASSERT_NE(runner.get_session_info("R115S", false), nullptr);
	ASSERT_NE(runner.get_session_info("SHFE.rb.2405", true), nullptr);
	ASSERT_NE(runner.get_comm_info("SHFE.rb.2405"), nullptr);

	// store 段：只写 path/session，不写 module -> L49-50 的默认模块分支；
	// InstDir 指到 bin/（TestUnits 的上一级），默认 libWtDataStorage.dylib 就在那里。
	ScopedInstDir inst;
	inst.set(std::string(WtHelper::getInstDir()) + "../");

	WTSVariant* root = WTSVariant::createObject();
	WTSVariant* store = WTSVariant::createObject();
	store->append("path", (env.dir() + "/storedata").c_str());
	store->append("session", "R115S");
	root->append("store", store);

	// 成员 _cur_* / _s_info 没在构造函数里初始化（BUG-N05-001 的同一族）；用静态存储
	// 先零初始化再跑构造函数，使首次 handle_push_quote 的 L143 判断确定可预期。
	static WtSimpDataMgr mgr;
	ASSERT_TRUE(mgr.init(root, &runner));
	root->release();
	ASSERT_NE(mgr.reader(), nullptr);		// .h L155

	// 11:30 正是第一交易时段末：L154 isLastOfSection=true -> L156-158 minutes--
	WTSTickData* tick = mkTick("SHFE.rb.2405", 20240102, 113000000, 3500.0);
	mgr.handle_push_quote("SHFE.rb.2405", tick);
	EXPECT_EQ(mgr.get_date(), 20240102u);			// L105-108
	EXPECT_EQ(mgr.get_secs(), 0u);					// L115-118
	EXPECT_EQ(mgr.get_min_time(), 1130u);			// L110-113 + L153-160
	EXPECT_EQ(mgr.get_trading_day(), 20240102u);

	WTSTickData* last = mgr.grab_last_tick("SHFE.rb.2405");
	ASSERT_NE(last, nullptr);						// L169-175
	EXPECT_DOUBLE_EQ(last->price(), 3500.0);
	last->release();
	EXPECT_EQ(mgr.grab_last_tick("SHFE.rb.2409"), nullptr);	// L169-171

	// L183：reader 非空即透传；空库返回 NULL 属正常
	mgr.get_tick_slice("SHFE.rb.2405", 10, 0);
	// L192-197：times==1 的直读路径
	EXPECT_EQ(mgr.get_kline_slice("SHFE.rb.2405", KP_Minute1, 1, 5, 0), nullptr);
	// L200-220：times>1 的重采样入口；空库在 L218-220 返回 NULL
	EXPECT_EQ(mgr.get_kline_slice("SHFE.rb.2405", KP_Minute1, 5, 4, 0), nullptr);

	tick->release();
}

//////////////////////////////////////////////////////////////////////////
// SimpData 缺符号 + ExecRunner 执行器变体 + 时段为空 + Porter 的 config_exec/run_exec
// 生产文件: WtSimpDataMgr.cpp:63-67；WtExecRunner.cpp:262-264,290-292,301-304,447；
//           WtExecPorter.cpp:72,82-85
//////////////////////////////////////////////////////////////////////////
TEST(r115_execmon, simpdata_missing_symbol_and_runner_variants)
{
	TempDir env("execmon");
	env.write("sessions.json", kSessionCfg);
	env.write("commodities.json", kCommodityCfg);
	env.write("actpolicy.json", kPolicyCfg);
	env.write("executers.json", kExecutersCfg);

	WtExecRunner runner;
	ASSERT_TRUE(runner.init("./r115_no_such_logcfg.json", true));

	// L61-67：dylib 存在（bin/libWtExeFact.dylib）但不导出 createDataReader
	{
		WTSVariant* root = WTSVariant::createObject();
		WTSVariant* store = WTSVariant::createObject();
		store->append("module", "../WtExeFact");
		store->append("path", (env.dir() + "/store2").c_str());
		store->append("session", "R115OTHER");
		root->append("store", store);
		WtSimpDataMgr mgr;
		EXPECT_FALSE(mgr.init(root, &runner));
		EXPECT_EQ(mgr.reader(), nullptr);
		root->release();
	}

	std::string cfg;
	cfg += "{\"basefiles\":{\"session\":\"" + env.file("sessions.json") + "\",";
	cfg += "\"commodity\":\"" + env.file("commodities.json") + "\"}";
	cfg += ",\"bspolicy\":\"" + env.file("actpolicy.json") + "\"";
	cfg += ",\"executers\":\"" + env.file("executers.json") + "\"}";
	ASSERT_TRUE(runner.config(cfg.c_str(), false));

	// 基础数据面：get_bd_mgr/get_hot_mgr 的公开取用（.h L98/L153）
	EXPECT_NE(runner.get_bd_mgr(), nullptr);
	EXPECT_NE(runner.get_hot_mgr(), nullptr);
	EXPECT_NE(runner.get_comm_info("SHFE.rb.2405"), nullptr);
	// L440-447：品种存在 -> 返回其 session（R24 只走过品种不存在的 L444-445）
	EXPECT_NE(runner.get_sess_info("SHFE.rb.2405"), nullptr);

	// local 未配 trader（L260-264）、diff 未配 trader（L288-292）、
	// diff 配了不存在的 trader（L294-304）都只记日志，config 仍 true
	EXPECT_NO_THROW(runner.run());

	// L71-72：空串走默认 cfgexec.json；L82-85：run_exec 在空适配器集合上安全
	ScopedCwd cwd(env.dir());
	EXPECT_NO_THROW(config_exec("", true));
	EXPECT_NO_THROW(run_exec());
}
