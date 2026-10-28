/*!
 * \file r78_dtservo_plugin_and_guards_test.cpp
 * \brief 第十五波 R78: WtDtServo/ParserAdapter 的 init 成功链与 WtDataManager/WtDtRunner 守卫补测
 *
 * 目标:
 *  - ParserAdapter::init（WtDtServo 版）的模块装载成功链（ParserAdapter.cpp:82-203），
 *    filter 非空/为空两支；模块用构建产物 libParserUDP.dylib，只调 init 不调 run，
 *    ParserUDP::init 只构造 asio endpoint（ParserUDP.cpp:98-113），不建 socket/线程
 *  - WtDtRunner 的日线周期解析与委托（WtDtRunner.cpp:311-348）、get_bars_by_date
 *    拒绝非分钟周期（:270-274）
 *  - WtDataManager::get_session_info 的未知合约早退（:145-156）与缓存首次装配
 *    遇到空库提前返回（:158-243），经 libWtDataStorageAD（init 不建线程）驱动
 *
 * 接缝: AD reader 的区间读在空库上会越界（BUG-R45-003），本文件只调用其
 * readTickSliceByDate / readKlineSliceByCount（均为 TODO 直接 NULL）以及
 * level2 的三个直接 NULL 接口，不碰 range 读。
 */
#include <gtest/gtest.h>

#include <WtDtServo/ParserAdapter.h>
#include <WtDtServo/WtDataManager.h>
#include <WtDtServo/WtDtRunner.h>
#include <WtDtServo/WtHelper.h>

#include <WTSTools/WTSBaseDataMgr.h>
#include <WTSUtils/WTSCfgLoader.h>

#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSVariant.hpp>

#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

using namespace wtp;

namespace {

fs::path makeTempDir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string("r78_") + tag + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	std::error_code ec;
	fs::remove_all(p, ec);
	fs::create_directories(p, ec);
	return p;
}

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

bool linkUdpParser(const fs::path& tmp, std::string& moduleRoot)
{
	const std::string bin = binDir();
	if (bin.empty())
		return false;
	const std::string src = bin + "libParserUDP.dylib";
	std::error_code ec;
	if (!fs::exists(src, ec))
		return false;

	void* h = dlopen(src.c_str(), RTLD_NOW);
	if (h == nullptr)
		return false;
	const bool symbols = dlsym(h, "createParser") != nullptr
		&& dlsym(h, "deleteParser") != nullptr;
	dlclose(h);
	if (!symbols)
		return false;

	const fs::path root = tmp / "modroot";
	fs::create_directories(root / "parsers", ec);
	if (ec)
		return false;
	fs::create_symlink(src, root / "parsers" / "libParserUDP.dylib", ec);
	if (ec)
		return false;

	moduleRoot = root.string() + "/";
	return true;
}

bool adStoreReady(const std::string& bin)
{
	if (bin.empty())
		return false;
	const std::string path = bin + "libWtDataStorageAD.dylib";
	std::error_code ec;
	if (!fs::exists(path, ec))
		return false;

	void* h = dlopen(path.c_str(), RTLD_NOW);
	if (h == nullptr)
		return false;
	const bool ok = dlsym(h, "createRdmDtReader") != nullptr
		&& dlsym(h, "deleteRdmDtReader") != nullptr;
	dlclose(h);
	return ok;
}

void writeFile(const fs::path& p, const std::string& content)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

const char* kSessionJson = R"JSON({
	"SD0930":{"name":"r78 day","offset":0,
		"sections":[{"from":930,"to":1130},{"from":1300,"to":1500}]}
})JSON";

const char* kCommodityJson = R"JSON({
	"CFFEX":{"IC":{"covermode":0,"pricemode":0,"category":1,"precision":1,
		"pricetick":0.2,"volscale":200,"name":"CSI500","exchg":"CFFEX",
		"session":"SD0930","holiday":"CHINA"}}
})JSON";

const char* kContractJson = R"JSON({
	"CFFEX":{"IC2204":{"name":"CSI500 2204","code":"IC2204","exchg":"CFFEX",
		"product":"IC","maxlimitqty":20,"maxmarketqty":10,"altcode":"IC2204"}}
})JSON";

