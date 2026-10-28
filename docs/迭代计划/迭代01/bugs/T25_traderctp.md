# T25_traderctp（TraderCTP 交易端）疑似缺陷

> 状态一律「待审核」；本迭代不修改 `src/**`。生产文件只有 `src/TraderCTP/TraderCTP.cpp`
> （仓库里没有 `src/TraderCTP/TraderSpi.cpp`，SPI 回调就是 `TraderCTP` 自身，见 `TraderCTP.h:32`）。
> 实测判决都来自 `CoverageTests_t_TraderCTP` 单条运行（隔离运行目录、`--gtest_also_run_disabled_tests`），
> 标注「源码判据」的条目表示本 lane 没能给出确定性运行时证据。

## BUG-T25-001

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/TraderCTP/TraderCTP.cpp`（构造表 L203-217）、`src/TraderCTP/TraderCTP.h`（L174 `m_sink`、L193 `m_bdMgr`、L177-180 会话四值）
- 相关测试: `src/TestUnits/coverage/t_TraderCTP/t25_traderctp_gap_test.cpp` → `DISABLED_plugin_created_trader_has_indeterminate_sink`（未崩，见「备注」）；可确定性复现的 NULL 面在 BUG-T25-003
- 输入/前置条件: 经插件 C 入口 `createTrader()` 拿到对象后，**不调用** `registerSpi()` 就调用任何会打日志或取合约的路径（本条走 `orderInsert`，L471-475 的 `write_log(m_sink, ...)`）。
- 期望行为: 构造完成后所有指针成员都是确定的空值，未登记 sink 时走「静默丢弃日志」这条已存在的分支（L32-33）。
- 实际行为: 构造表列了 12 个字段，`m_sink`、`m_bdMgr`、`m_frontID`、`m_sessionID`、`m_orderRef`、`m_lDate`、`m_uLastQryTime`、`m_lastQryTime`、`m_hInstCTP`、`m_funcCreator` 都不在表里且类内没有初值 ⇒ 未登记 sink 时 `write_log` 读的是**不确定指针**，`m_sink==NULL` 的保护形同抛硬币；同理 `m_funcCreator` 未初始化时 `connect()`（L310）会跳到垃圾地址。
- 复现步骤: 单独跑上面那条 `DISABLED_` 用例（先涂脏 64 块 512B 归还的堆，再 `createTrader()` + `orderInsert()`）。
- 影响范围: 真实装配顺序是 `WtTraderBroker` 先 `init` 再 `registerSpi`，所以常规链路不命中；但任何「先调用后登记」的调用顺序（含崩溃恢复路径与新增适配器）都是未定义行为。`m_frontID/m_sessionID/m_orderRef` 同族问题已由 lane T2 登记（`bugs/T2_traderctp_xele_ctpopt.md` 的 T2-CTP-01），本条只补**指针成员**这一半。
- 备注: 不阻塞覆盖率（启用用例全部先 `registerSpi`）。本 lane 实测这条**没有稳定崩**（3 次 rc=0，归还页面恰好是 0），所以缺陷按源码判据登记、级别定 Major 而不是 Blocker；确定性崩溃证据请见 BUG-T25-003。修复方向：给这批成员加类内初值 `= NULL / = 0`。

## BUG-T25-002

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/TraderCTP/TraderCTP.cpp`（`orderInsert` L485-501、`extractEntrustID` L1373-1395）
- 相关测试: `t25_traderctp_gap_test.cpp` → `order_insert_ignores_malformed_entrust_id_when_usertag_set`（启用，已钉死当前行为）
- 输入/前置条件: 状态 `WS_ALLREADY`，`WTSEntrust` 带 usertag 但 `entrustID` 不是三段式（例如 `"no-hash-at-all"`，缺 `#`）。
- 期望行为: 与 `orderAction`（L557-558 判 `extractEntrustID` 返回值后 `return -1`）一致：委托号不可解析就拒绝下单，不发请求、不写缓存。
- 实际行为: L493 `extractEntrustID(entrust->getEntrustID(), fid, sid, orderref);` **丢弃返回值**，三个出参保持未初始化，L495 直接把未初始化的 `orderref` 用 `fmt::format_to` 写进 `req.OrderRef` 发给柜台；同时 L500 把这个畸形 key 写进 eid 缓存。用例实测到「请求照发（`ReqOrderInsert` 计数 +1）+ 缓存里出现 `"no-hash-at-all" → "TAG-BAD"`」；`OrderRef` 的具体取值取决于栈上残留（所以本条不断言该字段，未初始化读本身即 UB）。
- 复现步骤: 启用该用例即可；`src/TestUnits/coverage/t_TraderCTP/t25_traderctp_gap_test.cpp:785`。
- 影响范围: 直接涉及**委托报文的委托号**——上 Layer 传错 entrustID 形态（或被截断）时不是拒绝而是发一个随机委托号，后续回报反查（eid 缓存）也会错位，属于下单正确性/资金风险面。
- 备注: 阻塞「把 L490-501 写成可证伪断言」的覆盖收口（本 lane 用「请求照发 + 缓存 key」这条可复现的侧面向后钉）。修复方向：`if (!extractEntrustID(...)) return -1;`。

