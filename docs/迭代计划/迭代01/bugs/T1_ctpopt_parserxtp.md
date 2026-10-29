# lane T1 缺陷登记 — TraderCTPOpt / ParserXTP

只登记，不修改 `src/**`，不改断言凑绿。用例侧被钉死的当前行为应与产品契约分开判断。

审查说明：以下结论来自本轮源码、接口和测试代码静态核对；本轮未运行测试或厂商 API，也未动态复现崩溃/挂起。带有外部 SDK 参数条件的条目保留为待验证，不据此宣称生产事故已复现。

前置说明：`docs/迭代计划/迭代01/manifests/api-brief-T07.md` 实际内容是 TraderXTP / TraderXTPXAlgo / TraderYD，不含 ParserXTP；本 lane 的 ParserXTP 符号来自主控指定目录下的源码直读（`ParserXTP.h/.cpp`）及本地 XTP 头文件。

---

## TraderCTPOpt（`src/TraderCTPOpt/TraderCTPOpt.cpp`、`TraderCTPOpt.h`）

- **T1-CTPOpt-01 构造函数漏初始化 `m_ayExecOrds`，正常行权单查询回调会读取不确定指针**
  - 判定：确认产品缺陷；原登记把多项生命周期后赋值的字段一并描述成“首用即 UB”，范围过宽。
  - 源码锚点：构造函数 `TraderCTPOpt.cpp:220-236` 初始化了 `m_ayOrders` 等数组，却漏掉头文件声明的 `m_ayExecOrds`（`TraderCTPOpt.h:220-227`）。`queryOrdersOpt()` 仅在 API 存在且状态为 `WS_ALLREADY` 时发起执行单查询（`.cpp:576-601`）；回调 `OnRspQryExecOrder()` 首次检查 `NULL == m_ayExecOrds`，随后可能创建并追加（`.cpp:1068-1095`）。该指针在构造、`init()`、登录状态转换中均无确定赋值，因此正常查询路径读到不确定值。
  - 测试前提与边界：覆盖测试的 `CtpProbe::harden()` / `R61OptProbe::harden()` 显式将 `m_ayExecOrds` 置空，避免由构造函数带来的不确定状态；这可以验证回调后续逻辑，不能验证真实对象构造安全。相关调用在 `cov_traderctpopt_spi_test.cpp:62-80,1047-1055`、`r61_traderctpopt_api_test.cpp:62-100,1024-1028`。
  - 其他被漏列字段：`m_frontID/m_sessionID/m_orderRef/m_lDate` 在成功登录回报中赋值（`.cpp:887-899`），`m_bdMgr` 由非空 `registerSpi()` 赋值（`.cpp:397-406`），`m_hInstCTP/m_funcCreator` 由 `init()` 赋值（`.cpp:243-280`）；这些字段仍建议有安全初值，但不能据此断言标准初始化登录顺序下均在首用时未初始化。`m_lDate` 也未列在旧标题中。
  - 修复方向：至少在初始化列表中将 `m_ayExecOrds` 初始化为 `NULL`；为其余裸指针、标量和原子成员设定明确初值，并在 `registerSpi(NULL)` 清理依赖 sink 的状态。新增构造后查询测试时不要先调用 `harden()` 清除被测缺陷。
  - 严重级别：高（正常执行单查询路径存在未初始化指针读取）。

- **T1-CTPOpt-02 `orderAction()/orderActionOpt()` 只检查状态，不检查 API 指针**
  - 判定：确认产品缺陷。
  - 源码锚点：两个入口分别只检查 `m_wrapperState`（`TraderCTPOpt.cpp:531-535,704-708`），稍后直接调用 `m_pUserAPI->ReqExecOrderAction/ReqOrderAction`（`:567,:733`）。`release()` 将 API 指针置空，却不复位 wrapper 状态（`:283-303`）；`isConnected()` 只看 `WS_ALLREADY`（`:1726-1729`）。因此已就绪后调用 `release()` 可留下“connected”状态和空 API 的组合，合法 action 输入会触发空指针解引用。
  - 测试前提与边界：`unlogged_state_rejects_every_request_with_minus_one` 的 ALLREADY+NULL 段没有调用 `orderAction` 或 `orderActionOpt`（`cov_traderctpopt_spi_test.cpp:481-519`）；`connect_subscribes_topics_and_disconnect_releases_api` 使用占位 worker，不能验证真实断开后的状态或线程退出（`r61_traderctpopt_api_test.cpp:490-521`）。本结论由状态转移与空指针解引用静态确认，未动态运行该组合。
  - 修复方向：两个 action 入口都增加 API 判空；`release()/disconnect()` 原子化复位就绪状态，并确保 `isConnected()` 与实际 API 生命周期一致。
  - 严重级别：中。

