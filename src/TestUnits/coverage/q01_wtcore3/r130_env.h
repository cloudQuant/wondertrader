/**
 * r130_env.h — lane R130 自用临时目录（非 .cpp，不会被 GLOB 收进编译单元）
 *
 * 每个用例一个 temp_directory_path()/r130_wtcore_<pid>_<seq>/，构造时
 * 全量重建并 setGenerateDir，析构时整棵删除；不复用 q01/p01 的 static
 * 缓存目录，避免同进程内先析构后复用导致目录不存在。
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <unistd.h>

#include <WtCore/WtHelper.h>

namespace r130 {

class TempEnv
{
public:
    explicit TempEnv(const char* tag)
    {
        static uint32_t seq = 0;
        std::filesystem::path p = std::filesystem::temp_directory_path()
            / ("r130_wtcore_" + std::string(tag) + "_"
               + std::to_string(static_cast<long long>(::getpid())) + "_"
               + std::to_string(++seq));
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        std::filesystem::create_directories(p, ec);
        _dir = p.string() + "/";
        WtHelper::setGenerateDir(_dir.c_str());
    }

    ~TempEnv()
    {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(_dir), ec);
    }

    const std::string& dir() const { return _dir; }

private:
    std::string _dir;
};

}  // namespace r130
