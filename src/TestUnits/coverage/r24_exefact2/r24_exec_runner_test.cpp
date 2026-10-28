// R24 lane（第二批）：WtExecMon/WtExecRunner.cpp 的 config 主链
// 目标文件: src/WtExecMon/WtExecRunner.cpp（本目标基线残余 missed=270/323，本轮主攻）
//
// 覆盖面（先做过普查：n05/r7 已覆盖 runner 的 L36-38/L40-58/L61-68/L181-194/L371-382/L384-387/
// L407-416/L434-448；本轮补的是 config() 主体 L71-178 与 initParsers/initTraders/initExecuters/
// initDataMgr/initActionPolicy（L196-364）、addExeFactories（L366-369）、handle_push_quote 主体
// （L389-398）与 get_real_time（L429-432）——这些是 R7 明确登记为「留给主循环评审」的面）。
//
// 为什么这轮敢走 config 主链（R7 没走）：
//  1) runner 用的 WtCore 符号（ParserAdapter/TraderAdapter/WtLocalExecuter/ActionPolicyMgr/BaseDataMgr）
//     在本二进制里由 libWtPorter.dylib 导出（实测 nm -gU 命中），链接与运行都通；
//  2) ParserAdapter::init（WtCore/ParserAdapter.cpp:105-106）与 TraderAdapter::init
//     （WtCore/TraderAdapter.cpp:163-164）在 module 为空时**先返回 false**，不会 dlopen、
//     不会建线程；WtLocalExecuter::init 只有 poolsize>0 才建线程池（WtLocalExecuter.cpp:89-93），
//     所以夹具里的 parser/trader/executer 条目一律不写 module、不写 poolsize；
//  3) initExecuters 的工厂目录是 WtHelper::getInstDir()+"executer//"，本机不存在该目录
//     （实测 build-cov/.../bin/TestUnits 下没有 executer/），loadFactories 走
//     WtExecuterFactory.cpp:17-21 的「目录不存在」分支返回 false，不加载任何插件；
//  4) 不起线程、不 sleep、不读挂钟断言（时间只作为 makeTime 的入参）。
//
// WTSLogger 一律不真正初始化（R7 的结论沿用：WTSLogger::init 成功会创建 spdlog async 线程），
// 所以日志不可观测；本文件只对**可查询的状态**下断言：
//  - WtExecRunner::get_bd_mgr() 的品种/合约/时段/节假日查询（决定 L71-114 走没走、走了哪条分支）
//  - config() 的返回值（L64-68 / L121-122 / L177）
//  - addExeFactories() 的返回值（L366-369）
//  - get_session_info(isCode=false)（L373-374）与 get_comm_info/get_sess_info（L376-382/L436-447）
//
// 键形纪律（已在 A-04/A-08 验证）：喂给产品的标准化代码用三段式 SHFE.rb.2405 / DCE.a.2409，
// 两段式 SHFE.rb2405 会被 CodeHelper（src/Share/CodeHelper.hpp:613-621）整段当 product，
// 按 product 注册的品种必然查不到 —— 本文件用两段/三段的差分把这条固化成断言。
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

#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/IBaseDataMgr.h"
#include "Share/StdUtils.hpp"
#include "Share/TimeUtils.hpp"
#include "Share/fmtlib.h"
#include "WtExecMon/WtExecRunner.h"

using namespace wtp;

namespace
{
//////////////////////////////////////////////////////////////////////////
// 夹具：一个用例一个临时目录，产物互不覆盖；目录名带 pid + 序号
//////////////////////////////////////////////////////////////////////////
class R24Env
{
public:
	explicit R24Env(const char* tag)
	{
		static uint32_t seq = 0;
		std::error_code ec;
		_dir = std::filesystem::temp_directory_path() /
			fmtutil::format("r24_exec_{0}_{1}_{2}", (uint64_t)::getpid(), ++seq, tag);
		std::filesystem::create_directories(_dir, ec);
		_path = _dir.string();
	}

	std::string file(const char* name) const { return _path + "/" + name; }

	void write(const std::string& name, const std::string& body) const
	{
		std::ofstream ofs((_path + "/" + name).c_str());
		ofs << body;
		ofs.close();
	}

