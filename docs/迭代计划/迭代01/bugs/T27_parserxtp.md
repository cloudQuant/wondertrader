# T27 ParserXTP 缺陷复核（2026-10-29）

初次复核记录（2026-10-29）：当时使用本机已有 `CoverageTests_t_ParserXTP` 二进制运行 8 条启用用例，全部 `PASS`：`XtpLoginF.login_success_with_dashed_trading_day_corrupts_the_trading_date`、`XtpLoginF.second_login_resubscribes_the_very_same_codes`、`XtpOnlineF.post_login_subscribe_sends_market_data_but_never_tick_by_tick`、`XtpData27.tickbytick_szentrust_side_digit_falls_outside_the_bsdirect_enum`、`XtpData27.tickbytick_sztrade_match_flag_collides_with_the_lend_direction`、`XtpData27.tickbytick_state_type_is_dropped_by_both_branches`、`XtpData27.tickbytick_szentrust_looks_up_szse_but_keeps_the_master_exchange`、`XtpOnlineF.post_login_subscribe_without_api_buffers_only`。多数测试是在 stub 上钉当时的实现行为；PASS 不等于这些行为应作为正确契约保留。

后续测试修订（当前工作树）：`tickbytick_state_type_is_dropped_by_both_branches` 已替换为禁用占位 `DISABLED_state_type_support_scope_is_not_confirmed`，不再把静默丢弃作为通过条件；新增的 `tickbytick_sztrade_cancel_branch_marks_szse_master_exchange` 单例定向运行 FAIL，实际 side 为字符 `'4'`，而不是期望的 `BDT_Unknown`。因此上面的 8 PASS 是旧测试版本的历史结果，不代表当前工作树全部启用用例通过。没有真实 XTP 登录或数据流证据。

## BUG-T27-001 — 分隔日期未经校验传入 `to_uint32`
- 状态: 已撤回（迭代02：TEST，原产品主张不成立）

- 复核状态：`输入格式条件待证；测试证明解析器行为，不证明 XTP 会返回该格式`。
- 证据：`ParserXTP.cpp:57-71` 只删除冒号，其余字符交给无字符校验的 `convert::to_uint32`（`src/Share/Converter.hpp:57-60`）。初次复核时，`login_success_with_dashed_trading_day_corrupts_the_trading_date` 在 fake API 返回 `2026-01-05` 时通过，观察到脏值 `2025700705`。
- 接口边界：本地 XTP `xtp_quote_api.h:238-241` 只说明登录成功后 `GetTradingDay()` 才会返回正确交易日，没有说明允许连字符/冒号格式。故不能从 fake 输入推出生产行情交易日会损坏；T1 中对带小数点时间字符串的预期也不是本条的日期格式证据。
- 修复/验证方向：向厂商确认 `GetTradingDay()` 的返回格式；解析时要求恰好 8 位数字并在无效时保留未就绪状态/报错。使用格式符合合同的测试验证正常路径，再对非法格式验证受控拒绝。

## BUG-T27-002 — 重登录重复订阅同一集合

- 状态: 已撤回
- 复核状态：`误报；成员集合是重登录时需要重放的期望订阅`。
- 证据：`DoSubscribeMD()` 复制 `m_fitSHSubs/m_fitSZSubs` 到局部 `codeFilter`，`codeFilter.clear()` 不会清空成员集合（`ParserXTP.cpp:499-582`）。本地供应商接口明确要求登录行情服务器后重新订阅：`src/API/XTP2.2.32.2/xtp_quote_api.h:293-299` 和 `:325-331`。因此保留集合并在再次登录时下发，是恢复订阅所需行为。
- 测试边界：初次复核运行的 `second_login_resubscribes_the_very_same_codes` 为 `PASS`，但它只是对同一 fake API 连续调用 `connect()`，不模拟连接断开，也没有证明重复调用会触发订阅限额或重订失败。不能据此声称“订阅重发风暴”或“额度超限”。
- 处置：关闭为误报，不清空成员期望集合。若需要覆盖真实 reconnect，应在 fake API 中分离 disconnect/login 状态，并验证重登录后每个期望品种至少被重新订阅一次；确认重复请求语义前不新增去重逻辑。

## BUG-T27-003 — 登录后动态订阅缺少逐笔订阅

