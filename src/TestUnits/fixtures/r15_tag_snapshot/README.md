# R15-001：HFT 标签指针与并发探针

本目录是定向诊断，**未修改生产代码，不能计作修复或验收通过**。macOS arm64，本轮编译当前 `HftStraBaseCtx.cpp`、`HftStraContext.cpp`、`WtHftEngine.cpp` 和测试 TU；链接复用 `/tmp/wt-r14-resume` 的旧静态库及动态库，既非当前整合树，也不是全依赖插桩。源码哈希见 `evidence-20261102/source.sha256`。

当前 `getOrderTag()` 返回 `_orders` 中字符串的内部指针。`HftStraContext::on_order()` 将其直接交给策略回调；测试派生类在受控策略回调内调用 protected 的 erase 或以更长字符串替换同一 ID 后，再读取回调参数。两个场景均触发 ASan heap-use-after-free，进程 exit 134。没有修改标签的对照用例通过（exit 0）。专用双线程测试对同一 ID 调用 `setUserTag`/`eraseOrderTag` 和 `getOrderTag`，TSan 报告 map 读写竞争，进程 exit 134。原始输出分别为 `asan-reentrant-erase.log`、`asan-reentrant-replace.log`、`asan-control.log`、`tsan-concurrent.log`。

回调中的删除/替换由测试派生对象白盒触发；仓库当前生产代码并未证明普通策略可以直接调用 protected 的删除方法，重用 ID 或真实 provider 并发时序也未经本测试证实。TSan 使用受控测试线程，不证明具体柜台必然并发；GoogleTest、旧依赖和系统库并非全部插桩。ASan 关闭 leak detection，不对泄漏作结论。原始失败是既有缺陷探针，不是通过。

复现需本机 Homebrew LLVM、Boost、GoogleTest、脚本列出的旧构建与库存在，在仓库根目录运行，输出目录必须不存在：

```sh
zsh src/TestUnits/fixtures/r15_tag_snapshot/run.sh /tmp/r15-snapshot-asan-new asan
zsh src/TestUnits/fixtures/r15_tag_snapshot/run.sh /tmp/r15-snapshot-tsan-new tsan
```

两条命令按上述已知红控预期以 134 退出。ASan 可复用编出的测试程序，在独立目录运行不修改标签的正控和替换红控：

```sh
mkdir /tmp/r15-snapshot-control-new
cd /tmp/r15-snapshot-control-new
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 /tmp/r15-snapshot-asan-new/tests --gtest_filter=R15CtxTest.order_callback_reads_tag_without_reentrant_mutation
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 /tmp/r15-snapshot-asan-new/tests --gtest_filter=R15CtxTest.tag_snapshot_survives_reentrant_replacement_during_order_callback
```

安全修复需要明确回调字符串允许保留的时长、标签读取与写入的线程/重入协议，以及同一 localid 复用和终态后迟到成交的处理。`getOrderTag` 是 protected 内联接口，`WtPorter::ExpHftContext` 也使用它，外部派生使用者未知；单改签名、修改类布局或仅为某一回调传临时字符串无法证明 ABI 与生命周期兼容。同步报单回报先于标签登记、终态回收和内存上界也未解决。
