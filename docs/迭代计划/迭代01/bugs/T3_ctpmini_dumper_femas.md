# T3 - TraderCTPMini / TraderDumper / ParserFemas 疑似缺陷（第六波·收发族）

> 本轮只写测试，未改任何 `src/**` 生产代码，也未为了变绿放宽断言。
> 以下条目均由本轮 Read/Grep 工具结果的真实行号推导，状态一律 `待审核`。
> 目录内自带的「改进建议」类文档：三模块目录下均无（`src/README.md` 是唯一泛化说明），
> 因此这些条目没有已知的既有记录。

分级：**Blocker** = 崩溃/资金订单风险/使目标模块无法达到 100%；**Major** = 边界行为与契约不符；**Minor** = 日志/提示/风格。

---

## T3-001 `ParserFemas::init` 加载失败后调用空函数指针

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/ParserFemas/ParserFemas.cpp:118-131`
- 相关测试: `femas_parser.DISABLED_init_needs_vendor_dylib_and_null_creator_crashes`
- 输入/前置条件: 配置只给 `front/broker/user/pass/flowdir`（或全默认），本机没有 `libUSTPmduserapiAF.so`
- 期望行为: `DLLHelper::load_library`/`get_symbol` 任一为空时应记日志并 `return false`
- 实际行为: `m_funcCreator = (FemasCreator)DLLHelper::get_symbol(m_hInst, creatorName)` 无校验，
  紧接 `m_pUserAPI = m_funcCreator(path.c_str())` → 空指针调用 → SIGSEGV（`RegisterSpi/RegisterFront` 之后同样无判空）
- 复现步骤: 直接构造 `ParserFemas` 后调 `init(config)`（本机必崩，故用例保持 DISABLED）
- 影响范围: 任何非 Windows/缺厂商库环境的加载即崩；也让 `init` 的前半段（flowdir 默认值、
  `create_directories`、module 默认值）在本轮无法进入，直接压低本模块可达覆盖率
- 备注: 不阻塞其余分支；建议加空指针守卫 + 失败返回 false（不实施）。同源问题见 T3-006。

## T3-002 `OnRspUserLogin` 在 `bIsLast` 分支解引用可能为空的 `m_pUserAPI`

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/ParserFemas/ParserFemas.cpp:179-193`（L183 `m_pUserAPI->GetTradingDay()`）
- 相关测试: `femas_parser.DISABLED_login_rsp_last_true_calls_null_api`、
  正向半边由 `femas_parser.login_rsp_skipped_when_not_last` 兜住
- 输入/前置条件: 未 init（`m_pUserAPI==NULL`）时收到 `bIsLast=true` 的登录回报
- 期望行为: 与 `connect/disconnect/subscribe` 一样先判 `if(m_pUserAPI)`
- 实际行为: 无判空直接解引用 → 崩溃
- 复现步骤: `ParserFemas p; p.OnRspUserLogin(&login, &rsp, 1, true);`
- 影响范围: 登录成功回报是唯一写入 `m_uTradingDate` 的地方，崩溃即整条订阅链路（`SubscribeMarketData`）不可达
- 备注: 阻塞 `L183-191` + `L368-403` 的覆盖。

