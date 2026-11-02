/*!
 * \file r85_dtservo_runner_guards_test.cpp
 * \brief 迭代01 第十六波 R85 - WtDtRunner 周期解析/默认区间/订阅守卫
 *
 * 覆盖（只读生产代码）:
 *  - WtDtRunner.cpp:66-71 配置未写 disable_dump 时进入的（本平台为空的）分支
 *  - 显式 parsers sidecar 缺失/解析失败 -> 初始化失败且不启动；无 parsers 配置仍可选
 *  - WtDtRunner.cpp:218-239 get_bars_by_range："m7"（times 非 5 倍数）/ "d2"（非 m 开头）
 *    与 endTime==0 的默认区间
 *  - WtDtRunner.cpp:258-281 get_bars_by_date："m7" 与 uDate==0
 *  - WtDtRunner.cpp:359-364 get_ticks_by_count 的 endTime==0
 *  - WtDtRunner.cpp:617-635 sub_bar 的非分钟周期（"d1" -> KP_DAY）
 *
 * 数据读取器用构建产物 libWtDataStorageAD.dylib（init 只记录目录，不建线程）；
 * 全部查询都落在「空库」上，只走 byCount/区间缓存首次装配的空返回，不碰
 * BUG-R45-003 的空库区间读，也不初始化全局 getRunner()。
 */
#include <gtest/gtest.h>

#include <WtDtServo/WtDataManager.h>
#include <WtDtServo/WtDtRunner.h>
#include <WtDtServo/WtHelper.h>
#include <WtDtServo/ParserAdapter.h>
#include <Includes/WTSVariant.hpp>

#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace wtp;

namespace {

std::vector<WtDtRunner*>& retainedRunnerGraphs()
{
	static std::vector<WtDtRunner*>* runners = new std::vector<WtDtRunner*>();
	return *runners;
}

fs::path makeTempDir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string("r85_") + tag + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	std::error_code ec;
	fs::remove_all(p, ec);
	fs::create_directories(p, ec);
	return p;
}

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
	"SD0930":{"name":"r85 day","offset":0,
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

const char* kCode = "CFFEX.IC.2204";

class RunnerFixture : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = makeTempDir("dtrunner");
		_bin = binDir();
		if (!adStoreReady(_bin))
			GTEST_SKIP() << "libWtDataStorageAD.dylib / createRdmDtReader 不可用";
		writeFile(_dir / "sessions.json", kSessionJson);
		writeFile(_dir / "commodities.json", kCommodityJson);
		writeFile(_dir / "contracts.json", kContractJson);
		_logCfg = (_dir / "no_such_logcfg.yaml").string();
		WtHelper::set_module_dir(_bin.c_str());
	}

	void TearDown() override
	{
		WtHelper::set_module_dir("");
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	// extra: 附加到配置根上的额外 JSON 片段（以逗号开头，可为空）
	std::string config(bool disableDump, const std::string& extra) const
	{
		const std::string d = _dir.string();
		std::string s = "{";
		if (disableDump)
			s += "\"disable_dump\":true,";
		s += "\"basefiles\":{";
		s += "\"session\":\"" + d + "/sessions.json\",";
		s += "\"commodity\":\"" + d + "/commodities.json\",";
		s += "\"contract\":\"" + d + "/contracts.json\"}";
		s += ",\"data\":{\"align_by_section\":false,\"store\":{\"module\":\"WtDataStorageAD\",\"path\":\""
			+ d + "/store/\"}}";
		s += extra;
		s += "}";
		return s;
	}

	std::string writeConfig(const char* name, bool disableDump, const std::string& extra)
	{
		const std::string p = (_dir / name).string();
		writeFile(p, config(disableDump, extra));
		return p;
	}

	fs::path	_dir;
	std::string	_bin;
	std::string	_logCfg;
};

}	// namespace

TEST(R85ModulePath, EmptyAndTrailingSeparatorsKeepRelativePaths)
{
	WtHelper::set_module_dir("");
	EXPECT_EQ(WtHelper::get_module_path("libReader.dylib"), "libReader.dylib");
	EXPECT_EQ(WtHelper::get_module_path("parsers/libParser.dylib"), "parsers/libParser.dylib");
	WtHelper::set_module_dir("module_root");
	EXPECT_EQ(WtHelper::get_module_path("libReader.dylib"), "module_root/libReader.dylib");
	WtHelper::set_module_dir("module_root/");
	EXPECT_EQ(WtHelper::get_module_path("parsers/libParser.dylib"), "module_root/parsers/libParser.dylib");
	WtHelper::set_module_dir("");
}

