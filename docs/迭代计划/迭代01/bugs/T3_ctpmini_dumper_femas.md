# T3 - TraderCTPMini / TraderDumper / ParserFemas 疑似缺陷（第六波·收发族）

> 本轮只写测试，未改任何 `src/**` 生产代码，也未为了变绿放宽断言。
> 以下为源码、接口和测试源码复核；本次没有编译或运行测试，也没有加载厂商库。
> 测试名称、断言和 `DISABLED` 只说明测试源码当前写法，不代表本次执行结果。
> 目录内自带的「改进建议」类文档：三模块目录下均无（`src/README.md` 是唯一泛化说明），
> 因此这些条目没有已知的既有记录。

分级：**Blocker** = 崩溃/资金订单风险/使目标模块无法达到 100%；**Major** = 边界行为与契约不符；**Minor** = 日志/提示/风格。

---

## T3-001 `ParserFemas::init` 加载失败后调用空函数指针

- 状态: 已合并（并入 `BUG-R105-003`）
- 严重级别: Blocker
- 模块/文件: `src/ParserFemas/ParserFemas.cpp:118-131`
- 相关测试: `femas_parser.DISABLED_init_needs_vendor_dylib_and_null_creator_crashes`
- 输入/前置条件: 配置可完成 flowdir 创建，但 `load_library` 或 `get_symbol` 返回空（本次未检查本机库状态）
- 期望行为: `DLLHelper::load_library`/`get_symbol` 任一为空时应记日志并 `return false`
- 实际行为: `m_funcCreator = (FemasCreator)DLLHelper::get_symbol(m_hInst, creatorName)` 无校验，
  紧接 `m_pUserAPI = m_funcCreator(path.c_str())`；如果 creator 为空，会进行无效函数指针调用。creator 返回空 API 时，后续也无判空。
- 复现步骤: 令库加载或 symbol 查询返回空再调 `init(config)`；本次未运行该路径，也未检查本机库状态（现有用例 DISABLED）
- 影响范围: 缺库/缺符号时存在无效函数指针调用风险；实际信号、进程后果和部署环境本次未确认
- 备注: 与 R105-003 的 ParserFemas 子项重复；T3-006 是 TraderCTPMini 交易 API 的相似空 creator 风险，但属于不同接口及代码路径。
- 审查裁定: 与 R105-003 中 ParserFemas 的缺库/缺符号路径完全重复。保留本段作 lane 交叉索引；主修复项及证据边界以 R105-003 为准。

## T3-002 `OnRspUserLogin` 在 `bIsLast` 分支解引用可能为空的 `m_pUserAPI`

- 状态: 已撤回（无 API 时收到登录回调的前提不成立）
- 严重级别: Blocker
- 模块/文件: `src/ParserFemas/ParserFemas.cpp:179-193`（L183 `m_pUserAPI->GetTradingDay()`）
- 相关测试: `femas_parser.DISABLED_login_rsp_last_true_calls_null_api`、
  正向半边由 `femas_parser.login_rsp_skipped_when_not_last` 兜住
- 输入/前置条件: 未 init（`m_pUserAPI==NULL`）时收到 `bIsLast=true` 的登录回报
- 期望行为: 登录回调由已创建并启动的厂商 API 发出；若 teardown 允许在途回调，应由生命周期同步/排空保证对象有效
- 实际行为: 若绕过生命周期直接在 `m_pUserAPI==NULL` 对象上调用该方法，会解引用空指针；这一前提不是已证实的正常 API 路径
- 复现步骤: `ParserFemas p; p.OnRspUserLogin(&login, &rsp, 1, true);` 是对未初始化实例的直接调用，只能证明方法在非法前提下不防御
- 影响范围: 原记录提出的登录/订阅链路崩溃未被产品调用路径支持；release 期间迟到回调另待验证
- 备注: 本条已撤回，不作为产品缺陷或覆盖率阻塞项。
- 审查裁定: `m_pUserAPI` 由 `init()` 创建，`connect()` 才调用厂商 `Init()`；常规厂商登录回调来自已创建并启动的 API。现有 DISABLED 用例直接对未初始化对象调用回调，不能证明产品会进入该状态。注销期间是否存在在途回调是另一个生命周期竞态，当前资料未验证，不由本条确认。

