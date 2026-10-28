# lane T4 缺陷记录 —— TraderFemas / CTPOptLoader / ParserCTPMini

工作流: 测试覆盖率迭代 01 / lane T4（第六波·收发族）。
范围: `src/TraderFemas`、`src/CTPOptLoader`、`src/ParserCTPMini`。
`src/**` 生产代码零改动；以下条目一律 **待审核**，不要在本迭代内修产品代码。

用例文件:
- `src/TestUnits/coverage/t_TraderFemas/femas_trader_test.cpp`
- `src/TestUnits/coverage/t_CTPOptLoader/ctpopt_loader_test.cpp`
- `src/TestUnits/coverage/t_ParserCTPMini/ctpmini_parser_test.cpp`

行号取自本轮读到的磁盘内容（`wc -l`：TraderFemas.cpp 1180、CTPOptLoader/TraderSpi.cpp 623、
CTPOptLoader.cpp 268、ParserCTPMini.cpp 467）。

---

## BUG-T4-001 TraderFemas 构造函数漏初始化 m_sink / m_lDate

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/TraderFemas/TraderFemas.cpp:71-88`（成员声明 `TraderFemas.h:204,214`）
- 相关测试: `femas_trader_test.cpp` 的 `register_spi_binds_sink_and_base_data_mgr`、
  `instrument_status_push_casts_vendor_status`（两处都必须在触发回调前先 `registerSpi(nullptr)` 才敢跑）、
  `qry_login_completes_initialization_when_already_logined`（因此无法断言上报的交易日）
- 输入/前置条件: `new TraderFemas()` 后、`registerSpi()` 之前，任何厂商回调到达。
- 期望行为: 所有指针成员在构造函数里归零，早期回调走「无 sink 静默」分支。
- 实际行为: 初始化列表覆盖了 15 个成员，唯独漏掉 `m_sink` 与 `m_lDate`；
  `OnFrontConnected`/`OnRspUserLogin`/`onInitialized`/`write_log` 等都会 `if(m_sink)` 判断后调用一个野值指针 → UB。
  `m_lDate` 又是 `onLoginResult(true, "", m_lDate)`(L809) 的实参 → 上层拿到随机交易日。
- 复现步骤: 构造对象后直接调 `OnFrontConnected()`（不 registerSpi）；ASan 下报 READ of unknown location / 直接崩。
- 影响范围: 交易通道启动早期（前置连上但适配器还没注册 sink 的窗口）。
- 备注: 修复方向=构造函数补 `m_sink(NULL), m_lDate(0)`。同类问题见 BUG-T4-008。**不阻塞本模块其余用例**。

## BUG-T4-002 OnRspQryInvestorPosition 构造了持仓项却从未 append 进数组

- 状态: 待审核
- 严重级别: Blocker（数据错误 + 泄漏）
- 模块/文件: `src/TraderFemas/TraderFemas.cpp:667-742`（`WTSArray::create()` 在 :678，`WTSPositionItem::create` 在 :684，字段填到 :727，`m_ayPosition->release()` 在 :738）
- 相关测试: `femas_trader_test.cpp` → `position_query_response_never_appends_the_item_it_builds`
- 输入/前置条件: 注册 sink + 基础数据（合约/品种可查），喂一包 `CUstpFtdcRspInvestorPositionField`（Position=5、YdPosition=3、FrozenPosition=1、PositionCost=175000），`bIsLast=true`。
- 期望行为: 组装出的 `WTSPositionItem` 应 `m_ayPosition->append(pos, false)`，`onRspPosition` 收到 1 条持仓。
- 实际行为: 684 行创建 `pos` 并逐字段填充，但整个函数里没有任何 `append`；
  `m_ayPosition` 是空的 → `onRspPosition(m_ayPosition)` 汇报 size==0，随后数组被 release，`pos` 既没入池也没删除（泄漏）。
- 复现步骤: 该用例里 `EXPECT_EQ(sink.positions[0].size, 0u)` —— **修复后这条会失败**，是故意留的哨兵。
- 影响范围: Femas 通道的持仓查询功能整体失效（TraderAdapter 侧永远看到空持仓）。
- 备注: 建议对照 `src/TraderCTP/TraderCTP.cpp` 的同名回调补 append + 引用计数处理。
  用例保持 `EXPECT_EQ(size, 0u)` 现状不改绿，修复提交时同步翻转该断言。

## BUG-T4-003 triggerQuery 解引用只在 connect() 里分配的 m_strandIO

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/TraderFemas/TraderFemas.cpp:1152-1154`（`boost::asio::post(*m_strandIO, ...)`），分配点在 `:201`（connect 内部）
- 相关测试: 启用侧 `queries_are_enqueued_only_after_channel_is_ready`（fixture 里把 strand 补上才敢跑）；
  禁用侧 `TraderFemasDisabled.query_account_before_connect_dereferences_null_strand`
