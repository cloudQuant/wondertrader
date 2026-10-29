# 14_ctp 第二轮复核

## 范围与口径

本组复核 `14_ctp.json` 中 15 项，只写本组报告、triage 与 evidence。复核者没有修改产品/测试/构建源码；BUG-R65-001 的测试用例清理由 worker 完成，目标已重新编译。本轮使用主代理刷新后的 CoverageTests 目标；各安全 stub 用例单独进程运行，SHA、命令、XML、stdout/return code 均存于 `evidence/14_ctp/runs/`。

本机是 Apple 平台，`TraderCTP.h:20-24` 选择 CTP 6.6.9；其他平台选择 CTP 6.3.15。仓库另有独立 CTPMini 1.5.8 插件，不存在 CTPIIMini 模块。父代理提供的 SimNow CTPIIMini API Ver1.2 文档产品/版本与本组 TraderCTP/CTPOpt 不对应，不能外推 NULL 末帧合同。CTP 6.3.15/6.6.9、CTPOpt 3.5.8、XTP 2.2.32.2 的本地头/SDK 文件清单和缺失契约列在 `evidence/14_ctp/local-contract-review.md`。

## 裁定计数

| 裁定 | 数量 | 项目 |
|---|---:|---|
| PRODUCT | 5 | T1-CTPOpt-02、BUG-T25-005、BUG-T25-006、BUG-T25-009、T2-OPT-05 |
| TEST | 1 | BUG-R65-001 |
| NOT_BUG | 2 | T2-CTP-04、T2-XELE-03 |
| DUPLICATE | 1 | T2-CTP-06 → BUG-T25-003 |
| EXTERNAL_BLOCKED | 6 | T1-CTPOpt-04、T1-XTP-04、BUG-T25-003、BUG-T25-008、T2-CTP-05、T2-OPT-02 |

证据等级为 STATIC 14 项、LOCAL_REPRO 1 项（BUG-T25-008 的“请求已送达 stub 且未回调→front disconnect→交付首次响应”条件顺序）。本地复现证明 wrapper 对该顺序的行为，不证明供应商 SDK 会产生该顺序。

## 逐项结论

| ID | 决定 / 证据 | 核实结论 |
|---|---|---|
| BUG-R65-001 | TEST / STATIC | TraderMocker 开仓成功没有写 `state_msg`，但公开 API 未规定成功文案或与 HftMocker 一致。worker 已移除禁用用例中仅凭 parity 的文案 oracle，改为 `DISABLED_OpenOrderSuccessMessageContractUnproven` 的 `GTEST_SKIP` 占位；该占位未运行。活动开仓用例的订单字段断言仍保留，现有 stub 结果验证这些字段。 |
| T1-CTPOpt-02 | PRODUCT / STATIC | 标准 runner 停机时先释放 trader adapters、后停 engine ticker。CTPOpt `release()` 清空 API 但保留 WS_ALLREADY；窗口内迟到的 adapter cancel 会进入未判空的撤单入口。应让停机先 quiesce engine/adapter 调用，或同步保护撤单与 API Release。 |
| T1-CTPOpt-04 | EXTERNAL_BLOCKED / STATIC | 缓存结算文本后若 CTPOpt 以 NULL payload 终止查询，代码会读空指针；本地 3.5.8 头只给签名，没有说明这种终帧是否有效。需补明确的“已有行后 NULL+bIsLast”合同。 |
| T1-XTP-04 | EXTERNAL_BLOCKED / STATIC | `settle_price` 原值直拷，`pre_settl_price` 用 DBL_MAX/FLT_MAX 归零；本地 XTP 2.2.32.2 头只标注昨日/今日字段，没有 sentinel 约定，邻字段处理方式不足以推出产品要求。 |
| BUG-T25-003 | EXTERNAL_BLOCKED / STATIC | 账户成功末帧 NULL 行、累计 settlement 后 NULL 终帧、sink 注销后的 OnRspError 分别存在潜在解引用；本地 CTP 6.3.15/6.6.9 头没有 nullability 或注销排空条款。disabled 用例只是人工调用。其 settlement 注释把 NULL+成功 RspInfo 描述成“错误末帧”，请求主代理修正。 |
| BUG-T25-005 | PRODUCT / STATIC | 查询前置 `m_bInQuery=true`；若查询尚未收到 bIsLast，disconnect 将 release 排队在受同一标志阻挡的 worker 后并 join，无停止优先级/超时。活动 stub 的正常 query worker 测试通过，禁用 no-reply 挂死探针未运行。 |
| BUG-T25-006 | PRODUCT / STATIC | CTP 的 GTD 是有效时间条件；`WTSOrderInfo` 构造未初始化 `m_orderFlag`，而 `makeOrderInfo` 只写 GFD/IOC，故有效 GTD 回报会把未初始化 enum 传给 sink。用例正确跳过读取该值；建议初始化为明确默认值，并把 WTSOrderFlag 不保留 GTD 到期语义写清。 |
| BUG-T25-008 | EXTERNAL_BLOCKED / LOCAL_REPRO | 修正后的单测从 `bringToLogin()` 开始，确认查询已实际交给 fake API 且尚无响应，再断线并交付该请求的首次响应。新编译独立运行失败，表现为 `WS_ALLREADY`、`isConnected=true`、成功登录计数增加。此 LOCAL_REPRO 只证明本地代码在该条件顺序下的行为；CTP 6.6.9（Apple）/6.3.15（其他平台）是否允许此时序仍待供应商合同，故继续 EXTERNAL_BLOCKED。命令、目标 SHA、日志和 XML 见 `evidence/vendor-fix/t25.{json,log,xml}`。 |
| BUG-T25-009 | PRODUCT / STATIC | queryConfirm worker 把 pending 状态置真后，ReqQrySettlementInfoConfirm 同步返回非零只记日志、不清标志；后续 query/disconnect 会停在 gate。现有 run01 通过只证明有日志，不证明队列恢复。confirm 同步失败也应 fail closed 并向 ITraderSpi 报失败。 |
| T2-CTP-04 | NOT_BUG / STATIC | GetTradingDay 调用在成功 OnRspUserLogin 分支；本地 CTP 6.6.9（Apple）与 6.3.15（其他平台）头均注释正确交易日仅在登录成功后可得。撤销“登录前调用”主张，不外推非法 post-login 日期。 |
| T2-CTP-05 | EXTERNAL_BLOCKED / STATIC | OnRspOrderInsert/Action 的 error 路径会读 payload 与 RspInfo；测试只用非空结构。本地 CTP 头签名没有规定错误回报的 NULL 组合，需要分别确认两个 callback 的 payload/RspInfo 约定。 |
| T2-CTP-06 | DUPLICATE / STATIC | 只是 BUG-T25-003 中账户与 settlement 空末帧子集，canonical 指向 BUG-T25-003；同样受供应商 callback shape 合同阻塞。 |
| T2-XELE-03 | NOT_BUG / STATIC | 标准 ParserAdapter 在 init/connect 前注册非空 SPI，因而初始化 `_bd_mgr`；`registerSpi(NULL)` 会清 sink 但保留 manager。当前候选不能证明正常路径读取未初始化 manager。此结论不覆盖 Xele socket 的 release/stop 生命周期。 |
| T2-OPT-02 | EXTERNAL_BLOCKED / STATIC | 正常 adapter 先注册 SPI；风险在 disconnect 注销并释放 API 时已有 OnRspUserLogin 回调在途，回调仍会访问 `m_pUserAPI`。CTPOpt 3.5.8 本地头没有 RegisterSpi(NULL)/Release 的 callback 排空承诺。 |
| T2-OPT-05 | PRODUCT / STATIC | CTPOpt `strToTime` 写入文件级共享 `tmp_timestr`；配置可启动多个 parser 实例，每实例调用独立 API，回调可写同一数组。有效 TimeType[9] 的最多 8 字符值可装入 10 字节缓冲，因此不支持常规输入溢出主张；修复为函数局部缓冲即可消除跨实例共享状态。 |

