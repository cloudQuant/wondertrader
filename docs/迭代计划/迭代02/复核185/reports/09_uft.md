# 09_uft 第二轮复核

本轮按 `groups/09_uft.json` 复核了 5 个输入项，逐项回看实现、接口、正常调用链和测试前置。只做静态核对，未运行本组 GTest；因此以下证据均为 `STATIC`，不把主代理的构建成功当成本组运行验收。

## 分类统计

| 决定 | 数量 | 编号 |
|---|---:|---|
| PRODUCT | 2 | BUG-R11-008、BUG-P06-006 |
| TEST | 2 | BUG-R175-003、BUG-P06-003 |
| NEEDS_DECISION | 1 | BUG-R83-002 |
| 合计 | 5 | |

## 逐项结论

### BUG-R11-008 — PRODUCT / STATIC

`ITraderApi::onLogout` 是注销回报，接口同时提供 `logout()`；仓库内的真实 API `TraderATP::OnRspCustLogoutResp` 只在注销成功时先把自己的状态改为 `TS_NOTLOGIN`，再回调 `onLogout`。UFT `TraderAdapter::onLogout()` 为空，`TraderAdapter::state()` 因而继续暴露 `AS_LOGINED` 或后续就绪状态。`onRspPosition()` 仍以 `AS_LOGINED` 作为推进订单查询的门槛，登出后的迟到持仓回报可能继续推进旧登录状态。

裁定只落在适配器状态未复位；`ITrdNotifySink::on_channel_lost` 的公开说明限定连接断开或异常，不能仅凭成功注销要求发出该回调。主代理已把 `q04_trader_reply.login_result_drives_state_and_position_query` 的期望改为 `AS_NOTLOGIN`；该用例还应在注销后注入 `onRspPosition(nullptr)`，断言状态不再推进且 `queryOrders` 计数不增。本轮没有运行该用例，生产代码修复和回归执行仍待完成。

建议在 `onLogout()` 把状态复位到 `AS_NOTLOGIN`。验收需覆盖成功登录、成功注销、注销后迟到的持仓回报不再推进订单查询状态；是否通知 `on_channel_lost` 不属于本项验收。

### BUG-R175-003 — TEST / STATIC

UFT `TraderAdapter::initExt` 确实会对 `api` 直接调用 `api->init(NULL)`，所以手工传 `nullptr` 会崩溃。但该方法没有声明 `api` 可空；`ITraderApi` 是初始化所需依赖。正常 runner 用 `init()` 通过插件工厂创建 API，并在工厂返回空时拒绝继续；仓库内两个 `initExt` 正常调用者均先创建 `TestTrader` 再传入。`WtCore::TraderAdapter::initExt` 多一个空指针检查，只能说明实现防御差异，不能单独建立 UFT 的 nullable 契约。

因此当前测试的“NULL api 必须返回 false”主张没有有效接口前置。两个禁用用例 `R163UftTrader.DISABLED_InitExtWithNullApiCrashesInsteadOfReturningFalse` 和 `DISABLED_q04_trader_entrust.bare_code_and_null_api_class` 仍含该主张/空指针分支；建议移出产品缺陷证据或改标为违反依赖前置的探针，不要启用。无需仅为此改产品实现；若未来决定 `initExt` 承诺接受空值，再单独写明返回值契约并添加判空实现。

### BUG-R83-002 — NEEDS_DECISION / STATIC

`WtBtPorter::hft_buy` 与 `hft_sell` 各自返回函数级 `static std::string` 的 `c_str()`；相邻 `hft_cancel_all` 使用 `thread_local`。公开头文件只说明返回订单 ID 字符串，`WtString` 只是 `const char*`，没有说明并发调用规则、指针所有权或失效时间。`run_backtest` 明确支持异步回放，策略回调可运行在 worker 线程；但当前仓库测试顺序读取或立即复制每次结果，没有建立跨线程调用或长期保留返回指针的契约。

