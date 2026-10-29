# lane T4 缺陷记录 —— TraderFemas / CTPOptLoader / ParserCTPMini

工作流: 测试覆盖率迭代 01 / lane T4（第六波·收发族）。
范围: `src/TraderFemas`、`src/CTPOptLoader`、`src/ParserCTPMini`。
`src/**` 生产代码零改动；以下候选的复审状态分别标注为确认、待验证、重复或撤回，不要在本迭代内修产品代码。

复审边界：本次逐项对照生产调用顺序、测试输入和已有记录；本子任务未运行测试。除 T4-018 外，条目结论见各节“复审判定”；主控另行独立重建运行的 T4-018 证据单独标明。

用例文件:
- `src/TestUnits/coverage/t_TraderFemas/femas_trader_test.cpp`
- `src/TestUnits/coverage/t_CTPOptLoader/ctpopt_loader_test.cpp`
- `src/TestUnits/coverage/t_ParserCTPMini/ctpmini_parser_test.cpp`

行号取自本轮读到的磁盘内容（`wc -l`：TraderFemas.cpp 1180、CTPOptLoader/TraderSpi.cpp 623、
CTPOptLoader.cpp 268、ParserCTPMini.cpp 467）。

---

## BUG-T4-001 TraderFemas 构造函数漏初始化 m_sink / m_lDate

- 状态: 已撤回（正常生产生命周期下不可达；构造不变量仍可补强）
- 严重级别: 不应定为 Blocker
- 模块/文件: `src/TraderFemas/TraderFemas.cpp:71-88`（成员声明 `TraderFemas.h:204,214`）
- 相关测试: `register_spi_binds_sink_and_base_data_mgr` 与 `instrument_status_push_casts_vendor_status` 在直接调用回调前显式 `registerSpi(...)`；无 sink 子例也先 `registerSpi(nullptr)`。`qry_login_completes_initialization_when_already_logined` 手工把状态设成 `WS_LOGINED`，不是完整登录路径，不能证明生产会把未初始化的 `m_lDate` 上报。
- 输入/前置条件: 只有绕过适配器生命周期，在构造后、注册 sink / 创建厂商 API 前直接调用回调，才会读取未初始化的 `m_sink`；该前置条件不是已证实的厂商回调时序。
- 期望行为: 构造时将 `m_sink` 设空、`m_lDate` 设零，作为对象不变量。
- 实际行为: 构造函数确实漏初始化这两个成员；但生产 `TraderAdapter::run()` 先 `registerSpi(this)` 再 `connect()`，而 `connect()` 创建 API、`RegisterSpi(this)` 后才 `Init()`。`m_lDate` 在成功登录回调中、调用 `onInitialized()` 前赋值。现有代码证据不支持“生产早期回调读野 sink / 随机交易日”的结论。
- 复现步骤: 文档中的直接回调只能构造不受支持的对象状态；本次未运行 ASan，也没有运行时复现证据。
- 影响范围: 正常 WtCore 接入路径未证实有影响；独立调用回调方法或错误的自定义集成仍有未定义行为风险。
- 备注: 可以在后续代码修复中补齐初始化并保留无 sink 单测，但应移除 Blocker 结论；与 T4-008 都需先区分直接调用测试形状和真实适配器生命周期。

## BUG-T4-002 OnRspQryInvestorPosition 构造了持仓项却从未 append 进数组

- 状态: 确认的生产代码缺陷（本地 stub 回调测试复现；真实厂商运行未验证）
- 严重级别: Blocker（数据错误 + 泄漏）
- 模块/文件: `src/TraderFemas/TraderFemas.cpp:667-742`（`WTSArray::create()` 在 :678，`WTSPositionItem::create` 在 :684，字段填到 :727，`m_ayPosition->release()` 在 :738）
- 相关测试: `femas_trader_test.cpp` → `position_query_response_appends_the_reported_item`。旧用例将 `size == 0` 当作通过；现改为断言一条持仓及合约代码。
- 输入/前置条件: 注册 sink + 基础数据（合约/品种可查），喂一包 `CUstpFtdcRspInvestorPositionField`（Position=5、YdPosition=3、FrozenPosition=1、PositionCost=175000），`bIsLast=true`。
- 期望行为: 组装出的 `WTSPositionItem` 应 `m_ayPosition->append(pos, false)`，`onRspPosition` 收到 1 条持仓。
- 实际行为: 684 行创建 `pos` 并逐字段填充，但整个函数里没有任何 `append`；
  `m_ayPosition` 是空的 → `onRspPosition(m_ayPosition)` 汇报 size==0，随后数组被 release，`pos` 未调用 `release()`，池对象未归还（泄漏）。