## BUG-T25-003

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/TraderCTP/TraderCTP.cpp`（`OnRspError` L1159-1162、`OnRspQrySettlementInfo` L1093-1096）
- 相关测试: `t25_traderctp_gap_test.cpp` → `DISABLED_rsp_error_with_null_sink_crashes`、`DISABLED_settlement_info_last_row_without_payload_crashes`
- 输入/前置条件:
  1. `registerSpi(NULL)`（`m_sink` 明确为 NULL）后厂商抛回一个 `OnRspError`；
  2. 结算单查询先收到 1 行有 `Content` 的回报，随后以「`pSettlementInfo==NULL` + `bIsLast==true`」收尾（CTP 无更多数据时的常见形状）。
- 期望行为: 与同文件其余 6 处回调一致——先判 `m_sink` 再打日志；回报对象为空时不取字段，只按「无内容」处理。
- 实际行为: `OnRspError` 无条件 `m_sink->handleTraderLog(...)`；`OnRspQrySettlementInfo` 的 `if (bIsLast && !m_strSettleInfo.empty())` 里无条件 `atoi(pSettlementInfo->TradingDay)` 且无条件 `m_sink->...`。实测两条用例都是 **SIGSEGV（rc=139）**。
- 复现步骤: `--gtest_also_run_disabled_tests --gtest_filter=T25TraderCTPTest.DISABLED_rsp_error_with_null_sink_crashes`（另一条同理）。
- 影响范围: 柜台在断链/流控时正是靠 `OnRspError` 报错的；只要 SPI 注册晚于首包（或将来有调用方传 NULL），错误上报本身就把进程打死——错误处理路径成为崩溃源。结算单同理。
- 备注: 阻塞覆盖率（这两个分支只能以 DISABLED 形式存在）。修复方向：统一走 `write_log(m_sink,...)`，并在 L1093 加上 `pSettlementInfo != NULL` 判定。

## BUG-T25-004

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/TraderCTP/TraderCTP.cpp`（`authenticate` L1476-1488）
- 相关测试: `t25_traderctp_gap_test.cpp` → `authenticate_return_code_is_swallowed_and_logs_nothing`（启用）
- 输入/前置条件: `m_pUserAPI->ReqAuthenticate(&req, id)` 同步返回非 0（柜台侧流控/报文错误/超过发送频率都会给非 0）。
- 期望行为: 与 `doLogin`(L441)、`logout`(L461)、`orderInsert`(L543)、`orderAction`(L581)、`confirm`(L1467) 一致：记一条 `Sending ... failed: <code>` 日志，并把失败暴露给上层（返回非 0 或走 `onLoginResult(false,...)`）。
- 实际行为: L1485 `m_pUserAPI->ReqAuthenticate(&req, genRequestID());` 返回值直接丢弃，函数恒返回 0。用例实测 `ret_=7` 时 **日志条数为 0**、状态停在 `WS_LOGINING`，之后只能等一个永不到来的 `OnRspAuthenticate`。
- 复现步骤: 启用该用例；`t25_traderctp_gap_test.cpp:753`。
- 影响范围: 认证请求被柜台同步拒绝时登录流程静默挂住（既不报错也不重试），运维侧看不到任何线索。
- 备注: 不阻塞覆盖率（分支本身已覆盖，只是行为不合契约）。修复方向：判返回值 + `write_log(LL_ERROR, ...)`。

