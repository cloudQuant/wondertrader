# BUG-I02-NEW-015：VWAP 初始化失败后的宿主收口与 V2 借用上下文

## 状态

候选 `PRODUCT_BUG`，`IN_PROGRESS`，待统一加入执行清单。编号 `BUG-I02-NEW-015` 在本轮创建前未被文档或清单占用。当前跟踪快照按已有 480 条清单计数；本候选尚未入册，不计入该统计。

## 复现证据

本机集成运行记录：[v3 运行日志](/tmp/wt-vwap-v2-local-v3-run.log)。夹具加载真实内置 `WtExeFact` V2 插件，经 `WtExecuterFactory` 和 `WtLocalExecuter` 建立执行单元；行情/基础数据接口为测试 stub。

- `MatchingDynamicProfilesRegisterFutureAndStockUnits` 通过：期货通用 VWAP 与股票 VWAP 的 profile 和各自实际 session 匹配，单元报告 `READY` 并出现在 unit registry。
- `SessionMismatchedProfileFailsAndDoesNotRegisterUnit` 中，初始化状态失败、unit 未注册和 mismatch 诊断断言到达；随后 fixture teardown 的 staged quiescence 断言失败，`quiesce_all()` 返回 false，fixture 中止进程。
- v3 日志因此不是整批 PASS：首个匹配用例通过，随后 mismatch 用例因 teardown 失败终止；后续 mismatch/坏配置场景未执行。

用户已明确无效执行单元必须明确报告失败且不得注册。session mismatch 路径中的 `FAILED` 状态和不注册符合该决定；这不替代宿主退出安全性的验收。

## 根因与问题边界

1. **宿主把已进入插件代码的失败初始化提升为全局 sticky fail-stop。** v3 失败时的 `WtLocalExecuter` 旧逻辑对 `externalCodeEntered` 的失败初始化路径无条件设置 `_quiesce_incomplete`。于是插件 V2 回调明确返回配置失败、执行单元没有登记的情况下，`quiesce_all()` 仍无法完成。应该依照失败回调是否正常返回、V2 合同是否能证明初始化副作用已清理、是否存在已接受回调/结果/未收口 owner 来判定能否安全收口；无法证明安全时仍须 fail-stop 保留 owner，不能一概把可证明清理的失败视为运行期不完整。
2. **内置 VWAP 单元违反失败返回时的 V2 no-borrow 约定。** `ExecuteUnit::init(ctx, ...)` 会将宿主借用的 `ExecuteContext*` 保存到基类 `_ctx`。通用/股票 VWAP 单元在后续发现 session/profile 或配置无效时返回初始化失败，但未在失败出口清空该 borrowed context。V2 初始化失败合同要求插件返回前释放/清除所有宿主借用，且失败单元不能继续被调用。仅让 wrapper 删除对象或让宿主忽略该指针，不能算插件合同已满足。

这两项相互关联但修复责任不同：宿主应只在存在未收口副作用时转入 sticky fail-stop；内置插件应在每一条失败出口清除 borrowed context，并同步释放或恢复其初始化期间取得的资源。两项都在修复中，当前没有修复后验收证据。

## 修复与验收方案

1. 对内置通用和股票 VWAP 的失败初始化，保证返回 V2 非零状态前不保留 `_ctx`、`stdCode` 或其他宿主借用；资源释放路径可重复调用且不会触发额外回调。
2. 对真实 `WtExeFact` V2 与 `WtLocalExecuter` 集成的失败路径，验证失败状态和诊断可查询、unit 不在 registry、没有策略或行情回调触达该 unit，且无遗留 callback/owner 时 `quiesce_all()` 正常完成。
3. 区分有清理证明的配置失败和异常/副作用不确定失败：前者不污染全局 quiesce 状态；后者保持 fail-closed 并保留相关 owner，禁止通过“让测试 teardown 成功”掩盖未清理的异步工作。
4. 覆盖通用 VWAP、StockVWAP 的匹配与 session mismatch，以及至少一种无效配置；整组测试必须运行到结束、无 abort，并有真实插件 artifact 和本轮编译证据。
5. 独立复审确认 no-borrow 和宿主清理门禁没有被削弱；记录检查到的实际 plugin API、registry、状态查询和 quiesce 链路。

## 未验证边界

当前仅有本机 fake 市场/数据接口的局部集成尝试。真实柜台/provider、并发初始化和销毁、真实行情回调与回报交错、跨平台构建及 sanitizer 均未验证。v3 失败日志应保留为修复前证据；不得据其中单个匹配用例 PASS 宣称 VWAP V2 整链通过。