- 复核状态：`实现能力不一致；是否为产品缺陷取决于动态订阅契约，建议按初始化路径补齐`。
- 证据：初次登录的 `DoSubscribeMD()` 对 SSE、SZSE 都调用 `SubscribeMarketData` 和 `SubscribeTickByTick`（`ParserXTP.cpp:510-534,552-576`）；已登录后的 `subscribe()` 只调用 `SubscribeMarketData`（`:625-678`）。初次复核运行的 `post_login_subscribe_sends_market_data_but_never_tick_by_tick` 通过，证明 stub 观察到该差异。XTP SDK 有逐笔订阅接口（`xtp_quote_api.h:325-331`），ParserXTP 也消费逐笔数据并转换为订单明细/成交记录（`ParserXTP.cpp:203-282`）。
- 证据边界：现有 `IParserApi::subscribe` 没有单独的 “tick-by-tick” 参数；按初次订阅行为推断动态新增应获得相同能力是合理产品推断，但尚无单独文档写明。动态代码会拿到快照、不拿逐笔已由源码与 stub 确认；对具体策略数据完整性的外部影响没有运行时量化。
- 修复方向：明确 XTP Parser 的 `subscribe` 应覆盖的行情类型；若与启动订阅一致，在两个交易所分支补 `SubscribeTickByTick`、返回码日志及逐笔异步失败处理。测试分别断言动态新增品种的 MD/TBT 代码、交易所和错误回报。

## BUG-T27-004 — 登录失败事件的异步 handler 未在执行时判 sink

- 复核状态：`条件性异步生命周期风险；未运行禁用用例或 XTP 实际回调`。
- 证据：`DoLogin()` 的两个失败 handler 在入队前检查 `m_sink`，但执行时直接解引用（`ParserXTP.cpp:457-476`）；成功 handler 在执行时重新判空（`:482-488`）。`registerSpi(nullptr)` 可以将 sink 清空（`:690-696`），所以 handler 排队后到执行前摘除 sink 会形成 NULL 解引用路径。
- 边界：`DISABLED_posted_login_handlers_deref_sink_nulled_after_post` 用 pump 人工控制顺序，本轮未运行；该测试证明的是公开对象在异步队列未排空时 detach 的场景，不是厂商 API 自己会产生该时序。单纯捕获 sink 指针仍需保证 sink 生命周期，不能只靠快照指针解决释放竞态。
- 修复方向：规定 `registerSpi(nullptr)` 与 pending handler 的生命周期契约；在执行时做有效性/生命周期保护，或在 detach 前停止并排空异步队列。增加“失败事件已排队→detach→pump”与“sink 保持有效→事件送达”对照测试。

## BUG-T27-005 — 深圳逐笔方向/成交标记被直接强转

- 复核状态：`确认的数据映射缺陷；修正测试后本机复现`。
- 证据：本地 vendor 头 `xquote_api_struct.h:333-338` 规定 SZ entrust `side` 为 `'1'` 买、`'2'` 卖、`'G'` 借入、`'F'` 出借；`:361-363` 规定 SZ `trade_flag` 为 `'4'` 撤单、`'F'` 成交。`ParserXTP.cpp:247,271` 将字节原样强转成 `WTSBSDirectType`。旧测试固化了这些错误结果；现已改为分别断言买、卖和成交/撤单标记无方向。此前三项分别读到 `'1'`、`'2'` 和 `BDT_Lend` 而 FAIL；新增修正后的撤单方向回归也单跑 FAIL，实际为字符 `'4'` 而非 `BDT_Unknown`。
- 测试修正：深圳委托 fixture 已将非法 `'S'` 改为 SDK 的卖方向 `'2'`，假基础数据管理器也已按交易所匹配。跨市场主档不匹配的独立控制 PASS；真实 SDK 网络回调未运行。
- 修复/测试方向：按交易所把 entrust side 映射为 WTS 方向，至少覆盖 SZ `'1'→BDT_Buy`、`'2'→BDT_Sell`、`'G'/'F'` 的借贷语义及未知值。SZ `trade_flag` 是成交类型标识，不是主动买卖方向；如无额外可用字段，应按产品契约映射为 `BDT_Unknown`，同时 `'F'→TT_Match`、`'4'→TT_Cancel`。保留 SH B/S/N 的单独测试，不把 SZ 字段和 SH 语义混用。

## BUG-T27-006 — 用错交易所查合约、落库交易所来自主档

- 状态: 已撤回
- 复核状态：`测试夹具误报；当前证据不足以认定产品映射错误`。
- 证据：旧测试用 SZ 报文和 SSE 主档，假管理器只比较 code、不比较 exchg，因而错误返回跨市场合约。现已按 `(code, exchg)` 匹配，独立的不匹配控制测试 PASS；正式接口 `IBaseDataMgr.h:40` 同时接收两个参数。
- 结论与修复方向：没有证据证明真实数据管理器会以不同交易所返回该合约，也没有接口契约证明落库必须采用报文交易所。应关闭产品缺陷结论。若测试主档错配时应拒绝，FakeBdMgr 必须按 `(code, exchg)` 匹配并预期无合约/不推送；若要测“主档为准”或“报文为准”，显式构造 alias 并先写清预期策略，不能把当前 fake 当生产查表语义。

