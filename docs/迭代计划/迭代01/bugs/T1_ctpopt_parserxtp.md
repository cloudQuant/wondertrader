# lane T1 缺陷登记 — TraderCTPOpt / ParserXTP

只登记，不修改 `src/**`，不改断言凑绿。用例侧凡被钉死的「当前行为」都在「现状」注明；
所有规避手段（探针硬化成员、DISABLED 改名）不掩盖缺陷本体。

前置说明：`docs/迭代计划/迭代01/manifests/api-brief-T07.md` 实际内容是 TraderXTP / TraderXTPXAlgo / TraderYD，
**不含 ParserXTP**；本 lane 的 ParserXTP 符号全部来自主控指定目录下的源码直读（ParserXTP.h/.cpp）。

---

## TraderCTPOpt（`src/TraderCTPOpt/TraderCTPOpt.cpp`、`TraderCTPOpt.h`）

- **T1-CTPOpt-01 构造函数漏初始化指针与标量成员，多处首用即读未初始化值（UB）**
  - 位置：`TraderCTPOpt.cpp:220-236`（初始化列表缺 `m_ayExecOrds`、`m_bdMgr`、`m_frontID`、`m_sessionID`、`m_orderRef`、`m_hInstCTP`、`m_funcCreator`）；`TraderCTPOpt.h:211-227`
  - 现状：`OnRspQryExecOrder` L1078 `if (NULL == m_ayExecOrds)` 读未初始化指针——非 NULL 垃圾时对野指针 `append`；
    `makeEntrustID` L386 打印未初始化的 `m_frontID/m_sessionID`，`m_orderRef` 是平凡默认构造的 `std::atomic<uint32_t>`，`fetch_add` 本身即 UB。
  - 用例侧规避：`CtpProbe::harden()` 在断言前把这些成员钉为确定值（派生类写 protected 成员，合法缝），否则断言不可复现。
  - 严重级别：高（UB，随机崩溃/随机委托号）
  - 状态: 待审核

- **T1-CTPOpt-02 `orderAction()/orderActionOpt()` 漏判 `m_pUserAPI == NULL`，与同类入口不一致**
  - 位置：`TraderCTPOpt.cpp:704-711`（只判 `m_wrapperState != WS_ALLREADY`）、`531-534`；对照 `orderInsert` L606、`queryAccount` L744 等均判 API 指针
  - 现状：状态被推到 `WS_ALLREADY` 而 API 指针为空（如 `release()` 之后未复位状态的时序）时，
    L733/L567 直接 `m_pUserAPI->ReqOrderAction/ReqExecOrderAction` 空指针解引用。
  - 用例侧规避：`unlogged_state_rejects_every_request` 在 ALLREADY 状态下**只调用**仍带 API 判空的入口；
    `orderAction/orderActionOpt` 的 ALLREADY+NULL 组合面未启用（会崩），留给主控裁定是否补 `DISABLED_`。
  - 严重级别：中
  - 状态: 待审核

- **T1-CTPOpt-03 `OnRspOrderAction/OnRspExecOrderAction` 分支语义反转，且 `pRspInfo==NULL` 时解引用**
  - 位置：`TraderCTPOpt.cpp:1098-1110`、`1028-1040`
  - 现状：`if (IsErrorRspInfo(pRspInfo)) { /*空*/ } else { WTSError::create(..., pRspInfo->ErrorMsg); m_bscSink->onTraderError(error); }`——
    真错误被吞掉；无错误（或 pRspInfo 为 NULL，`IsErrorRspInfo(NULL)==false`）反而解引用 `ErrorMsg` 并把「撤单成功」当交易错误上报；
    else 分支创建的 `WTSError` 在 sink 为空时也未 `release()`（对象池泄漏）。
  - 用例侧处理：`DISABLED_TraderCTPOpt.order_action_rsp_null_rspinfo_derefs_errormsg` 记录 NULL 崩溃面，未启用。
  - 严重级别：中（错报+漏报双向失真；生产柜台一般传非空，故不判高）
  - 状态: 待审核

- **T1-CTPOpt-04 `OnRspQrySettlementInfo` 收尾不判 `m_bscSink`，且末次回调 `pSettlementInfo` 可能为空**
  - 位置：`TraderCTPOpt.cpp:1145-1162`（L1160 `m_bscSink->onRspSettlementInfo(atoi(pSettlementInfo->TradingDay), ...)`）
  - 现状：文件其余回调都用 `if (m_bscSink)` 守卫；此处若「已拼出文本 + 末帧 pSettlementInfo==NULL（纯错误收尾）」则双重解引用。
  - 用例侧规避：`query_rsp_streams_fill_arrays_and_reset_state` 只在 sink 已注册、末帧非空时驱动该路径。
  - 严重级别：低
  - 状态: 待审核

- **T1-CTPOpt-05 `connect()` 对 `m_funcCreator` 无判空，库缺失即 NULL-call；worker 线程与查询队列耦合**
  - 位置：`TraderCTPOpt.cpp:305-363`（L311 `m_pUserAPI = m_funcCreator(...)` 紧跟 L312 `->RegisterSpi(this)`）
  - 现状：`init()` 只做 `load_library/get_symbol`（可安全失败，L266-276 不崩），但 `connect()` 假定符号必得；
    CI 无 `libsoptthosttraderapi_se*.dylib` 时直接崩，且 L331-362 拉起以 `m_bStopped` 为唯一出口的常驻线程。
  - 用例侧处理：`DISABLED_TraderCTPOpt.login_success_and_connect_require_real_ctp_lib`；`init` 的正/负配置路径单独启用覆盖。
  - 严重级别：中（阻塞可测性）
  - 状态: 待审核