- 输入/前置条件: 不调 `connect()`，直接 `queryAccount()`（状态 ALLREADY + 非空 m_pUserAPI 通过守卫）。
- 期望行为: 未初始化事件循环时拒绝或懒初始化。
- 实际行为: `m_strandIO` 初值 NULL → `post(*NULL, ...)` 崩。`queryAccount`(:441) 与四个 `OnRspQry*` 的 `bIsLast` 分支(:639/672/749/779) 都会踩。
- 复现步骤: 见 DISABLED 用例（需 `--gtest_also_run_disabled_tests`，会崩溃，勿在 CI 开）。
- 影响范围: 任何在 connect 之前发生的资金查询/查询应答收尾。
- 备注: 修复方向=构造函数里就建 strand，或 `triggerQuery` 先判空。

## BUG-T4-004 OnRspUserLogin 成功分支解引用 m_pUserAPI

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/TraderFemas/TraderFemas.cpp:567-586`（关键行 :578 `m_lDate = atoi(m_pUserAPI->GetTradingDay());`）
- 相关测试: `TraderFemasDisabled.user_login_success_branch_needs_vendor_api`
- 输入/前置条件: `m_pUserAPI == NULL`（未 connect 或厂商库加载失败）时收到登录成功应答。
- 期望行为: 先判 API 句柄，缺失则报错而不是继续。
- 实际行为: 直接虚调用 `GetTradingDay()` → 空指针 vtable。失败分支(:587-594)反而是安全的，所以启用侧只覆盖了拒绝分支。
- 复现步骤: DISABLED 用例需要真实 `CUstpFtdcTraderApi` 实例（47 个纯虚 + 仓库不产出该 .dylib）才能跑通正路径。
- 影响范围: 登录成功路径的行覆盖本迭代拿不到（见 manifest「未进入的面」）。
- 备注: 与 BUG-T4-001 的 m_lDate 组合，登录成功前 `m_lDate` 还是野值。

## BUG-T4-005 makeEntrustID 只判 length==0，不校验缓冲区容量

- 状态: 待审核
- 严重级别: Major（内存破坏）
- 模块/文件: `src/TraderFemas/TraderFemas.cpp:221-239`
- 相关测试: 启用侧 `make_entrust_id_rejects_null_and_zero_length_buffer`、`make_entrust_id_pads_local_order_id_to_twelve_digits`、`make_entrust_id_prefixes_session_id`；
  禁用侧 `TraderFemasDisabled.make_entrust_id_ignores_buffer_capacity`
- 输入/前置条件: `makeEntrustID(buf, sizeof(buf))`，`buf` 小于「会话号 + 12 位数字 + '\0'」。
- 期望行为: `length` 不足以容纳输出时返回 false。
- 实际行为: `fmtutil::format_to(buffer, "{}{:012d}", ...)` 完全无视 `length`，越界写；
  `length` 只在 `==0` 时用作拒绝条件。会话号来自 `MaxOrderLocalID`（厂商字段可达 16+ 字节），叠加 12 位数字，
  对调用方给的 32 字节缓冲已经是压线。
- 复现步骤: 小缓冲 + 长会话号（DISABLED 用例的形状），ASan 下 stack-buffer-overflow。
- 影响范围: 所有使用委托号缓冲的上层（TraderAdapter 常见 32 字节栈数组）。
- 备注: 修复方向=`format_to` 前按 length 校验，或改用带容量的 writer。

## BUG-T4-006 authenticate() 的厂商请求结构未清零

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/TraderFemas/TraderFemas.cpp:260-269`（`CUstpFtdcDSUserInfoField sDSUserInfo;` 在 :262）
- 相关测试: 未覆盖（该函数只有在有真实 API 实例时才走得通，见 BUG-T4-004 同款约束）
- 输入/前置条件: 登录流程调用 `authenticate()`。
- 期望行为: `memset(&sDSUserInfo, 0, sizeof(sDSUserInfo))` —— 同文件 `doLogin`(:273-274)、`logout`(:312-313) 都这么做了。
- 实际行为: 只 `strcpy` 了 AppID/AuthCode 和 `EncryptType='1'`，其余字段是未初始化栈数据，直接发给柜台。
- 复现步骤: 静态审查；同文件相邻函数的一致写法即证据。
- 影响范围: 认证请求可能带脏字段被柜台拒绝（难复现的现场问题）。
- 备注: 修复方向=补 memset。

