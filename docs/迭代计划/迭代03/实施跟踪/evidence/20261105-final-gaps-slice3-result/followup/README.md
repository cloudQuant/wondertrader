# 第 3 段 · 后续小节：例 195 启用、I03-NEW-130、I03-NEW-131（followup/）

- 来源：主代理在禁用用例处置完成后下达的三项任务。编号由主代理分配：I03-NEW-130 = BUG-R105-004（ParserCTPMini），I03-NEW-131 = BUG-T25-003（TraderCTP）。
- 日期：2026-11-05；HEAD 见 `HEAD.txt`（`ccaad010`，工作树含他人未提交改动）。
- 构建：`SCRATCH/slice3-result-build`（仓库源码，Debug，`WT_ENABLE_COVERAGE=ON`，`WT_WARNINGS_AS_ERRORS=ON`，Ninja `-j 3`）。没有构建 sanitizer 树；按主代理安排，sanitizer 由主代理执行。
- 本小节不修改迭代跟踪文档（执行跟踪.json、执行记录.md、新增问题.md、决策积压清单.md、迭代计划.md）。
- `SCRATCH` = `/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad`。

## 锁

| 文件 | 锁 |
|---|---|
| `src/ParserCTPMini/ParserCTPMini.cpp` | `lock.py slice3-result I03-NEW-130` → ok |
| `src/TestUnits/coverage/t_ParserCTPMini/s3_new130_ctpmini_rsp_error_test.cpp`（新建） | `lock.py slice3-result I03-NEW-130` → ok |
| `src/TraderCTP/TraderCTP.cpp`、`src/TraderCTP/TraderCTP.h` | `lock.py slice3-result I03-NEW-131` → ok（为新增成员同时登记了头文件） |
| `src/TestUnits/coverage/t_TraderXTP/t5_xtp_api_test.cpp` | 由 slice4-result 转交（locks.txt 中为 `BUG-I02-NEW-154+(transferred from slice4-result)`） |
| `t_ParserCTPMini/ctpmini_parser_test.cpp`、`t_TraderCTP/t25_traderctp_gap_test.cpp` | 已在本段名下（DISABLED-disposition） |

## 改动

`followup.patch` 相对本小节开始前的快照（`before.sha256`）。改后哈希见 `after.sha256`。红控构建输入哈希见 `red-build-inputs.sha256`，绿色构建输入哈希见 `green-build-inputs.sha256`；绿色构建之后 `ctpmini_parser_test.cpp` 又改过一次断言，最终状态以 `after.sha256` 为准，由 `logs/build-green2-ctpmini.log` 和最终逐例运行覆盖。

1. **例 195**：`TraderXTPTest.DISABLED_connect_calls_null_creator_and_writes_flow_dirs` 去掉前缀，失效注释改为“迭代03 最终验收复核后启用”。用例体未改，它没有显式断言，作为不崩溃回归。
2. **I03-NEW-130（产品，`ParserCTPMini.cpp`）**：`IsErrorRspInfo` 由恒返回 false 改为与 ParserCTPOpt/ParserFemas 相同：`pRspInfo` 非空且 `ErrorID != 0` 时返回 true，并写一条错误日志 `[ParserCTPMini] API response failed with error id N`（与 ParserCTPOpt 同格式）。该函数的其他行未改；文件为 BOM + 混合行尾，改动块沿用该处的 LF，其余 12 处 CRLF 保持不变。
   - 主代理原文写的是“按 ParserCTP 等同类 parser 实现”，但 **`ParserCTP::IsErrorRspInfo` 本身同样恒返回 false**（`ParserCTP.cpp:552-555`），是迭代02 BUG-R105-004 裁定所说“四个 IsErrorRspInfo 都恒 false”中的另一个。本次按已正确实现的 ParserCTPOpt/ParserFemas/ParserXTP 对齐；ParserCTP 未改（未分配，也未登记锁），请主代理决定是否另立编号。
   - 登录失败时，parser 只写错误日志，不发 WPE_Login 也不发其他失败事件，与 ParserCTPOpt 一致。IParserSpi 没有登录失败事件的约定，本次不新增。
