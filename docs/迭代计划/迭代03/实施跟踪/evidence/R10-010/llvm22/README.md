# BUG-R10-010 日志边界定向复验

## LLVM22 局部 ASan

`WtArbiExecuter.cpp`、`r15_mgr_executers.cpp`、`r10_wtcore_executers.cpp` 从当前源码重新编译，实体 `cov_boost_include` 优先，LLVM 22 `-fsanitize=address -fno-omit-frame-pointer`；其他对象及库复用 `/tmp/wt-i02-fresh-20261101-v8-build`。编译 argv 见 `commands.json`，链接实际 argv 见 `link-retry.json`，编译及链接分别在旧构建目录执行。LLVM22 链接器不支持原 `-no_warn_duplicate_libraries` 选项；移除此参数后旧依赖有 DWARF 解析警告，故移除 `-fatal_warnings` 才能链接，警告完整保留在 `link-retry.log`。该混合工具链构建不是全依赖插桩，不能外推完整系统无内存问题。

两个单例在各自独立目录、有界 30 秒执行，`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`（容器检查未关闭），全部 exit 0、XML 各 1/1 PASS、无 skip：

- `R15MgrTest.arbi_write_log_bounds_and_null_are_observable`：子进程检查短日志、空指针、恰到边界、超一字节及长消息输出的精确截断。测试对象未 init、没有执行单元；实际析构调用 `requestQuiesce()` / `waitQuiesced()`，子进程有界执行。
- `R10ExecuterTest.arbi_write_log_and_set_trader_null`：短消息及空 trader 正控，使用同样未 init 的栈对象正常析构。

日志、XML 以用例名分目录保留。两个测试不覆盖已加载执行单元或交易供应商的关闭排空。构建输入和二进制哈希见 `inputs.json`；完成运行时源码与普通构建哈希一致。LSan、外部库插桩、Windows、当前全树及整体回归未验证。

## 交叉对照和失败尝试

Apple clang 局部 ASan 构建见 `/private/tmp/wt-i03-r10-010/`：两个测试各在 30 秒 alarm 终止、无测试输出；独立空 ASan 程序也在 10 秒 alarm 终止，不能当作产品通过或失败。LLVM22 空 ASan 程序 exit 0。LLVM22 首轮编译缺 stdlib 声明，补 `-include stdlib.h`；第二轮缺 RapidJSON，补 `-isystem /usr/local/include`；以上尝试均非成功证据，最后编译以 `commands.json` 和 `link-retry.json` 为准。

普通 Apple clang 的独立重编见 `/private/tmp/wt-i03-r10-010-normal/`，相同两个用例各 1/1 PASS。未创建符号链接。
