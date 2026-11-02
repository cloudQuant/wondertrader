# BUG-P05-007 / BUG-R52-002 — 本机定向复验

仅在 /tmp 构建及运行，未修改仓库或 v8 构建；没有创建符号链接。Python 均通过 `/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python` 运行。`build.py` 从只读 v8 Ninja 命令提取构建选项，将生产 `WtRdmDtReader.cpp`、P05 的 `q03_rdm_kline_file_test.cpp`、R52 的 `r52_rdm_hot_adj_test.cpp` 指向当前工作区并单独编译为 ASan+UBSan 对象，调用 `/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++`，flags 包括 `-fsanitize=address,undefined -fno-omit-frame-pointer`。实体 Boost 头优先来自 `/tmp/wt-i02-fresh-20261101-v8-build/TestUnits/coverage/cov_boost_include`（boost 目录不是符号链接）。GTest 及其余静态/动态依赖使用 v8 未插桩对象及库；命令数组 `commands.json`、三份源码及第一二进制哈希 `sha256.json`。链接命令因 LLVM 22 链接器不支持 `-Wl,-no_warn_duplicate_libraries` 移除了该标志及 `-Wl,-fatal_warnings`；构建输出 `build-*.log`。编译后重新核对三个源码哈希，未发现漂移。

最小链接将上述三个新对象与现有 WtDataReader 对象及库链接，具体 `min-link.json`/`min-link.log`；可执行文件 SHA-256 `db1a28848e06e84c5c4efcaa24a97cff0e83d9f82d892f1e7d873f3d497e8b05`。`run-min.py` 为每例启动独立进程，超时 30 秒，`ASAN_OPTIONS=abort_on_error=1:detect_leaks=0`，`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`。未禁用 ASan container-overflow 检查。`min-results.json` 对应 `min-runs/0..6` 日志和 XML，7/7 PASS，exit 0，无 skip：

- P05-007：日线单 bar 的 range/count 首前、命中、首后；首前空窗口；正常原始日线字段对照，3/3。
- R52-002：后复权 RT range 单/多条、空块、首前/命中/末后，以及 count 首前/命中/末后，4/4。

另一混合链接把 v8 主测试目标的所有其他未插桩测试对象与相同三个 ASan 对象链接（`commands.json` 的最后一项、`CoverageTests_san`），7 项中 5 PASS、2 exit -6。失败为 R52 的 range/count 边界用例，日志 `runs/5/output.log`、`runs/6/output.log` 显示测试体构造 `std::vector<WTSBarStruct>` 过程中 ASan container-overflow；错误根因尚未确定。最小链接 7/7 PASS 不能将混合链接的这两次失败算作通过，也不能证明完整目标或所有依赖的内存安全；禁止据此将该诊断简单判为产品缺陷或误报。

其他限制：LSan 关闭，其余 v8 对象/库和外部库未全部插桩；历史日线尾碎片合同（NEW-032）、缓存缩容/刷新以及借用 slice 保活未覆盖；Windows、其他平台和最终当前树完整构建及整体回归未运行。无修复前红控重跑。源码在编译后核对与工作区相同，但其余依赖是旧快照，不能声称完整工作树一致。
