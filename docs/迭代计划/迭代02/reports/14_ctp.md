# 14_ctp 候选逐条复核

## 范围与证据边界

本报告复核输入组 `/tmp/wondertrader-review-20261029/groups/14_ctp.json` 的 44 条候选。逐条对照了候选原文、仓库实现、公开接口/锁定 SDK 头文件及现有测试断言。本轮只做源码审查，未运行测试、构建、全量命令、网络或真实柜台；因此没有条目标为 `LOCAL_REPRO`。产品判定为静态证据，涉及 CTP/XTP/期权 SDK 空载荷、回调时序或未明确业务策略的条目标为 `UNDECIDED/PENDING`。

只新增了本报告与对应 triage JSON；没有改动 bugs 原件、测试、生产代码或 build 文件。

## 判定汇总

| 判定 | 条数 | 处理 |
| --- | ---: | --- |
| PRODUCT | 25 | 源码/公开接口支持有效输入存在产品行为缺陷，建议进入计划 |
| DUPLICATE | 3 | Mini/期权裸下单或 Mini 停机与 canonical 根因合并，不重复计数 |
| TEST | 1 | 撤回“T1-XTP-03 正常 adapter 生命周期未初始化危害”主张 |
| UNDECIDED | 15 | 静态危险路径存在，但需 SDK 回调契约、对象生命周期或业务策略证据 |

| Lane | 候选数 | PRODUCT | DUPLICATE | TEST | UNDECIDED |
| --- | ---: | ---: | ---: | ---: | ---: |
| R61 | 5 | 2 | 3 | 0 | 0 |
| R65 | 2 | 1 | 0 | 0 | 1 |
| T1 CTPOpt/ParserXTP | 11 | 7 | 0 | 1 | 3 |
| T25 TraderCTP | 10 | 5 | 0 | 0 | 5 |
| T2 CTP/Xele/CTPOpt | 16 | 10 | 0 | 0 | 6 |

证据等级分布：29 条 `STATIC`、15 条 `PENDING`、0 条 `LOCAL_REPRO`。DUPLICATE 不计入新增独立产品缺陷；唯一引用分配缺陷以 `T2-CTP-08` 为 canonical。完整逐条结论、代码/测试行号及修复验收建议见 [14_ctp.json](../triage/14_ctp.json)。

## 重点结论

### 可进入修复计划的静态产品问题

优先关注三种共性风险：

- **CTP 交易生命周期**：TraderCTPOpt 与 TraderCTPMini 的 worker 没有可达停止路径，disconnect 的 join 会等待；TraderCTP 另有无 worker 时残留 release 队列项、停止状态未复位导致重连失效的问题。原“未连接即泄漏 API”说法撤回。
- **引用和下单错误返回**：空 tag 裸下单路径对 OrderRef 使用 `fetch_add(0)`，未分配新引用；TraderCTP 忽略畸形 entrust ID 的解析失败，也吞掉认证、登录、下单/撤单的同步错误返回。三个 CTP provider 的裸下单候选归到 T2-CTP-08。
- **Parser 生命周期与错误处理**：ParserXTP 没有工作线程停止/回收；ParserXeleSkt 的 bool 初始化接口让非法地址异常穿透，且 release/disconnect 没有 join 接收线程；ParserCTPOpt 错误码判定恒 false、登录失败仍走成功订阅路径，另有缺少 loader 检查和 500 项无界写入问题。

其他静态 PRODUCT 项包括行权查询回调数组错用、行权数组构造未初始化、Opt 撤单响应分支反置、Opt 未知持仓末行跳过聚合收尾、CTP 订单状态 b/c 映射错位、CTP 缓存初始化失败后的空 block 写入路径等。JSON 为每项给出具体修复目标与验收条件。

### 测试 oracle / 误报处置

- `src/TestUnits/coverage/t_TraderCTPOpt/cov_traderctpopt_spi_test.cpp:1057-1059` 将行权查询期待成普通订单数组的 0 长度，这是旧实现行为；R61 正确用例 `r61_traderctpopt_api_test.cpp:1035-1053` 期待行权数组记录。修正旧 oracle：一行输入应期待一行有效行权结果（实现修复前该断言应失败），并核对行内容。
- `src/TestUnits/coverage/t_TraderCTP/t2_traderctp_conversion_test.cpp:672-680` 对 `WDT_SHORT + WOT_CLOSETODAY` 期待 CTP Direction=Sell 与映射不符；转换式 `src/TraderCTP/TraderCTP.cpp:61-69` 和交易含义对应 Buy。将第 679 行改为 Buy，并保留 offset/time-condition 检查。
- T1-XTP-03 的“正常 adapter 生命周期受未初始化成员影响”不成立：`src/WtCore/ParserAdapter.cpp:56-61` 在 init 前调用 registerSpi，ParserXTP 在非空注册时取得 BaseDataMgr。该项标为 TEST；注销后的 manager 生命周期如仍要追踪，需另列明确调用前提。
- CTP 合约状态原样强转与本地 SDK/WTS 字符定义对齐，不纳入修复；同一候选中的订单状态 b/c 错映射仍保留为 PRODUCT。
- TraderMocker 开仓成功状态文案缺少明确公共契约，不能用 HftMocker 的相似行为直接定性，保留 UNDECIDED。GTD 订单状态字段确有未初始化风险，但 WTS 对 GTD 的表达策略未确认，因此也保留 UNDECIDED；现测试避免读取不确定值是正确的。

## 待补证事项

15 条 UNDECIDED/PENDING 主要缺少：CTP/XTP SDK 对 NULL callback payload 与释放后在途回调的保证；查询无终帧时断线/停止保证；GTD 在 WTS 模型中的表达策略；TraderMocker 成功状态文案契约；以及裸 API 下单是否必须先生成 entrust ID 的明确文档。未取得证据前，不将人工 callback 顺序、disabled 用例或厂商行为推断升级为已确认产品问题。