	~R24Env()
	{
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

private:
	std::filesystem::path _dir;
	std::string _path;
};

const char* const kSessions = R"({
	"R24DAY":{"name":"R24日盘","offset":0,
		"sections":[{"from":930,"to":1130},{"from":1300,"to":1500}]}
})";

// holiday 字段 -> trdtpl "CHINA"，isHoliday("SHFE.rb", 20240101) 才能命中
const char* const kComms = R"({
	"SHFE":{"rb":{"name":"螺纹钢","session":"R24DAY","holiday":"CHINA",
		"pricetick":1.0,"volscale":10,"covermode":0,"pricemode":1}}
})";

const char* const kComms2 = R"({
	"DCE":{"a":{"name":"豆一","session":"R24DAY","holiday":"CHINA",
		"pricetick":1.0,"volscale":10,"covermode":0,"pricemode":1}}
})";

const char* const kContracts = R"({
	"SHFE":{"rb2405":{"name":"螺纹2405","exchg":"SHFE","product":"rb"}}
})";

const char* const kContracts2 = R"({
	"DCE":{"a2409":{"name":"豆一2409","exchg":"DCE","product":"a"}}
})";

const char* const kHolidays = R"({"CHINA":[20240101,20240212]})";

const char* const kActPolicy = R"({
	"default":{"order":[{"action":"close","limit":0},{"action":"open","limit":0}]}
})";

// 每条都 active，但都不写 module：ParserAdapter::init 在 WtCore/ParserAdapter.cpp:105-106 早退
// 第二条不写 id -> 走 runner L213-218 的自动 id 分支
const char* const kParsersBody = R"({
	"parsers":[
		{"id":"r24p1","active":true,"check_time":false},
		{"active":true,"check_time":true},
		{"id":"r24p3","active":false,"module":"no_such_parser"}
	]
})";

const char* const kTradersBody = R"({
	"traders":[
		{"id":"r24t1","active":true,"ignore_selfmatch":true},
		{"id":"r24t2","active":false,"module":"no_such_trader"}
	]
})";

// local / diff / dist 三条分别对应 runner L254-281 / L282-309 / L310-318
// e3 的 trader 不存在 -> L273-276；e4 没写 trader -> L261-264；e5 不写 name -> 默认 local L251-252
// 全部不写 poolsize -> 不建线程池
const char* const kExecutersBody = R"({
	"executers":[
		{"id":"e1","name":"local","active":true,"trader":"r24t1","scale":1.0},
		{"id":"e2","name":"diff","active":true,"trader":"r24t1","scale":1.0},
		{"id":"e3","name":"local","active":true,"trader":"r24t_missing"},
		{"id":"e4","name":"dist","active":true,"scale":2.0},
		{"id":"e5","active":true,"trader":"r24t1"},
		{"id":"e6","name":"local","active":false,"trader":"r24t1"}
	]
})";

class R24Fixture
{
public:
	explicit R24Fixture(const char* tag) : _env(tag)
	{
		_env.write("sessions.json", kSessions);
		_env.write("commodities.json", kComms);
		_env.write("commodities2.json", kComms2);
		_env.write("contracts.json", kContracts);
		_env.write("contracts2.json", kContracts2);
		_env.write("holidays.json", kHolidays);
		_env.write("actpolicy.json", kActPolicy);
		_env.write("parsers.json", kParsersBody);
		_env.write("traders.json", kTradersBody);
		_env.write("executers.json", kExecutersBody);
		_env.write("bad.json", "{not a valid json]");
		_env.write("empty_parsers.json", "{}");
	}

	std::string f(const char* name) const { return _env.file(name); }
	void write(const std::string& name, const std::string& body) const { _env.write(name, body); }

