#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

#include "t27_seam.hpp"

#ifndef WT_T1_XTP_MISSING_SYMBOL_MODULE
#error "T1-XTP-01 fixture module names must be provided by coverage CMake"
#endif

#ifndef WT_T1_XTP_NULL_CREATOR_MODULE
#error "T1-XTP-01 fixture module names must be provided by coverage CMake"
#endif

#ifndef WT_T1_XTP_SUCCESSFUL_CREATOR_MODULE
#error "T1-XTP-01 fixture module names must be provided by coverage CMake"
#endif

using namespace t27;

namespace
{
	namespace fs = std::filesystem;
	using ModuleCounter = int (*)();

	struct TempDir
	{
		fs::path path;

		TempDir()
		{
			static std::atomic<uint64_t> counter(0);
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			path = fs::temp_directory_path() /
				("wt_t1_xtp_loader_" + std::to_string(stamp) + "_" + std::to_string(counter++));
			fs::create_directories(path);
		}

		~TempDir()
		{
			std::error_code ec;
			fs::remove_all(path, ec);
		}
	};

	WTSVariant* makeConfig(const fs::path& flowDir, const char* module)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		if (cfg == nullptr)
			return nullptr;
		cfg->append("host", "127.0.0.1");
		cfg->append("port", (int32_t)11101);
		cfg->append("user", "t1_xtp_user");
		cfg->append("pass", "t1_xtp_pass");
		cfg->append("protocol", static_cast<uint32_t>(XTP_PROTOCOL_UDP));
		cfg->append("clientid", 7u);
		cfg->append("hbinterval", 15u);
		cfg->append("buffsize", 2u);
		cfg->append("flowdir", flowDir.string().c_str());
		cfg->append("local_ip", "0.0.0.0");
		cfg->append("xtpmodule", module);
		return cfg;
	}

	ParserXTPMirror& mirror(ParserXTP& parser)
	{
		return *reinterpret_cast<ParserXTPMirror*>(&parser);
	}

	ModuleCounter moduleCounter(ParserXTP& parser, const char* name)
	{
		return reinterpret_cast<ModuleCounter>(DLLHelper::get_symbol(mirror(parser).m_hInst, name));
	}

	void expectFailedInit(ParserXTP& parser, FakeSink& sink, const char* diagnostic)
	{
		ParserXTPMirror& state = mirror(parser);
		EXPECT_EQ(nullptr, state.m_hInst);
		EXPECT_EQ(nullptr, state.m_funcCreator);
		EXPECT_EQ(nullptr, state.m_pUserAPI);
		EXPECT_TRUE(sink.hasLog(diagnostic));
		EXPECT_NO_THROW(parser.release());
	}
}

TEST(ParserXTPInit, missing_module_fails_without_registering_or_connecting)
{
	TempDir temp;
	const std::string missingModule = "wt_t1_xtp_missing_" + std::to_string(temp.path.filename().string().size()) +
		"_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
	WTSVariant* cfg = makeConfig(temp.path / "flow", missingModule.c_str());
	ASSERT_NE(nullptr, cfg);

	ParserXTP parser;
	FakeSink sink;
	parser.registerSpi(&sink);
	EXPECT_FALSE(parser.init(cfg));
	expectFailedInit(parser, sink, "Failed to load XTP quote API module");
	cfg->release();
}

TEST(ParserXTPInit, module_without_creator_symbol_fails_and_closes_handle)
{
	TempDir temp;
	WTSVariant* cfg = makeConfig(temp.path / "flow", WT_T1_XTP_MISSING_SYMBOL_MODULE);
	ASSERT_NE(nullptr, cfg);

	ParserXTP parser;
	FakeSink sink;
	parser.registerSpi(&sink);
	EXPECT_FALSE(parser.init(cfg));
	expectFailedInit(parser, sink, "creator symbol was not found");
	cfg->release();
}

TEST(ParserXTPInit, creator_returning_null_fails_and_closes_handle)
{
	TempDir temp;
	WTSVariant* cfg = makeConfig(temp.path / "flow", WT_T1_XTP_NULL_CREATOR_MODULE);
	ASSERT_NE(nullptr, cfg);

	ParserXTP parser;
	FakeSink sink;
	parser.registerSpi(&sink);
	EXPECT_FALSE(parser.init(cfg));
	expectFailedInit(parser, sink, "creator returned null");
	cfg->release();
}

TEST(ParserXTPInit, successful_creator_registers_parser_spi)
{
	TempDir temp;
	WTSVariant* cfg = makeConfig(temp.path / "flow", WT_T1_XTP_SUCCESSFUL_CREATOR_MODULE);
	ASSERT_NE(nullptr, cfg);

	ParserXTP parser;
	FakeSink sink;
	parser.registerSpi(&sink);
	ASSERT_TRUE(parser.init(cfg));
	ParserXTPMirror& state = mirror(parser);
	ASSERT_NE(nullptr, state.m_hInst);
	ASSERT_NE(nullptr, state.m_funcCreator);
	ASSERT_NE(nullptr, state.m_pUserAPI);

	ModuleCounter registered = moduleCounter(parser, "wt_t1_xtp_nonnull_spi_registrations");
	ModuleCounter unregistered = moduleCounter(parser, "wt_t1_xtp_null_spi_registrations");
	ModuleCounter released = moduleCounter(parser, "wt_t1_xtp_release_calls");
	ASSERT_NE(nullptr, registered);
	ASSERT_NE(nullptr, unregistered);
	ASSERT_NE(nullptr, released);
	EXPECT_EQ(1, registered());
	EXPECT_EQ(0, unregistered());
	EXPECT_EQ(0, released());

		parser.release();
	EXPECT_EQ(nullptr, state.m_pUserAPI);
	EXPECT_EQ(1, unregistered());
	EXPECT_EQ(1, released());
	cfg->release();
}