struct DtFixture
{
	fs::path _dir;
	std::string _bin;
	std::string _udpRoot;
	std::string _logCfg;

	DtFixture()
	{
		_dir = makeTempDir("dtservo");
		_bin = binDir();
		writeFile(_dir / "sessions.json", kSessionJson);
		writeFile(_dir / "commodities.json", kCommodityJson);
		writeFile(_dir / "contracts.json", kContractJson);
		_logCfg = (_dir / "no_such_logcfg.yaml").string();
	}

	~DtFixture()
	{
		WtHelper::set_module_dir("");
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	DtFixture(const DtFixture&) = delete;
	DtFixture& operator=(const DtFixture&) = delete;

	std::string path(const char* name) const { return (_dir / name).string(); }

	std::string baseConfig(bool withStore) const
	{
		const std::string d = _dir.string();
		std::string s = "{\"disable_dump\":true,\"basefiles\":{";
		s += "\"session\":\"" + d + "/sessions.json\",";
		s += "\"commodity\":\"" + d + "/commodities.json\",";
		s += "\"contract\":\"" + d + "/contracts.json\"}";
		if (withStore)
		{
			s += ",\"data\":{\"align_by_section\":false,\"store\":{\"module\":\"WtDataStorageAD\",\"path\":\""
				+ d + "/store/\"}}";
		}
		s += "}";
		return s;
	}

	WTSVariant* loadDataCfg()
	{
		const std::string p = path("datacfg.json");
		writeFile(p, "{\"align_by_section\":false,\"store\":{\"module\":\"WtDataStorageAD\",\"path\":\""
			+ _dir.string() + "/dmstore/\"}}");
		return WTSCfgLoader::load_from_file(p.c_str());
	}
};

void initBaseData(WTSBaseDataMgr& bd, const DtFixture& fx)
{
	ASSERT_TRUE(bd.loadSessions(fx.path("sessions.json").c_str()));
	ASSERT_TRUE(bd.loadCommodities(fx.path("commodities.json").c_str()));
	ASSERT_TRUE(bd.loadContracts(fx.path("contracts.json").c_str()));
}

}	// namespace

// ParserAdapter.cpp:82-203 模块装载成功 + filter 非空 -> 六种键匹配循环
TEST(R78DtParserInit, loads_udp_plugin_with_filter)
{
	DtFixture fx;
	if (!linkUdpParser(fx._dir, fx._udpRoot))
		GTEST_SKIP() << "libParserUDP.dylib not found in this build";
	WtHelper::set_module_dir(fx._udpRoot.c_str());

	WTSBaseDataMgr bd;
	initBaseData(bd, fx);

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "ParserUDP");
	cfg->append("filter", "CFFEX,IC2204");
	cfg->append("host", "127.0.0.1");
	cfg->append("bport", (uint32_t)17811);
	cfg->append("sport", (uint32_t)17812);

	{
		ParserAdapter pa(&bd, nullptr);
		EXPECT_TRUE(pa.init("r78dtparser1", cfg));
		EXPECT_STREQ("r78dtparser1", pa.id());
		EXPECT_EQ(static_cast<IBaseDataMgr*>(&bd), pa.getBaseDataMgr());
		// 只验证装载；run() 会 connect UDP，不调用
		pa.release();
	}

	cfg->release();
}

// ParserAdapter.cpp:176-187 filter 为空 -> 全量合约订阅分支
TEST(R78DtParserInit, loads_udp_plugin_without_filter)
{
	DtFixture fx;
	if (!linkUdpParser(fx._dir, fx._udpRoot))
		GTEST_SKIP() << "libParserUDP.dylib not found in this build";
	WtHelper::set_module_dir(fx._udpRoot.c_str());

	WTSBaseDataMgr bd;
	initBaseData(bd, fx);

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "ParserUDP");
	cfg->append("filter", "");
	cfg->append("host", "127.0.0.1");
	cfg->append("bport", (uint32_t)17813);
	cfg->append("sport", (uint32_t)17814);

	{
		ParserAdapter pa(&bd, nullptr);
		EXPECT_TRUE(pa.init("r78dtparser2", cfg));
		EXPECT_STREQ("r78dtparser2", pa.id());
		pa.release();
	}

	cfg->release();
}