- 证据边界: `OnRspQryInvestorPosition` 创建并填充 `pos`，但函数内无 `append`；末包将空数组交给 sink 并释放数组，局部 `pos` 也未 `release()`。修正后的本地 stub 测试 FAIL：期望 size 1、合约 `rb2401`，实际 size 0、合约为空；账户查询对照 PASS。真实 Femas 柜台及泄漏量测未运行。
- 影响范围: Femas 通道的持仓查询功能整体失效（TraderAdapter 侧永远看到空持仓）。
- 备注: 修复方向：确认 contract 后将有效 `pos` 以正确所有权转入 `m_ayPosition`（或未入数组时释放），并对照同族持仓聚合逻辑；把哨兵断言改成至少一条记录及关键字段断言。不要把当前 size 0 当作期望行为。

## BUG-T4-003 triggerQuery 解引用只在 connect() 里分配的 m_strandIO

- 状态: 待验证（现有 DISABLED 用例构造了不可能的前置状态）
- 严重级别: 暂不定级
- 模块/文件: `src/TraderFemas/TraderFemas.cpp:1152-1154`（`boost::asio::post(*m_strandIO, ...)`），分配点在 `:201`（connect 内部）
- 相关测试: 启用侧 `queries_are_enqueued_only_after_channel_is_ready` 由 fixture 手动注入 strand；禁用侧 `TraderFemasDisabled.query_account_before_connect_dereferences_null_strand` 手动设置 API 哨兵和 `WS_ALLREADY`。
- 输入/前置条件: `queryAccount()` 在真实代码中先检查 `m_pUserAPI != NULL` 且 `m_wrapperState == WS_ALLREADY`。禁用用例在没有 `connect()` 的情况下伪造这两个条件，因此并非“connect 前 queryAccount”的真实调用路径。
- 期望行为: `m_strandIO` 在任何可能触发 `triggerQuery()` 的路径之前就已建立；`triggerQuery()` 对未初始化状态应可诊断地返回。
- 实际行为: `triggerQuery()` 无条件对 `*m_strandIO` 发 post；`connect()` 当前在 `m_pUserAPI->Init()` 之后才分配 strand。若厂商 `Init()`/回调链允许在 `connect()` 返回前进入查询状态，存在初始化竞态的可能，但本次未找到足以证明该时序会发生的运行证据。
- 证据边界: 静态可见缺少 strand 判空；禁用测试的“API 已存在且 ALLREADY、但从未 connect”前提违反对象生命周期。启用测试只证明人工注入 strand 后的队列路径。
- 备注: 不将当前 DISABLED 用例表述为复现。修复方向是先于 `Init()` 建立 strand，并保留入口守卫；若要定为生产缺陷，需用 API stub 在 `Init()` 内同步触发回调/查询，或以厂商运行证据确认实际竞态。

## BUG-T4-004 OnRspUserLogin 成功分支解引用 m_pUserAPI

- 迭代02第二轮前状态（历史）: - 状态: 待验证（空 API 测试前提不符合正常回调生命周期）
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 暂不定级
- 模块/文件: `src/TraderFemas/TraderFemas.cpp:567-586`（关键行 :578 `m_lDate = atoi(m_pUserAPI->GetTradingDay());`）
- 相关测试: `TraderFemasDisabled.user_login_success_branch_needs_vendor_api` 直接以空 API 调成功回调；它未走 `connect()`，且本次未运行。
- 输入/前置条件: 空 `m_pUserAPI` 收到成功登录回报。生产 `connect()` 先创建 API 并注册 SPI，再调用 `Init()`；若模块加载失败或 creator/API 返回空，代码会更早在 `connect()` 解引用失败（BUG-T4-007），不是一条已证实能到达此回调的路径。
- 期望行为: 成功回调处理前校验 API 与响应对象；缺失时返回明确失败，不继续成功状态迁移。
- 实际行为: `OnRspUserLogin` 成功分支直接读 `pRspUserLogin->MaxOrderLocalID` 并调用 `m_pUserAPI->GetTradingDay()`。代码缺少空指针保护，但测试仅证明了人工直调空 API 的风险，没有证明厂商在正常生产生命周期中会以此状态回调。
- 证据边界: 不将“未 connect 或加载失败时收到成功回报”定为已确认产品路径。若关注释放竞态或迟到回调，需要厂商 API 生命周期证据/可控 stub。
- 备注: 防御式修复可先判空并处理 `pRspUserLogin`；不要再把该问题与未初始化 `m_lDate` 绑定，正常成功路径会先赋值交易日再通知上层。

## BUG-T4-005 makeEntrustID 只判 length==0，不校验缓冲区容量

