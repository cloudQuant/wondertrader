# T27_parserxtp（XTP 行情解析器）疑似缺陷

> 状态一律「待审核」；本迭代不修改 `src/**`。
> 行号来自本轮直读 `src/ParserXTP/ParserXTP.cpp`（696 行）、`ParserXTP.h`（131 行）、
> `src/Share/Converter.hpp`、`src/API/XTP2.2.32.2/{xquote_api_struct.h,xtp_api_struct_common.h}`。
> 与 T1 lane 的 `T1-XTP-01…05` 不重复：`init()` NULL-call、`connect()` 不可回收线程、构造函数漏初始化、
> 期货结算字段不一致、`unsubscribe()` 空实现这五条已由 T1 登记，本文件只在需要时补可复现入口。

## BUG-T27-001

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/ParserXTP/ParserXTP.cpp`（`strToTime` L57-71、`DoLogin` L481）、`src/Share/Converter.hpp`（`convert::to_uint32` L57-60）
- 相关测试: `src/TestUnits/coverage/t_ParserXTP/t27_parserxtp_api_seam_test.cpp` → `XtpLoginF.login_success_with_dashed_trading_day_corrupts_the_trading_date`、`XtpLoginF.login_success_with_colon_separated_trading_day_strips_only_colons`、`src/TestUnits/coverage/t_ParserXTP/t27_parserxtp_data_state_test.cpp` → `XtpLoginF.corrupt_trading_day_leaks_into_snapshot_and_queue_records`
- 输入/前置条件: 厂商 `GetTradingDay()` 返回的交易日串不是纯数字。`strToTime` 只剥 `':'`，其余字符（`'-'`、`'.'`、空格）原样拼进字符串，再交给 `convert::to_uint32`，而后者是 `h*10 + *str - '0'` 且**完全不校验字符是否是数字**。
- 期望行为: 非数字分隔符应被跳过或整个串被判非法并记错误日志，`m_uTradingDate` 保持 0/上一个有效值。
- 实际行为: `"2026-01-05"` → 逐位累加，`'-'`(45) 贡献 `45-48 = -3` → `m_uTradingDate = 2025700705`（不是 20260105，也不是溢出报错，是一个看起来像日期的脏值）。该值随后写进 `WTSTickStruct.trading_date`（L329）与买卖两个 `WTSOrdQueStruct.trading_date`（L368、L396），落库后可污染按交易日分片的索引。`"14:59:59.999"` 同理得 `1459588999`（这正是 T1 用例 `time_and_price_helpers_strip_colons_and_guard_sentinels`  currently FAIL 的原因，期望值 `145959999` 是按「产品会剥掉 `.`」写的，而产品不会）。
- 复现步骤: 三条用例任一单独跑（`/tmp` 下用 `case_runner.sh` 或直接 `--gtest_filter=XtpLoginF.login_success_with_dashed_trading_day_corrupts_the_trading_date`）。断言 `EXPECT_EQ(2025700705u, m->m_uTradingDate)` 当前为真。
- 影响范围: 行情快照与逐笔委托队列的 `trading_date` 字段；下游按交易日取历史切片/建索引（WtDataStorage 的日期分片、回放对齐）都会拿到查不到的日期。XTP 目前实测回的是 `YYYYMMDD`，所以正常链路不命中，一旦厂商换格式或本机时区/夏令时导致带分隔符就静默错。
- 备注: 修复方向有二：`strToTime` 里把非 `0-9` 字符全部跳过（而不是只跳 `:`），或 `convert::to_uint32` 加数字校验后返回失败。建议前者（后者是公用工具，改动面大）。同一函数还有 `strlen(pos)` 在循环里逐字符重算的 O(n²) 小毛病，不单开条目。

## BUG-T27-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/ParserXTP/ParserXTP.cpp`（`DoSubscribeMD` L497-582，关键在 L499、L536、L541、L578）
- 相关测试: `t27_parserxtp_api_seam_test.cpp` → `XtpLoginF.subscribe_md_clears_only_its_local_copy_so_buffers_survive`、`XtpLoginF.second_login_resubscribes_the_very_same_codes`
- 输入/前置条件: 登录成功一次（`DoLogin` L493 调 `DoSubscribeMD`），随后再登录一次（重连链路：`OnDisconnected` L191-195 的 lambda 睡 2 秒后调 `DoLogin`）。
- 期望行为: 订阅下发成功后应把已下发的代码从待发集合里摘掉（或维护一个「已订阅」集合做差集），重连时只补差异。
- 实际行为: `CodeSet codeFilter = m_fitSHSubs;` 取的是**局部拷贝**，L536/L578 的 `codeFilter.clear()` 清的是这个拷贝，成员集合 `m_fitSHSubs/m_fitSZSubs` 只增不减。两次登录的 `SubscribeMarketData` 入参数组逐元素相同（实测 `mdCalls[0]==mdCalls[2]`、`mdCalls[1]==mdCalls[3]`），即每次重连都把全量代码再发一遍。
- 复现步骤: 跑 `XtpLoginF.second_login_resubscribes_the_very_same_codes`；断言 `api.mdCalls.size()==4` 且两组内容相同。
- 影响范围: 网络抖动/心跳超时反复触发 `OnDisconnected → DoLogin` 时形成订阅重发风暴；柜台对同一 client_id 的订阅列表有额度限制时（XTP 侧确有「订阅数量上限」），重发可能直接换来错误码，进而走进 L513-517 的失败日志且永不自愈。`m_fitSHSubs/m_fitSZSubs` 只增不减也意味着策略侧换月/换标的后旧代码永远留在集合里。
- 备注: 两处的 `codeFilter.clear()` 字面上就是想清成员集合（否则写它没有意义），属笔误级缺陷。修复方向：改成 `m_fitSHSubs.clear()` / 或按已下发代码做 `erase`。

