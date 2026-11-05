# R22-002：StateMonitor 生命周期、状态查询和异常子项

2026-11-02，macOS arm64，当前工作树上的局部修复；本证据不关闭整项。线程创建和外部 join 由互斥串行，重复 run 不创建线程、重复 stop 不再 join 已 join 线程；工作线程内 stop 只置停止标志，由外部 owner 后续 stop/join。每个工作线程拥有自己的 `lastTime`。状态表和状态转换读写使用短锁；`advanceOnce` 持有 shared_ptr 快照，不持锁调用基础数据管理器、logger、DataManager 或 writer，避免同步回调重入查询时死锁。

后台线程遇到异常时捕获并记录 stderr，标记 `hasFailed()`，请求停止且不重新运行；普通状态谓词在失败后均返回 false，`tryGetRecordedState()` 只提供最后记录供诊断。`DataManager::canSessionReceive` 是仓内唯一外部状态谓词使用者，查询 `SS_RECEIVING`，失败后会拒绝接收；其他谓词记录的历史状态仍可通过诊断接口查看，不证明处理完成。仓内 `isAllInState(PROCING/Holiday)` 仅用于 worker 的正常循环，异常后线程退出。**没有即时宿主通知，不能撤销 writer 已发生的副作用，也不能证明外部二进制调用者、其他接收路径或真实 provider 的行为。**

## 控制和复现

`verify.py` 从 `/tmp/wt-r22-lifecycle-20261102/frozen/{red,green}` 的实体副本编译 StateMonitor.cpp、DataManager.cpp 和 `src/TestUnits/fixtures/r22_monitor_lifecycle/probe.cpp`，其余库复用 `/tmp/wt-i03-start-cmake/build_x64/Debug/libs`，只对三个源码 TU 插桩，不是整树干净构建。`baseline/` 保存本轮修改前的 StateMonitor 源和头；red 使用该版本并使用同一版冻结依赖。green 冻结的 `TimeUtils.hpp` SHA256 为 `63fe5f708b9d156606e88bead9158fee451a06f175f20dcf500672e66d41be54`，与本轮共享修复最终源码一致。`hashes.json`、`verify-results.json`、`*-build.log` 和逐场景日志记录源码/二进制哈希、命令、退出码；Boost 1.88、fmt、spdlog 均为实体目录。调用：

```
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python docs/迭代计划/迭代03/实施跟踪/evidence/R22-002/lifecycle-20261102/verify.py
```

最终 green 的 ASan/UBSan **8/8**、TSan **8/8** 分别通过：重复 stop、并发 stop、并发 run/stop、双实例、自回调 stop、外部 stop 已持锁等待 join 时回调内 stop、状态并发查询及 writer 抛异常。异常用例验证调用计数为 1、失败可见、不再次运行、原 PROCING 值仅可经诊断接口读取、普通谓词不会报告 RECEIVING/PROCED。同步 writer 查询在真实 `DataManager::transHisData` 到 fake `IDataWriter::transHisData` 调用栈内完成。最终实现前，异常控制在 ASan/TSan 中均退出 -6；旧版重复/并发/自 join 触发终止，TSan 给出并发线程指针、共享 `lastTime` 和状态转换/查询竞争。每项各进程执行并设 12 秒上限；ASan 关闭 LSan。

六个并发/多实例/重入/状态/异常场景在 ASan/UBSan 与 TSan 各额外重复五次，**60/60** 进程退出 0，详见 `final2-repeat-results.json`。这些重复在最终仅修改文档注释前运行；最终同代码的八个单例已在注释后重编。系统 libc++、GTest、预编库未插桩；TSan 通过不证明所有依赖无竞争。

此前 `green-asan-queries.log` 等保留了一次错误链接：新 StateMonitor 布局与预编旧 DataManager 的内联 `isInState` 混链，ASan/TSan 发生地址错误；重编 DataManager 后消失。此反例说明所有使用 StateMonitor 布局或内联方法的 TU 必须与新头共同重编，不能将新对象与旧对象混用。初次旧版多实例 TSan 的首报是旧 `TimeUtils::getTZOffset` 竞争，保存在 `red-tsan-multiple.log`；仅为定位下一处而抑制该竞争后，`red-tsan-multiple-suppressed.log` 报告 `lastTime` 竞争。最终上述 8/8 TSan 使用修复后的冻结 TimeUtils、未设 suppressions。

隔离配置并编译 `CoverageTests_i03_dtporter_start` 及依赖成功：

```
/opt/homebrew/bin/cmake -S src -B /tmp/wt-r22-lifecycle-20261102/cmake -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DWT_ENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
/opt/homebrew/bin/cmake --build /tmp/wt-r22-lifecycle-20261102/cmake --target CoverageTests_i03_dtporter_start --parallel 4
```

独立 ASan/UBSan 重编三个 R154 状态流转相关测试，`r154-final-run.log` 为 **3/3**。首次遗漏 Share include 的失败 `r154-build.log` 保留，补齐后成功。误运行该隔离目标中的全部 6 个 `I03DtPorterStart` parser 测试，6/6 通过，但其中有非 R22 专属项；这一运行超出当前集中修复阶段仅运行关联定向用例的约束，不能算整体回归。

## 未闭合

`WtDtRunner` 的成员布局使 `DataManager` 在 `StateMonitor` 之前析构，monitor 析构才 stop/join 太迟；当前改动没有替 owner 改写销毁顺序。回调线程自销毁、并发 initialize、多个 monitor 跨对象循环等待、阻塞/异步 provider callback 的排空与 owner 生存期仍未验证。具体类布局改变，依赖须重编，跨版本 ABI 未证。`TimeUtils::getTZOffset()` 返回 99 时日期调用仍会当小时偏移使用，初始化与运行失败传播另待处理。真实 writer/柜台、Windows/Linux、LSan 与最终整合树/全量回归均未验收。