- **T1-CTPOpt-03 撤单响应的成功/失败分支反置；空 `pRspInfo` 也会被解引用**
  - 判定：确认产品缺陷。该条同时覆盖 `OnRspOrderAction` 与 `OnRspExecOrderAction`；其中 `OnRspOrderAction` 的空错误分支不要再作为 T1-CTPOpt-07 的第二个独立问题登记。
  - 源码锚点：`IsErrorRspInfo()` 对空指针返回 false、对非零 `ErrorID` 返回 true（`TraderCTPOpt.cpp:1694-1700`）；两个回调却在 true 分支留空，在 false 分支读取 `pRspInfo->ErrorMsg` 并上报 `WTSError`（`:1028-1040,:1098-1110`）。所以真错误被吞，`ErrorID==0` 被当作撤单错误上报，空 `pRspInfo` 落入 false 分支后解引用。
  - 测试前提与边界：旧 R61 测试固化了成功错报、错误静默的反向预期；现拆为 order/exec action 的零码成功与非零错误四条独立契约用例。重建后四条均 FAIL：成功仍发错误，失败未发对应错误。空指针崩溃用例仍为 `DISABLED_TraderCTPOpt.order_action_rsp_null_rspinfo_derefs_errormsg`，本轮未运行；本地资料也没有证明真实柜台在该回调契约下会传 NULL。
  - 修复方向：成功回报不构造错误；非空错误回报以正确 error code/message 上报；空 `pRspInfo` 明确安全忽略或记录兜底错误。WTSError 的所有权由 `ITraderSpi::onTraderError` 文档未明确，本条不把未 `release()` 单独定性为泄漏；修复时应按接口的实际所有权约定处理。
  - 严重级别：中（错误漏报和成功错报）。

- **T1-CTPOpt-04 结算单收尾在无 sink 或空末帧时仍解引用**
  - 判定：源码确认缺少保护；“末帧为空且之前已有文本”是否符合当前柜台 SDK 回调契约，待厂商文档或可控 API 证据验证。
  - 源码锚点：数据累积仅在 `!IsErrorRspInfo(pRspInfo) && pSettlementInfo` 时进行（`TraderCTPOpt.cpp:1153-1156`），但有累积文本且 `bIsLast` 时，无条件读取 `pSettlementInfo->TradingDay` 并调用 `m_bscSink`（`:1158-1161`）。`querySettlement()` 会在发起新查询前清空累积字符串（`:828-845`）。
  - 测试前提与边界：`query_rsp_streams_fill_arrays_and_reset_state` 先注册 sink，并用同一个非空结算结构体驱动非末帧与末帧（`cov_traderctpopt_spi_test.cpp:1072-1083`）；没有覆盖 sink 为空或“前面累积成功、最后一帧错误且数据指针为空”。本轮未运行异常注入测试，且本地头文件只有回调签名，未给出空参数保证。
  - 修复方向：收尾前检查 sink；日期应仅从非空有效回调字段读取，或在累积首帧保存日期；空末帧时按明确错误策略结束查询，不对空指针解引用。补齐“累计后空错误末帧”与 sink 生命周期测试。
  - 严重级别：低/待验证（代码防护缺口明确，外部空参数可达性待证）。