## BUG-T25-005

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/TraderCTP/TraderCTP.cpp`（`connect` 的 worker L334-365、`disconnect` L368-379、各 `OnRsp*` 的 `m_bInQuery=false`：L810/L903/L936/L1084/L1103/L1133）
- 相关测试: `t25_traderctp_gap_test.cpp` → `DISABLED_disconnect_hangs_when_no_response_clears_in_query`（实测 HANG，>22s 存活被 KILL）；复位路径由启用用例 `connect_with_stub_creator_wires_spi_fronts_and_worker_sends_query` 覆盖
- 输入/前置条件: `connect()` 起 worker → 队列里执行掉一个查询（`m_bInQuery` 被置 true）→ 该查询的回报始终没有以 `bIsLast` 回来（断流、报文丢弃、柜台只回 `OnRspError`）。
- 期望行为: `m_bInQuery` 必须有超时/复位兜底（例如发送时间戳 + 超时重开，或 `OnRspError`/`OnFrontDisconnected` 里清零），`disconnect()` 必须能在有限时间内收尾。
- 实际行为: 只有 6 个 `OnRsp*` 的 `bIsLast` 分支会清零；`OnRspError`（L1159）与 `OnFrontDisconnected`（L713）都不清 ⇒ worker 在 `if(m_queQuery.empty() || m_bInQuery)` 上 1ms 步长无限自旋，`disconnect()` 的 `m_thrdWorker->join()` 永不返回。用例在 22s 看门狗下仍存活，判定 HANG。附带：此时 CPU 常驻一个忙等核心，且队列里剩余的查询项全部不被执行。
- 复现步骤: `--gtest_also_run_disabled_tests --gtest_filter=T25TraderCTPTest.DISABLED_disconnect_hangs_when_no_response_clears_in_query`。
- 影响范围: 断线/流控场景下进程退出会卡死在交易通道上（`WtRunner` 停机流程走 `disconnect`），并伴随一个空转线程；这是停机路径的硬阻塞。
- 备注: 阻塞覆盖率（本条不能以启用用例存在，`L337-343` 的自旋支只能靠有界轮询间接覆盖）。修复方向：worker 内加 `m_lastQryTime` 超时复位，或把 `OnRspError/OnFrontDisconnected` 也作为复位点。

## BUG-T25-006

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/TraderCTP/TraderCTP.cpp`（`makeOrderInfo` L1205-1215、`makeEntrust` L1281-1291、`wrapTimeCondition` L141-165）
- 相关测试: `t25_traderctp_gap_test.cpp` → `order_flag_stays_zero_when_time_condition_is_neither_gfd_nor_ioc`（启用）
- 输入/前置条件: 回报里的 `TimeCondition` 既不是 `THOST_FTDC_TC_GFD('3')` 也不是 `THOST_FTDC_TC_IOC('1')`（CTP 还有 GFS'2'/GTD'4'/GTC'5'/GFA'6'，交易所转单/条件单会用到）。
- 期望行为: 两条通道对未知时间条件给出同一个确定值（要么都映射到 `WTC_*` 再降回 `WOF_NOR`，要么记日志后拒绝推送）。注意产品自己写了 `wrapTimeCondition` 的两个重载却一处都没用。
- 实际行为: 两个函数都是 `if GFD / else if IOC`，**没有 else** ⇒ 字段保持对象初值：`WTSOrderInfo` 的 `m_orderFlag` 在 `_WTSOrderStruct` 构造表（`WTSTradeDef.hpp:318-328`）里没有初值、placement new 的值初始化把整块内存清零 ⇒ 推送出去是 `0`（不是 `WOF_NOR('0')/FAK/FOK` 任何一个合法值）；`WTSEntrust` 的构造表有 `m_orderFlag(WOF_NOR)` ⇒ 同一回报委托通道拿到的是 `WOF_NOR`。用例把这两个值分别钉死（0 与 48）。
- 复现步骤: 启用该用例；`t25_traderctp_gap_test.cpp:1736`。
- 影响范围: 下游按 `getOrderFlag()` 分派的重发/改单/风控判断在未知时间条件下会「三个分支都不命中」；订单与委托两条通道还互相不一致，排查时容易误判成数据丢失。
- 备注: 不阻塞覆盖率（`L141-165` 那 22 行是另一回事：内部链接 + 0 调用点，见 manifest 留白面）。修复方向：补 else 显式 `setOrderFlag(WOF_NOR)` 或直接使用已有的 `wrapTimeCondition`。