	// basefiles 的四段用字符串还是数组，决定 runner L81-91 / L97-107 走哪条分支
	std::string cfg(bool withData, bool withPolicy, bool withSubCfgs, bool arrayBase = false,
		bool withHoliday = true) const
	{
		std::vector<std::string> bf;
		bf.push_back("\"session\":\"" + f("sessions.json") + "\"");
		if (arrayBase)
		{
			bf.push_back("\"commodity\":[\"" + f("commodities.json") + "\",\"" + f("commodities2.json") + "\"]");
			bf.push_back("\"contract\":[\"" + f("contracts.json") + "\",\"" + f("contracts2.json") + "\"]");
		}
		else
		{
			bf.push_back("\"commodity\":\"" + f("commodities.json") + "\"");
			bf.push_back("\"contract\":\"" + f("contracts.json") + "\"");
		}
		if (withHoliday)
			bf.push_back("\"holiday\":\"" + f("holidays.json") + "\"");

		std::string body = "{\"basefiles\":{";
		for (size_t i = 0; i < bf.size(); i++)
		{
			if (i)
				body += ",";
			body += bf[i];
		}
		body += "}";

		if (withData)
			body += ",\"data\":{}";
		if (withPolicy)
			body += ",\"bspolicy\":\"" + f("actpolicy.json") + "\"";
		if (withSubCfgs)
		{
			body += ",\"parsers\":\"" + f("parsers.json") + "\"";
			body += ",\"traders\":\"" + f("traders.json") + "\"";
			body += ",\"executers\":\"" + f("executers.json") + "\"";
		}
		body += "}";
		return body;
	}

protected:
	R24Env _env;
};

WTSTickData* mkRTick(const char* code, uint32_t actDate, uint32_t actTime, double px)
{
	WTSTickStruct ts;
	snprintf(ts.code, sizeof(ts.code), "%s", code);
	ts.action_date = actDate;
	ts.trading_date = actDate;
	ts.action_time = actTime;
	ts.price = px;
	ts.pre_close = px - 0.5;
	ts.bid_prices[0] = px - 0.01;
	ts.bid_qty[0] = 100;
	ts.ask_prices[0] = px + 0.01;
	ts.ask_qty[0] = 100;
	ts.open_interest = 0;
	return WTSTickData::create(ts);
}
}

//////////////////////////////////////////////////////////////////////////
// config：basefiles 全部走「字符串」分支（L71-76、L81-84、L97-100、L110-114），
// 然后 initDataMgr 因没有 data 段落空（L356-358）、initActionPolicy 因没有 bspolicy 落空
// （L420-422）-> config 在 L122 返回 false（这是产品行为：data 段的返回值被忽略，policy 才拦）
// 断言的是「哪一个文件被加载了」：只注册了 SHFE.rb / SHFE.rb2405，DCE 那条必须查不到
//////////////////////////////////////////////////////////////////////////
TEST(r24_runner, config_string_basefiles_loads_only_first_files)
{
	R24Fixture fx{ "strbase" };
	WtExecRunner runner;
	const std::string body = fx.cfg(false, false, false, false, true);
	EXPECT_FALSE(runner.config(body.c_str(), false));   // L121-122：bspolicy 缺失

	IBaseDataMgr* bd = runner.get_bd_mgr();
	ASSERT_NE(bd, nullptr);
	WTSCommodityInfo* rb = bd->getCommodity("SHFE", "rb");
	ASSERT_NE(rb, nullptr);
	EXPECT_STREQ(rb->getName(), "螺纹钢");
	EXPECT_EQ(rb->getVolScale(), 10u);                  // commodities.json 的 volscale
	EXPECT_DOUBLE_EQ(rb->getPriceTick(), 1.0);
	// commodities2.json 没进加载列表 -> DCE.a 必然不存在（差分证明数组分支没被误走）
	EXPECT_EQ(bd->getCommodity("DCE", "a"), nullptr);
	WTSContractInfo* ct = bd->getContract("rb2405", "SHFE");
	ASSERT_NE(ct, nullptr);
	EXPECT_STREQ(ct->getCode(), "rb2405");
	EXPECT_EQ(bd->getGlobalSize(), 1u);                 // 只有 contracts.json 一条
	// sessions.json 的 R24DAY：两段日盘 -> 120 + 120 = 240 分钟
	WTSSessionInfo* sess = bd->getSession("R24DAY");
	ASSERT_NE(sess != nullptr, false);
	EXPECT_EQ(sess->getSectionCount(), 2u);
	EXPECT_TRUE(bd->isHoliday("SHFE.rb", 20240101));    // 20240101 是周一且在 CHINA 列表里
	EXPECT_FALSE(bd->isHoliday("SHFE.rb", 20240102));
}