## BUG-T4-007 init/connect 的模块加载失败不拦截

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/TraderFemas/TraderFemas.cpp:125`（load_library）、`:135`（get_symbol）、`:175`（`m_pUserAPI = m_funcCreator(...)`，随后 :176 直接解引用）
- 相关测试: `TraderFemasDisabled.init_dlopens_vendor_module`（本轮按约束不 dlopen，故禁用）
- 输入/前置条件: `flowdir`/`ctpmodule` 配错或厂商库缺失。
- 期望行为: `init` 在拿不到 creator 符号时返回 false；`connect` 在 `m_funcCreator==NULL` 时报错并中止。
- 实际行为: `init` 无论如何 `return true`(:137)；`connect` 无条件调用函数指针 → 空指针调用。
- 复现步骤: 关掉厂商库后 `init()`+`connect()`。
- 影响范围: 配置笔误表现为崩溃而不是可诊断的初始化失败。
- 备注: 同类见 BUG-T4-009（ParserCTPMini）、BUG-T4-014（CTPOptLoader）。

## BUG-T4-008 ParserCTPMini 构造函数漏初始化 m_sink / m_pBaseDataMgr / m_hInstCTP / m_funcCreator

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/ParserCTPMini/ParserCTPMini.cpp:83-88`（成员声明 `ParserCTPMini.h:100-108`）
- 相关测试: `ctpmini_parser_test.cpp` 全部启用用例都在 SetUp 里先 `registerSpi(&sink)`；
  禁用侧 `ParserCTPMiniDisabled.callback_before_register_spi_uses_wild_sink`
- 输入/前置条件: 工厂 `createParser()` 之后、`registerSpi()` 之前收到任何 CTPMini 回调。
- 期望行为: 指针成员归零。
- 实际行为: 只初始化了 `m_uTradingDate / m_pUserAPI / m_iRequestID`；
  `OnRtnDepthMarketData` 第一句 `if(m_pBaseDataMgr == NULL) return;`(:144) 判的就是野值，
  一旦非零就继续 `m_pBaseDataMgr->getContract(...)` → UB。
- 复现步骤: 构造后直接 `OnFrontConnected()`（DISABLED 用例）。
- 影响范围: 行情通道早期回调。
- 备注: 修复方向=构造函数补齐四个指针成员。

## BUG-T4-009 ParserCTPMini::init 空函数指针调用

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/ParserCTPMini/ParserCTPMini.cpp:126-133`（`get_symbol` :128、`m_pUserAPI = m_funcCreator(...)` :130、`m_pUserAPI->RegisterSpi(this)` :131）
- 相关测试: `ParserCTPMiniDisabled.init_dlopens_vendor_module`
- 输入/前置条件: `ctpmodule` 缺省名 `thostmduserapi` 在 bin 目录下不存在。
- 期望行为: 符号取不到就 `return false`。
- 实际行为: 连续两次无判空调用（creator 函数指针、返回的 API 指针）。
- 复现步骤: 无厂商库环境下 `init(cfg)`。
- 影响范围: 该模块的 init 正/负配置路径本迭代都没法启用（见 manifest）。
- 备注: 另外 `:112-116` 会在 CWD 下 `fs::create_directories` 建 flow 目录，测试环境有写盘副作用。

## BUG-T4-010 ParserCTPMini::IsErrorRspInfo 是恒返回 false 的桩函数

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/ParserCTPMini/ParserCTPMini.cpp:410-413`
- 相关测试: 启用侧 `login_response_is_ignored_until_last_packet`（只能用 `bIsLast=false`）；
  禁用侧 `ParserCTPMiniDisabled.login_success_branch_dereferences_null_api`
- 输入/前置条件: 任意错误应答（`OnRspUserLogin`/`OnRspSubMarketData`/`OnRspError`）。
- 期望行为: `return pRspInfo && pRspInfo->ErrorID != 0;`（同族 ParserCTP/TraderCTP 都是这个实现）。
- 实际行为: 形参被注释掉、`return false;`。于是
  `OnRspUserLogin`(:178-193) 把失败应答当成功：继续 `m_pUserAPI->GetTradingDay()`(:183) 并 `SubscribeMarketData()`；
  `OnRspSubMarketData`(:333-341) 的 if/else 两支恒走「无错误」空支 → 订阅失败完全静默。
