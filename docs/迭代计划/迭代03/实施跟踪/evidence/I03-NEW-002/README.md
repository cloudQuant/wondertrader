# I03-NEW-002：HFT 未绑定 trader 的交易动作局部保护（仍阻塞）

本次基线为此前 R15 局部修复之后的脏工作树快照 `baseline/`（SHA 在 `baseline.sha256`）。仅改 `HftStraBaseCtx.h/.cpp` 及 `r15_hft_stra_context.cpp`，相对基线差异为 `own-*.diff`。不能因为本子项通过而关闭 R15 或本新增问题。

- `_trader` 确定初始化为 null。未绑定时 8 个交易动作（单号/批量撤单、买卖、开平多空）在引擎访问和代码映射前记录错误并返回现有接口的失败表示：false、空订单集或 0。HFT 基接口对开平接口默认返回 0，当前本地订单生成器也将 0 保留为无订单值。错误记录函数是虚调用；即使它同步绑定 trader，已经拒绝的外层动作仍直接返回。
- 在绑定状态下，动作捕获适配器指针及本上下文绑定版本。在同步规则、主力代码、合约/品种及风控调用后、提交动作前核对绑定；变更（包括解绑、换绑、解绑后重新绑定同一指针）记录错误并拒绝。`setTrader` 每次调用增加版本。此机制不对跨线程访问提供同步，也不持有或延长 trader 生命周期；版本回绕以及正在执行外部请求期间的解绑仍未处理。
- long-only sell 原来调用公开 `stra_get_position`；后者内部规则解析可同步解绑并直接解引用空指针。因此增加内部 bool+输出值查询：保留原来重新解析、主力映射、方向 flag 和 `getPosition` 的处理，在潜在重入点校验绑定，无法确认时由 sell 返回空订单。公开 `stra_get_position`/`stra_get_undone` 的 double 错误合同未确定，代码仍保留原样；不要在未绑定时调用，也不能把该问题记为已修。

生命周期审计：`WtRtRunner::createHftContext` 先 addContext，找不到 trader 仍返回有效 ID；配置化 HFT 也先发布后只记录绑定错误。`WtHftEngine` 对已发布上下文调用 on_init；`WtPorter` 的 `hft_get_position` 与 `hft_get_undone` 可通过上下文 ID 直接访问公开查询。因此动作门控不能阻止所有未绑定查询。`on_trade` 在启用信号输出时也会间接查持仓；完整修复需要另行确定发布/生命周期门禁、查询失败合同、C API 和策略兼容性。

## 当前定向验证

macOS arm64，命令在仓库根目录执行：

```
/private/tmp/wt-i03-new002/build-normal.sh > /private/tmp/wt-i03-new002/build-normal-final6.log 2>&1
/private/tmp/wt-i03-new002/build-asan.sh > /private/tmp/wt-i03-new002/build-asan-final6.log 2>&1
/private/tmp/wt-i03-new002/run-final.sh /private/tmp/wt-i03-new002/final-tests normal-v5
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 /private/tmp/wt-i03-new002/run-final.sh /private/tmp/wt-i03-new002/asan-tests asan-v5
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /private/tmp/wt-i03-new002/verify-results.py
```

两个构建各重编当前 HftStraBaseCtx、HftStraContext、WtHftEngine 和测试 TU，分别使用 Apple clang 17、LLVM 22 ASan+UBSan（后者指定 Apple SDK libc++ 头）；其他静态/动态依赖复用 `/private/tmp/wt-r14-resume/build` 旧快照，测试对象排在旧归档前。LSan 未运行，其他依赖未插桩。实体 Boost 头和实体复制的 nanomsg 路径见脚本；`symlinks.log` 为空。`source-before-final.sha256`、`source-after-final.sha256` 比对一致，仅覆盖列出的源与依赖；`artifact.sha256` 固定产物，不证明全部依赖为当前树。

普通和局部 ASan+UBSan 各 24/24 PASS，逐例 XML 为 1 test、0 failure/error/disabled，见 `normal-v5-results.log`、`asan-v5-results.log`、`xml-summary.log` 和 `run/{normal-v5,asan-v5}-*/`。每项独立进程/目录，30 秒超时：17 项为此问题的初始化、8 动作未绑定、绑定/解绑、错误诊断重入、规则/主力/合同/品种和 sell 二次持仓查询重入、两种绑定位置查询正控；另 7 项为 R15 标签关联控制。正常绑定的 openLong 用当前真实 TraderAdapter 和进程内 `p11fix::FakeTraderApi`，验证 fake orderInsert 被调用 1 次、代码/价格/数量正确；不连接或发送真实柜台。long-only sell 的普通和主力映射正控，确认无仓可用时与原实现均报不足持仓并拒单。

## 红控及未通过记录

- 预修改 BaseCtx header/cpp 的隔离快照：未绑定动作、未绑定诊断重入、诊断中绑定共 3 例 exit 139；正常绑定的未知撤单、正常绑定 fake 下单各 exit 0（`red-final-results.log`、`red-positive-results.log`）。单独普通及主力映射的 long-only sell 正控在修改前各 exit 0（`red-position-positive-results.log`）。红控强制将指针置空以确定性复现，不能仅由此动态证明原构造器的未初始化值。
- 仅入口判空的中间候选：规则回调时解绑和换绑各 exit 139，正常绑定正控 exit 0（`partial-results.log`）；只比较指针、不核对版本的受控候选在解绑后重绑同一指针时 exit 139，正常绑定控制 exit 0（`aba-red-results.log`；该控制的 header 差异为 `aba-only.diff`）。这些是隔离候选，不是完整原始版本。
- 外层绑定检查已加、内部 sell 仍调用原公开持仓查询的中间候选：在第二次规则查询同步解绑时 exit 139（`position-red-results.log`）。修复后相应测试通过。
- 初次六种动作循环中，测试替身只在第一轮解绑；第三轮误调用未初始化的普通 TraderAdapter，普通 exit 139、ASan exit 134。原始日志保留在 `run/{normal-v4,asan-v4}-each_entry_and_exit_rejects_reentrant_unbind_from_rule_lookup/`。修正替身使每轮均解绑后，同一测试通过，不能抹去此前失败。

未验证跨线程修改、外部对象生命周期、真实供应商、Windows/Linux、完整当前树和整体回归。`traderBindingUnchanged` 所用 64 位版本可回绕；此子项不证明跨进程身份或订单归属。公开查询和未绑定上下文发布仍是明确未完成项，故保持阻塞。
