# BUG-I02-NEW-008：Diff 执行器回报缺少 owner 归属

## 裁定与证据范围

- 裁定：`PRODUCT_BUG`，状态 `IN_PROGRESS`。
- 已审查的静态源码链路确认存在跨执行器误归属路径；以下源码链路描述问题的原始触发路径，journal v4 的局部测试并未验证它已在真实交易链路中消除。
- 最新本机 journal v4 定向 suite 为 12/12 PASS，见 `/tmp/wt-diff-journal-v4-run.log`；该 suite 覆盖持久化、重放、失败关闭和并发 intent 等 journal 行为，不是实际下单/回报/查询整链验收。
- 独立复审无法验证历史 `floor` 的来源真实性；当前没有接入真实下单、成交/委托回报或账户查询。因此用户确认的分账语义尚未证明已在产品链路实现，本项继续为 `IN_PROGRESS`。

## 源码链路

1. `WtRunner` 为每个 `diff` executor 建立 `WtDiffExecuter`，设置 trader 后收集到 `pendingTraderBindings`，最后逐个调用同一 adapter 的 `addSink`（`src/WtRunner/WtRunner.cpp:617-661`）。因此一个 adapter 可同时拥有多个 Diff sink。
2. `TraderAdapter::forEachSink` 快照当前全部 sink 并逐个调用（`src/WtCore/TraderAdapter.cpp:289-315`）。订单状态、成交和委托失败处理均通过它广播（分别见 `:2611-2614`、`:2789-2802`、`:1963-1971`）。
3. 发单时 usertag 由 adapter 的 `_order_pattern` 和 adapter-local `localid` 拼成；adapter 前缀是 `otp.<trader-id>`，完整 tag 类似 `otp.<trader-id>.<localid>`，没有 executor 身份（`src/WtCore/TraderAdapter.cpp:491`、`:961-978`）。回调解析 localid 后仍向全部 sink 发回报。
4. `WtDiffExecuter::on_trade` 检查参数后按 `stdCode` 调用 `ownsCode`，随后执行 `curDiff = prevDiff - vol * ...` 并更新该实例自己的 `_diff_pos`；它不通过 localid 找到发单 executor，也没有 owner 字段（`src/WtCore/WtDiffExecuter.cpp:1714-1751`）。订单和委托回报同样按代码账本 `ownsCode(code)` 过滤后交给该实例的 unit（`:1790-1819`、`:1830-1860`）。
5. `ownsCode` 在该 executor 的目标/差量账本或 unit 生命周期表中找到该 code 即返回 true（`:724-742`）。因此，只要两个 executor 都已建立同一 code 的账本，一个 adapter 发出的同一条回报会被两个 sink 接受；单笔成交会分别改变两份差量账本，订单/委托回报也会进入多个 executor 的通知链。
6. 动态目标存在多执行器入口：`WtExecuterMgr` 的路由缺省包含 `ALL`（`src/WtCore/WtExecMgr.h:61-74`），`handle_pos_change` 对 `ALL` 广播给未显式路由的 executor（`src/WtCore/WtExecMgr.cpp:261-299`）；缓存目标提交会在找不到 executor 专属目标时回退到 `ALL`，再调用每个 executor 的 `set_position`（`:317-321`、`:324-396`）。只在启动配置中静态检查 executor，不能覆盖运行时新建账本的路径。

## 触发条件与根因

触发条件：同一个 `TraderAdapter` 绑定两个或更多 `WtDiffExecuter`，并且同一个标准合约代码进入多个 executor 的目标/差量/unit 账本；其中一个 executor 下单后 adapter 收到订单、成交或委托失败回报。

根因是回报路由键只包含 adapter-local localid 和合约代码，缺少执行器 owner 身份。adapter 对所有 sink 广播，而 Diff sink 通过“本地是否持有该 code”猜测归属。多个 executor 合法地持有同一个 code 时，这种过滤不足以区分订单所有者。ALL 与动态缓存目标进一步扩大了触发面。

## 最新运行与复审

