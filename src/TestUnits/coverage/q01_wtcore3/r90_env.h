/**
 * r90_env.h — lane R90 自用环境（wave17, WtCore 残余公开面）
 *
 * 只提供三样东西：
 *   - R90TempRoot：系统临时目录下一个进程级目录，进程退出时整棵删除；
 *   - R90GenEnv：gtest 全局环境，把 WtHelper 的生成目录钉到上述目录；
 *   - R90BinDir：从测试可执行文件位置回溯 build_x64/Debug/bin，供需要
 *     dlopen 的少数用例定位既有构建产物（找不到时用例自行 GTEST_SKIP）。
 *
 * 不修改任何生产代码，不引用 WtUftCore/WtBtCore 同名类。
 */
#pragma once

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#include <WtCore/WtHelper.h>

namespace r90 {

namespace fs = std::filesystem;

inline std::string& temp_root_ref()
{
    static std::string root;
    return root;
}

inline void cleanup_temp_root()
{
    std::error_code ec;
    fs::remove_all(fs::path(temp_root_ref()), ec);
}

/** 进程级唯一临时目录（不随单个用例删除，避免 WtHelper 目录 getter 的 static 缓存悬空）。 */
inline const std::string& temp_root()
{
    static const std::string root = []() {
        fs::path p = fs::temp_directory_path()
            / ("r90_wtcore_" + std::to_string(static_cast<long long>(::getpid())));
        std::error_code ec;
        fs::remove_all(p, ec);
        fs::create_directories(p, ec);
        temp_root_ref() = p.string();
        std::atexit(&cleanup_temp_root);
        return p.string() + "/";
    }();
    return root;
}

inline std::string write_file(const std::string& rel, const std::string& content)
{
    fs::path p = rel.empty() ? fs::path(temp_root()) : (fs::path(temp_root()) / rel);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream ofs(p.string().c_str(), std::ios::binary | std::ios::trunc);
    ofs << content;
    ofs.close();
    return p.string();
}

inline std::string read_file(const std::string& path)
{
    std::ifstream ifs(path.c_str(), std::ios::binary);
    if (!ifs.is_open())
        return std::string();
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

class GenEnv : public ::testing::Environment
{
public:
    void SetUp() override { WtHelper::setGenerateDir(temp_root().c_str()); }
};

inline std::string exe_dir()
{
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, 0);
    _NSGetExecutablePath(buf.data(), &size);
    std::error_code ec;
    fs::path exe = fs::weakly_canonical(fs::path(buf.data()), ec);
    if (ec)
        exe = fs::path(buf.data());
    return exe.parent_path().string();
#else
    char buf[4096] = {0};
    ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0)
        return std::string();
    return fs::path(std::string(buf, static_cast<size_t>(len))).parent_path().string();
#endif
}

/** 从可执行文件目录向上找含 libWtMsgQue / libWtExeFact 的构建输出目录。 */
inline std::string find_bin_dir()
{
    fs::path dir = exe_dir();
    for (int up = 0; up < 6 && !dir.empty(); ++up)
    {
        const fs::path candidates[] = {
            dir,
            dir / "bin",
            dir / "Debug" / "bin",
            dir / "build_x64" / "Debug" / "bin",
        };
        for (const fs::path& cand : candidates)
        {
            std::error_code ec;
            if (fs::exists(cand / "libWtMsgQue.dylib", ec)
                || fs::exists(cand / "libWtMsgQue.so", ec)
                || fs::exists(cand / "libWtExeFact.dylib", ec)
                || fs::exists(cand / "libWtExeFact.so", ec))
                return cand.string();
        }
        dir = dir.parent_path();
    }
    return std::string();
}

}  // namespace r90
