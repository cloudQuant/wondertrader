# BUG-R22-002 本机定向复验

当前 WtDtRunner.cpp、WtDtPorter.cpp、r22_porter_capi_test.cpp 和 r22_runner_init_test.cpp 以实体 Boost bridge 独立编译，其余静态、动态库及对象复用迭代02 v8；`commands.json` 保存逐条命令、退出码、日志路径。当前源和编译产物哈希见 `sha256.json`，运行后 `source-drift.json` 无漂移。测试可执行文件复制为真实普通文件 `CoverageTests` 以满足内部子进程定位，不使用符号链接。

三个顶层定向用例 3/3 PASS：C API 全天/非全天及同步/异步启动矩阵（父测试启动六个隔离子进程，包括 runner 直接启动正控）；两个 runner 初始化正控。日志与 XML 为 `run0` 至 `run2`；逐项命令与状态见 `results.json`。C API 用例使用不存在的 writer，检验状态机启动模式和启动模式透传，但不能证明真实 writer 成功启动。

附加局部 TSan 探针直接编译当前 StateMonitor.cpp 和最小 monitor_probe.cpp 到测试程序，支持库复用未插桩的 v8 libWtDtPorter；`tsan-commands.json` 为编译命令，`tsan-hashes.json` 保存源与二进制哈希。`nm -m` 验证测试程序本身定义 StateMonitor::run/stop。空状态表下运行真实 monitor.run()，等待 25ms，monitor.stop() 并 join；三次执行均报告 stop 写与 run 工作线程读同一字节的 data race（`tsan-run/output.log`, `output-symbolized.log`, `repeat.log`）；repeat 的机器可读结果显示 Python returncode -6/SIGABRT，源码无漂移。源码 `StateMonitor.cpp:553,564,572,601` 的普通 bool `_stopped` 与访问方向一致。

结论仅限 macOS 本机局部启动参数和 monitor 竞争：未作完整依赖的 TSan 插桩、成功 writer 链路、Windows/其他平台或整体回归。不能以普通定向通过关闭线程安全问题。