## BUG-T25-007

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/TraderCTP/TraderCTP.cpp`（`OnRspUserLogin` L758-782）、`src/Share/WtKVCache.hpp`（`init` L119-186、`put` L211-232）
- 相关测试: `t25_traderctp_gap_test.cpp` → `login_warns_and_continues_when_cache_dir_is_not_writable`（启用，钉住告警面）+ `DISABLED_order_insert_after_failed_cache_init_crashes`（实测 SIGSEGV，rc=139）
- 输入/前置条件: `flowdir/local/<broker>/` 已存在但**不可写**（目录被挂成只读、磁盘配额、上一轮以 root 跑过留下的属主），随后正常登录 → 状态推到 `WS_ALLREADY` → 下一笔带 usertag 的委托。
- 期望行为: `m_eidCache.init(...) / m_oidCache.init(...)` 的 false 返回值必须被处理：报错并终止登录（或退化为「不缓存 usertag」的安全模式），绝不能让缓存进入「有对象、无 block」的半初始化状态。
- 实际行为: L766/L779 只看回调日志、不接返回值；`init` 失败时 `_cache._file.reset()` 而 `_cache._block` 一直是 NULL。`get()` 因为只查 `_indice`（空表）会安全回落 `""`，但 `put()` 索引未命中时第一步就读 `_cache._block->_size` ⇒ 空指针解引用。启用用例实测：告警各 1 条、登录链照常 `WS_LOGINED` 并压入确认查询（即产品认为一切正常）；DISABLED 用例实测下单即 SIGSEGV。
- 复现步骤: 先跑 `--gtest_also_run_disabled_tests --gtest_filter=T25TraderCTPTest.DISABLED_order_insert_after_failed_cache_init_crashes`。
- 影响范围: flowdir 不可写是很常见的部署问题（换机器、权限、上一版残留），一旦命中就是「登录成功、第一笔委托直接崩」，且崩在下单路径上；同类「init 返回值被吞」在 `CTPLoader/CTPOptLoader` 的夹具里也存在，建议一并排查。
- 备注: 阻塞覆盖率（崩溃面只能 DISABLED）。修复方向：`put` 先判 `_cache._block`；`TraderCTP` 侧接住 init 的 false 并 `onLoginResult(false, "cache init failed", 0)`。

## BUG-T25-008

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/TraderCTP/TraderCTP.cpp`（`OnFrontDisconnected` L713-718、`OnRspQrySettlementInfoConfirm` L806-843、`OnRspSettlementInfoConfirm` L845-859）
- 相关测试: `t25_traderctp_gap_test.cpp` → `front_disconnect_resets_state_and_logout_reports_reason`（启用，已把当前行为钉死）
- 输入/前置条件: 先跑到 `WS_ALLREADY` → `OnFrontDisconnected(200)`（状态回到 `WS_NOTLOGIN`）→ 一个**在途**的确认查询回报随后到达（`ConfirmDate >= m_lDate`）。
- 期望行为: 断线后所有在途回报都应该被丢弃（或先把状态机锁在 `WS_NOTLOGIN`，等 `OnFrontConnected` → 重新认证登录），不能仅凭一条迟到的回报把通道报成「可用」。
- 实际行为: `OnRspQrySettlementInfoConfirm` 不检查任何前置状态，`uConfirmDate >= m_lDate` 成立就 `WS_CONFIRMED → WS_ALLREADY` 并 `m_sink->onLoginResult(true, "", m_lDate)`；`OnRspSettlementInfoConfirm` 只认 `WS_CONFIRM_QRYED`。用例实测断线后确实回到 `WS_ALLREADY`、`isConnected()==true`。
- 复现步骤: 启用该用例；`t25_traderctp_gap_test.cpp:1825`（断线后被迟到回报抬回就绪的那两句在 `:1840-1842`）。
- 影响范围: 断线瞬间上层会收到一次虚假的「登录成功」，交易/UI 侧可能在通道实际不可用时继续下单（下单侧只判 `WS_ALLREADY`，L471/L553）；与 BUG-T25-005 叠加时更糟（断线后 worker 仍卡在 inQuery）。
- 备注: 不阻塞覆盖率。修复方向：这两个回调加 `if (m_wrapperState == WS_LOGINED / WS_CONFIRM_QRYED)` 之类的前置判定，或引入断线代次（epoch）丢弃陈旧回报。

