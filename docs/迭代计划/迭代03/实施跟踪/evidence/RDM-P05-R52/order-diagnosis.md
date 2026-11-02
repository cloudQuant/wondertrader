# R52 ASan container-overflow 混链诊断（2026-11-01）

## 可重复的 A/B 对照

基于先前最小链接，不修改源码、库或 ASan 选项，只额外加入**同一个** `/tmp/wt-i02-fresh-20261101-v8-build/TestUnits/coverage/CMakeFiles/CoverageTests.dir/l05_dt_servo_porter/l05_dt_helper_bar_test.cpp.o`（旧、未插桩），比较它位于三个新插桩对象之前或之后。构建命令和顺序在 `order-link-commands.json`，二进制和对象指纹在 `order-sha256.txt`；`order-run.py`、`order-results.json` 记录四个单例的命令和 exit。ASan 选项均为 `abort_on_error=1:detect_leaks=0`，未关闭 container-overflow；UBSan `print_stacktrace=1:halt_on_error=1`。

| 对照 | range 边界用例 | count 边界用例 |
|---|---:|---:|
| 旧对象先链接 | -6 / ASan container-overflow | -6 / ASan container-overflow |
| 新对象先链接 | 0 / PASS | 0 / PASS |

`order-runs/` 保存四次独立运行的输出，两个通过项有 XML。旧对象靠前时堆栈显示 R52 测试体初始化一个 `vector<WTSBarStruct>`，新 ABI 初始化分配后出现对处于 container-poison 状态的 88 字节区域的写入，旧 `__construct_at_end` 内部调用旧 `allocator_traits::construct[abi:ne200100]`，与之前完整混链的失败堆栈相同。它发生在该次对应 vector 构造，而不能从这次栈单独归因 reader 代码。

## 被选择的弱符号和容器标注

两对象都提供完全相同、**不带 ABI 标签**的弱外部符号 `__ZNSt3__16vectorIN3wtp12WTSBarStructENS_9allocatorIS2_EEE18__construct_at_endIPKS2_S8_EEvT_T0_m`。链接 map `map-old_first.txt` 中该符号所属 `[1]` 为 l05 旧对象；`map-new_first.txt` 中该符号所属 `[2]` 为新的 R52 测试对象。`symbol-providers.txt` 列出旧全量对象中 5 个同名提供者。`old-first-disassembly.txt` 和 `new-first-disassembly.txt` 展示选中的各自实现；旧函数调用 `_ConstructTransaction ... ne200100`，新函数调用 `_ConstructTransaction ... nqe220108`。对其构造体 C2 的反汇编 `old-transaction2.txt`、`new-transaction2.txt` 显示新实现调用 `__annotate_increase`，旧实现无此调用。这个链接顺序导致的旧构造流程未解毒新实现设置的容器区域，与 ASan 对被 poison 区域写入的报告一致。

环境当前 `/usr/bin/clang++` 是 Apple clang 17，其标准库预处理值 `_LIBCPP_VERSION 200100`；Homebrew LLVM 22.1.8 的值为 `220108`。旧 l05 对象 `nm -m` 不含 `__asan_` 或 contiguous-container annotation 引用；新 R52 对象含 ASan 及 `__sanitizer_annotate_contiguous_container` 引用。两版本模板定义发生弱符号合并，混合插桩/头版本和对象顺序决定被选择的实现。

上述 A/B 与链接图证明**这次复现的直接触发依赖混合对象和顺序**；它不证明当前 reader 所有路径正确，也不证明全量/同一工具链、全插桩配置不会存在其他缺陷。应使用一致的编译器、libc++ 头和 sanitizer 选项重建关联对象后再进行严格 sanitizer 验证，保留此轮失败原始记录；不可关闭 ASan container-overflow 来宣布通过。P05-007 的旧尾碎片合同、R52-002 借用 slice 生命周期、LSan、Windows 和最终整合全量回归仍未验证。
