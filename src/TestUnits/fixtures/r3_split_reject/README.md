# R3 逐笔限流与拆单账本交互探针

本探针从当前 `TraderAdapter.cpp` 原样提取 `buy` 和 `sell` 两个函数并编译，其他订单、策略、合约、仓位和 provider 调用均为确定性替身。替身模拟每笔 1 手、共 3 手，前 2 笔提交成功，第 3 笔在提交前被拟议的逐笔频控拒绝，返回与当前 `doEntrust` 同样的 `UINT_MAX`。该限制器**只是测试接缝，尚未接入产品**。

2026-11-02 macOS 本机普通构建结果：买、卖三笔全部接受正控各 exit 0；第三笔被拒绝时，买、卖红控各 exit 1，分别输出 `accepted=2 returned=3 invalid=1 undone=3 expected=2` 和 `accepted=2 returned=3 invalid=1 undone=-3 expected=-2`。即使底层正确拒绝请求，上层现有拆单仍将失败标记当订单 ID 返回，并保留未提交部分的未完成量；单纯在 `doEntrust` 增加限流会扩大现有账本缺陷。

```sh
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python \
  src/TestUnits/fixtures/r3_split_reject/run.py /tmp/wt-i03-r3-split-probe-new
```

输出目录必须不存在；脚本 exit 0 只表明上述两项正控通过、两项已知红控失败，不能视为产品通过。第一次编写探针时 `sell_control` 的测试期望因无符号乘法溢出错误而失败；修正替身后，在新目录重编得到以上结果。

本次 `TraderAdapter.cpp` SHA-256 `8ea4906c4c1d4bc419d9f4f1ba508ac9810160ed7aa844c443247aced07fbc30`。测试没有执行真实交易通道、真实 provider 回调、并发、风控算法、撤单或 sanitizer；也不证明 provider 的失败返回和回报顺序。生产代码未修改。完整修复需同时处理逐请求原子预留、同步回调与失败语义、拆单部分成功账本及真实入口调用，不能把该探针当作解决方案。

## 迭代03 最终验收处置（2026-11-05，slice3-result）

- 分类：**SUPERSEDED**（不修）。`run.py` 抽取 `TraderAdapter::buy/sell` 方法体与替身编译；当前实现调用 `submitSplit` 等新辅助函数，替身未提供，14 个编译错误。该探针的买/卖“第 3 笔被拒仍返回 3 个 ID”红控是 BUG-R3-002/003/008 修复前的已知红，且所用限流器“只是测试接缝”。
- 由以下用例取代：独立夹具 `i03_trader_ratelimit_20261103`（编译真实 `TraderAdapter.cpp`，F3 夹具运行 plain/ASan/TSan 各 24/24）：`I03RateLimit.buy_split_partial_rate_reject`、`I03RateLimit.sell_split_partial_rate_reject`、`I03RateLimit.buy_and_sell_split_partial_api_failure`、`I03RateLimit.buy_and_sell_split_all_rejected`、`I03RateLimit.sell_close_long_split_partial_failure`、`I03RateLimit.nested_buy_inside_split_keeps_both_ledgers`；coverage 中拆单正向用例 `CoverageTests_wtcore::R3TraderTest.buy_splits_by_rule_cap_and_single_order_max_volume`、`CoverageTests_wtcore::R59Buy.open_splits_by_limit_volume_and_handles_market_price`、`CoverageTests_wtcore::R59Sell.open_splits_by_limit_volume_and_handles_market_price`（F3 pass）。**拆单子单被拒的返回值/未完成量**在 coverage 二进制中没有直接用例，只有上述夹具覆盖。