3. **I03-NEW-130（测试）**：新建 `t_ParserCTPMini/s3_new130_ctpmini_rsp_error_test.cpp`，共 5 例。合法前置：测试侧白盒把 `m_pUserAPI` 指向 `r105_ctpmini_api_stub.h` 的空实现（与 r105 lane 相同），再走公开的 `connect()` 和 `OnFrontConnected()` 发出登录请求，然后投递应答。不调 init，不加载厂商库，不开 socket，不起线程。
   - `error_login_last_packet_does_not_enter_success_branch`：ErrorID=3 的登录末包不发 WPE_Login、不取交易日、不下发订阅，写出错误日志；之后的订阅仍只缓存。
   - `zero_error_id_login_still_succeeds`、`null_rspinfo_login_still_succeeds`：正常应答照常成功（对照组）。
   - `rejected_login_does_not_block_a_later_successful_login`：登录被拒、断线重连后再次登录成功，只出现一次 WPE_Login。
   - `rsp_error_is_logged_only_for_nonzero_error_id`：OnRspError 只对非零 ErrorID 写错误日志。
4. **例 160 保持禁用**：只更新注释。它的断言（错误应答不产生 WPE_Login）本身正确，但前置是在未 connect、`m_pUserAPI` 为空的 parser 上投递登录应答。SDK 不会在没有 API 实例时回调，属于 BUG-T4-004 NOT_BUG 口径的无效前置，所以由第 3 条的合法前置用例取代。
5. **既有断言随合同更新（`ctpmini_parser_test.cpp`，`ParserCTPMiniTest.heartbeat_and_sub_response_paths_log_only`）**：修复后该例逐例运行失败（`logs/percase1-old-assertion-heartbeat_and_sub_response_paths_log_only.log`：`sink.logs.size()` 为 3，期望 1）。原断言 `logs.size()==1` 要求 ErrorID=42 的 OnRspError/OnRspSubMarketData 不产生任何日志，固化的正是“错误应答无声”的缺陷行为。它与同族合同相冲突：Femas 的 `femas_parser_callbacks_test.cpp:325-326` 要求错误回调可观察；ParserCTPOpt 的 `r105_parserctpopt_api_test.cpp:320-341` 断言同格式的 “API response failed with error id N”。现改为更严格的断言：恰好 3 条日志，第 2、3 条为 LL_ERROR 且含 “error id 42”，事件仍为空。这是按合同收紧断言，不是放宽。**主代理确认**（2026-11-05）：原断言编码的是缺陷行为，新断言与 CTPOpt/Femas 同类用例一致且更严格。
6. **I03-NEW-131（产品，`TraderCTP.cpp`/`TraderCTP.h`）**：`OnRspQrySettlementInfo` 在末帧不再解引用 `pSettlementInfo`。头文件新增两个成员，带类内初值 0：`m_uSettleDate`（最近一条已缓存行的 TradingDay）和 `m_uSettleQryDate`（未完成请求的日期）；`querySettlement` 在原有清空缓冲处一并设置它们。末帧规则（卡14c：无 SDK 契约的空响应 fail-closed）：
   - 末帧总是结束本次查询（`m_bInQuery=false`，原有行为），并清空缓冲和两个日期，保证一次查询只给一次结果。
   - 末帧带载荷：行为不变（缓冲非空才推送，日期取末帧 TradingDay）。
   - 末帧无载荷且缓冲非空：推送已缓存内容一次，日期取最近一条缓存行的 TradingDay（没有则取请求日）。
   - 末帧无载荷、缓冲为空、非错误、且有未完成请求：推送明确的空结果，日期为请求日。唯一消费者 TestTrader 在条件变量上等待该回调，空响应不推送会让它一直等下去。
   - 错误末帧且缓冲为空：不推送。这与既有启用用例 `settlement_info_concatenates_rows_and_pushes_only_when_non_empty` 的合同一致，未改该用例。
   - 无载荷末帧写一条 LL_WARN 日志（含已缓存字节数）。
7. **I03-NEW-131（测试，`t25_traderctp_gap_test.cpp`）**：
   - 例 173 `DISABLED_settlement_info_last_row_without_payload_crashes`（原无断言）改写为启用的合同用例 `settlement_info_null_last_row_ends_query_with_cached_content`：缓存一行后的无载荷末帧结束查询，推送 “line-A” 且日期为缓存行日期；迟到的重复末帧不重推；下一次查询不拼接旧内容。
   - 新增 `settlement_info_empty_response_without_payload_reports_empty_result`：有请求、无内容时推送请求日的空结果，同一查询的重复末帧不再推送。
   - 新增 `settlement_info_error_last_frame_without_payload_reports_nothing`：错误末帧、无载荷、缓冲为空时不推送，查询闸门照常复位。