// WtDtRunner.cpp:311-348 日线周期映射 + 委托（times==1 -> WtDataManager.cpp:339-342）
// WtDtRunner.cpp:270-274 get_bars_by_date 对非分钟周期的拒绝分支
TEST(R78DtRunner, day_period_queries)
{
	DtFixture fx;
	if (!adStoreReady(fx._bin))
		GTEST_SKIP() << "libWtDataStorageAD.dylib / createRdmDtReader 不可用";
	WtHelper::set_module_dir(fx._bin.c_str());

	const std::string cfgFile = fx.path("wtdt.json");
	writeFile(cfgFile, fx.baseConfig(true));

	WtDtRunner runner;
	runner.initialize(cfgFile.c_str(), true, fx._bin.c_str(), fx._logCfg.c_str(),
		nullptr, nullptr);

	// "d1" -> KP_DAY，且 get_bars_by_date 只支持分钟周期
	EXPECT_EQ(nullptr, runner.get_bars_by_date("CFFEX.IC.2204", "d1", 20240102));
	// "d1" + times==1 -> 直接委托 AD readKlineSliceByCount（TODO 返回 NULL）
	EXPECT_EQ(nullptr, runner.get_bars_by_count("CFFEX.IC.2204", "d1", 5, 20240102));
	// 分钟重采样首次装配遇到空库 -> WtDataManager 提前返回
	EXPECT_EQ(nullptr, runner.get_bars_by_date("CFFEX.IC.2204", "m10", 20240102));
	EXPECT_EQ(nullptr, runner.get_bars_by_count("CFFEX.IC.2204", "m10", 5));
	EXPECT_EQ(nullptr, runner.get_ticks_by_date("CFFEX.IC.2204", 20240102));
	EXPECT_EQ(nullptr, runner.get_ticks_by_count("CFFEX.IC.2204", 10, 202401021500));
	EXPECT_EQ(nullptr, runner.get_sbars_by_date("CFFEX.IC.2204", 30, 20240102));
}

// WtDataManager.cpp:145-156 未知合约 -> getCommodity 为空 -> 返回 NULL
// WtDataManager.cpp:158-243 / :199-243 会话为空后缓存首次装配遇到空库返回 NULL
// WtDataManager.cpp:124-143 level2 三入口的直接 NULL 透传
TEST(R78DtDataManager, unknown_contract_guards)
{
	DtFixture fx;
	if (!adStoreReady(fx._bin))
		GTEST_SKIP() << "libWtDataStorageAD.dylib / createRdmDtReader 不可用";
	WtHelper::set_module_dir(fx._bin.c_str());

	const std::string cfgFile = fx.path("wtdt.json");
	writeFile(cfgFile, fx.baseConfig(true));

	WtDtRunner runner;
	runner.initialize(cfgFile.c_str(), true, fx._bin.c_str(), fx._logCfg.c_str(),
		nullptr, nullptr);

	WTSVariant* dataCfg = fx.loadDataCfg();
	ASSERT_NE(nullptr, dataCfg);

	WtDataManager dm;
	ASSERT_TRUE(dm.init(dataCfg, &runner));

	// 未知品种: get_session_info(code,true) 在 getCommodity 处早退
	EXPECT_EQ(nullptr, dm.get_skline_slice_by_date("CFFEX.XX.2404", 30, 20240102));
	// times>1 的 K 线缓存首次装配：session 为空 + AD byCount 为 TODO NULL
	EXPECT_EQ(nullptr, dm.get_kline_slice_by_range("CFFEX.XX.2404", KP_Minute1, 2,
		202401020930, 202401021500));
	// level2 三个入口在 AD reader 上都是直接 NULL
	EXPECT_EQ(nullptr, dm.get_order_queue_slice("CFFEX.XX.2404", 202401020930, 202401021500));
	EXPECT_EQ(nullptr, dm.get_order_detail_slice("CFFEX.XX.2404", 202401020930, 202401021500));
	EXPECT_EQ(nullptr, dm.get_transaction_slice("CFFEX.XX.2404", 202401020930, 202401021500));
	// 已初始化 reader 的清缓存分支
	EXPECT_NO_THROW(dm.clear_cache());

	dataCfg->release();
}