- **T1-CTPOpt-05 `connect()` 不检查 creator/API 返回值；查询 worker 没有可达的停止路径**
  - 判定：两处产品缺陷均可由当前源码控制流确认。
  - 源码锚点：`init()` 未检查 `load_library/get_symbol` 结果且固定返回 true（`TraderCTPOpt.cpp:243-280`）；`connect()` 无条件调用 `m_funcCreator`，随后无条件 `RegisterSpi/Subscribe*/RegisterFront`（`:305-328`），缺库、缺符号或 creator 返回空 API 均可能崩溃。构造函数将 `m_bStopped=false`（`:220-236`），该字段全文件只有构造初始化与 worker 的 `while (!m_bStopped)` 检查（`:331-362`）；`disconnect()` 将 `release` 入队后马上 `join()`，但从未置停止标志（`:365-376`）。worker 执行 release 后仍继续循环，join 无法正常返回。队列的 `disconnect()` 写入也没有像查询入口那样持有 `m_mtxQuery`，需同时审查同步。
  - 测试前提与边界：`R61_TraderCTPOpt.connect_subscribes_topics_and_disconnect_releases_api` 注入 fake creator 并预置空操作 worker，实际断开后由用例手动 drain queue（`r61_traderctpopt_api_test.cpp:490-521`）；它证明 stub 路径的请求装配，不验证真实 worker 的停止/`join()`。本轮未在缺失厂商库或真实线程路径上运行。
  - 修复方向：`init()` 对库句柄、符号和创建结果逐层判空并返回失败；`connect()` 失败时不注册/解引用空 API。为 worker 提供受同步保护的 stop 状态或可关闭队列，先排空/取消查询、释放 API，再唤醒并 join；禁止在无法停止的线程上 join。
  - 严重级别：高（线程退出阻塞及连接失败崩溃）。

- **T1-CTPOpt-06 private 方法本身不可被派生探针直接调用**
  - 状态: 已撤回（当前证据只支持测试可达性改进；未确认标准调用链存在独立产品缺陷）
  - 判定：可测性观察，不是已确认的产品缺陷；旧条目将 `authenticate()` 无独立判空扩大为生产缺陷，证据不足。
  - 源码锚点：四个方法确实位于 `TraderCTPOpt.h:47-56` 的 `private` 区域。`queryConfirm()` 与 `confirm()` 各自检查 API 和状态（`TraderCTPOpt.cpp:1731-1781`）；`login()` 在调用 `authenticate()` 前检查 API（`:413-427`）。`doLogin()`、`authenticate()` 的其他调用来自认证/登录状态链（`:870-899`），当前证据没有建立标准调用链下 API 可空的时序。
  - 测试前提与边界：继承探针不能暴露 private 成员；状态机测试通过公开回调间接走到 `confirm()`（`cov_traderctpopt_spi_test.cpp:595-634`）。不能因此把 private 改成 protected，也不能声称这些分支已逐一覆盖。
  - 建议：若需补覆盖，优先用 API 替身和公开登录/回调链驱动状态；除非存在独立消费者契约，不为测试单独扩大生产可见性。若后续证明异步回调可与 release 并发，再为 `authenticate/doLogin` 设计生命周期同步及 null 处理。
  - 严重级别：无（测试可达性改进项）。

- **T1-CTPOpt-07 `OnRspError` 空实现；引用的 action 错误空分支与 T1-CTPOpt-03 重复**
  - 判定：`OnRspError` 未处理通用错误是独立的错误可观测性缺口；`OnRspOrderAction` 错误分支已由 T1-CTPOpt-03 覆盖，重复，不单独登记。
  - 源码锚点：`OnRspError()` 只保留 `[[maybe_unused]] int x = 0`（`TraderCTPOpt.cpp:1375-1378`）。同一 CTP SPI 的 `TraderCTP::OnRspError()` 会通过 sink 写出 request id、error id 和 message（`src/TraderCTP/TraderCTP.cpp:1159-1162`），接口 `ITraderSpi` 提供 `handleTraderLog()`（`src/Includes/ITraderApi.h:106-109`）。
  - 测试前提与边界：`front_lifecycle_events_forward_to_sink` 调用 `OnRspError(NULL,...)` 后要求日志计数不变（`cov_traderctpopt_spi_test.cpp:534-555`），这是对当前空实现的断言，不证明产品契约正确。现有对照代码直接解引用参数，但本轮无厂商契约材料证明 `pRspInfo` 必非空。
  - 修复方向：对有效错误响应记录 request/error code/message；先定义并处理空 `pRspInfo` 的行为。将测试改为断言错误可观测性，并把 action 错误断言集中到 T1-CTPOpt-03。
  - 严重级别：低（异步错误可能被吞）。

---

## ParserXTP（`src/ParserXTP/ParserXTP.cpp`、`ParserXTP.h`）

