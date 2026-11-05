# i03_leaks_main_20261104

迭代03 I03-NEW-015 / I03-NEW-016 / BUG-T6-009（含 BUG-I02-NEW-019、BUG-T4-002、BUG-I02-NEW-020）定向复现脚本。仅 macOS arm64 本机验证；不调用真实厂商 API，不覆盖 Windows、真实 Femas SDK 或整体回归。

- `manual_build.sh <base|fix> <normal|asan|tsan> <femas|mocker>`：只编产品 TU（`-Wall -Wextra -Werror`）+ 对应 lane 测试 TU，链接 Homebrew gtest_main。`base` 使用 `$WT_I03_LEAKS_ROOT/baseline/src` 下的改前源码副本（Femas/TraderMocker.h 取自 HEAD；TraderMocker.cpp 为含并行未提交修改的改前字节，sha256 `e45e4f0c…71db`）。asan 为 LLVM 22 `-fsanitize=address,undefined`，tsan 为 `-fsanitize=thread`。实体 Boost 副本放在搜索首位，不使用符号链接。
- `run_cases.py <cases.json> <results.json>`：每例单独进程、独立 cwd/TMPDIR，记录 argv、退出码与 XML 计数。`cases-*.json` 为本次用例清单（ASan 统一 `detect_leaks=1`、`LSAN_OPTIONS=exitcode=23`，无 suppression）。
- `red_main_relink.sh <构建目录> <输出目录>`：I03-NEW-015 红控，把 `r20_mq_fixture` 的 `notifier_check.cpp`/`fake_mq.cpp` 按主目标参数编译后重链 CoverageTests，复现旧 GLOB_RECURSE 行为（`main` 来自 `notifier_check.cpp:17`，exit 2 无输出）。

手工链接使用 `-Wl,-undefined,dynamic_lookup` 且不链 WtCore，因此 `t6_init.wtcore_init_ext_rejects_tradermocker_null_config` 在此构建中无效（NULL 调用），需在完整 coverage 目标中运行。