## T3-003 `ParserFemas::IsErrorRspInfo` 恒 false，登录错误响应误走成功

- 状态: 已合并（并入 `BUG-R105-004`）
- 严重级别: Major
- 模块/文件: `src/ParserFemas/ParserFemas.cpp:405-408`（`*/**pRspInfo` 参数被注释掉）
- 相关测试: `femas_parser.heartbeat_and_sub_rsp_are_sink_optional_noops`（按现状断言「什么都不做」）
- 输入/前置条件: 任意 `CUstpFtdcRspInfoField{ErrorID!=0}`
- 期望行为: `ErrorID!=0` 应被识别为错误，登录错误不得继续走成功通知/订阅分支；具体日志由回调层负责
- 实际行为: 恒 `false` → `OnRspUserLogin` 把非零错误响应当成功，继续读取交易日、通知登录并订阅；
  `OnRspSubMarketData` 的错误分支不可达。另 `OnRspError` 只是调用后丢弃结果，本身没有日志/事件处理，
  不能把它的无输出单独归因于该返回值
- 复现步骤: 用非零错误响应和有效 API stub 调用 `OnRspUserLogin(..., bIsLast=true)`，观察其仍走成功通知/订阅；现有测试没有该断言，本次未运行
- 影响范围: 有效 API 会话中的登录错误会走成功路径；订阅回报当前两侧均为空实现，不能把其无输出归因于恒 false。
- 备注: 简报里的「错误码映射」这一项在本模块尚未实现；本条已并入 R105-004。
- 审查裁定: 与 R105-004 的 Femas 子项重复，归并由 R105-004 维护。测试源码仅直接调用 `OnRspError`/`OnRspSubMarketData` 并观察无日志；本次未运行，且没有启用用例验证非零错误登录被当作成功。

## T3-004 `ParserFemas` 的连接状态返回值不符合接口语义；退订行为待确认

- 状态: 待验证（连接状态契约静态可确认；退订要求待确认）
- 严重级别: Major
- 模块/文件: `src/ParserFemas/ParserFemas.cpp:141-149`（`connect` 恒 `return true`）、
  `L447-449`（`unsubscribe` 空）、`L212-215`（`OnRspUnSubMarketData` 空）、`L330-339`（订阅失败分支空）
- 相关测试: `femas_parser.connect_without_api_reports_command_not_sent`、`disconnect_without_api_reports_command_not_sent`、`is_not_connected_without_api`，以及 `R105FemasTest.is_connected_tracks_front_connection_callbacks`。
- 期望行为: 按 `IParserApi` 注释，`connect`/`disconnect` 返回命令是否成功发送；`isConnected` 表示是否已连接
- 实际行为: 无 API 时 `connect`/`disconnect` 仍返回 true；`isConnected` 只检查 API 指针，而 API 在 `init()` 时已创建、尚未连接。
  `unsubscribe` 与 `OnRspSubMarketData` 为空实现，但接口本身给了默认空实现，尚无 Femas 必须支持退订的独立产品契约
- 复现步骤: 对未 init 的对象调用 `connect()`/`disconnect()`，均未发出命令却返回 true；初始化后、前置连接事件到达前，`isConnected()` 已返回 true
- 影响范围: 调用者无法用返回值区分请求是否发送，也无法用 `isConnected()` 得到实际连接状态
- 备注: IParserApi.h 明确写了连接/断开返回值语义及 `isConnected` 语义。旧测试固定了无 API 时返回 true，现已改为正确契约；重建后无 API 连接和断开两例 FAIL，未连接状态控制 PASS。注入 API 的连接回调状态测试也 FAIL，表明仅有 API 指针时即报告已连接、断线后仍报告已连接；真实厂商连接未运行。修复应跟踪连接事件状态，并在未发送命令时返回 false。退订能力另待确认。

## T3-006 `TraderCTPMini::init` 直接解引用 `params->get(...)` 且厂商符号无校验

- 状态: 待验证（静态失败路径成立；正向厂商路径未验证）
- 严重级别: Blocker
- 模块/文件: `src/TraderCTPMini/TraderCTPMini.cpp:102-105`、`L122-132`；`connect` 的 `L169`
- 相关测试: `ctpmini.DISABLED_init_and_connect_need_vendor_dylib`
- 输入/前置条件: 配置缺 `front`/`broker`/`user`/`pass` 任一键，或没有 `thosttraderapi` 动态库
- 期望行为: 用 `getCString` 或判空后返回 false
- 实际行为: `params->get("front")->asCString()` 对 NULL 返回值直接解引用；
  `m_funcCreator` 取空后 `connect()` 里 `m_funcCreator(...)` 空指针调用