TEST_F(RunnerFixture, StoreLoadsWithAndWithoutModuleDirectorySeparator)
{
	const fs::path moduleRoot = _dir / "module_root";
	fs::create_directories(moduleRoot);
	fs::copy_file(fs::path(_bin) / "libWtDataStorageAD.dylib",
		moduleRoot / "libWtDataStorageAD.dylib", fs::copy_options::overwrite_existing);
	const std::string cfgFile = writeConfig("wtdt_store_paths.json", true, "");
	for (const std::string& suffix : { std::string(), std::string("/") })
	{
		WtDtRunner runner;
		const std::string moduleDir = moduleRoot.string() + suffix;
		runner.initialize(cfgFile.c_str(), true, moduleDir.c_str(), _logCfg.c_str(), nullptr, nullptr);
		EXPECT_TRUE(runner.isInitialized()) << moduleDir;
		EXPECT_NE(nullptr, runner.getBaseDataMgr().getSession("SD0930")) << moduleDir;
	}
}

TEST_F(RunnerFixture, ParserLoadsWithAndWithoutModuleDirectorySeparator)
{
	const fs::path moduleRoot = _dir / "module_root";
	fs::create_directories(moduleRoot / "parsers");
	fs::copy_file(fs::path(_bin) / "libWtDataStorageAD.dylib",
		moduleRoot / "libWtDataStorageAD.dylib", fs::copy_options::overwrite_existing);
	fs::copy_file(fs::path(_bin) / "libParserUDP.dylib",
		moduleRoot / "parsers/libParserUDP.dylib", fs::copy_options::overwrite_existing);
	const std::string cfgFile = writeConfig("wtdt_parser_paths.json", true, "");
	WtDtRunner runner;
	runner.initialize(cfgFile.c_str(), true, moduleRoot.string().c_str(), _logCfg.c_str(), nullptr, nullptr);
	ASSERT_TRUE(runner.isInitialized());

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "ParserUDP");
	cfg->append("host", "127.0.0.1");
	for (const std::string& suffix : { std::string(), std::string("/") })
	{
		const std::string moduleDir = moduleRoot.string() + suffix;
		WtHelper::set_module_dir(moduleDir.c_str());
		ParserAdapter parser(&runner.getBaseDataMgr(), &runner);
		EXPECT_TRUE(parser.init("r85_parser_path", cfg)) << moduleDir;
		// init does not start ParserUDP's worker; release is safe before connect.
		parser.release();
	}
	cfg->release();
}

// WtDtRunner.cpp:152-175
TEST_F(RunnerFixture, parser_sidecar_errors_reject_initialization)
{
	// 显式 sidecar 指向一个存在但内容非法的文件，必须失败而不是继续启动。
	const std::string badParser = (_dir / "parsers_bad.json").string();
	writeFile(badParser, "this is not json");
	const std::string extra = ",\"parsers\":\"" + badParser + "\"";
	const std::string cfgFile = writeConfig("wtdt_broken.json", false, extra);

	WtDtRunner runner;
	runner.initialize(cfgFile.c_str(), true, _bin.c_str(), _logCfg.c_str(), nullptr, nullptr);

	EXPECT_FALSE(runner.isInitialized());
	EXPECT_EQ(runner.activeParserCount(), 0u);
	EXPECT_FALSE(runner.hasQuarantinedParser());
	EXPECT_FALSE(runner.hasRetainedParserAdapters());
	runner.start();
	EXPECT_FALSE(runner.isInitialized());
	EXPECT_EQ(runner.activeParserCount(), 0u);
	EXPECT_EQ(nullptr, runner.get_bars_by_count(kCode, "m7", 3, 202401021500));
}