- 状态: 确认的接口边界缺陷；当前仓库调用点未显示小缓冲区暴露
- 严重级别: Major（内存破坏）
- 模块/文件: `src/TraderFemas/TraderFemas.cpp:221-239`
- 相关测试: 启用侧 `make_entrust_id_rejects_null_and_zero_length_buffer`、`make_entrust_id_pads_local_order_id_to_twelve_digits`、`make_entrust_id_prefixes_session_id`；
  禁用侧 `TraderFemasDisabled.make_entrust_id_ignores_buffer_capacity`
- 输入/前置条件: 任意调用方传入的 `length` 小于实际格式化结果和结尾 `\0` 所需容量。
- 期望行为: `length` 不足以容纳输出时返回 false。
- 实际行为: `fmtutil::format_to(char*, ...)` 把裸指针作为输出迭代器，没有容量参数；本函数只拒绝 `length == 0`。Femas 的 `MaxOrderLocalID` 字段是 21 字节数组，格式化输出最多需要 20 字节会话号 + 12 位编号 + 1 个 NUL，共 33 字节。
- 证据边界: DISABLED 用例传 8 字节数组并设置长会话号，但本次未运行。仓库 `TraderAdapter::doEntrust()` 当前传入 64；因此能确认接口忽略容量，但不能据此声称当前仓库常见路径已发生越界或“32 字节缓冲压线”。
- 影响范围: 外部或未来调用方传入不足 33 字节时可越界写；当前已检索到的仓库主调用点传 64 字节。
- 备注: 修复方向：预先计算所需容量并拒绝不足输入，或采用带界格式化；为 32、33、64 字节边界添加安全断言。不要把禁用用例写成已运行的 ASan 复现。

## BUG-T4-006 authenticate() 的厂商请求结构未清零

- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 暂不定级
- 模块/文件: `src/TraderFemas/TraderFemas.cpp:260-269`（`CUstpFtdcDSUserInfoField sDSUserInfo;` 在 :262）
- 相关测试: 未覆盖。调用依赖登录/API 路径；现有测试没有 fake API 捕获认证请求原始字节。该证据缺口与 T4-004 的空 API 回调测试不是同一前置条件。
- 输入/前置条件: 登录流程调用 `authenticate()`。
- 期望行为: 为厂商请求结构提供确定的初始字节值；同文件 `doLogin`、`logout` 先清零再填字段。
- 实际行为: `CUstpFtdcDSUserInfoField` 只有 `AppID[31]`、`AuthCode[17]`、`EncryptType` 三个字段；代码给三个字段都赋了值，但两个字符串数组的 NUL 之后尾部字节及可能的结构填充未清零。
- 证据边界: 请求结构随后传给厂商 API，但没有厂商实现、请求抓取或原始字节 fake 证明 API 会读取/转发 NUL 后的尾部；不能据此断言柜台收到脏字段或拒绝认证。
- 备注: 可用 `CUstpFtdcDSUserInfoField sDSUserInfo{};` 明确零初始化，以消除未确定字节；若要作为安全/行为缺陷定级，应补厂商协议或 fake API 原始结构拷贝证据。


- 迭代02复核: NOT_BUG。结构只有 AppID、AuthCode、EncryptType 三个字段，调用处都被赋值；未清零的部分是两个 C 字符串 NUL 后的尾字节及可能的结构填充，当前没有证据表明厂商接口按整块原始字节读取它们或影响认证行为。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 证据不足（零初始化是合理防御；柜台影响未证）
## BUG-T4-007 init/connect 的模块加载失败不拦截

- 状态: 确认的生产错误路径缺陷（静态证据；未运行厂商库路径）
- 严重级别: Major
- 模块/文件: `src/TraderFemas/TraderFemas.cpp:125`（load_library）、`:135`（get_symbol）、`:175`（`m_pUserAPI = m_funcCreator(...)`，随后 :176 直接解引用）
- 相关测试: `TraderFemasDisabled.init_dlopens_vendor_module` 被禁用；即便运行也会进入真实模块加载，本次未运行。
- 输入/前置条件: `ctpmodule` 路径配置错误、厂商库缺失或 creator 符号缺失。
- 期望行为: `init` 在拿不到 creator 符号时返回 false；`connect` 在 `m_funcCreator==NULL` 时报错并中止。
- 实际行为: `init` 无论如何 `return true`(:137)；`connect` 无条件调用函数指针 → 空指针调用。
- 证据边界: `init()` 对 `load_library`/`get_symbol` 结果无判空并固定返回 true；`connect()` 无条件调用 `m_funcCreator`，再解引用返回 API。该分支由源码可确认，未在本次动态复现。
- 影响范围: 配置笔误表现为崩溃而不是可诊断的初始化失败。
- 备注: 修复方向：`init()` 校验 handle 和 creator 并返回 false；`connect()` 校验 creator 及 API 返回值。T4-009/014 是不同组件内的同一失败模式，不是同一代码点的重复记录。