- 复现步骤: 传入只含 `flowdir` 的 variant 调 `init`
- 影响范围: 缺键时存在空指针解引用风险；缺库/缺符号时 `connect()` 存在无效函数指针调用风险。具体进程后果和生产配置可达性本次未验证。
- 备注: TraderCTPMini 与 ParserFemas 虽都通过动态库加载 API，但此处是交易 API，和 R105-003 的行情 API 不是同一登记项。
- 审查裁定: `WTSVariant::get()` 在缺键/非对象时返回 NULL，源码随后直接 `asCString()`；creator 在 `connect()` 中无判空调用。静态失败路径成立。当前 DISABLED 用例没有运行；缺键空指针路径可用有效 variant 直接触发，成功连通则需要可控 creator/API，不应把两类前提混为一谈。

## T3-007 `OnRspQryTradingAccount` 末笔对空容器调 `clear()`

- 状态: 待验证（末笔无数据/错误回报前提需核对厂商契约）
- 严重级别: Blocker
- 模块/文件: `src/TraderCTPMini/TraderCTPMini.cpp:683-720`（`L713-719`）
- 相关测试: `ctpmini.DISABLED_qry_account_last_without_record_crashes`；
  正向半边在 `ctpmini.rsp_qry_trading_account_builds_balance_and_clears` 里绕开（首笔必须成功建容器）
- 输入/前置条件: 末笔错误回报或零行结果以 `bIsLast==true` 结束，且之前没有成功账户行
- 期望行为: 与同文件 `OnRspQryOrder:L925` / `OnRspQryTrade:L895` 一样 `if (m_ayFunds)` 再 clear
- 实际行为: `m_ayFunds` 仍为 NULL 时 `m_ayFunds->clear()` → 空指针解引用
- 复现步骤: `TraderCTPMini t; CThostFtdcRspInfoField bad{ErrorID=4}; t.OnRspQryTradingAccount(&acc,&bad,1,true);`
- 影响范围: 该回调序列若由厂商 API 产生会命中空容器风险；零行/错误末帧是否为柜台合法响应尚未确认
- 备注: 该缺陷单独阻塞 `L713-719` 的一支。
- 审查裁定: 若首个回调就是 `bIsLast=true` 且没有成功账户行，`m_ayFunds` 保持构造时的 NULL，末尾 `clear()` 是源码可确认的空指针解引用；现有启用用例先构造容器，DISABLED 用例只写了前提，未执行。尚未用厂商契约确认“零行/错误响应如何结束查询”；直接调用回调即可测试该分支，不需要真实厂商动态库。

## T3-008 `OnRspOrderAction` 错误判定反转；空响应指针另需契约验证

- 状态: 本地 stub 回调复现；真实柜台与 NULL 输入待验证
- 严重级别: Major
- 模块/文件: `src/TraderCTPMini/TraderCTPMini.cpp:669-681`
- 相关测试: `ctpmini.order_action_success_does_not_push_trader_error`、`ctpmini.order_action_error_pushes_cancel_error_once`；旧测试的反向期望已拆除。
- 输入/前置条件: 撤单回报且 `pRspInfo` 非空（成功或错误）；另有未验证的 NULL 响应输入
- 期望行为: 有错误才 `onTraderError`，成功不应上抛；NULL 时安全返回或给出受控失败
- 实际行为: `if (IsErrorRspInfo) {} else { 造 WTSError 并上抛 }` —— 成功路径反而报错；
  且 else 分支无条件读 `pRspInfo->ErrorMsg`，厂商传 NULL 即崩
- 复现步骤: 启用测试源码以非空成功/错误响应直接调用，可观察分支反转；NULL 响应路径没有测试且厂商字段可空性未核实
- 影响范围: 撤单成功会被上层当作交易通道错误；风险模块可能因此拒单/停摆
- 备注: 同文件 `OnRspOrderInsert:L655-667`、`OnErrRtnOrderInsert:L1308-1320` 对 `pRspInfo` 也无判空
  （经 `makeError`）。
