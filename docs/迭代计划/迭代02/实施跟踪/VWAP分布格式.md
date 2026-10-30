# VWAP 分布文件格式与迁移说明

本文说明当前工作区的 VWAP profile 解析规则，供整理现有 `Vwap_<商品名>.txt` 文件时使用。文件名和读取目录沿用现有执行单元配置：运行时按当前工作目录查找该名称。profile 必须匹配执行合约实际使用的 `WTSSessionInfo`；不会仅凭数组长度推断交易时段。

## V2 格式

V2 是 JSON 文档。一个文件可按 `profiles` 保存多个品种交易时段模板。下面是格式示意；`values` 行是说明占位，不是可加载的 JSON 数组，迁移时必须替换为完整的有限数值数组。

```jsonc
{
  "version": 2,
  "profiles": [
    {
      "session_id": "STOCK_DAY",
      "offset_minutes": 0,
      "sections": [
        { "from": 930, "to": 1130 },
        { "from": 1300, "to": 1500 }
      ],
      "bucket_unit": "trading_minute",
      "value_mode": "cumulative",
      "close_inclusive": true,
      "values": [ /* 用该 profile 所需的全部数值替换此说明 */ ]
    }
  ]
}
```

各字段必须按以下含义填写：

| 字段 | 要求 |
| --- | --- |
| `version` | 整数 `2`。根对象只允许 `version` 和 `profiles` 两个字段。 |
| `session_id` | 非空字符串，必须与运行时 `WTSSessionInfo::id()` 完全相同。 |
| `offset_minutes` | 整数，必须与该 session 的时区/夜盘偏移完全相同；当前解析器接受 `-1440` 到 `1440` 分钟。 |
| `sections` | 按交易顺序列出原始 HHMM 交易段，必须与该 session 的原始区间逐项完全一致。每段只包含整数 `from`、`to`，例如 `930` 表示 09:30。时段经 offset 转换后必须有正时长、顺序正确且不重叠。 |
| `bucket_unit` | 固定为 `trading_minute`。午休和交易段之间的休市时间不增加桶位。 |
| `value_mode` | 固定为 `cumulative`。解析器只检查该标记和数组数值格式；它不会验证 profile 数值是否单调、是否为百分比或是否在某个业务区间内。生成数据时应遵循现有 VWAP 业务对 `VwapAim` 的累计目标量约定。 |
| `close_inclusive` | 必须为布尔值 `true`。段末端的桶包含在 profile 中；单独的收盘端点只接受精确的 `HH:MM:00.000`。 |
| `values` | 数字数组；每项都必须是有限数字。长度必须恰好为 `WTSSessionInfo::getTradingMins() + 1`，不能少、不能多。 |

加载时，profile 的 `session_id`、offset、交易段顺序和交易分钟数必须与当前品种 session 完全相同。profile 可包含不同 session 的多项配置，但必须恰好有一项匹配当前 session；重复签名、缺少匹配项、额外/重复字段及错误类型都会使初始化失败。失败时不应继续复用旧 profile。

### 桶位和边界

桶位是累计交易分钟索引：第一段开盘时索引为 `0`；休市不推进索引；相邻交易段接点可以共用同一索引。以标准股票日盘为例：09:30 对应 `0`，11:30 和 13:00 对应 `120`，15:00 对应 `240`，所以数组长度是 `241`。收盘时刻的秒/毫秒必须为零；例如独立的 11:30 收盘端点只接受 `11:30:00.000`。开盘分钟内的有效时刻仍对应开盘桶。

执行单元还会按品种实际 session 验证每笔行情时间。休市、盘前/盘后、非法时刻和不支持的集合竞价行情不会映射到桶，也不得用于继续执行。

## 旧 CSV 兼容范围

解析器仅在以下条件全部成立时接收旧版逗号分隔 CSV：

- profile session 是 offset 为 `0` 的标准日盘，交易段严格为 `09:30–11:30`、`13:00–15:00`；
- 文件恰好有 `241` 个有限十进制数字；
- 每个字段非空且可以完整解析。

其他 session 必须改用 V2 JSON。不能把“同样有 241 项”当作 session 匹配证据。

仓库中跟踪的 [Vwap_name.txt](../../../../src/WtExeFact/Vwap_name.txt) 只有 **240** 项，因此不满足旧 CSV 的当前兼容条件。它也没有 session 元数据，不能安全推断缺失的开盘/收盘桶应如何转换。实际部署数据应按真实 `WTSSessionInfo` 生成完整 V2 profile，或在确认确为标准日盘后提供完整 241 项 CSV；不要直接给 240 项文件补一个猜测值。

## 目前明确拒绝的边界

- 空交易段、无效 HHMM、零时长段、offset 变换后乱序/重叠的交易段会被拒绝。当前 offset 范围是一日以内，以匹配 `WTSSessionInfo` 的单次跨日换算能力。
- 集合竞价时间不支持 profile 桶；执行单元会 fail-closed。
- 对 offset 为 `0` 且交易段 `to` 为 `2400` 的配置，`WTSSessionInfo::timeToMinutes(0000, false)` 无法识别该收盘别名，解析器会拒绝。`action_time` 没有日期，代码不能猜测午夜属于哪一个交易日。不要把此边界误读为所有跨午夜 session 都不支持：offset 和交易段排列可被 session API 正确表达且通过验证的配置有独立映射路径；部署前须使用实际品种 session 验证日期与交易时段配置。
- 桶位 helper 和 profile 单元测试已覆盖标准日盘、午休、不同 session、偏移夜盘、部分跨午夜时段、配置不匹配和非法值。当前本机 helper 定向测试为 17/17 通过；两种执行单元、V2 插件宿主和业务运行链路的联编/端到端验收尚未完成，因此此说明不是生产验收凭据。


## 订单失败重试与日期配对边界

StockVWAP 收到下单返回空订单 ID 时，会将本次新行情的时间作为重试门槛，并按配置的 `_fire_span` 延后重试；在此之前仍需收到时间递增且属于有效 session/执行窗口的新行情。若 `_fire_span` 为 `0`（包括 `tail_secs == duration`），失败重试使用 **1 秒**最短间隔，避免连续行情上每一笔都立即重试。此 1 秒保护只用于失败返回空 ID 的重试；成功发单仍沿用现有发单节奏。相关代码和回归用例已有修改，但两种 VWAP 执行单元及插件宿主仍未完成联编验收，不能视为已验收行为。

行情日期有 `ActionDay` 与 `TradingDay` 两种常见语义，交易源之间并不统一。目前 StockVWAP 对明确不合理的日期组合 fail-closed；对于非零 offset session，若时间倒退跨越 offset 日期边界但 `action_date` 未变化，会停止该单元后续执行并记录不支持的日期 rollover。若 `trading_date` 缺失，则代码会记录兼容性提示并按自然 `action_date` 排序；这并不构成对该 provider 跨午夜语义的验证。仓库中的 YD parser 将 `action_date` 设为交易日字段，YD 与自然 ActionDay 的组合兼容性尚未验收。当前实现不会假定所有行情 provider 都使用相同日期规则，也不会自动改写 provider 日期；部署前须针对实际 provider 的 `action_date`、`trading_date` 与 session offset 配对验证。
