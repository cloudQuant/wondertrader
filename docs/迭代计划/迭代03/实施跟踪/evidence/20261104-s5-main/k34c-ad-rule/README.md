# K34=C：AD 读端对 HOT/custom 规则代码 fail-closed（BUG-R122-002 第一步）

- 产品：`src/WtDataStorageAD/WtDataReaderAD.cpp` 的 `readTickSlice`/`readKlineSlice` 在任何缓存与存储访问前，对带规则标签（HOT/custom）的代码记错误并返回 NULL；不再提供预合并品种历史或当前映射的原始合约。用户已接受现有依赖预合并历史的 HOT 读取失败。改前/改后副本与哈希见 `before/`、`after/`。
- 用例：按裁定改写 `R100ADReaderTest.R100ReaderADHotReloadDayAndRtCacheGrowth`（HOT tick/K 线均拒绝且各记一条错误）与 `R4ReaderADTest.HotBackwardCutoffWithEmptyProductHistoryFailsClosed`（晚截止点也拒绝），改前用例副本在 `before/`。
- 红控：以改前读端重编 `CoverageTests_adstorage`，两例失败（`red.log`）；恢复改后文件（哈希一致）后两例通过（`green.log`），adstorage 全二进制逐例 282/282（`adstorage-summary.json`）。相关二进制（CoverageTests/dtservo/porter/wtcore）逐例除 `R90ExecPool.ArbiPoolForwardsEverything`（并行负载下时序，单跑 3/3）外全部通过。
- 未覆盖：K34=A 物理分段拼接未实施（R122-002 仍需）；其他读端（WtDataReader/RdmDtReader）的 HOT 来源仍按现状。