- 审查裁定: `IsErrorRspInfo` 对 NULL/零错误码返回 false；本函数 false 分支却创建并上报错误并读取 `pRspInfo->ErrorMsg`，反转与 NULL 风险由源码静态确认。修正后本机两条非空响应 stub 测试均 FAIL：成功回报被报错、错误回报没有上报。NULL 响应与真实柜台未运行。

## T3-009 TraderAdapter 登录失败后的未初始化交易日会影响刷新路径

- 状态: 待验证（仅保留失败登录刷新路径）
- 严重级别: Major
- 模块/文件: `src/TraderDumper/TraderAdapter.cpp:38-45,158-193,413-421`（`_date` 未在构造/初始化时设值；失败登录会置 `_done=true`，刷新路径随后读取 `_date`）；
  旁列的 `TraderCTPMini`/`ParserFemas` 成员必须按生命周期判定，见审查裁定
- 相关测试: `ctpmini.make_entrust_id_formats_front_session_ref_and_increments`（必须先显式注入
  front/session/orderRef 才能断言精确串，就是绕开这个不确定值）；
  `adapter_mgr.add_get_and_duplicate_rules`（`isAnyAlive()` 只在 `run()` 赋值之后才断言）
- 期望行为: 所有标量/裸指针/原子量在初始化列表里给初值
- 实际行为: `TraderAdapter` 构造未初始化 `_date`；登录失败回调会置 `_done=true`，manager 的 `refresh()` 会对已 done 适配器调用 `queryPosition/queryFund`，两处先读 `_date` 再决定是否访问 API，失败路径可读未初始化值
- 复现步骤: 需要让适配器走登录失败回调，再由 `TraderAdapterMgr::refresh()` 刷新；本次未运行该路径
- 影响范围: 登录失败后的主动刷新可能使用未初始化日期作门控，结果不确定
- 备注: 本条不再把所有未初始化成员一概当作常规路径缺陷。Mini 的 `m_frontID/m_sessionID/m_orderRef/m_lDate` 在成功登录回调中赋值；`m_hInstCTP/m_funcCreator` 在 init 中赋值。Femas 的 sink/API 相关字段依赖接口生命周期。Dumper 的 `_live_cnt` 虽未在构造函数初始化，但 `Dumper::run()` 先调用 manager.run()，后者按适配器数赋值再进入等待循环；因此“run(true) 因随机计数挂死”不成立。直接在 run 前调用 `isAnyAlive()` 的契约未定义，不作为本条影响证据。
- 审查裁定: 仅保留 `_date` 在失败登录刷新路径的风险；测试源码只在成功登录后测试刷新，当前未运行。其余原列字段属于待按具体调用顺序验证的生命周期初始化问题，不支撑本条原有的整体影响叙述。

## T3-010 `TraderAdapter` 回调无条件解引用 `_trader_api`

- 状态: 已撤回（标准失败初始化路径不会触发所述 API 回调）
- 严重级别: Blocker
- 模块/文件: `src/TraderDumper/TraderAdapter.cpp:143`（`handleEvent(WTE_Connect,0)`→`login`）、
  `L171`（`onLoginResult(true)`→`queryPositions`）、`L185/L193`（`queryFund/queryPosition`）、
  `L212`（`onRspAccount` 的 `if(!_done) _trader_api->queryTrades()`）、
  `L249`（`onRspTrades` 末尾**无任何守卫**的 `queryOrders()`）
- 相关测试: `trader_adapter.DISABLED_on_rsp_trades_always_derefs_null_api`、
  `trader_adapter.DISABLED_login_and_connect_success_paths_deref_null_api`
