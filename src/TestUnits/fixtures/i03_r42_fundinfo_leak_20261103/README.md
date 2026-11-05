# BUG-R42-001 子项：WtEngine 资金对象 WTSPortFundInfo 泄漏（NEW-034 关联）

2026-11-03，macOS arm64，Homebrew LLVM 22.1.8 ASan/UBSan + LSan（`detect_leaks=1`）。基础 HEAD `ccaad010e9169f610aded4d8dc0f5647f70a5cff`，工作区含其他迭代未提交修改。**仅为本子项的定向红绿，不是整项验收或整体回归。**

## 归因

`WtEngine::load_datas()` 每次调用 `WTSPortFundInfo::create()` 并直接覆盖 `_port_fund`；`WtEngine` 没有析构函数，引擎从不释放这一引用。故：

1. 正常 `init()` 后析构：144 字节资金对象泄漏（LSan 直接泄漏，栈 `WTSPortFundInfo::create → WtEngine::load_datas → WtEngine::init`）。
2. 重复 `load_datas()`：旧对象被覆盖再泄漏一份，且已借出的 `getFundInfo()` 指针与引擎当前对象不一致。
3. 引擎析构时未停止其风险监控器；监控线程通过 `WtPortContext::getFundInfo()` 借用资金对象。

## 修复（生产代码）

- `src/WtCore/WtEngine.h`：仅新增非虚析构声明 `~WtEngine();`（基类无虚析构，未改变 vtable/成员布局；`WtEngine` 不是插件 ABI 类型）。
- `src/WtCore/WtEngine.cpp`：
  - `~WtEngine()`：先 `WtEngine::release()` 排空 task worker（幂等），再对持有的风险监控器调用 `stop()`，之后才释放 `_port_fund`；若析构发生在本引擎的 task worker 线程（无法 join 自身），保留该对象不释放（有界，一份）而不冒 UAF 风险。
  - `load_datas()`：仅在 `_port_fund` 为空时创建；重复加载时原地重置内容（`WTSFundStruct()`），保持已借出指针有效。

`baseline/` 为修改前工作区的 `WtEngine.cpp/.h` 及 `SHA256SUMS`。

## 运行

```sh
bash src/TestUnits/fixtures/i03_r42_fundinfo_leak_20261103/run.sh /tmp/wt-i03-r42-leak-20261103 red
bash src/TestUnits/fixtures/i03_r42_fundinfo_leak_20261103/run.sh /tmp/wt-i03-r42-leak-20261103 green
```

脚本在同一实体快照（rsync 复制 `src/`，无符号链接）中只切换 `WtEngine.cpp/.h`，以 `-j2` 构建 `WtCore WTSTools WTSUtils`，每个用例独立进程、120 秒上限。配置使用 `-DWT_WARNINGS_AS_ERRORS=OFF`：首次构建因其他并行修改中的 `WTSDataFactory.cpp` 未用参数被 `-Werror` 拒绝（`evidence/build-red-attempt1-werror.log`），属本地绕行；`WtEngine.cpp/.h` 自身在 `-Wall -Wextra` 下无警告（`evidence/*-own-warnings.txt` 为空）。首次链接因 `-lnanomsg` 不可解析、次次因系统 `-liconv` 缺 `libiconv_*` 失败，改为不链 nanomsg、直接链接 `/usr/local/lib/libiconv.2.dylib` 后成功，红绿使用同一脚本最终版本。

## 结果（`evidence/red-results.txt` / `green-results.txt`，各用例 `.log/.xml`）

| 用例 | 红（修改前）| 绿（修改后）|
|---|---|---|
| DestroyWithoutInitIsSafe | exit 0 | exit 0 |
| InitThenDestroyLeavesNoFundAllocation | exit 1：断言通过但 LSan 报 144 字节 `load_datas` 泄漏 | exit 0，无 LSan 报告 |
| EngineReleasesItsOwnReferenceOnDestruction | exit 1：析构后引用计数 2（期望 1）+ 泄漏 | exit 0 |
| RepeatedLoadKeepsTheLentFundObject | exit 1：重载后指针变化 + 两份 144 字节泄漏 | exit 0 |
| RiskMonitorIsStoppedBeforeFundRelease | exit 1：`stop()` 0 次、析构后计数 2 + 泄漏 | exit 0：`stop()` 1 次，停止时引擎引用仍在（计数 2），析构后 1 |

绿色二进制 sha256 见 `evidence/green-binary.sha256`；输入快照哈希见 `evidence/*-inputs.sha256`（绿色的 `WtEngine.cpp` = `a3eeae45…`，`WtEngine.h` = `2bb56129…`）。所有绿色日志中无 `leaked`、`ERROR: AddressSanitizer` 或 `runtime error`。系统库、Boost、GTest 预编依赖未插桩。

## 测试全局 72 字节报告的定性（静态分析 + 本次未复现）

历史迭代02 日志 `docs/迭代计划/迭代02/实施跟踪/evidence/20261031-r42-llvm22/r42-strict.log` 中的 72 字节直接泄漏栈为 `std::string::operator= ← WtHelper::setGenerateDir ← P02DiffGenDirGuard::P02DiffGenDirGuard ← __cxx_global_var_init (p02_diff_executer_test.cpp)`：测试文件的**命名空间级静态对象**在 `WtHelper::_gen_dir`（`WtHelper.cpp` 中带字面量初值的 `static std::string`，属动态初始化）构造之前对其赋值；随后 `_gen_dir` 的初始化再次构造该字符串，覆盖已分配的 72 字节缓冲区。这是**测试侧静态初始化顺序问题**（同时会使该 guard 设置的目录被重置为 `./generated/`），不是 `load_datas` 的产品泄漏。当前工作区 `p02_diff_executer_test.cpp` 中已无 `P02DiffGenDirGuard`（静态检查，未运行 `CoverageTests_wtcore` 全体验证）；仍有 `r101_sel_ctx_gap_test.cpp` 通过 `AddGlobalTestEnvironment` 在 `SetUp()` 而非静态初始化时设置目录，不触发该顺序问题。本 fixture 在 `main()` 中设置生成目录，红绿两次运行均未出现 72 字节报告——这只说明本独立二进制无该问题，**不能证明聚合 `CoverageTests_wtcore` 已无此报告**，该报告未被压制或隐藏。

## 未验证/限制

- MSan：本机 Darwin arm64 的 LLVM 不支持 `-fsanitize=memory`，无 Valgrind，未初始化读取动态检测仍为 **阻塞（工具链）**。
- 第三方风险监控插件的 `stop()` 是否真正排空未知；若在其自身回调线程中析构引擎，`WtSimpleRiskMon::stop()` 只置标志不 join，此时资金对象仍会被释放（引擎同时也被析构，原本即属 UAF）。外部共享 wrapper 在引擎析构后继续使用 ctx 的情况不受支持。
- `q01_wtcore3/r130_misc_tail_test.cpp` 的 `CtaRunStartsRiskMonitor` 中 `R130RiskMon mon` 与工厂声明在 `engine` 之后、先于引擎析构；原先在 wrapper 析构时已有对已销毁对象的调用，本修改在 `~WtEngine` 中新增 `stop()` 调用同样作用于已销毁对象。该用例未在本轮运行，需该 lane 调整声明顺序后复核。
- 只运行本 fixture 五个定向用例；未运行 `CoverageTests_wtcore` 或整体回归（`NOT_RUN`），未验证 Windows/MSVC、真实风险插件或真实交易。