## BUG-T4-008 ParserCTPMini 构造函数漏初始化 m_sink / m_pBaseDataMgr / m_hInstCTP / m_funcCreator

- 状态: 已撤回（测试绕过了生产注册顺序；成员初始化仍值得补齐）
- 严重级别: 不应定为 Blocker
- 模块/文件: `src/ParserCTPMini/ParserCTPMini.cpp:83-88`（成员声明 `ParserCTPMini.h:100-108`）
- 相关测试: 启用用例在 `SetUp` 先 `registerSpi(&sink)`；禁用侧 `callback_before_register_spi_uses_wild_sink` 直接对未注册对象调回调。
- 输入/前置条件: 只有外部绕过 `ParserAdapter` 顺序，在 `registerSpi()`/`init()` 前直接调用 callback 才会读取这些未初始化成员。
- 期望行为: 指针成员均有确定的初始值。
- 实际行为: 构造函数漏初始化四个成员；但 `ParserAdapter::init()` 在调用 parser 的 `init()` 前先 `registerSpi(this)`，然后 `init()` 创建 API 并对厂商 SPI 注册 `this`；`ParserAdapter::run()` 后续才调用 `connect()`。仓库正常路径不存在“未注册 parser 已收到厂商回调”的证据。
- 证据边界: DISABLED 用例直接调用回调，没有模拟厂商 API，不能证明早期生产回调。`m_hInstCTP`/`m_funcCreator` 也会在 `init()` 中赋值；缺库导致的空 creator 调用另由 T4-009 记录。
- 备注: 可补齐构造初始化以强化对象不变量，但移除 Blocker/已复现表述；若未来支持绕过 `ParserAdapter` 的集成，应另明确生命周期契约。

## BUG-T4-009 ParserCTPMini::init 空函数指针调用

- 状态: 已合并（与 BUG-R105-003 重复登记）
- 严重级别: Blocker
- 模块/文件: `src/ParserCTPMini/ParserCTPMini.cpp:126-133`（`get_symbol` :128、`m_pUserAPI = m_funcCreator(...)` :130、`m_pUserAPI->RegisterSpi(this)` :131）
- 相关测试: `ParserCTPMiniDisabled.init_dlopens_vendor_module` 禁用；R105 的 `init_flowdir_creation_failure_throws_before_vendor_symbol` 在目录创建异常处提前退出，不覆盖空 creator 路径。两者本次均未运行。
- 输入/前置条件: `ctpmodule` 缺省名 `thostmduserapi` 在 bin 目录下不存在。
- 期望行为: 符号取不到就 `return false`。
- 实际行为: 连续两次无判空调用（creator 函数指针、返回的 API 指针）。
- 证据边界: 源码顺序是 load library → get symbol → 不判空调用 `m_funcCreator` → 不判空调用 API；缺库/缺符号时风险可由静态分支确认。本次未在缺库环境动态运行。
- 影响范围: ParserCTPMini 的必需厂商库缺失时，初始化不能返回可诊断失败。
- 备注: `BUG-R105-003` 已合并登记 ParserCTPMini/ParserFemas/ParserCTPOpt 同形问题；本条是其 Mini 子项，不另立重复修复项。修复方向为在首次调用前验证库句柄、符号和 API 返回值，并以可控缺库路径验证失败返回。`init()` 创建 flow 目录的 CWD 副作用与空 creator 是两个不同观察，不据此额外定为缺陷。

## BUG-T4-010 ParserCTPMini::IsErrorRspInfo 是恒返回 false 的桩函数

- 状态: 已合并（与 BUG-R105-004 重复登记）
- 严重级别: Blocker
- 模块/文件: `src/ParserCTPMini/ParserCTPMini.cpp:410-413`
- 相关测试: 启用侧 `login_response_is_ignored_until_last_packet` 只用 `bIsLast=false`；R105 的 API stub 登录用例传 `pRspInfo == NULL`，没有非零 ErrorID 用例。禁用侧 `login_success_branch_dereferences_null_api` 同时伪造空 API 和错误响应，不代表正常已初始化会话。本次未运行这些用例。
- 输入/前置条件: 任意错误应答（`OnRspUserLogin`/`OnRspSubMarketData`/`OnRspError`）。
- 期望行为: `return pRspInfo && pRspInfo->ErrorID != 0;`（同族 ParserCTP/TraderCTP 都是这个实现）。
- 实际行为: 形参被注释掉并固定返回 false。代码因此把非零 ErrorID 登录响应当作成功，继续读交易日和发订阅；订阅响应中的错误分支也不可达。
- 证据边界: 逻辑缺陷由谓词和分支静态可确认，但现有启用测试未传非零 ErrorID；禁用用例的空 API 前置条件无效，不能作为正常错误回报的运行复现。
- 影响范围: 有效 API 会话收到错误回报时，错误处理被跳过并走成功逻辑；“崩溃”仅来自把错误回报与空 API 的非法状态组合。
- 备注: 与 `BUG-R105-004` 重复登记。修复方向：按 `pRspInfo && pRspInfo->ErrorID != 0` 判断，并用 API stub 构造非零 ErrorID，断言不触发登录成功/订阅请求；不再将空 API 解引用写成主要产品影响。