## BUG-T27-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/ParserXTP/ParserXTP.cpp`（`subscribe` 登录后分支 L608-678，对比 `DoSubscribeMD` L512-534、L554-576）
- 相关测试: `t27_parserxtp_api_seam_test.cpp` → `XtpOnlineF.post_login_subscribe_sends_market_data_but_never_tick_by_tick`
- 输入/前置条件: 先登录成功（`m_uTradingDate != 0`），之后策略再新增订阅一只代码。
- 期望行为: 与 `DoSubscribeMD` 一致，登录后新增代码同样要同时下 `SubscribeMarketData` 与 `SubscribeTickByTick`。
- 实际行为: 登录后的直订分支只调 `SubscribeMarketData`（L637、L664），**没有任何 `SubscribeTickByTick` 调用**；实测 `api.tbtCalls` 在登录后订阅 1 只新代码后仍为 0 次。
- 复现步骤: 跑该用例；断言 `api.mdCalls.size()==1 && api.tbtCalls.empty()`。
- 影响范围: 登录后（含启动后动态换仓、盘中新增标的）新增的代码只能拿到快照 `OnDepthMarketData`，永远拿不到逐笔 `OnTickByTick` → `WTSOrdDtlData`/`WTSTransData` 缺失，Level2 委托对垒 `WTSOrdQueData` 也只在快照带 `bid1_qty/ask1_count` 时才有。这类缺失是静默的，日志里没有任何提示。
- 备注: 修复方向：在 L637 与 L664 之后各补一次 `SubscribeTickByTick`，并复用 L525-534 的日志分支。