## 运行结果（`runs.jsonl`，日志 `logs/<tag>.log/.xml`）

| 阶段 | 用例 | 结果 |
|---|---|---|
| 红控（产品未改，`logs/build-red.log`） | `S3New130CtpMiniRspError.error_login_last_packet_does_not_enter_success_branch` | FAIL（WPE_Login 计数 1、交易日 20260301） |
| 红控 | `...rejected_login_does_not_block_a_later_successful_login` | FAIL |
| 红控 | `...rsp_error_is_logged_only_for_nonzero_error_id` | FAIL（无错误日志） |
| 红控 | `...zero_error_id_login_still_succeeds`、`...null_rspinfo_login_still_succeeds` | PASS（对照组） |
| 红控 | `T25TraderCTPTest.settlement_info_null_last_row_ends_query_with_cached_content` | rc=-11（SIGSEGV，即原 BUG-T25-003） |
| 红控 | `...settlement_info_empty_response_without_payload_reports_empty_result` | FAIL（未推送） |
| 红控 | `...settlement_info_error_last_frame_without_payload_reports_nothing` | PASS（对照组） |
| 绿色（`logs/build-green.log`，含产品插件 ParserCTPMini/TraderCTP 的 -Werror 构建） | 上述 8 例 + `settlement_info_concatenates_rows_and_pushes_only_when_non_empty` + 例 195 `connect_calls_null_creator_and_writes_flow_dirs` | 各 `--gtest_repeat=20`，均 20/20 |
| 绿色（`logs/build-green2-ctpmini.log`） | `ParserCTPMiniTest.heartbeat_and_sub_response_paths_log_only`（更新断言后） | 20/20 |

## 相关 lane 全量（逐例，每例独立进程；`SCRATCH/s3tools/percase.py`）

| 二进制 | 结果 | 摘要 |
|---|---|---|
| CoverageTests_t_ParserCTPMini | 30/30 通过 | `logs/percase-CoverageTests_t_ParserCTPMini-summary.json`（断言更新前一轮为 29/30，见 `logs/percase1-...-summary-before-assertion-update.json`） |
| CoverageTests_t_TraderCTP | 89/89 通过 | `logs/percase-CoverageTests_t_TraderCTP-summary.json` |
| CoverageTests_t_TraderXTP | 60/60 通过 | `logs/percase-CoverageTests_t_TraderXTP-summary.json` |

- 影响面核对（`ninja -t deps`）：包含 `ParserCTPMini.h`、`TraderCTP.h` 的目标文件只属于产品插件 `ParserCTPMini`、`TraderCTP` 和上述两个 lane，均已重编并运行。
- 运行 cwd 在 `SCRATCH/s3fu/`，运行后已删除 profraw。

## 未覆盖与提请主代理处理

- 本段未跑 sanitizer。需要在 ASan+UBSan(+LSan) 下逐例重跑：上述 lane，特别是新文件 5 例、t25 的 3 例，以及原 UBSan 位置 `TraderCTP.cpp:1688`（现已移到 1703 附近，且不再解引用）。
- 仅 macOS arm64 本机 fake/stub；真实 CTPMini/CTP SDK 的错误应答形状、结算单分帧和空响应时序未验证（卡14 的真实 SDK 部分仍为 X）。
- 同类问题未处理（未分配，需另行登记）：
  - `ParserCTP::IsErrorRspInfo` 恒返回 false（BUG-R105-004 所述四处中的另一处）。→ 第二批已修（I03-NEW-132）。
  - `TraderCTPOpt::OnRspQrySettlementInfo`（`TraderCTPOpt.cpp:1617-1619`）与修复前的 TraderCTP 代码相同，末帧无载荷时同样解引用 NULL；该文件由 slice4-result 锁定。→ 第二批已修（I03-NEW-133）。
  - `TraderCTP::OnRspQryTradingAccount`（`TraderCTP.cpp:1490-1493`（改后行号））在非错误末帧直接解引用 `pTradingAccount`，没有判空，属卡14c 同类空响应形态。→ 第二批已修（I03-NEW-134）。
  - 末帧带载荷但 `Content` 为空、且缓冲为空的正常末帧，仍按原合同不推送。主代理只要求处理 NULL 载荷，这里没有扩展，调用方在这种情况下仍可能一直等待。→ 第二批已修（I03-NEW-131 残余）。