- **T1-XTP-01 `init()` 不检查动态库、符号及 API 创建结果**
  - 判定：确认产品缺陷（失败路径缺少安全返回）；崩溃结论是静态控制流推导，本轮未动态复现。
  - 源码锚点：`init()` 将 `load_library` 结果传入 `get_symbol`，随后无判空调用 `m_funcCreator` 并解引用创建结果执行 `RegisterSpi`（`ParserXTP.cpp:119-134`），无论成功与否都返回 true。
  - 测试前提与边界：`DISABLED_ParserXTP.init_and_connect_require_vendor_lib_and_leak_thread` 注释记录缺库风险，但用例包含 `init()` 与 `connect()` 两条路径且本轮未运行（`cov_parserxtp_spi_test.cpp:535-548`）；其余 SPI 用例通过 `ParserXTPMirror` 绕过真实初始化（`t27_seam.hpp:384-427`），不能作为 init 成功证据。
  - 修复方向：校验库句柄、symbol、API 返回值；失败时释放已获取资源并返回 false，不启动连接流程。以注入的 DLL loader/API creator 替身覆盖各失败分支。
  - 严重级别：高（配置失败可能崩溃，且错误被报告为初始化成功）。

- **T1-XTP-02 `connect()` 工作线程没有停止/回收路径**
  - 判定：确认生命周期缺陷。
  - 源码锚点：`connect()` 起线程后在 `while(true)` 中调用 `_asyncio.run_one()`（`ParserXTP.cpp:142-161`）；`release()` 只调用 `disconnect()`（`:137-140`），`disconnect()` 只注销 SPI、释放 API、置空 API 指针（`:163-173`），不停止 io_context、不 reset work guard、不 join/reset `_thrd_worker`。成员 `StdThread` 是 `std::thread` 的别名（`Share/StdUtils.hpp:23-24`），因此工作线程会留在后台；对象销毁时若仍 joinable，标准线程析构也不能安全完成。
  - 测试前提与边界：禁用测试明确因 worker 无法停止而不运行（`cov_parserxtp_spi_test.cpp:536-548`）。启用测试通过镜像预置非空占位 `_thrd_worker`，刻意绕过创建线程的分支（`t27_seam.hpp:384-427`）。本轮未动态运行 connect/disconnect。
  - 修复方向：使用停止标志和可唤醒的 executor；disconnect/release 顺序应停止接收新任务、停止或排空 io_context、唤醒并 join 线程、释放 API，析构负责兜底回收并保证幂等。
  - 严重级别：高（线程泄漏，且析构有终止进程风险）。

- **T1-XTP-03 构造函数未初始化若干原始成员；对正常 adapter 生命周期的危害尚未证明**

- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
  - 判定：构造状态不完整属源码事实；原登记将“未 registerSpi 即收到厂商回调”当成已证实产品路径，证据不足，按条件风险保留。
  - 源码锚点：构造函数只初始化 `m_uTradingDate/m_pUserAPI/m_iRequestID`（`ParserXTP.cpp:81-86`）；头文件还声明 `m_uClientID`、协议/网络配置字段、`m_sink/m_pBaseDataMgr`、`m_hInst/m_funcCreator`、`m_loginState`（`ParserXTP.h:98-130`）。标准 `ParserAdapter::initExt()` 的调用顺序是先 `registerSpi(this)` 再 `init()`（`src/WtCore/ParserAdapter.cpp:45-61`）；ParserXTP `init()` 为配置、库句柄和 creator 赋值（`ParserXTP.cpp:94-134`），随后才由 `connect()` 登录。因此不能把这些正常流程后赋值的配置字段均表述为标准路径上的首用 UB。`m_loginState` 在 ParserXTP 实现中没有读写使用。
  - 剩余风险与测试边界：若对象在 `registerSpi/init` 前被调用回调，`m_pBaseDataMgr` 等确有不确定读取风险；`registerSpi(nullptr)` 只清 `m_sink`，不清 `m_pBaseDataMgr`（`:690-696`），可能留下旧 manager。启用回调测试首步注册非空 sink。旧 `t27_seam.hpp` 在 `init()` 前读取未初始化的 `m_hInst/m_funcCreator` 并断言为 null，已移除这两处不安全读取；重建后 `XtpSeamF.*:XtpLoginF.*` 为 25/25 PASS、3 DISABLED。这不证明产品构造状态已修复。
  - 修复方向：在构造函数初始化所有原始成员；`registerSpi(nullptr)` 时同步清除 `m_pBaseDataMgr`。明确 API 生命周期前置条件，若回调可能早于注册/初始化，回调入口应能安全拒绝；不把测试专用镜像中“碰巧为零”当作构造保证。
  - 严重级别：低/条件风险（初始化不完整确定；标准 adapter 顺序下的实际可达性未确认）。