- `/tmp/wt-diff-journal-v4-run.log` 显示 `DiffOrderOwnershipJournalV1` 12/12 PASS，覆盖 owner ledger/active orders 重启持久化、目标 baseline 与 trade replay、local order 高水位和未决 intent、legacy local-id floor 检查、拒单/部分撤单进度幂等、终态后的迟到成交、未知订单和冲突 trade key fail closed、损坏 WAL、写入/flush 失败及并发 intent 串行化。
- 这些结果只证明本机定向 journal harness。独立复审无法证明被使用的历史 `floor` 来自可信、对应账户与合约且时间点明确的历史持仓/订单来源；本次也未连接实际下单、provider 回报或账户查询。不能据此宣称按发单 executor 分账已实现。
- `/tmp/wt-diff-owner-route-run.log` 的 `DiffOwnerSinkRegistry` 为 3/3 PASS，覆盖默认关闭、按 owner 路由、并发移除等待及回调重入移除。但测试通过 `#define private public` 直接调用 `forEachSinkForOwner` 内部辅助方法；生产订单/成交/拒单回调没有调用该路由的接线，生产回报路径仍未得到验证。
- capability 目前由调用方传入 `OwnerRoutingCapabilities` 多个 bool；测试 helper 手动将这些值全部设为 true。这些 bool 不是从真实 provider、插件 ABI 协商或可核验的生产证据中取得，不能证明 provider usertag 往返、稳定成交 ID 或持久化 floor 能力。
- 独立复审后，owner sink 注册在更新 owner map/classified sink 集合时增加了异常回滚，避免注册过程异常留下部分状态。3/3 suite 没有分配失败注入，因此只记录代码修正，异常回滚仍需定向验证。
- 完整 owner 归属仍需把 registry 接到生产 callback、取得可验证 capability、稳定成交键、所有账本变更入口的 owner 门禁、查询结果补账及订单终态/迟到回报映射。清单状态维持 `IN_PROGRESS`，不据 12/12 或 3/3 调整计数或验收状态。

## 用户已确认的业务合同

用户确认：同一账户、同一合约允许多个执行器共享；每笔成交归实际发出对应委托的执行器，并由各执行器分别记账。该决策解决目标共享语义，不证明现有广播回报已正确归属，也不代表已实现或验收。

## 修复建议

### 保持共享并修复 owner 路由

- 不得只因同一 adapter/code 已被另一 Diff 执行器使用就拒绝绑定或目标；保留用户确认的同账户同合约共享能力。
- 为每笔委托保留可验证且可恢复的发单 owner 关联；成交使用 provider 稳定标识及必要命名域组成的 stable trade key，journal 对重复回报/查询重放幂等，对冲突 key fail closed。不得仅凭合约代码或 adapter-local localid 猜 owner。
- owner-routing capability 必须来自可核验的版本化插件/provider 合同或运行时协商，不接受调用方任意设置 bool 作为生产能力证明；缺少任何能力时保持 fail closed。
- 对所有账本变更入口实施同一 owner 校验，包括 direct callback、manager fanout、显式 route、`ALL`、缓存目标、查询回填和恢复路径。unknown、missing、malformed 或歧义 owner 时不更新任何账本、不回退广播，并输出可诊断错误。
- 查询回填须记录可信数据来源、账户/合约和 as-of 边界；证明与 live callback 并发、乱序和重放不会重复计账或覆盖较新的状态。独立复审提出的历史 `floor` 来源真实性问题须由有来源证据的历史持仓/订单快照解决，不能只证明 journal 中保存了一个 floor 值。
- 明确定义委托状态的终态映射，以及部分成交、撤单/拒单终态后迟到成交的处理；owner/trade 映射不得在仍可接受的迟到回报前过早清理，并须覆盖重连/重启后的恢复和查询补账。
- 无法证明 owner 能力的旧 ABI 插件仍默认拒绝注册；版本协商显式进行，不静默退化成按 code 广播。旧 ABI 兼容窗口仍需单独安排。

## 验收条件

1. **共享配置和 owner 隔离**：两个同 adapter、同 code 的 Diff executor 均可按共享合同运行；owner A/B 的订单、委托回报和成交只更新各自状态、差量和通知，交换发单先后再验证。
2. **稳定成交键**：重复成交回调、查询重放、乱序回报及 trade-key 冲突测试均证明 exactly-once ledger effect；冲突不回退到 code 广播。
3. **全路径门禁**：对 callback、manager fanout、显式 route、`ALL`、缓存目标、查询补账及重启恢复逐一验证；缺失/未知/无效/歧义 owner 均不得修改任何 executor 账本。
4. **查询补账和历史 floor**：查询回填与 live 回报竞态不重复计账；历史 floor 具备账户、合约、as-of 时间和可信来源证据，并能在重启后校验和恢复。
5. **终态映射**：覆盖部分成交后撤单/拒单、重复终态、迟到成交、重连和终态映射清理；清理不得使允许的迟到成交失去 owner 或被重复应用。
6. **生产回调接线与 capability 证据**：订单、成交、撤单/拒单等生产回调均通过 owner token 路由；required capabilities 来自可核验接口/合同或版本协商，而非测试/调用方手工置 true。缺少证据时 registry 不启用。
7. **旧 ABI**：无法验证 owner token 的旧插件按默认策略拒绝注册，不产生委托；拒绝原因对调用者可见。
8. **外部验收**：fake/provider harness 完成后，仍需用真实 provider 验证下单标识保留、回报顺序、历史查询和账户分账；本机 journal 12/12 与 owner registry 3/3 不能代替此门槛。

需使用可控 fake adapter/provider 和定向单元/集成测试完成本地验收。stub 测试不能证明外部交易 provider 的 usertag 保留、回报次序或真实账户行为；相关外部环境另行记录，不得由本条本地 PASS 推导。
