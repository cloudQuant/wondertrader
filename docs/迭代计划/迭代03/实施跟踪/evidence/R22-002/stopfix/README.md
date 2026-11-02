# BUG-R22-002 StateMonitor 停止标志定向修复

源码范围：`src/WtDtCore/StateMonitor.h`、`StateMonitor.cpp`；新增定向测试 `src/TestUnits/coverage/q05_dtsrv/r44_state_monitor_lifecycle_test.cpp`。三个轮询读和 stop 写改为 atomic relaxed，只传递停止信号，线程结束同步由 join 保证；不保护 `_thrd`、状态表和依赖对象生命周期。

## TSan

复用旧 `/tmp/wt-i03-r22-focused/monitor_probe.cpp`，分别以旧版 `/tmp/wt-i03-r22-focused/monitor-tsan` 和新版 `/tmp/wt-i03-r22-stopfix/monitor-tsan` 运行。每次创建 10 个对象，分别 run、等待 25ms、stop/join；每次外置 20 秒 alarm，`TSAN_OPTIONS=halt_on_error=1:exitcode=66`。旧版三次 exit 134 且各有 StateMonitor::stop/run 数据竞争；新版十次 exit 0，日志及状态在 `runs/`。这不是并发 run/stop 或 callback 内 stop 的证明。

新版核心 TSan 构建：

```sh
/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -include stdlib.h -I/tmp/wt-i02-fresh-20261101-v8-build/TestUnits/coverage/cov_boost_include -isystem /usr/local/include -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I/opt/homebrew/include -isystem /Users/yunjinqi/Documents/new_projects/wondertrader/src -include /Users/yunjinqi/Documents/new_projects/wondertrader/src/Share/fmt_enum_compat.h -fPIC -g -std=gnu++17 -arch arm64 -Wall -Wextra -Wno-deprecated-copy-with-user-provided-copy '-Wno-#pragma-messages' -Werror -fsanitize=thread -fno-omit-frame-pointer -c /Users/yunjinqi/Documents/new_projects/wondertrader/src/WtDtCore/StateMonitor.cpp -o /tmp/wt-i03-r22-stopfix/monitor-tsan.o
/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -std=c++17 -g -fsanitize=thread -fno-omit-frame-pointer -include stdlib.h -I/tmp/wt-i02-fresh-20261101-v8-build/TestUnits/coverage/cov_boost_include -I/opt/homebrew/Cellar/fmt/11.2.0/include -I/Users/yunjinqi/Documents/new_projects/wondertrader/src -isystem /usr/local/include -include /Users/yunjinqi/Documents/new_projects/wondertrader/src/Share/fmt_enum_compat.h /tmp/wt-i03-r22-focused/monitor_probe.cpp /tmp/wt-i03-r22-stopfix/monitor-tsan.o -L/tmp/wt-i02-fresh-20261101-v8-build/build_x64/Debug/bin/WtDtPorter -L/opt/homebrew/lib -Wl,-rpath,/tmp/wt-i02-fresh-20261101-v8-build/build_x64/Debug/bin/WtDtPorter -Wl,-rpath,/tmp/wt-i02-fresh-20261101-v8-build/build_x64/Debug/bin -Wl,-rpath,/opt/homebrew/lib -lWtDtPorter -lboost_thread -o /tmp/wt-i03-r22-stopfix/monitor-tsan
```

TSan 下额外编译 `r44_state_monitor_lifecycle_test.cpp`、当前 `monitor-tsan.o` 和预编 GTest，定向运行 `R22StateMonitorStop.RunningWorkerObservesStopAcrossThreads` 和 `R44StateMonitorLifecycleTest.StopBeforeRunIsSafeAndIdempotent` 各 1/1 PASS，XML 在 `gtest-run/`；预编 WtDtPorter、GTest 及外部库未被 TSan 插桩。

## C API 启动正控

`capi-commands.json` 记录重编当前 WtDtRunner/WtDtPorter、两个 R22 测试 TU、StateMonitor 源码并重链的六步命令、退出码及编译日志；其余对象和库复用旧 v8。复制真实文件为 `CoverageTests` 供内部子进程执行，不创建符号链接。`CapiStartHonorsConfiguredAlldayModeInIsolatedProcesses` 和 allday/non-allday 两项 runner 初始化正控共 3/3 PASS，XML 和输出在 `capi-run/`。writer 是刻意不存在的 fake 场景，不证明真实 writer 成功启动或供应商行为。

## 限制

仅 macOS arm64 局部编译；其他依赖为 v8 未插桩构建，未做 Windows 或当前树整体回归。并发 run/stop、重复 stop、工作线程内 stop 导致 self-join、多个 monitor 共用 static lastTime、外部线程读取 StateInfo 状态、析构和 provider drain 不在本次修复/验证范围。修改原子成员可能要求所有使用 StateMonitor 布局的组件重新编译，跨版本二进制 ABI 兼容性未验证。