## T3-003 `ParserFemas::IsErrorRspInfo` 被写死返回 false，错误码全部被吞

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/ParserFemas/ParserFemas.cpp:405-408`（`*/**pRspInfo` 参数被注释掉）
- 相关测试: `femas_parser.heartbeat_and_sub_rsp_are_sink_optional_noops`（按现状断言「什么都不做」）
- 输入/前置条件: 任意 `CUstpFtdcRspInfoField{ErrorID!=0}`
- 期望行为: `ErrorID!=0` 视为错误并记日志/上抛（同族 `TraderCTPMini.cpp:1300-1306` 就是这么写的）
- 实际行为: 恒 `false` → `OnRspError`/`OnRspSubMarketData` 走「无错误」分支，
  `OnRspUserLogin` 把失败当成功继续订阅
- 复现步骤: 构造 `ErrorID=100` 的回报喂 `OnRspError`，sink 收不到任何日志
- 影响范围: 登录/订阅失败静默；行情通道「显示已连接但无数据」这类现场问题无法定位
- 备注: 简报里的「错误码映射」这一项在本模块等于不存在，本轮未强行造断言。

## T3-004 `ParserFemas` 生命周期函数无返回值语义 / 退订是空实现

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/ParserFemas/ParserFemas.cpp:141-149`（`connect` 恒 `return true`）、
  `L447-449`（`unsubscribe` 空）、`L212-215`（`OnRspUnSubMarketData` 空）、`L330-339`（订阅失败分支空）
- 相关测试: `femas_parser.lifecycle_without_api_reports_not_connected`、
  `femas_parser.subscribe_before_login_only_caches_codes`
- 期望行为: `m_pUserAPI==NULL` 时 `connect` 应返回 false；`unsubscribe` 应下发 `UnSubMarketData`
- 实际行为: 无 API 也报「连接命令发送成功」；退订请求被丢弃；`OnRspSubMarketData` 的成功/失败两侧都是空语句
- 复现步骤: 默认构造后 `connect()` → true、`isConnected()` → false（两者自相矛盾）
- 影响范围: 上层 `ParserAdapter`/`WtDtServo` 依据 `connect()` 返回值决定是否重连
- 备注: 用例已按现状断言（`EXPECT_TRUE(connect())` + `EXPECT_FALSE(isConnected())`），
  两条断言同时成立本身就是契约冲突的证据。

## T3-006 `TraderCTPMini::init` 直接解引用 `params->get(...)` 且厂商符号无校验

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/TraderCTPMini/TraderCTPMini.cpp:102-105`、`L122-132`；`connect` 的 `L169`
- 相关测试: `ctpmini.DISABLED_init_and_connect_need_vendor_dylib`
- 输入/前置条件: 配置缺 `front`/`broker`/`user`/`pass` 任一键，或没有 `thosttraderapi` 动态库
- 期望行为: 用 `getCString` 或判空后返回 false
- 实际行为: `params->get("front")->asCString()` 对 NULL 返回值直接解引用；
  `m_funcCreator` 取空后 `connect()` 里 `m_funcCreator(...)` 空指针调用
- 复现步骤: 传入只含 `flowdir` 的 variant 调 `init`
- 影响范围: 配置笔误 = 进程崩溃（这是实盘交易通道）
- 备注: 与同族 `TraderCTP`/`ParserFemas` 同形，建议一并修（不实施）。

## T3-007 `OnRspQryTradingAccount` 末笔对空容器调 `clear()`

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/TraderCTPMini/TraderCTPMini.cpp:683-720`（`L713-719`）
- 相关测试: `ctpmini.DISABLED_qry_account_last_without_record_crashes`；
  正向半边在 `ctpmini.rsp_qry_trading_account_builds_balance_and_clears` 里绕开（首笔必须成功建容器）
- 输入/前置条件: 账户查询只收到错误回报（或 0 笔记录）且 `bIsLast==true`
- 期望行为: 与同文件 `OnRspQryOrder:L925` / `OnRspQryTrade:L895` 一样 `if (m_ayFunds)` 再 clear
- 实际行为: `m_ayFunds` 仍为 NULL 时 `m_ayFunds->clear()` → 空指针解引用
- 复现步骤: `TraderCTPMini t; CThostFtdcRspInfoField bad{ErrorID=4}; t.OnRspQryTradingAccount(&acc,&bad,1,true);`
- 影响范围: 空资金账号/查询失败是实盘日常场景，查资金即崩
- 备注: 该缺陷单独阻塞 `L713-719` 的一支。

## T3-008 `OnRspOrderAction` 错误判定反了，且 `pRspInfo` 为空时崩溃

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/TraderCTPMini/TraderCTPMini.cpp:669-681`
- 相关测试: `ctpmini.order_action_success_branch_pushes_trader_error`（按现状固化，未放宽）
- 输入/前置条件: 撤单回报，`ErrorID==0`（成功）或 `pRspInfo==NULL`
- 期望行为: 有错误才 `onTraderError`，成功不应上抛；`pRspInfo==NULL` 应视为成功
- 实际行为: `if (IsErrorRspInfo) {} else { 造 WTSError 并上抛 }` —— 成功路径反而报错；
  且 else 分支无条件读 `pRspInfo->ErrorMsg`，厂商传 NULL 即崩
- 复现步骤: 见上述用例（成功回报 → `sink.errorRsp==1`；错误回报 → 不上抛）
- 影响范围: 撤单成功会被上层当作交易通道错误；风险模块可能因此拒单/停摆
- 备注: 同文件 `OnRspOrderInsert:L655-667`、`OnErrRtnOrderInsert:L1308-1320` 对 `pRspInfo` 也无判空
  （经 `makeError`）。

## T3-009 构造函数遗漏多个标量/指针成员初始化

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/TraderCTPMini/TraderCTPMini.cpp:77-93`（缺 `m_frontID`、`m_sessionID`、`m_lDate`、
  `m_hInstCTP`、`m_funcCreator`、`m_orderRef`/`m_iRequestID` 之外的原子量初值）；
  `src/TraderDumper/TraderAdapter.cpp:38-45`（缺 `_date`）；
  `src/TraderDumper/TraderAdapter.h:94-104`（`TraderAdapterMgr` 无构造函数，`_live_cnt` 不确定）；
  `src/ParserFemas/ParserFemas.cpp:83-88`（缺 `m_sink`、`m_pBaseDataMgr`、`m_hInst`、`m_funcCreator`）
- 相关测试: `ctpmini.make_entrust_id_formats_front_session_ref_and_increments`（必须先显式注入
  front/session/orderRef 才能断言精确串，就是绕开这个不确定值）；
  `adapter_mgr.add_get_and_duplicate_rules`（`isAnyAlive()` 只在 `run()` 赋值之后才断言）
- 期望行为: 所有标量/裸指针/原子量在初始化列表里给初值
- 实际行为: 栈上对象读到不确定的垃圾值 → `makeEntrustID` 输出不确定；
  `TraderAdapter::queryFund/queryPosition` 的 `if(_date==0)` 守卫可能被垃圾值绕过 → 解引用空的 `_trader_api`；
  `Dumper::run(true)` 的 `while(isAnyAlive())` 可能因 `_live_cnt` 垃圾值永不退出
- 复现步骤: 局部构造 `TraderAdapterMgr m; m.isAnyAlive();`（未 run 之前值不确定）
- 影响范围: 未定义行为 + 潜在挂死/崩溃；本轮所有相关断言都刻意避开这些不确定读取
- 备注: 对照 `src/WtBtPorter/WtBtRunner.cpp` 的同类问题（Q06-003 已记录），这是跨模块复发形态。

## T3-010 `TraderAdapter` 回调无条件解引用 `_trader_api`

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/TraderDumper/TraderAdapter.cpp:143`（`handleEvent(WTE_Connect,0)`→`login`）、
  `L171`（`onLoginResult(true)`→`queryPositions`）、`L185/L193`（`queryFund/queryPosition`）、
  `L212`（`onRspAccount` 的 `if(!_done) _trader_api->queryTrades()`）、
  `L249`（`onRspTrades` 末尾**无任何守卫**的 `queryOrders()`）
- 相关测试: `trader_adapter.DISABLED_on_rsp_trades_always_derefs_null_api`、
  `trader_adapter.DISABLED_login_and_connect_success_paths_deref_null_api`
- 输入/前置条件: `init()` 失败（模块加载失败）但 `Dumper::config` 仍把该适配器登记并 `run()`
- 期望行为: 每个回调前判 `_trader_api != NULL`
- 实际行为: `_trader_api` 为 NULL 时任何回报即崩；`onRspTrades` 连 `_done` 都不看
- 复现步骤: `TraderAdapter a(&mgr); a.init("c", paramsWithBadModule, &bd); WTSArray* ay = WTSArray::create(); a.onRspTrades(ay);`
- 影响范围: 落地模块（TraderDumper）的整条回报链；配合 T3-011 的「失败仍登记」，
  实盘配置写错一个 module 名就会在回报到达时崩
- 备注: 本轮正向覆盖只能在 `_done` 已置真后驱动 `onRspAccount/onRspPosition/onRspOrders`。

## T3-011 `Dumper::config` 关键节点不判空，且忽略适配器 init 返回值

- 状态: 待审核
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

## T3-012 `Dumper::run` 的等待与刷新既不可中断也不可靠

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/TraderDumper/Dumper.cpp:103-125`
- 相关测试: `dumper.DISABLED_run_once_loops_forever_and_run_threaded_spawns_worker`
- 输入/前置条件: `run(true)`（默认）或 `run(false)`
- 期望行为: 有超时/退出条件，`release()` 能终止轮询
- 实际行为: `run(true)` 是 `for(;;){ sleep(1s) }` 直到 `isAnyAlive()` 为假，`_stopped` 不参与判断；
  `run(false)` 起线程后按 `_refresh_span` 刷新，`release()` 靠 `_stopped` join，
  但 `_refresh_span` 若被配成 0 会 `sleep_for(seconds(0))` 空转
- 复现步骤: 任配一个未完成通道后调用 `run(true)`
- 影响范围: 进程退出/守护；本 lane 因「不起线程、不 sleep」规则整条 DISABLED
- 备注: 与 T3-009 的 `_live_cnt` 未初始化叠加后行为完全不确定。

## T3-013 `WtHelper::get_cwd` 忽略 `getcwd` 返回值

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/TraderDumper/WtHelper.cpp:24-35`
- 相关测试: `wt_helper.cwd_and_module_dir_are_round_trippable`（只在正常长度路径上断言）
- 输入/前置条件: 当前目录路径长度 ≥ 255（临时目录/CI workspace 常见）
- 期望行为: `getcwd` 返回 NULL 时报错或换 `PATH_MAX` 缓冲
- 实际行为: `char buffer[255]; getcwd(buffer, 255);` 返回值被丢弃，随后 `_cwd = buffer` 读取未写入的缓冲
- 复现步骤: 在深目录里首次调用 `WtHelper::get_cwd()`
- 影响范围: 模块目录探测（`TraderAdapter::init` 用它找 `traders/` 子目录），影响插件加载路径
- 备注: 同族 `WtDtServo/WtDtHelper` 也有同名 helper（BUG 汇总里已有多处 helper 复制的讨论）。

## T3-014 登录请求组装用裸 `strcpy` 拷到定长厂商字段

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/ParserFemas/ParserFemas.cpp:354-359`；
  `src/TraderCTPMini/TraderCTPMini.cpp:289-294`、`L311-314`、`L334-340`、`L433-449`、`L1335-1341`
- 相关测试: 未覆盖（要求 `m_pUserAPI` 非空，见 T3-006/T3-010）
- 期望行为: `strncpy`/带长度校验（同仓 `wt_strcpy` 已提供）
- 实际行为: `strcpy(req.BrokerID, m_strBroker.c_str())` 等直接把用户配置写进 `TThostFtdc*Type` 定长数组
- 复现步骤: 配置里把 `user` 写成超过 73 字节的串
- 影响范围: 栈上 `req` 结构溢出（配置错误即 UB，非外部输入面）
- 备注: 属风格一致性问题，列 Minor 供统一治理。

---

## 分级汇总

| 级别 | 数量 | 条目 |
| --- | --- | --- |
| Blocker | 5 | T3-001、T3-002、T3-006、T3-007、T3-010、T3-011（其中 T3-011 同时含 Major 面） |
| Major | 5 | T3-003、T3-004、T3-008、T3-009、T3-012 |
| Minor | 2 | T3-013、T3-014 |

（按条目号计：Blocker 6 条 T3-001/002/006/007/010/011，Major 5 条 T3-003/004/008/009/012，Minor 2 条 T3-013/014；
T3-005 号段未使用，因 `ParserFemas` 的 `connect/unsubscribe` 空实现已并入 T3-004。）

## 是否阻塞覆盖率目标

- 阻塞项: T3-001/T3-002（ParserFemas `init` 后半 + 登录回报 + `SubscribeMarketData`）、
  T3-006（TraderCTPMini `init/connect` 全部正向）、T3-007（资金查询空容器分支）、
  T3-010/T3-011（TraderDumper 登录/成交回报链、`config` 缺节负路径）。
  这些都需要「厂商 shim 动态库」或产品侧开一个 API 注入口才能进入，已在 manifest 第四节列明。
- 其余条目不阻塞，本轮已按实现现状固化断言（未放宽）。