- **T1-CTPOpt-06 `confirm()/queryConfirm()/authenticate()/doLogin()` 为 private，测试无法直接钉守卫分支**
  - 位置：`TraderCTPOpt.h:47-56`（`private:` 段），对照 `IsErrorRspInfo/makeOrderInfo/...` 在 `protected:`（L161-183）
  - 现状：`authenticate()` L1783-1795 完全不判 `m_pUserAPI`（仅被 `login→OnFrontConnected` 链调用，同文件其他 Req* 都有判空）；
    `confirm/queryConfirm` 的守卫分支只能靠 `OnRsp*` 回调间接进入，`queryConfirm()` 守卫（L1731-1736）本轮未被直接覆盖。
  - 用例侧处理：继承探针仅覆盖 protected 面；private 面不强闯（规则 2），留给主控裁定（friend 测试缝或提为 protected）。
  - 严重级别：低（可测性 + `authenticate` 判空缺失）
  - 状态: 待审核

- **T1-CTPOpt-07 空桩回调：`OnRspError` 只留占位变量，`OnRspOrderAction` 错误分支体为空**
  - 位置：`TraderCTPOpt.cpp:1375-1378`、`1100-1103`
  - 现状：柜台错误应答 `OnRspError` 被完全忽略（`[[maybe_unused]] int x = 0;`），错误码/文案既不写日志也不透传 sink——与其他 `OnRsp*` 一致地应走 `onTraderError`。
  - 用例侧处理：`front_lifecycle_events_forward_to_sink` 用「调用后日志计数不变」把该空实现钉死为可证伪断言。
  - 严重级别：低（吞错）
  - 状态: 待审核

---

## ParserXTP（`src/ParserXTP/ParserXTP.cpp`）

- **T1-XTP-01 `init()` 对符号加载结果零判空：缺库即 NULL-call 崩溃**
  - 位置：`ParserXTP.cpp:119-132`（`DLLHelper::load_library` → `get_symbol` → `m_funcCreator(...)` 三连无校验）
  - 现状：macOS/CI 上 `libxtpquoteapi*.dylib` 不存在时 `m_funcCreator==NULL`，L131 `m_funcCreator(m_uClientID, path, ...)` 直接段错误；
    L132 `m_pUserAPI->RegisterSpi(this)` 又假定创建必成功。整个 `init` 正/负配置路径在无厂商库环境不可测。
  - 用例侧处理：`DISABLED_ParserXTP.init_and_connect_require_vendor_lib_and_leak_thread`；其余 12 条启用用例全部绕开 `init`，
    用「未持 api 指针」的公开回调面覆盖。
  - 严重级别：高（阻塞该模块全部 init 覆盖；产品侧亦无法给出可读错误）
  - 状态: 待审核

- **T1-XTP-02 `connect()` 拉起的 worker 线程 `while(true)` 无停止条件，`release/disconnect` 均不回收**
  - 位置：`ParserXTP.cpp:142-161`（线程体 L150-157）；`release` L137-140、`disconnect` L163-173 都不触碰 `_thrd_worker`
  - 现状：连接一次后进程无法正常退出（StdThread 析构 join？线程永不结束 → 挂死）。单测/嵌入方停服路径不可达。
  - 用例侧处理：并入上述 DISABLED 用例；启用用例不调用 `connect()`（规则 6：不起线程、不赌退出）。
  - 严重级别：中（挂死；仅经 connect 触发）
  - 状态: 待审核

- **T1-XTP-03 构造函数漏初始化 `m_sink/m_pBaseDataMgr/m_hInst/m_funcCreator/m_uClientID` 等**
  - 位置：`ParserXTP.cpp:81-86`（列表仅 `m_uTradingDate/m_pUserAPI/m_iRequestID`），成员声明 `ParserXTP.h:98-130`
  - 现状：未 `registerSpi` 就直接收厂商回调（如 `OnError`→`write_log(m_sink,...)`）时读野指针；
    `m_loginState` 虽有 `[[maybe_unused]]` 标记同样未初始化。
  - 用例侧规避：所有启用用例第一步显式 `registerSpi(&fake)`（registerSpi L690-696 会把两指针钉成确定值）。
  - 严重级别：中（UB，取决于回调时序）
  - 状态: 待审核

- **T1-XTP-04 `OnDepthMarketData` 期货字段哨兵值处理不一致：`settl_price/open_interest` 未经 `checkValid`**
  - 位置：`ParserXTP.cpp:332-339`
  - 现状：`pre_settle` 走 `checkValid`（DBL_MAX/FLT_MAX→0），而 `settle_price = market_data->settl_price`、`open_interest` 直拷——
    XTP 对无结算价的品种下发 DBL_MAX 时会原样进入 `WTSTickStruct.settle_price`。
  - 用例侧钉死：`depth_market_data_converts_snapshot_and_level1_queues` 断言当前行为（settl 99.5 直拷、pre_settle DBL_MAX→0），
    若修复为一致校验，该断言会红，属预期证伪。
  - 严重级别：低
  - 状态: 待审核

- **T1-XTP-05 `subscribe()` 登录前的代码缓存不可观测、`unsubscribe()` 为空实现**
  - 位置：`ParserXTP.cpp:592-607`（缓存分支）、`681-683`（空 unsubscribe）
  - 现状：未登录时前缀解析结果只写私有 `m_fitSHSubs/m_fitSZSubs`，无 getter/friend 缝；退订任何代码都无效果（真退订只存在于 DoSubscribeMD）。
  - 用例侧处理：`subscribe_before_login_only_buffers_without_api_calls` 只能钉「零 sink 交互 + 不崩」；
    前缀解析正确性依赖登录后的 DoSubscribeMD 实链路，本轮进不去，留给主控裁定。
  - 严重级别：低（可测性/功能残缺）
  - 状态: 待审核
