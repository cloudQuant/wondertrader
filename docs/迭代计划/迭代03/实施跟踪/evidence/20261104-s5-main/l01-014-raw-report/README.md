# BUG-L01-014（卡6）：provider 无法转换的原始回报上送宿主；K42 首个 provider（CTP 订单查询完整性）

主线程，2026-11-05。决策依据：卡6 用户裁定，即无法解析的交易回报和持仓必须保存原始事件、暂停账户交易，人工对账后才能恢复，不自动重放；以及 K42 版本化查询状态。HEAD 见 `HEAD.txt`，本项涉及源码和测试的改后副本在 `after/`，哈希见 `sources.sha256`，改前副本在 `before/`（`*.batch3` 为第三批修改前快照）。

## ABI（`src/Includes/ITraderApi.h`，与 K42 共用协商机制）
- 能力位 `WT_TRADER_SPI_FEATURE_RAW_UNRESOLVED`（1<<1），由宿主通过可选导出 `setTraderSpiFeatures` 声明。
- `ITraderSpi` 末尾追加 `onUnresolvedProviderReport(kind, reason, raw)`，缺省为空实现，旧 vtable 前缀不变。provider 仅在宿主声明该位后才调用。未声明时 provider 只写一条 `LL_ERROR` 日志，与改前一样丢弃。

## 宿主（WtCore `TraderAdapter`）
- `onUnresolvedProviderReport`：先 `pauseAccount("unresolved_report", …)` 暂停整个账户，再把 `{schema_version:1, record_type:"provider_raw_report", kind, reason, raw}` 存入未解析报告表，已配置时同时写入 journal。不入账、不自动重放；恢复仍只走 6b=A 的显式入口（WtRunner 恢复目录、WtPorter C API）。
- 模块宿主在 init 时声明 `QUERY_STATUS | RAW_UNRESOLVED`（provider 导出该函数时）。

## Provider 迁移
共享助手 `src/Share/ProviderRawReport.hpp`：
- `hexOf`：SDK 结构体整份十六进制，与厂商字段布局无关；
- `textOf`：JSON/FIX 文本原样保存，上限 64 KiB，超出时标记 `truncated=1`；
- `report`：按能力位上送，或记日志。

| 插件 | 推送委托/成交 | 查询行（委托/成交/持仓） | 本机验证 |
|---|---|---|---|
| TraderCTP | 字段文本 | 委托、成交、持仓；订单查询按 K42 报告 COMPLETE/PARTIAL/FAILED | 构建＋用例＋红控（查询行） |
| TraderCTPMini | hex | 委托、成交、持仓 | 构建＋用例 |
| TraderCTPOpt | hex | 委托、成交；持仓沿用原有“批次不完整并丢弃”，另上送原始行 | 构建＋用例 |
| TraderFemas | hex | 委托、成交、持仓 | 构建＋用例＋红控（查询行） |
| TraderXTP | hex | 委托、成交 | 构建＋用例＋红控（查询行） |
| TraderYD | hex＋`InstrumentID`（有界读取） | 无独立查询路径（回报来自推送缓存） | 构建＋用例 |
| TraderHuaX | hex | 委托、成交 | 仅语法检查（见下） |
| TraderXTPXAlgo | hex | — | 仅语法检查 |
| TraderOES | hex | 委托、成交 | 语法检查 0 错误 |
| TraderATP | hex（成交） | 委托、成交 | 仅语法检查 |
| TraderDD | FIX 全字段文本 | 委托、成交、持仓 | 本机无厂商头文件，未编译 |
| TraderHTS | 原始 JSON（含解析失败的报文） | 委托、成交、持仓（hex） | 本机无厂商头文件，未编译 |
| TraderMocker | 不适用：成交由内部撮合生成，没有厂商回报 | — | — |

## 用例与结果
- 宿主：`p11_sync_report_test.cpp` 的 `l01014_raw_provider_report_is_kept_and_pauses_the_account`。
- CTP：
  - `k42_order_query_reports_completeness_when_host_supports_it`
  - `l01014_unconvertible_trade_is_forwarded_raw_only_to_a_host_that_keeps_it`
  - `l01014_unconvertible_position_and_trade_query_rows_are_forwarded_raw`