- 复现步骤: 任何非 0 ErrorID 的登录应答。
- 影响范围: 登录/订阅错误不可观测；与 BUG-T4-009 叠加表现为「订阅失败但进程崩」。
- 备注: 这属于产品代码里的未完成桩，需要单独提交实现，不建议在本迭代动。

## BUG-T4-011 行情转换对 DBL_MAX 字段留空 → 池化对象带出上一笔的值

- 状态: 待审核
- 严重级别: Major（数据错误）
- 模块/文件: `src/ParserCTPMini/ParserCTPMini.cpp:279-289`
- 相关测试: `depth_market_data_is_converted_into_a_tick`（断言 `settle_price==0.0` 只在**全新池对象**时成立）、
  `invalid_prices_become_zero`
- 输入/前置条件: `SettlementPrice == DBL_MAX` 且 `ExchangeID != "CZCE"`、`Turnover == DBL_MAX`。
- 期望行为: 无效字段显式置 0。
- 实际行为: `settle_price` 只在 `!=DBL_MAX` 时赋值(:279-281)、`total_turnover` 只在 `!=DBL_MAX` 时赋值(:286-289)。
  而 `WTSTickData::create()` 走 `WTSPoolObject::allocate()`（`ObjectPool::construct` 复用内存，不清零），
  于是这两列会带出**上一笔 tick 的残值**。
- 复现步骤: 连续两笔行情，第一笔 Turnover 有效、第二笔为 DBL_MAX，观察第二笔 tick 的 total_turnover。
  （本迭代没写这条：需要控制内存池复用顺序，断言不稳定。）
- 影响范围: 依赖 tick 成交额/结算价的下游（回放、指标、AD 存储）。
- 备注: 修复方向=`WTSTickStruct` 在 allocate 后 memset，或对这两个字段显式赋 0。

## BUG-T4-012 夜盘纠偏分支依赖挂钟时间

- 状态: 待审核
- 严重级别: Major（可测试性/行为不确定）
- 模块/文件: `src/ParserCTPMini/ParserCTPMini.cpp:228-252`（`TimeUtils::getDateTime(curDate, curTime)` :237）
- 相关测试: 未覆盖（启用侧用例会刻意把 `ActionDay` 设成与 `m_uTradingDate` 不同、或小时 <20，绕开这段）
- 输入/前置条件: `ActionDay == 交易日 && 行情小时 >= 20`。
- 期望行为: 由交易日/时段表推导，而不是读本地墙钟。
- 实际行为: 同一笔行情在 03:00–09:00 之间被整笔丢弃，其余时间被改写日期 → 结果随运行时刻漂移。
- 复现步骤: 在 curHour∈[3,9) 跑同一笔夜盘行情即丢数。
- 影响范围: 该分支无法写可证伪的确定性断言；本模块这块行覆盖留空。
- 备注: 建议把「当前时间」做成可注入（测试侧替换），再补用例。

## BUG-T4-013 subscribe 的固定 500 指针数组无边界检查

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/ParserCTPMini/ParserCTPMini.cpp:415-451`（`char * subscribe[500] = {NULL};`）
- 相关测试: `subscribe_before_login_only_caches_the_filter`（走 `m_uTradingDate==0` 的缓存分支，不碰这个数组）
- 输入/前置条件: 登录后 `subscribe(CodeSet)`，集合大小 > 500（全市场订阅完全可能）。
- 期望行为: 按 `vecSymbols.size()` 动态分配或分批。
- 实际行为: `nCount` 无上限自增写栈数组 → 栈越界。
- 复现步骤: 501 个代码调用 subscribe。
- 影响范围: 全市场订阅；登录后的分支本轮也没有启用用例（`m_uTradingDate` 只能通过会崩的登录成功分支置非 0）。
- 备注: 同族 `SubscribeMarketData()`(:373-408) 用 `new char*[]`，是正确写法，可参照。

## BUG-T4-014 CTPOptLoader::run 模块加载失败后仍调用空 creator

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/CTPOptLoader/CTPOptLoader.cpp:238-255`（`load_library` :238、`get_symbol` :248、`g_ctpCreator(flowPath...)` :255）
- 相关测试: `CTPOptLoaderDisabled.complete_config_reaches_dlopen_and_null_creator`；
  启用侧 5 条 run() 用例全部靠 L198 的必填项校验（SAVEPATH 留空）在 dlopen 之前返回