## 本轮本地 stub 结果

初始执行 11 个 enabled 单用例：7 PASS、4 FAIL；之后为 BUG-T25-008 追加一次独立安全 stub 复现，失败 1 项。合计 12 次单用例运行：7 PASS、5 FAIL；结果均为本地 stub 范围。

- run01、02、03、04、08、10、11 PASS。分别覆盖同步 query 失败日志、CTP 时间条件映射、结算文本聚合、worker 常规查询、CTPOpt 普通行情字段、Xele unregister 的非行情公共调用、TraderMocker 开仓字段。run11 的活动开仓用例仍断言代码、交易所、价格、数量、状态、剩余量和订单号，不断言 `state_msg` 文案；新 disabled/GTEST_SKIP 占位未运行，未计入上述 PASS 数。
- 初始 run05 FAIL 使用旧的 `bringToReady()` 后重复注入回报顺序，不能代表在途请求；已由 `evidence/vendor-fix/t25.*` 中的修正顺序测试取代。后者确认请求已送达 stub 且未响应，断线后第一次回报会使状态重新 ready；它仍不构成 CTP 厂商时序证据。
- run06 FAIL：`rsp_account_position_and_settlement` 有 4 处测试问题：账户期望应为 108400（当前断言 103400）；同一 position 被调用三次而每次 PositionCost=42000，当前平均价期望按两笔算成 7000、实际三笔为 21000；Long position 的 fixture 设置 LongFrozen，但源按 ShortFrozen 扣减；`bringToReady()` 在 `t2_traderctp_conversion_test.cpp:424-430` 没有 drain login 排入的 query，导致期望 queryDepth=0 与实际 1 不符。均已给主代理精确改法与 target/filter。
- run07 FAIL：不属于这 15 项的额外产品发现。`src/TraderCTPOpt/TraderCTPOpt.cpp:1076-1094` 把 `m_ayExecOrds` 填充后，却在 `:1091` 向 `onRspOrdersOpt` 传 `m_ayOrders`；测试 `src/TestUnits/coverage/t_TraderCTPOpt/cov_traderctpopt_spi_test.cpp:1056-1060` 期望行权数组 1 条，实际为 0。已立即报告主代理，建议最小修复传 `m_ayExecOrds` 并保留目标/过滤器 `CoverageTests_t_TraderCTPOpt / TraderCTPOpt.query_rsp_streams_fill_arrays_and_reset_state`。
- run09 FAIL：与 T1-XTP-04 sentinel 契约无关；该二进制执行时 `cov_parserxtp_spi_test.cpp:405` 仍断言 12.9，但 fixture `:370-375` 给 `bid[9]=21.0`。主代理随后已将源码 oracle 改为 21.0；本轮记录的是改前目标 SHA，修正后的测试尚未在本组重跑。

worker 已重新编译 `CoverageTests_t_TraderMocker` 目标；未运行 disabled/GTEST_SKIP 文案占位。本轮没有运行 disabled hang/crash 探针、ASan/TSan、完整测试集、真实柜台或交易。完整可复核命令、目标 SHA、XML、stdout 和 return code 见 evidence 目录中各 run JSON 与日志；占位未运行，不产生新的执行结果。

