# R13 缺陷清单 — `src/WtPorter/`（WtPorter.cpp C 导出族 + Exp*Context 转发层）

> 只读生产代码，本文件只登记不修复；每条都带一行独立成行的 `- 状态: 待审核`。
> 用例账见 `docs/迭代计划/迭代01/manifests/R13_wtporter.md`。
> 崩溃/挂死/落文件类一律整条 `DISABLED_`，触发条件写在用例名下面的注释里。
> 与 R6/Q06 已登记条目重复的，只在「同一根因的另一份拷贝」意义上补位，并在备注里指回原条。

## BUG-R13-001 createHftContext 绑定交易通道失败仍返回可用句柄，而 _trader 从未初始化

- 状态: 待审核
- 严重级别: Blocker
- 位置: `src/WtPorter/WtRtRunner.cpp:321-333`（`if(trdPtr){ ctx->setTrader(...); } else { WTSLogger::error(...); }` 之后 `return ctx->id();`）；成员声明 `src/WtCore/HftStraBaseCtx.h:673`（`TraderAdapter* _trader;` 无默认初值），构造列表 `src/WtCore/HftStraBaseCtx.cpp:41-48` 也没列它
- 现象: 交易通道名不存在（配置里的 trader id 与 `create_hft_context` 传的名字不一致、或 traders 段整段缺失）时，函数只打一行 error 就把句柄返回给调用方；该句柄在 `WtPorter.cpp` 的每一道 `if (ctx == NULL)` 守卫眼里都是「合法上下文」。此后 `hft_get_position`（`WtPorter.cpp:1668`→`HftStraBaseCtx.cpp:641/645`）、`hft_get_undone`（`:1725`→`HftStraBaseCtx.cpp:651-679`）、`hft_cancel`（`:2100`→`HftStraBaseCtx.cpp:188-191`）、`hft_cancel_all`（`:2121`→`:193-199`）、`hft_buy/hft_sell`（`:2157/:2193`→`:211/:262`）、以及 `ExpHftContext::on_channel_ready/on_channel_lost`（`src/WtPorter/ExpHftContext.cpp:53`、`:66` 的 `_trader->id()`）全部在 `new` 出来的堆对象上解引用一个未初始化指针。
- 影响范围: ①实盘侧「通道名拼错」从可恢复错误变成随机地址解引用（不是可 catch 的异常，导出层的 `catch(...)` 抓不到）；②本轮 HFT 导出族里 6 个入口（约 30 行）只能整条 `DISABLED_`，取样口径下拿不到；③同一句柄在 `hft_get_position_profit/avgpx`（只读 `_pos_map`）上又是安全的，行为差异完全取决于实现细节，集成方无法用一次冒烟测覆盖。
- 相关测试: `r13_porter_sel_hft_export_test.cpp:R13PorterHftExport.DISABLED_hft_trader_bound_entries_deref_unbound_pointer`（未启用）；启用对照 `R13PorterHftExport.hft_map_only_position_getters_are_exactly_zero`
- 备注: 与 BUG-R6-009（WtRtRunner 构造列表漏 4 个成员）是同一次改造的成组缺陷；修复方向是 `_trader = NULL` + 绑定失败时不返回句柄（或返回 0），本轮不实施。

## BUG-R13-002 HftStraBaseCtx 的 _ud_modified 未初始化，ExpHftContext 的转发尾段读它

- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtCore/HftStraBaseCtx.h:686`（`bool _ud_modified;` 无默认初值）、构造列表 `src/WtCore/HftStraBaseCtx.cpp:41-48`（只列了 `_engine/_slippage/_data_agent`）；消费点 `HftStraBaseCtx.cpp:143-150`（on_tick）、`:152-158`（on_order_queue）、`:161-167`（on_order_detail）、`:170-176`（on_transaction）、`:179-186`（on_bar）、`:536-545`（on_trade）、`:555-567`（on_order）、`:569-585`（on_channel_ready/lost）、`:587-594`（on_entrust）
- 现象: `ExpHftContext` 的每个 `on_*` 都是「先 `getRunner().xxx(...)` 转发，再调 `HftStraBaseCtx::on_*`」；而基类那一半无条件先读 `_ud_modified`，随机为真时立刻执行 `save_userdata()`（往 `WtHelper::getOutputDir()` 下建目录并写策略文件）。同族的 CTA/SEL 构造函数都列了这个成员（`CtaStraBaseCtx.cpp:67`、`SelStraBaseCtx.cpp:49`），只有 HFT 漏了。
- 影响范围: ①`ExpHftContext.cpp` 的 on_bar(:43)、on_tick(:176-187)、on_entrust(:83-86)、on_order(:144-147)、on_trade(:238-241) 共约 24 行在「进程内、不落文件」的取样口径下不可靠，本轮只取了 on_bar 的 NULL 守卫支（`:32-33`）；②实盘侧新建 HFT 策略的第一个 bar/tick 可能凭空产生一次用户数据落盘（写的是上一份策略残留的文件名），属于静默数据污染。
- 相关测试: `r13_porter_expctx_forward_test.cpp:R13ExpContext.hft_on_bar_with_null_bar_returns_before_any_forward`（只取守卫支）、`R13ExpContext.DISABLED_init_and_session_end_and_hft_trader_paths_need_output_and_trader_seams`
- 备注: 建议与 BUG-R13-001、BUG-R6-009 一并修（同一个构造列表）。

## BUG-R13-003 sel_get_last_entertag 在 ctx==NULL 时 return 0，把空指针递给 C 调用方

- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtPorter/WtPorter.cpp:1629-1634`（`WtString sel_get_last_entertag(...)` 里 `if (ctx == NULL) return 0;`）；同形一份在 `src/WtPorter/WtPorter.cpp:839-846`（`cta_get_last_entertag`）
- 现象: 函数返回类型是 `WtString`（`const char*`），未命中上下文时返回整数 0 而不是 `""` 或某个默认串；同族的 `hft_load_userdata`（`:2244-2251`）、`sel_load_userdata`（`:1166`）都老老实实返回 `defVal`，只有 entertag 这两个入口给空指针。Python/C# 绑定拿到以后一律 `strlen`/`decode`，直接段错误。
- 影响范围: 「策略还没成交」是正常状态，返回值却是不可用的指针；两个入口约 12 行的错误路径语义与同族不一致。
- 相关测试: 本轮启用用例不碰这两个入口（`cta_get_last_entertag` 已由 `bugs/q06_porters.md` BUG-Q06-001 记过一次，本条补 SEL 那份拷贝并说明是同一段代码抄了两遍）
- 备注: 修复方向是 `return "";` 并把 CTA/SEL/HFT 三族的字符串入口默认值口径统一。

## BUG-R13-004 create_sel_context 的 period 未识别时静默落 TPT_None 并照样返回句柄

- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtPorter/WtRtRunner.cpp:338-355`（`wt_stricmp` 五连比较，`else ptype = TPT_None;`）→ `src/WtCore/WtSelEngine.cpp:390-415`（`addContext` 不看 period，照建 TaskInfo、照进 `_ctx_map`）→ C 入口 `src/WtPorter/WtPorter.cpp:1135-1138`
- 现象: 导出层与 runner 都不校验 period 字符串；写成 `"d1"`/`"day"`/`"1d"`（很常见的把 K 线周期写法带过来）时 `ptype` 落 `TPT_None`，上下文照建、句柄照返回、`if (ctx == NULL)` 守卫全部放行，但选股任务永远不会被调度（`on_strategy_schedule` 不触发）。
- 影响范围: 失败模式是「策略静默不跑」，没有任何错误日志；`WtPorter.cpp:1135` 这条入口的可观测行为与它的参数命名（`period`）不匹配。CTA 侧 `cta_sub_bar_events`（`:1002`）是同一族「拼错不报错」。
- 相关测试: `r13_porter_sel_hft_export_test.cpp:R13PorterSelExport.create_sel_context_accepts_every_documented_period_and_returns_distinct_handles`（钉住现状：未识别周期仍返回可用句柄；若将来改为拒绝，该用例必须同步翻转断言）
- 备注: 修复方向是未识别 period 直接 `return 0` 并写 error 日志。

## BUG-R13-005 hft_buy/hft_sell 用函数级 static std::string 回传，hft_cancel_all 却是 thread_local

- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtPorter/WtPorter.cpp:2157-2181`（`hft_buy`：`static std::string ret;`）、`:2193-2217`（`hft_sell`：同）、对照 `:2121-2145`（`hft_cancel_all`：`static thread_local std::string ret;`）
- 现象: 三个函数都把 `std::stringstream` 的结果塞进一个函数级静态串再 `return ret.c_str()`。`hft_cancel_all` 用的是 thread_local（同一线程内下一次调用仍会覆盖，但至少没有跨线程数据竞态），`hft_buy/hft_sell` 用的是裸 static。多策略/多线程下，A 线程拿到 B 线程 `ret.c_str()` 的悬垂/被改写结果；C 调用方（Python/C#）通常在下一个导出调用之前不复制返回值，条件成立。
- 影响范围: 下单返回值（本地委托号串）可能被并发改写 → 撤单撤到别人的委托上；同一文件的线程安全口径不一致说明是改造时漏改。与 `R4` 那类「缓存项被掏空后静默丢数据」同属「返回值生命周期不成立」。
- 相关测试: 无启用用例（三个入口都要解引用未绑定 `_trader`，见 BUG-R13-001，已整条 `DISABLED_`）
- 备注: 本轮按「值得盯的一类缺陷形态」逐个核过 map 取值处：`WtCtaEngine::getContext`/`WtHftEngine::getContext`/`WtSelEngine::getContext`（WtCore/WtCtaEngine.cpp:139-146、WtHftEngine.cpp:353-360、WtSelEngine.cpp:418-425）都是 `return it->second;` 拷贝、没有 `std::move` 也没有 erase-after-move，**未发现 R4 同型缺陷**；`WtRtRunner::getCtaContext/getHftContext/getSelContext`（WtRtRunner.cpp:369-383）只是转发，同样干净。本条是返回值生命周期问题，不是 map 掏空。

