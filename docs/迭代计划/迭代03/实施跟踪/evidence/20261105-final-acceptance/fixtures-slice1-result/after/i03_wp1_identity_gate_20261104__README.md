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

## 2026-11-05 期望更新（K44）

`Wp1Test.OwnerScopeDoesNotLeakIntoSinkCallbacks` 原期望：owner `exec:a` 的 `buyAsOwner` 在 `orderInsert` 同步回报中触发非 owner 观察者的裸 `buy()`（同一合约），该重入下单成功且 owner 为空。决策 K44（2026-11-04 用户裁定）：owner 路由开启时，HFT/人工裸 API 订单以账户 `manual` owner 独占参与合约归属，因此同一合约上的裸单被 `AccountTradingGate::claimContract` 拒绝（当前 `TraderAdapter::doEntrust` 的 “manual entrust rejected”），F3 上该例 `inner.size()==0` 失败属夹具期望落后于 K44，不是产品回归。

更新后的用例仍检查“回调内下单不继承 owner exec:a”：同一合约的裸单必须被拒绝（若继承 exec:a 会以 exec:a 身份被接受）；另一合约（`SHFE.au.2212`）的裸单必须被接受且账本 owner 为空、不计入 exec:a 的在途（若继承会记到 exec:a）。其余用例、`run.sh` 未改；`run.sh` 中 `git rev-parse` 与 docs 证据 `s5before` 依赖仍由夹具运行器在镜像中处理，baseline3 红控依赖 docs 证据，夹具运行器中不运行（not-applicable）。