## BUG-T27-004

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/ParserXTP/ParserXTP.cpp`（`DoLogin` L462-464、L471-473，对照 L482-488）
- 相关测试: `t27_parserxtp_api_seam_test.cpp` → `XtpLoginF.DISABLED_posted_login_handlers_deref_sink_nulled_after_post`（打开即 SIGSEGV，实测 rc=139）
- 输入/前置条件: 登录失败（`Login()` 返回 -1 或其他非 0），此时 `m_sink` 非空 → 通过 L457 的判定并把 handler post 进 `_asyncio`；在 worker 线程真正执行该 handler 之前，宿主调用 `registerSpi(nullptr)`（L692 会把 `m_sink` 置空）。
- 期望行为: 与成功分支的 lambda（L483 外面包了 `if (m_sink)`）一致，执行时再判一次空，或在入队处快照 sink 指针。
- 实际行为: 两个失败分支的 lambda 直接 `m_sink->handleEvent(WPE_Connect, iResult)` / `m_sink->handleEvent(WPE_Connect, 0)`，**没有 NULL 判定**。post 时的判定不构成执行时的保证（跨线程、跨时间），执行时解引用 NULL。
- 复现步骤: `--gtest_also_run_disabled_tests --gtest_filter=XtpLoginF.DISABLED_posted_login_handlers_deref_sink_null_ed_after_post`（单跑 rc=139）。
- 影响范围: 实盘停服/重配流程（先摘 sink 再断线重连）与 `OnDisconnected → 2 秒后 DoLogin` 的组合。崩在 worker 线程里，主线程拿不到任何可诊断信息。同类「入队时判定、执行时不判定」的写法在 L192-194（`write_log` 有守卫、`DoLogin` 又走一遍）里是安全的，只有这两个 lambda 漏了。
- 备注: 未改动产品代码，本条只用 DISABLED 用例钉住事实。修复方向：两个 lambda 内加 `if (m_sink)`，或捕获 `sink` 副本并在 handler 里判空。

## BUG-T27-005

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/ParserXTP/ParserXTP.cpp`（`OnTickByTick` L247-248、L271）
- 相关测试: `t27_parserxtp_data_state_test.cpp` → `XtpData27.tickbytick_szentrust_side_digit_falls_outside_the_bsdirect_enum`、`XtpData27.tickbytick_sztrade_match_flag_collides_with_the_lend_direction`、`XtpData27.tickbytick_sztrade_cancel_branch_marks_szse_master_exchange`
- 输入/前置条件: 深圳逐笔回报。`xquote_api_struct.h:333-338` 明确 `XTPTickByTickEntrust.side` 在深市是 `'1'`=买 / `'2'`=卖 / `'G'`=借入 / `'F'`=出借；`XTPTickByTickTrade.trade_flag` 在深市是 `'4'`=撤 / `'F'`=成交（同文件 L361-363）。而 `WTSTypes.h:383-390` 的 `WTSBSDirectType` 是 `BDT_Buy='B'`、`BDT_Sell='S'`、`BDT_Unknown=' '`、`BDT_Borrow='G'`、`BDT_Lend='F'`。
- 期望行为: 按交易所语义把 `side`/`trade_flag` 映射成 `WTSBSDirectType`（深市 `'1'→BDT_Buy`、`'2'→BDT_Sell`；成交标识不参与方向）。
- 实际行为: L247/L271 都是裸强转 `(WTSBSDirectType)eInfo.side` / `(WTSBSDirectType)tInfo.trade_flag`。实测钉死两个后果：① 深证逐笔委托方向存成 `'1'`(49)，既不等于 `BDT_Buy` 也不等于 `BDT_Sell`/`BDT_Unknown`，是枚举外的值；② 深证逐笔成交方向存成 `'F'`(70)，与 `BDT_Lend`（出借）**恰好相等**，即一笔普通成交被标成「借出」；撤单则存成 `'4'`(52)。同处的 `otype`（L248）反倒是通的，因为 `WTSOrdDetailType` 本身就是按 ASCII 定义的（`ODT_LimitPrice='2'`）。
- 复现步骤: 三条用例直接跑；`EXPECT_EQ(BDT_Lend, sink.lastTrans.side)` 当前为真。
- 影响范围: 深市逐笔的方向字段被下游（撮合回放、主动买卖统计、委托对垒重建）按 `BDT_Buy/BDT_Sell` 判断时全部落空或被误判成借出借入。属于数据正确性缺陷而非崩溃。
- 备注: 修复方向：`OnTickByTick` 里按 `tbt_data->exchange_id` 分两张小表映射 side；成交记录不应把 `trade_flag` 当方向（`ttype` 的判定 L272 是对的）。

## BUG-T27-006

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/ParserXTP/ParserXTP.cpp`（L210-220、L238、L262、L296-307、L319、L366、L394）
- 相关测试: `t27_parserxtp_data_state_test.cpp` → `XtpData27.tickbytick_szentrust_looks_up_szse_but_keeps_the_master_exchange`
- 输入/前置条件: 厂商报文的 `exchange_id` 与本地品种主档登记的交易所不一致（代码在两市场重号，例如 `000001` 既是深市个股也可能是某上交所品种的挂错主档）。
- 期望行为: 记录里的 `exchg` 应当与这条行情实际来源的交易所一致，或在两者不一致时告警。
- 实际行为: 查合约用的是报文换算出的 `SSE/SZSE`（L210-220、L296-305），但写进 `WTSTickStruct.exchg` / `WTSOrdDtlStruct.exchg` / `WTSOrdQueStruct.exchg` 的一律是 `commInfo->getExchg()`（L238、L262、L319、L366、L394）。实测：查询键 `SZSE.000001`，落库 `exchg="SSE"`。
- 复现步骤: 跑该用例；断言 `bd.lastExchg=="SZSE"` 且 `sink.lastOrdDtl.exchg` 为 `"SSE"`。
- 影响范围: 主档挂错交易所时，落库数据里的交易所与查询键互相矛盾，回放端按 `exchg.code` 再查主档会查到另一只。正常配置下两者一致，所以是低危。
- 备注: 也可能是有意为之（以主档为准），但缺一条不一致告警。

## BUG-T27-007

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/ParserXTP/ParserXTP.cpp`（`OnTickByTick` L233、L257）
- 相关测试: `t27_parserxtp_data_state_test.cpp` → `XtpData27.tickbytick_state_type_is_dropped_by_both_branches`
- 输入/前置条件: `tbt_data->type == XTP_TBT_STATE`（值为 3，`xtp_api_data_type.h:388`，XTP 2.2.32 新增，上海新债券 Level2 独有）。
- 期望行为: 要么按状态订单落一种 WTS 记录，要么明确记一条「不支持的逐笔类型」日志。
- 实际行为: `if(type==XTP_TBT_ENTRUST) … else if(type==XTP_TBT_TRADE) …` 两个分支都不进，函数直接结束；实测合约查到了（`getContractCalls==1`）但 `handleOrderDetail/handleTransaction` 都是 0 次、日志 0 条 → 静默丢弃。
- 复现步骤: 跑该用例。
- 影响范围: 上海新债券 Level2 的状态订单全部丢数据，且无从发现。产品侧的 `ParserXTP.h:80` 也没有更细分的发版差异。
- 备注: `XTPTickByTickStatus` 结构体在头文件里是有的，若需要支持可映射到 `WTSOrdDtlData` 的某新类型。

