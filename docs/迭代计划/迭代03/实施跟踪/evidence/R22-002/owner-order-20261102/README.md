# R22-002 owner 外部析构顺序定向复核

2026-11-02，macOS arm64。生产源码检索到直接拥有 StateMonitor 的两个对象：WtDtPorter::WtDtRunner 的值成员和 QuoteFactory 的进程全局对象。WtDtServo 的同名 runner 没有该成员；DataManager 只借用 StateMonitor。WtDtRunner 原先析构体为空，成员销毁时 DataManager 比 StateMonitor 先析构；QuoteFactory 原先全局定义顺序亦导致 DataManager 先析构，而 Windows 控制处理器另有直接 DataManager.release 路径。

本次变更：WtDtRunner 外部析构体在成员销毁前调用 `_state_mon.stop()`；QuoteFactory 在所有相关全局对象定义之后构造静态 guard，使进程静态析构首先 stop/join monitor，并在 Windows CTRL_CLOSE_EVENT 先 stop 后 release writer。未改 WtDtServo。`baseline/WtDtRunner.cpp` 和 `baseline/main.cpp` 保存本任务开始时的工作树文件（WtDtRunner 已有其他未提交内容），与当前文件直接 diff 只包含上述变更；没有覆盖既有工作。

## 定向测试

`runner_owner_probe.cpp` 使用真实 WtDtRunner、StateMonitor、DataManager 与内存 fake writer，先阻塞 writer callback，再从外部线程 delete runner。旧版在 callback 尚未结束时调用 writer.release，报告 `early=1 releaseBeforeDone=1`，exit 1；绿色候选则先等待 callback 和 monitor 完成后调用 release，exit 0。fixture 使旧版的 fake release 等待外部监控线程确认退出后才结束，避免测试人为触发真实 UAF。writer callback 本身还同步调用 monitor.stop，覆盖外部 stop/join 与回调内 stop 的重入。因为等待 200ms 观察提前 release，绿色结果还依赖调度，并非证明所有调度；旧版观察到的提前调用为确定反例。

`quote_owner_probe.cpp` 编译真实 QuoteFactory main TU（仅将 main 改名以供 fixture 驱动），按全局析构路径执行退出，活动 callback 仍被屏障阻塞时如果 writer.release 提前执行则打印 `EARLY_RELEASE` 并立即 _Exit(9)，避免制造 UAF；旧版 ASan 控制打印该标记并 exit 9。绿色的 guard 等到 callback 结束，打印 `RELEASE_AFTER_CALLBACK` 后 exit 0。TSan 旧版同样先输出 `EARLY_RELEASE`，随后 TSan 报告测试提前 _Exit 造成线程未 join，exit -6，不能把 -6 单独归因于产品数据竞争；绿色 TSan exit 0。两种 owner 的未启动空析构正控均 exit 0。fixture 不 detach 线程；QuoteFactory 测试的 `std::exit` 用于触发真实全局析构。callback 自行删除 owner 或在其内触发 std::exit 不属于本测试覆盖的合法调用。

每个 owner 的绿色 ASan/UBSan 和 TSan 活动 callback 测试各重复五次，共 20/20 exit 0，见 `repeat-results.json`。日志和退出码保存在 `runner-results.json`、`quote-results.json` 及逐项日志；命令可通过以下脚本复现，依赖 `/tmp/wt-r22-owner-20261102/{red,green}` 实体快照和此前隔离 CMake 库：

```
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python docs/迭代计划/迭代03/实施跟踪/evidence/R22-002/owner-order-20261102/verify_runner.py
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python docs/迭代计划/迭代03/实施跟踪/evidence/R22-002/owner-order-20261102/verify_quote.py
```

所编 TU 包括 owner、StateMonitor、DataManager、测试和 WtDtPorter 必要辅助源，其余 WtDtCore/WTSTools/WTSUtils 库来自此前隔离 CMake 构建；只对这批源码插桩，LSan 禁用。最初漏链接 WtDtPorter.cpp 导致 getRunner 未定义，诊断在临时构建日志中，补齐后通过。实体快照与 Boost 目录没有符号链接。`runner-sha256.json`、`final-hashes.txt` 列源码哈希；测试前冻结 `TimeUtils.hpp` 为 `63fe5f708b9d156606e88bead9158fee451a06f175f20dcf500672e66d41be54`。现工作树该文件由其他代理继续修改为 `3b603ba7...`，测试快照因此不等同当前整合树；两个 owner 文件和 monitor 源与冻结一致。

## 未闭合

此补丁只保证外部线程在不并发操作 owner 的条件下等待监控线程。监控 callback 内 delete runner、从该 callback 调用 std::exit、析构与 start/initialize/其他访问并发、多个 monitor 跨对象循环等待均未支持；不得据此声称安全。没有建立 parser/provider 异步回调 drain、writer 内部后台工作停机或模块卸载合同；外部 callback 自毁和失效 owner 需要独立协议。Windows 控制处理器改动未在 Windows 编译运行；真实 writer/SDK、Linux、LSan 及最终整合树回归未验。R22-002 不关闭。