## BUG-T4-011 行情转换对 DBL_MAX 字段留空 → 池化对象带出上一笔的值

- 状态: 已撤回（对象池复用前提错误，源码会重新零初始化 tick）
- 严重级别: 不构成已确认数据缺陷
- 模块/文件: `src/ParserCTPMini/ParserCTPMini.cpp:279-289`
- 相关测试: `depth_market_data_is_converted_into_a_tick` 断言 `settle_price==0.0`；`invalid_prices_become_zero` 只覆盖部分报价字段。前者的单次调用本身不验证池复用，但构造链源码可确定复用对象会重新初始化。
- 输入/前置条件: `SettlementPrice == DBL_MAX` 且 `ExchangeID != "CZCE"`、`Turnover == DBL_MAX`。
- 期望行为: 无效字段显式置 0。
- 实际行为: 这两个字段在 DBL_MAX 输入下不显式赋值，但并非保留池中旧值：`WTSTickData::create()` 调 `allocate()`；`WTSPoolObject::allocate()` 调 `ObjectPool::construct()`，其 placement-new 会调用 `WTSTickData()`；成员 `WTSTickStruct` 的构造函数每次都 `memset(this, 0, sizeof(...))`。
- 证据边界: `depth_market_data_is_converted_into_a_tick` 的新对象断言单独不足以证明池复用行为；池构造链源码足以否定“残留上一笔值”的前提。本次未运行测试。
- 影响范围: 在本条列出的非 CZCE 前置条件下，`SettlementPrice`/`Turnover == DBL_MAX` 的未赋值目标字段在新构造 tick 中保持零；现有证据不支持上一笔残值污染。
- 备注: 撤回“池化残值”缺陷及其修复建议。若想固定无效字段应为零，可补字段级测试，但不能把它描述为当前对象池缺陷。

## BUG-T4-012 夜盘纠偏分支依赖挂钟时间

- 状态: 已合并（与 BUG-R105-001 重复登记）
- 严重级别: Major（可测试性/行为不确定）
- 模块/文件: `src/ParserCTPMini/ParserCTPMini.cpp:228-252`（`TimeUtils::getDateTime(curDate, curTime)` :237）
- 相关测试: T4 原用例避开挂钟分支；R105 的 `night_session_recomputes_action_date` 已触及其中一条路径，但基于本地当前时间，不构成跨午夜各分支的确定性覆盖。本次未运行。
- 输入/前置条件: `ActionDay == 交易日 && 行情小时 >= 20`。
- 期望行为: 由交易日/时段表推导，而不是读本地墙钟。
- 实际行为: `ActionDay == m_uTradingDate && actHour >= 20` 时调用本地墙钟；本地 `curHour` 位于 `[3,9)` 会丢弃行情，日期纠偏依赖运行时刻。外层已要求 `actHour >= 20`，所以内层 `actHour == 0` 分支不可达。
- 证据边界: 代码条件静态可确认；现有 R105 测试只覆盖当前主机时间下的一条路径，不证明每个时间段。本次未运行。
- 备注: `BUG-R105-001` 已跨 ParserCTP / Mini / Femas 合并登记，本条是 Mini 子项，不另立重复编号。修复方向按交易日/时段规则推导；若仍依赖系统时钟，应注入时钟并覆盖可达边界。

## BUG-T4-013 subscribe 的固定 500 指针数组无边界检查

