#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#include "Share/DLLHelper.hpp"
#include "WtCore/WtHelper.h"

namespace wtp_test
{
	/*
	 * Locate the product-built plugin beside the coverage executable's output
	 * directory, then copy it into a private traders/ directory. Preserve the
	 * product executer search path when available. Runner plugin lookup uses
	 * WtHelper::getInstDir(), so redirect that directory only for this fixture;
	 * the shared product output is read-only.
	 */
	class TraderMockerFixture
	{
	public:
		explicit TraderMockerFixture(const char* name) : _name(name ? name : "test") {}
		TraderMockerFixture(const TraderMockerFixture&) = delete;
		TraderMockerFixture& operator=(const TraderMockerFixture&) = delete;

		~TraderMockerFixture()
		{
			if (_installed)
				WtHelper::setInstDir(_previous_inst_dir.c_str());

			std::error_code ec;
			if (!_fixture_dir.empty())
				std::filesystem::remove_all(_fixture_dir, ec);
		}

		bool install()
		{
			const std::string module = DLLHelper::wrap_module("TraderMocker");
			std::filesystem::path runner_dir(WtHelper::getInstDir());
			if (runner_dir.empty())
			{
				_error = "runner executable directory is not configured";
				return false;
			}
			if (runner_dir.filename().empty())
				runner_dir = runner_dir.parent_path();

			_product_plugin = runner_dir.parent_path() / module;
			std::error_code ec;
			if (!std::filesystem::is_regular_file(_product_plugin, ec))
			{
				_error = "product plugin not found at " + _product_plugin.string();
				if (ec)
					_error += ": " + ec.message();
				return false;
			}

			const std::filesystem::path temp_root = std::filesystem::temp_directory_path(ec);
			if (ec)
			{
				_error = "could not locate temporary directory: " + ec.message();
				return false;
			}

			static std::atomic<unsigned long long> sequence{ 0 };
			const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
			for (unsigned int attempt = 0; attempt < 32; ++attempt)
			{
				const auto unique = sequence.fetch_add(1, std::memory_order_relaxed);
				_fixture_dir = temp_root / ("wt_" + _name + "_trader_mocker_"
					+ std::to_string(nonce) + "_" + std::to_string(unique));
				ec.clear();
				if (std::filesystem::create_directories(_fixture_dir / "traders", ec))
					break;
				_fixture_dir.clear();
				if (ec)
				{
					_error = "could not create isolated fixture directory: " + ec.message();
					return false;
				}
			}
			if (_fixture_dir.empty())
			{
				_error = "could not allocate a unique isolated fixture directory";
				return false;
			}

			ec.clear();
			if (!std::filesystem::copy_file(_product_plugin,
				_fixture_dir / "traders" / module, ec))
			{
				_error = "could not copy product plugin into isolated fixture directory";
				if (ec)
					_error += ": " + ec.message();
				std::filesystem::remove_all(_fixture_dir, ec);
				_fixture_dir.clear();
				return false;
			}

			std::filesystem::path product_executers = runner_dir / "executer";
			ec.clear();
			if (!std::filesystem::is_directory(product_executers, ec))
			{
				product_executers = runner_dir.parent_path() / "WtPorter" / "executer";
				ec.clear();
			}
			if (std::filesystem::is_directory(product_executers, ec))
			{
				ec.clear();
				std::filesystem::create_directory_symlink(product_executers,
					_fixture_dir / "executer", ec);
				if (ec)
				{
					ec.clear();
					std::filesystem::copy(product_executers, _fixture_dir / "executer",
						std::filesystem::copy_options::recursive, ec);
				}
				if (ec)
				{
					_error = "could not preserve the product executer lookup directory: " + ec.message();
					std::filesystem::remove_all(_fixture_dir, ec);
					_fixture_dir.clear();
					return false;
				}
			}

			_previous_inst_dir = WtHelper::getInstDir();
			const std::string fixture_inst_dir = _fixture_dir.string() + "/";
			WtHelper::setInstDir(fixture_inst_dir.c_str());
			_installed = true;
			return true;
		}

		const std::filesystem::path& product_plugin() const { return _product_plugin; }
		const std::string& error() const { return _error; }

	private:
		std::string _name;
		std::filesystem::path _product_plugin;
		std::filesystem::path _fixture_dir;
		std::string _previous_inst_dir;
		std::string _error;
		bool _installed = false;
	};
}
