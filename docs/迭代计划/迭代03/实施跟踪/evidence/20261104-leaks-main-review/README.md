# 2026-11-04 主线程复核：I03-NEW-015 / I03-NEW-016 / BUG-T6-009

- 源码哈希（复跑开始时）：`src-hashes.txt`（TraderFemas.cpp 5c90ac11…、TraderMocker.h 30c5d9c2…、TraderMocker.cpp dc790cc5…、coverage CMakeLists 73f6c5a9…）。
- I03-NEW-015：直接运行实现方构建的 `/tmp/wt-i03-leaks-20261104/build/.../CoverageTests --gtest_list_tests`，exit 0，列出 1320 例（`coverage-main-list.txt`）；核对 CMake 差异为按自带 CMakeLists.txt 的子目录整目录排除。未独立重编主目标。
- I03-NEW-016 / T6-009：以 `manual_build.sh fix asan femas`、`fix asan mocker`、`fix tsan mocker`（WT_I03_LEAKS_ROOT=/tmp/wt-i03-main-leaks-20261104）独立重编，三次 exit 0；按实现方用例清单逐例运行并核对 XML：Femas ASan/UBSan/LSan 7 例各 1/1、整组 42/42；Mocker ASan 与 TSan 各 9 例 1/1（`results-*.json`、`main-rerun-summary.txt`）。
- 两个 Mocker “ALL” 组 exit -6：ASan 报 SEGV（pc=0）于 `t6_init.wtcore_init_ext_rejects_tradermocker_null_config`，该例依赖 WtCore 符号而手工构建未链接 WtCore（实现方 lane 运行同样排除），不计产品失败，但该例在本构建中未验证（`mocker-fix-asan-ALL-output.log`）。
- **证据覆盖说明（主线程失误）**：`run_cases.py` 硬编码根目录 `/tmp/wt-i03-leaks-20261104`。主线程第一次复跑因 zsh 未分词导致构建未执行、二进制不存在，`perl exec` 失败后仍 exit 0；该次及第二次复跑的逐例 cwd/output.log/result.xml 写入了实现方的 `/tmp/wt-i03-leaks-20261104/run/{femas,mocker}-fix-*`，覆盖了实现方对应逐例输出。实现方 `logs/results-*.json`、`mb-*.log`、base 变体及 lane 运行目录未受影响。第一次复跑结果无效，不计。
- 夹具缺陷：`run_cases.py` 在二进制缺失时报告 exit 0，应以 XML 存在与否判定，已在本复核中补查 XML。