- 状态: 已合并（与 BUG-R105-002 重复登记）
- 严重级别: Major
- 模块/文件: `src/ParserCTPMini/ParserCTPMini.cpp:415-451`（`char * subscribe[500] = {NULL};`）
- 相关测试: T4 的 `subscribe_before_login_only_caches_the_filter` 走缓存分支、不碰数组；R105 的 `login_failure_and_inline_subscribe_branches` 只用两个代码，覆盖登录后循环但不触及 501 边界。本次未运行。
- 输入/前置条件: 登录后 `subscribe(CodeSet)`，集合大小 > 500（全市场订阅完全可能）。
- 期望行为: 按 `vecSymbols.size()` 动态分配或分批。
- 实际行为: `nCount` 无上限自增写栈数组 → 栈越界。
- 证据边界: `nCount` 无上限地写入 `char* subscribe[500]`，第 501 项越界是源码可证的边界；本次没有执行越界用例。
- 影响范围: 输入超过 500 项时存在越界风险。R105 stub 用两个代码经过登录后分支，但没有测试 501 边界；本次未执行越界用例。
- 备注: 与 `BUG-R105-002` 的 Mini 子项重复；修复方向为按输入大小动态分配或分批调用，并以 fake API 在安全边界上检查 500/501 项。不能用不稳定的崩溃测试替代边界断言。

## BUG-T4-014 CTPOptLoader::run 模块加载失败后仍调用空 creator

- 状态: 确认的生产错误路径缺陷（静态证据；未运行厂商模块路径）
- 严重级别: Major（模块配置/部署错误导致崩溃）
- 模块/文件: `src/CTPOptLoader/CTPOptLoader.cpp:238-255`（`load_library` :238、`get_symbol` :248、`g_ctpCreator(flowPath...)` :255）
- 相关测试: `CTPOptLoaderDisabled.complete_config_reaches_dlopen_and_null_creator` 禁用；启用侧 run() 用例在必填项校验前后返回，没有进入厂商库加载。此子任务未运行测试。
- 输入/前置条件: 配置完整且 `MODULE_NAME` 指向缺失的厂商库，或库已加载但 creator 符号缺失；未配置时默认路径为 `./soptthosttraderapi_se.so`。
- 期望行为: 加载失败 → 打印 + 返回非 0。
- 实际行为: 模块句柄和 creator 为空时只打印信息，仍调用 `g_ctpCreator(...)`；若 creator 返回空，后续立即对 API 指针调用 `RegisterSpi`。
- 证据边界: 该错误路径由源码控制流确认；没有在本次任务中 dlopen 厂商库或运行空函数调用。测试还会写当前目录的 flow 路径，因此不能把它当作无副作用的普通单测。
- 影响范围: 配置完整但厂商库缺失/符号不匹配时，`run()` 崩溃而不能向调用方返回初始化失败。
- 备注: 与 T4-007/009 是不同组件内的同类错误模式，不是同一代码点的重复。修复方向：分别验证句柄、符号、creator 返回值并以非零失败状态返回。相对 `./CTPFlow` 的路径是否错误取决于 loader 的目录约定，本次不据此另定缺陷。

## BUG-T4-015 OnRspQryInstrument 最后一包无条件 exit(0)

- 状态: 确认的进程控制缺陷；宿主影响取决于调用契约
- 严重级别: Major（同进程宿主被终止；一次性 CLI 退出情形另论）
- 模块/文件: `src/CTPOptLoader/TraderSpi.cpp:391-394`（`if(bIsLast){ DumpToJson(); exit(0); }`）
- 相关测试: `CTPOptLoaderDisabled.last_instrument_packet_exits_the_process` 禁用；启用侧 OnRspQryInstrument 用例均传 `bIsLast=false`。本次未运行退出测试。
- 输入/前置条件: 合约查询的最后一包（无论有没有错误）。
- 期望行为: 回调通知 `run()` 完成并返回状态；是否结束进程由宿主决定。最终查询报错时也不应报告状态 0。
- 实际行为: 每次 `bIsLast` 都先 `DumpToJson()` 再 `exit(0)`，不检查最终 `pRspInfo` 错误状态；`CTPOptLoader` 是导出 `run()` 的 SHARED 模块，仓库 `LoaderRunner` 会动态加载并调用该入口。该调用会结束整个进程，而不是只结束模块函数。
- 证据边界: 退出语义可从源码确认；本次未运行禁用测试，也未验证所有外部宿主的复用契约。不能声称本轮实测了 gtest 被中断或框架误报通过。`CTPLoader` 的相同模式另记为 BUG-T6-002；两处是不同模块的独立实现。
- 影响范围: 使用 `bAsync=true` 的同进程调用方即使 `run()` 已返回，最终响应到达时仍会被终止；一次性独立 LoaderRunner 进程正常结束的要求则需由产品契约确认。错误末包也会以进程码 0 结束。
- 备注: 修复方向：用完成状态/通知让 `run()` 收尾并返回准确状态；若只支持一次性 CLI，应由 CLI 宿主在收到完成后退出。安全验证应在子进程中断言退出状态，不能在测试主进程内直接触发 `exit()`。

## BUG-T4-016 LoadFromJson 双重解析 + 变量遮蔽，第一个 root 泄漏