- 输入/前置条件: `config.module` 指向不存在的期权厂商库、且 `traders/soptthosttraderapi_se.so` 也不存在。
- 期望行为: 加载失败 → 打印 + 返回非 0。
- 实际行为: `if (dllInst == NULL)` 只 printf(:239-240)、`if (g_ctpCreator == NULL)` 只 printf(:250-251)，
  随后无条件 `pUserApi = g_ctpCreator(flowPath.c_str());` → 空函数指针调用。
  另外 :253-254 用相对路径在 CWD 建 `./CTPFlow/<broker>/<investor>/`，产物位置随调用方目录漂移。
- 复现步骤: 见 DISABLED 用例（会崩，勿在 CI 开）。
- 影响范围: Loader 的正配置路径整体不可测、不可用；本迭代 CTPOptLoader 的行覆盖只能停在参数校验段。
- 备注: 修复方向=两处判空 + return，flow 目录改走 `SAVEPATH`/模块目录绝对路径。

## BUG-T4-015 OnRspQryInstrument 最后一包无条件 exit(0)

- 状态: 待审核
- 严重级别: Blocker（宿主进程）
- 模块/文件: `src/CTPOptLoader/TraderSpi.cpp:391-394`（`if(bIsLast){ DumpToJson(); exit(0); }`）
- 相关测试: `CTPOptLoaderDisabled.last_instrument_packet_exits_the_process`；
  启用侧所有 OnRspQryInstrument 用例都传 `bIsLast=false`
- 输入/前置条件: 合约查询的最后一包（无论有没有错误）。
- 期望行为: 作为 DLL 入口不该 exit 宿主进程；至少应返回状态给 `run()` 并由调用方决定退出码，
  查询失败时不该 `exit(0)`（失败被报成成功）。
- 实际行为: `exit(0)` → 在测试里会带走整个 gtest 进程且退出码为 0（跑测试的框架会把「崩溃/中断」误读成全通过）。
- 复现步骤: `spi.OnRspQryInstrument(&ins, &ok, 1, true)`。
- 影响范围: DumpToJson 的两个落地文件（commodities.json/contracts.json）与 LoadFromJson 的正路径都拿不到覆盖。
- 备注: 这是本轮唯一「整条禁用而不是想办法测」的路径；改法（例如把 exit 移到调用方）需要产品侧评审。

