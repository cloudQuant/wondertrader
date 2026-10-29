# R13 缺陷清单 — `src/WtPorter/`（WtPorter.cpp C 导出族 + Exp*Context 转发层）

> 只读生产代码，本文件只登记不修复；每条都带一行独立成行的 `- 状态: 待审核`。
> 用例账见 `docs/迭代计划/迭代01/manifests/R13_wtporter.md`。
> 崩溃/挂死/落文件类一律整条 `DISABLED_`，触发条件写在用例名下面的注释里。
> 与 R6/Q06/R42/R163 已登记条目重复的，在对应条目处指回；本文件保留实盘导出族的具体发生点，不重复计数。

## BUG-R13-001 createHftContext 绑定交易通道失败仍返回可用句柄，而 _trader 从未初始化

- 迭代02第二轮前状态（历史）: - 状态: 待审核
- 状态: 已合并（迭代02：主编号 BUG-R42-002）
- 严重级别: Blocker
- 位置: `src/WtPorter/WtRtRunner.cpp:321-333`（`if(trdPtr){ ctx->setTrader(...); } else { WTSLogger::error(...); }` 之后 `return ctx->id();`）；成员声明 `src/WtCore/HftStraBaseCtx.h:673`（`TraderAdapter* _trader;` 无默认初值），构造列表 `src/WtCore/HftStraBaseCtx.cpp:41-48` 也没列它
- 现象: 交易通道名不存在（配置里的 trader id 与 `create_hft_context` 传的名字不一致、或 traders 段整段缺失）时，函数只打一行 error 就把句柄返回给调用方；该句柄在 `WtPorter.cpp` 的每一道 `if (ctx == NULL)` 守卫眼里都是「合法上下文」。此后 `hft_get_position`（`WtPorter.cpp:1668`→`HftStraBaseCtx.cpp:641/645`）、`hft_get_undone`（`:1725`→`HftStraBaseCtx.cpp:651-679`）、`hft_cancel`（`:2100`→`HftStraBaseCtx.cpp:188-191`）、`hft_cancel_all`（`:2121`→`:193-199`）、`hft_buy/hft_sell`（`:2157/:2193`→`:211/:262`）、以及 `ExpHftContext::on_channel_ready/on_channel_lost`（`src/WtPorter/ExpHftContext.cpp:53`、`:66` 的 `_trader->id()`）全部在 `new` 出来的堆对象上解引用一个未初始化指针。
- 影响范围: ①实盘侧「通道名拼错」从可恢复错误变成随机地址解引用（不是可 catch 的异常，导出层的 `catch(...)` 抓不到）；②本轮 HFT 导出族里 6 个入口（约 30 行）只能整条 `DISABLED_`，取样口径下拿不到；③同一句柄在 `hft_get_position_profit/avgpx`（只读 `_pos_map`）上又是安全的，行为差异完全取决于实现细节，集成方无法用一次冒烟测覆盖。
- 相关测试: `r13_porter_sel_hft_export_test.cpp:R13PorterHftExport.DISABLED_hft_trader_bound_entries_deref_unbound_pointer`（未启用）；启用对照 `R13PorterHftExport.map_getters_userdata_and_logging_use_registered_dummy_trader` 使用已登记的 id-only adapter，在隔离子进程中 PASS。该 adapter 没有交易 API，不证明真实通道就绪。
- 证据等级: 源码证据；本轮相关崩溃用例为 `DISABLED_`，未在本次审查中运行。
- 备注: `_trader` 未初始化本身与既有 BUG-R42-002 重复。此条额外记录的是 `createHftContext` 在绑定失败后仍把上下文留在 map 并返回非零句柄的 runner 行为；与 BUG-R6-009 的 WtRtRunner 成员漏初始化不是同一根因。建议先在 HFT context 构造时设 `_trader = NULL`，再明确无 trader 是否允许创建；若不允许，应在失败时从引擎 map 回滚上下文并返回 0。所有依赖 trader 的入口仍需在空指针时安全失败，本轮不实施。

## BUG-R13-002 HftStraBaseCtx 的 _ud_modified 未初始化，ExpHftContext 的转发尾段读它