---

# 第二批：I03-NEW-132、I03-NEW-133、I03-NEW-134、I03-NEW-131 残余（主代理 2026-11-05 第二次下达）

- 编号由主代理分配。heartbeat 断言改写已经主代理确认（见上文第 5 条）。
- 锁：
  - `src/ParserCTP/ParserCTP.cpp`、`t_ParserCTP/r105_parserctp_api_test.cpp`、`t_ParserCTP/t5_ctp_parser_test.cpp`：`lock.py slice3-result I03-NEW-132` → ok。
  - `t_TraderCTPOpt/i03s5_traderctpopt_lifecycle_test.cpp`：`lock.py slice3-result I03-NEW-133` → ok。
  - `src/TraderCTPOpt/TraderCTPOpt.cpp`：由 slice4-result 转交。
  - **`src/TraderCTPOpt/TraderCTPOpt.h` 仍由 slice4-result 锁定**（`lock.py` 返回 `LOCKED ... by slice4-result`），本批未改该头文件，处理方式见下文 NEW-133。
- 改前快照 `before2.sha256`，改后 `after2.sha256`（与绿色构建输入 `green2-build-inputs.sha256` 一致），差异 `followup2.patch`（7 个文件）。TraderCTPOpt.cpp 改前已重新阅读：slice1/slice4 最近改动的 query-status 与 lifecycle 代码均未触及，本批只改 settlement 查询与回调、析构函数末尾和文件头的局部表。

## 改动

1. **I03-NEW-132（ParserCTP.cpp）**：`IsErrorRspInfo` 与 NEW-130 相同，`pRspInfo` 非空且 `ErrorID != 0` 才是错误，并写 `[ParserCTP] API response failed with error id N`。
   - 测试追加在 `r105_parserctp_api_test.cpp`，而不是新建文件。原因是 `r105_ctp_api_stub.h` 在头文件内定义了非 inline 的 `CThostFtdcMdApi::CreateFtdcMdApi`，第二个 TU 包含它会重复定义。
   - 前置为本 lane 的合法会话：`init`（工厂指向 stub）→ `connect` → `OnFrontConnected` 发出登录请求，不调用真实 SDK。
   - 4 例，与 NEW-130 对应：错误登录不进入成功分支、ErrorID=0 照常成功、被拒后重连再登录成功、OnRspError 只对非零 ErrorID 记错误。
   - `t5_ctp_parser_test.cpp` 的 `ParserCTPTest.heartbeat_and_logout_callbacks` 原断言 ErrorID=5 的订阅应答“不得新增日志”，与 ctpmini 那条断言同型，都固化缺陷行为。按主代理对 ctpmini 的确认口径，改为恰好 2 条日志，其中第 2 条为 LL_ERROR 且含 “error id 5”，事件仍只 1 个。红控中旧产品上这条新断言失败。**主代理确认**（2026-11-05）：两处断言改写（ctpmini 与 t5）均已确认。
2. **I03-NEW-133（TraderCTPOpt.cpp）**：与 TraderCTP 相同的末帧合同（NEW-131 + 下面的残余规则）。
   - 由于头文件被锁，“最近缓存行日期 / 未完成请求日期”放在 `.cpp` 内按实例（`this`）为键的局部表，由互斥量保护。三个函数负责维护：
     - `settleBegin`：querySettlement 时登记；
     - `settleRow`：缓存行时更新；
     - `settleEnd`：末帧时取出并删除，析构时兜底删除。
   - 表与互斥量都是有意不析构的堆对象，避免退出期静态析构顺序问题；LSan 视为可达，不应报告。
   - 语义与 TraderCTP 的两个成员完全等价。→ 第三批已改为成员并删除该表（见下文）。
   - 测试 3 例，追加在 `i03s5_traderctpopt_lifecycle_test.cpp`（`S3New133*`）：
     - 缓存后无载荷末帧推送一次，且不重复、不拼接；
     - 空响应和空内容末帧给出请求日的空结果，且只给一次；
     - 错误末帧不推送。
   - 用例里注册的局部 sink 由 RAII 守卫在退出时恢复为 fixture sink，并摘掉 stub API，ASSERT 早退时同样生效。
