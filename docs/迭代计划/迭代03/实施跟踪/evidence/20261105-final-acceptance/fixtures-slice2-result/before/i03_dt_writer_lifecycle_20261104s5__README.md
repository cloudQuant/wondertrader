# 写端 / DataManager 生命周期夹具（迭代03，2026-11-04，dt-writer 包）

关联条目：BUG-I02-NEW-038、BUG-R18-002、BUG-R23-001、BUG-P10-008（卡9：9a=A+C、9b=A、9c=A、9d=A）。本目录是局部定向夹具，**不是整体回归**。

## 内容

- `fake_writer.cpp`：三种假 writer 动态库。`s5_fake_legacy` 不导出生命周期 V1；`s5_fake_v1` 导出 V1；`s5_fake_v1_async` 导出 V1 且自带回调线程。`writeTick` 在 sink 回调返回后再访问自身对象并记录 `write-exit`（析构后访问记为 `write-exit-dead`），事件写入 `$S5_FAKE_TRACE`。
- `lifecycle_test.cpp`：通过真实 `DataManager`（被测树编译）加载上述假库与被测树的真实 `libWtDataStorage.dylib`，覆盖回调内 release 延迟到安全点、writer 自有线程回调内 release 不自连接、旧 ABI writer 隔离不卸载、重复 init 拒绝、回调阻塞时 release 有界（`release_timeout_ms`）并隔离、停止期间并发写入不丢已接受数据、析构恰好释放一次。
- `p10_008_reentry_test.cpp`：直接经 `createWriter` 使用真实 writer，覆盖委托队列/委托明细/成交三流同块同步重入返回且按存储顺序广播、回调期间不持块锁（另一线程可追加并按序投递）、回调抛异常被逐事件隔离且后续事件送达。
- `run.sh <src_root> <out_dir> <normal|asan|tsan> [baseline]`：从 `<src_root>` 编译 `WtDataStorage/*.cpp`、`WtDtCore/DataManager.cpp`、`WtDtCore/WtHelper.cpp`，假库与测试源始终取本仓库；`baseline` 以 `-DS5_BASELINE_HOST` 编译（旧 DataManager 无状态观察接口，状态断言跳过，其余以事件顺序/崩溃/超时判定）。WTSTools/WTSUtils 静态库默认取隔离快照构建 `/tmp/wt-i03-s5-dt-writer-after/dbg`（`S5_LIBS` 可覆盖），未插桩。sanitizer 变体使用 Homebrew LLVM 22。
- `run_cases.sh <out_dir> <run_root> <秒>`：每例独立进程、独立 cwd、`perl alarm` 硬超时，汇总退出码、XML 与 sanitizer 报告数。

脚本不创建符号链接（构建后检查 `symlinks.txt` 为空）。测试不连接任何柜台或行情源。

## 复现

```sh
bash src/TestUnits/fixtures/i03_dt_writer_lifecycle_20261104s5/run.sh /tmp/wt-i03-s5-dt-writer-after/src /tmp/<新目录> normal
bash src/TestUnits/fixtures/i03_dt_writer_lifecycle_20261104s5/run_cases.sh /tmp/<新目录> /tmp/<运行目录> 60
```

红控以 `baseline` 模式对改前源码树运行（见证据目录 `docs/迭代计划/迭代03/实施跟踪/evidence/20261104-s5-dt-writer/README.md`）。

## 限制

假库只证明宿主按 V1 合同行事，不证明任何真实第三方 writer 能排空；外部预编依赖、Boost、GTest 未插桩；未验证 Windows/MSVC、跨进程、真实磁盘故障与整体回归。
