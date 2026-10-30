# BUG-P04-009 v8 本机 focused 验证

日期：2026-10-30；平台：macOS arm64。主审与独立复审均已通过，本范围没有逻辑 blocker。最终 v8 运行 2/2 PASS、无 skip，执行清单按有限本机范围记为 `TEST_PASS`。

## 根因与修复

有两条相关路径。请求不存在/坏数据的主 K 线时，主序列 key 或空的 bars cache 可能在成功加载并校验之前发布；随后 `run()` 选中不存在/空数据会出错，失败状态也会阻断后续重试。`run()` 现验证主 key 对应的 cache entry 与 bar vector 均有效，无主序列时安全完成；bin reader 以局部 `BarsList` 暂存读取结果，文件未读到完整 `WTSBarStruct` 记录、记录不完整或数量超过 `INT32_MAX` 时拒绝，并只在数据校验成功后发布到 cache。重采样路径也检查 slice 的 `int32_t` 索引上限。

v8 用例覆盖真实的 `WtDataStorage` dylib：header-only 空 DSB 被 loader 读为空时，不发布空 raw cache；修复同一文件后，通过同一个 replayer instance 再读成功，并完成 resampling/有效主日线正控。另一用例验证 CSV 主 K 缺失时安全完成，随后在同一 replayer instance 补齐有效 CSV 后可正常回放。

## 运行历史与 oracle 纠正

- v6 首次运行 1/2：CSV 主 K 失败/重试用例通过；bin loader 测试的 `recovered` 为空。复查后发现这是测试 fixture 的时间范围错误：fixture 按 15:00 终点查询，却只提供 09:35–10:30 的 bars。测试改为 09:30 起始，并加入上一交易日数据作为正控；这次失败归类为测试前提错误，不是产品回归证据。
- v7：fixture oracle 调整后 2/2 PASS。
- v8：在最终 `HisDataReplayer.cpp` 源码重新编译、使用已重编的实际 `WtDataStorage` dylib 后，2/2 PASS 且无 skip。**状态仅依据 v8，不以 v6/v7 中间结果代替最终验收。**

## 复现与边界

[`reproduce-v8.sh`](reproduce-v8.sh) 显式构建真实 `libWtDataStorage.dylib` 与最终 replay/test 对象，然后按 v4 focused link 结构手工链接；测试通过 `DLLHelper` 在运行时加载 build tree 中的 storage dylib。链接使用 `-Wl,-undefined,dynamic_lookup`。此过程复用已存在依赖库，是 focused incremental build/link，不是 clean rebuild 或 aggregate `CoverageTests`。

Windows、ASan/TSan、真实历史行情、完整 backtest/runner 生产整链未验收。focused fixtures 证明本地 storage dylib 的 loader/retry 行为，不替代大规模、跨平台或真实数据验收。