3. **I03-NEW-131 残余（TraderCTP.cpp）**：末帧带载荷、内容为空且没有缓存时，只要有未完成请求且非错误，也给出明确的空结果。日期取末帧 TradingDay，为 0 时取请求日；没有未完成请求时不推送。无载荷末帧的规则不变，两种情况合并为同一条判断。
   - 新增 t25 `settlement_info_empty_content_last_row_reports_empty_result`。
4. **I03-NEW-134（TraderCTP.cpp `OnRspQryTradingAccount`）**：非错误末帧 `pTradingAccount == NULL` 时不解引用，推送一个空的资金数组（`onRspAccount` 收到非 NULL、size 0 的数组），并写 LL_WARN。
   - 依据该文件的查询回报约定：末帧总推送数组，持仓/订单/成交无数据时推空数组（如 t2 用例 “未知合约 ⇒ bIsLast 仍推送空数组”）。
   - 下游 `TraderAdapter::onRspAccount` 对 NULL/空数组都按“资金查询完成”推进状态，不读取资金字段，所以不会伪造任何资金值。
   - 错误末帧仍不推送（既有合同，与 i03s5 `ErrorFinalFrameThenLateErrorCallbackIsIgnored` 一致）；非末帧的 NULL 载荷不推送，也不结束查询。
   - 新增 t25 `account_null_last_frame_reports_empty_account_list`；t25 的 sink 增加 `last_account_null_` 字段，用来区分 NULL 数组与空数组。

## 运行结果（`runs.jsonl`）

| 阶段 | 用例 | 结果 |
|---|---|---|
| 红控（`logs/build-red2.log`，第二批产品未改） | R105CtpTest.s3new132 错误登录 / 被拒后再登录 / OnRspError 记错误 | 3 例 FAIL |
| 红控 | R105CtpTest.s3new132_zero_error_id_login_still_succeeds | PASS（对照组） |
| 红控 | ParserCTPTest.heartbeat_and_logout_callbacks（新断言） | FAIL |
| 红控 | S3New133NullLastRowEndsQueryWithCachedContent | rc=-11（SIGSEGV，原 1619 行解引用） |
| 红控 | S3New133EmptyResponseReportsEmptyResultOnce | FAIL |
| 红控 | S3New133ErrorLastFrameWithoutPayloadReportsNothing | PASS（对照组） |
| 红控 | t25 settlement_info_empty_content_last_row_reports_empty_result | FAIL（NEW-131 残余） |
| 红控 | t25 account_null_last_frame_reports_empty_account_list | rc=-11（SIGSEGV，原 1493 行解引用） |
| 绿色（`logs/build-green3.log`，含产品插件 ParserCTP/TraderCTP/TraderCTPOpt 的 -Werror 构建，3 个 dylib 重链接） | 上述 10 例，以及 t25 第一批 3 例和 `settlement_info_concatenates_rows_and_pushes_only_when_non_empty` | 各 `--gtest_repeat=20`，均 20/20 |

## 相关 lane 全量（逐例，每例独立进程）

| 二进制 | 结果 | 摘要 |
|---|---|---|
| CoverageTests_t_ParserCTP | 24/24 | `logs/percase2-CoverageTests_t_ParserCTP-summary.json` |
| CoverageTests_t_TraderCTP | 91/91 | `logs/percase2-CoverageTests_t_TraderCTP-summary.json` |
| CoverageTests_t_TraderCTPOpt | 60/60 | `logs/percase2-CoverageTests_t_TraderCTPOpt-summary.json` |

- 影响面：编译 `ParserCTP.cpp`、`TraderCTP.cpp`、`TraderCTPOpt.cpp` 的目标文件只属于对应的产品插件和这三个 lane，均已重编并运行。第一批的 CoverageTests_t_ParserCTPMini（30/30）与 CoverageTests_t_TraderXTP（60/60）不受第二批影响。
- 未跑 sanitizer，请主代理在 ASan+UBSan(+LSan) 下逐例重跑上述 lane。重点：TraderCTPOpt 的局部表（LSan 下应为可达）、两个原 SIGSEGV 位置。

