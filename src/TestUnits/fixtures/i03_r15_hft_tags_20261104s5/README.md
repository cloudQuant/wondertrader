# BUG-R15-001 / I03-NEW-002：HFT 订单标签与未绑定上下文（迭代03 s5）

本夹具只用本地 fake provider（`p11fix::FakeTraderApi` 派生），不连接柜台、不真实下单。

## 被测合同

- 标签按 localid 存放于互斥保护的映射；字符串驻留且不可变，回调拿到的 `const char*` 在同一 localid 被替换或删除后仍有效（上下文生命周期内），内存上界为不同标签值的数量。
- `stra_buy/stra_sell` 通过 `TraderAdapter::EntrustObserverScope` 在 localid 预留后、provider 调用前登记标签，同步回报即可读到；未被接受的预留会删除其标签。
- 存活订单标签从不淘汰；终态（撤单/全成/委托失败）进入有界退役窗口（`kRetiredOrderTagLimit` = 16384 笔），窗口内迟到成交仍可读回标签，超出窗口只淘汰最早退役的终态标签（读回空串）。
- I03-NEW-002：未绑定上下文的 `stra_get_position/stra_get_undone` 不再解引用空指针，返回 0 并记错误日志；8 个交易动作在未绑定时拒绝（既有行为）。double 查询无法携带失败状态，0 与合法零值不可区分，属待裁定合同。

## 运行

```sh
# 依赖库目录需已构建 WtCore WtDtCore WTSTools WTSUtils（非覆盖率 Debug）
bash src/TestUnits/fixtures/i03_r15_hft_tags_20261104s5/run.sh <lib-build-dir> <新输出目录> current-asan
bash src/TestUnits/fixtures/i03_r15_hft_tags_20261104s5/run.sh <lib-build-dir> <新输出目录> current-tsan
bash src/TestUnits/fixtures/i03_r15_hft_tags_20261104s5/run.sh <lib-build-dir> <新输出目录> before-asan   # 红控
```

`before-*` 用 `docs/迭代计划/迭代03/实施跟踪/evidence/20261104-s5-wp1-ledger/before/src/WtCore/HftStraBaseCtx.{h,cpp}`（本轮改前副本）经复制的 overlay 编译（不建符号链接），`-DR15_S5_BASELINE`。用例来源为覆盖率 lane `q01_wtcore3/r15_hft_stra_context.cpp`（`R15CtxTest.*`）。HftStraBaseCtx、HftStraContext、WtHftEngine、TraderAdapter 与测试 TU 带 sanitizer 编译，其余对象来自非插桩库，属局部插桩。

结果与限制见 `docs/迭代计划/迭代03/实施跟踪/evidence/20261104-s5-wp1-ledger/README.md`。
