/**
 * r160_common.h — lane R160 自用小工具（第二十四波）
 *
 * 只提供与生产代码无关的文件系统辅助：
 *   - exe_dir()/find_module_dir()：从测试二进制目录向上找插件动态库
 *   - TempDir：系统临时目录下的唯一子目录，析构整棵删除
 *   - ScopedCwd：用例内切工作目录，析构恢复
 * 不启动线程、不访问网络；插件/数据目录均由各自用例自行装配。
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <Share/DLLHelper.hpp>

namespace r160 {

namespace fs = std::filesystem;

inline std::string exe_dir()
{
    char buf[4096] = { 0 };
#ifdef __APPLE__
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return std::string();
    std::error_code ec;
    fs::path p = fs::canonical(fs::path(buf), ec);
    return ec ? fs::path(buf).parent_path().string() : p.parent_path().string();
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return std::string();
    return fs::path(std::string(buf, static_cast<size_t>(n))).parent_path().string();
#endif
}

/// 在测试二进制所在目录及上三级里找 <libWtMsgQue.dylib / libTraderMocker.dylib> 等
inline std::string find_module_dir(const char* module)
{
    const std::string wrapped = DLLHelper::wrap_module(module, "lib");
    fs::path dir(exe_dir());
    for (int up = 0; up < 4 && !dir.empty(); ++up)
    {
        std::error_code ec;
        if (fs::exists(dir / wrapped, ec))
            return dir.string();
        dir = dir.parent_path();
    }
    return std::string();
}

class TempDir
{
public:
    explicit TempDir(const std::string& tag)
    {
        static uint32_t seq = 0;
        std::error_code ec;
        _path = fs::temp_directory_path(ec)
            / ("wt_r160_" + tag + "_" + std::to_string(static_cast<long long>(::getpid()))
                + "_" + std::to_string(++seq));
        fs::remove_all(_path, ec);
        fs::create_directories(_path, ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(_path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string path(const std::string& rel = "") const
    {
        return rel.empty() ? _path.string() : (_path / rel).string();
    }

    const fs::path& raw() const { return _path; }

private:
    fs::path _path;
};

class ScopedCwd
{
public:
    explicit ScopedCwd(const std::string& dir)
    {
        std::error_code ec;
        _old = fs::current_path(ec);
        _ok = !ec && (::chdir(dir.c_str()) == 0);
    }

    ~ScopedCwd()
    {
        if (!_old.empty())
        {
            std::error_code ec;
            fs::current_path(_old, ec);
        }
    }

    ScopedCwd(const ScopedCwd&) = delete;
    ScopedCwd& operator=(const ScopedCwd&) = delete;

    bool ok() const { return _ok; }

private:
    fs::path _old;
    bool _ok = false;
};

} // namespace r160
