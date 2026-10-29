# BUG-L00-005 最终测试目标 provider 核对

日期：2026-10-29。只读检查生成的链接命令、`dyld_info -imports`、`nm -g -C` 与产品/测试 CMake。未在本子任务构建或运行测试。旧二进制的错绑是在主代理重链前观察到；下列“当前”均指 11:46–11:48 刷新后的目标。

## 已确认并由目标隔离修正

- **`CoverageTests_m_WtRunner`（旧错绑，当前已修）**：该目标直接编译 `WtRunner.cpp`；`src/WtRunner/WtRunner.h:17,178` 指定并按值持有 WtCore 的 `EventNotifier`。旧二进制把 `EventNotifier::{init,ctor,dtor}` 绑定到 `libWtBtPorter`。两版不是同一 ABI：Core 的创建函数指针只有 `const char*` 参数、对象含 asio/worker 状态（`src/WtCore/EventNotifier.h:18,58-67`）；BtCore 版本多一个 `bool` 参数且对象无这些状态（`src/WtBtCore/EventNotifier.h:24,110-117`），其 `init` 也以双参数调用（`src/WtBtCore/EventNotifier.cpp:109-110`）。当前目标链接行只含 `libWtCore.a`；`nm` 显示 Core `EventNotifier` 方法为本地 `T`，`dyld_info` 不再导入这些方法。当前专属链接分支见 `src/TestUnits/coverage/CMakeLists.txt:447-464`。
- **`CoverageTests_porter`（旧错绑，当前已修）**：目标源码含 `WtPorter/WtRtRunner.cpp`，其 runner 使用 WtCore notifier；旧二进制把 notifier 绑定到 `libWtBtPorter`。当前链接依赖移除 `WtBtPorter`（`src/TestUnits/coverage/CMakeLists.txt:278-285`），最新导入来自 `libWtExecMon`；`src/WtExecMon/CMakeLists.txt:23` 指向 WtCore，匹配 `src/WtPorter/CMakeLists.txt:22`。对应运行记录/旧二进制信息见 `l00-porter/`。
- **`CoverageTests_m_WtLatencyUFT`（旧错绑，当前已修）**：旧二进制将 `ActionPolicyMgr::getActionRules` 从 UFT 归档未定义符号导入 `libWtExecMon`，尽管 `src/WtLatencyUFT/CMakeLists.txt:20-26` 使用 `WtUftCore`。当前专属链接行只含 `libWtUftCore.a`；`nm` 显示 `ActionPolicyMgr` 方法为本地 `T`，相关 dyld 导入消失。当前分支见 `src/TestUnits/coverage/CMakeLists.txt:484-500`。这是测试链接污染，未据此认定生产行为错误。
- **`CoverageTests_m_WtUftRunner`**：当前只链接 `WtUftCore` 和工具库，`dyld_info` 未见 ActionPolicy/EventNotifier 从 Core 家族动态库导入；父代理保存的刷新证据在 `l00-after/`。

## 已查且 provider 与消费家族一致

- **`CoverageTests_m_WtLatencyHFT`**：`ActionPolicyMgr` 方法从 `libWtExecMon` 导入；`src/WtLatencyHFT/CMakeLists.txt:22` 与 `src/WtExecMon/CMakeLists.txt:23` 都选 WtCore。当前目标没有 EventNotifier 方法导入。`WtHftEngine::init` 从 `libWtPorter` 导入，该库也以 WtCore 为依赖。
- **`CoverageTests_btporter`**：EventNotifier 从 `libWtBtPorter` 导入，与 `src/WtBtPorter/CMakeLists.txt:22-31` 的 WtBtCore 依赖匹配。
- 对 `CoverageTests_wtcore/uftcore/btcore/dtcore` 仅筛查了 EventNotifier/ActionPolicyMgr 的动态导入，未观察到匹配项。该过滤结果不能证明其它重复类或静态抽取均安全。

## 未完成范围

没有完成 37 个同名类与所有最终 CoverageTests 目标的成员级链接映射，也没有为所有静态归档对象提供 link map/来源映射。归档有同名符号、或某目标链接过多个核心，都不单独证明错误成员已抽取。故整体 BUG-L00-005 保持本地未完成；后续需逐目标核对其实际符号提供者/来源，范围留给主代理统一安排。本次证据只支持上述已检查的类和目标，不外推到未检查项。

## 复核命令

只读命令：

```sh
/opt/homebrew/bin/ninja -C build-cov -t commands CoverageTests_m_WtRunner
/opt/homebrew/bin/ninja -C build-cov -t commands CoverageTests_m_WtLatencyUFT
/opt/homebrew/bin/ninja -C build-cov -t commands CoverageTests_m_WtLatencyHFT
/opt/homebrew/bin/ninja -C build-cov -t commands CoverageTests_porter
dyld_info -imports build-cov/build_x64/Debug/bin/TestUnits/<target>
xcrun nm -g -C build-cov/build_x64/Debug/bin/TestUnits/<target>
```