## BUG-R13-006 导出层有 create 无 destroy，上下文永久累积在引擎的 _ctx_map 里

- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtPorter/WtPorter.cpp:404`（create_cta_context）、`:1135`（create_sel_context）、`:1652`（create_hft_context）；`src/WtPorter/WtRtRunner.cpp:314-355` 只有 `addContext`，没有 `delContext`/`closeContext`；`src/WtCore/WtCtaEngine.cpp:133-137` 的 `_ctx_map[sid] = ctx` 也没有删除入口
- 现象: PorterDefs/WtPorter.h 里没有任何销毁/关闭上下文的 C 入口，`WtSelEngine::addContext` 只防「同一 id 重复注册」（WtSelEngine.cpp:396-401），防不住「同一策略名重复 create」。宿主每 reload 一次策略就漏一个上下文（含其 `_pos_map`/`_kline_tags`/`_user_datas`），且同名上下文之间用户数据互不可见。
- 影响范围: 长时间运行的实盘进程内存单调增长；测试侧只能反复新建夹具上下文（本轮 3 个文件共新建 15 个上下文，全部留在单例里直到进程结束）。
- 相关测试: `r13_porter_cta_export_test.cpp:R13PorterCtaExport.userdata_is_per_context_not_shared_between_two_ctas`（顺带钉住「同名不同句柄是两个独立上下文」，没有互相覆盖）
- 备注: 修复方向是补 `destroy_cta_context` 一族的导出，或在 `release_porter` 里清空 `_ctx_map`。

## BUG-R13-007 WtPorter 导出族对 cfgfile / id / message 等 C 字符串参数一律 strlen 而不判空

- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtPorter/WtPorter.cpp:260-266`（`config_porter` 的 `strlen(cfgfile)`）、`:333-343`（`write_log` 的 `strlen(catName)`）、`:209-212`（`feed_adj_factors` 收 `WtString stdCode` 并按值构造 std::string）
- 现象: 这些入口对 NULL 参数没有任何守卫：`strlen(NULL)` 与 `std::string(NULL)` 都是 UB，而函数声明（`WtPorter.h`）里没有一个参数标了 non-null 约定。BUG-R6-004 已经为 `parser_*` 的 `id` 记过一次，本条把范围补齐到 config/feed/write 一族，方便一次评审收口。
- 影响范围: 外部绑定的空指针传参直接把宿主打死；错误分支（如 `config_porter` 的默认文件名支）在进程内取样时必须先绕过这个洞。
- 相关测试: 无启用用例（NULL 传参一律不进启用用例）
- 备注: 与 BUG-R6-004 同族，建议合并处理：在导出层加 `SAFESTR(x)` 宏统一兜底。
