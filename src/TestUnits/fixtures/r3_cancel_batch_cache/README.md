# R3 批量撤单缓存遗漏隔离探针

`run.py` 从当前 `src/WtCore/TraderAdapter.cpp` 原样提取 `doCancel` 和两个 `cancel` 方法，使用本目录替身编译并执行。替身的 `checkCancelLimits` 始终放行，provider 的返回可控；本探针只验证缓存记账的位置，**不是**真实风控、provider、线程或完整适配器测试。

```sh
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/r3_cancel_batch_cache/run.py /tmp/wt-i03-r3-batch-cache-new
R3_SANITIZE=1 /Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/r3_cancel_batch_cache/run.py /tmp/wt-i03-r3-batch-cache-asan-new
```

输出目录须不存在。当前源码的单笔成功为 `calls=1 cache=1`（exit 0），同步失败为 `calls=1 cache=0`（exit 0）；批量成功两笔为 `calls=2 cache=0`，应记两笔的断言失败（exit 1）。驱动进程 exit 0 仅表示观测到上述预期红绿结果，**不表示缺陷已修复**。首次编译先后因替身缺少类型别名、map 初始化错误失败，修正后重新编译得到上述结果。

本机 ASan/UBSan 构建成功，但运行首个 `single` 对照约 45 秒无输出后人工中断；未得到 sanitizer 结果，不能计作通过。脚本现为每个子进程设置 20 秒超时。

现有单笔路径在 provider 返回成功后才追加缓存；批量路径调用同一 `doCancel` 但没有追加。单纯移动记账位置会使批量路径进入现有永久排除机制，违背超限后自动恢复的已定规则；且请求在 provider 返回前没有预留，无法证明并发和同步重入满足频控。第 N+1 次是否到达实际 API、拒绝后恢复、异常、时钟、同步回调和完整交易链均未由此探针验证。

## 迭代03 最终验收处置（2026-11-05，slice3-result）

- 分类：**SUPERSEDED**（不修）。`run.py` 从 `TraderAdapter.cpp` 抽取 `doCancel`/两个 `cancel` 方法体，与本目录替身编译；当前实现引用 `RiskParams`、`getRiskParams`、`_risk_mon_enabled`、`CodeTimeCacheMap`、预留等替身没有的标识符，normal 与 `R3_SANITIZE=1` 均编译失败。该探针的“批量成功未记缓存”红控是 BUG-R3-002/003 修复前的已知红。
- 由以下用例取代：coverage（F3 均为 pass）`CoverageTests_wtcore::R3TraderTest.cancel_freq_window_denies_cancel_after_boundary`、`CoverageTests_wtcore::R3TraderTest.cancel_localid_synchronous_api_failure_is_not_cached`、`CoverageTests_wtcore::R3TraderTest.cancel_localid_risk_rejection_retries_do_not_grow_cache`、`CoverageTests_wtcore::R3TraderTest.cancel_localid_frequency_cache_uses_standardized_contract_code`、`CoverageTests_wtcore::R170TraderRisk.CancelWindowRejectsAtBoundaryAndRecovers`。**批量撤单路径**的缓存记账在 coverage 二进制中没有直接用例；等价证明在独立夹具 `i03_trader_ratelimit_20261103`（编译真实 `TraderAdapter.cpp`，F3 夹具运行 plain/ASan/TSan 各 24/24）：`I03RateLimit.cancel_boundary_batch_uses_same_window`、`I03RateLimit.batch_cancel_survives_sync_cancel_reports`。
