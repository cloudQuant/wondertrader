# Iteration 03 frequency fixture

The runner extracts the current TraderAdapter prechecks, request methods and both cancellation entry points. It compiles those functions with local fake order objects and a fake provider, together with the real FrequencyWindow helper. It does not instantiate the complete TraderAdapter, connect a provider, or exercise the real order ledger.

Run in a new output directory:

```sh
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python src/TestUnits/fixtures/i03_frequency_20261103/run.py /tmp/i03-frequency-fixture-NEW
```

The helper uses injected millisecond times for deterministic N/N+1, exact expiry, idle expiry, disabled limits, explicit rollback, pending reservations, and cross-key expiry checks. Extracted request methods check N/N+1, batching, reentry, concurrency, conservative handling of undocumented negative returns and exceptions, and daily exclusion. The fake provider tests do not establish SDK return contracts. The real ITraderApi return type does not distinguish a definite pre-submission failure from an uncertain outcome, so the extracted production methods retain the slot after negative results; the explicit rollback test is helper-only.

## 迭代03 最终验收处置（2026-11-05，slice3-result）

- 分类：**SUPERSEDED**（不修）。`run.py` 从 `TraderAdapter.cpp` 按签名抽取 `doEntrust/checkCancelLimits/checkOrderLimits/isTradeEnabled/doCancel/cancel`，与 `check.cpp` 替身编译；当前方法体引用 `account_net_limits::Registry`、`_account_gate`、`getPosition`、`getUndoneQty` 等替身不存在的成员，20 个编译错误，探针未运行。
- 由以下用例取代：coverage（F3 均为 pass）`CoverageTests_wtcore::R3TraderTest.order_freq_window_denies_submission_after_boundary`、`CoverageTests_wtcore::R3TraderTest.order_freq_window_precheck_uses_standard_code_after_raw_submission`、`CoverageTests_wtcore::R3TraderTest.cancel_freq_window_denies_cancel_after_boundary`、`CoverageTests_wtcore::R170TraderRisk.OrderWindowRejectsAtBoundaryAndRecovers`、`CoverageTests_wtcore::R170TraderRisk.CancelWindowRejectsAtBoundaryAndRecovers`、`CoverageTests_wtcore::R90TraderRisk.FrequencyWindowsRejectStandardCodeAfterRawCodeEvents`、`CoverageTests_wtcore::P11PreOrderRisk.order_frequency_window_rejects_standard_code_after_successful_orders`；N/N+1、批量、同步重入与并发预留由真实适配器夹具 `i03_trader_ratelimit_20261103`（24 例，plain/ASan/TSan 通过）覆盖。FrequencyWindow 辅助类的注入时钟单元检查（精确过期、空闲过期、跨键过期）在 coverage 中无独立单元用例，行为经上述边界/恢复用例间接覆盖。
