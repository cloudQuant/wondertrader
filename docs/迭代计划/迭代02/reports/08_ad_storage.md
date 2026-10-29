# 08_ad_storage 逐条复核

## 范围与结论

复核输入：`/tmp/wondertrader-review-20261029/groups/08_ad_storage.json`，共 45 条；逐项核对 issue 原文、实际实现、公开接口字段/注释与测试的输入和断言。输出明细见 [`triage/08_ad_storage.json`](../triage/08_ad_storage.json)。

| 判定 | 数量 |
|---|---:|
| PRODUCT | 23 |
| TEST | 3 |
| DUPLICATE | 4 |
| UNDECIDED | 15 |
| NOT_BUG | 0 |
| 合计 | 45 |

本轮证据级别：STATIC 30，LOCAL_REPRO 0，PENDING 15。本轮未运行测试/构建，因此没有任何 LOCAL_REPRO；输入材料里声称的旧复现不能作为本轮实跑记录。UNDECIDED 条目具体缺少的合同、SDK 时序、配置 schema 或失败注入证据见 JSON 各行。

## 需要修正的测试前提

- `BUG-R32-002`：`src/TestUnits/coverage/p05_datastorage2/r4_writer_ad_cache_test.cpp:265` 未设置 `pre_interest`，但期待 `additional()==10`。公开字段定义 `src/Includes/WTSStruct.h:185-190` 是“上日总持”，输入 0 时现有计算是按 0 使用；测试应显式把 `pre_interest` 设为 50，除非上游契约另行规定 0 表示缺失。不要用该红测改变生产算法。用例名是 `DISABLED_ProcFlagOneOnNewTradingDateRebaselinesCacheBlockedByR4Bug01`，精确过滤器为 `CoverageTests_adstorage --gtest_filter=R4WriterADTest.DISABLED_ProcFlagOneOnNewTradingDateRebaselinesCacheBlockedByR4Bug01 --gtest_also_run_disabled_tests`；先隔离 `_async_task` 未初始化前置问题。
- R21 tick 时间用例中的期望游标（如 `r21_rdmtick_slice_test.cpp:63-68,97-100,223-230`）沿用了错误量纲。修复产品时间编码后，应由完整 `date + HHMM*100000 + SSsss` 生成期望，并分开验证首读、older/newer 扩窗、排他上界。
- `BUG-R122-001` 与 `BUG-R155-001` 的禁用夹具不可直接作为回绕证明：R122 在首次读前已 seed 满；R142 原 fixture 先因商品元数据缺失失败。应先构造首次读后追加场景，且隔离环形缓冲引用别名与切片索引错误。
- `R4-BUG-09` 的 `FAIL()` 不是除零复现；`BUG-P05-006` 的长短混合 value 不会令固定大小 memcpy 的输出游标越界写；`BUG-R60-007` 的 NULL action 合法性未定义。三者不要按当前文字当作已证产品失败。

## 验收边界

PRODUCT 表示从实现与有效调用/字段语义可静态确定缺陷；不等于本轮运行验收通过。DUPLICATE 指向既有 canonical，修复验收需覆盖本条额外调用位置。TEST 表示当前测试输入/期待不能支持其产品主张。UNDECIDED 条目在补齐接口合同、配置要求、SDK 事件时序或明确的故障策略前不进入产品修复结论。具体修复动作和可执行验收条件见每条 `fix` 与 `acceptance`。
