#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "CTPLoader/TraderSpi.h"
#include "t6_ctploader_yaml_factory_stub.hpp"

extern "C" int run(const char* cfgfile, bool bAsync, bool isFile);

extern CThostFtdcTraderApi* pUserApi;
extern std::string FRONT_ADDR;
extern std::string BROKER_ID;
extern std::string INVESTOR_ID;
extern std::string PASSWORD;
extern std::string SAVEPATH;
extern std::string APPID;
extern std::string AUTHCODE;
extern uint32_t CLASSMASK;
extern bool ONLYINCFG;
extern bool INCREMENTAL;
extern bool QRYFEES;
extern std::string COMM_FILE;
extern std::string CONT_FILE;
extern std::string FEES_FILE;
extern std::string MODULE_NAME;
extern std::string FEE_FILTERS;
extern std::map<std::string, std::string> MAP_NAME;
extern std::map<std::string, std::string> MAP_SESSION;
extern std::set<std::string> SET_FILTERS;

namespace
{
	class TempWorkspace
	{
	public:
		TempWorkspace()
		{
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			for (uint32_t attempt = 0; attempt < 100 && _path.empty(); ++attempt)
			{
				std::filesystem::path candidate = std::filesystem::temp_directory_path()
					/ ("t6_ctploader_yaml_" + std::to_string(stamp) + "_" + std::to_string(attempt));
				std::error_code ec;
				if (std::filesystem::create_directory(candidate, ec))
					_path = candidate;
			}
			if (_path.empty())
				throw std::runtime_error("failed to create an isolated T6 YAML test directory");
			std::filesystem::create_directories(_path / "output");
		}

		~TempWorkspace()
		{
			remove();
		}

		const std::filesystem::path& path() const { return _path; }

		bool remove()
		{
			std::error_code ec;
			std::filesystem::remove_all(_path, ec);
			return !ec && !std::filesystem::exists(_path);
		}

	private:
		std::filesystem::path _path;
	};

	class ScopedCurrentPath
	{
	public:
		explicit ScopedCurrentPath(const std::filesystem::path& path)
			: _previous(std::filesystem::current_path())
		{
			std::filesystem::current_path(path);
		}

		~ScopedCurrentPath()
		{
			std::error_code ec;
			std::filesystem::current_path(_previous, ec);
		}

	private:
		std::filesystem::path _previous;
	};

	struct FakeApiCleanup
	{
		void cleanup()
		{
			t6_yaml_ctp_api().releaseRegisteredSpi();
			pUserApi = nullptr;
		}

		~FakeApiCleanup() { cleanup(); }
	};
}

