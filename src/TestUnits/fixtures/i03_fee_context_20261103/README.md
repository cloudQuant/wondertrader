# 费用上下文计算定向验证

本夹具验证 `wt_fee::Schedule` 的查询与 `wt_fee::Context` 快照计算。旧候选夹具提供 8 个 `FeeScheduleCandidate` 用例；本夹具提供 5 个 `FeeContext` 用例。验证范围包括合约优先、品种回退、缺费与合法零费、最低费、负费率返佣及正负分位舍入、溢出和非法输入、重载失败清空、快照独立性，以及旧版数字字符串在小数逗号区域设置下的解析。

## 依赖

以下命令在 macOS arm64、Apple clang 17.0.0 和 C++17 下验证。`run.sh` 固定使用 `/usr/bin/c++`、系统 C++ 标准库和 UBSan 运行时，包含 `src`、`/opt/homebrew/include`、`/usr/local/include`，静态链接 `/opt/homebrew/lib/libgtest_main.a` 和 `/opt/homebrew/lib/libgtest.a`，并使用 pthread。RapidJSON 的 `rapidjson/document.h` 此机位于 `/usr/local/include`；GoogleTest 头文件及其上述两个库必须可用。脚本还需要 Bash、`mkdir`、`tee` 和 `shasum`，不依赖 Python 或 CMake。

测试需要仓库中的 `src/TestUnits/fixtures/i03_fee_schedule_candidate/fee_schedule_test.cpp` 与 `src/TestUnits/fixtures/fees_minfee.json`；后者由旧候选用例以仓库根目录为相对路径读取。小数逗号用例尝试 `de_DE.UTF-8`、`fr_FR.UTF-8`、`nl_NL.UTF-8`；三者均不可用时 GoogleTest 会明确报告 SKIPPED，该用例不应计为通过。测试会恢复原 `LC_NUMERIC`。

## 构建及重跑

在仓库根目录运行。输出目录必须不存在；每次复跑更换目录名称：

```bash
GTEST_FILTER='FeeContext.*:FeeScheduleCandidate.*' \
  bash src/TestUnits/fixtures/i03_fee_context_20261103/run.sh \
  /tmp/wt-i03-fee-context-review-NEW
```

脚本顺序构建并执行普通和 UBSan 变体，使用 `-Wall -Wextra -Werror`；UBSan 变体另启用 `-fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all`，并设置 `UBSAN_OPTIONS=halt_on_error=1`。过滤器选择两个测试套件，共 13 个用例。脚本输出两个可执行文件、`normal.log`、`ubsan.log` 和包含头文件及本夹具测试源 SHA-256 的 `hashes.txt`。检查日志中的通过数和 SKIPPED；仅凭脚本退出码不能把跳过的 locale 测试算作通过。

## 本机记录

- 使用上述过滤器的复跑结果：普通 **13/13 通过**，局部 UBSan **13/13 通过**，无跳过；实际选择 `de_DE.UTF-8`，小数分隔符为逗号。
- 普通日志：`/tmp/wt-i03-fee-context-review-20261103/normal.log`，SHA-256 `1041cf9c02d58cfe54458931e5bd9f5dad03b140970ac965d44328dd66dc655e`。
- UBSan 日志：`/tmp/wt-i03-fee-context-review-20261103/ubsan.log`，SHA-256 `4fe88cb9a543b64114bbc59bc00673a471200c5df30c9d6f92133eb374b1ffbd`。
- 当时 `src/Share/FeeSchedule.hpp` 的 SHA-256 为 `524d61c11b9894b922e2eae2fe34388e7e4fcc99d68395b75dcbcfaf3cba4e92`；本夹具测试源为 `9373088b75e7e5dde476593d7744a3e7a82ebe65d6551818a10ea6556a427e8c`；旧候选测试源为 `5c7141383f65cbbe65a5ebd6192eeb22f805012955f50144ef5309cee85f4999`；脚本为 `912cc03fced7c221faff9cb264c2b82c3bd36788f1ecc6f8cd7a576b5758eb2c`。
- 修改前头文件快照在 `/tmp/wt-i03-fee-context-before-20261103/FeeSchedule.hpp`，SHA-256 `ceac71d704d4330fd6dbeedcfe7abe41ce90f9d5edfb155783bcf57b4a7c78d8`。上述 `/tmp` 内容是本机临时证据，不保证长期保留。

## 边界

这些是头文件和独立夹具的局部结果，不证明回测或实盘消费者正确接入、真实成交身份与品种归属、账本异常传播、并发重载、其他平台、完整回归或真实 provider 行为。调用方仍须依据实际成交及合约元数据核验物理合约与品种关系。GoogleTest 静态库未做 UBSan 插桩，故 UBSan 结果不代表全部依赖已插桩。负费率的 `std::round` 按分舍入与旧实现的整数截断行为不同。
