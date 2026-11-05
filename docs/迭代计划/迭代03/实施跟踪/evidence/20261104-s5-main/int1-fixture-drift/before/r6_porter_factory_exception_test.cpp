#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <system_error>

#include "WtPorter/WtPorter.h"

#ifndef WT_R6_THROWING_FACTORY_DIR
#error "WT_R6_THROWING_FACTORY_DIR must point to the throwing factory fixture"
#endif
#if !defined(WT_R6_CTA_FACTORY_PLUGIN) || !defined(WT_R6_SEL_FACTORY_PLUGIN) || !defined(WT_R6_HFT_FACTORY_PLUGIN)
#error "WT_R6_*_FACTORY_PLUGIN must point to the isolated real factory plugins"
#endif

namespace
{
	class IsolatedFactoryDirectory
	{
	public:
		explicit IsolatedFactoryDirectory(const char* pluginPath)
		{
			static unsigned int nextId = 0;
			_dir = std::filesystem::path(::testing::TempDir()) /
				("r6_capi_factory_positive_" + std::to_string(++nextId));
			std::error_code ec;
			std::filesystem::remove_all(_dir, ec);
			ec.clear();
			_ok = std::filesystem::create_directories(_dir, ec) && !ec;
			if (!_ok)
				return;

			const std::filesystem::path source(pluginPath);
			std::filesystem::copy_file(source, _dir / source.filename(),
				std::filesystem::copy_options::overwrite_existing, ec);
			_ok = !ec;
		}

		~IsolatedFactoryDirectory()
		{
			std::error_code ec;
			std::filesystem::remove_all(_dir, ec);
		}

		bool ok() const { return _ok; }
		std::string path_with_separator() const { return _dir.string() + "/"; }

	private:
		std::filesystem::path _dir;
		bool _ok {false};
	};
}

TEST(R6PorterFactoryExceptions, cta_creator_exception_returns_false_without_crossing_c_api)
{
	EXPECT_NO_THROW(EXPECT_FALSE(reg_cta_factories(WT_R6_THROWING_FACTORY_DIR)));
}

TEST(R6PorterFactoryExceptions, sel_creator_exception_returns_false_without_crossing_c_api)
{
	EXPECT_NO_THROW(EXPECT_FALSE(reg_sel_factories(WT_R6_THROWING_FACTORY_DIR)));
}

TEST(R6PorterFactoryExceptions, hft_creator_exception_returns_false_without_crossing_c_api)
{
	EXPECT_NO_THROW(EXPECT_FALSE(reg_hft_factories(WT_R6_THROWING_FACTORY_DIR)));
}

TEST(R6PorterFactoryRegistration, cta_real_plugin_registers_through_c_api)
{
	IsolatedFactoryDirectory dir(WT_R6_CTA_FACTORY_PLUGIN);
	ASSERT_TRUE(dir.ok());
	EXPECT_NO_THROW(EXPECT_TRUE(reg_cta_factories(dir.path_with_separator().c_str())));
}

TEST(R6PorterFactoryRegistration, sel_real_plugin_registers_through_c_api)
{
	IsolatedFactoryDirectory dir(WT_R6_SEL_FACTORY_PLUGIN);
	ASSERT_TRUE(dir.ok());
	EXPECT_NO_THROW(EXPECT_TRUE(reg_sel_factories(dir.path_with_separator().c_str())));
}

TEST(R6PorterFactoryRegistration, hft_real_plugin_registers_through_c_api)
{
	IsolatedFactoryDirectory dir(WT_R6_HFT_FACTORY_PLUGIN);
	ASSERT_TRUE(dir.ok());
	EXPECT_NO_THROW(EXPECT_TRUE(reg_hft_factories(dir.path_with_separator().c_str())));
}