## 第二批残余（未处理，如实说明）

- `TraderCTPOpt::OnRspQryTradingAccount`（`TraderCTPOpt.cpp` 中 `pTradingAccount->PreBalance` 一行）与修复前的 TraderCTP 相同，非错误末帧不判空就解引用。→ 第三批已修（I03-NEW-135）。
- TraderCTPOpt 的结算日期状态暂放在 `.cpp` 局部表；头文件锁转交后可改为成员。→ 第三批已完成。
- 登录失败时 ParserCTP/ParserCTPMini 只写错误日志，不发失败事件（与 ParserCTPOpt 一致；IParserSpi 没有登录失败事件约定）。
- 仅 macOS arm64 本机 stub/fake；真实 CTP/CTPOpt SDK 的错误应答、结算单分帧、空资金末帧时序均未验证。

---

# 第三批：NEW-133 状态改为成员、I03-NEW-135（主代理 2026-11-05 第三次下达）

- 主代理确认两处断言改写（ctpmini 与 t5），已在第一批第 5 条和第二批第 1 条注明。
- 锁：`src/TraderCTPOpt/TraderCTPOpt.h` 已由 slice4-result 转交（locks.txt：`BUG-I02-NEW-154+(transferred from slice4-result)`）；`lock.py slice3-result I03-NEW-135 TraderCTPOpt.cpp TraderCTPOpt.h` → ok。
- 改前快照 `before3.sha256`（TraderCTPOpt.cpp 与第二批 `after2.sha256` 一致，期间无他人改动），改后 `after3.sha256`（与绿色构建输入 `green3-build-inputs.sha256` 一致），差异 `followup3.patch`。

## 改动

1. **NEW-133 状态改为成员**：
   - `TraderCTPOpt.h` 在 `m_strSettleInfo` 后新增 `m_uSettleDate`、`m_uSettleQryDate`（类内初值 0），与 TraderCTP 写法相同。
   - `.cpp` 删除整张实例表：`SettleQueryDates`、互斥量、`settleBegin/settleRow/settleEnd`，以及第二批为它加入的 `<mutex>`/`<unordered_map>` include；析构函数中的 `settleEnd(this)` 也一并删除。头文件原有的 `<mutex>` 不是本段加的，未动。
   - `querySettlement`、行缓存与末帧改为读写成员，末帧结束时把两个成员清零。逻辑与 TraderCTP 的 `OnRspQrySettlementInfo` 逐行对应，行为不变。
   - 相对第二批之前快照的累计差异，只涉及结算单查询与回调、资金末帧两处。
2. **I03-NEW-135（`TraderCTPOpt::OnRspQryTradingAccount`）**：与 NEW-134 相同。
   - 非错误末帧 `pTradingAccount == NULL` 时推送非 NULL 的空资金数组，并写 LL_WARN。
   - 错误末帧不推送；带载荷的末帧行为不变（包括 CTPOpt 特有的 `CloseProfit + CashIn`）。
   - 新增用例 `I03S5TraderCTPOptLifecycle.S3New135NullAccountLastFrameReportsEmptyAccountList`，覆盖：NULL/ok 与 NULL/NULL 末帧各推一个空数组；错误末帧与非末帧不推送；带载荷末帧推 1 条（对照）。

## 运行结果

| 阶段 | 用例 | 结果 |
|---|---|---|
| 红控（`logs/build-red3.log`，产品为第二批状态） | S3New135NullAccountLastFrameReportsEmptyAccountList | rc=-11（SIGSEGV，原 `pTradingAccount->PreBalance`） |
| 绿色（`logs/build-green4.log`，含产品插件 TraderCTPOpt 的 -Werror 构建） | S3New135 + S3New133 三例（成员化之后） | 各 `--gtest_repeat=20`，均 20/20 |
| lane 全量逐例 | CoverageTests_t_TraderCTPOpt | 61/61（`logs/percase3-CoverageTests_t_TraderCTPOpt-summary.json`） |

- 影响面（`ninja -t deps`）：包含 `TraderCTPOpt.h` 的目标文件只属于产品插件 TraderCTPOpt 和 CoverageTests_t_TraderCTPOpt lane（4 个测试 TU + 产品 TU），均已重编并运行。
- 未跑 sanitizer；仅 macOS arm64 本机 stub。
