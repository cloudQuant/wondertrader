# Exported API 测试隔离修复

原四个 Exported 测试分别单进程 PASS，但作为同进程组运行时共享 `getRunner()` 的静态对象，首项初始化会使后续 initialize 早退。旧二进制 `perl -e 'alarm 8; exec @ARGV' -- /tmp/wt-i03-servo-atomic/tests '--gtest_filter=R45ParserAdapterFakeTest.Exported*' --gtest_output=xml:/tmp/wt-i03-servo-group-red/result.xml`：首项通过，成功提交及同步重入两项断言失败，最后并发项等待屏障，8 秒 alarm 终止（退出 142）。见 red/。此前 9/9 仅是各项独立进程，不证明合组可靠。

修复后四个场景各在独立 fork 子进程执行真实 C 导出、原有断言及并发屏障。子进程 10 秒 alarm，父进程 12 秒 watchdog 杀死并回收异常未退出子进程，信号或非零退出使测试失败；子进程 gtest 断言失败转成退出 1。新增阻塞子进程负控，在 1 秒检查 SIGALRM，证明限时生效。父进程仅用于等待各子进程，不初始化 C 导出单例。

使用 `/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python /tmp/wt_i03_servo_group_final_build.py` 编译当前相关源和实体 fake；依然复用其他既有构建依赖。按 `green/commands.json` 在一个进程中运行过滤组合 `R45ParserAdapterFakeTest.Exported*:R45ParserAdapterFakeTest.ManagerReportsPartialConnectSubmission:R45ParserAdapterFakeTest.ManagerRunsAndReleasesLiveAdapters:R45ParserAdapterFakeTest.RunReportsConnectionCommandSubmission`，外层 alarm 40 秒，8/8 PASS、0 skip、退出 0。包含 4 个导出场景、超时负控和 3 个关联 adapter/manager 正反控。green/result.xml 与 output.log 留存。

新增 fork/wait 及信号测试仅 POSIX 实现，Windows 分支显式 GTEST_SKIP，不计通过；此测试文件原有无条件 dlfcn/unistd/dlopen 依赖，Windows 编译问题并未因本次宏保护解决。Windows、TSan、真实 SDK 和整体回归仍未验证。