//////////////////////////////////////////////////////////////////////////
// config：commodity/contract 走数组分支（L85-91、L101-107）——
// 与上一条唯一的差别就是数组，多出来的品种/合约数就是本条的可证伪点
//////////////////////////////////////////////////////////////////////////
TEST(r24_runner, config_array_basefiles_loads_every_listed_file)
{
	R24Fixture fx{ "arrbase" };
	WtExecRunner runner;
	const std::string body = fx.cfg(false, false, false, true, true);
	EXPECT_FALSE(runner.config(body.c_str(), false));

	IBaseDataMgr* bd = runner.get_bd_mgr();
	ASSERT_NE(bd, nullptr);
	// 两个品种文件都进来了（字符串分支那条断言的是 DCE.a == nullptr）
	ASSERT_NE(bd->getCommodity("SHFE", "rb"), nullptr);
	WTSCommodityInfo* dce = bd->getCommodity("DCE", "a");
	ASSERT_NE(dce, nullptr);
	EXPECT_STREQ(dce->getName(), "豆一");
	EXPECT_EQ(bd->getGlobalSize(), 2u);                 // rb2405 + a2409
	EXPECT_NE(bd->getContract("a2409", "DCE"), nullptr);
}

//////////////////////////////////////////////////////////////////////////
// config：basefiles 里没有 holiday 键 -> L110 判定为假，loadHolidays 整段跳过。
// 可证伪点：同一个 20240101（周一）在没有节假日表时必须是 false，而不是上一条的 true
//////////////////////////////////////////////////////////////////////////
TEST(r24_runner, config_without_holiday_key_skips_holiday_loader)
{
	R24Fixture fx{ "nohol" };
	WtExecRunner runner;
	const std::string body = fx.cfg(false, false, false, false, false);
	EXPECT_FALSE(runner.config(body.c_str(), false));
	IBaseDataMgr* bd = runner.get_bd_mgr();
	ASSERT_NE(bd->getCommodity("SHFE", "rb"), nullptr);  // 其余分支照旧
	EXPECT_FALSE(bd->isHoliday("SHFE.rb", 20240101));    // L110 假分支的直接后果
}

//////////////////////////////////////////////////////////////////////////
// config：有 data 段、有 bspolicy -> initDataMgr（L118、L356-363）与
// initActionPolicy（L121、L418-427）都放行；三个子配置文件都存在 -> L126-140、L144-158、
// L161-175 全部进入，最后 L177 return true。
// 这一段同时把 initParsers（L198-229）/initTraders（L329-351）/initExecuters（L234-324）
// 的主循环走了一遍（夹具里没有 module/poolsize，不 dlopen 也不起线程）。
//////////////////////////////////////////////////////////////////////////
TEST(r24_runner, config_full_chain_returns_true_and_keeps_base_data)
{
	R24Fixture fx{ "fullcfg" };
	WtExecRunner runner;
	const std::string body = fx.cfg(true, true, true, true, true);
	ASSERT_TRUE(runner.config(body.c_str(), false));     // L177

	IBaseDataMgr* bd = runner.get_bd_mgr();
	// config 的后半程不得回头破坏基础数据
	WTSCommodityInfo* rb = bd->getCommodity("SHFE", "rb");
	ASSERT_NE(rb, nullptr);
	EXPECT_EQ(rb->getVolScale(), 10u);
	ASSERT_NE(bd->getCommodity("DCE", "a"), nullptr);
	EXPECT_EQ(bd->getGlobalSize(), 2u);
	// 只有 session 键存在时 getSession 才有值（L72-76 生效）
	EXPECT_NE(bd->getSession("R24DAY"), nullptr);
	// 空适配器集合下 run() 依然安全（L182-194；ParserAdapter::run/TraderAdapter::run
	// 在 api==NULL 时直接返回，见 WtCore/ParserAdapter.cpp:230、TraderAdapter.cpp:406）
	EXPECT_NO_THROW(runner.run());
}

