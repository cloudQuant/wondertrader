# Replay / storage / extrema v5 本机定向证据

日期：2026-10-30；平台：macOS arm64。主审及独立复审均已完成，未发现逻辑 blocker。依据本批 focused 构建和运行结果，执行清单中的六条按**有限本机范围**记录 `TEST_PASS`，不表示完整调用链、平台或 sanitizer 验收完成。

## 结果

| 缺陷 | 本机运行 | 本轮确认的根因/修复与覆盖 |
|---|---|---|
| `BUG-L06-004` | MatchEngine 15/15 | 主动单在盘口对手量为 0 时旧撮合逻辑仍生成合成成交。当前 `qty <= 0` 时跳过账本/成交处理；买卖两侧均验证零量不成交、余量保持，后续正对手量按实际数量成交。 |
| `BUG-R23-005` | R23 RDM 16/16 | tick 截止判定只比较时钟或使用不一致边界，历史/RT 在等于 `etime` 和跨 action date 场景会产生不同结果。当前按 `(action_date, action_time)` 顺序比较，并保留恰好等于截止点的 tick；history 与 RT 对照通过。 |
| `BUG-R23-007` | R23 RDM 16/16 | 空缓存路径的数量/切片构造可能与实际 bar 数不符。当前在没有数据、空历史和 RT 有数据等路径上验证返回空切片或仅含实际 bar 的切片，size/block count 一致。 |
| `BUG-R12-07` | extrema 19/19 | `WTSKlineSlice` 的空切片和索引边界下极值查询可能无效访问。当前空、负索引、越界、跨块、有效/INVALID price 字段均按错误值与合法极值合同处理。 |
| `BUG-P12-003` | extrema 19/19 | 闭区间 API 守卫曾允许 `end == size`，后续包含式遍历会访问 `size`。当前 `end == size`、负范围及越界输入返回 `INVALID_DOUBLE`，合法单元素/末尾范围仍可查询。此处没有 ASan 证据。 |
| `BUG-L04-005` | AD reader 18/18，另有 2 项 disabled | 首次和增量 LMDB K 线读取对 key/value 尺寸校验不足，可能将畸形字节复制/解释为 `WTSBarStruct`。当前先校验 key/value 大小，再批量发布或提交缓存；测试覆盖短 value、短/长 raw key、初始装载拒绝、增量拒绝和缓存不污染。两个 disabled 测试仍未运行。 |

R23 测试还包含 `CountSizeCheckFailureCanRecoverAfterFileRepair`。旧 oracle 错误期待短文件首读回退到前一日数据；现已修正为 NULL/空结果，再修复文件并验证读取成功。该 oracle 纠正作为 R23 suite 的相邻验证记录；**不据此更改 `BUG-R23-004` 的状态**。

## 重放

完整命令见 [`reproduce-v5.sh`](reproduce-v5.sh)。脚本先显式调用 Ninja 编译相关生产及 CoverageTests 对象，再手工链接四个 focused binary，最后运行 MatchEngine、R23 RDM、R12/P12 extrema 和 AD reader 的专属 suite。它不是 clean rebuild 或 aggregate `CoverageTests` 构建；链接行使用 `-Wl,-undefined,dynamic_lookup` 并复用现有 GoogleTest、Boost、zstd、iconv 和项目静态库。RDM 链接显式加入 `WtDataReader.cpp.o`，用于解析 `proc_block_data`。复现脚本未在归档本批时执行。

原始 Ninja 控制台日志的 SHA-256 与已归档的 v4 日志相同，因为两次都是同一组 16 个编译步骤、输出不含时间戳。当前源码、focused object 和 v5 binary 哈希另列在 `run-v5.json`；其中 MatchEngine/RDM v5 binary 与对应 object 哈希不同于 v4。该控制台日志记录构建步骤，不单独作为源码版本证明。

## 未验收边界

- Windows 原生编译/运行以及 ASan/TSan 均无通过证据；本记录只覆盖 macOS arm64 focused binary。
- 没有 clean rebuild、aggregate 测试、完整回测/生产 reader 调用链或真实存储数据验收。
- `BUG-L04-005` 的首次短 value 修复后重试通过关闭旧 reader、修复 LMDB 并重新初始化；本批没有证明同一个 reader 对同一损坏项修复后重试的合同。raw-key 测试同 reader 的窄窗口重试会避开坏 key，不能替代该证明。
- `WTSKlineData` / slice 的实际超 `INT32_MAX` 大小，以及 `size()`/`extract` 的大数据窄化边界未覆盖；当前跨 `INT32_MAX` 用例仅模拟 slice block count 和安全索引，不代表完整大数据分配或 extract 验收。
- AD Reader 的两个 disabled 案例仍未运行；RDM 并发访问、借用 slice 生命周期和其他平台未验证。
- `BUG-P04-009` 不属于此 v5 验收：同源 bin 空缓存路径仍在修复，保持原 `NOT_STARTED` 状态。v5 的公共构建日志里即便出现其他对象编译，也没有把它们的测试结果纳入本批状态。
