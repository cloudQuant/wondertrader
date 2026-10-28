/**
 * r170_env.h — lane R170 / wave25
 *
 * 实时 ticker run() 用例的公共夹具（非 .cpp，不会被 GLOB 收进编译单元）：
 *   - TempEnv  ：temp_directory_path()/r170_wtcore_<tag>_<pid>_<seq>/，构造重建、
 *                析构整棵删除；每次实例化独立，不像 q01sel::Q01TempEnv 那样
 *                按进程静态记忆化（同进程多用例时目录会被前一个用例删掉）。
 *   - makeAg   ：登记 TRADING 时段（0:00-<close>）与 SHFE.ag/ag2512，
 *                专供 0 点起始的短会话，使 ticker 自动闭合后仍留在交易时段内，
 *                线程下一轮只做 10ms 轮询而不是 10 秒休市 sleep。
 *
 * 不联网、不读真实行情；ticker 线程只由 stop() 收尾。
 */
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include <unistd.h>

#include <WtCore/WtHelper.h>

#include "q01_sel_env.h"

namespace r170 {

using namespace wtp;

class TempEnv
{
public:
    explicit TempEnv(const char* tag)
    {
        static uint32_t seq = 0;
        std::filesystem::path p = std::filesystem::temp_directory_path()
            / ("r170_wtcore_" + std::string(tag) + "_"
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

/**
 * 登记一个 0:00 起的短会话（close 为 HHMM，如 400 表示 04:00）。
 * 品种/合约信息挂到该 session 上，ticker::init 可经 IBaseDataMgr 取到时段。
 */
inline void makeAg(q01sel::BaseDataMgr& bd, uint32_t closeHHMM)
{
    bd.regisSessionWithTime("TRADING", 0, closeHHMM);
    WTSSessionInfo* sess = bd.session("TRADING");
    WTSCommodityInfo* ag = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
    ag->setSessionInfo(sess);
    bd.regis(ag, q01sel::makeCt("SHFE", "ag", "ag2512"));
}

}  // namespace r170