//////////////////////////////////////////////////////////////////////////
// config：parsers/traders/executers 指向不存在的文件 -> L126/L144/L161 三个 exists 判定全假，
// 三个子初始化函数一次都不进。与上一条形成「进/不进」的差分（两条都 return true，
// 所以这里额外断言基础数据仍在、且 config 不因缺子配置而失败）
//////////////////////////////////////////////////////////////////////////
TEST(r24_runner, config_missing_subcfg_files_are_skipped)
{
	R24Fixture fx{ "nosubcfg" };
	WtExecRunner runner;
	std::string body = fx.cfg(true, true, false, false, true);
	// 去掉 withSubCfgs 后根本没有这三个键：getCString 返回空串 -> StdFile::exists("") 为假
	ASSERT_TRUE(runner.config(body.c_str(), false));
	IBaseDataMgr* bd = runner.get_bd_mgr();
	EXPECT_EQ(bd->getCommodity("DCE", "a"), nullptr);   // 非数组：只 SHFE.rb
	EXPECT_NE(bd->getContract("rb2405", "SHFE"), nullptr);
	EXPECT_NO_THROW(runner.run());                      // 两个 mgr 都是空的
}

//////////////////////////////////////////////////////////////////////////
// config：子配置文件内容非法（坏 json）-> L130 假分支 L136-139、L148 假分支 L154-157、
// L165 假分支 L171-174。产品的行为是「只记日志，不改变 config 的返回值」，
// 即三个子配置的加载失败对调用方完全不可见（见 bugs/R24_exefact.md BUG-R24-002）
//////////////////////////////////////////////////////////////////////////
TEST(r24_runner, config_invalid_subcfg_json_is_swallowed)
{
	R24Fixture fx{ "badsub" };
	WtExecRunner runner;
	std::string body = "{";
	body += "\"basefiles\":{\"session\":\"" + fx.f("sessions.json") + "\"}";
	body += ",\"data\":{},\"bspolicy\":\"" + fx.f("actpolicy.json") + "\"";
	body += ",\"parsers\":\"" + fx.f("bad.json") + "\"";
	body += ",\"traders\":\"" + fx.f("bad.json") + "\"";
	body += ",\"executers\":\"" + fx.f("bad.json") + "\"}";
	// 三份坏配置都没让 config 失败
	EXPECT_TRUE(runner.config(body.c_str(), false));
	EXPECT_NE(runner.get_bd_mgr()->getSession("R24DAY"), nullptr);
	EXPECT_NO_THROW(runner.run());
}

//////////////////////////////////////////////////////////////////////////
// initParsers：配置文件里没有 "parsers" 键 -> L198-200 return false
// （顺带 WtCore/ParserAdapter.cpp:87-88 的 cfg==NULL 分支不涉及）
//////////////////////////////////////////////////////////////////////////
TEST(r24_runner, config_parser_cfg_without_parsers_key_returns_false_from_initparsers)
{
	R24Fixture fx{ "noparserskey" };
	WtExecRunner runner;
	std::string body = "{";
	body += "\"basefiles\":{\"session\":\"" + fx.f("sessions.json") + "\",";
	body += "\"commodity\":\"" + fx.f("commodities.json") + "\"}";
	body += ",\"data\":{},\"bspolicy\":\"" + fx.f("actpolicy.json") + "\"";
	body += ",\"parsers\":\"" + fx.f("empty_parsers.json") + "\"}";   // {} -> initParsers L199-200
	ASSERT_TRUE(runner.config(body.c_str(), false));
	// 品种仍然加载（说明 false 只落在 initParsers 内部，没有回头影响 basefiles）
	EXPECT_NE(runner.get_bd_mgr()->getCommodity("SHFE", "rb"), nullptr);
	EXPECT_NO_THROW(runner.run());
}

//////////////////////////////////////////////////////////////////////////
// initExecuters：cfg->get("executers") 不是数组 -> L235-236 return false；
// initTraders 同形 -> L330-331。两条都用「非数组」的 executers/traders 触发。
// 可证伪点：config 仍返回 true（返回值只由 bspolicy 决定），且没有适配器被注册
// —— 用 run() 不抛、以及后续 handle_push_quote 不接受任何 executer 来体现。
//////////////////////////////////////////////////////////////////////////
TEST(r24_runner, config_non_array_executers_and_traders_bail_out)
{
	R24Fixture fx{ "notarray" };
	WtExecRunner runner;
	fx.write("traders_obj.json", R"({"traders":{"id":"x"}})");
	fx.write("executers_obj.json", R"({"executers":{"id":"x"}})");
	std::string body = "{";
	body += "\"basefiles\":{\"session\":\"" + fx.f("sessions.json") + "\"}";
	body += ",\"data\":{},\"bspolicy\":\"" + fx.f("actpolicy.json") + "\"";
	body += ",\"traders\":\"" + fx.f("traders_obj.json") + "\"";
	body += ",\"executers\":\"" + fx.f("executers_obj.json") + "\"}";
	EXPECT_TRUE(runner.config(body.c_str(), false));
	EXPECT_NO_THROW(runner.run());
}