- 状态: 确认的静态内存/错误路径缺陷；未运行
- 严重级别: Major
- 模块/文件: `src/CTPOptLoader/TraderSpi.cpp:441-476`（外层 `root = load_from_file(path)` :441、`if(root){ WTSVariant* root = load_from_file(path);` :444、末尾 `root->release()` 释放的是内层 :476）
- 相关测试: 未覆盖。`LoadFromJson()` 为私有路径，通过增量登录流程到达；现有 CTPOptLoader 用例没有 fake API 登录正路径。本次未运行。
- 输入/前置条件: `incremental: true` 且保存目录里已有 contracts.json。
- 期望行为: 解析一次，用完释放一次。
- 实际行为: 有效文件被解析两遍，内层同名变量遮蔽外层指针；末尾只释放内层 root，外层解析树泄漏。若两次读取间文件变更导致第二次返回空，内层空指针会在 `memberNames()` 被解引用；这是并发/瞬时文件变化条件，不应写成每次加载都会崩。
- 证据边界: 重复调用和外层未释放可由源码静态确认；第二次读取失败需要额外文件竞态，本次没有复现。
- 影响范围: 增量拉取模式。
- 备注: 修复方向：复用第一次解析得到的 root，并对失败/正常路径各释放一次；对可能为空的根对象在遍历前检查。

## BUG-T4-017 增量加载把行权价读成了 "product" 键

- 状态: 确认的静态反序列化缺陷；未运行
- 严重级别: Major（数据错误）
- 模块/文件: `src/CTPOptLoader/TraderSpi.cpp:459-471`（option 字段读取与 `pCont` 层级）；写出端 `:539-568`
- 相关测试: 未覆盖；增量 option 字段需要走 `LoadFromJson()`。现有 CTPOptLoader 用例没有 fake API 登录正路径，本次未运行。
- 输入/前置条件: contracts.json 里合约的 `option` 段。
- 期望行为: 先取 `option` 子对象，再从其中读取 `optiontype`、`underlying`、`strikeprice`、`underlyingscale`。
- 实际行为: `has("option")` 后仍在父级 `pCont` 读取四个字段；写出端把这些键都放在嵌套 `option` 对象中，且行权价键名是 `strikeprice`。因此有效导出文件在增量加载时不能还原这些 option 属性；`product` 还被当作行权价键。
- 证据边界: 写出/读取层级不对称由源码静态确认；本次没有执行 JSON 往返，也不推断字符串到 double 的具体转换结果。
- 影响范围: 期权增量加载后 option 类型、标的、行权价和标的乘数不能按导出字段恢复。
- 备注: 修复方向：获取非空 `option` 子对象并使用对应键名；补充含真实层级的序列化→读取往返断言，四个字段分别验证。

## BUG-T4-018 run() 不校验配置段是否存在

- 状态: 确认的生产代码缺陷（主控独立重建并验证缺段崩溃）
- 严重级别: Major
- 模块/文件: `src/CTPOptLoader/CTPOptLoader.cpp:81-90`、`:152-160`
- 相关测试: `missing_json_file_returns_zero_without_touching_globals` 的实际参数为 `run(..., true, true)`，覆盖缺文件分支。新增 `yaml_content_missing_ctp_section_returns_zero_without_touching_globals`、`yaml_content_missing_config_section_returns_zero_without_touching_globals` 分别覆盖两个独立缺段输入。主控独立重建后报告：缺文件用例 PASS；缺 `ctp` 与缺 `config` 两个用例均 `rc=139`。本次子任务未执行这些命令。
- 输入/前置条件: `isFile=false` 时分别传入有效 YAML 根对象但缺少 `ctp` 或 `config`；缺文件是不同路径：`WTSCfgLoader::load_from_file()` 返回 NULL 后 `run()` 提前返回。
- 期望行为: 必需段缺失时按配置错误路径返回，并在改写全局配置前完成校验。
- 实际行为: 缺 `ctp` 时直接 `ctp->getCString("front")`；缺 `config` 时先读取并写入 ctp 全局字段，再对空 `cfg` 调 `getCString("path")`。两个独立缺段用例的主控重建结果均为 `rc=139`。缺文件不会进入这两个空段访问。
- 复现步骤: 对应测试分别是 `yaml_content_missing_ctp_section_returns_zero_without_touching_globals` 与 `yaml_content_missing_config_section_returns_zero_without_touching_globals`；主控报告重建运行均以 `rc=139` 结束。R36-003 旧用例把文件名传给 `isFile=false`，不能当缺文件复现。
- 影响范围: 配置笔误表现为崩溃。
- 备注: `WTSVariant::getCString` 对段内缺键与段指针为空是不同情况。R36-003 与本条根因相同，旧用例参数错误；当前三个分离用例把缺文件和缺段路径区分开。修复方向：先校验根节点及 `ctp`/`config` 均为有效对象，再读取字段或修改全局配置；所有失败分支释放根节点，并分别保留三条输入测试。