- 状态: 已合并（主记录 BUG-R42-002；同一 _ud_modified 未初始化字段/回调读取问题）
- 严重级别: Major
- 位置: `src/WtCore/HftStraBaseCtx.h:686`（`bool _ud_modified;` 无默认初值）、构造列表 `src/WtCore/HftStraBaseCtx.cpp:41-48`（只列了 `_engine/_slippage/_data_agent`）；消费点 `HftStraBaseCtx.cpp:143-150`（on_tick）、`:152-158`（on_order_queue）、`:161-167`（on_order_detail）、`:170-176`（on_transaction）、`:179-186`（on_bar）、`:536-545`（on_trade）、`:555-567`（on_order）、`:569-585`（on_channel_ready/lost）、`:587-594`（on_entrust）
- 现象: `ExpHftContext` 的各个非 NULL `on_*` 在转发后会调用 `HftStraBaseCtx::on_*`；基类在多个回调内直接读取未初始化的 `_ud_modified`（消费点见位置）。若该未定义读取表现为真，会调用 `save_userdata()`，它按当前 context 的 `_name` 形成 `ud_<name>.json`，尝试新建并写当前 `_user_datas`（`HftStraBaseCtx.cpp:726-750`）。同族 CTA/SEL 已显式初始化同类标志（`CtaStraBaseCtx.cpp:67`、`SelStraBaseCtx.cpp:49`）。
- 影响范围: 非 NULL HFT 回调读取未初始化 `bool`，构成未定义行为；在某些运行中可能出现意外的空用户数据文件副作用。文件名来自当前 context 的 `_name`，不是上一份策略名；已有文件的写入/覆盖行为以及实盘中的具体副作用未在本轮实测。
- 相关测试: `r13_porter_expctx_forward_test.cpp:R13ExpContext.hft_on_bar_with_null_bar_returns_before_any_forward`（只取守卫支）、`R13ExpContext.DISABLED_init_and_session_end_and_hft_trader_paths_need_output_and_trader_seams`
- 证据等级: 源码证据；启用用例只执行 `newBar == NULL` 的早退守卫，没有走到基类读取 `_ud_modified` 的非空回调尾段；相关 DISABLED 用例未在本次审查中运行。
- 备注: `_ud_modified` 未初始化与既有 BUG-R42-002 完全重复（同一成员、同一构造函数），不另计一条。`_trader` 与 `_ud_modified` 虽都在 `HftStraBaseCtx` 构造函数漏初始化，但应按 BUG-R42-002 的同一修复项处理；修复方向是成员默认初始化 `_ud_modified = false`。

## BUG-R13-003 sel_get_last_entertag 在 ctx==NULL 时 return 0，把空指针递给 C 调用方

- 状态: 已合并（主记录 Q06-001；CTA/SEL 空句柄返回 NULL 的同类 WtString 缺陷，本项 SEL occurrence 已引用）
- 严重级别: Major
- 位置: `src/WtPorter/WtPorter.cpp:1629-1634`（`WtString sel_get_last_entertag(...)` 里 `if (ctx == NULL) return 0;`）；同形一份在 `src/WtPorter/WtPorter.cpp:839-846`（`cta_get_last_entertag`）
- 现象: 函数返回类型是 `WtString`（`const char*`），未命中上下文时返回整数 0 而不是 `""` 或某个默认串；同族的 `hft_load_userdata`（`:2244-2251`）、`sel_load_userdata`（`:1166`）都老老实实返回 `defVal`，只有 entertag 这两个入口给空指针。Python/C# 绑定拿到以后一律 `strlen`/`decode`，直接段错误。
- 影响范围: 「策略还没成交」是正常状态，返回值却是不可用的指针；两个入口约 12 行的错误路径语义与同族不一致。
- 相关测试: 本轮 R13 启用用例不验证 unknown-handle 的期望值；R35 的 `SelLastEnterTagRealHandleIsBlankStringUnknownIsNull`（:183-190）把 unknown 分支断言为 `nullptr`，这是对现状的 characterization，不是期望行为，修复时应翻转为非空空串。
- 证据等级: 源码证据；`WtPorter.cpp:1632-1635` 的空句柄支返回 NULL，`SelStraBaseCtx.cpp:1094-1104` 的空持仓支返回 `""`。本次未运行 SEL 空句柄用例。
- 备注: CTA 同形已由 `bugs/q06_porters.md` BUG-Q06-001 登记；SEL 是同一缺陷模式的另一导出 occurrence，建议并入该条扩展位置，不单独重复计数。修复方向是空句柄返回 `""`，并把 CTA/SEL 行为断言统一到安全字符串。