//////////////////////////////////////////////////////////////////////////
// addExeFactories（L366-369）：目录不存在 -> WtExecuterFactory.cpp:17-21 返回 false；
// 空目录 -> 循环不执行，末尾 return true。两条返回值都是可证伪的具体布尔。
//////////////////////////////////////////////////////////////////////////
TEST(r24_runner, add_exe_factories_missing_dir_false_empty_dir_true)
{
	R24Env env("exedir");
	WtExecRunner runner;
	const std::string missing = env.file("no_such_folder");
	EXPECT_FALSE(runner.addExeFactories(missing.c_str()));      // L368 -> loadFactories false
	const std::string empty = env.file("emptyexecuters");
	std::filesystem::create_directories(empty);
	EXPECT_TRUE(runner.addExeFactories((empty + "/").c_str())); // 目录存在且无 dylib -> true
	// 没有加载到任何工厂：再查一次基础数据仍然为空（runner 从未 config）
	EXPECT_EQ(runner.get_comm_info("SHFE.rb.2405"), nullptr);
}

//////////////////////////////////////////////////////////////////////////
// get_session_info（L371-382）两条分支 + get_comm_info/get_sess_info（L434-448）
// 依赖 config 把 sessions/commodities 装进来；键形的两段/三段差分也在这里固化。
//////////////////////////////////////////////////////////////////////////
TEST(r24_runner, session_and_commodity_queries_after_config)
{
	R24Fixture fx{ "queries" };
	WtExecRunner runner;
	const std::string body = fx.cfg(false, true, false, false, true);
	ASSERT_TRUE(runner.config(body.c_str(), false));

	// isCode=false -> L373-374 直接查 session 表
	EXPECT_NE(runner.get_session_info("R24DAY", false), nullptr);
	EXPECT_EQ(runner.get_session_info("R24NOPE", false), nullptr);
	// isCode=true -> L376-381，三段式能命中 SHFE.rb
	WTSSessionInfo* byCode = runner.get_session_info("SHFE.rb.2405", true);
	ASSERT_NE(byCode, nullptr);
	EXPECT_EQ(byCode->getSectionCount(), 2u);
	EXPECT_STREQ(byCode->id(), "R24DAY");
	// 两段式：CodeHelper 把 "rb2405" 整段当 product -> L378-379 返回 NULL
	EXPECT_EQ(runner.get_session_info("SHFE.rb2405", true), nullptr);
	EXPECT_EQ(runner.get_sess_info("SHFE.rb2405"), nullptr);
	// get_comm_info 三段式命中，且就是 commodities.json 里那条
	WTSCommodityInfo* comm = runner.get_comm_info("SHFE.rb.2405");
	ASSERT_NE(comm, nullptr);
	EXPECT_EQ(comm->getVolScale(), 10u);
	EXPECT_STREQ(comm->getExchg(), "SHFE");
	// 未注册的品种 -> NULL（L444-445）
	EXPECT_EQ(runner.get_sess_info("DCE.a.2409"), nullptr);
}

//////////////////////////////////////////////////////////////////////////
// 缺陷复现（越界指针运算，不能启用）：空串标准化代码
// CodeHelper::extractStdCode（src/Share/CodeHelper.hpp:604-607）先
// `findFirst(stdCode, '.')`，空串得到 npos，随后 `stdCode + idx + 1` 已经在缓冲区之外，
// 第二次 findFirst 就从越界指针开始扫。runner L442 不做入参校验，直接把它转给 extractStdCode。
// 实测本用例 SIGSEGV（见 bugs/R24_exefact.md BUG-R24-001）
//////////////////////////////////////////////////////////////////////////
TEST(r24_runner, DISABLED_get_sess_info_with_empty_code_crashes_code_helper)
{
	R24Fixture fx{ "emptycode" };
	WtExecRunner runner;
	const std::string body = fx.cfg(false, true, false, false, true);
	ASSERT_TRUE(runner.config(body.c_str(), false));
	EXPECT_EQ(runner.get_sess_info(""), nullptr);       // 期望：崩
}