## BUG-T27-008

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/ParserXTP/ParserXTP.cpp`（`disconnect` L163-173、`subscribe` L594、L635）
- 相关测试: `t27_parserxtp_api_seam_test.cpp` → `XtpOnlineF.post_login_subscribe_without_api_buffers_only`
- 输入/前置条件: 登录成功后调用 `disconnect()`（`m_pUserAPI` 被置 NULL），随后再 `subscribe()` 新代码。
- 期望行为: 断开后 `subscribe` 要么拒绝并记日志，要么把状态机退回「未登录」让重连后的 `DoSubscribeMD` 补发。
- 实际行为: `disconnect()` 不清 `m_uTradingDate`（实测仍为 20260105），于是 `subscribe` 继续走「已登录」分支；该分支里 `if (m_pUserAPI && nCount > 0)`（L635/L662）判空为假，代码只进成员集合、**不下发也不报错**。集合不清空 → 下一次 `DoLogin` 会带上它们，但如果宿主只是断开没再重连（例如 `release()` 路径），这批代码就永久静默丢失。
- 复现步骤: 跑该用例；断言 `api.mdCalls` 未增长、`sink.logs` 为空、`m_fitSHSubs` 里出现了新代码。
- 影响范围: 停服重启/热切换数据源时新增订阅静默不生效；日志里没有任何线索。
- 备注: 与 T1-XTP-05（`unsubscribe` 是空实现）同属「订阅生命周期不完整」一族，但触发点不同，故单开一条。

## BUG-T27-009

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: 嫌疑位置：`src/ParserXTP/ParserXTP.cpp` L305（`code = market_data->ticker`）、L220/L307（`getContract(tbt_data->ticker / code.c_str(), …)`）、L224/L311（`{}` 里的 `ticker` 可变参）、L27-37（`write_log` 的 `fmt::format_to(buffer, …)` 写进 `static thread_local char buffer[512]`）；底层数组定义在 `src/API/XTP2.2.32.2/xquote_api_struct.h:20/189`（`char ticker[XTP_TICKER_LEN]`）
- 相关测试: 无启用用例（按本轮规则不构造越界输入去「证明」崩溃）；同族事实与判定见 `docs/迭代计划/迭代01/bugs/R16_share_tools.md`
- 输入/前置条件: 厂商在定长 `ticker` 数组里填满字符且**不以 `'\0'` 结尾**（XTP 文档要求带结尾符，但这是厂商侧约定，产品代码零校验）。
- 期望行为: 对定长数组按 `strnlen(arr, sizeof(arr))` 取串，或在拷贝前显式判结尾符。
- 实际行为: L305 用 `std::string operator=(const char*)` 构造 → 内部 `strlen` 会一路读穿数组尾；L224/L311 把裸 `ticker` 当 `%s` 交给 fmt，同样依赖结尾符。`write_log` 侧则是 `fmt::format_to(buffer, …)` 对 512 字节缓冲**无截断保护**（传的是裸 `char*`，fmt 拿不到容量）；`XTPRI.error_msg` 有 `XTP_ERR_MSG_LEN=124` 上限，单条错误日志安全，风险集中在未闭合的 `ticker`。本轮夹具一律用 `snprintf(..., sizeof(ticker), "%s", code)` 写成带结尾符的短串，所以这两处只被覆盖到「安全输入」这一侧。
- 复现步骤: 若要证明，需构造 `XTPMD` 使 `ticker` 填满 16 字节（`XTP_TICKER_LEN=16`，`xtp_api_data_type.h:21`）且无 `'\0'` 后调 `OnDepthMarketData`——本轮规则明确禁止用夹具制造崩溃，故未落用例，留给主控裁定是否单独开一条 `DISABLED_` 崩溃族用例。
- 影响范围: 越界读只影响日志与查询键的尾部字符，通常表现为「Instrument SZSE.xxxxx…乱码 not exists」；在 ASan 下会稳定报 global-buffer-overflow。
- 备注: 未证明的因果不下结论，这里只登记位置与判定方式。修复方向：两处 `ticker` 读取改 `std::string(arr, strnlen(arr, XTP_TICKER_LEN))`。