- 迭代02复核: TEST。构造函数确有多项仅在 init/注册后设置的成员；候选声称正常 adapter 生命周期危险不成立：标准 adapter 在 init 前 registerSpi(this)，XTP 注册时设置 BaseDataMgr。现证据只支持非标准直接调用/注销后的条件性风险，不支持正常路径产品 bug。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- **T1-XTP-04 `settl_price` 未经过 sentinel 归一化；现有证据不足以确认输入契约要求**
  - 判定：待验证，不作为已确认产品缺陷；旧条目把 `open_interest` 与浮点 sentinel 一并登记不准确。
  - 源码锚点：`checkValid()` 仅将 `DBL_MAX/FLT_MAX` 转为 0（`ParserXTP.cpp:73-79`）。快照转换中 `settl_price` 原样赋给 `settle_price`，`pre_settl_price` 则经过 `checkValid`（`:332-339`）。本地 XTP 结构头把 `settl_price` 标为 `double`，但没有提供该字段 sentinel 取值约定；`pre_total_long_positon/total_long_positon/pre_settl_price/settl_price` 位于 `期权数据` 注释段（`src/API/XTP2.2.32.2/xquote_api_struct.h:205-213`），而产品代码只在 `CC_Future` 分支转换它们，预期资产类别也需核对。持仓量 `total_long_positon` 是 `int64_t`，不能把它等同于浮点 sentinel 校验。
  - 测试前提与边界：`depth_market_data_converts_snapshot_and_level1_queues` 输入正常 `settl_price=99.5`、持仓量 `777`，并只为 `pre_settl_price` 输入 `DBL_MAX`（`cov_parserxtp_spi_test.cpp:342-400`）。它验证当前映射，不证明 vendor 会对 `settl_price` 发送 `DBL_MAX`，也未证明持仓量的异常值语义。SDK 文档/在线行情样本未核对，本轮未运行测试。
  - 修复方向：先从 XTP 对应版本文档确认结算价及持仓量的无效值定义；若结算价也以这些浮点 sentinel 表示，则统一归一化并增加相应 fixture；持仓量按其整型契约单独校验和转换，不能直接套用浮点 `checkValid`。
  - 严重级别：待验证。

- **T1-XTP-05 `unsubscribe()` 是空实现；pre-login 缓存本身并非缺陷，但当前测试没有验证其内容**
  - 判定：退订功能缺失为确认产品缺陷；pre-login 缓存可见性是测试覆盖边界，不单独判产品缺陷。
  - 源码/接口锚点：`ParserXTP::unsubscribe(const CodeSet&)` 函数体为空（`ParserXTP.cpp:681-683`）。本地 XTP `QuoteApi` 明确提供 `UnSubscribeMarketData()`（`src/API/XTP2.2.32.2/xtp_quote_api.h:301-307`）；通用 `IParserApi` 同时定义 `subscribe` 和 `unsubscribe`（`src/Includes/IParserApi.h:120-128`）。因此当前实现对调用方任何退订集合均不产生厂商退订请求；若产品有意声明 XTP provider 不支持退订，应在公开能力/配置契约中明确，而不能把空实现当成已完成退订。
  - 前置缓存与测试边界：未登录时 `subscribe()` 缓存 SSE/SZSE 代码（`:592-607`），`DoLogin()` 在返回成功后设置交易日并调用 `DoSubscribeMD()`（`:445-494`），后者读取缓存订阅行情和逐笔（`:497-582`）；这段代码表明缓存机制有消费路径。`subscribe_before_login_only_buffers_without_api_calls` 只断言无 sink 交互，未直接断言私有缓存或登录后的重放（`cov_parserxtp_spi_test.cpp:499-519`），所以解析/重放仍需测试；不能据此断言缓存不可用。
  - 修复方向：按交易所分组调用 XTP `UnSubscribeMarketData()`，处理返回码/异步回调并更新本地订阅集合；明确退订后重连时是否需要重订。通过 fake QuoteApi 断言代码、交易所和调用次数，并增加 pre-login 缓存成功登录后的重放测试。
  - 严重级别：中（公开退订接口无效果）。
