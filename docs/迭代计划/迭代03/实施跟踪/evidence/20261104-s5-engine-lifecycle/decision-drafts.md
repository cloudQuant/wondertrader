# engine-lifecycle 包：评估结论与决策卡草稿（供主线程整合，非裁定）

## A. BUG-R10-004（D）：SEL 同 context 的 schedule/tick 顺序与重入

**R202-001 EventStrand 覆盖评估（静态分析）**：`WtSelRtTicker` 已用 `wt_ticker::EventStrand` 把 `on_tick`（`handle_tick`）与 worker（`worker_pass`）串行化，二者调用的 `WtSelEngine::on_minute_end` 也在 strand 内。但 `WtSelEngine::on_minute_end`（`WtSelEngine.cpp` 约 347 行）对每个触发任务 `StdThreadPtr thrd(new StdThread(...ctx->on_schedule...))`，线程逃出 strand：

1. `on_schedule` 与后续 tick 对同一无锁 `SelStraBaseCtx` 并发，数据竞争仍在（strand 未覆盖）。
2. `StdThread` 即 `std::thread`，局部 `shared_ptr` 在循环迭代结束析构一个仍 joinable 的线程，按标准调用 `std::terminate`——**任何一次任务触发都会终止进程**（既有测试刻意在节假日早退分支前返回以避开）。

3. 同处 `getContext(tInfo->_id)` 用的是 `makeTaskId()` 生成的任务序号，而 `_ctx_map` 以 context id 为键（`addContext` 中 `_tasks[ctx->id()]`/`_ctx_map[sid]`），两套序列不同，即使不 terminate 也可能取到错误或空 context。既有 `q01_wtcore3/r211_wtcore_rest_test.cpp` 的 `R211SelEngine.DISABLED_ExpiredTaskTerminatesBeforeSchedule` 已固化 1 与 3（DISABLED，未运行）。

**2026-11-04 追加**：terminate（1）与 context 查找（3）已按 I03-NEW-066 最小修复（派发线程由引擎持有、`release()` join、按 context id 查找、线程体捕获异常），未改变执行位置；数据竞争（2）仍待本卡裁定。

**要决定什么**：SEL 任务回调的执行位置。

- **A（建议）**：在 ticker strand 内同步执行 `ctx->on_schedule`（即“全局/每引擎 strand”）。与 2026-11-04 用户对 ticker 的 9d=A 裁定一致：同步重入由 EventStrand 排队到安全点；stop 先排空再 join；消除 terminate 与 schedule/tick 竞争。代价：某策略长时间 `on_schedule` 会推迟同引擎其他 SEL 策略的 tick 处理（SEL 为低频选股，影响可接受）；回调异常需在边界捕获并记录、该策略标记失败后不再调度。
- **B**：per-context strand（每 context 一个串行队列/线程）。保留跨策略并行；须另定义 A→B 同步跨 context 重入的排队规则以防死锁，并管理每 context 线程的 stop/join。
- **C**：per-context 状态锁。改动最小但同步重入（回调内触发 tick/schedule）会自锁或需递归锁，难以证明无死锁。

**影响**：BUG-R10-004、BUG-I02-NEW-155（同步前提）。**裁定后**：实现 A 时新增确定性用例（同 context schedule/tick 交错、至多一次、异常隔离、stop/析构、直接 `WtSelEngine::run` 重复调用）与 TSan。

## B. BUG-I02-NEW-155（D）：SEL pending portfolio signal 合并与换月

**复现路径（静态分析）**：`WtSelEngine::handle_pos_change` 以 `pInfo->_volume + diffQty` 计算目标；无有效价格时 `WtEngine::append_signal` 只写 `_sig_map[code]._volume`（待触发目标），不改 `_pos_map`。同一合约两个策略在价格到达前先后 +2、+3：第二次以 0+3 覆盖待触发的 2，最终 3 而非 5。`WtCtaEngine::handle_pos_change`（约 484 行）同型。

- **B1 合并（建议）**：存在待触发目标时以其为基准累加：`target = (pending ? pending._volume : pInfo->_volume) + diff`。依赖卡 A 选 A（所有调用都在 ticker strand 内，单线程，无需额外锁）；若选 B/C 须对 `_sig_map/_pos_map` 加同一把锁。
- **B2 换月**：待触发目标按物理合约（`realCode`）保存，换月后新增量映射到新合约、旧合约待触发目标在其价格到达时照常执行（现状，建议保留，不迁移）；或换月时撤销旧合约待触发目标并把增量迁到新合约（需定义经济语义）。

**裁定后**：实现合并与（若选）迁移规则，补无价连续增量、价格恢复、HOT 映射与换月、schedule/tick 交错用例。

## C. BUG-R211-001（Z，建议“用户指示忽略”剩余子项）