- CTPMini：`L01014UnconvertibleReportsAreForwardedRawWhenHostSupportsIt`
- CTPOpt：`l01014_unconvertible_trade_is_forwarded_raw_when_host_supports_it`
- Femas：
  - `l01014_unconvertible_trade_is_forwarded_raw_when_host_supports_it`
  - `l01014_unconvertible_query_rows_are_forwarded_raw_when_host_supports_it`
- XTP：
  - `l01014_unregistered_trade_is_forwarded_raw_when_host_supports_it`
  - `l01014_unregistered_query_rows_are_forwarded_raw`
- YD：`l01014_unknown_instrument_reports_are_forwarded_raw_when_host_supports_it`
- 日志在 `logs/`。

**红控**
- 新回调与能力位是新增 ABI，改前不存在，推送路径和宿主无法做红控。
- 查询行这一批修改前，CTP、Femas、XTP 已有 `setSpiFeatures`，因此用 `*.batch3` 改前源码重建并运行同一用例：
  - CTP `n_unresolved_` 为 0，预期 1（`ctp-query-raw-red.log`）；
  - Femas 为 0，预期 3（`femas-query-raw-red.log`）；
  - XTP 为 0，预期 2（`xtp-query-raw-red.log`）。
  - 恢复改后源码（cmp 一致）并全量重建后均通过。

**逐例回归（每例独立进程）**
- `regression-summary-hosts-and-plugins.json`：t_TraderCTP/CTPMini/CTPOpt/Femas/XTP/YD 与 wtcore/porter/m_WtRunner/uftcore 共 2057 例，pass 2048、skipped 9、失败 0。
- `regression-summary-plugin-lanes-final.json`：最终插件 lane 共 375 例，375 通过。

**语法检查**（clang++ -fsyntax-only，`logs/syntax-*.log`）
- OES 加厂商 include 后 0 错误。
- HuaX 5 处 `strcpy_s`（Windows 专有）、XTPXAlgo 1 处与 ATP 1 处 fmt 格式化枚举/atomic：均为既有代码所在行，改动行内无任何诊断（脚本按 diff hunk 比对）。

## 过程问题（已修复）
- 第一、二批插件迁移脚本按“CRLF 行数 > 10 视为 CRLF 文件”整体改写换行。CTPMini/CTPOpt/XTP/YD/OES 的 .cpp 与 OES.h 因此整体变成 CRLF，XTPXAlgo/ATP 的 .cpp 少数 LF 行被改，Femas.cpp 的 10 个 CRLF 被去掉。
- 同一脚本早先还把 `TraderCTP.cpp`（HEAD 62/1624 行 CRLF）和 `WtCore/TraderAdapter.h`（HEAD 528/806）整体改为 CRLF。
- 已用 `fix_eol.py` 按 difflib 对齐：未改动的行恢复改前/HEAD 的行尾，新增行取文件多数行尾。修复后去掉 CR 的内容与修复前逐字节一致，插件相对 before 的 diff 只剩新增行。
- `audit_eol.py` 对全部 478 个已修改文件复查：剩 14 个文件各有 1–27 行“内容相同、行尾不同”，都是把少数离群行统一成文件主行尾，来自早期修改，内容无变化，未回退。
- 之后的编辑一律用 `newline=''` 原样读写。

## 未完成与外部项
- DD、HTS 本机无厂商头文件，未编译。HuaX、XTPXAlgo、ATP 只做了语法检查。均须在对应平台构建验证（X）。
- 真实柜台回报、硬断电、Windows 审计日志 fail-closed 未验证（X）。
- UFT 宿主未声明这两项能力（卡6 账户门禁限于 WtCore）。UFT 下 provider 维持旧行为，只记日志。
- K42 查询完整性只迁移了 CTP 的订单查询；成交和持仓查询的状态，以及其他插件的迁移，归 BUG-I02-NEW-154。
- 新发现：TraderOES 查询的最后一行转换失败时提前 `return`，同时跳过最终 `onRspOrders/onRspTrades` 回调。本项只增加上送，未改变该语义，另立 I03-NEW-101。