需要维护者确认：WtBtPorter HFT 导出是否只允许串行策略回调线程调用，以及返回指针在何时失效。建议保持 ABI 返回类型不变，明确结果只在同一线程下一次调用同一导出前有效，并把 `hft_buy/sell` 缓冲改为 `thread_local`；若调用方必须跨调用长期保留，应另增调用方缓冲或显式所有权 API。选择后再按承诺增加并发/生命周期测试。

### BUG-P06-003 — TEST / STATIC

`WtUftEngine::get_session_info(stdCode, true)` 在找到合约后会解引用 `cInfo->getCommInfo()`。但当前生产 runner 使用 `WTSBaseDataMgr`：合同配置显式指定的商品未找到时会跳过该合约；`rules` 路径创建商品对象并设置到合约上，随后才把合约追加到代码列表。因此正常 loader 可查询到的合约都经过 `setCommInfo`。候选触发条件来自测试假管理器的 `reg()`：它把直接 `WTSContractInfo::create()` 的对象放进 map，却没有建立商品关联，违反了本轮查到的生产装载形状。

这不证明任意自定义 `IBaseDataMgr` 实现都安全；结论仅是现有产品运行路径不能由该夹具触发。主代理已从 disabled 混合崩溃类中移除未关联合约分支并加注说明。无需为该无效夹具新增产品修复；若将来声明支持返回未关联合约的自定义 manager，应另定义该扩展契约并单独验收。

### BUG-P06-006 — PRODUCT / STATIC

`WtUftRunner::config()` 从 `basefiles.session` 加载 session，但不检查加载结果；随后把 `env` 传给 `WtUftEngine::init()` 并最终返回成功。引擎运行时从 `env.product.session` 取会话名并调用 ticker `init()`；`init()` 允许 `getSession()` 返回空指针而继续。`run()` 随后先调用策略 `on_init()`，再在 `calcTradingDate(_s_info->id(), ...)` 解引用空 session，尚未启动线程就崩溃。缺少/拼错的 session 是外部配置进入正常 runner 路径的输入，不能因其配置错误而把未受控崩溃归为有效依赖缺失。

修复应在启动策略和 ticker 线程前验证实际选中的 session；把 `init`/`run` 的失败状态传回 `WtUftRunner`，使无效配置被拒绝，且不得继续启动 parsers/traders。建议增加有效引擎与未知 `env.product.session` 的受控失败用例，断言没有 `on_init`、`on_session_begin` 或 ticker 线程启动；同时保留已有有效 session 正向流程。当前 `p06_uft_ticker.init_and_forward_before_thread_start` 只测 `_thrd == NULL` 的转发分支，`null_engine_guard_on_forward` 是不完整装配，均不能验收这个正常 runner 路径。

## 补充观察：首笔 tick 与分钟跳跃（不增加本组条目）

UFT ticker 在 `_cur_pos == 0` 时只记录首分钟，不调用 `engine->on_tick`；但相同的 warm-up 规则也存在于 CTA、SEL、HFT ticker。`R123UftTicker.auto_close_after_check_time` 的注释和断言明确把首笔 tick 作为分钟定位数据，并断言自动闭合前引擎还没有该 tick。`R78UftTicker.run_stop_and_full_tick_flow` 则在 `ticker.stop()` 后才发送未来时间的首笔 tick，属于停止后的非正常生命周期；不能把它单独当成首笔漏转发的产品契约。若要改变首笔语义，需先确认跨引擎的公开/产品期望。

时间位置前进时，UFT ticker 只关闭上次观察到的 `_cur_pos` 一次，不合成中间空分钟。当前接口和有效调用链未声明必须为没有行情的分钟生成空 bar，所以本轮不把“多分钟跳跃未补齐空分钟”计作产品缺陷。

## 执行边界

本轮没有执行测试、并发探针、崩溃探针或 sanitizer。禁用用例没有被启用；没有 `LOCAL_REPRO` 证据。详细静态核对记录见 [evidence/09_uft/static-inspection.md](../evidence/09_uft/static-inspection.md)。