## BUG-T25-009

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/TraderCTP/TraderCTP.cpp`（`confirm` L1450-1474、`OnRspQrySettlementInfoConfirm` L833、`queryConfirm` L1422-1448）
- 相关测试: `t25_traderctp_gap_test.cpp` → `confirm_send_failure_logs_error_and_state_stays_confirm_qryed`、`query_confirm_send_failure_is_logged_inside_worker_item`、`confirm_guard_rejects_when_api_is_gone_or_state_moved_on`（均启用）
- 输入/前置条件: 结算确认报文 `ReqSettlementInfoConfirm` 同步返回非 0（`ret_=8`），或 `m_pUserAPI` 已被摘掉（`confirm()` 返回 -1）。
- 期望行为: 确认请求发送失败要让上层可感知（返回码上抛 / `onLoginResult(false, ...)` / 重新排队），而不是停在中间状态等一个没发出的请求的回报。
- 实际行为: `confirm()` 的返回值被 L833 的调用点丢弃，只留一条日志；状态停在 `WS_CONFIRM_QRYED`。用例实测此时 `onLoginResult` 计数为 0（既不成功也不失败），只有等柜台**自己**补一个 `OnRspSettlementInfoConfirm` 才能进 `WS_ALLREADY`——而请求根本没发出去，所以该回报不会来，登录链无限等待。`queryConfirm` 的发送失败（L1437-1441）同样只记日志、不重排；`OnRspQrySettlementInfoConfirm` 里 `pSettlementInfoConfirm==NULL` 时也是「发一次确认就不再看结果」。
- 复现步骤: 启用上述三条；`t25_traderctp_gap_test.cpp:1135 / 1156 / 1886`。
- 影响范围: 登录流程在「已登录未就绪」这一步可能永久卡住且对外无提示（`isConnected()` 恒 false，上层只会看到「一直连不上」），是可用性而非正确性问题，故定 Major 不定 Blocker。
- 备注: 不阻塞覆盖率。修复方向：发送失败时把确认动作重新压 `m_queQuery`（有 1s 节流）或直接 `onLoginResult(false, ...)`。

## BUG-T25-010

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/TraderCTP/TraderCTP.cpp`（`disconnect` L368-372 对比 `queryAccount` L596-605 等 6 个入队点）
- 相关测试: `t25_traderctp_gap_test.cpp` → `disconnect_without_worker_only_queues_release`（启用，钉住「无 worker 时队列里留下一个永不执行的 release」）；竞态本身没有确定性用例
- 输入/前置条件: `connect()` 已起 worker（L336），此时调用 `disconnect()`。
- 期望行为: 与其它 6 个入队点一致，`m_queQuery.push(...)` 必须在 `StdUniqueLock lock(m_mtxQuery)` 保护下做——worker 正是用同一把锁做 `front()/pop()`（L354-359）。
- 实际行为: L370 的 `m_queQuery.push([this]{ release(); });` 是**唯一不加锁**的入队点，与 worker 的 `empty()/front()/pop()` 并发访问 `std::queue`（链表容器）构成数据竞争；同文件其余入队点全部加锁，可见是漏写而不是设计。
- 复现步骤: 源码判据（`TraderCTP.cpp:370` 无 `StdUniqueLock`，对照 L597/L620/L643/L667/L691/L1430）。本 lane 尝试过用「worker 正在消费 + 主线程 disconnect」放大命中概率，但没有做成可重复的判决，因此**不声称已实测复现**，只登记嫌疑位置与判据。
- 影响范围: 只在 `connect()` 之后调用 `disconnect()` 时存在（生产停机流程每次都走）：概率性破坏队列（丢项/踩坏链表节点），最坏表现是 worker 拿到一个已被破坏的 handler；与 BUG-T25-005 的挂死叠加时更难归因。
- 备注: 不阻塞覆盖率（行已覆盖，竞态不可证）。修复方向：给 L370 补同款 `StdUniqueLock` 作用域。

## 分级汇总

- Blocker：BUG-T25-002（委托号未初始化即发单）、BUG-T25-003（错误上报路径 SIGSEGV ×2）、BUG-T25-005（disconnect 挂死，实测 HANG）、BUG-T25-007（缓存 init 被吞 → 下单 SIGSEGV，实测）
- Major：BUG-T25-001（未初始化指针成员，源码判据）、BUG-T25-006（时间条件漏映射、两通道不一致）、BUG-T25-008（迟到回报复活「已就绪」）、BUG-T25-009（确认失败后登录链无限等待）、BUG-T25-010（`disconnect` 入队不加锁，仅判据）
- Minor：BUG-T25-004（`ReqAuthenticate` 返回码被吞）
