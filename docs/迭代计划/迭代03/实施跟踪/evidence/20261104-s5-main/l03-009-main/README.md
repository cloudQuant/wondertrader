# BUG-L03-009：主线程独立复核（卡2 时间合同）

- 用 LLVM 22（`clang++ -std=c++17 -g -O1 -fsanitize=address,undefined,signed-integer-overflow -fno-sanitize-recover=undefined`，Homebrew Boost 1.88 / GTest 1.17）把 `l03_includes/l03_session_info_test.cpp` 与 header-only 的 `WTSSessionInfo.hpp` 一起重编。该测试含 int32 极值、±1441/±2880、左右 0000/2400 对齐、多日 checked 日期、重复 setter 等用例。在 UTC、America/New_York、Asia/Shanghai 三个时区各 31/31 通过，无 sanitizer 报告（`l03-*.log/xml`）。三个时区结果一致，说明会话分钟运算不受本机时区/DST 影响。
- 新增 `s5_main_l03009_offset_config_test.cpp`，2 例：会话配置 offset 为 ±1441、±2880、INT32_MAX 时整份文件被拒绝，不留部分注册；±1440、0、300、-420 时接受。两例在当前树通过（`l03009.log/xml`）。这是对既有实现（`loadSessions` 的 `sessionInteger(-1440,1440)`）的验证，其红绿已由 2026-11-03 的证据给出。
- 生产调用方均已用 `tryGetOffsetDate`（WtDataWriter、WtDataWriterAD、StateMonitor、WtVWapExeUnit、WtStockVWapExeUnit），旧 `getOffsetDate` 已无生产调用方（主线程 grep 核对）。写端在入队前同步校验会话日期，失败时 `writeTick` 返回 false，即对调用方的失败回执。
- 未覆盖：Windows/MSVC、真实行情。