z15 已修“空监控器崩溃”子项（`init_riskmon` 对空工厂/空监控器返回 false）。剩余：`active=true` 的风控插件加载失败时引擎/runner 应拒绝启动还是降级运行，仓库内无已裁定合同（卡8c 只覆盖执行器风控未知代码）。可选：A 拒绝启动（与 `fee_config_valid()` 同模式：WtEngine 增加 `riskmon_config_valid()`，三引擎 `run()` 与 `WtRunner::initEngine`/`WtRtRunner::initEngine` 检查）；B 降级并告警（现状）。建议 A，但这是产品选择；按卡15 记“用户指示忽略”，解除条件：用户选定 A/B。若选 A，本包可改 WtEngine/三引擎，runner 两处各需 3 行（非本包）。

## D. BUG-R160-002（D/X）：MQ confirm 配置语义

`MQServer` 的 `confirm=true` 含义是“没有订阅连接时暂存消息，不发送”（`MQServer.cpp` 约 138 行），不是投递确认；暂存队列无上界。宿主三处 EventNotifier 均固定传 false。

- **D1（建议）**：保持固定 false，明确“不支持 confirm 配置”（关闭配置语义子项），清单中“confirm=true 可观察”验收条件改为“固定 false 可观察”。
- **D2**：新增可选 `confirm` 配置键（默认 false），选 true 时必须同时定义暂存上界与溢出策略。

Windows/MSVC 签名一致性仍为 X（需 Windows 构建）。

## E. BUG-R41-003（D）：EventNotifier 有界停止与隔离

剩余：publisher 持续阻塞时析构 join 无界；自线程析构；重复 init。静态结论：worker 上只运行宿主内部闭包与 MQ 插件 `publish_message`，宿主未把 notifier 指针交给插件，`on_mq_log` 为空函数，**自线程析构经现有 ABI 不可达**。需决定：

- **E1（建议，符合 9a/9b）**：worker 共享状态改为堆上共享对象；析构先停止接收、释放 work guard、在有界时间（建议配置 `stop_timeout_ms`，默认 5000）内等待排空；超时则记录错误、分离线程、保留共享状态与 MQ server（不调用 `destroy_server`），隔离到进程退出。
- **E2**：保持无界 join（现状）。

另建议（无需裁定，但会改动 R10-005 用例细节，故本轮未做）：按 9c=A 在已初始化时直接拒绝重复 `init`；`create_server` 返回 0 时 `init` 返回 false 且不启动 worker。

## F. BUG-P01-004（D）：历史 schema 与发布后订阅失败

- 发布后订阅（`CtaStraBaseCtx::load_data` 末尾逐个 `stra_sub_ticks`）：`WtEngine::sub_tick` 为 void、只写本地订阅表、不回调策略，失败只可能是 `std::bad_alloc`，不存在外部重入路径。建议合同：恢复期间内存分配失败视为该 context 致命错误（向上传播，不再部分运行），无需回滚已发布状态。需用户确认。
- 历史 schema：现行文件无版本号且有历史缺省兼容，“明确支持的旧版本”须由用户给出（候选：仅当前形状 + 已知缺字段兼容；未知形状整体拒绝）。
- 两次早期 ASan container-overflow（静态分析）：第一份报告读路径为 `basic_string[abi:nqe220108]`（LLVM 22 libc++ 头）而同一 vector 的分配/扩容路径为 `[abi:ne200100]`（另一版本 libc++ 头）；当时构建只插桩 3 个 TU、其余对象与预编 gtest/系统 libc++ 未插桩且混用工具链。这正是 AddressSanitizer 文档所述 container-overflow 误报类别（同一容器被未插桩/不同注释配置的代码修改）。“同版本依赖独立重建”需要用同一 LLVM 版本的已插桩 libc++、gtest 与全部依赖重建，本机 Homebrew LLVM 未提供 ASan 插桩的 libc++，仍为 X/阻塞。

## G. I03-NEW-008：实盘是否应用费用模板的 `minfee`

现状：回测 `HisDataReplayer`/`wt_fee::calculate` 按笔应用最低手续费；实盘 `WtEngine` 历来只校验 `minfee`、不应用（旧注释“This engine does not apply it”，`R130EngineFee` 旧断言固化）。本轮接入 FeeSchedule 时保持实盘不应用。选项：A 实盘同样应用（实盘/回测一致，已有部署费用会变化）；B 保持不应用（现状）。建议 A，但属行为变化需用户确认；确认后只需在 `calc_fee_checked` 中传入 `_min_fee` 并更新 R130 旧断言。

## H. I03-NEW-008：实盘 CTA/SEL 理论成交在缺费时的处置

本轮实现：实盘 CTA/SEL context 与引擎组合账本在缺费时**保留持仓变化**（策略目标已驱动执行器真实下单，丢弃会使策略与执行侧账本分叉），费用记 0 并经 `fee_error_count()/last_fee_error()` 与每代码首次 error 日志可观察。回测侧（`CtaMocker/SelMocker`）选择的是“费用预检失败保留信号、不应用”。若用户要求实盘也在缺费时阻止信号（fail-closed），需另定执行器侧语义。