## BUG-T4-016 LoadFromJson 双重解析 + 变量遮蔽，第一个 root 泄漏

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/CTPOptLoader/TraderSpi.cpp:441-476`（外层 `root = load_from_file(path)` :441、`if(root){ WTSVariant* root = load_from_file(path);` :444、末尾 `root->release()` 释放的是内层 :476）
- 相关测试: 未覆盖（增量分支只有在 OnRspUserLogin 成功后才会被调用，见 BUG-T4-014/015 的阻塞）
- 输入/前置条件: `incremental: true` 且保存目录里已有 contracts.json。
- 期望行为: 解析一次，用完释放一次。
- 实际行为: 同一文件被解析两遍；外层 `root` 遮蔽后没有 release → 泄漏一个 WTSVariant 树；
  若第二遍失败（文件被截断），内层 `root` 为 NULL → `root->memberNames()` 空指针解引用。
- 复现步骤: 静态审查即可确认（:441 与 :444 是同一表达式的两次调用）。
- 影响范围: 增量拉取模式。
- 备注: 修复方向=删掉 :444 的重复行。

## BUG-T4-017 增量加载把行权价读成了 "product" 键

- 状态: 待审核
- 严重级别: Major（数据错误）
- 模块/文件: `src/CTPOptLoader/TraderSpi.cpp:466-469`（`contract.m_strikePrice = pCont->getDouble("product");` :468）
- 相关测试: 未覆盖（同 BUG-T4-016 的阻塞）
- 输入/前置条件: contracts.json 里合约的 `option` 段。
- 期望行为: 读 `strikeprice`（与 DumpToJson 写出的键对称）。
- 实际行为: 读 `"product"`（同名键在上一行刚被 `getCString("product")` 当产品代码用过），
  期权合约的行权价在增量模式下变成产品码的数值转换结果。
- 复现步骤: 静态审查；DumpToJson 的写出键可对照 `:524-560` 区段。
- 影响范围: 期权增量刷新后 strike 失真。
- 备注: 修复方向=改键名并补一条「写出→读回」的往返用例。

## BUG-T4-018 run() 不校验配置段是否存在

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/CTPOptLoader/CTPOptLoader.cpp:81-90`、`:152-160`
- 相关测试: 启用侧两条 yaml 用例都同时给 `ctp:` 和 `config:` 段（缺一即崩），因此没有「缺段」用例
- 输入/前置条件: 配置文件里没有 `ctp` 或 `config` 段。
- 期望行为: `root->get("ctp")==NULL` 时报错返回。
- 实际行为: 直接 `ctp->getCString("front")` / `cfg->getCString("path")` → 空指针成员调用。
- 复现步骤: `run("config:\n  path: x\n", true, false)`。
- 影响范围: 配置笔误表现为崩溃。
- 备注: `WTSVariant::getCString` 对**段内缺失的键**是安全的（返回默认值），风险只在段本身缺失。

## BUG-T4-019 extractProductID 用 'A'..'z' 判字母

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/CTPOptLoader/TraderSpi.cpp:74-85`（`while('A' <= instrument[nLen] && instrument[nLen] <= 'z')`）
- 相关测试: `ctpopt_loader_test.cpp` → `extract_product_id_stops_at_first_digit`（把当前行为钉住）
- 输入/前置条件: 合约代码含 `[ \ ] ^ _ ` ` 这些落在 `Z` 与 `a` 之间的字符。
- 期望行为: 用 `isalpha()`（或明确的字母表）截产品码。
- 实际行为: `'_'`(95) 等被当字母保留 → `"m_o2409"` 的产品码会是 `"m_o"`（当前期权用例正是依赖这一点，pid 传 "m_o"）。
  对真实 CTP 代码（小写字母+数字）无影响，故定级 Minor。
- 复现步骤: `extractProductID("a_b123") == "a_b"`。
- 影响范围: 自定义/非 CTP 代码规范的产品码归类。
- 备注: 若将来修成 `isalpha`，本轮 `extract_product_id_stops_at_first_digit` 与期权用例的 pid 需要同步调整。

---

## 本轮未发现 / 明确未进入的面

按「发现即记」的口径，上面 19 条都是静态审查 + 用例执行路径直接撞出来的；除此之外，
以下区域本轮**没有**留下结论（既没说没问题，也没法说有问题），需要后续 lane 或真实厂商环境：

1. **三个模块的 init/connect 正路径**：都要 dlopen 厂商库（TraderFemas `libUSTPtraderapiAF`、
   ParserCTPMini `libthostmduserapi`、CTPOptLoader `soptthosttraderapi_se`），
   仓库不产出这些产物、且失败即空指针调用（BUG-T4-007/009/014）→ 本轮按约束一律不启用。
2. **`CUstpFtdcTraderApi` / `CThostFtdcTraderApi` 的假实例**：两个厂商基类都是 47+ 个纯虚函数，
   本轮没有落地 fake（工程量与收益不匹配），因此
   `login`/`orderInsert`/`orderAction`/`logout` 的「已就绪」正路径与
   `OnRspUserLogin` 成功分支（BUG-T4-004）只有 DISABLED 形状，没有真实断言。
3. **CTPOptLoader 的 DumpToJson / LoadFromJson**：被 BUG-T4-015 的 `exit(0)` 与 BUG-T4-014 挡在门外，
   两个 JSON 落地文件（`TraderSpi.cpp:490-608`）本轮 0 覆盖。
4. **ParserCTPMini 夜盘日期纠偏**：BUG-T4-012 的挂钟依赖导致不可写确定性断言。
5. **线程/挂死类**：`TraderFemas::connect()` 起 worker 线程 + `std::this_thread::sleep_for`(:206)、
   `triggerQuery` 里的 50ms sleep(:1161)、`disconnect()` post 到 worker(:216) ——
   本轮一律没进（规则 6），因此 `m_thrdWorker`/`m_strandIO` 的生命周期有没有泄漏/悬挂没有结论。
6. **`TraderFemas::init` 的 `m_bQryOnline` 语义**（`TraderFemas.cpp:109-110`）：
   未配 `query` 前置时把「查询通道在线」置 true，看起来反直觉，但和 :584 的
   `if (m_bQryOnline) onInitialized();` 合起来是自洽的（没有独立查询通道就当已在线）。
   本轮只验证了「配了 query 但查询前置登录不上 → 永远不到 WS_ALLREADY、且无超时」这一半，
   没把它当缺陷写，等真实柜台环境再定级。
