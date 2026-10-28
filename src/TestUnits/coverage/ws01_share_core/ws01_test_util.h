// WS01 Share core utilities: shared test helpers (not a test translation unit).
#pragma once

#include <atomic>
#include <filesystem>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace ws01
{

// Per-instance unique temporary directory under the system temp dir.
// Removed recursively on destruction.
class TempDir
{
public:
	TempDir()
	{
		static std::atomic<unsigned> counter{ 0 };
#ifdef _WIN32
		unsigned long pid = 0;
#else
		unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
		_dir = std::filesystem::temp_directory_path()
			/ ("ws01_share_core_" + std::to_string(pid) + "_" + std::to_string(counter.fetch_add(1)));
		std::error_code ec;
		std::filesystem::create_directories(_dir, ec);
	}

	~TempDir()
	{
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

	TempDir(const TempDir&) = delete;
	TempDir& operator=(const TempDir&) = delete;

	std::string file(const std::string& name) const
	{
		return (_dir / name).string();
	}

	const std::filesystem::path& path() const { return _dir; }

private:
	std::filesystem::path _dir;
};

} // namespace ws01
