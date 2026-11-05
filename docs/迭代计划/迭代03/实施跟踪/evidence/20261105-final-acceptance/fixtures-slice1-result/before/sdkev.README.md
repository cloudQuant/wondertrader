# 迭代03 卡14（2026-11-03 裁定）：SDK 事件双事件 / 批次上限 / 空响应 fail-closed

用户裁定：14a=C（"已发送"与"已确认"分开，不把发送成功报为业务成功）、14b=A/C（按 SDK 声明分批，可配置）、14c=C（严格按 SDK 契约；无契约前空响应 fail-closed）。
本目录只含本机 fake 夹具；**不连接真实 CTP/CTPOpt 柜台、不下单、不结算**，无符号链接。不是 SDK、Windows、最终整合树或完整回归验收。

## 涉及项与改动

| ID | 模块 | 本机候选行为 |
|---|---|---|
| BUG-T26-011 | `src/CTPLoader/TraderSpi.{h,cpp}` | 在既有在途跟踪上补：每请求非空载荷帧计数，终帧 0 行即空响应 → 批次失败且**不发布任何文件**（含 contracts/commodities）；合约查询空 → 不排队费率请求；日志由 "succeed" 改为 "request N sent, awaiting confirmation"/"send failed"，终局输出 "N sent, M confirmed"。非费率模式：调用前登记请求号，同步返回非零 → 退出失败（旧实现挂起）；错误终帧/已发请求的空响应/命中请求号的 OnRspError → 退出失败且不落盘（旧实现错误终帧仍 `DumpToJson(); exit(0)`）；已发请求后他号回报忽略。 |
| BUG-R65-002 | `src/TraderCTPOpt/TraderCTPOpt.{h,cpp}` | `queryPositions` 调用前登记在途请求号；同步非零 → 撤销登记、释放 `m_bInQuery`、错误日志；只接受在途请求号的持仓回报，新请求首帧丢弃旧聚合；末帧（已确认）才发布；同号重复终帧、失败后迟到帧、他号帧均忽略；命中在途号的 `OnRspError` 为失败终态。 |
| T1-CTPOpt-07 | `src/TraderCTPOpt/TraderCTPOpt.cpp` | `OnRspError(pRspInfo==NULL)` 不再静默，按失败记 LL_ERROR；`ErrorID==0` 保持静默。 |
| T2-OPT-04 | `src/ParserCTPOpt/ParserCTPOpt.{h,cpp}`（清单责任模块为 ParserCTPOpt） | 新配置 `subbatch`（正整数，默认 500）；`SubscribeMarketData` 按上限分批，每个符号恰好提交一次；非法值 init fail-closed；日志只报 "sent ... awaiting confirmation"，确认在 `OnRspSubMarketData` 另记。 |

## SDK 头文件依据（GBK 原文，行号按原文件）

- `src/API/CTPOpt3.5.8/ThostFtdcMdApi.h:130-134`：`///订阅行情。` / `///@param ppInstrumentID 合约ID` / `///@param nCount 要订阅/退订行情的合约个数` / `///@remark `（空）/ `virtual int SubscribeMarketData(char *ppInstrumentID[], int nCount) = 0;` —— **无单次上限、无返回值语义**。`:61` `OnRspSubMarketData(...)`、`:58` `OnRspError(...)` 仅有 "订阅行情应答/错误应答" 注释。
- `src/API/CTPOpt3.5.8/ThostFtdcTraderApi.h:578` `ReqQryInvestorPosition(..., int nRequestID) = 0;`、`:121` `OnRspQryInvestorPosition(...)`、`:247` `OnRspError(...)`：无返回码含义、无非零返回后是否仍回调、无空 `pRspInfo`/空载荷契约。
- `src/API/CTP6.6.9/ThostFtdcTraderApi.h:645/636/633` `ReqQryInstrument/ReqQryInstrumentCommissionRate/ReqQryInstrumentMarginRate`、`:155/146/143` 对应 `OnRsp*`、`:260` `OnRspError`；Linux 用 `src/API/CTP6.3.15/ThostFtdcTraderApi.h:605/596/593`、`:155/146/143`、`:257`。全部头文件中 `@return` 只出现在 `CreateFtdc*Api`/`Join`，**无任何 Req*/Subscribe 返回码、批次上限、终帧/空响应契约**。
- 因此 500 只是本模块历史固定缓冲容量，**不是厂商声明值**；真实上限、`-1/-2/-3` 语义、空结果是否以单个 NULL 终帧表达、非零返回后迟到回调均为 **X（未验证）**。

## 复现

```sh
# CTPLoader 探针：baseline（反向应用本目录补丁=修改前）/normal/ASan+UBSan/TSan × 11 场景
bash src/TestUnits/fixtures/i03_sdk_events_20261103/run.sh /tmp/<新目录>
# 旧 T26-011 20 场景回归（需上一步输出的 current 快照）
bash src/TestUnits/fixtures/i03_sdk_events_20261103/run_t26_regression.sh /tmp/<新目录2> /tmp/<上一步目录>
```

依赖 LLVM 22（`/opt/homebrew/Cellar/llvm/22.1.8`）、Homebrew boost 1.88/fmt/spdlog/libiconv 实体路径与 `/tmp/wt-i03-r41-drain/libs/libWTSUtils.a`（插桩库，故链接 `-fprofile-instr-generate`）。ASan 使用 `detect_leaks=0`，不声称 LSan 通过。
TraderCTPOpt/ParserCTPOpt 用例在 coverage lane `t_TraderCTPOpt`（`r61_traderctpopt_api_test.cpp` 末尾 `I03_CTPOptSdkEvents.*`、改写的持仓/错误用例）与 `t_ParserCTPOpt`（`r105_parserctpopt_api_test.cpp`）。红色对照以修改前产品文件 + `-DWT_I03_SDK_EVENTS_BASELINE` 编译同一测试源。

## 合同变更（按裁定，不是放宽断言）

- `generic_error_response_logs_nonzero_error_and_keeps_success_quiet`：空 `pRspInfo` 由 "no-op" 改为必须出现失败日志。
- `logged_in_subscription_handles_500_and_501_contracts`：501 由单次 501 改为 500+1 两批。
- 订阅日志 "N contracts subscribed in total" 改为 "sent ... awaiting confirmation"；失败日志改为按批次。
- 直接注入持仓回调的既有用例需先登记在途请求号；同一批次的帧使用同一请求号。

## 未闭合

真实 CTP/CTPOpt SDK 时序、请求号回显、空结果表达、流控返回码、Windows/MSVC、Linux 实测、完整回归（NOT_RUN）。`CTPLoader.cpp` 的 `run()` 在 `Join()` 返回后仍 `return 0`（终局靠进程 exit），未改宿主返回协议。TraderCTPOpt 其他查询（账户/委托/成交/结算/行权）同步非零后 `m_bInQuery` 仍会卡住队列，worker 与 SPI 线程间 `m_bInQuery`/`m_queQuery` 的既有数据竞争未处理——均未纳入本次范围。持仓空结果（NULL+末帧）仍发布空快照，未按 14c 改为失败（会阻断空仓账户，需另行裁定）。ParserCTPOpt `OnRspError(NULL)`/`OnRspSubMarketData(NULL,NULL)` 仍静默（不在本次 ID 范围）。R76-02/R76-03（ParserUDP/ParserXeleSkt）不在本次范围。