## BUG-T27-007 — `XTP_TBT_STATE` 没有落地记录

- 复核状态：`已确认当前不处理该类型；是否构成缺陷需产品能力范围确认`。
- 证据：本地 `xtp_api_data_type.h:383-389` 将 `XTP_TBT_STATE=3` 定义为 2.2.32 新增的上海新债券 Level2 逐笔状态订单；`XTPTickByTickStatus` 结构存在（`xquote_api_struct.h:367-375`）。`OnTickByTick()` 只处理 ENTRUST 和 TRADE（`ParserXTP.cpp:233-283`）。原测试把静默丢弃当通过条件，现改为禁用的能力待定占位，不将丢弃视为合规行为。
- 证据边界：接口头证明 vendor 有此数据，不证明 ParserXTP 对上海新债券承诺支持，且通用 WTS 记录是否能表达状态字段尚未确定。故不把静默丢弃直接升级为已确认产品缺陷。
- 修复方向：产品若支持该品类，应定义 WTS 表达与回放语义、补状态订单映射及字段级测试；若暂不支持，至少记录/文档说明不支持类型，避免监控侧无法区分“无状态单”与“被忽略”。

## BUG-T27-008 — API 为空时登录后 `subscribe()` 只缓冲

- 状态: 已撤回
- 复核状态：`误报；期望订阅集合在断线时保留，待下次登录重放`。
- 证据：`disconnect()` 释放 API 并置空（`ParserXTP.cpp:163-173`），但不清理 `m_uTradingDate` 和订阅集合；随后 `subscribe()` 将代码放入成员期望集合，因 `m_pUserAPI==NULL` 不发送（`:592-678`）。下次登录 `DoLogin()` 成功后会调用 `DoSubscribeMD()`（`:479-494`）。XTP 本地 SDK 明确断开/重登录后需要重订行情（`xtp_quote_api.h:293-299,325-331`）。
- 测试边界：初次复核运行的 `post_login_subscribe_without_api_buffers_only` 为 `PASS`，只证明断开后 API 为空时代码被缓存；没有重连并验证重放。断开期间没有市场连接，不能把不下发本身视为静默丢失。
- 处置：关闭为误报，并与 T27-002 的“期望集合保留以供重登录”归并说明。可将测试扩展为 disconnect→subscribe→login/reconnect，断言新代码在重连后被发送；另有 `unsubscribe()` 空实现已由 T1-XTP-05 登记，不在本条重复。

## BUG-T27-009 — ticker 固定数组未做 NUL 边界限制

- 状态: 已撤回
- 复核状态：`误报；本地供应商 SDK 结构注释明确保证 NUL 结尾`。
- 证据：本地 XTP 头 `src/API/XTP2.2.32.2/xquote_api_struct.h:183-189` 对 `XTPMarketDataStruct::ticker` 写明“不带空格，以 `\0` 结尾”；逐笔结构 `:377-381` 对 `XTPTickByTickStruct::ticker` 有同样约定。ParserXTP 在行情中把该字段转为 `std::string`（`ParserXTP.cpp:305`），在行情/逐笔错误日志传入该字段（`:311,224`）。原条目假设厂商填满数组且无终止符，与本地 SDK 描述的合规输入前提冲突。
- 结论：在供应商 SDK 合规输入下，本条所称 ticker 越界读没有证据；按错误输入人为造无 NUL 测试不能证明生产缺陷。`write_log()` 使用固定 buffer 仍是通用实现风险，但本条 ticker 字段在该接口中有上限和终止约定，不能据此声称会造成 512 字节溢出。
- 处置：关闭为误报。若未来要提高对损坏报文的防御，可以使用有界字符串构造，但应作为通用硬化事项，不保留本条的生产影响结论。

## 汇总

| 处置 | 编号 | 结论 |
| --- | --- | --- |
| 确认或较强的实现缺口 | T27-003、005 | T27-003 的逐笔动态订阅需以产品是否承诺同等行情类型为准；T27-005 有本地 XTP 字段定义支持 |
| 条件风险/能力待定 | T27-001、004、007 | 日期格式、异步 detach 时序、状态单能力范围分别待接口/产品确认 |
| 误报/合并关闭 | T27-002、006、008、009 | 保留重连期望集合；FakeBdMgr 忽略交易所；ticker 有 NUL 结构契约 |
| 测试需要修正或增强 | T27-005、006、008 | SZ entrust fixture 改为 side `'2'`；fake 查表遵守 exchange key；补断线后订阅重放断言 |
| 动态检查 | 修正后的产品契约回归存在 FAIL | 深圳委托、成交与撤单方向有本地 fake 输入证据；状态类型能力仍未确定，真实 XTP 未运行。 |
