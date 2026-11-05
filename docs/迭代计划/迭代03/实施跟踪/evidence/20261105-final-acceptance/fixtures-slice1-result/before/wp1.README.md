# WP1 交易身份、账户门禁与订单账本夹具（卡5/卡6）

只用本地 fake provider（`wp1_fake_provider.cpp` 代理到测试内对象）与本地执行插件（`wp1_fake_exec_plugin.cpp`、`wp1_twap_plugin.cpp`），不连接柜台、不真实下单/撤单。

```sh
bash src/TestUnits/fixtures/i03_wp1_identity_gate_20261104/run.sh <lib-build-dir> <新输出目录> <variant>
```

- `<lib-build-dir>`：非覆盖率 Debug 构建，含 WtCore WtDtCore WTSTools WTSUtils 静态库。
- variant：`current-asan`（ASan+UBSan）、`current-tsan`、`baseline-asan`（`baseline/`，切片1前）、`baseline2-asan`（`baseline2/`，切片5前）、`baseline3-asan`（迭代03 s5 复核开始时的切片5 `TraderAdapter.cpp/.h`，取自 `docs/迭代计划/迭代03/实施跟踪/evidence/20261104-s5-wp1-ledger/before/`）。baseline 变体经复制的 overlay 编译，不建符号链接，只编译各自基线公开接口可用的用例（`-DWP1_BASELINE/-DWP1_BASELINE2/-DWP1_BASELINE3`）。
- 脚本记录源码哈希并在运行前复核；工作区他人并发修改被监视文件时脚本会以 `shasum -c` 失败退出，需重跑。
- `wp1_twap_plugin.cpp` 把真实 `WtTWapExeUnit`（只读编译 `src/WtExeFact`）包装为 ABI 2.2 插件，分别声明/不声明 `WT_EXEC_PLUGIN_V2_CAP_HOST_ORDER_IDENTITY`，用于卡5b（旧身份 ABI 非共享、无自动超时撤单）端到端用例。
- `Wp1Test.ChildScenario` 只作为 SIGKILL 崩溃子进程辅助，直接运行时 SKIP。

逐次结果与限制见 `docs/迭代计划/迭代03/实施跟踪/evidence/20261104-wp1-review/`、`20261104-s5-main/wp1-fixture/` 与 `20261104-s5-wp1-ledger/`。