TEST(t6_ctploader_yaml_smoke, valid_file_applies_config_and_initializes_fake_without_network)
{
	TempWorkspace workspace;
	const std::filesystem::path workspacePath = workspace.path();
	const std::filesystem::path cfgPath = workspacePath / "valid_ctp.yaml";
	const std::filesystem::path savePath = workspacePath / "output";
	const std::filesystem::path modulePath = workspacePath / "fake-ctp-module.stub";
	{
		std::ofstream marker(modulePath);
		ASSERT_TRUE(marker.good());
		marker << "test-only module marker; Apple factory is supplied by this executable\n";
	}
	{
		std::ofstream cfg(cfgPath);
		ASSERT_TRUE(cfg.good());
		cfg << "ctp:\n"
			<< "  front: tcp://127.0.0.1:1\n"
			<< "  broker: '9550'\n"
			<< "  user: 'T6USER'\n"
			<< "  pass: 'test-password'\n"
			<< "  appid: 'test-app'\n"
			<< "  authcode: 'test-auth'\n"
			<< "  module: '" << modulePath.string() << "'\n"
			<< "config:\n"
			<< "  path: '" << savePath.string() << "'\n"
			<< "  mask: 7\n"
			<< "  onlyincfg: true\n"
			<< "  incremental: true\n"
			<< "  qryfees: true\n"
			<< "  commfile: 't6-commodities.json'\n"
			<< "  contfile: 't6-contracts.json'\n"
			<< "  feesfile: 't6-fees.json'\n";
	}

	FakeApiCleanup apiCleanup;
	T6YamlCtpApiStub& api = t6_yaml_ctp_api();
	pUserApi = nullptr;
	api.reset();
	FRONT_ADDR = "before-front";
	BROKER_ID = "before-broker";
	INVESTOR_ID = "before-user";
	PASSWORD = "before-password";
	SAVEPATH = "before-path";
	APPID = "before-app";
	AUTHCODE = "before-auth";
	CLASSMASK = 0;
	ONLYINCFG = false;
	INCREMENTAL = false;
	QRYFEES = false;
	COMM_FILE = "before-commodities.json";
	CONT_FILE = "before-contracts.json";
	FEES_FILE = "before-fees.json";
	MODULE_NAME = "before-module";
	FEE_FILTERS.clear();
	MAP_NAME.clear();
	MAP_SESSION.clear();
	SET_FILTERS.clear();

	int result = -1;
	{
		ScopedCurrentPath cwd(workspacePath);
		EXPECT_NO_THROW(result = run(cfgPath.string().c_str(), false, true));
		EXPECT_TRUE(std::filesystem::exists(workspacePath / "CTPFlow" / "9550" / "T6USER"));
	}

	// K02=B（2026-11-04 用户裁定）：run() 等待 SPI 发布的终态并返回该结果码，随后注销 SPI、释放 API、
	// 清空 pUserApi，不再调用 Join()。假 API 在费率批次进行中报告前置断开，终态为失败（改前期望：返回 0、
	// 保留 pUserApi、调用 Join，见 evidence/20261104-s5-main/k02-loader/before-int1/）。
	EXPECT_EQ(EXIT_FAILURE, result);
	EXPECT_EQ(nullptr, pUserApi);
	EXPECT_EQ(1, api.create_calls);
	EXPECT_EQ("./CTPFlow/9550/T6USER/", api.flow_path);
	EXPECT_EQ("tcp://127.0.0.1:1", api.front);
	EXPECT_EQ(2, api.register_spi_calls);
	EXPECT_EQ(nullptr, api.spi);
	EXPECT_EQ(1, api.subscribe_public_calls);
	EXPECT_EQ(1, api.subscribe_private_calls);
	EXPECT_EQ(1, api.init_calls);
	EXPECT_EQ(0, api.join_calls);
	EXPECT_EQ(1u, api.n_auth_);
	EXPECT_EQ(1u, api.n_login_);
	EXPECT_EQ((std::vector<std::string>{
		"CreateFtdcTraderApi", "RegisterSpi", "SubscribePublicTopic",
		"SubscribePrivateTopic", "RegisterFront", "Init", "RegisterSpi"}), api.call_order);

	EXPECT_EQ("tcp://127.0.0.1:1", FRONT_ADDR);
	EXPECT_EQ("9550", BROKER_ID);
	EXPECT_EQ("T6USER", INVESTOR_ID);
	EXPECT_EQ("test-password", PASSWORD);
	EXPECT_EQ("test-app", APPID);
	EXPECT_EQ("test-auth", AUTHCODE);
	EXPECT_EQ(7u, CLASSMASK);
	EXPECT_TRUE(ONLYINCFG);
	EXPECT_TRUE(INCREMENTAL);
	EXPECT_TRUE(QRYFEES);
	EXPECT_EQ("t6-commodities.json", COMM_FILE);
	EXPECT_EQ("t6-contracts.json", CONT_FILE);
	EXPECT_EQ("t6-fees.json", FEES_FILE);
	EXPECT_EQ(modulePath.string(), MODULE_NAME);
	EXPECT_EQ(0u, SET_FILTERS.size());
	EXPECT_NE(std::string::npos, SAVEPATH.find(savePath.string()));

	apiCleanup.cleanup();
	EXPECT_TRUE(workspace.remove()) << "temporary CTPFlow and YAML files must be removed";
}
