# Q06 - WtPorter / WtBtPorter 实盘与回测 C 导出层候选项复核

> 本轮只复核记录，不修改生产代码或测试。以下证据来自源码与测试契约静态检查；本轮没有运行测试。
> “待确认”表示调用顺序、参数可空性或产品支持范围尚未由公开契约证明；测试断言当前行为不等同于期望行为。

## Q06-001 `cta_get_last_entertag` 的空句柄/无 mocker 分支返回 NULL

- 复核结论: 源码行为成立；是否为产品缺陷取决于 `WtString` 的空值契约和调用方是否允许 NULL。当前证据不支持“策略没有持仓就返回 NULL”。
- 迭代02第二轮前状态（历史）: - 状态: 待确认（接口空值契约）
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 待定
- 位置: `src/WtPorter/WtPorter.cpp:839-845`；`src/WtBtPorter/WtBtPorter.cpp:729-735`
- 源码证据: 实盘导出在 `getCtaContext(cHandle)` 未命中时 `return 0`；上下文存在时才调用 `stra_get_last_entertag`。该函数注释明确说“没有持仓记录返回空字符串”。回测导出忽略 `cHandle`，只在全局 `cta_mocker()` 为空时返回 `0`，因此回测这条路径是“没有 mocker”，不是“未知句柄”。
- 测试契约与边界: `Q06PorterCapi.CtaStringGettersUnknownHandle` 接受 NULL 或空串；`Q06BtPorterCapi.CtaEntriesRefuseWithoutMocker` 在无 mocker 条件下也接受 NULL 或空串。R35 原测试将未知句柄固定断言为 NULL，现改为允许 NULL 或有效空串，并继续要求有效句柄无记录时返回非 NULL 空串；修正后的 `R35PorterTrailGetters.CtaLastEnterTagRealHandleIsBlankUnknownAllowsNullOrEmpty` 本机 PASS。当前没有测试绑定层对 NULL 做 `strlen`/decode 后崩溃。
- 重复关系: SEL 同形导出已记在 `BUG-R13-003`，建议与 CTA occurrence 合并为同一空字符串 API 契约问题；`R35` 的真实句柄空标签测试覆盖另一个成功查找分支，不证明 unknown-handle 期望值。
- 修复方向: 先在 C 头文件明确该返回值是否可空。若返回值契约要求始终为有效字符串，错误分支统一返回静态 `""` 并将 CTA/SEL/回测测试固定为非空空串；若 NULL 是允许的错误表示，应在绑定文档中明确并要求调用方判空。

## Q06-002 实盘 C 导出在 engine 未就绪时直接解引用

- 复核结论: 消费入口与空指针条件由源码证实；实际崩溃没有运行证明，且“必须在成功 config 后调用”是否为正式 API 前置条件尚未找到清晰的 C 头契约。
- 迭代02第二轮前状态（历史）: - 状态: 待确认（初始化顺序契约）；源码风险明确
- 状态: 已合并（迭代02：主编号 BUG-R6-006）
- 严重级别: Major（若导出 API 承诺受控失败）
- 位置: `src/WtPorter/WtPorter.cpp:859/874/885/896/907/1217/1228/1239/1411/1423/1745/1757/1769/1781`；`src/WtPorter/WtRtRunner.cpp:361-366`
- 源码证据: 上述价格/日期/时间导出直接使用 `getRunner().getEngine()->...`；`WtRtRunner::getEngine()` 只是返回 `_engine`，未就绪时没有入口守卫。`get_raw_stdcode()` 也直接调用 `_engine->get_rawcode()`。函数静态 runner 的 `_engine` 在成功配置前为空；非静态 `WtRtRunner` 的 `_engine` 还涉及构造未初始化，属于不同前置条件。
- 测试契约与边界: `Q06PorterCapi.DISABLED_UninitializedEngineGettersCrash` 是空占位（主体只有 `(void)0`），不是崩溃复现。实盘各导出仅在错误调用顺序下危险，本轮未运行。回测同名入口使用 `getRunner().replayer()` 成员对象而非 nullable `WtEngine*`；这只说明两侧实现结构不同，不证明回测 getter 全族在任意状态下均安全。
- 重复关系: `R13` manifest 将这组 getter 与 `get_raw_stdcode` 标为 Q06-002 覆盖；`BUG-R6-003` 的 `createExtParser` 是相同未就绪状态下的另一个消费点，不是同一函数缺陷。`BUG-R46-001` 的非静态 runner `_engine` 构造问题也与静态 C API 在 config 前为空不是同一触发条件。
- 修复方向: 若未就绪调用需要可控失败，为导出入口集中增加 engine readiness 检查并返回其 ABI 可表达的中性值/错误；若调用方必须先初始化，则在头文件写明前置条件，并让初始化失败可被调用方识别。避免把 disabled 空占位描述为已复现崩溃。