- 输入/前置条件: `init()` 失败（模块加载失败）但 `Dumper::config` 仍把该适配器登记并 `run()`
- 期望行为: SPI 回调只能由已创建的交易 API 发出；release/异步回调边界需由生命周期协议保证
- 实际行为: 多个方法不判空调用 `_trader_api`，但标准失败加载路径中 API 指针为空时，`run()` 直接返回 false，不会调用 `connect()` 或由该 API 发出回调
- 复现步骤: 原记录通过直接调用 `onRspTrades` 人工构造空 API 回调；这不是已证实的标准 API 调用路径
- 影响范围: 不能从当前证据推出配置错误后厂商回调会触发空指针；release 后的在途回调/悬空指针需另行核实
- 备注: 现有 DISABLED 用例只手动调用 SPI 方法；结合 `TraderAdapter::run()` 的空 API 早退，原问题前提已撤回。
- 审查裁定: `TraderAdapter::run()` 在 `_trader_api==NULL` 时立即返回 false，不会注册 sink 或调用 `connect()`；因此“模块加载失败仍登记”不会产生此对象的交易 API 回调。DISABLED 用例直接在未绑定 API 的对象上手动调用 SPI 回调，不是标准厂商回调路径。若需审查 release 后的在途回调，应另立并验证注销/排空时序；本条给出的空 API 因果撤回。

## T3-011 `Dumper::config` 关键节点不判空，且忽略适配器 init 返回值

- 状态: 待验证（源码失败路径成立；测试源码未在本次运行）
- 严重级别: Blocker
- 模块/文件: `src/TraderDumper/Dumper.cpp:22-31`（`root` 未判空）、`L34-35`（`cfgBF = root->get("basefiles")`
  后直接 `cfgBF->get("session")`）、`L70-71`（`cfg = root->get("traders")` 后直接 `cfg->size()`）、
  `L86-92`（`adapter->init(...)` 返回值被丢弃后仍 `addAdapter`）
- 相关测试: `dumper.DISABLED_config_without_basefiles_or_traders_crashes`（缺节崩溃）、
  `TempCwd.dumper_config_registers_adapters_even_when_module_load_failed`（按现状断言 size+1，未放宽）
- 输入/前置条件: ① 配置文件不存在/JSON 语法错误 → `root==NULL`；② 缺 `basefiles` 或 `traders` 任一节；
  ③ `traders` 里模块名写错
- 期望行为: ①②给出可读错误并返回 false；③跳过该通道（不要登记）
- 实际行为: ①②空指针解引用；③失败适配器进入 `_adapters`，`run()` 时 `_live_cnt` 计数与永不 `decAlive`
  的通道对不上 → `Dumper::run(true)` 的 `for(;;)` 死循环
- 复现步骤: `Dumper d; d.config("{}", false, "/tmp/x/");`（缺 traders）
- 影响范围: 配置解析入口；也让本轮无法用「缺节」这种自然负例去覆盖 `config`
- 备注: `WTSLogger::init` 与 `loadSessions/loadCommodities/loadContracts` 侧都有判空，唯此处没有。
- 审查裁定: 对 NULL 配置根、缺少 `basefiles`、缺少 `traders` 的解引用，以及失败 init 后仍 addAdapter，均可从源码确认；run() 又先将 live count 设为 map size，而失败 adapter 的 run() 不递减计数，故同步等待可一直保持 alive。缺节测试是 DISABLED；已有启用测试源码覆盖缺模块仍登记，但本次没有运行。上述配置负路径不要求厂商库。

## T3-012 `refresh_span=0` 时后台刷新线程无间隔空转

- 状态: 待验证（零间隔的产品语义需确认）
- 严重级别: Major
- 模块/文件: `src/TraderDumper/Dumper.cpp:103-125`
- 相关测试: `dumper.DISABLED_run_once_loops_forever_and_run_threaded_spawns_worker`
- 输入/前置条件: `config.refresh_span: 0` 后调用 `run(false)`
- 期望行为: 配置为零时拒绝配置、采用非零默认值或明确禁用周期刷新，避免无间隔轮询
- 实际行为: worker 每轮执行 `sleep_for(seconds(0))` 后立即刷新并重试；代码没有最小值校验
- 复现步骤: 将配置的 `refresh_span` 设为 0 并启动 `run(false)`；本次未运行线程路径
- 影响范围: 若零值允许到达后台 worker，会造成无节流循环；严重程度取决于配置契约
- 备注: `run(true)` 的同步等待会等到所有已登记通道完成，可能是该 API 的批量落地语义；没有独立停止契约时，不把它不可由同线程 `release()` 中断认定为缺陷。现有 DISABLED 用例混合描述同步等待和线程模式，也未提供零间隔运行证据。
- 审查裁定: 零间隔是源码确定行为，`refresh_span` 的合法范围未在接口/示例中明确；产品契约确认前保留待验证。R114 测试源码存在零间隔 worker 路径但本次未运行。

