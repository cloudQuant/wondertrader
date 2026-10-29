# 09_uft 静态复核记录

范围：本轮输入 `groups/09_uft.json` 的五个 key。按审核说明重新检查了当前实现、公开接口/注释、正常 runner/API 调用者和相关测试。未复用旧裁定的结论。

## R11-008

- `ITraderApi::onLogout` 标注为注销回报，`logout()` 是对应请求。
- `TraderATP::OnRspCustLogoutResp` 成功分支在 callback 前设 `TS_NOTLOGIN`；UFT adapter 的 callback 为空。
- UFT 的 `state()` 可观察，`onRspPosition` 还会按 `AS_LOGINED` 推进状态机，因此只裁定 adapter 状态没有清回未登录。
- `ITrdNotifySink::on_channel_lost` 说明的是连接断开或异常；没有推导成功注销必须调用它。
- 共享树中的 `q04_trader_reply_machine_test` 已将登出后的状态期望改为 `AS_NOTLOGIN`。本轮没有运行。

## R175-003

- UFT `initExt` 直接对 api 调用 `init(NULL)`；`init()` 的 null 检查是 `params`，与 api 指针不同。
- `WtUftRunner::initTraders` 使用 `init()`；`init()` 从插件工厂生成 API 并检查空返回。
- 仓库内 `UftLatencyTool`、`HftLatencyTool` 的 `initExt` 调用先用 `new TestTrader()` 建立非空 API。
- 当前 disabled `r163` 和 q04 entrust 用例以 nullptr 作为前置，缺少该方法可空的契约。本轮未运行禁用用例。

## R83-002

- `hft_buy`、`hft_sell` 各写自己的函数级静态 `std::string`；`hft_cancel_all` 使用 `thread_local`。
- `WtBtPorter` 公开头只写返回订单 ID 字符串，`WtString` 是 `const char*`，没有并发/所有权/失效时点说明。
- 回测 runner 公开异步模式并在线程中调用 replayer；HFT strategy callback 由回放路径触发。
- 现有非空测试按顺序立即读/复制每个结果，空字符串测试也只即时 `strlen`。它们不证明多线程调用或保留指针后的契约。
- 需要维护者给出线程和生命周期规则；不需要外部 SDK。

## P06-003

- 生产 UFT runner 使用 `WTSBaseDataMgr`。
- `loadContracts()` 显式商品查询失败时 `continue`；`rules` 分支创建商品；后续先 `setCommInfo(commInfo)` 再 append 到 `_code_list` / `_fullcode_map`。
- 候选测试用 `P06BaseDataMgr::reg()` 直接登记 `WTSContractInfo::create()` 结果，没有建立关联。该 fixture 形状不来自当前生产 loader。
- 主代理已将未关联对象分支从 disabled 混合崩溃类移除；本轮未运行该类。

## P06-006

- `WtUftRunner::config` 加载基础 session 文件后不验证 `loadSessions()` 结果；`initEngine()` 把 `_config.env` 传给 UFT engine，最后 `config()` 返回 true。
- `WtUftEngine::run` 从 `env.product.session` 调用 ticker `init()`；未知 session 可令 `_s_info == NULL`。
- `WtUftRtTicker::run` 在策略初始化后通过 `_s_info->id()` 计算交易日，空 session 会在 worker 创建前被解引用。
- 当前 active P06 ticker 用例停留在 `_thrd == NULL` 路径；R123 正向用例使用存在的 session。两者都没有未知 session 的受控失败断言。

## 补充：首笔与分钟跳跃

- UFT 首次设置 `_cur_pos` 时不调用 `engine->on_tick`。CTA/SEL/HFT ticker 存在相同 warm-up 分支。
- R123 test 明确把首笔作为分钟定位数据并期待自动闭合前无 tick；R78 test 则在 `stop()` 后发首笔 tick，不能单独代表正常活动线程。
- 当前实现只在分钟位置前进时关闭 `_cur_pos` 一次。未找到要求合成缺失中间分钟空 bar 的接口契约。

## 执行记录

无本轮测试或运行探针。所有五项 `evidence_level` 为 `STATIC`；没有生成可称为 `LOCAL_REPRO` 的运行记录。