## BUG-T4-019 extractProductID 用 'A'..'z' 判字母

- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 模块/文件: `src/CTPOptLoader/TraderSpi.cpp:74-85`（`while('A' <= instrument[nLen] && instrument[nLen] <= 'z')`）
- 相关测试: `ctpopt_loader_test.cpp` → `extract_product_id_stops_at_first_digit` 只覆盖字母/数字边界；option 用例把 `ProductID="m_o"` 直接从厂商字段传入，并不调用此函数。本次未运行。
- 输入/前置条件: 只有未来生产代码调用此 helper 且输入在首个数字前含 Z 与 a 之间的 ASCII 标点时，当前区间判断才会影响归类。
- 期望行为: 明确 helper 的产品码字符集合；若保留 CTP 代码中的下划线，应显式允许 `_`，不能简单改成 `isalpha()`。
- 实际行为: ASCII 范围 `'A'..'z'` 同时接受若干标点；但本次仓库检索仅找到函数定义和单元测试调用，`OnRspQryInstrument` 使用厂商字段 `ProductID`，未调用该 helper。现有证据不支持当前行情/合约落地受影响。
- 证据边界: option 测试夹具里的 `ProductID="m_o"` 证明下划线是本仓库需要保留的一个合法输入形状，但不证明 helper 当前处理了该路径，也不定义其他标点规则。
- 备注: 暂从已确认产品缺陷列表撤回，保留为未使用辅助函数的输入契约问题。若将来接入生产调用，先依据 `ProductID` 格式明确允许字符，再补下划线和非法标点断言。


- 迭代02复核: NOT_BUG。ASCII 范围确实包含若干标点，但仓库检索到的生产 OnRspQryInstrument 路径不调用 extractProductID；唯一其他调用是单测。现有产品行为没有受该辅助函数影响的证据，字符集合契约也未定义。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 当前生产影响未证实（辅助函数无生产调用点；输入字符契约待确认）
---

## 本轮未发现 / 明确未进入的面

上面 19 个编号是候选记录，不代表全部已确认，也不代表都由测试路径运行触发。本次复审没有运行测试；T4-018 的运行证据来自主控独立重建。其余路径按各节的静态证据、测试前提及重复关系判定。以下区域仍需要后续 lane 或真实厂商环境：

1. **三个模块的 init/connect 正路径**：都要 dlopen 厂商库（TraderFemas `libUSTPtraderapiAF`、
   ParserCTPMini `libthostmduserapi`、CTPOptLoader `soptthosttraderapi_se`），
   仓库不产出这些产物、且失败即空指针调用（BUG-T4-007/009/014）→ 本轮按约束一律不启用。
2. **交易模块厂商 API 假实例**：TraderFemas/CTPOptLoader 仍没有 fake API 登录正路径，因此
   `login`/`orderInsert`/`orderAction`/`logout` 的「已就绪」正路径、T4-004 空 API 回调前提和 CTPOptLoader 登录加载路径未获运行验证。
   ParserCTPMini 的 R105 stub 已覆盖成功登录回调，但没有非零 ErrorID 的 `IsErrorRspInfo` 断言。
3. **CTPOptLoader 的 DumpToJson / LoadFromJson**：被 BUG-T4-015 的 `exit(0)` 与 BUG-T4-014 挡在门外，
   两个 JSON 落地文件（`TraderSpi.cpp:490-608`）本轮 0 覆盖。
4. **ParserCTPMini 夜盘日期纠偏**：R105 的 `night_session_recomputes_action_date` 覆盖当前主机时钟下的一条路径；全部跨午夜分支仍缺可注入时钟的确定性验收。
5. **线程/挂死类**：`TraderFemas::connect()` 起 worker 线程 + `std::this_thread::sleep_for`(:206)、
   `triggerQuery` 里的 50ms sleep(:1161)、`disconnect()` post 到 worker(:216) ——
   本轮一律没进（规则 6），因此 `m_thrdWorker`/`m_strandIO` 的生命周期有没有泄漏/悬挂没有结论。
6. **`TraderFemas::init` 的 `m_bQryOnline` 语义**（`TraderFemas.cpp:109-110`）：
   未配 `query` 前置时把「查询通道在线」置 true，看起来反直觉，但和 :584 的
   `if (m_bQryOnline) onInitialized();` 合起来是自洽的（没有独立查询通道就当已在线）。
   本轮只验证了「配了 query 但查询前置登录不上 → 永远不到 WS_ALLREADY、且无超时」这一半，
   没把它当缺陷写，等真实柜台环境再定级。