TEST_F(RunnerFixture, missing_parser_sidecar_rejects_initialization)
{
	const std::string missingParser = (_dir / "no_such_parsers.json").string();
	const std::string extra = ",\"parsers\":\"" + missingParser + "\"";
	const std::string cfgFile = writeConfig("wtdt_missing_parser.json", true, extra);

	WtDtRunner runner;
	runner.initialize(cfgFile.c_str(), true, _bin.c_str(), _logCfg.c_str(), nullptr, nullptr);

	EXPECT_FALSE(runner.isInitialized());
	EXPECT_EQ(runner.activeParserCount(), 0u);
	EXPECT_FALSE(runner.hasQuarantinedParser());
	EXPECT_FALSE(runner.hasRetainedParserAdapters());
	runner.start();
	EXPECT_FALSE(runner.isInitialized());
	EXPECT_EQ(runner.activeParserCount(), 0u);
}

TEST_F(RunnerFixture, parser_config_shapes_are_rejected_before_manager_mutation)
{
	const fs::path missingMembers = _dir / "parsers_missing_members.json";
	const fs::path wrongShape = _dir / "parsers_wrong_shape.json";
	writeFile(missingMembers, R"JSON({})JSON");
	writeFile(wrongShape, R"JSON({"parsers":{}})JSON");

	const std::vector<std::pair<std::string, std::string>> cases = {
		{"wrong_top_level_type", ",\"parsers\":true"},
		{"missing_sidecar_parsers", ",\"parsers\":\"" + missingMembers.string() + "\""},
		{"non_array_sidecar_parsers", ",\"parsers\":\"" + wrongShape.string() + "\""},
		{"duplicate_parser_ids", ",\"parsers\":[{\"active\":true,\"id\":\"dup\"},"
			"{\"active\":true,\"id\":\"dup\"}]"}
	};

	for (const auto& testCase : cases)
	{
		const std::string cfgFile = writeConfig((testCase.first + ".json").c_str(), true, testCase.second);
		WtDtRunner runner;
		runner.initialize(cfgFile.c_str(), true, _bin.c_str(), _logCfg.c_str(), nullptr, nullptr);

		EXPECT_FALSE(runner.isInitialized()) << testCase.first;
		EXPECT_EQ(nullptr, runner.getBaseDataMgr().getSession("SD0930")) << testCase.first;
		EXPECT_EQ(runner.activeParserCount(), 0u) << testCase.first;
		EXPECT_FALSE(runner.hasQuarantinedParser()) << testCase.first;
		EXPECT_FALSE(runner.hasRetainedParserAdapters()) << testCase.first;
	}
}

TEST_F(RunnerFixture, corrected_parser_sidecar_can_retry_after_preflight_failure)
{
	const fs::path sidecar = _dir / "parsers_retry.json";
	writeFile(sidecar, "not json");
	const std::string extra = ",\"parsers\":\"" + sidecar.string() + "\"";
	const std::string cfgFile = writeConfig("wtdt_retry_parser.json", true, extra);

	WtDtRunner runner;
	runner.initialize(cfgFile.c_str(), true, _bin.c_str(), _logCfg.c_str(), nullptr, nullptr);
	EXPECT_FALSE(runner.isInitialized());
	EXPECT_EQ(nullptr, runner.getBaseDataMgr().getSession("SD0930"));
	EXPECT_FALSE(runner.hasQuarantinedParser());
	EXPECT_FALSE(runner.hasRetainedParserAdapters());

	// The first failure must happen before data-manager/store initialization;
	// the corrected retry then reaches the single initDataMgr call in initialize().
	writeFile(sidecar, R"JSON({"parsers":[]})JSON");
	runner.initialize(cfgFile.c_str(), true, _bin.c_str(), _logCfg.c_str(), nullptr, nullptr);
	EXPECT_TRUE(runner.isInitialized());
	EXPECT_NE(nullptr, runner.getBaseDataMgr().getSession("SD0930"));
	EXPECT_EQ(runner.activeParserCount(), 0u);
	EXPECT_FALSE(runner.hasQuarantinedParser());
	EXPECT_FALSE(runner.hasRetainedParserAdapters());
}