## T3-013 `WtHelper::get_cwd` 忽略 `getcwd` 返回值

- 状态: 待验证（失败路径静态成立；本次未运行）
- 严重级别: Minor
- 模块/文件: `src/TraderDumper/WtHelper.cpp:24-35`
- 相关测试: `wt_helper.cwd_and_module_dir_are_round_trippable`（只在正常长度路径上断言）
- 输入/前置条件: 当前目录路径长度 ≥ 255（临时目录/CI workspace 常见）
- 期望行为: `getcwd` 返回 NULL 时报错或换 `PATH_MAX` 缓冲
- 实际行为: `char buffer[255]; getcwd(buffer, 255);` 返回值被丢弃，随后 `_cwd = buffer` 读取未写入的缓冲
- 复现步骤: 在深目录里首次调用 `WtHelper::get_cwd()`
- 影响范围: 模块目录探测（`TraderAdapter::init` 用它找 `traders/` 子目录），影响插件加载路径
- 备注: 同族 `WtDtServo/WtDtHelper` 也有同名 helper（BUG 汇总里已有多处 helper 复制的讨论）。
- 审查裁定: `getcwd/_getcwd` 失败时返回值被忽略，局部缓冲区随后被当作 NUL 结尾字符串读取，静态失败路径成立；正常目录测试不覆盖该失败条件，本次未运行。

## T3-014 ParserFemas/TraderCTPMini 请求组装用裸 `strcpy` 拷到定长厂商字段

- 状态: 待验证（超长字段风险静态成立；正常配置边界未验证）
- 严重级别: Major（仅在超出厂商字段容量的配置下）
- 模块/文件: `src/ParserFemas/ParserFemas.cpp:354-359`；
  `src/TraderCTPMini/TraderCTPMini.cpp:289-294`、`L311-314`、`L334-340`、`L433-449`、`L1335-1341`
- 相关测试: 未覆盖超长字段边界；现有正向请求路径需要有效 API，测试源码没有注入超长配置值
- 期望行为: `strncpy`/带长度校验（同仓 `wt_strcpy` 已提供）
- 实际行为: `strcpy(req.BrokerID, m_strBroker.c_str())` 等直接把用户配置写进 `TThostFtdc*Type` 定长数组
- 复现步骤: 将 `user` 配成超过对应 SDK 定长数组容量的字符串；例如 CTPMini 的 UserID 字段为 16 字节（含结尾 NUL）
- 影响范围: 栈上 `req` 结构溢出（配置错误即 UB，非外部输入面）
- 备注: 原记录将其列为风格问题；复核发现它属于条件性定长数组越界风险。
- 审查裁定: 风险不只是风格：CTPMini SDK 头文件把 BrokerID/UserID/Password 等定义成固定长度数组，超长配置会越界写入栈上请求结构。当前没有长度校验或启用边界用例；标准柜台配置是否会超限未经验证，按条件性内存安全风险记录，不声称常规配置可触发。

---

## 分级汇总

| 级别 | 数量 | 条目 |
| --- | --- | --- |
| 当前待审核/待验证 | 9 | T3-004、T3-006、T3-007、T3-008、T3-009、T3-011、T3-012、T3-013、T3-014 |
| 已合并 | 2 | T3-001→R105-003、T3-003→R105-004 |
| 已撤回 | 2 | T3-002、T3-010 |

（原始编号共 13 条；本次裁定后仍待审核/待验证 9 条、合并 2 条、撤回 2 条。T3-005 号段未使用。T3-004 中的退订空实现因接口默认允许 no-op，尚未认定为产品缺陷。）

## 是否阻塞覆盖率目标

- 原记录把多条失败路径统称为必须依赖厂商 shim，范围过宽。T3-006 的缺键路径、T3-007 的查询回调、T3-011 的配置缺节/缺模块路径都可通过本地参数或直接回调进入，不需要真实厂商库；测试是否应启用需按断言/死亡测试策略另行决定。
- T3-001/T3-003 已合并到 R105；T3-002/T3-010 已撤回，不计为覆盖率阻塞证据。
- 本次只审查源码与测试源码，未运行本 lane 测试；不能据此给覆盖率、运行时或厂商行为验收结论。