## Q06-003 `WtBtRunner` 的 `_hft_mocker`（及 `_exec_mocker`）未默认初始化

- 复核结论: 构造遗漏由源码证实；静态单例的零初始化不能证明普通对象构造后为 NULL。另一个 R214 用例曾对普通对象调用 getter，即使丢弃结果也会读取未初始化指针；该不安全调用现已移除，不能当作已运行的 UB 复现。
- 状态: 已合并（BUG-R46-001）
- 严重级别: 条件性（仅对非静态 `WtBtRunner` 的相关读取路径）
- 位置: `src/WtBtPorter/WtBtRunner.cpp:47-88`；成员声明 `src/WtBtPorter/WtBtRunner.h:635-638`
- 源码证据: 初始化列表显式置空 `_cta_mocker/_sel_mocker`，未列 `_hft_mocker/_exec_mocker`。对自动/动态存储期对象，这两个裸指针没有确定初值；`initHftMocker()`、相关 HFT 导出和 `initExecMocker()` 读取这些成员时存在 UB。`getRunner()` 返回的函数静态对象经静态存储零初始化，不能代表普通构造对象的行为。
- 测试契约与边界: `Q06BtPorterCapi.HftEntriesRefuseWithoutMocker` 断言全局 `getRunner().hft_mocker()==nullptr`，依赖静态零初始化，只验证该单例现状。`R214BtRunnerInline.AccessorsAndExtLoaderFlags` 原对普通对象调用 `hft_mocker()` 并丢弃结果，现已移除该读取；重建后该单例 PASS，仅验证其余 getter 和 loader 状态，不验证 `_hft_mocker` 初始化。`_exec_mocker` 仍未被该回归验证。
- 重复关系: `BUG-R9-006` 是同一 `_hft_mocker/_exec_mocker` 构造字段遗漏；`BUG-R46-001` 已覆盖这两个字段及具体消费者。Q06-003、R9-006 不应重复计数。
- 修复方向: 对所有裸指针成员采用类内默认初始化或完整构造初始化列表，并按 R46-001 的消费者清单复核 `_worker/_loader_auto_trans` 等字段。测试要区分函数静态单例与显式构造对象，避免在未修复的 UB 前提上运行解引用操作。

## Q06-004 实盘 feeder 回调无守卫，`feedRawBars` 的 loader 守卫不匹配

- 复核结论: 源码证实两类问题；未注册/未 load 时触发的崩溃仅为静态条件，未在本轮复现。raw-only loader 被 final-loader 守卫挡住是可由代码路径推出的功能错误。
- 状态: 待审核（部分为调用顺序条件）
- 严重级别: Major（若 feeder API 允许在错误顺序下安全拒绝）；raw-only 路径为功能缺陷候选
- 位置: `src/WtPorter/WtRtRunner.cpp:203-272,277-292`；C 导出 `src/WtPorter/WtPorter.cpp:193-212`
- 源码证据: `loadRawHisBars` 把 `_feed_obj/_feeder_bars` 设为 raw feeder；`loadFinalHisBars` 设置同一槽位为 final feeder；`loadAdjFactors/loadAllAdjFactors` 才设置 `_feeder_fcts`。但 `feedAdjFactors` 无守卫直接调用 `_feeder_fcts`；`feedRawBars` 只检查 `_ext_fnl_bar_loader` 后调用 `_feeder_bars`。因此：未先执行对应 load 就 feed 会读空/未初始化的函数指针；raw-only 注册并通过 `loadRawHisBars` 设置 feeder 后，仍会因 final loader 为空而提前返回。
- 测试契约与边界: `Q06PorterRunner.FeedRawBarsUsesCachedFeeder` 覆盖“先 load final 再 feed”；loader-free 用例只证明 final-loader 未注册时的返回分支。R35 正向 feeder 用例也按“先 load 再 feed”顺序，包含 raw loader 场景但没有 raw-only 组合。`DISABLED_feed_adj_factors_without_feeder_jumps_to_garbage` 是栈 runner + 未初始化 callback 的禁用测试；它不能稳定证明会跳向垃圾地址。本轮未运行测试。
- 重复关系: feeder 未初始化与 `BUG-R6-009` / `BUG-R46-001` 的字段遗漏消费路径重复；R13 manifest 也将这些 feeder 调用列为 Q06-004 交叉覆盖。此处额外要保留的是 `feedRawBars` 只看 final loader 的条件错误。
- 修复方向: 以 `_feeder_bars/_feeder_fcts` 是否已设置作为调用安全守卫；`feedRawBars` 不应要求无关的 final loader 存在，或应分别维护 raw/final feeder 状态并检查实际来源。确认 `feed_*` 只能在 loader 回调内使用时，把此前置条件写进头文件，并让违规顺序返回错误而非调用未初始化函数指针。