TEST_F(RunnerFixture, FailedParserInitializationDoesNotRegisterOrStartRunner)
{
	const fs::path parserSource = fs::path(_bin) / "libParserUDP.dylib";
	ASSERT_TRUE(fs::exists(parserSource)) << parserSource.string();
	const fs::path moduleRoot = _dir / "module_root";
	const fs::path parserDir = moduleRoot / "parsers";
	fs::create_directories(parserDir);
	fs::copy_file(parserSource, parserDir / "libParserUDP.dylib",
		fs::copy_options::overwrite_existing);
	fs::copy_file(fs::path(_bin) / "libWtDataStorageAD.dylib",
		moduleRoot / "libWtDataStorageAD.dylib", fs::copy_options::overwrite_existing);

	const std::string parser = ",\"parsers\":[{\"active\":true,\"id\":\"r85_bad_parser\","
		"\"module\":\"ParserUDP\",\"host\":\"not-an-ip-address\","
		"\"bport\":31001,\"sport\":31002}]";
	const std::string cfgFile = writeConfig("wtdt_parser_init_failure.json", true, parser);

	// Keep the runner and quarantined parser graph alive for the test process:
	// the provider has no verified callback-drain contract for destruction.
	for (const std::string& suffix : { std::string(), std::string("/") })
	{
		WtDtRunner* runner = new WtDtRunner();
		const std::string moduleDir = moduleRoot.string() + suffix;
		runner->initialize(cfgFile.c_str(), true, moduleDir.c_str(),
			_logCfg.c_str(), nullptr, nullptr);

		// A failed parser candidate is kept in quarantine and never registered.
		EXPECT_FALSE(runner->isInitialized()) << moduleDir;
		EXPECT_EQ(runner->activeParserCount(), 0u) << moduleDir;
		EXPECT_TRUE(runner->hasQuarantinedParser()) << moduleDir;
		EXPECT_TRUE(runner->hasRetainedParserAdapters()) << moduleDir;
		runner->start();
		EXPECT_FALSE(runner->isInitialized()) << moduleDir;
		EXPECT_EQ(runner->activeParserCount(), 0u) << moduleDir;
		retainedRunnerGraphs().push_back(runner);
	}
}

// WtDtRunner.cpp:218-239
TEST_F(RunnerFixture, bars_by_range_period_and_default_end_time)
{
	const std::string cfgFile = writeConfig("wtdt_range.json", true, "");
	WtDtRunner runner;
	runner.initialize(cfgFile.c_str(), true, _bin.c_str(), _logCfg.c_str(), nullptr, nullptr);

	// times 非 5 倍数 -> KP_Minute1 + realTimes=7 -> 缓存首次装配空库返回 NULL
	EXPECT_EQ(nullptr, runner.get_bars_by_range(kCode, "m7", 202401020930, 202401021500));
	// 非 m 开头 -> KP_DAY（times=2 仍走缓存装配）
	EXPECT_EQ(nullptr, runner.get_bars_by_range(kCode, "d2", 20240102, 20240103));
	// endTime==0 -> 用当日 2359 兜底
	EXPECT_EQ(nullptr, runner.get_bars_by_range(kCode, "m7", 202401020930, 0));
}

// WtDtRunner.cpp:258-281
TEST_F(RunnerFixture, bars_by_date_period_and_default_date)
{
	const std::string cfgFile = writeConfig("wtdt_date.json", true, "");
	WtDtRunner runner;
	runner.initialize(cfgFile.c_str(), true, _bin.c_str(), _logCfg.c_str(), nullptr, nullptr);

	EXPECT_EQ(nullptr, runner.get_bars_by_date(kCode, "m7", 20240102));
	// uDate==0 -> 当前日期兜底
	EXPECT_EQ(nullptr, runner.get_bars_by_date(kCode, "m7", 0));
}

// WtDtRunner.cpp:359-364 + :617-635
TEST_F(RunnerFixture, ticks_by_count_default_end_and_day_bar_subscription)
{
	const std::string cfgFile = writeConfig("wtdt_count.json", true, "");
	WtDtRunner runner;
	runner.initialize(cfgFile.c_str(), true, _bin.c_str(), _logCfg.c_str(), nullptr, nullptr);

	// endTime==0 -> 当日 2359 -> AD byCount 空返回
	EXPECT_EQ(nullptr, runner.get_ticks_by_count(kCode, 5, 0));

	// 非 m 开头 -> KP_DAY/realTimes=1 -> subscribe_bar 的 times==1 分支读到空库即返回
	EXPECT_NO_THROW(runner.sub_bar(kCode, "d1"));
	// 分钟周期同样只落订阅表
	EXPECT_NO_THROW(runner.sub_bar(kCode, "m7"));
}
