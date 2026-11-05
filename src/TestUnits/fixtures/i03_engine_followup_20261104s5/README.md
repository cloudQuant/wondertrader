# I03-NEW-065 / I03-NEW-066 定向夹具（engine-lifecycle 追加）

只用本地 fake，不连接柜台、不下单、不打开网络。

- `fake_mq.cpp`：`-DFAKE_CREATE_ID=<n>` 决定 `create_server` 返回值（0 = 模块未能创建服务）；导出计数器。`run.sh` 以 `libWtMsgQue.dylib` 名放进各用例 cwd（`EventNotifier::init` 从 cwd 加载）。
- `notifier_test.cpp`（I03-NEW-065）：`create_server` 返回 0 时 `init` 必须返回 false、不启动 worker、不发布、不销毁 0 号服务；正控 id=7 时照常发布并在析构时销毁。
- `../../coverage/q01_wtcore3/s5_sel_schedule_thread_test.cpp`（I03-NEW-066，同时在覆盖率 q01 lane 中）：任务触发后在其自身 context 上恰运行一次、另一 context 不被调度、`release()` join 派发线程；连续多日触发全部被 join。死亡测试子进程运行，旧实现的 `std::terminate` 记为断言失败。

运行：

```sh
bash src/TestUnits/fixtures/i03_engine_followup_20261104s5/run.sh <workdir> <label> <libdir> <header-src> <asan|tsan>
```

`<libdir>` 为一棵 sanitizer 构建的 `libWtCore.a/libWTSTools.a/libWTSUtils.a`，`<header-src>` 为同一快照的头文件树。每例独立 cwd、120 s alarm；死亡测试 fork 子进程中 LSan 会报告 libsystem 的 fork 期分配，故 SEL 两例关闭 LSan。