## Q06-005 回测 Porter 导出忽略 `CtxHandler`，句柄是否应隔离待产品契约确认

- 复核结论: 导出函数从单例 mocker 取对象、忽略传入句柄是源码事实；“多策略场景不成立”尚未由支持多 mocker 的产品契约证明。
- 迭代02第二轮前状态（历史）: - 状态: 待确认（回测实例/句柄契约）
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 待定
- 位置: `src/WtBtPorter/WtBtPorter.cpp:589/729/913/1470/1759/1900` 等；`src/WtBtPorter/WtBtRunner.cpp:266-309,449-505`
- 源码证据: 回测导出通过 `getRunner().cta_mocker()/sel_mocker()/hft_mocker()` 访问每类的单个槽位，多个函数将参数写为 `CtxHandler /*cHandle*/`。`WtBtRunner::initCtaMocker/initHftMocker/initSelMocker` 会删除并替换对应槽位；配置路径一次只按 `env.mocker` 选择一种 mocker。`WtBtPorter.cpp` 明确将 runner 设计为单例。
- 测试契约与边界: Q06 回测 C API 用例大多在无 mocker 时传非零 `kAnyHandle`，断言中性返回值；它们没有创建两个 mocker 后比较句柄路由，也未运行真实回测。本轮没有发现公开文档承诺一个 runner 可同时运行多个同类策略的证据。
- 修复方向: 若设计为每类单一 active mocker，应在 API 文档说明 `CtxHandler` 仅供初始化返回/回调标识，并校验旧句柄以避免替换后的 stale handle 误操作新 mocker；若要求多实例隔离，应按 handle 管理 mocker，而非继续访问单槽位。

## Q06-006 `write_log` 对 `catName` 直接调用 `strlen`

- 复核结论: 传 NULL 时源码行为为未定义；C 头未找到允许 NULL 的声明，所以当前只能登记参数契约待确认，不据此推断 Python/其他绑定一定崩溃。
- 迭代02第二轮前状态（历史）: - 状态: 待确认（`catName` nullability）
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 待定
- 位置: `src/WtPorter/WtPorter.cpp:333-343`；`src/WtBtPorter/WtBtPorter.cpp:285-295`
- 源码证据: 两侧均先执行 `strlen(catName)`，NULL 会进入未定义行为。函数内只有 `catName` 被直接 `strlen`；`message` 是传给 logger 的另一参数，本条没有证明其 NULL 后果，故不把它纳入同一结论。
- 测试契约与边界: `*.WriteLogAcceptsNamedAndEmptyCategory` 仅覆盖空串和非空字符串；没有 NULL 用例。本轮未运行 NULL 调用。
- 重复关系: 实盘 occurrence 已在 `BUG-R13-007` 中交叉引用；回测字符串边界亦列于 `BUG-R83-001` 的交叉引用。相同导出行不重复计数。
- 修复方向: 明确分类名是否可空；若可空，将 NULL 与空串映射到默认分类；若必填，入口拒绝 NULL 并安全记录错误。不要未经逐参审查把 `message`、其他字符串都推导为同一缺陷。

## Q06-007 回测 C 头缺少 HFT 通道事件常量，READY 回调使用硬编码值

- 复核结论: 常量缺失和回调值 1000 由源码证实；运行值与实盘 `CHNL_EVENT_READY` 相同，当前更像 API 可用性/一致性缺口，未证明运行时事件错误。
- 状态: 待审核（API 常量完整性）
- 严重级别: Low
- 位置: `src/WtBtPorter/WtBtRunner.cpp:413-417`；`src/WtBtPorter/PorterDefs.h:35-41`；对照 `src/WtPorter/PorterDefs.h:45-46`
- 源码证据: 回测 `hft_on_channel_ready` 把 `1000` 传给回调并注释为 `CHNL_EVENT_READY`；回测头未定义该符号，实盘头定义 READY=1000、LOST=1001。当前回测实现只展示 ready 事件，未找到 backtest lost 的产生路径。
- 测试契约与边界: `Q06BtRunner.HftMicroAndTradeEventsPassThrough` 断言字面量 `1000U`，能固定实现值但不能验证绑定层可用公共符号；本轮未运行该测试。
- 修复方向: 在共享 Porter 定义中统一导出 READY 常量；只有在回测确实支持 LOST 时再公开对应事件，不因实盘存在该常量就推定回测会发出它。