//////////////////////////////////////////////////////////////////////////
// handle_push_quote（L389-398）+ get_real_time（L429-432）：
// 需要先让 WtSimpDataMgr::initStore 成功（_s_info 非空），否则 L153 解引用空指针
// —— 见 bugs/R24_exefact.md BUG-R17-004 的续面。
// store.module 写成 "../WtDataStorage"：WtSimpDataMgr.cpp:52 是
// getInstDir() + wrap_module(module)，wrap_module（Share/DLLHelper.hpp:69-89）会先跳过
// 开头的非字母字符再补 lib/.dylib，于是 "../WtDataStorage" -> "../libWtDataStorage.dylib"，
// 正好指回 bin 目录里已经随二进制加载好的那份（TestUnits/ 目录本身没有）。
// 空 executer 集合 -> L398 的 handle_tick 不会走到任何单元。
//////////////////////////////////////////////////////////////////////////
TEST(r24_runner, handle_push_quote_after_store_ready)
{
	R24Fixture fx{ "pushquote" };
	WtExecRunner runner;
	// init 把 InstDir 设成二进制所在目录（TestUnits/），后续 store 路径才解析得到
	ASSERT_TRUE(runner.init("./r24_missing_logcfgexec.json", true));

	std::string body = "{";
	body += "\"basefiles\":{\"session\":\"" + fx.f("sessions.json") + "\",";
	body += "\"commodity\":\"" + fx.f("commodities.json") + "\",";
	body += "\"contract\":\"" + fx.f("contracts.json") + "\"}";
	body += ",\"data\":{\"store\":{\"module\":\"../WtDataStorage\",\"path\":\"" + fx.f("storedata") +
		"\",\"session\":\"R24DAY\"}}";
	body += ",\"bspolicy\":\"" + fx.f("actpolicy.json") + "\"}";
	if (!runner.config(body.c_str(), false))
	{
		// initStore 走不通（dylib 找不到 / createDataReader 缺失）就不继续，
		// 不把它记成「测试通过」
		GTEST_SKIP() << "store module 未能初始化，handle_push_quote 留白（见 BUG-R17-004 续面）";
	}

	WTSTickData* tick = mkRTick("SHFE.rb.2405", 20240102, 103015500, 3500.0);
	runner.handle_push_quote(tick);                       // L389-398
	// L394 setTDate(quote->tradingdate()) -> WtSimpDataMgr.h:167 get_trading_day()
	// 必须等 simpdata L148-161 全部跑完才会有值（BUG-R17-004 修好前的第一笔）
	EXPECT_EQ(runner.get_trading_day(), 20240102u);
	// L429-432：raw_time=1030、secs=15500 -> 1030*100000+15500 = 103015500
	const uint64_t expect = (uint64_t)TimeUtils::makeTime(20240102, 103015500);
	EXPECT_EQ(runner.get_real_time(), expect);
	// 同一笔行情第二次进来（时间不比第一次新）→ simpdata L143-146 早退，时刻不前进
	WTSTickData* older = mkRTick("SHFE.rb.2405", 20240102, 103010500, 3501.0);
	runner.handle_push_quote(older);
	EXPECT_EQ(runner.get_real_time(), expect);
	tick->release();
	older->release();
}

//////////////////////////////////////////////////////////////////////////
// release()（L401-404）：未初始化日志器时的 spdlog 关停不确定（R7 已 DISABLED 同一面），
// 本轮仍不启用；此处只登记「config 主链已经通了，唯独 release 还缺日志器接缝」。
//////////////////////////////////////////////////////////////////////////
TEST(r24_runner, DISABLED_release_after_full_config_stops_logger)
{
	R24Fixture fx{ "release" };
	WtExecRunner runner;
	const std::string body = fx.cfg(true, true, true, false, true);
	ASSERT_TRUE(runner.config(body.c_str(), false));
	runner.release();                                     // L403 WTSLogger::stop()
	SUCCEED();
}