## BUG-R13-004 文档示例 period 被映射为 TPT_None，策略上下文建成但永不调度

- 状态: 待审核
- 严重级别: Major
- 位置: public API 文档 `src/WtPorter/WtPorter.h:686-700` 写明 `period 如 d1/w1/m1 等`；实现 `src/WtPorter/WtRtRunner.cpp:338-358` 只识别 `d/w/m/y/min`，其它输入均设为 `TPT_None` 并仍创建上下文、返回句柄；`src/WtCore/WtSelEngine.cpp:260-265,325-333` 中 `TPT_None` 保持 `bIgnore=true` 后 continue，不会触发 `on_schedule`。
- 现象: 文档直接给出的 `d1/w1/m1` 三种示例均不被解析，落入 `TPT_None`，句柄有效但任务不执行；任意未知字符串也同样静默进入该分支。
- 影响范围: 用户按公开头文件示例创建策略时，配置成功但策略调度静默失效。这里不只是假设用户把 K 线周期写法误传给 API，公开示例本身与实现不一致。
- 相关测试: 原测试把 `w/m/y/min/d` 称作所有文档周期，还把未知字符串返回句柄当成通过条件；现改为 `create_sel_context_registers_contexts_for_supported_period_tokens`，仅验证这些实现识别的 token 能注册上下文及句柄区分。修正后单独运行 PASS，但不验证调度；文档示例和未知输入的行为需要在确定 API 规范后另做调度级回归。
- 证据等级: 文档与生产源码静态证据；本次未运行调度线程/`on_schedule` 回调来独立验证端到端触发结果。
- 备注: 修复需先确定 API 规范：按当前 public header 把 `d1/w1/m1`（及需支持的周期）映射到正确 `TaskPeriodType`，或同步更改 public header；未知输入应有明确、可观察的处理策略。`TPT_None` 注释虽为“不重复”，当前 scheduler 分支并不执行一次性回调。

## BUG-R13-005 hft_buy/hft_sell 用函数级 static std::string 回传，hft_cancel_all 却是 thread_local

- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtPorter/WtPorter.cpp:2157-2181`（`hft_buy`：`static std::string ret;`）、`:2193-2217`（`hft_sell`：同）、对照 `:2121-2145`（`hft_cancel_all`：`static thread_local std::string ret;`）
- 现象: `hft_buy` 与 `hft_sell` 各自使用函数级 `static std::string`，并在每次调用时赋值后返回 `ret.c_str()`（`WtPorter.cpp:2157-2211`）；同一函数若被多个线程同时调用，会并发读写同一个 string，构成数据竞争。`hft_cancel_all` 使用 `static thread_local std::string`（:2121-2145），说明实现口径不同，但不能单凭这一点证明公开 API 承诺并发安全。
- 影响范围: 若宿主允许并发调用同一 buy/sell 导出入口，返回缓冲有数据竞争；连续同线程调用同一入口也会覆盖旧指针所指内容。当前没有证据证明 Python/C# 调用方是否在下一次调用前复制，故不据此断言必然“撤单撤到别人的委托”。
- 相关测试: 无启用用例；未绑定 `_trader` 的禁用路径见 BUG-R13-001。本次未运行并发复现，也未验证订单 ID 非空的真实返回串。
- 证据等级: 源码证实共享 `static` 缓冲；并发发生条件及调用方指针生命周期契约待验证。`bugs/R163.md:76-78` 已记录同一 WtPorter 路径并明确不重复登记；`BUG-R83-002` 是 WtBtPorter 的对应实现，应作同族交叉引用，而不是重复计同一发生点。
- 备注: 如并发调用属于受支持的使用方式，`thread_local` 可消除跨线程共享，但返回指针仍会被同线程下一次相同函数调用覆盖；更稳妥的 C ABI 是调用方提供输出缓冲，或明文约定返回指针有效期。未确认契约前，保留为待验证并发缺陷，不宣称实测影响。

## BUG-R13-006 导出层有 create 无 destroy，上下文永久累积在引擎的 _ctx_map 里

- 迭代02第二轮前状态（历史）: - 状态: 待审核
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: Minor（仅当支持运行期反复创建/重载上下文时；生命周期契约待确认）
- 位置: `src/WtPorter/WtPorter.cpp:404`（create_cta_context）、`:1135`（create_sel_context）、`:1652`（create_hft_context）；`src/WtPorter/WtRtRunner.cpp:314-355` 只有 `addContext`，没有 `delContext`/`closeContext`；`src/WtCore/WtCtaEngine.cpp:133-137` 的 `_ctx_map[sid] = ctx` 也没有删除入口
- 现象: `create_*_context` 把新对象以 shared_ptr 放进各引擎的 context map（CTA：`WtCtaEngine.cpp:133-137`）；WtPorter.h 只有 create、没有 destroy/close 入口。`release_porter` 调 `WtRtRunner::release()`（WtPorter.cpp:286-289），后者释放 adapter、当前 `_engine` 和 logger，但不清 context maps（WtRtRunner.cpp:1141-1155；各 Engine::release 也不清 `_ctx_map`）。源码因此证明重复 create 会让 map 持有对象直至 runner/进程析构；是否构成支持场景中的泄漏，取决于宿主是否允许运行期反复创建/重载上下文。
- 影响范围与证据边界: 若宿主循环创建短期 context，内存会随调用数增长；仓库没有证据表明策略热重载是受支持工作流。R13 manifest 记录测试夹具创建 15 个上下文并留在单例直到进程结束（manifests/R13_wtporter.md:161），这是测试行为，不能单独证明生产中会持续增长。
- 相关测试: `r13_porter_cta_export_test.cpp:R13PorterCtaExport.userdata_is_per_context_not_shared_between_two_ctas` 证明同名不同句柄有独立状态，不验证 destroy/release 清理或内存增长。
- 证据等级: context 强引用、无导出删除入口、release 不清 map 均为源码证据；运行期重载是否属于公开生命周期契约待产品确认，本次未做增长/清理实验。
- 备注: 若支持动态创建/重载，补三族 destroy API 并同步移除 `_ctx_map`/SEL `_tasks`；HFT 还需从 trader adapter detach sink 后再释放上下文，`release_porter` 也应清除 maps。若上下文设计为进程级固定对象，应在 API 文档明确只能创建一次/进程生命周期，避免把条件性风险表述为必然的长时间生产泄漏。

## BUG-R13-007 C 字符串空指针边界需按入口拆分；write_log occurrence 已登记

- 状态: 已合并（迭代02：主编号 Q06-006）
- 严重级别: Minor
- 位置: `WtPorter.cpp:260-266` 的 `config_porter` 对 `cfgfile` 调 `strlen`；`:333-343` 的 `write_log` 对 `catName` 调 `strlen`。本文件实际只有这两处直接 `strlen`。
- 现象与边界: `cfgfile == NULL` 或 `catName == NULL` 时这两行存在未定义行为，这是源码可证的条件性结果；但 C API 未声明 NULL 为可接受输入，`config_porter` 只明确支持空字符串走默认 `config.json`。因此是否按产品缺陷接收，仍需确认各入口的 nullability 契约。`feed_adj_factors`（:209-212）只把 `stdCode` 指针转发给注册回调，本函数没有 `strlen` 或 `std::string(NULL)`；其回调如何处理 NULL 未由本条证明。`write_log` 的 `message` 也只是向 WTSLogger 转发，不能从本条直接推断具体 NULL 后果。
- 影响范围: 仅对明确允许 NULL 的入口可按功能缺陷定级；对必填字符串参数，这是无效输入时缺少防御处理的鲁棒性风险，不应概括成“所有 C 字符串入口都一律 strlen 并崩溃”。
- 相关测试: 本条没有 NULL 入参的启用用例；本次未独立运行 NULL 调用。`write_log(catName=NULL)` 与 `bugs/q06_porters.md` Q06-006 是完全相同的实盘导出行，不应重复计数；parser ID 的 NULL 路径已由 BUG-R6-004 覆盖。
- 备注: 建议拆分并按契约处理：若 `config_porter(NULL, ...)` 应等同空串，则入口显式判空后走默认配置；`write_log` 的 `catName` 可按 Q06-006 统一空分类语义。对 `message`、`stdCode`、ID 等必填参数逐项拒绝并记错，不建议用全局 `SAFESTR` 把非法 NULL 静默转换成空串。

- 迭代02复核: DUPLICATE。本条把 config_porter 与 write_log 的 C 字符串边界合并描述；write_log(catName) 的同一生产行已由 Q06-006 覆盖，不能重复计数。config_porter 的 cfgfile NULLability 也未由公开契约证明为有效输入。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
